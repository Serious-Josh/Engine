#include <cassert>
#include "Scene/SceneSwitchTiming.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "Renderer.h"
#include "DisplacementGPUIndexLayout.h"
#include "Displacement/DisplacementUpdateCoordinator.h"
#include "Displacement/DisplacementCommandRecorder.h"
#include "Displacement/DisplacementRequestValidation.h"
#include "../../assets/Shaders/GIModeConfig.hlsli"
#include "Renderer/Mesh.h"
#include "Renderer/Passes/EquirectangularToCubemapPass.h"
#include "Renderer/Passes/EnvironmentMipPass.h"
#include "Renderer/Passes/DiffuseIrradiancePass.h"
#include "Renderer/Passes/GGXMultiscatterIrradiancePass.h"
#include "Renderer/Texture.h"
#include "Material.h"
#include "Scene/Camera.h"
#include "ShaderData.h"
#include "Scene/DirectionalLight.h"
#include "Scene/Scene.h"
#include "DemoContent/MirrorTestScene.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <string>
#include <d3d12.h>
#include <algorithm>
#include <cctype>
#include <iterator>
#include <utility>
#include <cstdlib>
#include <climits>
#include <chrono>
#include <atomic>
#include <optional>
#include <stdexcept>

namespace
{
    // Scene lifetimes and command recordings share one non-reusing ID source.
    std::uint64_t AllocateRendererIdentity()
    {
        static std::atomic<std::uint64_t> next{1};
        std::uint64_t value = next.load(std::memory_order_relaxed);

        while (value != UINT64_MAX)
        {
            if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            {
                return value;
            }
        }

        return 0; // Never wrap and accept a stale lifetime.
    }
} // namespace

// --------------
// Pending displacement ownership and recording validation
// --------------

Renderer::~Renderer()
{
    if (m_heightmapRegistration.owner)
    {
        Log::Error("Renderer destroyed before checked displacement heightmap unregistration.");
        std::abort();
    }
}

bool Renderer::ValidateDisplacementHeightmapRegistration() const
{
    const auto& registration = m_heightmapRegistration;

    if (!registration.owner ||
        !registration.sceneLifetime ||
        registration.sceneLifetime != m_sceneResourceLifetime ||
        registration.scene != m_sceneResourceScene)
    {
        return false;
    }

    if (registration.descriptorHeap != &m_resourceDescriptorHeap ||
        !registration.heapResource ||
        registration.heapResource != m_resourceDescriptorHeap.GetHeap())
    {
        return false;
    }

    // The owner cannot shut down/reinitialize while registered; destruction is
    // fatal before its members die. Validate that lease before mutable delivery.
    const auto& owner = *registration.owner;

    if (!owner.m_initialized ||
        owner.m_heightmapRegistrationOwner != this ||
        owner.m_heightmapRegistrationLifetime != registration.sceneLifetime ||
        &owner.m_scene != registration.scene)
    {
        return false;
    }

    if (&owner.m_displacementTestMesh != registration.mesh ||
        &owner.m_displacementHeightTexture != registration.texture ||
        owner.m_heightmapDescriptorHeap != registration.descriptorHeap)
    {
        return false;
    }

    const auto& texture = owner.m_displacementHeightTexture;

    if (!registration.resource || texture.GetResource() != registration.resource ||

        registration.descriptor == UINT_MAX || texture.GetDescriptorIndex() != registration.descriptor)
    {
        return false;
    }

    // Scene structure may legitimately change before checked resource release.
    // Owner/member identity stays valid even if its render object was removed.

    return true;
}

bool Renderer::RegisterDisplacementHeightmap(MirrorTestScene& owner)
{
    if (m_heightmapRegistration.owner)
    {
        return m_heightmapRegistration.owner == &owner && !m_heightmapRegistration.useUnresolved &&
               !HasUnresolvedDisplacementUpdate() && ValidateDisplacementHeightmapRegistration();
    }

    if (HasUnresolvedDisplacementUpdate() ||
        m_geometryUpdateFailed ||
        !owner.m_initialized ||
        owner.m_heightmapRegistrationOwner)
    {
        return false;
    }

    if (!m_sceneResourceLifetime ||
        m_sceneResourceScene != &owner.m_scene ||
        owner.m_heightmapDescriptorHeap != &m_resourceDescriptorHeap ||
        !m_resourceDescriptorHeap.GetHeap())
    {
        return false;
    }

    Texture& texture = owner.m_displacementHeightTexture;

    if (!texture.GetResource() || texture.GetDescriptorIndex() == UINT_MAX || texture.GetSubresourceStates().empty())
    {
        return false;
    }

    const auto description = texture.GetResource()->GetDesc();
    const auto& objects = owner.m_scene.GetRenderObjects();

    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        description.Format != DXGI_FORMAT_R8_UNORM ||
        description.DepthOrArraySize != 1 ||
        description.SampleDesc.Count != 1)
    {
        return false;
    }

    if (!std::any_of(objects.begin(), objects.end(), [&owner](const RenderObject& object) { return object.mesh == &owner.m_displacementTestMesh; }))
    {
        return false;
    }

    m_heightmapRegistration = {&owner,
                               &owner.m_scene,
                               &owner.m_displacementTestMesh,
                               &texture,
                               texture.GetResource(),
                               &m_resourceDescriptorHeap,
                               m_resourceDescriptorHeap.GetHeap(),
                               texture.GetDescriptorIndex(),
                               m_sceneResourceLifetime,
                               false};
    owner.m_heightmapRegistrationOwner = this;
    owner.m_heightmapRegistrationLifetime = m_sceneResourceLifetime;
    return true;
}

bool Renderer::GenerateDisplacementMeshGPU(
    MirrorTestScene& owner,
    const DisplacementMeshSettings& settings)
{
    if (m_heightmapRegistration.owner != &owner || !ValidateDisplacementHeightmapRegistration())
    {
        return false;
    }

    const bool generated =

        GenerateDisplacementMeshGPU(owner.m_displacementTestMesh, owner.m_displacementHeightTexture, settings);

    if (!generated && m_geometryUpdateFailed)
    {
        // No automatic drain recovery: retain registration on uncertain or
        // partially mutated geometry/AS failure, just like pending ownership.
        m_heightmapRegistration.useUnresolved = true;
    }

    return generated;
}

Renderer::DisplacementRequestResult Renderer::RequestDisplacementUpdate(
    MirrorTestScene& owner,
    std::uint64_t requestRevision)
{
    std::optional<HitchTimingScope> diagnosticRequest;

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        m_frameHitchDiagnostics.current.requestCalled = true;
        diagnosticRequest.emplace(m_frameHitchDiagnostics.current.requestMilliseconds);
    }

    if (!kEnableFrameIntegratedDisplacement || m_activeRecordingGeneration != 0 ||
        m_displacementExitScopeGeneration != 0 || m_heightmapRegistration.owner != &owner ||

        !ValidateDisplacementHeightmapRegistration() || m_heightmapRegistration.useUnresolved ||
        !owner.HasCurrentDisplacementSettings() || !owner.HasPendingDisplacementGeometryUpdate() ||
        requestRevision == 0 || requestRevision != owner.GetRequestedDisplacementRevision())
    {
        return DisplacementRequestResult::Error;
    }

    const auto& pending = m_pendingDisplacementUpdate;

    if (pending.status != DisplacementUpdateStatus::Empty)
    {
        if (!ValidatePendingDisplacementIdentity() || pending.scene != &owner.m_scene ||

            pending.mesh != &owner.m_displacementTestMesh)
        {
            return DisplacementRequestResult::Error;
        }

        if (pending.status == DisplacementUpdateStatus::Prepared && ValidatePendingDisplacementReservation())
        {
            return DisplacementRequestResult::Deferred;
        }

        if (pending.status == DisplacementUpdateStatus::Submitted && ValidatePendingDisplacementCoverage())
        {
            const auto completion = ObservePendingDisplacementCompletion();

            if (completion == DisplacementCompletionObservation::Pending ||

                completion == DisplacementCompletionObservation::Complete)
            {
                return DisplacementRequestResult::Deferred;
            }
        }

        return DisplacementRequestResult::Error; // Never fall back after mutation/uncertain submission.
    }

    if (HasUnresolvedDisplacementUpdate() || m_geometryUpdateFailed)
    {
        return DisplacementRequestResult::Error;
    }

    if (m_completedDisplacementUpdate.requestRevision != 0)
    {
        if (m_completedDisplacementUpdate.scene != &owner.m_scene ||

            m_completedDisplacementUpdate.sceneResourceLifetime != m_sceneResourceLifetime ||

            m_completedDisplacementUpdate.sceneRevision != owner.m_scene.GetStructureRevision())
        {
            return DisplacementRequestResult::Error;
        }

        return DisplacementRequestResult::Deferred; // Deliver completion before accepting another request.
    }

    // This is a synchronous CPU snapshot, not GPU generation. Preparation
    // copies the owner's LOD view before returning and reserves before submit.

    const auto settings = owner.GetCurrentDisplacementSettings();

    if (!PrepareFrameDisplacementRequest(owner.m_scene, owner.m_displacementTestMesh, requestRevision, settings))
    {
        return DisplacementRequestResult::Error;
    }

    m_displacementWallClock = {};

    m_displacementDeferredFrames = 0;
    m_displacementCompletionObserved = false;
    m_displacementTerminalErrorLogged = false;

    m_displacementWallClock.accepted = CpuTiming::Clock::now();

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        m_frameHitchDiagnostics.current.revision = requestRevision;
        m_frameHitchDiagnostics.current.accepted = true;
    }

    {
        FrameHitchProbe loggingProbe(
            *this,
            FrameHitchRegion::AcceptedPhaseLogging,
            !kSuppressDisplacementLifecycleLoggingForDiagnostics);

        if constexpr (!kSuppressDisplacementLifecycleLoggingForDiagnostics)
        {
            Log::Info("Async displacement accepted revision " + std::to_string(requestRevision) + ".");
        }
    }

    return DisplacementRequestResult::Accepted;
}

void Renderer::InvalidateRecordingEligibility()
{
    m_activeRecordingGeneration = 0;
    m_activeRecordingSlot = UINT_MAX;
    m_frameDisplacementPreparation = {};
}

bool Renderer::BeginTrackedRecording(unsigned int frameSlot)
{
    if(m_commandRecordingGeneration != 0)
    {
        return false; // Resolve the previous list before resetting its ownership.
    }

    if (m_sceneInitializationAttempt.candidate &&
        (m_sceneInitializationAttempt.failed ||
         m_sceneInitializationAttempt.commands != SceneInitializationCommands::BeginAttempted))
    {
        return false;
    }

    if (m_displacementCoverage.enabled)
    {
        const auto outcome = m_displacementCoverage.current.outcome;

        if (m_displacementCoverage.admissionClosed || !ValidatePendingDisplacementCoverage() ||

            m_displacementCoverage.current.ownsReservation || m_displacementCoverage.current.ownsSubmission ||
            (outcome == DisplacementRecordingOutcome::Submitted &&

             m_pendingDisplacementUpdate.status != DisplacementUpdateStatus::Submitted) ||
            (outcome != DisplacementRecordingOutcome::Submitted && outcome != DisplacementRecordingOutcome::Discarded))
        {
            return false; // Never reset an unresolved covered recording.
        }
    }

    m_rayTracingScene.CollectRetiredMemberships(m_commandContext);
    InvalidateRecordingEligibility(); // Includes allocator/list reset attempts.
    const std::uint64_t generation = AllocateRendererIdentity();

    if (generation == 0 || !m_commandContext.Begin(frameSlot))
    {
        return false;
    }

    m_activeRecordingGeneration = generation;
    m_commandRecordingGeneration = generation;
    m_commandSubmissionAttempted = false;
    m_activeRecordingSlot = frameSlot;

    if (!m_rayTracingScene.BeginMembershipRecording(generation))
    {
        return false;
    }

    if(!m_materialPreviewRenderer.BeginRecording(generation))
    {
        return false;
    }

    m_textureThumbnails.BeginRecording(generation);

    m_frameDisplacementPreparation.generation = generation;
    m_frameDisplacementPreparation.slot = frameSlot;

    m_frameDisplacementPreparation.slotAcquired = true;

    if (m_displacementCoverage.enabled)
    {
        m_displacementCoverage.current = {};
        m_displacementCoverage.current.generation = generation;
        m_displacementCoverage.current.frameSlot = frameSlot;
        m_displacementCoverage.current.outcome = DisplacementRecordingOutcome::Active;
    }

    return true;
}

void Renderer::ObserveFrameUploadReset(unsigned int frameSlot)
{
    auto& preparation = m_frameDisplacementPreparation;

    const bool matchesRecording =
        m_activeRecordingGeneration != 0 && preparation.generation == m_activeRecordingGeneration;
    const bool matchesSlot =
        frameSlot < kDisplacementLODFrameCount && m_activeRecordingSlot == frameSlot && preparation.slot == frameSlot;

    if (!matchesRecording || !matchesSlot || !preparation.slotAcquired)
    {
        preparation = {};
        return;
    }

    if (!m_uploadBuffer.GetResource() || preparation.uploadResetCount != 0)
    {
        preparation = {}; // Failed/duplicate observation cannot authorize displacement.
        return;
    }

    preparation.uploadResetCount = 1;
}

void Renderer::ObserveDiagnosticLODBinding(
    unsigned int frameSlot,
    D3D12_GPU_VIRTUAL_ADDRESS address,
    size_t bytes,
    bool descriptorCreated)
{
    auto& preparation = m_frameDisplacementPreparation;

    const bool matchesRecording =
        m_activeRecordingGeneration != 0 && preparation.generation == m_activeRecordingGeneration;

    const bool matchesSlot =
        frameSlot < kDisplacementLODFrameCount && m_activeRecordingSlot == frameSlot && preparation.slot == frameSlot;

    if (!matchesRecording || !matchesSlot || !preparation.slotAcquired)
    {
        preparation = {};
        return;
    }

    if (preparation.uploadResetCount != 1 || preparation.diagnosticConsistent)
    {
        preparation = {};
        return;
    }

    if (address == 0 || bytes == 0 || m_displacementLODDescriptorIndices[frameSlot] == UINT_MAX ||
        !m_uploadBuffer.GetResource())
    {
        preparation = {};
        return;
    }

    auto& identity = m_diagnosticLODBindings[frameSlot];

    // Publish identity only after CreateRawSRV actually succeeded. Ordinary
    // frames must match that resource, address, size and unchanged descriptor.

    if (descriptorCreated)
    {
        identity = {m_uploadBuffer.GetResource(), address, bytes, m_displacementLODDescriptorIndices[frameSlot]};
    }

    preparation.diagnosticConsistent = identity.resource == m_uploadBuffer.GetResource() &&
                                       identity.address == address && identity.bytes == bytes &&

                                       identity.descriptor == m_displacementLODDescriptorIndices[frameSlot];

    if (!preparation.diagnosticConsistent)
    {
        preparation = {};
    }
}

bool Renderer::ValidateFrameDisplacementPreparation(unsigned int frameSlot) const
{
    return RendererDisplacement::ValidateFramePreparation(m_frameDisplacementPreparation, m_activeRecordingGeneration,
                                                          m_activeRecordingSlot, frameSlot, kDisplacementLODFrameCount);
}

bool Renderer::EndTrackedRecording()
{
    if(m_commandRecordingGeneration == 0 || m_commandSubmissionAttempted ||
        m_commandRecordingGeneration != m_activeRecordingGeneration)
    {
        return false;
    }

    if (m_displacementCoverage.enabled && !ValidateCoveredDisplacementRecording())
    {
        m_displacementCoverage.failed = true;
        InvalidateRecordingEligibility();
        return false;
    }

    const bool ended = m_commandContext.End();

    auto& recovery = m_pendingDisplacementUpdate.frameRecovery;

    if (recovery.state == DisplacementFrameExitState::Recording && ended)
    {
        recovery.state = DisplacementFrameExitState::Ended;
    }

    if (m_displacementCoverage.enabled)
    {
        if (ended)
        {
            m_displacementCoverage.current.outcome = DisplacementRecordingOutcome::Recorded;
        }
        else
        {
            m_displacementCoverage.failed = true;
        }
    }

    InvalidateRecordingEligibility();
    return ended;
}

bool Renderer::AbortTrackedRecording()
{
    if(m_commandRecordingGeneration == 0)
    {
        return false;
    }

    if(m_commandSubmissionAttempted)
    {
        m_textureThumbnails.Uncertain(m_commandRecordingGeneration);
        m_materialPreviewRenderer.MarkSubmissionUncertain(m_commandRecordingGeneration);
        return false;
    }

    if (m_displacementCoverage.enabled)
    {
        const auto outcome = m_displacementCoverage.current.outcome;

        if (outcome != DisplacementRecordingOutcome::Active && outcome != DisplacementRecordingOutcome::Recorded)
        {
            return false; // No abort resolution after any possible submission.
        }
    }

    const auto materialSlot = m_activeRecordingSlot;
    const auto uploadGeneration = m_commandRecordingGeneration;
    const bool discarded = m_commandContext.AbortRecording();

    if(!discarded)
    {
        m_textureThumbnails.Uncertain(m_commandRecordingGeneration);
    }

    if(discarded)
    {
        m_textureThumbnails.Discarded(m_commandRecordingGeneration);
        m_rayTracingScene.DiscardMembershipRecording(m_commandRecordingGeneration);

        if(!m_materialPreviewRenderer.DiscardRecording(m_commandRecordingGeneration))
        {
            return false;
        }

        m_commandRecordingGeneration = 0;
        m_materialDependencies.Discard(materialSlot);

        for(auto& entry : m_preparedTextureUploads)
        {
            entry.upload->Discard(uploadGeneration);
        }
    }

    if (m_displacementCoverage.enabled)
    {
        if (discarded)
        {
            m_displacementCoverage.current.outcome = DisplacementRecordingOutcome::Discarded;
        }
        else
        {
            m_displacementCoverage.failed = true;
        }
    }

    InvalidateRecordingEligibility();
    return discarded;
}

bool Renderer::ExecuteTrackedRecording(unsigned int frameSlot)
{
    if(m_commandRecordingGeneration == 0 || m_commandSubmissionAttempted)
    {
        return false;
    }

    if (m_sceneInitializationAttempt.candidate &&
        (m_sceneInitializationAttempt.failed ||
         m_sceneInitializationAttempt.commands != SceneInitializationCommands::SubmissionAttempted))
    {
        return false;
    }

    if (m_displacementCoverage.enabled)
    {
        m_displacementCoverage.failed = true;

        InvalidateRecordingEligibility();
        return false; // Covered submissions require reserved exact provenance.
    }

    InvalidateRecordingEligibility();

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        m_frameHitchDiagnostics.current.contextStart = CpuTiming::Clock::now();
    }

    if (!m_rayTracingScene.AttemptMembershipSubmission(m_commandRecordingGeneration))
    {
        return false;
    }

    m_commandSubmissionAttempted = true;
    const bool executed = m_commandContext.Execute(frameSlot);

    if(executed)
    {
        if (!m_rayTracingScene.ConfirmMembershipSubmission(m_commandRecordingGeneration, m_commandContext))
        {
            Log::Error("DXR membership submission association failed; retaining snapshot ownership.");
            return false;
        }

        m_textureThumbnails.Submitted(m_commandRecordingGeneration, m_commandContext.GetLastSubmissionFenceValue());

        if(!m_materialPreviewRenderer.ConfirmSubmission(m_commandRecordingGeneration,
            m_commandContext.GetLastSubmissionFenceValue()))
        {
            m_textureThumbnails.Uncertain(m_commandRecordingGeneration);
            m_materialPreviewRenderer.MarkSubmissionUncertain(m_commandRecordingGeneration);
            return false;
        }

        m_commandRecordingGeneration = 0;
        m_commandSubmissionAttempted = false;
    }
    else
    {
        m_textureThumbnails.Uncertain(m_commandRecordingGeneration);
        m_materialPreviewRenderer.MarkSubmissionUncertain(m_commandRecordingGeneration);
    }

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        m_frameHitchDiagnostics.current.contextEnd = CpuTiming::Clock::now();
    }

    return executed;
}

void Renderer::SubmitArchiveMaterialProgress(unsigned int frameSlot)
{
    const auto requireProgress = [](bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    };

    requireProgress(BeginTrackedRecording(frameSlot), "Archive admission cannot begin tracked frame.");

    // Capture both identities before uploads; End invalidates active recording eligibility.
    const auto recordingGeneration = m_activeRecordingGeneration;
    const auto recordedMaterialRevision = m_materialPublication.revision;
    auto* commandList = m_commandContext.GetCommandList();
    RecordPreparedTextureUploads(commandList);

    // Preserve short circuit and the archive failure policy: a false Execute may follow
    // queue execution. Neither failed recording nor uncertain submission permits cleanup here.
    requireProgress(RecordMaterialTable(frameSlot, commandList) && EndTrackedRecording() &&
                        ExecuteTrackedRecording(frameSlot),
                    "Archive material frame submission failed.");

    const auto submissionFence = m_commandContext.GetLastSubmissionFenceValue();
    const bool confirmed = ConfirmRecordedMaterialSubmission(
        frameSlot, recordingGeneration, recordedMaterialRevision, submissionFence);
    requireProgress(confirmed, "Archive texture dependency submission failed.");
}

bool Renderer::ConfirmRecordedMaterialSubmission(
    unsigned int frameSlot,
    std::uint64_t recordingGeneration,
    std::uint64_t recordedRevision,
    std::uint64_t submissionFence)
{
    // Uploads acquire the exact generation/fence before the slot's dependencies do.
    // A dependency failure can therefore leave uploads associated; do not roll them back.
    for (auto& entry : m_preparedTextureUploads)
    {
        entry.upload->ConfirmSubmission(recordingGeneration, submissionFence);
    }

    if (!m_materialDependencies.Confirm(frameSlot, submissionFence))
    {
        return false;
    }

    // Publish the submitted table revision only after dependency association succeeds.
    // Uncertainty, logging and exceptions remain each caller's responsibility.
    m_materialPublication.ConfirmSubmission(frameSlot, recordedRevision);
    return true;
}

bool Renderer::BeginPendingDisplacementCoverage()
{
    const RendererDisplacement::ValidationContext context{m_sceneResourceScene, m_sceneResourceLifetime,
                                                          m_activeRecordingGeneration, kDisplacementLODFrameCount,
                                                          m_geometryUpdateFailed};
    return RendererDisplacement::BeginCoverage(m_pendingDisplacementUpdate, m_displacementCoverage, context,
                                               m_activeRecordingSlot);
}

bool Renderer::ValidatePendingDisplacementCoverage() const
{
    const RendererDisplacement::ValidationContext context{m_sceneResourceScene, m_sceneResourceLifetime,
                                                          m_activeRecordingGeneration, kDisplacementLODFrameCount,
                                                          m_geometryUpdateFailed};
    return RendererDisplacement::ValidateCoverage(m_pendingDisplacementUpdate, m_displacementCoverage, context);
}

bool Renderer::ValidatePendingDisplacementRetirementCoverage() const
{
    if (!ValidatePendingDisplacementCoverage())
    {
        return false;
    }

    const auto& coverage = m_displacementCoverage;
    const auto& pending = m_pendingDisplacementUpdate;
    const auto& original = coverage.original;

    if (coverage.submittedCount == 0 ||
        original.outcome != DisplacementRecordingOutcome::Submitted ||
        original.generation != pending.recordingGeneration ||
        original.frameSlot != pending.frameSlot)
    {
        return false;
    }

    if (original.submission.lifetime != pending.submission.lifetime ||
        original.submission.retentionID != pending.submission.retentionID ||
        original.submission.fenceValue != pending.submission.fenceValue ||
        m_commandContext.QuerySubmissionFenceStatus(pending.submission) == SubmissionFenceStatus::Error)
    {
        return false;
    }

    if (!m_rayTracingScene.ValidateRetirementBatch(pending.mesh, pending.retirementBatch))
    {
        return false;
    }

    if (!RendererDisplacement::ValidateHistoricalRetirementCoverage(coverage, kDisplacementLODFrameCount))
    {
        return false;
    }

    switch (coverage.current.outcome)
    {
    case DisplacementRecordingOutcome::Active:
        return ValidateCoveredDisplacementRecording();
    case DisplacementRecordingOutcome::Recorded:
        return m_activeRecordingGeneration == 0 && ValidateCoveredDisplacementGeometry();
    case DisplacementRecordingOutcome::Submitted:

    case DisplacementRecordingOutcome::Discarded:

        return m_activeRecordingGeneration == 0;
    default:
        return false;
    }
}

bool Renderer::ValidatePendingDisplacementCoverageResolution() const
{
    const auto& coverage = m_displacementCoverage;
    const auto& pending = m_pendingDisplacementUpdate;

    if (ObservePendingDisplacementCompletion() != DisplacementCompletionObservation::Complete ||
        !ValidatePendingDisplacementRetirementCoverage() ||
        m_activeRecordingGeneration != 0 ||
        m_displacementExitScopeGeneration != 0)
    {
        return false;
    }

    if (!ValidateDisplacementHeightmapRegistration() ||
        coverage.current.outcome != DisplacementRecordingOutcome::Submitted ||
        !ValidateCoveredDisplacementGeometry())
    {
        return false;
    }

    if (coverage.current.ownsReservation || coverage.current.ownsSubmission)
    {
        return false;
    }

    for (const auto& receipt : coverage.submitted)
    {
        if (receipt.ownsReservation)
        {
            return false; // A reservation cannot be discharged as a successful submission.
        }
    }

    if (!RendererDisplacement::ValidateCoverageResolutionRecords(pending, coverage))
    {
        return false;
    }

    SubmittedDisplacementBinding binding{};

    if (!TryBuildPendingDisplacementBinding(pending.mesh, binding) ||

        pending.mesh->GetVertexBufferUAVDescriptorIndex() == UINT_MAX ||

        pending.mesh->GetIndexBufferUAVDescriptorIndex() == UINT_MAX)
    {
        return false;
    }

    return true;
}

bool Renderer::BeginPendingDisplacementCoverageResolution()
{
    if (!ValidatePendingDisplacementCoverageResolution())
    {

        return false;
    }

    auto& coverage = m_displacementCoverage;

    for (const auto& receipt : coverage.submitted)
    {
        if (receipt.ownsSubmission)
        {
            return false; // Provenance ownership must be explicitly discharged first.
        }
    }

    // Single-threaded Renderer ownership: freeze Begin/End/submission eligibility
    // before any completion/publication mutation. Preserve the original receipt
    // throughout partial failure; no retry or teardown is authorized by failure.
    coverage.resolving = true;
    return true;
}

bool Renderer::ValidateCoveredDisplacementGeometry() const
{
    if (!ValidatePendingDisplacementCoverage())
    {
        return false;
    }

    const auto& pending = m_pendingDisplacementUpdate;
    const auto& receipt = m_displacementCoverage.current;

    if (m_displacementCoverage.submittedCount != 0)
    {
        const auto& original = m_displacementCoverage.original.geometry;

        if (receipt.geometry.blasAddress != original.blasAddress ||
            receipt.geometry.retirement.sceneLifetime != original.retirement.sceneLifetime ||
            receipt.geometry.retirement.blas.ownerLifetime != original.retirement.blas.ownerLifetime ||

            receipt.geometry.retirement.blas.batchID != original.retirement.blas.batchID)
        {

            return false; // Never absorb a second replacement into this coverage window.
        }
    }

    if (receipt.geometry.geometryVersion != pending.requestRevision ||

        receipt.geometry.blasAddress != pending.replacementBLASAddress || !pending.replacementBLASAddress)
    {
        return false;
    }

    // Validate only the current, unsubmitted recording against mutable TLAS
    // state. Frozen submission receipts retain this checked fact historically.
    return m_rayTracingScene.ValidateRetirementBatch(pending.mesh, pending.retirementBatch) &&
           m_rayTracingScene.ValidateTLASGeometryEvidence(pending.mesh, receipt.geometry) &&

           receipt.geometry.retirement.sceneLifetime == pending.retirementBatch.sceneLifetime &&

           receipt.geometry.retirement.blas.ownerLifetime == pending.retirementBatch.blas.ownerLifetime &&
           receipt.geometry.retirement.blas.batchID == pending.retirementBatch.blas.batchID;
}

bool Renderer::ValidateCoveredDisplacementRecording() const
{
    const auto& receipt = m_displacementCoverage.current;

    if (receipt.outcome != DisplacementRecordingOutcome::Active ||
        receipt.generation == 0 ||
        receipt.generation != m_activeRecordingGeneration ||
        receipt.frameSlot != m_activeRecordingSlot)
    {
        return false;
    }

    if (!ValidateFrameDisplacementPreparation(receipt.frameSlot) ||
        !ValidateCoveredDisplacementGeometry())
    {
        return false;
    }

    const auto& pending = m_pendingDisplacementUpdate;

    if (m_displacementCoverage.submittedCount == 0)
    {
        return pending.recordingGeneration == receipt.generation &&
               pending.recordingStage == DisplacementRecordingStage::MergedTLAS &&

               pending.historyPreparation == DisplacementHistoryPreparation::Performed &&
               pending.tlasPreparation[receipt.frameSlot] == DisplacementTLASPreparation::Recorded;
    }

    return pending.status == DisplacementUpdateStatus::Submitted;
}

ReservedSubmissionResult Renderer::ExecuteCoveredDisplacementRecording(
    unsigned int frameSlot,
    SubmissionFenceReservation reservation,
    SubmissionFenceToken& token)
{
    auto& coverage = m_displacementCoverage;
    auto& receipt = coverage.current;

    const auto& frameRecovery = m_pendingDisplacementUpdate.frameRecovery;

    if(m_commandSubmissionAttempted || receipt.outcome == DisplacementRecordingOutcome::Submitted ||
        receipt.outcome == DisplacementRecordingOutcome::SubmissionUncertain)
    {
        return ReservedSubmissionResult::SubmissionUncertain;
    }

    if(m_commandRecordingGeneration == 0 || m_commandRecordingGeneration != receipt.generation)
    {
        return ReservedSubmissionResult::NotSubmitted;
    }

    if (coverage.submittedCount == 0 && frameRecovery.state != DisplacementFrameExitState::Empty &&
        (frameRecovery.state != DisplacementFrameExitState::Ended || frameRecovery.generation != receipt.generation ||
         frameRecovery.frameSlot != frameSlot || frameRecovery.sceneLifetime != coverage.sceneLifetime))
    {
        return frameRecovery.state == DisplacementFrameExitState::Uncertain
                   ? ReservedSubmissionResult::SubmissionUncertain
                   : ReservedSubmissionResult::NotSubmitted;
    }

    if (coverage.enabled && receipt.outcome == DisplacementRecordingOutcome::SubmissionUncertain)
    {
        return ReservedSubmissionResult::SubmissionUncertain;
    }

    if (!ValidateCoveredDisplacementGeometry() || receipt.outcome != DisplacementRecordingOutcome::Recorded ||
        m_activeRecordingGeneration != 0 || receipt.frameSlot != frameSlot ||
        coverage.submittedCount == coverage.submitted.size() || !reservation.lifetime || !reservation.reservationID)
    {
        return ReservedSubmissionResult::NotSubmitted;
    }

    if (coverage.submittedCount == 0)
    {
        const auto& pending = m_pendingDisplacementUpdate;

        if (pending.status != DisplacementUpdateStatus::Submitting ||

            pending.reservation.lifetime != reservation.lifetime ||

            pending.reservation.reservationID != reservation.reservationID)
        {
            return ReservedSubmissionResult::NotSubmitted;
        }
    }
    else if (!receipt.ownsReservation || receipt.reservation.lifetime != reservation.lifetime ||
             receipt.reservation.reservationID != reservation.reservationID)
    {
        return ReservedSubmissionResult::NotSubmitted;
    }

    receipt.reservation = reservation;

    receipt.outcome = DisplacementRecordingOutcome::SubmissionUncertain;

    auto& recovery = m_pendingDisplacementUpdate.frameRecovery;

    if (coverage.submittedCount == 0 && recovery.state != DisplacementFrameExitState::Empty)
    {
        // Arm before the API call. No exit may attempt abort recovery once
        // Execute could have handed this recording to the GPU.

        recovery.state = DisplacementFrameExitState::Uncertain;
    }

    SubmissionFenceToken submitted{};

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        m_frameHitchDiagnostics.current.contextStart = CpuTiming::Clock::now();
    }

    if (!m_rayTracingScene.AttemptMembershipSubmission(m_commandRecordingGeneration))
    {
        return ReservedSubmissionResult::SubmissionUncertain;
    }

    m_commandSubmissionAttempted = true;
    const auto result = m_commandContext.ExecuteReserved(frameSlot, reservation, submitted);

    if(result == ReservedSubmissionResult::Submitted)
    {
        if (!m_rayTracingScene.ConfirmMembershipSubmission(m_commandRecordingGeneration, m_commandContext))
        {
            Log::Error("DXR membership reserved submission association failed; retaining snapshot ownership.");
            return ReservedSubmissionResult::SubmissionUncertain;
        }

        m_textureThumbnails.Submitted(m_commandRecordingGeneration, submitted.fenceValue);

        if(!m_materialPreviewRenderer.ConfirmSubmission(m_commandRecordingGeneration, submitted.fenceValue))
        {
            m_textureThumbnails.Uncertain(m_commandRecordingGeneration);
            m_materialPreviewRenderer.MarkSubmissionUncertain(m_commandRecordingGeneration);
            Log::Error("Preview submission association failed; retaining quarantine and submission provenance.");
        }
        else
        {
            m_commandRecordingGeneration = 0;
            m_commandSubmissionAttempted = false;
        }
    }
    else if(result == ReservedSubmissionResult::NotSubmitted)
    {
        m_rayTracingScene.MembershipNotSubmitted(m_commandRecordingGeneration);
        m_commandSubmissionAttempted = false;
    }
    else
    {
        m_textureThumbnails.Uncertain(m_commandRecordingGeneration);
        m_materialPreviewRenderer.MarkSubmissionUncertain(m_commandRecordingGeneration);
    }

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        m_frameHitchDiagnostics.current.contextEnd = CpuTiming::Clock::now();
    }

    if (coverage.submittedCount == 0 && result == ReservedSubmissionResult::Submitted)
    {
        m_displacementWallClock.submissionReturned = CpuTiming::Clock::now();
    }

    if (result == ReservedSubmissionResult::NotSubmitted)
    {
        if (coverage.submittedCount == 0 && recovery.state != DisplacementFrameExitState::Empty)
        {
            recovery.state = DisplacementFrameExitState::Ended;
        }

        if (!receipt.ownsReservation)
        {
            receipt.reservation = {};
        }

        receipt.outcome = DisplacementRecordingOutcome::Recorded;
        return result;
    }

    // Preserve provenance even if a post-submit check fails. Never cancel or
    // reset ownership after a possible submission. The caller still owns it.
    receipt.submission = submitted;

    if (coverage.submittedCount != 0 && submitted.fenceValue != 0)
    {
        // Preserve returned ownership even if validation below fails. An
        // uncertain receipt is never eligible for cancellation or cleanup.
        receipt.ownsSubmission = true;
    }

    token = submitted;

    const bool exactSubmission = result == ReservedSubmissionResult::Submitted &&

                                 submitted.lifetime == reservation.lifetime &&
                                 submitted.retentionID == reservation.reservationID &&

                                 m_commandContext.GetFrameFenceValue(frameSlot) == submitted.fenceValue &&

                                 m_commandContext.QuerySubmissionFenceStatus(submitted) != SubmissionFenceStatus::Error;

    bool ordered = true;

    if (coverage.submittedCount != 0)
    {
        const auto previous = coverage.submitted[coverage.submittedCount - 1].submission;
        ordered = submitted.lifetime == previous.lifetime && submitted.fenceValue > previous.fenceValue;
    }

    if (!exactSubmission || !ordered)
    {
        coverage.failed = true;
        return ReservedSubmissionResult::SubmissionUncertain;
    }

    receipt.outcome = DisplacementRecordingOutcome::Submitted;
    receipt.ownsReservation = false; // ExecuteReserved consumed the reservation.
    coverage.submitted[coverage.submittedCount] = receipt;

    if (coverage.submittedCount == 0)
    {
        coverage.original = receipt;
    }

    ++coverage.submittedCount;

    receipt.ownsSubmission = false; // Sole later-token owner is the history entry.
    return ReservedSubmissionResult::Submitted;
}

bool Renderer::ReserveLaterDisplacementSubmission()
{
    auto& coverage = m_displacementCoverage;
    auto& receipt = coverage.current;

    if (coverage.admissionClosed ||
        !ValidatePendingDisplacementCoverage() ||
        coverage.submittedCount == 0 ||
        m_pendingDisplacementUpdate.status != DisplacementUpdateStatus::Submitted)
    {
        return false;
    }

    if (receipt.outcome != DisplacementRecordingOutcome::Active ||
        receipt.generation != m_activeRecordingGeneration ||
        receipt.frameSlot != m_activeRecordingSlot ||
        receipt.ownsReservation)
    {
        return false;
    }

    if (receipt.ownsSubmission ||
        m_displacementExitScopeGeneration == 0 ||
        m_displacementExitScopeGeneration != receipt.generation ||
        receipt.reservation.lifetime != 0)
    {
        return false;
    }

    if (receipt.reservation.reservationID != 0)
    {
        return false;
    }

    if (coverage.submittedCount == coverage.submitted.size())
    {
        coverage.failed = true; // Never evict historical evidence to make room.

        return false;
    }

    const auto reservation = m_commandContext.ReserveSubmissionFence();

    if (!reservation.lifetime || !reservation.reservationID)
    {
        coverage.failed = true;
        return false;
    }

    receipt.reservation = reservation;
    receipt.ownsReservation = true;
    return true;
}

bool Renderer::CleanupLaterDisplacementSubmissions()
{
    auto& coverage = m_displacementCoverage;

    // Preflight the entire closed frontier before releasing any provenance.
    // This includes original completion, exact old-batch ownership, current
    // replacement TLAS references, and target geometry/count validation.

    if (!ValidatePendingDisplacementCoverageResolution())
    {
        coverage.failed = true;
        return false;
    }

    for (size_t index = 1; index < coverage.submittedCount; ++index)
    {
        const auto& receipt = coverage.submitted[index];

        if (receipt.ownsSubmission &&
            m_commandContext.QuerySubmissionFenceStatus(receipt.submission) == SubmissionFenceStatus::Error)
        {
            coverage.failed = true;
            return false;
        }
    }

    // Pending is permitted: these exact successful submissions were validated
    // against replacement-only geometry before submission. The completed
    // original fence covers earlier old-BLAS users. Retention owns provenance,
    // not resources; slot fences still protect allocator and upload reuse.

    for (size_t index = 1; index < coverage.submittedCount; ++index)
    {
        auto& receipt = coverage.submitted[index];

        if (!receipt.ownsSubmission)
        {

            continue; // Immutable receipt survives release; never query it again.
        }

        if (!m_commandContext.ReleaseSubmissionFence(receipt.submission))
        {
            coverage.failed = true;
            return false;
        }

        receipt.ownsSubmission = false;
    }

    return true;
}

bool Renderer::ResolveLaterDisplacementFrameExit()
{
    auto& coverage = m_displacementCoverage;

    auto& receipt = coverage.current;
    const auto fail = [&]()
    {
        coverage.failed = true;
        m_geometryUpdateFailed = true;
        InvalidateRecordingEligibility();
        return false;
    };

    if (!coverage.enabled || coverage.submittedCount == 0 ||

        m_pendingDisplacementUpdate.status != DisplacementUpdateStatus::Submitted ||

        !ValidatePendingDisplacementIdentity() || !ValidateDisplacementHeightmapRegistration() ||
        coverage.sceneLifetime != m_pendingDisplacementUpdate.sceneResourceLifetime ||

        coverage.mesh != m_pendingDisplacementUpdate.mesh ||

        coverage.requestRevision != m_pendingDisplacementUpdate.requestRevision ||
        m_displacementExitScopeGeneration == 0 || receipt.generation != m_displacementExitScopeGeneration ||

        receipt.frameSlot >= kDisplacementLODFrameCount ||
        (m_activeRecordingGeneration != 0 &&
         (m_activeRecordingGeneration != receipt.generation || m_activeRecordingSlot != receipt.frameSlot)))
    {
        return fail(); // Never abort a stale or different command recording.
    }

    // No mutable current-TLAS query is needed for an immutable submitted
    // receipt. Token ownership already transferred to its history entry.

    if (receipt.outcome == DisplacementRecordingOutcome::Submitted)
    {
        if (coverage.submittedCount < 2)
        {
            return fail();
        }

        const auto& submitted = coverage.submitted[coverage.submittedCount - 1];

        if (receipt.generation != submitted.generation || receipt.frameSlot != submitted.frameSlot ||
            receipt.submission.lifetime != submitted.submission.lifetime ||
            receipt.submission.retentionID != submitted.submission.retentionID ||
            receipt.submission.fenceValue != submitted.submission.fenceValue || receipt.ownsReservation ||
            receipt.ownsSubmission)
        {
            return fail();
        }

        return true;
    }

    if (receipt.outcome != DisplacementRecordingOutcome::Active &&

        receipt.outcome != DisplacementRecordingOutcome::Recorded &&

        receipt.outcome != DisplacementRecordingOutcome::Discarded)
    {
        return fail(); // Possible submission: retain every identity and owner.
    }

    if (receipt.ownsSubmission ||
        (receipt.outcome != DisplacementRecordingOutcome::Discarded && !AbortTrackedRecording()))
    {
        return fail(); // End/eligibility invalidation alone never proves discard.
    }

    if (receipt.ownsReservation)
    {
        if (!m_commandContext.CancelSubmissionFenceReservation(receipt.reservation))
        {
            return fail();
        }

        receipt.ownsReservation = false;
    }

    // Preserve the discarded receipt and all original ownership. Ordinary
    // frame CPU history/TLAS mutations have no rollback contract here.
    coverage.failed = true;

    m_geometryUpdateFailed = true;

    return true;
}

bool Renderer::ValidatePendingDisplacementRecording(unsigned int frameSlot) const
{
    const RendererDisplacement::ValidationContext context{m_sceneResourceScene, m_sceneResourceLifetime,
                                                          m_activeRecordingGeneration, kDisplacementLODFrameCount,
                                                          m_geometryUpdateFailed};
    return RendererDisplacement::ValidateRecording(m_pendingDisplacementUpdate, context, m_activeRecordingSlot,
                                                   frameSlot);
}

bool Renderer::AdvancePendingDisplacementRecording(
    unsigned int frameSlot,
    DisplacementRecordingStage stage,
    bool recordingSucceeded)
{
    const RendererDisplacement::ValidationContext context{m_sceneResourceScene, m_sceneResourceLifetime,
                                                          m_activeRecordingGeneration, kDisplacementLODFrameCount,
                                                          m_geometryUpdateFailed};

    if (!RendererDisplacement::AdvanceRecording(m_pendingDisplacementUpdate, m_frameDisplacementPreparation, context,
                                                m_activeRecordingSlot, frameSlot, stage, recordingSucceeded,
                                                m_rayTracingScene))
    {
        InvalidateRecordingEligibility();
        return false;
    }

    return true;
}

bool Renderer::HasUnresolvedDisplacementUpdate() const
{
    return m_heightmapRegistration.useUnresolved ||
           RendererDisplacement::HasUnresolvedState(m_pendingDisplacementUpdate, m_displacementCoverage);
}

bool Renderer::ValidatePendingDisplacementReservation() const
{
    const RendererDisplacement::ValidationContext context{m_sceneResourceScene, m_sceneResourceLifetime,
                                                          m_activeRecordingGeneration, kDisplacementLODFrameCount,
                                                          m_geometryUpdateFailed};

    return RendererDisplacement::ValidateReservation(m_pendingDisplacementUpdate, context);
}

bool Renderer::ReservePendingDisplacementSubmission()
{
    auto& pending = m_pendingDisplacementUpdate;

    const bool prepared = pending.status == DisplacementUpdateStatus::Prepared && m_activeRecordingGeneration == 0 &&
                          pending.recordingGeneration == 0 && pending.frameSlot == UINT_MAX &&

                          pending.requestRevision != 0 && !m_geometryUpdateFailed &&
                          ValidatePendingDisplacementIdentity();

    if ((!prepared && !ValidatePendingDisplacementRecording(pending.frameSlot)) ||
        pending.reservationOwnership != DisplacementReservationOwnership::None || pending.reservation.lifetime != 0 ||
        pending.reservation.reservationID != 0)
    {
        return false;
    }

    const auto reservation = m_commandContext.ReserveSubmissionFence();

    if (reservation.lifetime == 0 || reservation.reservationID == 0)
    {
        return false;
    }

    pending.reservation = reservation;
    pending.reservationOwnership = DisplacementReservationOwnership::Reserved;
    return true;
}

bool Renderer::CancelPendingDisplacementReservation()
{
    auto& pending = m_pendingDisplacementUpdate;

    if (!ValidatePendingDisplacementReservation())
    {
        return false;
    }

    // CommandContext proves this exact reservation has not attempted submission.

    if (!m_commandContext.CancelSubmissionFenceReservation(pending.reservation))
    {
        pending.reservationOwnership = DisplacementReservationOwnership::Uncertain;
        pending.status = DisplacementUpdateStatus::FailedUncertain;
        InvalidateRecordingEligibility();
        return false;
    }

    pending.reservation = {};
    pending.reservationOwnership = DisplacementReservationOwnership::None;
    // Cancellation resolves only token capacity, never geometry/AS/patch state.

    return true;
}

bool Renderer::ArmPendingDisplacementSubmission()
{
    auto& pending = m_pendingDisplacementUpdate;

    if (!ValidatePendingDisplacementReservation() ||
        m_activeRecordingGeneration != 0 ||
        pending.recordingGeneration == 0 ||
        pending.recordingStage != DisplacementRecordingStage::MergedTLAS)
    {
        return false;
    }

    if (pending.historyPreparation != DisplacementHistoryPreparation::Performed ||
        pending.tlasPreparation[pending.frameSlot] != DisplacementTLASPreparation::Recorded)
    {
        return false;
    }

    pending.reservationOwnership = DisplacementReservationOwnership::Attempting;
    pending.status = DisplacementUpdateStatus::Submitting;
    return true;
}

bool Renderer::ResolvePendingDisplacementSubmission(
    ReservedSubmissionResult result,
    SubmissionFenceToken token)
{
    auto& pending = m_pendingDisplacementUpdate;

    if (pending.status != DisplacementUpdateStatus::Submitting ||
        pending.reservationOwnership != DisplacementReservationOwnership::Attempting)
    {
        return false;
    }

    InvalidateRecordingEligibility();

    const bool emptyToken = token.fenceValue == 0 && token.lifetime == 0 && token.retentionID == 0;

    if (result == ReservedSubmissionResult::NotSubmitted && emptyToken)
    {
        pending.status = DisplacementUpdateStatus::Recording;
        pending.reservationOwnership = DisplacementReservationOwnership::Reserved;
        return true; // Still unresolved; cancellation must succeed explicitly.
    }

    pending.submission = token; // Preserve even erroneous returned ownership.
    const auto& recovery = pending.frameRecovery;

    const auto& original = m_displacementCoverage.original;
    const bool coveredFrameSubmission =
        recovery.state == DisplacementFrameExitState::Empty ||
        (recovery.state == DisplacementFrameExitState::Uncertain && recovery.generation == original.generation &&

         recovery.frameSlot == original.frameSlot && recovery.sceneLifetime == pending.sceneResourceLifetime &&
         original.outcome == DisplacementRecordingOutcome::Submitted &&
         original.submission.lifetime == token.lifetime && original.submission.retentionID == token.retentionID &&

         original.submission.fenceValue == token.fenceValue);

    if (result == ReservedSubmissionResult::Submitted && token.lifetime == pending.reservation.lifetime &&
        token.retentionID == pending.reservation.reservationID && m_geometryUpdateFailed && coveredFrameSubmission &&
        ValidatePendingDisplacementIdentity() &&
        m_commandContext.QuerySubmissionFenceStatus(token) != SubmissionFenceStatus::Error &&

        AssociatePendingDisplacementTLASSubmission(pending.frameSlot, pending.recordingGeneration, result, token))
    {
        pending.reservation = {};
        pending.reservationOwnership = DisplacementReservationOwnership::None;
        pending.status = DisplacementUpdateStatus::Submitted;

        if constexpr (kEnableFrameHitchDiagnostics)
        {
            m_frameHitchDiagnostics.current.submitted = true;
        }

        {
            FrameHitchProbe loggingProbe(
                *this,
                FrameHitchRegion::SubmittedPhaseLogging,
                !kSuppressDisplacementLifecycleLoggingForDiagnostics);

            if constexpr (!kSuppressDisplacementLifecycleLoggingForDiagnostics)
            {
                Log::Info("Async displacement submitted revision " + std::to_string(pending.requestRevision) +
                          ", fence " + std::to_string(token.fenceValue) + ".");
            }
        }

        if (pending.frameRecovery.state == DisplacementFrameExitState::Uncertain)
        {
            // Ownership transfers only after exact token and TLAS association.
            pending.frameRecovery.state = DisplacementFrameExitState::Submitted;
        }

        return true;
    }

    pending.reservationOwnership = DisplacementReservationOwnership::Uncertain;
    pending.status = DisplacementUpdateStatus::FailedUncertain;
    m_geometryUpdateFailed = true;
    return false;
}

bool Renderer::CapturePendingDisplacementTLASUse(
    unsigned int frameSlot,
    bool rebuilt,
    bool recordingSucceeded,
    std::uint64_t previousTLASRecording)
{
    auto& pending = m_pendingDisplacementUpdate;

    if (m_displacementCoverage.enabled)
    {
        const auto& receipt = m_displacementCoverage.current;

        if (!ValidatePendingDisplacementCoverage() ||
            receipt.outcome != DisplacementRecordingOutcome::Active ||
            receipt.generation != m_activeRecordingGeneration ||
            receipt.frameSlot != frameSlot)
        {
            return false;
        }

        if (receipt.geometry.tlasRecording != 0)
        {
            return false;
        }
    }

    const bool isDisplacementFrame = pending.status == DisplacementUpdateStatus::Recording;

    if (!m_geometryUpdateFailed || !ValidatePendingDisplacementIdentity() ||
        !ValidateFrameDisplacementPreparation(frameSlot))
    {
        return false;
    }

    if (isDisplacementFrame)
    {
        if (!ValidatePendingDisplacementRecording(frameSlot))
        {
            return false;
        }
    }
    else
    {
        const auto completion = ObservePendingDisplacementCompletion();

        if (completion != DisplacementCompletionObservation::Pending &&
            completion != DisplacementCompletionObservation::Complete)
        {
            return false;
        }
    }

    auto& use = pending.tlasUses[frameSlot];

    if (use.consumerGeneration == m_activeRecordingGeneration)
    {
        return false; // No second upload/build evidence for this consumer recording.
    }

    TLASGeometryRecordingEvidence evidence{};

    if (rebuilt)
    {
        if (!recordingSucceeded || previousTLASRecording == m_rayTracingScene.GetTLASRecordingIdentity() ||
            (isDisplacementFrame && pending.recordingStage != DisplacementRecordingStage::BLAS))
        {
            return false;
        }

        const auto batch =

            isDisplacementFrame ? m_rayTracingScene.GetExclusiveRetirementBatch(pending.mesh) : pending.retirementBatch;

        if (!m_rayTracingScene.TryGetTLASGeometryEvidence(pending.mesh, batch, pending.requestRevision,
                                                          m_activeRecordingGeneration, frameSlot, evidence))
        {
            return false;
        }
    }
    else
    {
        if (isDisplacementFrame)
        {
            return false; // The displacement-containing frame must build merged TLAS.
        }

        if (m_displacementCoverage.enabled && m_displacementCoverage.submittedCount != 0)
        {
            // Revalidate the current shared TLAS for this new consumer. The
            // prior receipt itself remains immutable historical evidence.

            const auto& source = m_displacementCoverage.submitted[m_displacementCoverage.submittedCount - 1];

            if (m_rayTracingScene.ValidateTLASGeometryEvidence(pending.mesh, source.geometry))
            {
                evidence = source.geometry;
            }
        }
        else
        {
            for (const auto& source : pending.tlasUses)
            {

                if (source.evidence.geometryVersion == pending.requestRevision &&

                    m_commandContext.QuerySubmissionFenceStatus(source.submission) != SubmissionFenceStatus::Error &&
                    m_rayTracingScene.ValidateTLASGeometryEvidence(pending.mesh, source.evidence))
                {
                    evidence = source.evidence;
                    break;
                }
            }
        }

        if (evidence.tlasRecording == 0)
        {
            return false;
        }
    }

    if (!isDisplacementFrame && evidence.blasAddress != pending.replacementBLASAddress)
    {
        return false;
    }

    if (isDisplacementFrame)
    {
        pending.retirementBatch = evidence.retirement;
        pending.replacementBLASAddress = evidence.blasAddress;
    }

    use = {evidence, m_activeRecordingGeneration, {}};

    if (m_displacementCoverage.enabled)
    {
        m_displacementCoverage.current.geometry = evidence;
    }

    if (!isDisplacementFrame && rebuilt)
    {
        pending.tlasPreparation[frameSlot] = DisplacementTLASPreparation::Recorded;
    }

    // No-build use leaves this slot's upload preparation pending: consuming the
    // shared TLAS does not mean this slot's instance upload was rewritten.
    return true;
}

bool Renderer::AssociatePendingDisplacementTLASSubmission(
    unsigned int frameSlot,
    std::uint64_t generation,
    ReservedSubmissionResult result,
    SubmissionFenceToken token)
{
    auto& pending = m_pendingDisplacementUpdate;

    const auto fail = [&]()
    {
        m_geometryUpdateFailed = true;
        pending.status = DisplacementUpdateStatus::FailedUncertain;

        return false;
    };

    if (frameSlot >= kDisplacementLODFrameCount || !generation || m_activeRecordingGeneration != 0 ||

        !ValidatePendingDisplacementIdentity() || !m_geometryUpdateFailed ||

        (pending.status != DisplacementUpdateStatus::Submitting &&

         pending.status != DisplacementUpdateStatus::Submitted))
    {
        return fail();
    }

    auto& use = pending.tlasUses[frameSlot];

    const bool alreadyAssociated =

        use.submission.fenceValue != 0 || use.submission.lifetime != 0 || use.submission.retentionID != 0;

    const bool matchesGeometry = use.evidence.geometryVersion == pending.requestRevision &&

                                 use.evidence.blasAddress == pending.replacementBLASAddress;

    if (result != ReservedSubmissionResult::Submitted || use.consumerGeneration != generation || alreadyAssociated ||
        !matchesGeometry || !m_rayTracingScene.ValidateTLASGeometryEvidence(pending.mesh, use.evidence) ||
        m_commandContext.GetFrameFenceValue(frameSlot) != token.fenceValue ||
        m_commandContext.QuerySubmissionFenceStatus(token) == SubmissionFenceStatus::Error)
    {
        return fail();
    }

    // Later frames must belong to this queue lifetime and follow the original
    // displacement submission. The first submission has no predecessor token.

    if (pending.status == DisplacementUpdateStatus::Submitted &&
        (token.lifetime != pending.submission.lifetime || token.fenceValue <= pending.submission.fenceValue))
    {
        return fail();
    }

    use.submission = token;
    return true;
}

bool Renderer::ValidatePendingDisplacementIdentity() const
{
    const RendererDisplacement::ValidationContext context{m_sceneResourceScene, m_sceneResourceLifetime};

    return RendererDisplacement::ValidateIdentity(m_pendingDisplacementUpdate, context);
}

bool Renderer::PreparePendingDisplacementForFrame(unsigned int frameSlot)
{
    PendingDisplacementUpdate& pending = m_pendingDisplacementUpdate;

    // One record owns the exclusive update: replacement/release/Render entry
    // points remain guarded until it is resolved. Never prepare a submitted or
    // uncertain record, or invalidate history for a different slot/lifetime.

    if (pending.status != DisplacementUpdateStatus::Recording ||
        frameSlot >= kDisplacementLODFrameCount ||
        pending.frameSlot != frameSlot ||
        pending.submission.fenceValue != 0)
    {
        return false;
    }

    if (pending.submission.lifetime != 0 ||
        pending.submission.retentionID != 0 ||
        pending.requestRevision == 0 ||
        !m_geometryUpdateFailed)
    {
        return false;
    }

    if (!HasUnresolvedDisplacementUpdate() ||
        !ValidatePendingDisplacementIdentity() ||
        !ValidatePendingDisplacementRecording(frameSlot) ||
        !ValidateFrameDisplacementPreparation(frameSlot))
    {
        return false;
    }

    if (pending.historyPreparation == DisplacementHistoryPreparation::NotRequested)
    {
        pending.historyPreparation = DisplacementHistoryPreparation::Requested;
    }

    if (pending.historyPreparation == DisplacementHistoryPreparation::Requested)
    {
        ResetSceneHistory();
        pending.historyPreparation = DisplacementHistoryPreparation::Performed;
    }

    for (unsigned int slot = 0; slot < kDisplacementLODFrameCount; ++slot)
    {
        if (pending.tlasPreparation[slot] == DisplacementTLASPreparation::NotRequested)
        {
            pending.tlasPreparation[slot] = DisplacementTLASPreparation::Requested;
            m_pendingGeometryTLASRefresh[slot] = true;
        }

        // Repeated preparation must not re-arm consumed requests or downgrade
        // a Recorded milestone. No TLAS upload/build occurs in this helper.
    }

    return true;
}

bool Renderer::PrepareFrameDisplacementRequest(
    const Scene& scene,
    Mesh& mesh,
    std::uint64_t requestRevision,
    const DisplacementMeshSettings& settings)
{
    if (!kEnableFrameDisplacementRequestPreparation ||
        HasUnresolvedDisplacementUpdate() ||
        m_geometryUpdateFailed ||
        m_completedDisplacementUpdate.requestRevision != 0)
    {
        return false;
    }

    if (m_activeRecordingGeneration != 0 ||
        !requestRevision ||
        !m_sceneResourceLifetime ||
        m_sceneResourceScene != &scene)
    {
        return false;
    }

    if (settings.patchLODs.empty())
    {
        return false;
    }

    if (!ValidateDisplacementHeightmapRegistration() || m_heightmapRegistration.scene != &scene ||

        m_heightmapRegistration.mesh != &mesh)
    {
        return false;
    }

    const auto& objects = scene.GetRenderObjects();

    if (!std::any_of(objects.begin(), objects.end(),
                     [&mesh](const RenderObject& object) { return object.mesh == &mesh; }))
    {
        return false;
    }

    // Allocate/build the owned CPU snapshot before reserving scarce token capacity.
    std::vector<std::uint32_t> patchLODs(
        settings.patchLODs.begin(),
        settings.patchLODs.end());
    DisplacementMeshSettings ownedSettings = settings;

    ownedSettings.patchLODs = patchLODs;
    DisplacementGPUIndexLayout layout{};

    RayTracingGeometryCounts counts{};

    if (!BuildDisplacementRequestLayout(mesh, ownedSettings, layout, counts) ||

        !m_displacementGPUPatchBuffer.HasReadyPersistentCapacity(layout.patches.size()))
    {
        return false; // Growth belongs to a separate confirmed-idle preparation stage.
    }

    auto& pending = m_pendingDisplacementUpdate;
    pending.scene = &scene;
    pending.sceneResourceLifetime = m_sceneResourceLifetime;
    pending.sceneRevision = scene.GetStructureRevision();

    pending.requestRevision = requestRevision;
    pending.mesh = &mesh;
    pending.targetCounts = counts;

    pending.patchLODs = std::move(patchLODs);

    pending.settings = settings;
    pending.settings.patchLODs = {}; // Never store a view into caller/local memory.
    pending.layout = std::move(layout);

    pending.preparedPatchResource = m_displacementGPUPatchBuffer.GetResource();
    pending.preparedPatchDescriptor = m_displacementGPUPatchBuffer.GetDescriptorIndex();
    pending.status = DisplacementUpdateStatus::Prepared;

    // Reservation is the final fallible acquisition. Failure cannot have submitted
    // anything or changed patch tracking, so there is no reservation to cancel.

    if (!ReservePendingDisplacementSubmission())
    {
        pending.status = DisplacementUpdateStatus::Discarded;

        if (!pending.ResetResolved())
        {
            m_geometryUpdateFailed = true;
        }

        return false;
    }

    return true;
}

bool Renderer::CancelPreparedDisplacementRequest()
{
    auto& pending = m_pendingDisplacementUpdate;

    if (pending.status != DisplacementUpdateStatus::Prepared || m_displacementCoverage.enabled ||
        !ValidatePendingDisplacementReservation())
    {
        return false;
    }

    if (!CancelPendingDisplacementReservation())
    {
        m_geometryUpdateFailed = true;
        return false;
    }

    pending.status = DisplacementUpdateStatus::Discarded;

    if (!pending.ResetResolved())
    {
        m_geometryUpdateFailed = true;
        return false;
    }

    return true;
}

bool Renderer::ActivatePreparedDisplacementRequest(unsigned int frameSlot)
{
    auto& pending = m_pendingDisplacementUpdate;

    if (!kEnableFrameDisplacementRequestPreparation ||
        pending.status != DisplacementUpdateStatus::Prepared ||
        !ValidatePendingDisplacementIdentity() ||
        !ValidateDisplacementHeightmapRegistration())
    {
        return false;
    }

    if (m_geometryUpdateFailed ||
        m_displacementCoverage.enabled ||
        m_activeRecordingGeneration == 0 ||
        m_activeRecordingSlot != frameSlot)
    {
        return false;
    }

    if (frameSlot >= kDisplacementLODFrameCount ||
        !m_frameDisplacementPreparation.slotAcquired ||
        m_frameDisplacementPreparation.uploadResetCount != 0 ||
        pending.reservationOwnership != DisplacementReservationOwnership::Reserved)
    {
        return false;
    }

    if (!pending.reservation.lifetime ||
        !pending.reservation.reservationID ||
        pending.preparedPatchResource != m_displacementGPUPatchBuffer.GetResource() ||
        pending.preparedPatchDescriptor != m_displacementGPUPatchBuffer.GetDescriptorIndex())
    {
        return false;
    }

    if (!m_displacementGPUPatchBuffer.HasReadyPersistentCapacity(pending.layout.patches.size()))
    {
        return false;
    }

    const RendererDisplacement::ValidationContext context{m_sceneResourceScene, m_sceneResourceLifetime,
                                                          m_activeRecordingGeneration, kDisplacementLODFrameCount,
                                                          m_geometryUpdateFailed};
    return RendererDisplacement::ActivateRecording(pending, m_displacementCoverage, context, m_activeRecordingSlot,
                                                   frameSlot, m_geometryUpdateFailed);
}

Renderer::DisplacementFrameExitScope::DisplacementFrameExitScope(Renderer& owner) : renderer(owner)
{
    const auto& coverage = renderer.m_displacementCoverage;
    const bool laterFrame = renderer.ValidatePendingDisplacementCoverage() && coverage.submittedCount != 0 &&
                            renderer.m_pendingDisplacementUpdate.status == DisplacementUpdateStatus::Submitted &&
                            coverage.current.outcome == DisplacementRecordingOutcome::Active &&
                            coverage.current.generation == renderer.m_activeRecordingGeneration &&
                            coverage.current.frameSlot == renderer.m_activeRecordingSlot &&
                            renderer.m_activeRecordingGeneration != 0 &&
                            renderer.ValidateDisplacementHeightmapRegistration();

    if (kEnableDisplacementFrameExitResolution && renderer.m_displacementExitScopeGeneration == 0 &&
        (renderer.ValidatePendingDisplacementRecording(renderer.m_activeRecordingSlot) || laterFrame))
    {
        renderer.m_displacementExitScopeGeneration = renderer.m_activeRecordingGeneration;
        armed = true;
    }
}

Renderer::DisplacementFrameExitScope::~DisplacementFrameExitScope()
{
    // Not instantiated by active Render. The future scope covers every return,
    // rather than relying on each individual allocation/pass failure branch.

    if (!armed)
    {
        return;
    }

    if (!renderer.ResolveDisplacementFrameExit())
    {
        Log::Error("Displacement frame exit remains unresolved; rendering and teardown remain blocked.");
    }

    renderer.m_displacementExitScopeGeneration = 0;
}

bool Renderer::CaptureDisplacementFrameRecovery(unsigned int frameSlot)
{
    auto& pending = m_pendingDisplacementUpdate;

    if (!kEnableDisplacementFrameExitResolution ||
        m_displacementExitScopeGeneration == 0 ||
        m_displacementExitScopeGeneration != m_activeRecordingGeneration ||
        pending.frameRecovery.state != DisplacementFrameExitState::Empty)
    {
        return false;
    }

    if (!ValidatePendingDisplacementRecording(frameSlot) ||
        !ValidatePendingDisplacementReservation() ||
        !ValidatePendingDisplacementCoverage() ||
        !ValidateFrameDisplacementPreparation(frameSlot))
    {
        return false;
    }

    if (!ValidateDisplacementHeightmapRegistration() ||
        m_heightmapRegistration.mesh != pending.mesh ||
        m_heightmapRegistration.sceneLifetime != pending.sceneResourceLifetime)
    {
        return false;
    }

    DisplacementFrameRecovery recovery{};
    recovery.generation = m_activeRecordingGeneration;
    recovery.sceneLifetime = pending.sceneResourceLifetime;
    recovery.frameSlot = frameSlot;

    recovery.mesh = pending.mesh;

    recovery.heightmap = m_heightmapRegistration.texture;
    recovery.patchResource = m_displacementGPUPatchBuffer.GetResource();
    recovery.vertexState = pending.mesh->GetVertexBufferState();

    recovery.indexState = pending.mesh->GetIndexBufferState();

    recovery.heightmapStates = recovery.heightmap->GetSubresourceStates();

    recovery.patchState = m_displacementGPUPatchBuffer.GetStateSnapshot();
    recovery.state = DisplacementFrameExitState::Recording;

    pending.frameRecovery = std::move(recovery);
    return true;
}

bool Renderer::ResolveDisplacementFrameExit()
{
    // Later frames own their coverage recording, not the original update's
    // snapshots. In particular, never restore already-submitted patch states.

    if (m_displacementCoverage.enabled && m_displacementCoverage.submittedCount != 0 &&
        m_displacementCoverage.current.generation != m_pendingDisplacementUpdate.recordingGeneration)
    {
        return ResolveLaterDisplacementFrameExit();
    }

    auto& pending = m_pendingDisplacementUpdate;
    auto& recovery = pending.frameRecovery;

    if (recovery.state == DisplacementFrameExitState::Empty && m_displacementCoverage.enabled)
    {
        // Diagnostic allocation can fail before snapshots/compute exist.
        // Resolve that original whole-frame recording too, without inventing
        // snapshots or treating eligibility invalidation as discard.
        const auto& receipt = m_displacementCoverage.current;
        const bool matching =
            ValidatePendingDisplacementIdentity() && pending.status == DisplacementUpdateStatus::Recording &&
            receipt.generation == pending.recordingGeneration &&
            receipt.generation == m_displacementExitScopeGeneration && receipt.frameSlot == pending.frameSlot;
        const bool discarded = matching && AbortTrackedRecording();
        const bool canceled = discarded && CancelPendingDisplacementReservation();
        m_displacementCoverage.failed = true;
        m_geometryUpdateFailed = true;

        if (!canceled)
        {
            pending.status = DisplacementUpdateStatus::FailedUncertain;
        }

        return canceled;
    }

    if (recovery.state == DisplacementFrameExitState::Empty ||
        recovery.state == DisplacementFrameExitState::Discarded ||

        recovery.state == DisplacementFrameExitState::Submitted)
    {
        return true; // Submitted ownership belongs to completion, never abort.
    }

    m_geometryUpdateFailed = true;
    const auto& receipt = m_displacementCoverage.current;
    const bool unsubmitted =

        recovery.state == DisplacementFrameExitState::Recording || recovery.state == DisplacementFrameExitState::Ended;
    const bool matchingRecording =
        recovery.generation != 0 && recovery.generation == pending.recordingGeneration &&
        recovery.generation == receipt.generation && recovery.frameSlot == pending.frameSlot &&
        recovery.frameSlot == receipt.frameSlot &&
        (m_activeRecordingGeneration == 0 ||
         (m_activeRecordingGeneration == recovery.generation && m_activeRecordingSlot == recovery.frameSlot));
    const bool validIdentity = ValidatePendingDisplacementIdentity() && ValidateDisplacementHeightmapRegistration() &&
                               recovery.sceneLifetime == pending.sceneResourceLifetime &&
                               recovery.mesh == pending.mesh && recovery.heightmap == m_heightmapRegistration.texture &&
                               recovery.patchResource == m_displacementGPUPatchBuffer.GetResource();

    if (!unsubmitted || !matchingRecording || !validIdentity ||
        pending.reservationOwnership != DisplacementReservationOwnership::Reserved ||

        (receipt.outcome != DisplacementRecordingOutcome::Active &&

         receipt.outcome != DisplacementRecordingOutcome::Recorded) ||
        !AbortTrackedRecording())
    {
        // Failed Close (including Close after End) is not discard confirmation.
        // Keep snapshots, staging, reservation/token and all AS resources owned.
        recovery.state = DisplacementFrameExitState::Uncertain;

        pending.status = DisplacementUpdateStatus::FailedUncertain;
        m_displacementCoverage.failed = true;
        InvalidateRecordingEligibility();
        return false;
    }

    recovery.mesh->RestoreBufferStatesAfterAbort(recovery.vertexState, recovery.indexState);
    recovery.heightmap->RestoreStatesAfterAbort(recovery.heightmapStates);
    m_displacementGPUPatchBuffer.RestoreStateAfterAbort(recovery.patchState);
    recovery.state = DisplacementFrameExitState::Discarded;

    // This restores only tracking for discarded barriers. AS ownership and
    // TLAS CPU instance descriptions have no rollback contract. Retain both
    // old/new resources and the reservation; never resume this failed frame.

    if (recovery.asMutationStarted)
    {
        pending.status = DisplacementUpdateStatus::FailedUncertain;
    }

    return true;
}

Renderer::FrameDisplacementRecordingResult Renderer::RecordPendingDisplacementForFrame(unsigned int frameSlot)
{
    FrameHitchProbe displacementProbe(
        *this,
        FrameHitchRegion::Displacement);

    auto& pending = m_pendingDisplacementUpdate;

    if (!kEnableFrameIntegratedDisplacementRecording || !ValidatePendingDisplacementRecording(frameSlot) ||
        !ValidateFrameDisplacementPreparation(frameSlot) || !ValidatePendingDisplacementReservation() ||
        !ValidatePendingDisplacementCoverage() || m_frameDisplacementPreparation.ordinaryTLASRecorded ||
        m_frameDisplacementPreparation.mergedTLASRecorded || pending.recordingStage != DisplacementRecordingStage::None)
    {
        return FrameDisplacementRecordingResult::Rejected;
    }

    if (!ValidateDisplacementHeightmapRegistration() || m_heightmapRegistration.mesh != pending.mesh ||
        m_heightmapRegistration.sceneLifetime != pending.sceneResourceLifetime)
    {

        return FrameDisplacementRecordingResult::Rejected;
    }

    Texture& heightmap = *m_heightmapRegistration.texture;

    if (pending.patchLODs.empty() || pending.preparedPatchResource != m_displacementGPUPatchBuffer.GetResource() ||
        pending.preparedPatchDescriptor != m_displacementGPUPatchBuffer.GetDescriptorIndex())
    {
        return FrameDisplacementRecordingResult::Rejected;
    }

    const auto& layout = pending.layout;
    const auto& settings = pending.settings;

    if (pending.targetCounts.indexCount != layout.totalIndexCount ||
        !m_displacementGPUPatchBuffer.HasReadyPersistentCapacity(layout.patches.size()))
    {
        return FrameDisplacementRecordingResult::Rejected;
    }

    SubmittedDisplacementBinding binding{};

    if (!TryBuildPendingDisplacementBinding(pending.mesh, binding))
    {
        return FrameDisplacementRecordingResult::Rejected;
    }

    m_displacementWallClock.recordingStart = CpuTiming::Clock::now();
    Mesh& mesh = *pending.mesh;

    if (!CaptureDisplacementFrameRecovery(frameSlot))
    {
        return FrameDisplacementRecordingResult::Rejected;
    }

    const auto abortFrame = [&]()
    {
        const bool discarded = ResolveDisplacementFrameExit();
        return discarded ? FrameDisplacementRecordingResult::Discarded : FrameDisplacementRecordingResult::Unresolved;
    };

    if (!PreparePendingDisplacementForFrame(frameSlot))
    {
        return abortFrame();
    }

    ID3D12GraphicsCommandList* commandList = m_commandContext.GetCommandList();

    ID3D12DescriptorHeap* heaps[] = {m_resourceDescriptorHeap.GetHeap()};
    commandList->SetDescriptorHeaps(1, heaps);

    CpuTiming::Clock::time_point diagnosticStageStart{};

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        diagnosticStageStart = CpuTiming::Clock::now();
    }

    if (!RendererDisplacement::RecordPatchAndCompute(commandList, m_displacementGPUPatchBuffer, m_uploadBuffer,
                                                     m_displacementVerticesPass, m_displacementIndicesPass, mesh,
                                                     heightmap, layout, settings))
    {
        return abortFrame();
    }

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        m_displacementWallClock.diagnosticComputeMilliseconds = CpuTiming::MillisecondsSince(diagnosticStageStart);

        m_frameHitchDiagnostics.current.compute = m_displacementWallClock.diagnosticComputeMilliseconds;
    }

    if (!AdvancePendingDisplacementRecording(frameSlot, DisplacementRecordingStage::Compute, true))
    {
        return abortFrame();
    }

    m_displacementWallClock.computeEnd = CpuTiming::Clock::now();

    // Read transitions and UAV barriers come from the existing compute/AS APIs.
    // No inter-submission decay occurs between compute and BLAS input reads.

    pending.frameRecovery.asMutationStarted = true;
    ID3D12GraphicsCommandList4* buildList = m_commandContext.GetRayTracingCommandList();

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        diagnosticStageStart = CpuTiming::Clock::now();
    }

    if (!m_rayTracingScene.RebuildMeshBLAS(m_graphicsDevice.GetRayTracingDevice(), buildList, &mesh,
                                           pending.targetCounts))
    {
        return abortFrame();
    }

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        m_displacementWallClock.diagnosticBLASMilliseconds = CpuTiming::MillisecondsSince(diagnosticStageStart);

        m_frameHitchDiagnostics.current.blas = m_displacementWallClock.diagnosticBLASMilliseconds;
    }

    if (!AdvancePendingDisplacementRecording(frameSlot, DisplacementRecordingStage::BLAS, true))
    {
        return abortFrame();
    }

    const auto previousTLASRecording = m_rayTracingScene.GetTLASRecordingIdentity();

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        diagnosticStageStart = CpuTiming::Clock::now();
    }

    if (!m_rayTracingScene.UpdateTransformsAndRefreshTLASReferences(buildList, frameSlot, *pending.scene))
    {
        return abortFrame();
    }

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        m_displacementWallClock.diagnosticTLASMilliseconds = CpuTiming::MillisecondsSince(diagnosticStageStart);
        m_frameHitchDiagnostics.current.tlas = m_displacementWallClock.diagnosticTLASMilliseconds;
    }

    if (!CapturePendingDisplacementTLASUse(frameSlot, true, true, previousTLASRecording) ||

        !AdvancePendingDisplacementRecording(frameSlot, DisplacementRecordingStage::MergedTLAS, true))
    {
        return abortFrame();
    }

    m_pendingGeometryTLASRefresh[frameSlot] = false;
    m_displacementWallClock.recordingEnd = CpuTiming::Clock::now();

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        m_frameHitchDiagnostics.current.recorded = true;
    }

    {
        FrameHitchProbe loggingProbe(
            *this,
            FrameHitchRegion::RecordedPhaseLogging,
            !kSuppressDisplacementLifecycleLoggingForDiagnostics);

        if constexpr (!kSuppressDisplacementLifecycleLoggingForDiagnostics)
        {
            Log::Info("Async displacement recorded revision " + std::to_string(pending.requestRevision) +
                      ", frame slot " + std::to_string(frameSlot) + ".");
        }
    }

    return FrameDisplacementRecordingResult::Recorded;
}

Renderer::DisplacementCompletionObservation Renderer::ObservePendingDisplacementCompletion() const
{
    const PendingDisplacementUpdate& pending = m_pendingDisplacementUpdate;

    if (pending.status == DisplacementUpdateStatus::Empty)
    {
        // An allegedly empty record must not conceal a retained submission.

        return HasUnresolvedDisplacementUpdate() ? DisplacementCompletionObservation::Error
                                                 : DisplacementCompletionObservation::NoUpdate;
    }

    if (pending.status != DisplacementUpdateStatus::Submitted ||
        pending.reservationOwnership != DisplacementReservationOwnership::None || pending.reservation.lifetime != 0 ||
        pending.reservation.reservationID != 0 || !ValidatePendingDisplacementIdentity() ||

        pending.frameSlot >= kDisplacementLODFrameCount)
    {
        return DisplacementCompletionObservation::Error;
    }

    // Query validates token provenance and detects device loss. No state is
    // changed, even on completion/error: unresolved lifecycle guards stay armed.

    switch (m_commandContext.QuerySubmissionFenceStatus(pending.submission))
    {
    case SubmissionFenceStatus::Pending:
        return DisplacementCompletionObservation::Pending;
    case SubmissionFenceStatus::Complete:
        return DisplacementCompletionObservation::Complete;
    case SubmissionFenceStatus::Error:
        return DisplacementCompletionObservation::Error;
    }

    return DisplacementCompletionObservation::Error;
}

bool Renderer::FinalizePendingDisplacementUpdate(DisplacementCompletionResult& result)
{
    if (m_displacementCoverage.enabled)
    {
        return false; // Covered updates require selective, async-aware retirement.
    }

    PendingDisplacementUpdate& pending = m_pendingDisplacementUpdate;

    const auto fail = [&]()
    {
        m_geometryUpdateFailed = true;
        pending.status = DisplacementUpdateStatus::FailedUncertain;

        Log::Error("Pending displacement finalization failed; rendering and resource reuse remain disabled.");
        return false;
    };
    const DisplacementCompletionObservation observation = ObservePendingDisplacementCompletion();

    if (observation != DisplacementCompletionObservation::Complete)
    {
        if (observation == DisplacementCompletionObservation::Error)
        {
            return fail();
        }

        return false; // NoUpdate/Pending: observation only, no mutation.
    }

    // The single record plus guarded Render/replacement/release paths excludes
    // later old-BLAS readers and other retirement batches. This update's DIRECT
    // fence covers earlier queue users. Do not relax those guards independently.

    if (!m_geometryUpdateFailed || !HasUnresolvedDisplacementUpdate() || !ValidatePendingDisplacementIdentity() ||

        pending.requestRevision == 0)
    {
        return fail();
    }

    Mesh& mesh = *pending.mesh; // Identity/lifetime validated before dereference.
    const size_t vertexCount = pending.targetCounts.vertexCount;
    const size_t indexCount = pending.targetCounts.indexCount;

    ID3D12Resource* vertices = mesh.GetVertexBufferResource();
    ID3D12Resource* indices = mesh.GetIndexBufferResource();

    if (vertexCount == 0 || indexCount == 0 || indexCount % 3 != 0 || vertexCount > mesh.GetVertexCapacity() ||

        indexCount > mesh.GetIndexCapacity() || mesh.GetVertexStride() != 48 || vertexCount > UINT_MAX / 48u ||
        indexCount > UINT_MAX / sizeof(unsigned int) || mesh.GetIndexFormat() != DXGI_FORMAT_R32_UINT || !vertices ||
        !indices || mesh.GetVertexBufferAddress() == 0 || mesh.GetIndexBufferAddress() == 0 ||

        mesh.GetVertexBufferUAVDescriptorIndex() == UINT_MAX || mesh.GetIndexBufferUAVDescriptorIndex() == UINT_MAX)
    {
        return fail();
    }

    const D3D12_RESOURCE_DESC vertexDesc = vertices->GetDesc();
    const D3D12_RESOURCE_DESC indexDesc = indices->GetDesc();

    if (vertexDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||

        indexDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || vertexDesc.Width < vertexCount * 48u ||

        indexDesc.Width < indexCount * sizeof(unsigned int))
    {
        return fail();
    }

    const DisplacementCompletionResult completed{pending.scene, pending.sceneResourceLifetime, pending.sceneRevision,
                                                 pending.requestRevision};
    // One-shot boundary: any subsequent failure becomes terminal, so neither
    // non-idempotent patch completion nor publication can be repeated.

    pending.status = DisplacementUpdateStatus::Finalizing;

    if (!m_displacementGPUPatchBuffer.ConfirmUpdateCompletion(true))
    {
        return fail();
    }

    if (!mesh.SetGPUGeneratedGeometryCounts(vertexCount, indexCount))
    {
        return fail();
    }

    m_rayTracingScene.ReleaseRetiredBLASResources();
    ResetSceneHistory();
    m_pendingGeometryTLASRefresh[0] = true;
    m_pendingGeometryTLASRefresh[1] = true;

    if (!m_commandContext.ReleaseSubmissionFence(pending.submission))
    {
        return fail();
    }

    pending.submission = {};
    pending.status = DisplacementUpdateStatus::Completed;

    if (!pending.ResetResolved())
    {
        return fail();
    }

    result = completed;
    m_geometryUpdateFailed = false;
    return true;
}

bool Renderer::FinalizeCoveredDisplacementUpdate(DisplacementCompletionResult& result)
{
    auto& pending = m_pendingDisplacementUpdate;
    auto& coverage = m_displacementCoverage;

    const auto fail = [&]()
    {
        coverage.failed = true;
        pending.status = DisplacementUpdateStatus::FailedUncertain;
        m_geometryUpdateFailed = true;
        Log::Error("Covered displacement finalization failed; ownership and rendering remain blocked.");

        return false;
    };

    const auto observation = ObservePendingDisplacementCompletion();

    if (observation != DisplacementCompletionObservation::Complete)
    {
        if (observation == DisplacementCompletionObservation::Error)
        {
            return fail();
        }

        return false; // NoUpdate/Pending cannot resolve coverage or publish geometry.
    }

    if (!ValidatePendingDisplacementRetirementCoverage() || m_activeRecordingGeneration != 0)
    {
        return fail();
    }

    // Discharge later provenance only after the full retirement proof. Their
    // replacement resources remain owned even when their fences are pending.

    if (!CleanupLaterDisplacementSubmissions())
    {
        return fail();
    }

    if (!BeginPendingDisplacementCoverageResolution())
    {
        return fail();
    }

    const DisplacementCompletionResult completed{pending.scene, pending.sceneResourceLifetime, pending.sceneRevision,
                                                 pending.requestRevision};
    pending.status = DisplacementUpdateStatus::Finalizing;

    if (!m_displacementGPUPatchBuffer.ConfirmUpdateCompletion(true))
    {
        return fail();
    }

    if (!pending.mesh->SetGPUGeneratedGeometryCounts(pending.targetCounts.vertexCount, pending.targetCounts.indexCount))
    {
        return fail();
    }

    // The completed original DIRECT submission covers all earlier old-BLAS
    // users. Every later accepted submission uses this replacement version.
    // Release only this exclusive batch; later readers may still use the new AS.

    if (!m_rayTracingScene.ReleaseRetirementBatch(pending.mesh, pending.retirementBatch, true, true))
    {
        return fail();
    }

    // History was performed before the original frame's temporal consumers.
    // Keep TLAS flags and pending per-slot uploads as recorded; shared-TLAS reuse
    // never implies that another slot's instance upload has been consumed.

    if (!m_commandContext.ReleaseSubmissionFence(pending.submission))
    {
        return fail();
    }

    pending.submission = {};

    pending.status = DisplacementUpdateStatus::Completed;

    if (!pending.ResetResolved())
    {
        return fail();
    }

    // This is the sole coverage-clear point. All fallible finalization steps
    // succeeded; no active/unsubmitted recording or owned original token remains.
    // Checked cleanup resolved all later-token owners; historical receipts
    // remained intact throughout finalization, including every failure path.

    coverage = {};
    result = completed;
    m_geometryUpdateFailed = false;
    return true;
}

bool Renderer::TryGetSubmittedDisplacementBinding(
    const Mesh* mesh,
    SubmittedDisplacementBinding& binding) const
{
    const PendingDisplacementUpdate& pending = m_pendingDisplacementUpdate;

    // Compare identity without dereferencing the caller's pointer. Observation
    // validates scene lifetime/membership and retained submission provenance.

    if (!mesh || mesh != pending.mesh || pending.requestRevision == 0)
    {
        return false;
    }

    const DisplacementCompletionObservation observation = ObservePendingDisplacementCompletion();

    if (observation != DisplacementCompletionObservation::Pending &&
        observation != DisplacementCompletionObservation::Complete)
    {

        return false;
    }

    return TryBuildPendingDisplacementBinding(mesh, binding);
}

bool Renderer::TryGetRecordingDisplacementBinding(
    const Mesh* mesh,
    unsigned int frameSlot,
    SubmittedDisplacementBinding& binding) const
{
    const PendingDisplacementUpdate& pending = m_pendingDisplacementUpdate;

    if (!mesh ||
        mesh != pending.mesh ||
        !ValidatePendingDisplacementRecording(frameSlot) ||
        !ValidateFrameDisplacementPreparation(frameSlot))
    {
        return false;
    }

    if (!m_frameDisplacementPreparation.mergedTLASRecorded ||
        pending.historyPreparation != DisplacementHistoryPreparation::Performed ||
        pending.recordingStage != DisplacementRecordingStage::MergedTLAS ||
        pending.tlasPreparation[frameSlot] != DisplacementTLASPreparation::Recorded)
    {
        return false;
    }

    return TryBuildPendingDisplacementBinding(mesh, binding);
}

bool Renderer::TryBuildPendingDisplacementBinding(

    const Mesh* mesh,
    SubmittedDisplacementBinding& binding) const
{
    const PendingDisplacementUpdate& pending = m_pendingDisplacementUpdate;

    // Independently protect the dereference; lifecycle/queue eligibility is
    // established by the recording-time or submitted-version caller.

    if (!mesh || mesh != pending.mesh || !ValidatePendingDisplacementIdentity())
    {
        return false;
    }

    return RendererDisplacement::TryBuildGeometryBinding(*mesh, pending.targetCounts, binding);
}

// --------------
// Initialization
// --------------

bool Renderer::Initialize(
    void* windowHandle,
    unsigned int width,
    unsigned int height,
    const std::filesystem::path& assetRoot, std::span<const std::byte> packagedShaders, const HDRImageData* initialEnvironment, std::string environmentIdentity)
{
    if (m_heightmapRegistration.owner)
    {
        Log::Error("Cannot reinitialize Renderer before checked heightmap unregistration.");
        return false;
    }

    RecordingEligibilityScope recordingEligibility{*this};

    if (HasUnresolvedDisplacementUpdate())
    {
        Log::Error("Cannot initialize Renderer with an unresolved displacement update.");
        return false;
    }

    m_packagedRuntime = !packagedShaders.empty();
    m_sceneResourceLifetime = 0;
    m_sceneResourceScene = nullptr;

    m_applicationWidth = width;
    m_applicationHeight = height;
    m_sceneWidth = width;
    m_sceneHeight = height;

    if (!InitializeDevice())
    {
        return false;
    }

    if (!m_gpuTimestampProfiler.Initialize(m_graphicsDevice.GetDevice(), m_commandContext.GetCommandQueue()))
    {
        Log::Error("Failed to init GPU timestamp profiler.");

        return false;
    }

    ID3D12Device* device = m_graphicsDevice.GetDevice();

    m_assetRoot = assetRoot;

    const std::filesystem::path shaderRoot = m_assetRoot / "Shaders";

    if (!m_packagedRuntime && !DiscoverEnvironments())
    {
        return false;
    }

    if (!(m_packagedRuntime ? m_shaderLibrary.InitializePackaged(packagedShaders) : m_shaderLibrary.Initialize(shaderRoot)))
    {
        Log::Error("Failed to initialize shader library.");
        return false;
    }

    if (!m_displacementVerticesPass.Initialize(m_graphicsDevice, m_shaderLibrary))
    {

        Log::Error("Failed to initialize displacement vertices pass.");
        return false;
    }

    if (!m_displacementIndicesPass.Initialize(m_graphicsDevice, m_shaderLibrary))
    {
        Log::Error("Failed to initialize displacement indices pass.");
        return false;
    }

    if (!m_deferredLightingPass.Initialize(device, m_shaderLibrary))
    {
        Log::Error("Failed to initialize deferred lighting pass.");

        return false;
    }

    if (!m_toneMapPass.Initialize(device, m_shaderLibrary))
    {
        Log::Error("Failed to initialize tone map pass.");

        return false;
    }

    if(!m_packagedRuntime && !m_editorColliderPass.Initialize(device, m_shaderLibrary))
    {
        Log::Error("Failed to initialize editor collider line pass.");
        return false;
    }
    if (!m_resourceUploader.Initialize(device))
    {
        Log::Error("Failed to init resource uploader.");
        return false;
    }

    if (!m_uploadBuffer.Initialize(device, 1024 * 1024, 2))
    {
        Log::Error("Failed to init upload buffer.");
        return false;
    }

    if (!m_resourceDescriptorHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, ResourceDescriptorCapacity, true))
    {
        Log::Error("Failed to init resource descriptor heap.");

        return false;
    }

    if (!m_rtvDescriptorHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 256, false))
    {
        Log::Error("Failed to init rtv descriptor heap.");

        return false;
    }

    if (!m_brdfIntegrationLUT.Initialize(device, 512, 512, 1, 1, DXGI_FORMAT_R16G16_FLOAT,
                                         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET))
    {
        Log::Error("Failed to init BRDF integration LUT.");
        return false;
    }

    if (!m_brdfIntegrationLUT.CreateSRV(device, m_resourceDescriptorHeap))
    {
        Log::Error("Failed to create BRDF integration LUT SRV.");
        return false;
    }

    if (!m_brdfIntegrationPass.Initialize(device, m_shaderLibrary, m_rtvDescriptorHeap, m_brdfIntegrationLUT))
    {
        Log::Error("Failed to init BRDF integration pass.");

        return false;
    }

    if (!m_ggxAverageAlbedoLUT.Initialize(device, 512, 1, 1, 1, DXGI_FORMAT_R16_FLOAT,
                                          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET))
    {
        Log::Error("Failed to init GGX average albedo LUT.");

        return false;
    }

    if (!m_ggxAverageAlbedoLUT.CreateSRV(device, m_resourceDescriptorHeap))
    {
        Log::Error("Failed to create GGX average albedo LUT SRV.");

        return false;
    }

    if (!m_ggxAverageAlbedoPass.Initialize(device, m_shaderLibrary, m_rtvDescriptorHeap, m_ggxAverageAlbedoLUT))
    {
        Log::Error("Failed to init GGX average albedo pass.");

        return false;
    }

    if (!m_rayTracedShadowPass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_sceneWidth,
                                          m_sceneHeight))
    {
        Log::Error("Faield to init ray traced shadow pass.");
        return false;
    }

    if (!m_rayTracedReflectionPass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_sceneWidth,
                                              m_sceneHeight))
    {
        Log::Error("Failed to init ray traced reflection pass.");
        return false;
    }

    const uint32_t giWidth = (m_sceneWidth + 1u) / 2u;

    const uint32_t giHeight = (m_sceneHeight + 1u) / 2u;

    const uint32_t aoWidth = (m_sceneWidth + 1u) / 2u;
    const uint32_t aoHeight = (m_sceneHeight + 1u) / 2u;

    if (!m_rayTracedGIPass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, giWidth, giHeight))
    {
        Log::Error("Failed to init ray traced gi pass.");
        return false;
    }

    if (!m_rayTracedAOPass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, aoWidth, aoHeight))
    {
        Log::Error("Failed to init ray traced AO pass.");

        return false;
    }

    if (!m_aoTemporalPass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, aoWidth, aoHeight))
    {
        Log::Error("Failed to init AO temporal pass.");

        return false;
    }

    if (!m_aoDenoisePass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, aoWidth, aoHeight))
    {
        Log::Error("Failed to init ray traced ao denoise pass.");

        return false;
    }

    if (!m_aoResolvePass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_sceneWidth, m_sceneHeight))
    {
        Log::Error("Failed to initialize AO resolve pass.");
        return false;
    }

    if (!m_rayTracedReflectionTemporalPass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap,
                                                      m_sceneWidth, m_sceneHeight))
    {
        Log::Error("Failed to init ray traced reflection temporal pass.");

        return false;
    }

    if (!m_rayTracedReflectionDenoisePass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap,
                                                     m_sceneWidth, m_sceneHeight))
    {
        Log::Error("Failed to init ray traced reflection denoise pass.");
        return false;
    }

    if (!m_reflectionContributionLimitPass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap,
                                                      m_sceneWidth, m_sceneHeight))
    {
        Log::Error("Failed to initialize reflection contribution limiter.");
        return false;
    }

    if (!m_nrdReflectionPreparePass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_sceneWidth,
                                               m_sceneHeight))
    {
        Log::Error("Failed to initialize NRD reflection preparation pass.");
        return false;
    }

    if (!m_nrdGIPreparePass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, giWidth, giHeight,
                                       NrdPreparePass::Signal::Diffuse))
    {
        Log::Error("Failed to initialize NRD GI preparation pass.");

        return false;
    }

    if (!m_nrdReflectionResolvePass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_sceneWidth,
                                               m_sceneHeight))
    {
        Log::Error("Failed to initialize NRD reflection resolve pass.");
        return false;
    }

    if (!m_nrdDiffuseResolvePass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_sceneWidth,
                                            m_sceneHeight))
    {
        Log::Error("Failed to initialize NRD diffuse resolve pass.");
        return false;
    }

    if (!m_giSpatialDenoisePass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_sceneWidth,
                                           m_sceneHeight))
    {
        Log::Error("Failed to init the ray traced GI spatial denoise pass.");

        return false;
    }

    if (!m_nrdReflectionDenoiser.Initialize(device, m_commandContext.GetCommandQueue(), m_resourceDescriptorHeap,
                                            m_sceneWidth, m_sceneHeight, 2, NrdDenoiser::Signal::Specular))
    {
        Log::Error("Failed to init NRD reflection denoiser.");

        return false;
    }

    if (!m_nrdGIDenoiser.Initialize(device, m_commandContext.GetCommandQueue(), m_resourceDescriptorHeap, giWidth,
                                    giHeight, 2, NrdDenoiser::Signal::Diffuse))
    {
        Log::Error("Failed to init NRD diffuse GI denoiser.");

        return false;
    }

    Log::Info("NRD REBLUR initialized.");
    Log::Info("NRD persistent memory: " + std::to_string(m_nrdReflectionDenoiser.GetPersistentMemoryUsageInMb()) +
              " MiB");

    Log::Info("NRD transient memory: " + std::to_string(m_nrdReflectionDenoiser.GetAliasableMemoryUsageInMb()) +

              " MiB");
    Log::Info("NRD total memory: " + std::to_string(m_nrdReflectionDenoiser.GetTotalMemoryUsageInMb()) + " MiB");

    if (!m_shadowDenoisePass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_sceneWidth, m_sceneHeight))
    {
        Log::Error("Failed to init shadow denoise pass.");

        return false;
    }

    if(m_packagedRuntime) m_availableEnvironments = {{initialEnvironment?"Packaged scene HDRI":"Embedded neutral environment", {}}};
    if (m_availableEnvironments.empty())
    {
        Log::Error("No HDR environments were discovered.");
        return false;
    }

    m_currentEnvironmentIndex = 0;

    const EnvironmentAsset& defaultEnvironment = m_availableEnvironments[m_currentEnvironmentIndex];

    const std::filesystem::path environmentPath = m_packagedRuntime ? std::filesystem::path{} : m_assetRoot / defaultEnvironment.relativePath;

    m_environmentIdentity = m_packagedRuntime ? (environmentIdentity.empty()?"neutral":environmentIdentity) : "bootstrap:"+defaultEnvironment.displayName;
    if (!(initialEnvironment ? m_environment.Initialize(device,*initialEnvironment,m_resourceUploader,m_resourceDescriptorHeap) :
        m_environment.Initialize(device, environmentPath, m_resourceUploader, m_resourceDescriptorHeap)))
    {
        Log::Error("Failed to initialize environment: " + defaultEnvironment.displayName);

        m_environment.Shutdown();
        return false;
    }

    Log::Info("Initial environment: " + defaultEnvironment.displayName);

    if (!BeginTrackedRecording(0))
    {
        Log::Error("Failed to begin environment-independent LUT generation.");

        return false;
    }

    ID3D12GraphicsCommandList* lutCommandList = m_commandContext.GetCommandList();

    ID3D12DescriptorHeap* descriptorHeaps[] = {m_resourceDescriptorHeap.GetHeap()};

    lutCommandList->SetDescriptorHeaps(1, descriptorHeaps);

    m_brdfIntegrationPass.Execute(lutCommandList, m_brdfIntegrationLUT);

    m_ggxAverageAlbedoPass.Execute(lutCommandList, m_ggxAverageAlbedoLUT);

    if (!EndTrackedRecording())
    {
        Log::Error("Failed to end environment-independent LUT generation.");
        return false;
    }

    const bool lutSubmitted = ExecuteTrackedRecording(0);

    // Execute can fail to signal after submitting work; still attempt to drain it.

    if (!m_commandContext.WaitForGPU())
    {
        Log::Error("Failed to confirm GPU completion after LUT generation.");
        return false;
    }

    if (!lutSubmitted)
    {
        Log::Error("Failed to execute or signal environment-independent LUT generation.");
        return false;
    }

    if (!PreprocessEnvironment(m_environment))
    {
        Log::Error("Failed to preprocess environment.");
        return false;
    }

    if (!m_sceneColor.Initialize(device, m_sceneWidth, m_sceneHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, m_rtvDescriptorHeap,
                                 m_resourceDescriptorHeap))
    {
        Log::Error("Failed to init HDR scene color");
        return false;
    }

    if(!m_finalSceneColor.Initialize(device, m_sceneWidth, m_sceneHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
                                    m_rtvDescriptorHeap, m_resourceDescriptorHeap))
    {
        Log::Error("Failed to initialize final scene color.");
        return false;
    }

    if (!m_dsvDescriptorHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 16, false))
    {
        Log::Error("Failed to init dsv descriptor heap.");

        return false;
    }

    if (!m_packagedRuntime && !m_debugUI.Initialize(static_cast<HWND>(windowHandle), device, m_commandContext.GetCommandQueue(),
                              m_resourceDescriptorHeap))
    {
        Log::Error("Failed to init debug UI.");
        return false;
    }

    m_debugUIInitialized = !m_packagedRuntime;

    if (!m_gBuffer.Initialize(device, m_sceneWidth, m_sceneHeight, m_rtvDescriptorHeap, m_dsvDescriptorHeap,
                              m_resourceDescriptorHeap))
    {
        Log::Error("Failed to init G-Buffer.");
        return false;
    }

    if (!m_selectionMask.Initialize(device, m_sceneWidth, m_sceneHeight, DXGI_FORMAT_R8_UNORM, m_rtvDescriptorHeap,
                                    m_resourceDescriptorHeap))
    {
        Log::Error("Failed to init selection mask.");
        return false;
    }

    if (!InitializeSwapChain(windowHandle))
    {
        return false;
    }

    if(!m_packagedRuntime && !m_materialPreviewRenderer.Initialize(
            device,
            m_shaderLibrary,
            m_resourceUploader,
            m_rtvDescriptorHeap,
            m_dsvDescriptorHeap,
            m_resourceDescriptorHeap))
    {
        Log::Error("Failed to initialize material preview resources.");
        return false;
    }

    if(!m_packagedRuntime) m_textureThumbnails.Configure(device, m_resourceDescriptorHeap);

    m_resizeFailed = false;
    return true;
}

bool Renderer::InitializeDevice()
{
    RecordingEligibilityScope recordingEligibility{*this};

    if (!m_graphicsDevice.Initialize())
    {
        Log::Error("Failed to init graphics device.");
        return false;
    }

    if (!m_commandContext.Initialize(m_graphicsDevice.GetDevice()))
    {
        Log::Error("Failed to init command context");
        return false;
    }
    m_commandContextInitialized=true;

    return true;
}

bool Renderer::InitializeSwapChain(void* windowHandle)
{
    return m_swapChain.Initialize(m_graphicsDevice.GetFactory(), m_graphicsDevice.GetDevice(),
                                  m_commandContext.GetCommandQueue(), windowHandle, m_applicationWidth, m_applicationHeight);
}

// Resize and geometry drawing
bool Renderer::Resize(
    unsigned int width,
    unsigned int height)
{
    m_gpuTimestampProfiler.BreakFrameSequence();
    ResetCpuSubmissionCadence();

    if(m_resizeFailed)
    {
        return false;
    }

    if(width == 0 || height == 0)
    {
        return true;
    }

    if(!m_commandContext.WaitForGPU())
    {
        m_resizeFailed = true;
        Log::Error("Failed to confirm GPU completion before resize.");
        return false;
    }

    // In-place recreation is not transactional. Keep rendering disabled until both stages succeed.
    m_resizeFailed = true;

    if(!ResizeApplicationResources(width, height))
    {
        return false;
    }

    m_resizeFailed = false;
    return true;
}

bool Renderer::ResizeScene(unsigned int width, unsigned int height)
{
    if(m_resizeFailed || m_activeRecordingGeneration != 0 || m_displacementExitScopeGeneration != 0)
    {
        return false;
    }

    if(width == 0 || height == 0 || (width == m_sceneWidth && height == m_sceneHeight))
    {
        return true;
    }

    m_gpuTimestampProfiler.BreakFrameSequence();
    ResetCpuSubmissionCadence();
    m_resizeFailed = true;

    if(!m_commandContext.WaitForGPU())
    {
        Log::Error("Failed to confirm GPU completion before scene resize.");
        return false;
    }

    if(!ResizeSceneResources(width, height))
    {
        return false;
    }

    m_resizeFailed = false;
    return true;
}

bool Renderer::ResizeApplicationResources(
    unsigned int width,
    unsigned int height)
{
    // ResizeBuffers or view creation can fail after destroying the previous backbuffers.
    m_applicationWidth = 0;
    m_applicationHeight = 0;

    if(!m_swapChain.Resize(width, height))
    {
        Log::Error("Failed to resize swap chain.");
        return false;
    }

    m_applicationWidth = width;
    m_applicationHeight = height;
    return true;
}

bool Renderer::ResizeSceneResources(
    unsigned int width,
    unsigned int height)
{
    // No coherent scene extent is published while resources are partially recreated.
    m_sceneWidth = 0;
    m_sceneHeight = 0;

    if(!m_gBuffer.Resize(m_graphicsDevice.GetDevice(), width, height, m_rtvDescriptorHeap, m_dsvDescriptorHeap,
                          m_resourceDescriptorHeap))
    {
        Log::Error("Failed to resize G-Buffer.");

        return false;
    }

    if(!m_rayTracedShadowPass.Resize(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize ray traced shadow pass.");

        return false;
    }

    if(!m_rayTracedReflectionPass.Resize(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize ray traced reflection pass.");

        return false;
    }

    const uint32_t giWidth = (width + 1u) / 2u;
    const uint32_t giHeight = (height + 1u) / 2u;

    const uint32_t aoWidth = (width + 1u) / 2u;
    const uint32_t aoHeight = (height + 1u) / 2u;

    if(m_giSignedCorrectionInitialized &&

        !m_giSignedCorrectionPass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, width, height))
    {
        return false;
    }

    if(!m_rayTracedGIPass.Resize(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, giWidth, giHeight))
    {
        Log::Error("Failed to resize ray traced GI pass.");
        return false;
    }

    if(!m_rayTracedAOPass.Resize(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, aoWidth, aoHeight))
    {
        Log::Error("Failed to resize ray traced AO pass.");
        return false;
    }

    if(!m_aoTemporalPass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, aoWidth, aoHeight))
    {
        Log::Error("Failed to resize AO temporal pass.");
        return false;
    }

    if(!m_aoDenoisePass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, aoWidth, aoHeight))
    {
        Log::Error("Failed to resize AO denoise pass.");
        return false;
    }

    if(!m_aoResolvePass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize AO resolve pass.");
        return false;
    }

    if(!m_rayTracedReflectionTemporalPass.Resize(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, width,
                                                  height))
    {
        Log::Error("Failed to resize ray traced reflection temporal pass.");

        return false;
    }

    if(!m_reflectionContributionLimitPass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize reflection contribution limiter.");
        return false;
    }

    if(!m_nrdReflectionPreparePass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize NRD reflection preparation pass.");
        return false;
    }

    if(!m_nrdGIPreparePass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, giWidth, giHeight))
    {
        Log::Error("Failed to resize NRD GI preparation pass.");
        return false;
    }

    if(!m_nrdReflectionResolvePass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize NRD reflection resolve pass.");
        return false;
    }

    if(!m_nrdDiffuseResolvePass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize NRD diffuse resolve pass.");
        return false;
    }

    if(!m_giSpatialDenoisePass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize gi spatial denoise pass.");
        return false;
    }

    if(!m_nrdReflectionDenoiser.Resize(m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize NRD reflection denoiser.");
        return false;
    }

    if(!m_nrdGIDenoiser.Resize(m_resourceDescriptorHeap, giWidth, giHeight))
    {

        Log::Error("Failed to resize NRD diffuse GI denoiser.");
        return false;
    }

    m_hasPreviousNrdCamera = false;

    if(!m_rayTracedReflectionDenoisePass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize ray traced reflection denoise pass.");
        return false;
    }

    if(!m_shadowDenoisePass.Resize(m_graphicsDevice, m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize shadow denoise pass.");
        return false;
    }

    m_sceneColor.Shutdown();

    if(!m_sceneColor.Initialize(m_graphicsDevice.GetDevice(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                 m_rtvDescriptorHeap, m_resourceDescriptorHeap))
    {
        Log::Error("Failed to resize HDR scene color.");

        return false;
    }

    m_selectionMask.Shutdown();

    if(!m_selectionMask.Initialize(m_graphicsDevice.GetDevice(), width, height, DXGI_FORMAT_R8_UNORM,
                                    m_rtvDescriptorHeap, m_resourceDescriptorHeap))
    {

        Log::Error("Failed to resize selection mask.");
        return false;
    }

    m_finalSceneColor.Shutdown();

    if(!m_finalSceneColor.Initialize(m_graphicsDevice.GetDevice(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                                    m_rtvDescriptorHeap, m_resourceDescriptorHeap))
    {
        Log::Error("Failed to resize final scene color.");
        return false;
    }

    m_sceneWidth = width;
    m_sceneHeight = height;
    return true;
}

bool Renderer::TryResolveDrawGeometryBinding(

    ID3D12GraphicsCommandList* commandList,
    const Mesh& mesh,
    DrawGeometryBinding& binding) const
{
    DrawGeometryBinding resolved{};
    const auto& pending = m_pendingDisplacementUpdate;

    if (kEnablePendingDisplacementDrawBindings && pending.mesh == &mesh)
    {
        if (!commandList || commandList != m_commandContext.GetCommandList() || m_activeRecordingGeneration == 0 ||

            m_activeRecordingSlot >= kDisplacementLODFrameCount)
        {
            return false;
        }

        switch (pending.status)
        {
        case DisplacementUpdateStatus::Recording:

            if (!TryGetRecordingDisplacementBinding(&mesh, m_activeRecordingSlot, resolved.displacement))
            {
                return false;
            }

            resolved.source = DrawGeometrySource::RecordingDisplacement;
            break;

        case DisplacementUpdateStatus::Submitted:

            if (!TryGetSubmittedDisplacementBinding(&mesh, resolved.displacement) ||

                (m_displacementCoverage.enabled && !ValidateCoveredDisplacementRecording()))
            {
                return false;
            }

            resolved.source = DrawGeometrySource::SubmittedDisplacement;
            break;

        default:

            return false; // Unsubmitted, uncertain or finalizing versions cannot be drawn.
        }

        // Both provenance helpers construct matching views and counts from the
        // same immutable target. Publication to Mesh remains completion-only.

        resolved.vertexCount = resolved.displacement.vertexCount;
        resolved.indexCount = resolved.displacement.indexCount;
        resolved.indexed = true;
    }
    else
    {
        resolved.publishedMesh = &mesh;

        resolved.vertexCount = mesh.GetVertexCount();
        resolved.indexCount = mesh.GetIndexCount();
        resolved.indexed = mesh.IsIndexed();
    }

    binding = resolved;

    return true;
}

void Renderer::BindDrawGeometry(
    ID3D12GraphicsCommandList* commandList,
    const DrawGeometryBinding& binding) const
{
    if (binding.source == DrawGeometrySource::PublishedMesh)
    {
        binding.publishedMesh->Bind(commandList);
        return;
    }

    // Pending versions use validated views, including the target index size.

    commandList->IASetVertexBuffers(0, 1, &binding.displacement.vertexView);

    if (binding.indexed)
    {
        commandList->IASetIndexBuffer(&binding.displacement.indexView);
    }
}

void Renderer::Draw(ID3D12GraphicsCommandList* commandList, const RenderObject& object,
    D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress)
{
    if(!object.mesh)
    {
        return;
    }

    const auto& metadata = object.mesh->GetMetadata();

    for(const auto& submesh : metadata.submeshes)
    {
        const auto& slot = metadata.materialSlots[submesh.materialSlot];
        Material* material = object.GetSlotMaterial(slot.id);
        // Single-range procedural geometry may have provisional GPU counts this
        // frame. Preserve the existing geometry-binding count in that case.
        const auto count = metadata.submeshes.size() == 1 ? UINT_MAX : submesh.indexCount;
        DrawMaterialRange(commandList, object, *material, submesh.firstIndex, count,
            cameraConstantsAddress);
    }
}

void Renderer::DrawMaterialRange(
    ID3D12GraphicsCommandList* commandList,
    const RenderObject& object,
    Material& selectedMaterial, unsigned int firstIndex, unsigned int rangeCount,
    D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress)
{
    if (!object.mesh || !selectedMaterial.pipeline || !selectedMaterial.texture)
    {
        return;
    }

    const PresentationTransform presentation = object.GetPresentationTransform();
    ObjectConstants objectConstants{};
    objectConstants.model = presentation.currentModel;

    objectConstants.previousModel = presentation.previousModel;

    objectConstants.normalMatrix = objectConstants.model.Inverse().Transpose();

    D3D12_GPU_VIRTUAL_ADDRESS objectConstantsAddress = m_uploadBuffer.Allocate(
        &objectConstants, sizeof(ObjectConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if (objectConstantsAddress == 0)
    {
        Log::Error("Failed to allocate object constants.");
        return;
    }

    selectedMaterial.pipeline->Bind(commandList);

    const auto materialIndex = selectedMaterial.gpuIndex;

    if(materialIndex >= m_materialPublication.desired.size())
    {
        Log::Error("Render object references an invalid material-table index.");
        return;
    }

    // Legacy raster root constants must agree with the table used by deferred/DXR passes.
    // Snapshot changes are rejected during recording, so this is the recorded revision.
    const auto& material = m_materialPublication.desired[materialIndex];
    MaterialConstants materialConstants{};
    materialConstants.textureIndex = material.textureIndex;
    materialConstants.roughness = material.roughness;
    materialConstants.materialIndex = materialIndex;
    materialConstants.materialBufferIndex = CurrentMaterialBuffer().GetDescriptorIndex();

    materialConstants.roughnessTextureIndex = material.roughnessTextureIndex;
    std::copy(std::begin(material.baseColor), std::end(material.baseColor), materialConstants.baseColor);

    commandList->SetGraphicsRoot32BitConstants(0, 8, &materialConstants, 0);

    const unsigned int displacementLODDescriptorIndex =
        m_displacementLODDescriptorIndices[m_swapChain.GetCurrentBackBufferIndex()];

    commandList->SetGraphicsRoot32BitConstants(0, 1, &displacementLODDescriptorIndex, 8);

    commandList->SetGraphicsRootConstantBufferView(1, cameraConstantsAddress);

    commandList->SetGraphicsRootConstantBufferView(2, objectConstantsAddress);

    DrawGeometryBinding geometry{};

    if (!TryResolveDrawGeometryBinding(commandList, *object.mesh, geometry))
    {
        if (kEnableFrameIntegratedDisplacement && m_displacementCoverage.enabled)
        {
            m_displacementCoverage.failed = true;
        }

        return;
    }

    const auto availableCount = geometry.indexed ? geometry.indexCount : geometry.vertexCount;

    if(rangeCount == UINT_MAX)
    {
        rangeCount = static_cast<unsigned int>(availableCount);
    }

    if(firstIndex > availableCount || rangeCount > availableCount - firstIndex)
    {
        Log::Error("Submesh draw range exceeds the active geometry binding.");
        return;
    }

    BindDrawGeometry(commandList, geometry);

    selectedMaterial.texture->Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |

                                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (selectedMaterial.normalTexture)
    {
        selectedMaterial.normalTexture->Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |

                                                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    if (selectedMaterial.roughnessTexture)
    {
        selectedMaterial.roughnessTexture->Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |

                                                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    if (selectedMaterial.metallicTexture)
    {
        selectedMaterial.metallicTexture->Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    if (selectedMaterial.armTexture)
    {
        selectedMaterial.armTexture->Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |

                                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    if (selectedMaterial.aoTexture)
    {
        selectedMaterial.aoTexture->Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    if(selectedMaterial.heightTexture)
    {
        selectedMaterial.heightTexture->Transition(commandList,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    if (geometry.indexed)
    {
        const uint64_t indexCount = static_cast<uint64_t>(rangeCount);

        commandList->DrawIndexedInstanced(rangeCount, 1, firstIndex, 0, 0);

        ++m_currentFrameStatistics.geometryDrawCalls;
        m_currentFrameStatistics.submittedTriangles += indexCount / 3;
    }
    else
    {
        const uint64_t vertexCount = static_cast<uint64_t>(rangeCount);

        commandList->DrawInstanced(rangeCount, 1, firstIndex, 0);

        ++m_currentFrameStatistics.geometryDrawCalls;
        m_currentFrameStatistics.submittedTriangles += vertexCount / 3;
    }
}

void Renderer::DrawSelectionMask(
    ID3D12GraphicsCommandList* commandList,
    const RenderObject& object,
    D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress)
{
    if (!object.mesh)
    {
        return;
    }

    const PresentationTransform presentation = object.GetPresentationTransform();
    ObjectConstants objectConstants{};
    objectConstants.model = presentation.currentModel;

    objectConstants.previousModel = presentation.previousModel;
    objectConstants.normalMatrix = objectConstants.model.Inverse().Transpose();

    const D3D12_GPU_VIRTUAL_ADDRESS objectConstantsAddress = m_uploadBuffer.Allocate(

        &objectConstants, sizeof(ObjectConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if (objectConstantsAddress == 0)
    {
        Log::Error("Failed to allocate selection mask object constants.");
        return;
    }

    m_selectionMaskPass.Bind(commandList);

    commandList->SetGraphicsRootConstantBufferView(0, cameraConstantsAddress);
    commandList->SetGraphicsRootConstantBufferView(1, objectConstantsAddress);

    DrawGeometryBinding geometry{};

    if (!TryResolveDrawGeometryBinding(commandList, *object.mesh, geometry))
    {
        if (kEnableFrameIntegratedDisplacement && m_displacementCoverage.enabled)
        {
            m_displacementCoverage.failed = true;
        }

        return;
    }

    BindDrawGeometry(commandList, geometry);

    if (geometry.indexed)
    {
        commandList->DrawIndexedInstanced(static_cast<UINT>(geometry.indexCount), 1, 0, 0, 0);
    }
    else
    {
        commandList->DrawInstanced(static_cast<UINT>(geometry.vertexCount), 1, 0, 0);
    }
}

// --------------
// Geometry pass
// --------------
void Renderer::BeginGeometryPass(ID3D12GraphicsCommandList* commandList)
{
    RenderTarget& albedo = m_gBuffer.GetMaterial();
    RenderTarget& normal = m_gBuffer.GetSurface();
    RenderTarget& motion = m_gBuffer.GetMotion();

    RenderTarget& geometricNormal = m_gBuffer.GetGeometricNormal();

    DepthBuffer& depth = m_gBuffer.GetDepth();

    albedo.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    normal.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);

    motion.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);

    geometricNormal.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    depth.Transition(commandList, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    const float albedoClearColor[] = {1.0f, 0.0f, 1.0f, 1.0f};

    const float normalClearColor[] = {0.0f, 1.0f, 0.0f, 1.0f};

    const float motionClearColor[] = {0.0f, 0.0f, 0.0f, 0.0f};

    const float geometricNormalClearColor[] = {0.0f, 1.0f, 0.0f, 0.0f};

    D3D12_CPU_DESCRIPTOR_HANDLE renderTargets[] = {albedo.GetRTV(), normal.GetRTV(), motion.GetRTV(),
                                                   geometricNormal.GetRTV()};

    commandList->ClearRenderTargetView(renderTargets[0], albedoClearColor, 0, nullptr);
    commandList->ClearRenderTargetView(renderTargets[1], normalClearColor, 0, nullptr);

    commandList->ClearRenderTargetView(renderTargets[2], motionClearColor, 0, nullptr);
    commandList->ClearRenderTargetView(renderTargets[3], geometricNormalClearColor, 0, nullptr);

    commandList->ClearDepthStencilView(depth.GetDSV(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_CPU_DESCRIPTOR_HANDLE dsv = depth.GetDSV();

    commandList->OMSetRenderTargets(4, renderTargets, FALSE, &dsv);
}

void Renderer::EndGeometryPass(ID3D12GraphicsCommandList* commandList)
{
    constexpr D3D12_RESOURCE_STATES shaderReadState =

        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    m_gBuffer.GetMaterial().Transition(commandList, shaderReadState);

    m_gBuffer.GetSurface().Transition(commandList, shaderReadState);
    m_gBuffer.GetMotion().Transition(commandList, shaderReadState);
    m_gBuffer.GetGeometricNormal().Transition(commandList, shaderReadState);

    m_gBuffer.GetDepth().Transition(commandList, shaderReadState);
}

// Scene resources and history

bool Renderer::InitializeSceneResources(
    const Scene& scene,
    const std::vector<Material*>& materials)
{
    if (scene.HasPhysicsPresentation())
    {
        Log::Error("Clear physics presentation before initializing another renderer scene lifetime.");
        return false;
    }
    if (m_sceneInitializationAttempt.candidate || m_sceneResourceScene || m_activeRecordingGeneration != 0 ||
        m_displacementExitScopeGeneration != 0)
    {
        Log::Error("Scene initialization requires checked release and no unresolved initialization or recording.");
        return false;
    }

    if (m_heightmapRegistration.owner)
    {
        Log::Error("Cannot replace registered scene resources before checked release.");

        return false;
    }

    if (HasUnresolvedDisplacementUpdate())
    {
        Log::Error("Cannot initialize scene resources with an unresolved displacement update.");

        return false;
    }

    const std::uint64_t lifetime = AllocateRendererIdentity();

    if (lifetime == 0)
    {
        Log::Error("Scene-resource lifetime identifiers exhausted.");
        return false;
    }

    m_sceneInitializationAttempt = {};
    m_sceneInitializationAttempt.candidate = &scene;

    const auto initialize = [&]() -> bool
    {
        // A failed replacement has no valid published lifetime identity.
        m_sceneResourceLifetime = 0;
        m_sceneResourceScene = nullptr;

        if (!InitializeMaterials(materials))
        {
            Log::Error("Failed to init scene materials.");
            return false;
        }

        if(!ValidateSceneMaterialSlots(scene, materials))
        {
            return false;
        }

        if (!InitializeRayTracingScene(scene))
        {
            Log::Error("Failed to init ray tracing scene.");
            // Retain resources until checked failed-attempt cleanup proves safety.
            return false;
        }

        m_selectionMaskCandidate.reset();
        m_selectionMaskCandidateLifetime = 0;
        m_selectionMaskPass.Shutdown();

        const RenderObject* firstRenderableObject = nullptr;

        for (const RenderObject& object : scene.GetRenderObjects())
        {
            if (object.mesh)
            {
                firstRenderableObject = &object;
                break;
            }
        }

        if (firstRenderableObject)
        {
            if (!m_selectionMaskPass.Initialize(m_graphicsDevice.GetDevice(), m_shaderLibrary,
                                                firstRenderableObject->mesh->GetVertexLayout()))
            {
                Log::Error("Failed to init selection mask pass.");
                return false;
            }
        }

        ResetSceneHistory();
        m_sceneResourceLifetime = lifetime;
        m_sceneResourceScene = &scene;

        return true;
    };

    bool initialized = false;

    try
    {
        initialized = initialize();
    }
    catch (...)
    {
        m_sceneInitializationAttempt.failed = true;
        throw;
    }

    if (initialized)
    {
        m_sceneInitializationAttempt = {};
    }
    else
    {
        m_sceneInitializationAttempt.failed = true;
    }

    return initialized;
}

bool Renderer::InitializeMaterials(const std::vector<Material*>& materials)
{
    if (materials.empty())
    {
        Log::Error("Cannot init an empty material buffer.");
        return false;
    }

    if (materials.size() > UINT_MAX)
    {
        Log::Error("Material count exceeds the supported 32-bit limit.");
        return false;
    }

    std::vector<GPUMaterial> gpuMaterials;
    gpuMaterials.reserve(materials.size());

    m_publishedMaterialIdentities.assign(materials.begin(), materials.end());

    for (size_t i = 0; i < materials.size(); ++i)
    {
        if (!materials[i])
        {
            Log::Error("Cannot init null material.");
            return false;
        }

        materials[i]->gpuIndex = static_cast<unsigned int>(i);
        gpuMaterials.push_back(materials[i]->GetGPUData());
    }

    std::vector<Texture::Dependency> dependencies;

    for(auto* material : materials)
    {
        Texture* textures[] = {material->texture, material->normalTexture, material->roughnessTexture,
            material->metallicTexture, material->armTexture, material->aoTexture, material->heightTexture};

        for(auto* texture : textures)
        {
            if(!texture)
            {
                continue;
            }

            auto retained = texture->Retain();

            if(!retained)
            {
                Log::Error("Cannot retain a material texture allocation and descriptor.");
                return false;
            }

            dependencies.push_back(std::move(retained));
        }
    }

    if(!m_materialDependencies.SetDesired(std::move(dependencies)))
    {
        Log::Error("Material texture dependency capacity exceeded.");
        return false;
    }

    m_materialPublication = {};

    if(!m_materialPublication.SetSnapshot(gpuMaterials))
    {
        Log::Error("Failed to prepare frame-slot material uploads.");
        return false;
    }

    for(auto& buffer : m_materialBuffers)
    {
        if(!buffer.InitializeStorage(m_graphicsDevice.GetDevice(), sizeof(GPUMaterial),
            static_cast<unsigned int>(gpuMaterials.size()), m_resourceDescriptorHeap))
        {
            Log::Error("Failed to allocate frame-slot material table.");
            return false; // Retained until checked failed-initialization cleanup.
        }
    }

    for(auto& upload : m_materialUpload)
        if(!upload.Initialize(m_graphicsDevice.GetDevice(), gpuMaterials.size() * sizeof(GPUMaterial), 1))
            return false;

    return true;
}

bool Renderer::InitializeRayTracingScene(const Scene& scene)
{
    SceneSwitchTiming::Measure asTiming(SceneSwitchTiming::Stage::AccelerationStructures);
    // No AS or instance buffer is needed when every object is intentionally meshless.
    // Scene-resource release still owns draining and destruction when switching scenes.
    if(!scene.HasRenderableMeshes())
    {
        return true;
    }

    RecordingEligibilityScope recordingEligibility{*this};

    if (!m_graphicsDevice.SupportsRayTracing())
    {
        Log::Error("Cannot initialize ray tracing scene: ray tracing is not supported.");

        return false;
    }

    auto& attempt = m_sceneInitializationAttempt;
    attempt.commands = SceneInitializationCommands::BeginAttempted;

    if (!BeginTrackedRecording(0))
    {
        Log::Error("Failed to begin acceleration structure construction.");

        return false;
    }

    attempt.commands = SceneInitializationCommands::Recording;
    attempt.recordingGeneration = m_activeRecordingGeneration;

    if (!m_rayTracingScene.Initialize(m_graphicsDevice.GetRayTracingDevice(),
                                      m_commandContext.GetRayTracingCommandList(), m_resourceUploader,
                                      m_resourceDescriptorHeap, scene))
    {
        Log::Error("Failed to init ray tracing scene resources.");
        return false;
    }

    attempt.commands = SceneInitializationCommands::Recorded;
    attempt.commands = SceneInitializationCommands::CloseAttempted;

    if (!EndTrackedRecording())
    {
        return false;
    }

    attempt.commands = SceneInitializationCommands::Closed;
    attempt.commands = SceneInitializationCommands::SubmissionAttempted;
    const bool buildSubmitted = ExecuteTrackedRecording(0);

    if (buildSubmitted)
    {
        attempt.commands = SceneInitializationCommands::Submitted;
    }

    // A failed submission fence signal does not mean the build was discarded.
    const bool completed = SceneSwitchTiming::MeasureCall(SceneSwitchTiming::Stage::ASWait, [&]()
    {
        return m_commandContext.WaitForGPU();
    });

    if (!completed)
    {
        Log::Error("Failed to confirm acceleration structure build completion.");

        return false;
    }

    attempt.gpuCompletionProven = true;
    attempt.commands = SceneInitializationCommands::Complete;

    if (!buildSubmitted)
    {
        Log::Error("Failed to execute or signal acceleration structure construction.");
        return false;
    }

    return true;
}

bool Renderer::CleanupFailedSceneInitialization(const Scene& candidate)
{
    auto& attempt = m_sceneInitializationAttempt;

    if (attempt.candidate != &candidate || !attempt.failed || m_sceneResourceScene || m_sceneResourceLifetime != 0)
    {
        return false;
    }

    if (HasUnresolvedDisplacementUpdate() || m_heightmapRegistration.owner ||
        m_completedDisplacementUpdate.requestRevision != 0 || m_displacementExitScopeGeneration != 0)
    {
        return false;
    }

    if (m_activeRecordingGeneration != 0 &&
        (m_activeRecordingGeneration != attempt.recordingGeneration || m_activeRecordingSlot != 0))
    {
        return false;
    }

    // Also handles Begin/Close failures and ended-but-unsubmitted lists. A
    // successful drain alone does not prevent those old commands executing later.
    if (!m_commandContext.DiscardCommandsAfterGPUDrain())
    {
        return false;
    }

    attempt.gpuCompletionProven = true;
    attempt.commands = SceneInitializationCommands::Abandoned;
    if(!ResolvePreviewAfterGPUDrain())
    {
        return false;
    }

    InvalidateRecordingEligibility();

    m_rayTracingScene.Shutdown();
    ShutdownMaterialTables();
    m_selectionMaskCandidate.reset();
    m_selectionMaskCandidateLifetime = 0;
    m_selectionMaskPass.Shutdown();
    ResetSceneHistory();

    m_sceneInitializationAttempt = {};
    return true;
}

bool Renderer::ReleaseSceneResources()
{
    if (m_sceneResourceScene && m_sceneResourceScene->HasPhysicsPresentation())
    {
        Log::Error("Clear physics presentation at the idle boundary before releasing scene resources.");
        return false;
    }
    if (m_sceneInitializationAttempt.candidate)
    {
        Log::Error("Partial scene initialization requires checked failed-attempt cleanup.");
        return false;
    }

    if (m_completedDisplacementUpdate.requestRevision != 0)
    {
        Log::Error("Cannot release scene resources before delivering displacement completion.");
        return false;
    }

    if (HasUnresolvedDisplacementUpdate())
    {
        Log::Error("Cannot release scene resources with an unresolved displacement update.");
        return false;
    }

    if (m_heightmapRegistration.owner && !ValidateDisplacementHeightmapRegistration())
    {
        Log::Error("Cannot release scene resources with invalid heightmap ownership.");
        return false;
    }

    if(!m_commandContext.DiscardCommandsAfterGPUDrain())
    {
        Log::Error("Scene resource release could not prove material dependency retirement safety.");
        return false;
    }

    if(!ResolvePreviewAfterGPUDrain())
    {
        return false;
    }

    InvalidateRecordingEligibility();
    m_sceneResourceLifetime = 0;
    m_sceneResourceScene = nullptr;
    m_rayTracingScene.Shutdown();
    ShutdownMaterialTables();

    m_selectionMaskCandidate.reset();
    m_selectionMaskCandidateLifetime = 0;
    m_selectionMaskPass.Shutdown();

    ResetSceneHistory();

    if (m_heightmapRegistration.owner)
    {
        m_heightmapRegistration.owner->m_heightmapRegistrationOwner = nullptr;

        m_heightmapRegistration.owner->m_heightmapRegistrationLifetime = 0;

        m_heightmapRegistration = {};
    }

    return true;
}

void Renderer::ResetSceneHistory()
{
    m_gpuTimestampProfiler.BreakFrameSequence();
    ResetCpuSubmissionCadence();
    m_giSignedCorrectionPass.ResetHistory();

    m_signedCorrectionExecutedLastFrame = false;
    m_rayTracedShadowPass.ResetHistory();

    m_rayTracedReflectionTemporalPass.ResetHistory();

    m_aoTemporalPass.ResetHistory();

    m_hasPreviousViewProjection = false;
    m_hasPreviousShadowLightDirection = false;
    m_hasPreviousGILight = false;
    m_hasPreviousNrdCamera = false;

    m_nrdExecutedLastFrame = false;
    m_nativeReflectionExecutedLastFrame = false;
    m_aoExecutedLastFrame = false;

    m_shadowSampleIndex = 0;

    m_reflectionSampleIndex = 0;
    m_nrdFrameIndex = 0;
}

// --------------
// Rendering
// --------------

const char* Renderer::GetDisplacementDiagnosticPhase() const
{
    switch (m_pendingDisplacementUpdate.status)
    {
    case DisplacementUpdateStatus::Empty:

        return m_completedDisplacementUpdate.requestRevision != 0 ? "finalized-awaiting-delivery" : "idle";

    case DisplacementUpdateStatus::Prepared:
        return "prepared";

    case DisplacementUpdateStatus::Recording:

        return "recording";
    case DisplacementUpdateStatus::Submitting:
        return "submitting";
    case DisplacementUpdateStatus::Submitted:
        return "submitted";
    case DisplacementUpdateStatus::Finalizing:
        return "finalizing";
    case DisplacementUpdateStatus::Completed:
        return "completed";

    case DisplacementUpdateStatus::Discarded:
        return "discarded";

    case DisplacementUpdateStatus::FailedUncertain:
        return "uncertain";
    }

    return "invalid";
}

Renderer::FrameHitchProbe::FrameHitchProbe(Renderer& renderer, FrameHitchRegion region, bool measure)
{
    if constexpr (kEnableFrameHitchDiagnostics)
    {
        auto& span = renderer.m_frameHitchDiagnostics.current.regions[static_cast<size_t>(region)];

        if (!span)
        {
            span = 0.0;
        }

        // A reached but suppressed phase remains a measured zero, not n/a.

        if (measure)
        {
            m_total = &*span;
            m_start = CpuTiming::Clock::now();
        }
    }
}

Renderer::FrameHitchProbe::~FrameHitchProbe()
{
    if (m_total)
    {
        *m_total += CpuTiming::MillisecondsSince(m_start);
    }
}

void Renderer::RecordFrameHitchRegion(
    FrameHitchRegion region,
    double milliseconds,
    std::uint64_t cameraRevision)
{
    if constexpr (kEnableFrameHitchDiagnostics)
    {
        auto& span = m_frameHitchDiagnostics.current.regions[static_cast<size_t>(region)];
        span = span.value_or(0.0) + milliseconds;
        m_frameHitchDiagnostics.current.cameraLoggingRevision = cameraRevision;
    }
}

void Renderer::BeginFrameHitchDiagnostics(std::uint64_t engineFrameSerial)
{
    if constexpr (kEnableFrameHitchDiagnostics)
    {
        m_commandContext.EnableExecuteCpuTiming(true);
        auto& snapshot = m_frameHitchDiagnostics.current;
        snapshot = {};
        snapshot.engineSerial = engineFrameSerial;
        snapshot.revision = m_pendingDisplacementUpdate.requestRevision != 0

                                ? m_pendingDisplacementUpdate.requestRevision
                                : m_completedDisplacementUpdate.requestRevision;
        snapshot.phaseStart = GetDisplacementDiagnosticPhase();
    }
}

void Renderer::RecordFrameHitchDiagnostics(
    CpuTiming::Clock::time_point frameStart,
    double frameMilliseconds,
    std::optional<FrameRenderResult> outcome)
{
    if constexpr (!kEnableFrameHitchDiagnostics)
    {
        return;
    }

    const auto now = CpuTiming::Clock::now();
    auto& diagnostics = m_frameHitchDiagnostics;

    if (diagnostics.lastReport == CpuTiming::Clock::time_point{})
    {
        diagnostics.lastReport = now;
    }

    auto& snapshot = diagnostics.current;
    snapshot.outcome = outcome;
    snapshot.execution = frameMilliseconds;
    snapshot.phaseEnd = GetDisplacementDiagnosticPhase();

    if (outcome == FrameRenderResult::Rendered)
    {
        snapshot.render = m_cpuFrameTiming; // This Frame() completed this Render.

        if (diagnostics.previousRenderedStart != CpuTiming::Clock::time_point{})
        {
            snapshot.cadence =
                std::chrono::duration<double, std::milli>(frameStart - diagnostics.previousRenderedStart).count();
        }
    }

    diagnostics.CommitFrame(now, frameStart, frameMilliseconds, outcome, m_cpuFrameTiming,
                            kCpuHitchThresholdMilliseconds);
}

void Renderer::ReportFrameHitchDiagnostics(CpuTiming::Clock::time_point now) const
{
    m_frameHitchDiagnostics.Report(now, kCpuHitchThresholdMilliseconds);
}

void Renderer::LogDisplacementWallClockSummary(
    std::uint64_t revision,
    size_t laterSubmissions)
{
    FrameHitchProbe loggingProbe(
        *this,
        FrameHitchRegion::CompletionPhaseLogging,
        !kSuppressDisplacementLifecycleLoggingForDiagnostics);

    if constexpr (kSuppressDisplacementLifecycleLoggingForDiagnostics)
    {
        return;
    }

    const auto& timing = m_displacementWallClock;

    const auto milliseconds = [](CpuTiming::Clock::time_point start, CpuTiming::Clock::time_point end)
    { return std::chrono::duration<double, std::milli>(end - start).count(); };
    const auto sinceAcceptance = [&](CpuTiming::Clock::time_point timestamp)
    { return milliseconds(timing.accepted, timestamp); };

    const double deferredMilliseconds = timing.firstDeferred == CpuTiming::Clock::time_point{}
                                            ? 0.0

                                            : milliseconds(
                                                timing.firstDeferred,
                                                timing.finalized);

    // Offsets share the acceptance origin (0 ms). Submission is CPU call return;
    // completion is first CPU observation. Neither is a GPU execution timestamp.
    Log::Info("Async displacement CPU wall-clock ms, revision " + std::to_string(revision) +
              ": accepted=0, recordStart=" + std::to_string(sinceAcceptance(timing.recordingStart)) +
              ", recordEnd=" + std::to_string(sinceAcceptance(timing.recordingEnd)) +
              ", submitReturn=" + std::to_string(sinceAcceptance(timing.submissionReturned)) + ", originalObserved=" +
              std::to_string(sinceAcceptance(timing.originalCompletionObserved)) + ", firstDeferred=" +
              (timing.firstDeferred == CpuTiming::Clock::time_point{}
                   ? std::string("none")
                   : std::to_string(sinceAcceptance(timing.firstDeferred))) +
              ", finalized=" + std::to_string(sinceAcceptance(timing.finalized)) + "; recordPreparationAndCompute=" +
              std::to_string(milliseconds(timing.recordingStart, timing.computeEnd)) +
              ", recordBLAS_TLAS=" + std::to_string(milliseconds(timing.computeEnd, timing.recordingEnd)) +

              "; deferredInterval=" + std::to_string(deferredMilliseconds) + ", deferredIterations=" +
              std::to_string(m_displacementDeferredFrames) + ", laterSubmissions=" + std::to_string(laterSubmissions) +

              "; slotAcquisitionTotal=" + std::to_string(timing.acquisitionMilliseconds) +

              ", slotFenceWaitTotal=" + std::to_string(timing.fenceWaitMilliseconds) +

              ", slotFenceWaitMax=" + std::to_string(timing.maxFenceWaitMilliseconds) +
              ", acquisitionCalls=" + std::to_string(timing.acquiredFrames) +
              ", blockingSlotWaits=" + std::to_string(timing.blockingSlotWaits) + ".");
}

void Renderer::LogAsyncDisplacementError()
{
    if (kEnableAsyncCameraDisplacementUpdates && !m_displacementTerminalErrorLogged)
    {
        m_displacementTerminalErrorLogged = true;
        Log::Error("Async displacement terminal error; ownership remains fail-closed.");
    }
}

Renderer::DisplacementCompletionStatus Renderer::ProcessDisplacementCompletion()
{
    std::optional<HitchTimingScope> diagnosticCompletion;

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        m_frameHitchDiagnostics.current.completionCalled = true;
        diagnosticCompletion.emplace(m_frameHitchDiagnostics.current.completionMilliseconds);
    }

    auto& pending = m_pendingDisplacementUpdate;
    auto& coverage = m_displacementCoverage;
    const auto fail = [&]()
    {
        m_geometryUpdateFailed = true;
        coverage.failed = true;
        LogAsyncDisplacementError();
        return DisplacementCompletionStatus::Error;
    };

    if (m_activeRecordingGeneration != 0 || m_displacementExitScopeGeneration != 0)
    {
        return fail(); // Never finalize or close admission during a live frame.
    }

    if (pending.status == DisplacementUpdateStatus::Empty)
    {
        if (HasUnresolvedDisplacementUpdate() || m_geometryUpdateFailed)
        {
            return fail();
        }

        return m_completedDisplacementUpdate.requestRevision != 0 ? DisplacementCompletionStatus::Finalized
                                                                  : DisplacementCompletionStatus::NoPendingUpdate;
    }

    if (m_completedDisplacementUpdate.requestRevision != 0)
    {
        return fail(); // Never overwrite an undelivered completion payload.
    }

    if (pending.status == DisplacementUpdateStatus::Prepared)
    {
        if (!ValidatePendingDisplacementReservation() || !ValidateDisplacementHeightmapRegistration())
        {
            return fail();
        }

        return DisplacementCompletionStatus::AwaitingSubmission;
    }

    if (!ValidatePendingDisplacementCoverage() || !ValidateDisplacementHeightmapRegistration() ||
        coverage.current.outcome != DisplacementRecordingOutcome::Submitted ||
        !ValidatePendingDisplacementRetirementCoverage())
    {
        return fail(); // Active, ended, discarded and uncertain frames need exit resolution first.
    }

    // Freeze at the bounded frontier even if the original fence is still
    // pending. Existing submissions can complete without adding another frame.

    if (coverage.submittedCount == coverage.submitted.size())
    {
        coverage.admissionClosed = true;
    }

    const auto observation = ObservePendingDisplacementCompletion();

    if (observation == DisplacementCompletionObservation::Pending)
    {
        return DisplacementCompletionStatus::OriginalSubmissionPending;
    }

    if (observation != DisplacementCompletionObservation::Complete)
    {
        return fail();
    }

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        m_frameHitchDiagnostics.current.revision = pending.requestRevision;
        m_frameHitchDiagnostics.current.completed = true;
    }

    if (!m_displacementCompletionObserved)
    {
        m_displacementWallClock.originalCompletionObserved = CpuTiming::Clock::now();
        m_displacementCompletionObserved = true;
        {
            FrameHitchProbe loggingProbe(
                *this,
                FrameHitchRegion::CompletionPhaseLogging,
                !kSuppressDisplacementLifecycleLoggingForDiagnostics);

            if constexpr (!kSuppressDisplacementLifecycleLoggingForDiagnostics)
            {
                Log::Info("Async displacement original fence completed for revision " +

                          std::to_string(pending.requestRevision) + ".");
            }
        }
    }

    coverage.admissionClosed = true;
    // Close admission while validating/finalizing, not until later fences drain.
    // Invalid or uncertain coverage remains terminal rather than admitting work.
    const size_t laterSubmissions = coverage.submittedCount - 1;
    DisplacementCompletionResult completed{};

    if (!FinalizeCoveredDisplacementUpdate(completed))
    {
        return fail(); // Cleanup succeeded; a remaining finalizer failure is terminal.
    }

    m_displacementWallClock.finalized = CpuTiming::Clock::now();

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        const auto& timing = m_displacementWallClock;
        auto& diagnostics = m_frameHitchDiagnostics;

        diagnostics.current.finalized = true;

        diagnostics.AddCompletedDisplacementSample(
            timing.diagnosticComputeMilliseconds, timing.diagnosticBLASMilliseconds, timing.diagnosticTLASMilliseconds,
            std::chrono::duration<double, std::milli>(timing.originalCompletionObserved - timing.submissionReturned)
                .count(),
            std::chrono::duration<double, std::milli>(timing.finalized - timing.originalCompletionObserved).count());
    }

    completed.acceptedAt = m_displacementWallClock.accepted;
    m_completedDisplacementUpdate = completed;

    LogDisplacementWallClockSummary(completed.requestRevision, laterSubmissions);
    return DisplacementCompletionStatus::Finalized;
}

bool Renderer::TakeDisplacementCompletion(
    MirrorTestScene& owner,
    DisplacementCompletionResult& result)
{
    if (m_activeRecordingGeneration != 0 ||
        m_displacementExitScopeGeneration != 0 ||
        m_completedDisplacementUpdate.requestRevision == 0 ||
        m_heightmapRegistration.owner != &owner)
    {
        return false;
    }

    if (!ValidateDisplacementHeightmapRegistration() ||
        m_heightmapRegistration.useUnresolved ||
        HasUnresolvedDisplacementUpdate() ||
        m_geometryUpdateFailed)
    {
        return false;
    }

    if (m_completedDisplacementUpdate.scene != &owner.m_scene ||
        m_completedDisplacementUpdate.sceneResourceLifetime != m_sceneResourceLifetime ||
        m_completedDisplacementUpdate.sceneRevision != owner.m_scene.GetStructureRevision())
    {
        return false;
    }

    result = m_completedDisplacementUpdate;

    m_completedDisplacementUpdate = {};
    return true;
}

Renderer::FrameRenderResult Renderer::Render(
    const Scene& scene,
    const RendererOptions& options)
{
    PollPreparedTextureUploads();
    m_textureThumbnails.SetCompletedFence(m_commandContext.GetCompletedFenceValue());

    if(m_materialPreviewRenderer.IsQuarantined())
    {
        return FrameRenderResult::Error;
    }
    // Retain the latest snapshot even if scene admission defers this frame.
    m_materialPreviewRenderer.SetRequest(options.materialPreview);

    if(m_resizeFailed || m_applicationWidth == 0 || m_applicationHeight == 0)
    {
        return FrameRenderResult::Error;
    }

    if(!m_rayTracingScene.ValidateMaterialBindings(scene))
    {
        Log::Error("Scene material-slot bindings changed without a checked scene-resource rebuild.");
        return FrameRenderResult::Error;
    }

    if (m_sceneInitializationAttempt.candidate)
    {
        return FrameRenderResult::Error;
    }

    std::optional<FrameHitchProbe> admissionProbe;

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        admissionProbe.emplace(*this, FrameHitchRegion::Admission);
    }

    if (kEnableFrameIntegratedDisplacement)
    {
        const auto completion = ProcessDisplacementCompletion();

        if (completion == DisplacementCompletionStatus::Error)
        {
            return FrameRenderResult::Error;
        }

        if (completion == DisplacementCompletionStatus::LaterCoveragePending ||
            (completion == DisplacementCompletionStatus::OriginalSubmissionPending &&
             m_displacementCoverage.admissionClosed))
        {
            // No Begin/reset, upload writes, submission or Present on deferral.
            m_hasPreviousRenderExecute = false;
            m_cpuFrameTiming = {};

            if (m_displacementDeferredFrames != UINT64_MAX)
            {
                ++m_displacementDeferredFrames;
            }

            if (m_displacementDeferredFrames == 1)
            {
                m_displacementWallClock.firstDeferred = CpuTiming::Clock::now();

                Log::Info("Async displacement frame admission deferred until covered submissions complete.");
            }

            return FrameRenderResult::Deferred;
        }
    }

    admissionProbe.reset();

    if (!RecordRenderFrame(scene, options))
    {
        LogAsyncDisplacementError();
        return FrameRenderResult::Error;
    }

    return FrameRenderResult::Rendered;
}

CameraConstants Renderer::BuildFrameCameraConstants(
    const Matrix4& currentView,
    const Matrix4& currentViewProjection,
    const Matrix4& previousView,
    const Matrix4& previousViewProjection,
    bool hasPreviousViewProjection)
{
    CameraConstants cameraConstants{};
    cameraConstants.viewProjection = currentViewProjection;
    cameraConstants.previousViewProjection =
        hasPreviousViewProjection ? previousViewProjection : currentViewProjection;
    cameraConstants.view = currentView;
    cameraConstants.previousView = hasPreviousViewProjection ? previousView : currentView;
    return cameraConstants;
}

void Renderer::RecordSceneSelectionMask(
    ID3D12GraphicsCommandList* commandList,
    const Scene& scene,
    const RendererOptions& options,
    D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress)
{
    // BeginTrackedRecording acquired this slot; End has not invalidated it yet.
    const unsigned int frameSlot = m_activeRecordingSlot;

    m_gpuTimestampProfiler.BeginRegion(commandList, frameSlot, GpuTimestampProfiler::Region::SelectionMask);

    m_selectionMask.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);

    const float maskClearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
    commandList->ClearRenderTargetView(m_selectionMask.GetRTV(), maskClearColor, 0, nullptr);

    const auto& renderObjects = scene.GetRenderObjects();

    const size_t selectionCount = options.selectedObjects.empty() ? 1 : options.selectedObjects.size();
    for (size_t selection = 0; selection < selectionCount; ++selection)
    {
        const auto index = options.selectedObjects.empty() ? options.selectedObject : options.selectedObjects[selection];
        if (index >= renderObjects.size())
        {
            continue;
        }
        const RenderObject& selectedObject = renderObjects[index];

        if (selectedObject.mesh)
        {
            DepthBuffer& depth = m_gBuffer.GetDepth();

            depth.Transition(commandList, D3D12_RESOURCE_STATE_DEPTH_READ);

            const D3D12_CPU_DESCRIPTOR_HANDLE maskRTV = m_selectionMask.GetRTV();
            const D3D12_CPU_DESCRIPTOR_HANDLE depthDSV = depth.GetReadOnlyDSV();

            commandList->OMSetRenderTargets(1, &maskRTV, FALSE, &depthDSV);

            DrawSelectionMask(commandList, selectedObject, cameraConstantsAddress);
        }
    }

    m_selectionMask.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    m_gpuTimestampProfiler.EndRegion(commandList, frameSlot, GpuTimestampProfiler::Region::SelectionMask);
}

bool Renderer::RecordAmbientOcclusionPasses(
    ID3D12GraphicsCommandList* commandList,
    unsigned int frameSlot,
    const AOConstants& constants,
    const ShadowConstants& shadowConstants,
    unsigned int width,
    unsigned int height)
{
    // Preserve upload failure before any AO history reset or radius mutation.
    const D3D12_GPU_VIRTUAL_ADDRESS aoConstantsAddress =
        m_uploadBuffer.Allocate(&constants, sizeof(AOConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if (aoConstantsAddress == 0)
    {
        Log::Error("Failed to allocate ray traced AO constants.");
        return false;
    }

    if (!m_aoExecutedLastFrame || m_previousAORadius != constants.maxDistance)
    {
        m_aoTemporalPass.ResetHistory();
    }

    m_previousAORadius = constants.maxDistance;

    m_gpuTimestampProfiler.BeginRegion(commandList, frameSlot, GpuTimestampProfiler::Region::RayTracedAO);
    m_rayTracedAOPass.Execute(m_commandContext.GetRayTracingCommandList(), m_resourceDescriptorHeap,
                              m_rayTracingScene, aoConstantsAddress, width, height);
    m_gpuTimestampProfiler.EndRegion(commandList, frameSlot, GpuTimestampProfiler::Region::RayTracedAO);

    m_gpuTimestampProfiler.BeginRegion(commandList, frameSlot, GpuTimestampProfiler::Region::AOTemporal);
    m_aoTemporalPass.Execute(commandList, m_rayTracedAOPass.GetRawAOTexture(), m_gBuffer, m_uploadBuffer,
                             constants.inverseViewProjection, shadowConstants.previousViewProjection,
                             shadowConstants.previousInverseViewProjection, width, height);
    m_gpuTimestampProfiler.EndRegion(commandList, frameSlot, GpuTimestampProfiler::Region::AOTemporal);

    m_gpuTimestampProfiler.BeginRegion(commandList, frameSlot, GpuTimestampProfiler::Region::AODenoise);
    m_aoDenoisePass.Execute(commandList, m_aoTemporalPass.GetAccumulatedAOTexture(), m_gBuffer, m_uploadBuffer,
                            constants.inverseViewProjection, width, height);
    m_gpuTimestampProfiler.EndRegion(commandList, frameSlot, GpuTimestampProfiler::Region::AODenoise);

    m_aoResolvePass.Execute(commandList, m_aoDenoisePass.GetFilteredAOTexture(), m_gBuffer.GetDepth(),
                            m_gBuffer.GetSurface(), m_uploadBuffer, m_sceneWidth, m_sceneHeight);

    return true;
}

bool Renderer::RecordRenderFrame(
    const Scene& scene,
    const RendererOptions& options)
{
    FinalSceneImage finalImage{};

    if (!TryGetFinalSceneImage(finalImage) || !m_swapChain.GetCurrentBackBuffer())
    {
        Log::Error("Scene presentation requires a valid final texture and application backbuffer.");
        return false;
    }

    std::optional<FrameHitchProbe> validationProbe;

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        validationProbe.emplace(*this, FrameHitchRegion::InitialValidation);
    }

    // Eligibility is scoped to recording; invalidation does not discard commands or release owners.
    RecordingEligibilityScope recordingEligibility{*this};
    const bool preparedDisplacement =
        kEnableFrameIntegratedDisplacement && m_pendingDisplacementUpdate.status == DisplacementUpdateStatus::Prepared;
    const bool submittedDisplacement =
        kEnableFrameIntegratedDisplacement && m_pendingDisplacementUpdate.status == DisplacementUpdateStatus::Submitted;
    const bool displacementFrame = preparedDisplacement || submittedDisplacement;

    if (displacementFrame && (!ValidatePendingDisplacementIdentity() || m_pendingDisplacementUpdate.scene != &scene ||
                              !ValidateDisplacementHeightmapRegistration() || m_heightmapRegistration.useUnresolved ||
                              (preparedDisplacement && !ValidatePendingDisplacementReservation()) ||

                              (submittedDisplacement &&
                               (!ValidatePendingDisplacementCoverage() ||
                                ObservePendingDisplacementCompletion() == DisplacementCompletionObservation::Error))))
    {
        Log::Error("Invalid frame-integrated displacement ownership.");
        return false;
    }

    if (HasUnresolvedDisplacementUpdate() && !displacementFrame)
    {
        Log::Error("Rendering disabled while a displacement update remains unresolved.");
        return false;
    }

    if (m_geometryUpdateFailed && !submittedDisplacement)
    {
        Log::Error("Rendering disabled: a geometry update failed and "
                   "ray tracing scene consistency cannot be guaranteed.");

        return false;
    }

    validationProbe.reset();
    std::optional<FrameHitchProbe> preBeginProbe;

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        preBeginProbe.emplace(*this, FrameHitchRegion::PreBegin);
    }

    // Preserve cadence and feature setup before acquiring the selected allocator/upload slot.
    const bool hasPreviousExecute = m_hasPreviousRenderExecute;

    // Any early return breaks the sequence; only a completed frame re-arms it.
    m_hasPreviousRenderExecute = false;
    m_cpuFrameTiming = {};

    m_cpuFrameTiming.frameSerial = m_gpuProfilerFrameSerial;
    CpuTiming::ScopedDuration renderDuration(m_cpuFrameTiming.render);

    const unsigned int frameIndex = m_swapChain.GetCurrentBackBufferIndex();

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        m_frameHitchDiagnostics.current.slot = frameIndex;
    }

    const ToneMapDebugView debugView = m_debugUI.GetToneMapDebugView();

    // An aborted/skipped frame must not leave CPU history state pointing at unsubmitted writes.
    struct SignedHistoryFrameGuard
    {
        GISignedCorrectionPass& pass;

        bool completed = false;

        ~SignedHistoryFrameGuard()
        {
            if (!completed)
            {
                pass.ResetHistory();
            }
        }
    } signedHistoryFrame{m_giSignedCorrectionPass};

    const bool hasRenderableMeshes = scene.HasRenderableMeshes();

    // Compile-time mode selection is shared with the shader; diagnostics never consume stale corrections.
#if GI_DIAGNOSTIC_MODE == GI_MODE_BASELINE_CORRECTION

    const bool runSignedCorrection = hasRenderableMeshes && options.enableRTGI;
#else

    const bool runSignedCorrection = false;
#endif

    if (runSignedCorrection && !m_giSignedCorrectionInitialized)
    {
        if (!m_giSignedCorrectionPass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_sceneWidth,
                                                 m_sceneHeight))
        {
            return false;
        }

        m_giSignedCorrectionInitialized = true;
    }

    if (!runSignedCorrection)
    {
        m_giSignedCorrectionPass.ResetHistory();
    }

    const bool runContactAO = options.enableRTGI && options.rtgiContactAOStrength > 0.0f;

    const float aoRadius =
        runContactAO && debugView != ToneMapDebugView::HDRIDiffuseAmbientOnly ? options.rtgiContactAORadius : 1.5f;

    const bool runAO = hasRenderableMeshes && (!options.enableRTGI || runContactAO || debugView == ToneMapDebugView::AORaw ||
                       debugView == ToneMapDebugView::AOTemporal || debugView == ToneMapDebugView::AOFiltered ||

                       debugView == ToneMapDebugView::HDRIDiffuseAmbientOnly);

    preBeginProbe.reset();
    const auto beginStart = CpuTiming::Clock::now();
    // Begin performs the existing slot-fence wait/reset before any per-slot upload writes.
    const bool beganRecording = BeginTrackedRecording(frameIndex);
    m_cpuFrameTiming.begin = CpuTiming::MillisecondsSince(beginStart);
    m_cpuFrameTiming.fenceWaited = m_commandContext.DidBeginWaitForFence();

    m_cpuFrameTiming.fenceWait = m_commandContext.GetBeginFenceWaitMilliseconds();
    m_cpuFrameTiming.commandReset = m_commandContext.GetBeginResetMilliseconds();

    std::optional<FrameHitchProbe> postBeginProbe;

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        postBeginProbe.emplace(*this, FrameHitchRegion::PostBegin);
    }

    auto& displacementTiming = m_displacementWallClock;

    if (displacementTiming.accepted != CpuTiming::Clock::time_point{} &&
        displacementTiming.finalized == CpuTiming::Clock::time_point{})
    {
        ++displacementTiming.acquiredFrames;

        displacementTiming.acquisitionMilliseconds += m_cpuFrameTiming.begin;
        displacementTiming.fenceWaitMilliseconds += m_cpuFrameTiming.fenceWait;
        displacementTiming.maxFenceWaitMilliseconds =

            (std::max)(displacementTiming.maxFenceWaitMilliseconds, m_cpuFrameTiming.fenceWait);

        if (m_cpuFrameTiming.fenceWaited)
        {
            ++displacementTiming.blockingSlotWaits;
        }
    }

    if (!beganRecording)
    {
        Log::Error("failed to begin command recording.");
        return false;
    }

    if (preparedDisplacement && !ActivatePreparedDisplacementRequest(frameIndex))
    {
        // No displacement commands or AS mutations have been recorded yet.
        // Preserve the prepared request/reservation even after checked abort.

        if (!AbortTrackedRecording())
        {
            m_geometryUpdateFailed = true;
        }

        return false;
    }

    // Recovery must cover every later exit, including execution and Present failure.
    std::optional<DisplacementFrameExitScope> displacementExit;

    if (displacementFrame)
    {
        displacementExit.emplace(*this);

        if (!displacementExit->armed)
        {
            m_geometryUpdateFailed = true;
            m_displacementCoverage.failed = true;
            return false;
        }

        if (submittedDisplacement && !ReserveLaterDisplacementSubmission())
        {
            return false;
        }
    }

    postBeginProbe.reset();
    const auto recordingStart = CpuTiming::Clock::now();

    std::optional<FrameHitchProbe> profilerProbe;

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        profilerProbe.emplace(*this, FrameHitchRegion::Profiler);
    }

    if (m_gpuTimestampProfiler.CollectCompleted(m_commandContext.GetCompletedFenceValue()))
    {
        const GpuTimestampProfiler::Results& gpuResults = m_gpuTimestampProfiler.GetResults();

        m_debugUI.RecordFramePacingGaps(gpuResults);

        for (unsigned int i = 0; i < GpuTimestampProfiler::FrameCount; ++i)
        {
            PendingRendererStatistics& pending = m_pendingStatistics[i];

            if (!pending.valid)
            {
                continue;
            }

            if (pending.statistics.frameSerial != gpuResults.frameSerial)
            {
                continue;
            }

            m_statistics = pending.statistics;
            m_statistics.gpuTimings = gpuResults;

            pending.valid = false;
            break;
        }
    }

    profilerProbe.reset();
    std::optional<FrameHitchProbe> recordingSetupProbe;

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        recordingSetupProbe.emplace(*this, FrameHitchRegion::RecordingSetup);
    }

    ID3D12GraphicsCommandList* commandList = m_commandContext.GetCommandList();
    // Keep generation/revision evidence through End, which invalidates active eligibility.
    const auto textureUploadGeneration = m_activeRecordingGeneration;
    RecordPreparedTextureUploads(commandList); // Copies/transitions precede every material consumer.
    const auto materialRevision = m_materialPublication.revision;

    if (!RecordMaterialTable(frameIndex, commandList))
    {
        Log::Error("Failed to record frame-slot material publication.");
        return false;
    }

    if (materialRevision != m_materialPublication.lastSubmitted)
    {
        ResetSceneHistory();
    }

    m_gpuTimestampProfiler.BeginFrame(commandList, frameIndex);

    m_currentFrameStatistics = {};
    m_currentFrameStatistics.frameSerial = m_gpuProfilerFrameSerial;
    m_currentFrameStatistics.renderObjectCount = static_cast<uint64_t>(scene.GetRenderObjects().size());
    m_currentFrameStatistics.tlasInstanceCount = static_cast<uint64_t>(m_rayTracingScene.GetInstanceCount());

    // Acquired-slot reset and diagnostic binding are also displacement eligibility evidence.
    m_uploadBuffer.Reset(frameIndex);
    ObserveFrameUploadReset(frameIndex);

    // Camera-selected displacement LODs.
    // Diagnostic only; does not modify geometry.
    const auto& diagnosticPatchLODs = scene.GetDisplacementPatchLODs();

    const D3D12_GPU_VIRTUAL_ADDRESS diagnosticLODAddress =

        m_uploadBuffer.Allocate(diagnosticPatchLODs.data(), sizeof(diagnosticPatchLODs), 256);

    if (diagnosticLODAddress == 0)
    {
        Log::Error("Failed to allocate displacement LOD diagnostic data.");
        return false;
    }

    unsigned int& diagnosticLODDescriptorIndex = m_displacementLODDescriptorIndices[frameIndex];

    const bool diagnosticDescriptorCreated = diagnosticLODDescriptorIndex == UINT_MAX;

    if (diagnosticLODDescriptorIndex == UINT_MAX)
    {
        if (!m_uploadBuffer.CreateRawSRV(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, diagnosticLODAddress,
                                         sizeof(diagnosticPatchLODs), diagnosticLODDescriptorIndex))
        {
            Log::Error("Failed to create displacement LOD diagnostic SRV.");
            return false;
        }
    }

    ObserveDiagnosticLODBinding(frameIndex, diagnosticLODAddress, sizeof(diagnosticPatchLODs),
                                diagnosticDescriptorCreated);

    recordingSetupProbe.reset();

    if (preparedDisplacement &&

        RecordPendingDisplacementForFrame(frameIndex) != FrameDisplacementRecordingResult::Recorded)
    {
        return false;
    }

    std::optional<FrameHitchProbe> ordinaryProbe;

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        ordinaryProbe.emplace(*this, FrameHitchRegion::OrdinaryPasses);
    }

    // Accepted authored/physics presentation updates TLAS; displacement may already have recorded it.
    const bool tlasNeedsUpdate = hasRenderableMeshes &&
        (!scene.GetChangedTransforms().empty() || !scene.GetChangedPresentationTransforms().empty());

    const bool displacementTLASRecorded = m_frameDisplacementPreparation.mergedTLASRecorded;

    m_debugUI.SetTLASUpdated(hasRenderableMeshes &&
        (displacementTLASRecorded || tlasNeedsUpdate || m_pendingGeometryTLASRefresh[frameIndex]));

    if (hasRenderableMeshes && !displacementTLASRecorded && (tlasNeedsUpdate || m_pendingGeometryTLASRefresh[frameIndex]))
    {
        const auto previousTLASRecording = m_rayTracingScene.GetTLASRecordingIdentity();

        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::TlasUpdate);
        const bool updated = m_rayTracingScene.Update(m_commandContext.GetRayTracingCommandList(), frameIndex, scene);
        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::TlasUpdate);

        if (!updated)
        {
            Log::Error("Failed to update ray tracing scene.");
            return false;
        }

        m_frameDisplacementPreparation.ordinaryTLASRecorded = true;
        m_pendingGeometryTLASRefresh[frameIndex] = false;

        if (submittedDisplacement &&
            !CapturePendingDisplacementTLASUse(frameIndex, true, updated, previousTLASRecording))
        {
            return false;
        }
    }
    else if (submittedDisplacement && !CapturePendingDisplacementTLASUse(frameIndex, false, false, 0))
    {
        return false;
    }

    if (displacementFrame)
    {
        DrawGeometryBinding binding{};

        if (!TryResolveDrawGeometryBinding(commandList, *m_pendingDisplacementUpdate.mesh, binding))
        {
            return false; // Never record dependent work with stale published views.
        }
    }

    // Assemble/upload constants in place; later camera/light/sample writes remain stateful.
    const Camera& camera = scene.GetCamera();
    const DirectionalLight& light = scene.GetDirectionalLight();

    const Matrix4 currentView = camera.GetViewMatrix();
    const Matrix4 currentProjection = camera.GetProjectionMatrix();
    const Matrix4 currentViewProjection = currentProjection * currentView;

    CameraConstants cameraConstants = BuildFrameCameraConstants(
        currentView, currentViewProjection, m_previousView, m_previousViewProjection, m_hasPreviousViewProjection);

    const Matrix4 previousNrdView = m_hasPreviousNrdCamera ? m_previousNrdView : currentView;

    const Matrix4 previousNrdProjection = m_hasPreviousNrdCamera ? m_previousNrdProjection : currentProjection;

    D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress = m_uploadBuffer.Allocate(
        &cameraConstants, sizeof(CameraConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if (cameraConstantsAddress == 0)
    {
        Log::Error("Failed to allocate camera constants.");
        return false;
    }

    ReconstructionConstants reconstructionConstants{};
    reconstructionConstants.inverseViewProjection = cameraConstants.viewProjection.Inverse();

    const Vector3& cameraPosition = camera.GetPosition();

    reconstructionConstants.cameraPosition[0] = cameraPosition.x;
    reconstructionConstants.cameraPosition[1] = cameraPosition.y;
    reconstructionConstants.cameraPosition[2] = cameraPosition.z;

    reconstructionConstants.geometricNormalIndex = m_gBuffer.GetGeometricNormal().GetSRVIndex();

    reconstructionConstants.motionIndex = m_gBuffer.GetMotion().GetSRVIndex();

    D3D12_GPU_VIRTUAL_ADDRESS reconstructionConstantsAddress = m_uploadBuffer.Allocate(
        &reconstructionConstants, sizeof(ReconstructionConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if (reconstructionConstantsAddress == 0)
    {
        Log::Error("Failed to allocate reconstruction constants.");
        return false;
    }

    DirectionalLightConstants lightConstants{};

    const Vector3 lightDirection = light.direction.Normalized();
    lightConstants.direction[0] = lightDirection.x;
    lightConstants.direction[1] = lightDirection.y;
    lightConstants.direction[2] = lightDirection.z;

    lightConstants.intensity = light.intensity;

    lightConstants.color[0] = light.color.x;
    lightConstants.color[1] = light.color.y;
    lightConstants.color[2] = light.color.z;
    lightConstants.ambientStrength = light.ambientStrength;

    D3D12_GPU_VIRTUAL_ADDRESS lightConstantsAddress = m_uploadBuffer.Allocate(
        &lightConstants, sizeof(DirectionalLightConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if (lightConstantsAddress == 0)
    {
        Log::Error("Failed to allocate directional light constants.");

        return false;
    }

    // These history resets/writes intentionally precede later uploads and recording failures.
    const Vector3 currentLightDirection = light.direction.Normalized();

    if (m_hasPreviousShadowLightDirection)
    {
        const float directionDifference = (currentLightDirection - m_previousShadowLightDirection).Length();

        if (directionDifference > 0.0001f)
        {
            m_rayTracedShadowPass.ResetHistory();
            m_giSignedCorrectionPass.ResetHistory();
        }
    }

    m_previousShadowLightDirection = currentLightDirection;

    m_hasPreviousShadowLightDirection = true;

    if (m_hasPreviousGILight)
    {
        const bool intensityChanged = light.intensity != m_previousGILightIntensity;

        const bool colorChanged = light.color.x != m_previousGILightColor.x ||
                                  light.color.y != m_previousGILightColor.y ||
                                  light.color.z != m_previousGILightColor.z;

        if (intensityChanged || colorChanged)
        {
            m_giSignedCorrectionPass.ResetHistory();
        }
    }

    m_previousGILightIntensity = light.intensity;
    m_previousGILightColor = light.color;
    m_hasPreviousGILight = true;

    ShadowConstants shadowConstants{};
    shadowConstants.inverseViewProjection = reconstructionConstants.inverseViewProjection;

    if (m_hasPreviousViewProjection)
    {
        shadowConstants.previousViewProjection = m_previousViewProjection;
        shadowConstants.previousInverseViewProjection = m_previousViewProjection.Inverse();
    }
    else
    {
        shadowConstants.previousViewProjection = cameraConstants.viewProjection;
        shadowConstants.previousInverseViewProjection = reconstructionConstants.inverseViewProjection;
    }

    shadowConstants.lightDirection[0] = lightDirection.x;
    shadowConstants.lightDirection[1] = lightDirection.y;
    shadowConstants.lightDirection[2] = lightDirection.z;
    shadowConstants.depthIndex = m_gBuffer.GetDepth().GetSRVIndex();

    shadowConstants.surfaceIndex = m_gBuffer.GetSurface().GetSRVIndex();

    shadowConstants.geometricNormalIndex = m_gBuffer.GetGeometricNormal().GetSRVIndex();
    shadowConstants.lightAngularRadius = light.angularRadius;

    shadowConstants.frameIndex = m_shadowSampleIndex++;
    shadowConstants.historyIndex = m_rayTracedShadowPass.GetVisibilityTexture().GetDescriptorIndex();
    shadowConstants.previousDepthIndex = m_rayTracedShadowPass.GetPreviousDepthIndex();
    shadowConstants.outputDepthIndex = m_rayTracedShadowPass.GetOutputDepthIndex();

    shadowConstants.previousHistoryLengthIndex = m_rayTracedShadowPass.GetPreviousHistoryLengthIndex();
    shadowConstants.outputHistoryLengthIndex = m_rayTracedShadowPass.GetOutputHistoryLengthIndex();
    shadowConstants.accumulatedFrameCount = m_rayTracedShadowPass.GetAccumulatedFrameCount();

    ReflectionConstants reflectionConstants{};

    reflectionConstants.inverseViewProjection = reconstructionConstants.inverseViewProjection;
    reflectionConstants.cameraPosition[0] = cameraPosition.x;
    reflectionConstants.cameraPosition[1] = cameraPosition.y;
    reflectionConstants.cameraPosition[2] = cameraPosition.z;

    reflectionConstants.depthIndex = m_gBuffer.GetDepth().GetSRVIndex();
    reflectionConstants.surfaceIndex = m_gBuffer.GetSurface().GetSRVIndex();
    reflectionConstants.geometricNormalIndex = m_gBuffer.GetGeometricNormal().GetSRVIndex();
    reflectionConstants.instanceDataIndex = m_rayTracingScene.GetInstanceDataDescriptorIndex();

    reflectionConstants.materialBufferIndex = CurrentMaterialBuffer().GetDescriptorIndex();
    reflectionConstants.environmentIndex = m_environment.GetSourceTexture().GetDescriptorIndex();
    reflectionConstants.brdfIntegrationLUTIndex = m_brdfIntegrationLUT.GetDescriptorIndex();

    reflectionConstants.ggxAverageAlbedoLUTIndex = m_ggxAverageAlbedoLUT.GetDescriptorIndex();

    reflectionConstants.frameIndex = m_reflectionSampleIndex++;
    reflectionConstants.environmentCubemapIndex = m_environment.GetCubemap().GetDescriptorIndex();

    reflectionConstants.diffuseIrradianceIndex = m_environment.GetDiffuseIrradiance().GetDescriptorIndex();
    reflectionConstants.ggxMultiscatterIrradianceIndex =

        m_environment.GetGGXMultiscatterIrradiance().GetDescriptorIndex();
    reflectionConstants.materialIndex = m_gBuffer.GetMaterial().GetSRVIndex();
    reflectionConstants.importanceConditionalCDFIndex =

        m_environment.GetImportanceConditionalCDF().GetDescriptorIndex();

    reflectionConstants.importanceMarginalCDFIndex = m_environment.GetImportanceMarginalCDF().GetDescriptorIndex();
    reflectionConstants.importanceWidth = m_environment.GetImportanceWidth();
    reflectionConstants.importanceHeight = m_environment.GetImportanceHeight();
    reflectionConstants.importanceTotalWeight = m_environment.GetImportanceTotalWeight();

    m_previousViewProjection = cameraConstants.viewProjection;
    m_previousView = cameraConstants.view;
    m_hasPreviousViewProjection = true;

    D3D12_GPU_VIRTUAL_ADDRESS shadowConstantsAddress = m_uploadBuffer.Allocate(
        &shadowConstants, sizeof(ShadowConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if (shadowConstantsAddress == 0)
    {
        Log::Error("Failed to allocate ray traced shadow constants.");
        return false;
    }

    D3D12_GPU_VIRTUAL_ADDRESS reflectionConstantsAddress = m_uploadBuffer.Allocate(
        &reflectionConstants, sizeof(ReflectionConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if (reflectionConstantsAddress == 0)
    {
        Log::Error("Failed to allocate ray traced reflection constants.");
        return false;
    }

    GIConstants giConstants{};
    giConstants.inverseViewProjection = reconstructionConstants.inverseViewProjection;
    giConstants.depthIndex = m_gBuffer.GetDepth().GetSRVIndex();
    giConstants.surfaceIndex = m_gBuffer.GetSurface().GetSRVIndex();

    giConstants.geometricNormalIndex = m_gBuffer.GetGeometricNormal().GetSRVIndex();
    giConstants.fullResolution[0] = m_sceneWidth;
    giConstants.fullResolution[1] = m_sceneHeight;
    giConstants.frameIndex = reflectionConstants.frameIndex;

    giConstants.instanceDataIndex = reflectionConstants.instanceDataIndex;
    giConstants.materialBufferIndex = reflectionConstants.materialBufferIndex;
    giConstants.environmentIndex = reflectionConstants.environmentIndex;
    giConstants.diffuseIrradianceIndex = m_environment.GetDiffuseIrradiance().GetDescriptorIndex();

    giConstants.diffuseIrradianceArrayIndex = m_environment.GetDiffuseIrradianceArrayIndex();

    if (runSignedCorrection)
    {
        giConstants.signedCorrectionIndex = m_giSignedCorrectionPass.GetSignedCorrectionTexture().GetUAVIndex();
    }

    const D3D12_GPU_VIRTUAL_ADDRESS giConstantsAddress =
        m_uploadBuffer.Allocate(&giConstants, sizeof(GIConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if (giConstantsAddress == 0)
    {
        Log::Error("Failed to allocate ray traced GI constants.");
        return false;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_swapChain.GetCurrentBackBuffer();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    commandList->ResourceBarrier(1, &barrier);

    ID3D12DescriptorHeap* descriptorHeaps[] = {m_resourceDescriptorHeap.GetHeap()};
    commandList->SetDescriptorHeaps(1, descriptorHeaps);

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_swapChain.GetCurrentRTV();

    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(m_sceneWidth);

    viewport.Height = static_cast<float>(m_sceneHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    commandList->RSSetViewports(1, &viewport);

    D3D12_RECT scissor{};
    scissor.left = 0;
    scissor.top = 0;
    scissor.right = static_cast<LONG>(m_sceneWidth);

    scissor.bottom = static_cast<LONG>(m_sceneHeight);

    commandList->RSSetScissorRects(1, &scissor);

    const float clearColor[] = {0.1f, 0.2f, 0.4f, 1.0f};

    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::Geometry);

    // Selection uses geometry depth before EndGeometryPass transitions the G-buffer for consumers.
    BeginGeometryPass(commandList);

    for (const RenderObject& object : scene.GetRenderObjects())
    {
        Draw(commandList, object, cameraConstantsAddress);
    }

    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::Geometry);
    RecordSceneSelectionMask(commandList, scene, options, cameraConstantsAddress);

    EndGeometryPass(commandList);

    constexpr float nrdDenoisingRange = 1000.0f;
    bool runNrd = false;
    bool runNative = false;
    const bool limiterEnabled = m_debugUI.IsReflectionContributionLimiterEnabled();

    Texture* debugTexture = nullptr;
    RenderTarget* debugRenderTarget = nullptr;

    // Preserve the fixed AO -> shadows -> reflections -> GI -> denoise -> lighting order.
    if (hasRenderableMeshes)
    {
        const uint32_t aoWidth = (m_sceneWidth + 1u) / 2u;
        const uint32_t aoHeight = (m_sceneHeight + 1u) / 2u;

        if (runAO)
        {
            AOConstants aoConstants{};

            aoConstants.inverseViewProjection = reconstructionConstants.inverseViewProjection;
            aoConstants.depthIndex = m_gBuffer.GetDepth().GetSRVIndex();
            aoConstants.surfaceIndex = m_gBuffer.GetSurface().GetSRVIndex();
            aoConstants.frameIndex = reflectionConstants.frameIndex;

            aoConstants.maxDistance = aoRadius;

            aoConstants.originBias = 0.01f;
            aoConstants.rayTMin = 0.001f;

            if (!RecordAmbientOcclusionPasses(commandList, frameIndex, aoConstants, shadowConstants, aoWidth, aoHeight))
            {
                return false;
            }
        }

        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::RayTracedShadows);

        m_rayTracedShadowPass.Execute(m_commandContext.GetRayTracingCommandList(), m_resourceDescriptorHeap,
                                      m_rayTracingScene, shadowConstantsAddress, m_sceneWidth, m_sceneHeight);

        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::RayTracedShadows);

        constexpr D3D12_RESOURCE_STATES shaderReadState =

            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        m_environment.GetSourceTexture().Transition(commandList, shaderReadState);

        m_environment.GetCubemap().Transition(commandList, shaderReadState);
        m_environment.GetDiffuseIrradiance().Transition(commandList, shaderReadState);
        m_environment.GetGGXMultiscatterIrradiance().Transition(commandList, shaderReadState);
        m_brdfIntegrationLUT.Transition(commandList, shaderReadState);
        m_ggxAverageAlbedoLUT.Transition(commandList, shaderReadState);

        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::RayTracedReflections);

        m_rayTracedReflectionPass.Execute(m_commandContext.GetRayTracingCommandList(), m_resourceDescriptorHeap,
                                          m_rayTracingScene, reflectionConstantsAddress, lightConstantsAddress, m_sceneWidth,
                                          m_sceneHeight);

        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::RayTracedReflections);

        const uint32_t giWidth = (m_sceneWidth + 1u) / 2u;
        const uint32_t giHeight = (m_sceneHeight + 1u) / 2u;

        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::RayTracedGI);

        if (runSignedCorrection)
        {
            m_giSignedCorrectionPass.GetSignedCorrectionTexture().Transition(commandList,
                                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        m_rayTracedGIPass.Execute(m_commandContext.GetRayTracingCommandList(), m_resourceDescriptorHeap, m_rayTracingScene,
                                  giConstantsAddress, lightConstantsAddress, giWidth, giHeight);

        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::RayTracedGI);

        Texture* giNrdInput = &m_rayTracedGIPass.GetRawGITexture();

        bool signedGIReconstructed = false;

        if (runSignedCorrection)
        {
            m_giSignedCorrectionPass.GetSignedCorrectionTexture().Transition(
                commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            if (!m_giSignedCorrectionPass.Execute(commandList, m_uploadBuffer, m_gBuffer, currentView, currentProjection))
            {
                m_giSignedCorrectionPass.ResetHistory();
                Log::Error("Failed to execute signed GI correction accumulation.");
                return false;
            }

            if (!m_giSignedCorrectionPass.Reconstruct(commandList, m_uploadBuffer, m_gBuffer,
                                                      m_environment.GetDiffuseIrradiance(),
                                                      m_rayTracedGIPass.GetRawGITexture()))
            {
                Log::Error("Failed to reconstruct signed GI irradiance.");
                return false;
            }

            giNrdInput = &m_giSignedCorrectionPass.GetReconstructedTexture();
            signedGIReconstructed = true;
        }

        const ReflectionDenoiserMode reflectionMode = m_debugUI.GetReflectionDenoiserMode();
        runNrd = reflectionMode == ReflectionDenoiserMode::Nrd;

        switch (debugView)
        {
        case ToneMapDebugView::NrdViewZ:
        case ToneMapDebugView::NrdNormal:
        case ToneMapDebugView::NrdRoughness:
        case ToneMapDebugView::NrdMotion:
        case ToneMapDebugView::NrdSpecularRadiance:
        case ToneMapDebugView::NrdHitDistance:
        case ToneMapDebugView::NrdResolvedReflection:
        case ToneMapDebugView::GIDenoised:
            runNrd = true;
            break;
        default:
            break;
        }

        // Native mode can coexist with NRD forced by RTGI or debug output.
        runNrd = runNrd || options.enableRTGI;
        runNative = reflectionMode == ReflectionDenoiserMode::Native;

        const bool resetNrdHistory =
            !m_hasPreviousNrdCamera || !m_nrdExecutedLastFrame || limiterEnabled != m_previousContributionLimiterEnabled;
        // Restart diffuse NRD history when signed production GI resumes after being inactive.
        const bool resetGIHistory = resetNrdHistory || runSignedCorrection != m_signedCorrectionExecutedLastFrame;

        if (runNrd)
        {
            m_nrdReflectionDenoiser.NewFrame();
            m_nrdGIDenoiser.NewFrame();

            if (!m_nrdReflectionDenoiser.SetFrameSettings(currentView, currentProjection, previousNrdView,
                                                          previousNrdProjection, m_nrdFrameIndex, nrdDenoisingRange,
                                                          resetNrdHistory))
            {
                return false;
            }

            if (!m_nrdGIDenoiser.SetFrameSettings(currentView, currentProjection, previousNrdView, previousNrdProjection,
                                                  m_nrdFrameIndex, nrdDenoisingRange, resetGIHistory))
            {
                return false;
            }

            Texture* nrdInput = &m_rayTracedReflectionPass.GetRawReflectionTexture();

            if (limiterEnabled)
            {
                m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex,
                                                   GpuTimestampProfiler::Region::ReflectionContributionLimiter);

                const bool limited =
                    m_reflectionContributionLimitPass.Execute(commandList, *nrdInput, m_uploadBuffer, m_sceneWidth, m_sceneHeight);
                m_gpuTimestampProfiler.EndRegion(commandList, frameIndex,
                                                 GpuTimestampProfiler::Region::ReflectionContributionLimiter);

                if (!limited)
                {
                    Log::Error("Failed to execute reflection contribution limiter.");
                    return false;
                }

                nrdInput = &m_reflectionContributionLimitPass.GetOutputTexture();
            }

            m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdPrepare);

            m_nrdReflectionPreparePass.ExecuteReflections(
                commandList, m_gBuffer, *nrdInput, m_rayTracedReflectionPass.GetSpecularTrackingDistanceTexture(),
                m_uploadBuffer, reconstructionConstants.inverseViewProjection, currentView,
                CurrentMaterialBuffer().GetDescriptorIndex(), m_sceneWidth, m_sceneHeight, nrdDenoisingRange);

            m_nrdGIPreparePass.ExecuteGI(commandList, m_gBuffer, *giNrdInput, m_rayTracedGIPass.GetHitDistanceTexture(),
                                         m_uploadBuffer, reconstructionConstants.inverseViewProjection, currentView,
                                         CurrentMaterialBuffer().GetDescriptorIndex(), m_sceneWidth, m_sceneHeight, giWidth, giHeight,
                                         nrdDenoisingRange);

            m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdPrepare);

            m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdReblurSpecular);

            m_nrdReflectionDenoiser.Denoise(
                commandList, m_nrdReflectionPreparePass.GetMotionTexture(),
                m_nrdReflectionPreparePass.GetNormalRoughnessTexture(), m_nrdReflectionPreparePass.GetViewZTexture(),
                m_nrdReflectionPreparePass.GetSpecRadianceHitDistTexture(), NrdDenoiser::Signal::Specular);

            m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdReblurSpecular);

            m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdReblurDiffuse);

            m_nrdGIDenoiser.Denoise(commandList, m_nrdGIPreparePass.GetMotionTexture(),
                                    m_nrdGIPreparePass.GetNormalRoughnessTexture(), m_nrdGIPreparePass.GetViewZTexture(),
                                    m_nrdGIPreparePass.GetDiffRadianceHitDistTexture(), NrdDenoiser::Signal::Diffuse);

            m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdReblurDiffuse);

            commandList->SetDescriptorHeaps(1, descriptorHeaps);

            m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdResolve);

            m_nrdReflectionResolvePass.Execute(commandList, m_gBuffer, m_nrdReflectionDenoiser.GetOutputTexture(),
                                               m_uploadBuffer, reconstructionConstants.inverseViewProjection, currentView,
                                               CurrentMaterialBuffer().GetDescriptorIndex(), m_sceneWidth, m_sceneHeight);

            m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdResolve);

            m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdDiffuseResolve);
            m_nrdDiffuseResolvePass.Execute(commandList, m_gBuffer, m_nrdGIDenoiser.GetDiffuseOutputTexture(),
                                            m_nrdGIPreparePass.GetViewZTexture(), m_uploadBuffer, m_sceneWidth, m_sceneHeight,
                                            nrdDenoisingRange);

            m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdDiffuseResolve);

            m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::GISpatialDenoise);
            m_giSpatialDenoisePass.Execute(commandList, m_gBuffer, m_nrdDiffuseResolvePass.GetOutputTexture(),
                                           m_uploadBuffer, m_sceneWidth, m_sceneHeight);
            m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::GISpatialDenoise);
        }

        if (runNative)
        {
            if (!m_nativeReflectionExecutedLastFrame)
            {
                m_rayTracedReflectionTemporalPass.ResetHistory();
            }

            m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ReflectionTemporal);
            m_rayTracedReflectionTemporalPass.Execute(commandList, m_rayTracedReflectionPass.GetRawReflectionTexture(),
                                                      m_gBuffer, m_uploadBuffer, reflectionConstants.inverseViewProjection,
                                                      shadowConstants.previousViewProjection,
                                                      shadowConstants.previousInverseViewProjection, m_sceneWidth, m_sceneHeight);

            m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ReflectionTemporal);

            m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ReflectionDenoise);
            m_rayTracedReflectionDenoisePass.Execute(commandList, m_rayTracedReflectionTemporalPass.GetReflectionTexture(),
                                                     m_rayTracedReflectionPass.GetHitDistanceTexture(), m_gBuffer,
                                                     m_uploadBuffer, reflectionConstants.inverseViewProjection, m_sceneWidth,
                                                     m_sceneHeight);
            m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ReflectionDenoise);
        }

        Texture& finalReflection = reflectionMode == ReflectionDenoiserMode::Nrd
                                       ? m_nrdReflectionResolvePass.GetOutputTexture()
                                       : m_rayTracedReflectionDenoisePass.GetFilteredReflectionTexture();

        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ShadowDenoise);

        m_shadowDenoisePass.Execute(commandList, m_rayTracedShadowPass.GetVisibilityTexture(), m_gBuffer, m_uploadBuffer,
                                    reconstructionConstants.inverseViewProjection, m_sceneWidth, m_sceneHeight);

        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ShadowDenoise);

        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::DeferredLighting);

        m_deferredLightingPass.Execute(
            commandList, m_gBuffer, CurrentMaterialBuffer(), m_environment.GetSourceTexture(), m_environment.GetCubemap(),
            m_environment.GetDiffuseIrradiance(), m_brdfIntegrationLUT, m_ggxAverageAlbedoLUT,
            m_environment.GetGGXMultiscatterIrradiance(), m_shadowDenoisePass.GetFilteredVisibilityTexture(),
            finalReflection, m_giSpatialDenoisePass.GetOutputTexture(), m_aoResolvePass.GetOutputTexture(),
            options.enableRTGI, options.rtgiContactAOStrength, m_sceneColor, reconstructionConstantsAddress,
            lightConstantsAddress, static_cast<unsigned int>(debugView), m_debugUI.IsShadingNormalCorrectionEnabled());

        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::DeferredLighting);

        switch (debugView)
        {
        case ToneMapDebugView::GBufferWorldNormal:
            debugRenderTarget = &m_gBuffer.GetSurface();
            break;
        case ToneMapDebugView::GBufferAlbedo:
            debugRenderTarget = &m_gBuffer.GetMaterial();
            break;
        case ToneMapDebugView::GBufferMotion:
            debugRenderTarget = &m_gBuffer.GetMotion();
            break;
        case ToneMapDebugView::SpecularTrackingDistance:
            debugTexture = &m_rayTracedReflectionPass.GetSpecularTrackingDistanceTexture();
            break;
        case ToneMapDebugView::NrdViewZ:
            debugTexture = &m_nrdReflectionPreparePass.GetViewZTexture();
            break;
        case ToneMapDebugView::NrdNormal:
        case ToneMapDebugView::NrdRoughness:
            debugTexture = &m_nrdReflectionPreparePass.GetNormalRoughnessTexture();
            break;
        case ToneMapDebugView::NrdMotion:
            debugTexture = &m_nrdReflectionPreparePass.GetMotionTexture();
            break;
        case ToneMapDebugView::NrdSpecularRadiance:
        case ToneMapDebugView::NrdHitDistance:
            debugTexture = &m_nrdReflectionPreparePass.GetSpecRadianceHitDistTexture();
            break;
        case ToneMapDebugView::NrdResolvedReflection:
            debugTexture = &m_nrdReflectionResolvePass.GetOutputTexture();
            break;

        case ToneMapDebugView::SelectionMask:
            debugRenderTarget = &m_selectionMask;
            break;
        case ToneMapDebugView::GIRaw:
            debugTexture = &m_rayTracedGIPass.GetRawGITexture();

            break;
        case ToneMapDebugView::GIHitDistance:
            debugTexture = &m_rayTracedGIPass.GetHitDistanceTexture();
            break;
        case ToneMapDebugView::GIDenoised:
            debugTexture = &m_giSpatialDenoisePass.GetOutputTexture();
            break;
        case ToneMapDebugView::GISignedReconstructed:
            // A null debug input safely falls back to normal rendering in ToneMapPass.

            if (runSignedCorrection && signedGIReconstructed)
            {
                debugTexture = &m_giSignedCorrectionPass.GetReconstructedTexture();
            }

            break;
        case ToneMapDebugView::AORaw:
            debugTexture = &m_rayTracedAOPass.GetRawAOTexture();
            break;
        case ToneMapDebugView::AOTemporal:
            debugTexture = &m_aoTemporalPass.GetAccumulatedAOTexture();
            break;
        case ToneMapDebugView::AOFiltered:
            debugTexture = &m_aoDenoisePass.GetFilteredAOTexture();
            break;
        default:
            break;
        }

    }
    else
    {
        // Do not dispatch rays, denoise, light, or sample historical debug outputs
        // without a TLAS. Initialize the only color input consumed by tone mapping.
        m_sceneColor.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float emptyColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
        commandList->ClearRenderTargetView(m_sceneColor.GetRTV(), emptyColor, 0, nullptr);
    }

    // Final composition precedes collider lines and the distinct Editor/packaged presentation paths.
    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ToneMap);

    m_finalSceneColor.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_toneMapPass.Execute(commandList, m_sceneColor, m_finalSceneColor.GetRTV(), debugTexture,
                          hasRenderableMeshes ? debugView : ToneMapDebugView::None, nrdDenoisingRange,
                          debugRenderTarget, &m_selectionMask, options.selectionOutlineColor,
                          debugView == ToneMapDebugView::None ? options.selectionOutlineThickness : 0,
                          m_gBuffer.GetDepth().GetSRVIndex());

    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ToneMap);

    // Draw only into the final editor image: temporal history and lighting must never see these lines.
    const EditorColliderOverlay* colliderOverlay = options.editorColliderOverlay;
    if (!m_packagedRuntime && colliderOverlay)
    {
        if (colliderOverlay->Matches(scene, m_sceneResourceLifetime))
        {
            m_editorColliderPass.Execute(commandList, m_finalSceneColor, m_gBuffer.GetDepth(),
                currentViewProjection, *colliderOverlay);
        }
    }
    m_finalSceneColor.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    if (m_packagedRuntime)
    {
        const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(m_applicationWidth), static_cast<float>(m_applicationHeight), 0, 1};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(m_applicationWidth), static_cast<LONG>(m_applicationHeight)};
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);
        m_toneMapPass.Execute(commandList, m_sceneColor, rtvHandle, nullptr, ToneMapDebugView::None, nrdDenoisingRange,
            nullptr, &m_selectionMask, options.selectionOutlineColor, 0, m_gBuffer.GetDepth().GetSRVIndex());
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        commandList->ResourceBarrier(1, &barrier);
    }
    else
    {
        // Preview updates share scene submission; unchanged valid outputs require no draws.
        ID3D12DescriptorHeap* previewHeap = m_resourceDescriptorHeap.GetHeap();
        commandList->SetDescriptorHeaps(1, &previewHeap);

        if (!m_materialPreviewRenderer.Record({commandList, frameIndex,
                m_brdfIntegrationLUT.GetDescriptorIndex(), m_ggxAverageAlbedoLUT.GetDescriptorIndex(),
                m_activeRecordingGeneration}))
        {
            return false;
        }

        if (!m_textureThumbnails.Record(commandList, m_activeRecordingGeneration))
        {
            return false;
        }

        // Uploaded editor previews use the same tracked transitions as material textures.
        for (Texture* texture : options.editorTextures)
        {
            texture->Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        // UI composes the scene image onto a clean application-sized background.
        const D3D12_VIEWPORT uiViewport{
            0.0f, 0.0f, static_cast<float>(m_applicationWidth), static_cast<float>(m_applicationHeight), 0.0f, 1.0f};
        const D3D12_RECT uiScissor{0, 0, static_cast<LONG>(m_applicationWidth), static_cast<LONG>(m_applicationHeight)};
        commandList->RSSetViewports(1, &uiViewport);
        commandList->RSSetScissorRects(1, &uiScissor);
        commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        ordinaryProbe.reset();
        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::DebugUI);
        {
            FrameHitchProbe uiProbe(
                *this,
                FrameHitchRegion::DebugUI);

            if constexpr (kEnableFrameHitchDiagnostics)
            {
                m_frameHitchDiagnostics.current.uiTextureObserved = true;
                m_debugUI.EndFrame(commandList, &m_frameHitchDiagnostics.current.uiTextureUploadRequested);
            }
            else
            {
                m_debugUI.EndFrame(commandList);
            }
        }

        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::DebugUI);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

        commandList->ResourceBarrier(1, &barrier);

    }

    m_gpuTimestampProfiler.EndFrameAndResolve(commandList, frameIndex);

    m_cpuFrameTiming.recording = CpuTiming::MillisecondsSince(recordingStart);
    // Close before ordinary/covered execution; keep their failure and provenance policies separate.
    const auto endStart = CpuTiming::Clock::now();
    const bool endedRecording = EndTrackedRecording();
    m_cpuFrameTiming.end = CpuTiming::MillisecondsSince(endStart);

    if (!endedRecording)
    {
        Log::Error("Failed to end command recording.");
        return false;
    }

    const auto executeStart = CpuTiming::Clock::now();
    bool executed = false;

    if (displacementFrame)
    {
        const auto generation = m_displacementCoverage.current.generation;
        const bool armed = !preparedDisplacement || ArmPendingDisplacementSubmission();

        if (!armed)
        {
            return false;
        }

        const auto reservation =
            preparedDisplacement ? m_pendingDisplacementUpdate.reservation : m_displacementCoverage.current.reservation;
        SubmissionFenceToken token{};
        const auto result = ExecuteCoveredDisplacementRecording(frameIndex, reservation, token);

        if (preparedDisplacement)
        {
            const bool resolved = ResolvePendingDisplacementSubmission(result, token);
            executed = result == ReservedSubmissionResult::Submitted && resolved;
        }
        else
        {
            executed = result == ReservedSubmissionResult::Submitted &&
                       AssociatePendingDisplacementTLASSubmission(frameIndex, generation, result, token);
        }
    }
    else
    {
        executed = ExecuteTrackedRecording(frameIndex);
    }

    const auto executeEnd = CpuTiming::Clock::now();
    m_cpuFrameTiming.execute = std::chrono::duration<double, std::milli>(executeEnd - executeStart).count();

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        auto& snapshot = m_frameHitchDiagnostics.current;
        const auto& timing = m_commandContext.GetExecuteCpuTiming();

        if (executed && snapshot.contextStart != CpuTiming::Clock::time_point{} && timing.commandsCalled &&
            timing.signalCalled)
        {
            snapshot.executeValidation =
                timing.validation +
                std::chrono::duration<double, std::milli>(snapshot.contextStart - executeStart).count();
            snapshot.executeCommands = timing.commands;
            snapshot.executeSignal = timing.signal;
            snapshot.executeAssociation =
                timing.bookkeeping +

                std::chrono::duration<double, std::milli>(executeEnd - snapshot.contextEnd).count();
        }
    }

    if (!executed)
    {
        m_materialDependencies.MarkUncertain(frameIndex);

        for (auto& entry : m_preparedTextureUploads)
        {
            entry.upload->MarkUncertain(textureUploadGeneration);
        }

        m_gpuTimestampProfiler.BreakFrameSequence();

        Log::Error("Failed to submit command list or signal its fence; GPU work may already be in flight.");
        return false;
    }

    // Capture the successful submission fence now; confirmation retains owners, not completion proof.
    const auto materialSubmissionFence = m_commandContext.GetLastSubmissionFenceValue();
    if (!ConfirmRecordedMaterialSubmission(
            frameIndex, textureUploadGeneration, materialRevision, materialSubmissionFence))
    {
        m_materialDependencies.MarkUncertain(frameIndex);
        Log::Error("Material dependency submission association failed; retaining allocations.");
        return false;
    }

    std::optional<FrameHitchProbe> postSubmissionProbe;

    if constexpr (kEnableFrameHitchDiagnostics)
    {
        postSubmissionProbe.emplace(*this, FrameHitchRegion::PostSubmission);
    }

    // Submitted facts survive a later Present failure; do not move them into successful history.
    m_cpuFrameTiming.submitted = true;
    m_cpuFrameTiming.executeStart = executeStart;

    // CPU call-entry cadence, not GPU execution start or idle time.

    if (hasPreviousExecute)
    {
        m_cpuFrameTiming.executeInterval =
            std::chrono::duration<double, std::milli>(executeStart - m_previousRenderExecuteStart).count();
        m_cpuFrameTiming.executeIntervalValid = true;
    }

    const uint64_t submissionFence = m_commandContext.GetFrameFenceValue(frameIndex);

    m_gpuTimestampProfiler.MarkSubmitted(frameIndex, submissionFence, m_gpuProfilerFrameSerial);
    m_aoExecutedLastFrame = runAO;

    m_pendingStatistics[frameIndex].statistics = m_currentFrameStatistics;
    m_pendingStatistics[frameIndex].valid = true;

    ++m_gpuProfilerFrameSerial;

    postSubmissionProbe.reset();
    const auto presentStart = CpuTiming::Clock::now();
    const bool presented = m_swapChain.Present();
    m_cpuFrameTiming.present = CpuTiming::MillisecondsSince(presentStart);
    m_cpuFrameTiming.presentReturn = CpuTiming::Clock::now();

    m_cpuFrameTiming.presentationReturned = true;

    if (!presented)
    {
        Log::Error("Failed to present swap chain.");
        return false;
    }

    // Only this tail follows successful Present; earlier recording-time histories remain separate.
    FrameHitchProbe postPresentProbe(
        *this,
        FrameHitchRegion::PostPresent);
    m_previousNrdView = currentView;
    m_previousNrdProjection = currentProjection;
    m_hasPreviousNrdCamera = true;
    m_nrdExecutedLastFrame = runNrd;

    m_nativeReflectionExecutedLastFrame = runNative;

    if (runNrd)
    {
        m_previousContributionLimiterEnabled = limiterEnabled;
        m_signedCorrectionExecutedLastFrame = runSignedCorrection;
        ++m_nrdFrameIndex;
    }

    signedHistoryFrame.completed = true;
    m_cpuFrameTiming.valid = true;
    m_previousRenderExecuteStart = executeStart;
    m_hasPreviousRenderExecute = true;

    return true;
}

// --------------
// Shutdown
// --------------

void Renderer::Shutdown()
{
    if (m_sceneInitializationAttempt.candidate)
    {
        Log::Error("Renderer shutdown: failed scene initialization requires checked cleanup; aborting.");
        std::abort();
    }

    if (m_heightmapRegistration.owner)
    {
        Log::Error("Renderer shutdown requires checked scene-resource release before heightmap teardown.");
        std::abort();
    }

    InvalidateRecordingEligibility();

    if (HasUnresolvedDisplacementUpdate())
    {
        Log::Error("Renderer shutdown: unresolved displacement update; aborting without teardown.");
        std::abort();
    }

    m_sceneResourceLifetime = 0;
    m_sceneResourceScene = nullptr;
    Log::Info("Renderer shutdown: waiting for GPU...");

    if (m_commandContextInitialized && !m_commandContext.WaitForGPU())
    {
        Log::Error("Renderer shutdown: GPU completion is unconfirmed; aborting without resource teardown.");

        std::abort();
    }

    if(m_commandContextInitialized && !m_commandContext.DiscardCommandsAfterGPUDrain())
    {
        Log::Error("Renderer shutdown: unsubmitted commands could not be discarded.");
        std::abort();
    }

    m_rayTracingScene.Shutdown();
    m_textureThumbnails.ClearAfterGPUDrain();

    Log::Info("Renderer shutdown: GPU finished.");
    ShutdownMaterialTables();

    m_materialPreviewRenderer.Shutdown();
    m_commandRecordingGeneration = 0;
    m_commandSubmissionAttempted = false;

    Log::Info("Renderer shutdown: shutting down DebugUI...");
    if(m_debugUIInitialized) m_debugUI.Shutdown();
    m_debugUIInitialized=false;
    Log::Info("Renderer shutdown: DebugUI finished.");

    Log::Info("Renderer shutdown: releasing renderer resources...");

    m_swapChain.Shutdown();
    m_gpuTimestampProfiler.Shutdown();
    m_commandContext.Shutdown();
    m_commandContextInitialized=false;
    m_resourceUploader.Shutdown();

    m_gBuffer.Shutdown();

    m_deferredLightingPass.Shutdown();
    m_toneMapPass.Shutdown();
    m_editorColliderPass.Shutdown();
    m_brdfIntegrationPass.Shutdown();
    m_ggxAverageAlbedoPass.Shutdown();
    m_rayTracedShadowPass.Shutdown();
    m_rayTracedReflectionPass.Shutdown();
    m_rayTracedGIPass.Shutdown();
    m_rayTracedAOPass.Shutdown();
    m_aoResolvePass.Shutdown();
    m_aoDenoisePass.Shutdown();
    m_aoTemporalPass.Shutdown();

    m_rayTracedReflectionTemporalPass.Shutdown();
    m_rayTracedReflectionDenoisePass.Shutdown();
    m_reflectionContributionLimitPass.Shutdown();
    m_nrdReflectionPreparePass.Shutdown();
    m_nrdGIPreparePass.Shutdown();
    m_nrdReflectionDenoiser.Shutdown();
    m_nrdGIDenoiser.Shutdown();
    m_nrdReflectionResolvePass.Shutdown();
    m_nrdDiffuseResolvePass.Shutdown();
    m_giSpatialDenoisePass.Shutdown();
    m_giSignedCorrectionPass.Shutdown();
    m_giSignedCorrectionInitialized = false;
    m_shadowDenoisePass.Shutdown();
    m_selectionMaskCandidate.reset();
    m_selectionMaskCandidateLifetime = 0;
    m_selectionMaskPass.Shutdown();
    m_displacementIndicesPass.Shutdown();
    m_displacementVerticesPass.Shutdown();

    m_displacementGPUPatchBuffer.Shutdown();

    for (unsigned int& descriptorIndex : m_displacementLODDescriptorIndices)
    {
        if (descriptorIndex != UINT_MAX)
        {
            m_resourceDescriptorHeap.Free(descriptorIndex);
            descriptorIndex = UINT_MAX;
        }
    }

    m_uploadBuffer.Shutdown();
    m_environment.Shutdown();
    m_sceneColor.Shutdown();
    m_finalSceneColor.Shutdown();
    m_selectionMask.Shutdown();

    m_brdfIntegrationLUT.Shutdown();
    m_ggxAverageAlbedoLUT.Shutdown();

    m_resourceDescriptorHeap.Shutdown();
    m_rtvDescriptorHeap.Shutdown();
    m_dsvDescriptorHeap.Shutdown();

    m_shaderLibrary.Shutdown();
    Log::Info("Renderer shutdown: resources released.");

    m_graphicsDevice.Shutdown();
}

bool Renderer::ReplaceMeshGeometry(
    Mesh& mesh,
    const void* vertices,
    size_t vertexCount,
    const unsigned int* indices,
    size_t indexCount)
{
    RecordingEligibilityScope recordingEligibility{*this};

    if (HasUnresolvedDisplacementUpdate())
    {
        Log::Error("Cannot replace geometry with an unresolved displacement update.");
        return false;
    }

    if (m_geometryUpdateFailed)
    {
        return false;
    }

    // No outstanding graphics work may reference the old buffers or BLAS.

    if (!m_commandContext.WaitForGPU())
    {
        m_geometryUpdateFailed = true;
        return false;
    }

    // Failure before this point leaves the existing geometry intact.

    if (!mesh.ReplaceGeometry(m_graphicsDevice.GetDevice(), m_resourceUploader, m_resourceDescriptorHeap, vertices,
                              vertexCount, indices, indexCount))
    {
        // Conservatively stop rendering: a partial replacement must
        // never be followed by ray tracing against stale geometry.
        m_geometryUpdateFailed = true;
        return false;
    }

    // From here onward, the existing BLAS must not be used again
    // until the replacement and TLAS refresh complete.
    m_geometryUpdateFailed = true;

    constexpr unsigned int buildFrameIndex = 0;

    if (!BeginTrackedRecording(buildFrameIndex))
    {
        Log::Error("Failed to begin geometry acceleration structure update.");
        return false;
    }

    ID3D12GraphicsCommandList4* commandList = m_commandContext.GetRayTracingCommandList();

    if (!m_rayTracingScene.RebuildMeshBLAS(m_graphicsDevice.GetRayTracingDevice(), commandList, &mesh))
    {
        AbortTrackedRecording();

        Log::Error("Failed to rebuild mesh BLAS.");
        return false;
    }

    if (!m_rayTracingScene.RefreshTLASReferences(commandList, buildFrameIndex))
    {
        AbortTrackedRecording();
        Log::Error("Failed to refresh TLAS references.");

        return false;
    }

    if (!EndTrackedRecording())
    {
        Log::Error("Failed to close geometry update command list.");
        return false;
    }

    const bool submitted = ExecuteTrackedRecording(buildFrameIndex);

    // Execute() may fail after GPU submission. Always attempt to drain.

    if (!m_commandContext.WaitForGPU())
    {
        Log::Error("GPU completion unconfirmed after geometry update.");
        return false;
    }

    if (!submitted)
    {
        Log::Error("Geometry update submission failed.");

        return false;
    }

    m_rayTracingScene.ReleaseRetiredBLASResources();

    ResetSceneHistory();

    m_pendingGeometryTLASRefresh[0] = true;
    m_pendingGeometryTLASRefresh[1] = true;
    m_geometryUpdateFailed = false;

    return true;
}

bool Renderer::BuildDisplacementRequestLayout(
    const Mesh& mesh,
    const DisplacementMeshSettings& settings,
    DisplacementGPUIndexLayout& layout,
    RayTracingGeometryCounts& counts) const
{
    return RendererDisplacement::BuildDisplacementRequestLayout(mesh, settings, layout, counts);
}

bool Renderer::GenerateDisplacementMeshGPU(
    Mesh& mesh,
    Texture& heightmap,
    const DisplacementMeshSettings& settings)
{
    if (!ValidateDisplacementHeightmapRegistration() || m_heightmapRegistration.mesh != &mesh ||
        m_heightmapRegistration.texture != &heightmap)
    {
        return false;
    }

    RecordingEligibilityScope recordingEligibility{*this};

    if (HasUnresolvedDisplacementUpdate())
    {
        Log::Error("Cannot generate displacement with an unresolved displacement update.");
        return false;
    }

    using TimingClock = std::chrono::steady_clock;
    const auto timingStart = TimingClock::now();

    if (m_geometryUpdateFailed)
    {
        Log::Error("Cannot generate GPU displacement geometry: "
                   "a previous geometry update failed.");
        return false;
    }

    DisplacementGPUIndexLayout layout{};

    RayTracingGeometryCounts generatedCounts{};

    if (!BuildDisplacementRequestLayout(mesh, settings, layout, generatedCounts))
    {
        return false;
    }

    const auto layoutDone = TimingClock::now();

    const bool reusePatchStorage = m_displacementGPUPatchBuffer.HasReadyPersistentCapacity(layout.patches.size());

    // No-growth: Begin(0) fences allocator/upload-slot CPU reuse. In-place
    // geometry writes follow prior rendering on this same DIRECT queue, with
    // the existing transition barriers; no live descriptors/resources change.
    // Begin(0) also protects slot-zero TLAS instance upload writes. Shared AS
    // GPU accesses follow prior rendering on the same queue; replaced BLAS
    // resources remain retained until the combined completion wait succeeds.
    // Preparation/replacement still requires actual full-queue idle.

    if (!reusePatchStorage && !m_commandContext.WaitForGPU())
    {
        Log::Error("Failed to wait for GPU before displacement generation.");

        m_geometryUpdateFailed = true;
        return false;
    }

    const auto initialWaitDone = TimingClock::now();

    if (!reusePatchStorage && !m_displacementGPUPatchBuffer.PreparePersistentStorage(

                                  m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, layout.patches.size(),
                                  true)) // Only reached after the full WaitForGPU confirmed idle.
    {
        Log::Error("Failed to prepare persistent GPU displacement patch buffer.");

        return false;
    }

    const auto patchPreparationDone = TimingClock::now();

    constexpr unsigned int generationFrameIndex = 0;

    if (!BeginTrackedRecording(generationFrameIndex))
    {
        Log::Error("Failed to begin GPU displacement generation.");
        return false;
    }

    const auto beginDone = TimingClock::now();
    m_uploadBuffer.Reset(generationFrameIndex);
    ObserveFrameUploadReset(generationFrameIndex);

    // Save the states before recording compute transitions.
    // These are needed if the command list is aborted.
    const D3D12_RESOURCE_STATES originalVertexState = mesh.GetVertexBufferState();

    const D3D12_RESOURCE_STATES originalIndexState = mesh.GetIndexBufferState();
    const auto originalHeightmapStates = heightmap.GetSubresourceStates();
    const auto originalPatchState = m_displacementGPUPatchBuffer.GetStateSnapshot();

    const auto restoreAbortedStates = [&]()
    {
        mesh.RestoreBufferStatesAfterAbort(originalVertexState, originalIndexState);
        heightmap.RestoreStatesAfterAbort(originalHeightmapStates);

        m_displacementGPUPatchBuffer.RestoreStateAfterAbort(originalPatchState);
    };

    const auto abortGeneration = [&]()
    {
        if (!AbortTrackedRecording())
        {
            m_geometryUpdateFailed = true;

            Log::Error("Displacement recording discard unconfirmed; resource tracking remains unresolved.");

            return;
        }

        // No Execute() occurred, so none of these barriers reached the GPU.
        // This restores buffer tracking only; an AS mutation failure keeps
        // m_geometryUpdateFailed set and retains all AS ownership changes.
        restoreAbortedStates();
    };

    ID3D12GraphicsCommandList* commandList = m_commandContext.GetCommandList();

    ID3D12DescriptorHeap* descriptorHeaps[] = {m_resourceDescriptorHeap.GetHeap()};

    commandList->SetDescriptorHeaps(1, descriptorHeaps);

    const auto patchRecordingStart = TimingClock::now();

    if (!m_displacementGPUPatchBuffer.RecordUpdate(commandList, m_uploadBuffer, layout))
    {
        Log::Error("Failed to record GPU displacement patch metadata update.");
        abortGeneration();
        return false;
    }

    const auto patchRecordingDone = TimingClock::now();

    if (!m_displacementVerticesPass.Execute(commandList, mesh, heightmap, m_uploadBuffer, settings.patchesPerAxis,
                                            settings.subdivisionsPerPatch, settings.displacementScale,
                                            settings.displacementMidpoint))
    {
        Log::Error("Failed to record GPU displacement vertex generation.");
        abortGeneration();
        return false;
    }

    if (!m_displacementIndicesPass.Execute(commandList, m_uploadBuffer, mesh, settings.patchesPerAxis,
                                           settings.subdivisionsPerPatch,
                                           m_displacementGPUPatchBuffer.GetDescriptorIndex()))
    {
        Log::Error("Failed to record GPU displacement index generation.");

        abortGeneration();
        return false;
    }

    const auto computeRecordingDone = TimingClock::now();

    // AS recording mutates CPU ownership and TLAS instance metadata. From
    // this point an aborted list cannot make the scene renderable again.

    m_geometryUpdateFailed = true;

    // Compute pass read transitions order UAV writes before BLAS input reads
    // in this same list; there is no inter-submission COMMON decay here.
    ID3D12GraphicsCommandList4* buildCommandList = m_commandContext.GetRayTracingCommandList();

    if (!m_rayTracingScene.RebuildMeshBLAS(m_graphicsDevice.GetRayTracingDevice(), buildCommandList, &mesh,
                                           generatedCounts))
    {
        Log::Error("Failed to record GPU displacement BLAS rebuild.");
        abortGeneration();
        return false;
    }

    if (!m_rayTracingScene.RefreshTLASReferences(buildCommandList, generationFrameIndex))
    {
        Log::Error("Failed to refresh TLAS references after GPU displacement generation.");
        abortGeneration();
        return false;
    }

    const auto buildRecordingDone = TimingClock::now();

    if (!EndTrackedRecording())
    {
        Log::Error("Failed to close GPU displacement acceleration structure update.");
        abortGeneration();
        return false;
    }

    const auto recordingDone = TimingClock::now();

    const bool submitted = ExecuteTrackedRecording(generationFrameIndex);

    const auto executeDone = TimingClock::now();

    // Execute() may submit work even if fence signaling fails.
    // Always attempt to confirm GPU completion.

    if (!m_commandContext.WaitForGPU())
    {
        Log::Error("GPU completion unconfirmed after combined displacement/BLAS/TLAS update.");
        return false;
    }

    const auto waitDone = TimingClock::now();

    if (!submitted)
    {
        Log::Error("Failed to submit combined displacement/BLAS/TLAS update.");

        return false;
    }

    // Both submission and completion succeeded. Only now resolve staging and
    // publish the counts used provisionally during BLAS recording.

    if (!m_displacementGPUPatchBuffer.ConfirmUpdateCompletion(true))
    {
        Log::Error("Failed to resolve completed GPU displacement patch update.");
        return false;
    }

    if (!mesh.SetGPUGeneratedGeometryCounts(generatedCounts.vertexCount, generatedCounts.indexCount))
    {
        Log::Error("Failed to register GPU-generated displacement geometry counts.");
        return false;
    }

    const auto countsDone = TimingClock::now();

    // The GPU has finished using the previous BLAS resources.
    m_rayTracingScene.ReleaseRetiredBLASResources();

    // Geometry changed, so temporal rendering history is no longer valid.
    ResetSceneHistory();

    // Ensure both frame slots refresh their TLAS state.
    m_pendingGeometryTLASRefresh[0] = true;
    m_pendingGeometryTLASRefresh[1] = true;

    // The generated geometry and acceleration structures are ready.
    m_geometryUpdateFailed = false;

    const auto cleanupDone = TimingClock::now();

    const auto milliseconds = [](TimingClock::time_point begin, TimingClock::time_point end)
    { return std::to_string(std::chrono::duration<double, std::milli>(end - begin).count()); };
    // CPU wall-clock only: waits include queued work and CPU blocking. Patch
    // staging records the copy; all GPU work is covered by the combined wait.
    // Total excludes formatting/logging and local teardown.
    Log::Info(std::string("Displacement CPU wall-clock ms (not GPU execution times), path=") +
              (reusePatchStorage ? "no-growth" : "idle-prepare") +
              ": layout/validation=" + milliseconds(timingStart, layoutDone) +
              ", capacity check/conditional full wait=" + milliseconds(layoutDone, initialWaitDone) +

              ", patch preparation=" + milliseconds(initialWaitDone, patchPreparationDone) +
              ", begin (includes slot fence wait)=" + milliseconds(patchPreparationDone, beginDone) +
              ", slot fence blocking wait=" + std::to_string(m_commandContext.GetBeginFenceWaitMilliseconds()) +

              ", recording setup=" + milliseconds(beginDone, patchRecordingStart) +
              ", patch staging/recording=" + milliseconds(patchRecordingStart, patchRecordingDone) +
              ", compute dispatch recording=" + milliseconds(patchRecordingDone, computeRecordingDone) +
              ", BLAS/TLAS recording=" + milliseconds(computeRecordingDone, buildRecordingDone) +
              ", combined close=" + milliseconds(buildRecordingDone, recordingDone) +

              ", combined submission call=" + milliseconds(recordingDone, executeDone) +

              ", combined blocking wait=" + milliseconds(executeDone, waitDone) + ", completion/count registration=" +
              milliseconds(waitDone, countsDone) + ", cleanup/history=" + milliseconds(countsDone, cleanupDone) +
              ", total=" + milliseconds(timingStart, cleanupDone));

    return true;
}

bool Renderer::DrainAndResolveSceneDisplacement(

    const Scene& scene,
    DisplacementCompletionResult& result)
{
    if (m_activeRecordingGeneration != 0 || m_displacementExitScopeGeneration != 0 || m_sceneResourceScene != &scene ||
        m_sceneResourceLifetime == 0)
    {
        Log::Error("Cannot resolve scene displacement during recording or for a different scene lifetime.");
        return false;
    }

    // Replace, rather than supplement, Engine's existing teardown wait.
    // A failed drain authorizes no cancellation, publication or resource release.

    if (!m_commandContext.WaitForGPU())
    {
        return false;
    }

    const auto status = m_pendingDisplacementUpdate.status;

    if (status == DisplacementUpdateStatus::Prepared)
    {
        if (m_pendingDisplacementUpdate.frameRecovery.state != DisplacementFrameExitState::Empty ||

            !ValidateDisplacementHeightmapRegistration() || !CancelPreparedDisplacementRequest())
        {
            Log::Error("Cannot cancel prepared displacement ownership during scene teardown.");
            return false;
        }
    }

    else if (status == DisplacementUpdateStatus::Submitted)
    {
        // Queue idle alone does not resolve patch tracking or attest success.
        // Existing polling validates the original token, every later owner,
        // coverage, replacement identity and selective retirement before commit.

        if (ProcessDisplacementCompletion() != DisplacementCompletionStatus::Finalized)
        {
            Log::Error("Drained displacement submission could not be safely finalized.");

            return false;
        }
    }
    else if (status != DisplacementUpdateStatus::Empty)
    {
        Log::Error("Cannot resolve discarded, recording or uncertain displacement ownership by queue drain.");

        return false;
    }

    if (HasUnresolvedDisplacementUpdate())
    {
        return false; // Includes uncertain synchronous heightmap use and coverage.
    }

    DisplacementCompletionResult completed{};

    if (m_completedDisplacementUpdate.requestRevision != 0)
    {
        if (!ValidateDisplacementHeightmapRegistration() || m_heightmapRegistration.scene != &scene ||
            !TakeDisplacementCompletion(*m_heightmapRegistration.owner, completed))
        {
            return false; // Preserve an undeliverable payload and reject release.
        }
    }

    result = completed;
    return true;
}

std::uint64_t Renderer::GetEditorCompletedFence() const
{
    return m_commandContext.GetCompletedFenceValue();
}

std::uint64_t Renderer::GetEditorSubmissionFence() const
{
    return m_commandContext.GetLastSubmissionFenceValue();
}

bool Renderer::WaitForGPU()
{
    return m_commandContext.WaitForGPU();
}

bool Renderer::TryGetFinalSceneImage(FinalSceneImage& image) const
{
    image = {};

    if(m_resizeFailed || !m_finalSceneColor.GetResource() || m_finalSceneColor.GetSRVIndex() == UINT_MAX)
    {
        return false;
    }

    if(m_sceneWidth == 0 || m_sceneHeight == 0 ||
       m_finalSceneColor.GetWidth() != m_sceneWidth || m_finalSceneColor.GetHeight() != m_sceneHeight)
    {
        return false;
    }

    image.srv = m_resourceDescriptorHeap.GetGPUHandle(m_finalSceneColor.GetSRVIndex());
    image.width = m_sceneWidth;
    image.height = m_sceneHeight;
    return true;
}

ID3D12Device* Renderer::GetDevice()
{
    return m_graphicsDevice.GetDevice();
}

ResourceUploader& Renderer::GetResourceUploader()
{
    return m_resourceUploader;
}

DescriptorHeap& Renderer::GetResourceDescriptorHeap()
{
    return m_resourceDescriptorHeap;
}

ShaderLibrary& Renderer::GetShaderLibrary()
{
    return m_shaderLibrary;
}

DebugUI& Renderer::GetDebugUI()
{
    return m_debugUI;
}

// Runtime environment discovery and preprocessing

bool Renderer::DiscoverEnvironments()
{
    if(m_packagedRuntime) { Log::Error("Loose environment discovery rejected in packaged mode."); return false; }
    m_availableEnvironments.clear();

    const std::filesystem::path environmentDirectory = m_assetRoot / "Textures" / "Environment";

    std::error_code error;

    if (!std::filesystem::exists(environmentDirectory, error) || error)
    {
        Log::Error("Failed to discover environment directory.");
        return false;
    }

    std::filesystem::directory_iterator iterator(
        environmentDirectory,
        error);
    const std::filesystem::directory_iterator end;

    if (error)
    {
        Log::Error("Failed iterating through environment directory.");
        return false;
    }

    for (; iterator != end; iterator.increment(error))
    {
        if (error)
        {
            Log::Error("Error iterating through directory.");
            m_availableEnvironments.clear();
            return false;
        }

        const auto& entry = *iterator;

        if (!entry.is_regular_file(error))
        {
            if (error)
            {
                m_availableEnvironments.clear();
                return false;
            }

            continue;
        }

        std::string extension = entry.path().extension().string();

        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (extension != ".hdr")
        {
            continue;
        }

        EnvironmentAsset asset;
        asset.displayName = entry.path().stem().string();
        asset.relativePath = std::filesystem::relative(entry.path(), m_assetRoot, error);

        if (error)
        {
            m_availableEnvironments.clear();
            return false;
        }

        m_availableEnvironments.push_back(std::move(asset));
    }

    std::sort(m_availableEnvironments.begin(), m_availableEnvironments.end(),
              [](const EnvironmentAsset& a, const EnvironmentAsset& b) { return a.displayName < b.displayName; });

    return true;
}

bool Renderer::SetEnvironment(size_t index)
{
    if(m_packagedRuntime) { Log::Error("Loose environment selection rejected in packaged mode."); return false; }
    if (index >= m_availableEnvironments.size())
    {
        Log::Error("Invalid environment index.");
        return false;
    }

    if (index == m_currentEnvironmentIndex)
    {
        return true;
    }

    if (!m_commandContext.WaitForGPU())
    {
        Log::Error("Failed to confirm GPU completion before environment switch.");
        return false;
    }

    m_environment.Shutdown();

    const EnvironmentAsset& asset = m_availableEnvironments[index];

    if(m_packagedRuntime) { Log::Error("Loose environment loading rejected in packaged mode."); return false; }
    const std::filesystem::path environmentPath = m_assetRoot / asset.relativePath;

    if (!m_environment.Initialize(m_graphicsDevice.GetDevice(), environmentPath, m_resourceUploader,
                                  m_resourceDescriptorHeap))
    {
        Log::Error("Failed to initialize environment: " + asset.displayName);

        m_environment.Shutdown();
        return false;
    }

    if (!PreprocessEnvironment(m_environment))
    {
        Log::Error("Failed to preprocess environment: " + asset.displayName);

        // Defer resource release to shutdown: preprocessing may have submitted work.
        return false;
    }

    m_currentEnvironmentIndex = index;

    ResetSceneHistory();

    Log::Info("Environment changed to: " + asset.displayName);

    return true;
}

bool Renderer::SetEnvironmentImage(const HDRImageData* image,std::string identity,std::string displayName,std::string reference)
{
    if(identity==m_environmentIdentity) return true;
    if(m_activeRecordingGeneration || !m_commandContext.WaitForGPU() || !m_commandContext.DiscardCommandsAfterGPUDrain())
    { Log::Error("Environment replacement requires a confirmed GPU drain outside recording."); return false; }
    m_environment.Shutdown();
    const bool loaded=image ? m_environment.Initialize(m_graphicsDevice.GetDevice(),*image,m_resourceUploader,m_resourceDescriptorHeap) :
        m_environment.Initialize(m_graphicsDevice.GetDevice(),std::filesystem::path{},m_resourceUploader,m_resourceDescriptorHeap);
    if(!loaded || !PreprocessEnvironment(m_environment)) { Log::Error("Required environment upload/conversion failed."); return false; }
    m_environmentIdentity=std::move(identity);
    auto entry=std::find_if(m_availableEnvironments.begin(),m_availableEnvironments.end(),[&](const auto& e){return e.relativePath.empty() && e.reference==reference && e.displayName==displayName;});
    if(entry==m_availableEnvironments.end()) { m_availableEnvironments.push_back({std::move(displayName),{},std::move(reference)}); m_currentEnvironmentIndex=m_availableEnvironments.size()-1; }
    else m_currentEnvironmentIndex=static_cast<size_t>(entry-m_availableEnvironments.begin());
    ResetSceneHistory(); return true;
}

bool Renderer::PreprocessEnvironment(EnvironmentMap& environment)
{
    RecordingEligibilityScope recordingEligibility{*this};

    EnvironmentMipPass environmentMipPass;

    EquirectangularToCubemapPass equirectangularToCubemapPass;

    DiffuseIrradiancePass diffuseIrradiancePass;

    GGXMultiscatterIrradiancePass ggxMultiscatterIrradiancePass;

    auto cleanup = [&]()
    {
        ggxMultiscatterIrradiancePass.Shutdown();
        diffuseIrradiancePass.Shutdown();

        equirectangularToCubemapPass.Shutdown();

        environmentMipPass.Shutdown();
    };

    ID3D12Device* device = m_graphicsDevice.GetDevice();

    if (!environmentMipPass.Initialize(device, m_shaderLibrary, m_rtvDescriptorHeap, environment.GetSourceTexture()))
    {
        Log::Error("Failed to initialize environment mip pass.");
        cleanup();
        return false;
    }

    if (!equirectangularToCubemapPass.Initialize(device, m_shaderLibrary, m_rtvDescriptorHeap,
                                                 environment.GetCubemap()))
    {
        Log::Error("Failed to initialize equirectangular-to-cubemap pass.");
        cleanup();
        return false;
    }

    if (!diffuseIrradiancePass.Initialize(device, m_shaderLibrary, m_rtvDescriptorHeap,
                                          environment.GetDiffuseIrradiance()))
    {
        Log::Error("Failed to initialize diffuse irradiance pass.");
        cleanup();
        return false;
    }

    if (!ggxMultiscatterIrradiancePass.Initialize(device, m_shaderLibrary, m_rtvDescriptorHeap,
                                                  environment.GetGGXMultiscatterIrradiance()))
    {
        Log::Error("Failed to initialize GGX multiscatter irradiance pass.");
        cleanup();
        return false;
    }

    if (!BeginTrackedRecording(0))
    {
        Log::Error("Failed to begin environment preprocessing.");
        cleanup();
        return false;
    }

    ID3D12GraphicsCommandList* commandList = m_commandContext.GetCommandList();

    ID3D12DescriptorHeap* descriptorHeaps[] = {m_resourceDescriptorHeap.GetHeap()};

    commandList->SetDescriptorHeaps(1, descriptorHeaps);

    environmentMipPass.Execute(commandList, environment.GetSourceTexture());

    diffuseIrradiancePass.Execute(commandList, environment.GetSourceTexture(), environment.GetDiffuseIrradiance());

    equirectangularToCubemapPass.Execute(commandList, environment.GetSourceTexture(), environment.GetCubemap());

    ggxMultiscatterIrradiancePass.Execute(commandList, environment.GetSourceTexture(), m_brdfIntegrationLUT,
                                          environment.GetGGXMultiscatterIrradiance());

    if (!EndTrackedRecording())
    {
        Log::Error("Failed to end environment preprocessing.");
        cleanup();
        return false;
    }

    const bool submitted = ExecuteTrackedRecording(0);

    if (!m_commandContext.WaitForGPU())
    {
        Log::Error("Failed to confirm environment preprocessing completion; retaining temporary GPU resources until process exit.");

        // These passes own raw D3D12 pointers. Do not release them or recycle their
        // descriptors while submitted work may still reference them.
        return false;
    }

    cleanup();

    if (!submitted)
    {
        Log::Error("Failed to execute environment preprocessing.");
        return false;
    }

    return true;
}

const RendererStatistics& Renderer::GetStatistics() const
{
    return m_statistics;
}

StructuredBuffer& Renderer::CurrentMaterialBuffer()
{
    ENGINE_ASSERT(m_activeRecordingSlot < CommandContext::FrameCount);
    return m_materialBuffers[m_activeRecordingSlot];
}

void Renderer::ShutdownMaterialTables()
{
    InvalidateMaterialAdmission();
    for(auto& replacement : m_materialReplacements) replacement.reset();
    m_publishedMaterialIdentities.clear();
    // Same checked completion/discard precondition as the former single scene table.
    for(auto& buffer : m_materialBuffers)
    {
        buffer.Shutdown();
    }

    for(auto& upload : m_materialUpload) upload.Shutdown();
    m_materialPublication = {};
    m_materialDependencies.ClearAfterGPUDrain();

    for(auto& entry : m_preparedTextureUploads)
    {
        entry.upload->ClearAfterGPUDrain();
    }

    m_preparedTextureUploads.clear();
}

bool Renderer::SubmitMaterialTableSnapshot(std::uint64_t identity, const std::vector<GPUMaterial>& records,
    std::string* errorMessage)
{
    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(identity == 0 || identity != m_sceneResourceLifetime || !m_sceneResourceScene ||
       m_sceneInitializationAttempt.candidate || m_activeRecordingGeneration != 0 ||
       m_displacementExitScopeGeneration != 0 || m_resizeFailed)
    {
        if(errorMessage)
        {
            *errorMessage = "Material publication requires the current initialized scene and no active recording.";
        }

        return false;
    }

    if(!m_materialPublication.SetSnapshot(records))
    {
        if(errorMessage)
        {
            *errorMessage = "Invalid material count, parameters, revision, or changed texture bindings.";
        }

        return false;
    }

    return true;
}

bool Renderer::RecordMaterialTable(unsigned int slot, ID3D12GraphicsCommandList* commandList)
{
    if(!commandList || slot >= CommandContext::FrameCount || m_activeRecordingSlot != slot ||
       m_activeRecordingGeneration == 0 || m_materialPublication.revision == 0)
    {
        return false;
    }

    if(!m_materialDependencies.Acquire(slot, m_commandContext.GetCompletedFenceValue(), m_materialPublication.revision))
    {
        return false;
    }

    if(m_materialPublication.published[slot] == m_materialPublication.revision)
    {
        return true;
    }

    if(auto& replacement = m_materialReplacements[slot])
    {
        // Acquire above proves this slot's last submitted use completed and no
        // unresolved recording owns it. Other slots keep their old generation.
        m_materialBuffers[slot].Swap(replacement->buffer);
        m_materialUpload[slot].Swap(replacement->upload);
        replacement.reset();
    }

    auto& materialUpload = m_materialUpload[slot];
    materialUpload.Reset(0); // BeginTrackedRecording has checked this slot's fence.
    const auto bytes = m_materialPublication.desired.size() * sizeof(GPUMaterial);
    const auto address = materialUpload.Allocate(m_materialPublication.desired.data(), bytes, 4);

    if(address == 0)
    {
        return false;
    }

    auto* upload = materialUpload.GetResource();
    auto* destination = m_materialBuffers[slot].GetResource();
    // Default-heap BUFFER resources decay to COMMON at each ExecuteCommandLists boundary.
    // A discarded recording never executes its transitions. COMMON implicitly promotes to
    // COPY_DEST for the copy, then the explicit barrier orders all raster/compute/DXR reads.
    commandList->CopyBufferRegion(destination, 0, upload, address - upload->GetGPUVirtualAddress(), bytes);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = destination;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(1, &barrier);
    return true;
}

bool Renderer::CopyMaterialTableSnapshot(std::uint64_t identity, std::vector<GPUMaterial>& records,
    std::string* errorMessage) const
{
    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(identity == 0 || identity != m_sceneResourceLifetime || !m_sceneResourceScene ||
       m_sceneInitializationAttempt.candidate || m_activeRecordingGeneration != 0 ||
       m_displacementExitScopeGeneration != 0 || m_resizeFailed)
    {
        if(errorMessage)
        {
            *errorMessage = "Material snapshot requires the current initialized scene and no active recording.";
        }

        return false;
    }

    auto copy = m_materialPublication.desired;
    records.swap(copy);
    return true;
}

bool Renderer::QueuePreparedTextureUpload(const PreparedProjectTexture& prepared, std::uint64_t& ticket, std::string& error)
{
    error.clear();
    ticket = 0;

    if(!m_graphicsDevice.GetDevice() || m_sceneResourceLifetime == 0 || m_sceneInitializationAttempt.candidate ||
       m_activeRecordingGeneration != 0 || m_displacementExitScopeGeneration != 0 ||
       m_resizeFailed || !ValidatePreparedProjectTexture(prepared))
    {
        error = "Rejected: prepared texture data or Renderer lifecycle is invalid.";
        return false;
    }

    if(prepared.pixels.size() > kTextureUploadByteCapacity)
    {
        error = "Rejected: prepared texture exceeds the 64 MiB CPU byte limit.";
        return false;
    }

    PollPreparedTextureUploads();
    size_t bytes = 0;

    for(const auto& entry : m_preparedTextureUploads)
    {
        bytes += entry.upload->CPUBytes();
    }

    if(m_preparedTextureUploads.size() >= kTextureUploadCapacity ||
       prepared.pixels.size() > kTextureUploadByteCapacity - bytes)
    {
        error = "Deferred: prepared texture request/CPU byte capacity unavailable.";
        return false;
    }

    try
    {
        const auto identity = AllocateRendererIdentity();

        if(identity == 0)
        {
            error = "Rejected: upload identities exhausted.";
            return false;
        }

        auto upload = std::make_unique<PreparedTextureUpload>(prepared);
        m_preparedTextureUploads.push_back({identity, std::move(upload), {}});
        ticket = identity;
        return true;
    }
    catch(const std::exception& exception)
    {
        error = std::string("Rejected: cannot retain prepared texture request: ") + exception.what();
        return false;
    }
}

std::optional<PreparedTextureUpload::State> Renderer::GetPreparedTextureUploadState(std::uint64_t ticket, std::string* error) const
{
    if(error)
    {
        error->clear();
    }

    for(const auto& entry : m_preparedTextureUploads)
    {
        if(entry.ticket == ticket)
        {
            if(error)
            {
                *error = entry.error;
            }

            return entry.upload->GetState();
        }
    }

    return std::nullopt;
}

bool Renderer::TakeCompletedPreparedTextureUpload(std::uint64_t ticket, Texture& destination)
{
    if(m_activeRecordingGeneration != 0 || m_sceneInitializationAttempt.candidate)
    {
        return false;
    }

    PollPreparedTextureUploads();

    for(auto& entry : m_preparedTextureUploads)
    {
        if(entry.ticket == ticket)
        {
            return entry.upload->Take(destination);
        }
    }

    return false;
}

void Renderer::CancelPreparedTextureUpload(std::uint64_t ticket)
{
    for(auto& entry : m_preparedTextureUploads)
    {
        if(entry.ticket == ticket)
        {
            entry.upload->Cancel();
        }
    }
}

void Renderer::PollPreparedTextureUploads()
{
    ValidateMaterialAdmissionLifecycle();
    const auto completed = m_commandContext.GetCompletedFenceValue();

    for(auto& entry : m_preparedTextureUploads)
    {
        entry.upload->Poll(completed);
    }

    std::erase_if(m_preparedTextureUploads, [](const TextureUploadEntry& entry) { return entry.upload->CanRetire(); });
}

void Renderer::RecordPreparedTextureUploads(ID3D12GraphicsCommandList* commands)
{
    size_t staging = 0;

    for(const auto& entry : m_preparedTextureUploads)
    {
        staging += entry.upload->StagingBytes();
    }

    for(auto& entry : m_preparedTextureUploads)
    {
        if(entry.upload->GetState() != PreparedTextureUpload::State::Queued)
        {
            continue;
        }

        const auto result = entry.upload->Record(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, commands,
            m_activeRecordingGeneration, m_activeRecordingSlot, kTextureUploadByteCapacity - staging, entry.error);

        if(result == PreparedTextureUpload::RecordResult::Recorded)
        {
            break; // At most one new GPU upload recording per frame.
        }
    }
}

bool Renderer::SubmitMaterialTableRevision(std::uint64_t identity, const std::vector<GPUMaterial>& records,
    std::vector<Texture::Dependency> dependencies, std::string* errorMessage)
{
    const auto fail = [&](const char* message)
    {
        if(errorMessage)
        {
            *errorMessage = message;
        }

        return false;
    };

    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(identity == 0 || identity != m_sceneResourceLifetime || !m_sceneResourceScene ||
       m_sceneInitializationAttempt.candidate || m_activeRecordingGeneration != 0 ||
       m_displacementExitScopeGeneration != 0 || m_resizeFailed)
    {
        return fail("Material revision requires the current scene and no active recording.");
    }

    for(const auto& dependency : dependencies)
    {
        if(!dependency || !dependency->UsesDescriptorHeap(m_resourceDescriptorHeap))
        {
            return fail("Material revision contains an invalid dependency lease or descriptor heap.");
        }
    }

    for(const auto& record : records)
    {
        const unsigned int descriptors[] = {record.textureIndex, record.normalTextureIndex, record.roughnessTextureIndex,
            record.metallicTextureIndex, record.armTextureIndex, record.aoTextureIndex, record.heightTextureIndex};

        for(auto descriptor : descriptors)
        {
            if(descriptor != UINT_MAX && std::none_of(dependencies.begin(), dependencies.end(),
                [descriptor](const Texture::Dependency& lease) { return lease->GetDescriptorIndex() == descriptor; }))
            {
                return fail("Material revision references a texture without retained ownership.");
            }
        }
    }

    auto candidate = m_materialPublication;

    if(!candidate.SetSnapshot(records, true))
    {
        return fail("Invalid material revision count or parameters.");
    }

    if(!m_materialDependencies.SetDesired(std::move(dependencies)))
    {
        return fail("Deferred: material dependency retention capacity unavailable.");
    }

    static_assert(std::is_nothrow_move_assignable_v<decltype(m_materialPublication)>);
    m_materialPublication = std::move(candidate);
    return true;
}

bool Renderer::ValidateSceneMaterialSlots(const Scene& scene, const std::vector<Material*>& materials) const
{
    // Every slot must belong to the published table, which retains textures
    // for all catalog materials, including overrides unused by the fallback.
    for(const auto& object : scene.GetRenderObjects())
    {
        if(!object.mesh)
        {
            continue;
        }

        const auto& metadata = object.mesh->GetMetadata();
        const auto count = object.mesh->IsIndexed() ? object.mesh->GetIndexCount() : object.mesh->GetVertexCount();
        std::string error;

        if(!ValidateMeshMetadata(metadata, count, error))
        {
            Log::Error("Invalid scene mesh metadata: " + error);
            return false;
        }

        for(const auto& [slotID, material] : object.slotMaterials)
        {
            const auto slot = std::find_if(metadata.materialSlots.begin(), metadata.materialSlots.end(),
                [&](const MeshMaterialSlot& entry) { return entry.id == slotID; });

            if(slot == metadata.materialSlots.end() || !material)
            {
                Log::Error("Object contains an unknown or unresolved material slot.");
                return false;
            }
        }

        for(const auto& slot : metadata.materialSlots)
        {
            const auto* material = object.GetSlotMaterial(slot.id);

            if(!material || !material->pipeline || !material->texture ||
                std::find(materials.begin(), materials.end(), material) == materials.end() ||
                material->gpuIndex >= m_materialPublication.desired.size())
            {
                Log::Error("Material slot is absent from the retained scene material table: " + slot.id);
                return false;
            }
        }
    }

    return true;
}

bool Renderer::ResolvePreviewAfterGPUDrain()
{
    m_textureThumbnails.ResolveAfterGPUDrain();
    m_textureThumbnails.Discarded(m_commandRecordingGeneration);

    // Call only after DiscardCommandsAfterGPUDrain succeeds. Unknown execution
    // means actual attachment states are unknowable: recreate instead of rollback.
    if(m_materialPreviewRenderer.IsQuarantined())
    {
        m_materialPreviewRenderer.Shutdown();

        if(!m_materialPreviewRenderer.Initialize(m_graphicsDevice.GetDevice(), m_shaderLibrary,
            m_resourceUploader, m_rtvDescriptorHeap, m_dsvDescriptorHeap, m_resourceDescriptorHeap))
        {
            Log::Error("Preview recovery failed after checked GPU drain; Renderer must stop.");
            return false;
        }
    }
    else if(!m_materialPreviewRenderer.DiscardRecording(m_commandRecordingGeneration))
    {
        return false;
    }

    m_commandRecordingGeneration = 0;
    m_commandSubmissionAttempted = false;
    return true;
}

void Renderer::AdoptSelectionMaskCandidate() noexcept
{
    // Future coordinator only: freshly checked layout/source/lifetime, no
    // intervening mutation or recording. Never replace an active pipeline.
    assert(!m_commandRecordingGeneration && !m_activeRecordingGeneration);
    assert(m_selectionMaskCandidateLifetime == m_sceneResourceLifetime);
    assert(m_selectionMaskCandidate && !m_selectionMaskPass.IsInitialized());
    m_selectionMaskPass.AdoptPrepared(*m_selectionMaskCandidate);
}

bool Renderer::HasPublishedMaterial(const Material& material) const noexcept
{
    return m_sceneResourceLifetime && material.gpuIndex < m_publishedMaterialIdentities.size() &&
        material.gpuIndex < m_materialPublication.desired.size() &&
        m_publishedMaterialIdentities[material.gpuIndex] == &material;
}

bool Renderer::CanEditPendingMaterialBindings(const Scene& scene) const noexcept
{
    return m_sceneResourceScene == &scene && m_sceneResourceLifetime && !m_commandRecordingGeneration &&
        !m_activeRecordingGeneration && !m_commandSubmissionAttempted && !m_sceneInitializationAttempt.candidate &&
        !m_sceneInitializationAttempt.failed && !m_resizeFailed && !m_geometryUpdateFailed &&
        !m_displacementExitScopeGeneration && !HasUnresolvedDisplacementUpdate();
}


Renderer::SelectionPublicationIdentity Renderer::GetSelectionPublicationIdentity(const Scene& scene) const noexcept
{
    if(m_sceneResourceScene != &scene || m_sceneResourceLifetime == 0) return {};
    return {m_sceneResourceLifetime, m_rayTracingScene.m_materialBindingEpoch};
}
