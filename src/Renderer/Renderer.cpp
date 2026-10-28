#include "Renderer/BuiltinEnvironment.h"
#include "Scene/SceneDocument.h"
#include <cassert>
#include "Debug/TransitionTiming.h"
#include <cstring>
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
#include "Renderer/FrameLighting.h"
#include "Renderer/Rtxdi/RtxdiShaderData.h"
#include "Scene/Scene.h"
#include "DemoContent/MirrorTestScene.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <string>
#include <d3d12.h>
#include <dxgi1_4.h>
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

    // UPSCALING.2B: the radical inverse of index in base (Halton sequence), in [0, 1).
    float Halton(std::uint32_t index, std::uint32_t base)
    {
        float result = 0.0f;
        float fraction = 1.0f;

        while(index > 0)
        {
            fraction /= static_cast<float>(base);
            result += fraction * static_cast<float>(index % base);
            index /= base;
        }

        return result;
    }

    // UPSCALING.2B: projection whose pixel centres sample the unjittered image at centre + (x, y) render pixels (x right,
    // y down). NDC y points up, so a sample further down needs the image shifted up. Adding the offset times the w row
    // moves every clip position by the same NDC amount.
    Matrix4 JitterProjection(const Matrix4& projection, float x, float y, unsigned int width, unsigned int height)
    {
        const float clipX = -2.0f * x / static_cast<float>(width);
        const float clipY = 2.0f * y / static_cast<float>(height);
        Matrix4 jittered = projection;

        for(int column = 0; column < 4; ++column)
        {
            jittered.m[0][column] += clipX * projection.m[3][column];
            jittered.m[1][column] += clipY * projection.m[3][column];
        }

        return jittered;
    }

    // The hybrid lighting mode's NRD instances: REBLUR_SPECULAR on the full-resolution RT reflections and REBLUR_DIFFUSE
    // on the half-resolution RTGI, each its own instance. NRD.2 moved this configuration out of NrdDenoiser unchanged,
    // including the shared integration name. It is independent of any path-tracing denoiser configuration.
    constexpr nrd::Identifier HybridNrdReflectionIdentifier = 0;
    constexpr nrd::Identifier HybridNrdGIIdentifier = 1;
    constexpr std::uint32_t HybridNrdQueuedFrames = 2;

    // NRD.4B path-traced REBLUR: one REBLUR_DIFFUSE_SPECULAR instance, SDK defaults except the hit-distance parameters
    // the extraction and prepare pass normalize with (NrdPathTracePrepareConstants, PathTraceReference.hlsl), area 3x3
    // hit-distance reconstruction (a 1 spp pixel often has no sample of one lobe; the default pre-pass radii stay on, as
    // reconstruction requires) and diffuse material comparison (no diffuse blending across the dielectric/metal ID).
    constexpr nrd::Identifier PathTraceNrdIdentifier = 0;
    // The prepare pass's no-surface view Z is 2 * range + 1; surfaces beyond the range are left undenoised.
    constexpr float PathTraceNrdDenoisingRange = 1000.0f;

    NrdDenoiser::Description PathTraceNrdDescription()
    {
        NrdDenoiser::Description description;
        description.name = "PathTraceREBLUR";
        description.denoisers.push_back({PathTraceNrdIdentifier, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR});
        description.queuedFrameCount = HybridNrdQueuedFrames;
        // ER.1 Phase 3.1: retained while inactive and evicted only after its last use completed.
        description.externallySynchronized = true;
        return description;
    }

    nrd::ReblurSettings PathTraceNrdSettings()
    {
        const NrdPathTracePrepareConstants prepare{};
        nrd::ReblurSettings settings;
        settings.hitDistanceParameters.A = prepare.hitDistanceA;
        settings.hitDistanceParameters.B = prepare.hitDistanceB;
        settings.hitDistanceParameters.C = prepare.hitDistanceC;
        settings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3;
        settings.minMaterialForDiffuse = 0.0f;
        return settings;
    }

    // NRD.RELAX.3 diagnostic RELAX: one RELAX_DIFFUSE_SPECULAR instance of its own. SDK defaults (nrd::RelaxSettings{})
    // except:
    //   - hit-distance reconstruction AREA_3X3: Engine's vertex-0 lobe selection is probabilistic, so a pixel can lack
    //     one lobe's sample ("hitT = 0 for the skipped lobe"); NRDSettings.h requires reconstruction then, and the default
    //     non-zero pre-pass radii it also requires stay on. Engine's selection does not meet the [1/4, 3/4] + Bayer
    //     condition that guarantees a 3x3 sample (NRDRelax0.md section 9), so this is the conservative setting, not a
    //     guarantee.
    //   - antilag off (accelerationAmount 0, resetAmount 0): the SDK README's instruction for an initial integration.
    //   - NRD.RELAX.5D: 5 A-trous iterations, set explicitly. NRD.RELAX.5C's 4 measured better and cost about 10 % less
    //     (NRDRelax5B.md, NRDRelax5C.md), but showed low-frequency noise across surfaces during and just after camera
    //     movement in the Editor (NRDRelax5D.md).
    // Nothing else is tuned; REBLUR's settings are not reused.
    constexpr nrd::Identifier PathTraceRelaxIdentifier = 0;

    NrdDenoiser::Description PathTraceRelaxDescription()
    {
        NrdDenoiser::Description description;
        description.name = "PathTraceRELAX";
        description.denoisers.push_back({PathTraceRelaxIdentifier, nrd::Denoiser::RELAX_DIFFUSE_SPECULAR});
        description.queuedFrameCount = HybridNrdQueuedFrames;
        // ER.1 Phase 3.1: retained while inactive and evicted only after its last use completed.
        description.externallySynchronized = true;
        return description;
    }

    nrd::RelaxSettings PathTraceRelaxSettings()
    {
        nrd::RelaxSettings settings;
        settings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3;
        settings.antilagSettings.accelerationAmount = 0.0f;
        settings.antilagSettings.resetAmount = 0.0f;
        settings.atrousIterationNum = 5;
        return settings;
    }

    constexpr nrd::Identifier PathTraceHybridDiffuseIdentifier = 17;
    constexpr nrd::Identifier PathTraceHybridSpecularIdentifier = 29;

    NrdDenoiser::Description PathTraceHybridDescription()
    {
        NrdDenoiser::Description description;
        description.name = "PathTraceHybridDiagnostic";
        description.denoisers = {
            {PathTraceHybridDiffuseIdentifier, nrd::Denoiser::RELAX_DIFFUSE},
            {PathTraceHybridSpecularIdentifier, nrd::Denoiser::REBLUR_SPECULAR}};
        description.queuedFrameCount = HybridNrdQueuedFrames;
        return description;
    }

    NrdDenoiser::Description HybridNrdReflectionDescription()
    {
        // ER.1 Phase 3.1: retained across path tracing and evicted only after its last use completed.
        return {"ReflectionREBLUR", {{HybridNrdReflectionIdentifier, nrd::Denoiser::REBLUR_SPECULAR}},
            HybridNrdQueuedFrames, false, true};
    }

    NrdDenoiser::Description HybridNrdGIDescription()
    {
        return {"ReflectionREBLUR", {{HybridNrdGIIdentifier, nrd::Denoiser::REBLUR_DIFFUSE}}, HybridNrdQueuedFrames, false,
            true};
    }

    nrd::ReblurSettings HybridNrdReflectionSettings()
    {
        nrd::ReblurSettings settings{};

        settings.hitDistanceParameters.A = 3.0f;
        settings.hitDistanceParameters.B = 0.1f;
        settings.hitDistanceParameters.C = 20.0f;
        settings.maxAccumulatedFrameNum = 8;

        settings.checkerboardMode = nrd::CheckerboardMode::OFF;
        settings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::OFF;

        // Temporary A/B test: skip specular reconstruction after history rejection.
        // NRD permits zero reconstructed frames; keep the remaining History Fix work.
        constexpr bool kDisableSpecularHistoryReconstruction = true;

        if(kDisableSpecularHistoryReconstruction)
        {
            settings.historyFixFrameNum = 0;
        }

        return settings;
    }

    // The same hit-distance normalization as NrdReflectionPrepare.hlsl.
    nrd::ReblurSettings HybridNrdGISettings()
    {
        nrd::ReblurSettings settings{};

        settings.hitDistanceParameters.A = 3.0f;
        settings.hitDistanceParameters.B = 0.1f;
        settings.hitDistanceParameters.C = 20.0f;

        settings.maxAccumulatedFrameNum = 30;
        settings.diffusePrepassBlurRadius = 30.0f;
        settings.checkerboardMode = nrd::CheckerboardMode::OFF;
        settings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::OFF;
        return settings;
    }

    // A reflecting model matrix reverses triangle winding on screen (FOUNDATION.6).
    bool HasMirroredWinding(const Matrix4& model)
    {
        const float determinant = model.m[0][0] * (model.m[1][1] * model.m[2][2] - model.m[1][2] * model.m[2][1]) -
            model.m[0][1] * (model.m[1][0] * model.m[2][2] - model.m[1][2] * model.m[2][0]) +
            model.m[0][2] * (model.m[1][0] * model.m[2][1] - model.m[1][1] * model.m[2][0]);
        return determinant < 0.0f;
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
        m_referenceRayCounts.Abort(materialSlot, uploadGeneration);
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
        InvalidateSceneLighting(SceneLightingInvalidation::Displacement);
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
    InvalidateSceneLighting(SceneLightingInvalidation::Displacement);
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
    const std::filesystem::path& assetRoot, std::span<const std::byte> packagedShaders,
    const HDRImageData* initialEnvironment, std::string environmentIdentity, bool deferEditorResources,
    std::span<const std::byte> packagedMaterialTables)
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

    m_dlssFailure.clear();
    m_dlssRetryPending = false;
    m_dlssPublishedState.clear();
    m_dlssPublishedDetail.clear();
    m_packagedRuntime = !packagedShaders.empty();
    m_sceneResourceLifetime = 0;
    m_sceneResourceScene = nullptr;

    // Diagnostics only: the existing CPU frame-hitch report is logged when ENGINE_FRAME_HITCH_LOGGING is exactly "1".
    // A longer value does not fit the buffer and returns its required size, so it leaves the report off.
    char frameHitchLogging[2]{};
    m_frameHitchDiagnostics.loggingEnabled = IsFrameHitchDiagnosticsEnabled() &&
        GetEnvironmentVariableA("ENGINE_FRAME_HITCH_LOGGING", frameHitchLogging, sizeof(frameHitchLogging)) == 1 &&
        frameHitchLogging[0] == '1';

    // NRD RELAX diagnostics, each applied on top of PathTraceRelaxSettings() into the one override that regular RELAX
    // and the hybrid's RELAX diffuse share; every other RELAX setting stays production.
    //   R.2 ENGINE_NRD_RELAX_ANTILAG exactly "1": the SDK's default antilag (nrd::RelaxAntilagSettings{}) in place of
    //   the production off. R.8 exactly "2": NVIDIA RTXPT's antilag (Rtxpt/NRD/NrdConfig.cpp, NRD 4.17.3), acceleration
    //   0.55, reset 0.5, spatial sigma 2.5, temporal sigma 0.3; its other RELAX settings are not taken. "0", unset or
    //   anything else: production.
    //   R.5 ENGINE_NRD_RELAX_DIFFUSE_HISTORY exactly "16": diffuseMaxAccumulatedFrameNum 16 in place of 30 (still above
    //   diffuseMaxFastAccumulatedFrameNum 6 and historyFixFrameNum 3). "30", unset or anything else: production.
    //   R.7 ENGINE_NRD_RELAX_DIFFUSE_PREPASS exactly "0": diffusePrepassBlurRadius 0 in place of 30 (specular stays 50,
    //   reconstruction stays AREA_3X3). An approved diagnostic deviation: NRD 4.17.3 documents both prepass radii as
    //   required with probabilistic lobe selection. Engine's rough-surface diffuse has no probabilistic zeros, but its
    //   indirect diffuse on delta-specular surfaces with a diffuse layer does. "30", unset or anything else: production.
    {
        nrd::RelaxSettings relaxSettings = PathTraceRelaxSettings();
        std::string selectors;

        char relaxAntilag[2]{};

        const bool antilagRead =
            GetEnvironmentVariableA("ENGINE_NRD_RELAX_ANTILAG", relaxAntilag, sizeof(relaxAntilag)) == 1;

        if (antilagRead && relaxAntilag[0] == '1')
        {
            relaxSettings.antilagSettings = nrd::RelaxAntilagSettings{};
            selectors += " ENGINE_NRD_RELAX_ANTILAG=1 (SDK defaults)";
        }
        else if (antilagRead && relaxAntilag[0] == '2')
        {
            relaxSettings.antilagSettings.accelerationAmount = 0.55f;
            relaxSettings.antilagSettings.resetAmount = 0.5f;
            relaxSettings.antilagSettings.spatialSigmaScale = 2.5f;
            relaxSettings.antilagSettings.temporalSigmaScale = 0.3f;
            selectors += " ENGINE_NRD_RELAX_ANTILAG=2 (RTXPT)";
        }

        char relaxDiffuseHistory[3]{};

        if (GetEnvironmentVariableA("ENGINE_NRD_RELAX_DIFFUSE_HISTORY", relaxDiffuseHistory,
            sizeof(relaxDiffuseHistory)) == 2 && std::string(relaxDiffuseHistory) == "16")
        {
            relaxSettings.diffuseMaxAccumulatedFrameNum = 16;
            selectors += " ENGINE_NRD_RELAX_DIFFUSE_HISTORY";
        }

        char relaxDiffusePrepass[2]{};

        if (GetEnvironmentVariableA("ENGINE_NRD_RELAX_DIFFUSE_PREPASS", relaxDiffusePrepass,
            sizeof(relaxDiffusePrepass)) == 1 && relaxDiffusePrepass[0] == '0')
        {
            relaxSettings.diffusePrepassBlurRadius = 0.0f;
            selectors += " ENGINE_NRD_RELAX_DIFFUSE_PREPASS";
        }

        if (!selectors.empty())
        {
            m_nrdRelaxSettingsOverride = relaxSettings;
        }

        Log::Info("NRD RELAX: diffusePrepassBlurRadius " + std::to_string(relaxSettings.diffusePrepassBlurRadius) +
            ", specularPrepassBlurRadius " + std::to_string(relaxSettings.specularPrepassBlurRadius) +
            ", diffuseMaxAccumulatedFrameNum " +
            std::to_string(relaxSettings.diffuseMaxAccumulatedFrameNum) + ", specularMaxAccumulatedFrameNum " +
            std::to_string(relaxSettings.specularMaxAccumulatedFrameNum) + ", antilag accelerationAmount " +
            std::to_string(relaxSettings.antilagSettings.accelerationAmount) + ", resetAmount " +
            std::to_string(relaxSettings.antilagSettings.resetAmount) + ", spatialSigmaScale " +
            std::to_string(relaxSettings.antilagSettings.spatialSigmaScale) + ", temporalSigmaScale " +
            std::to_string(relaxSettings.antilagSettings.temporalSigmaScale) +
            (selectors.empty() ? " (production)" : " (" + selectors.substr(1) + ")"));
    }

    // NRD RELAX R.9A diagnostics: ENGINE_NRD_RELAX_HISTORY_CONFIDENCE exactly "1" or "2" enables NRD history confidence
    // for regular RELAX with a constant diffuse confidence of 1 or 0.1 (specular 1). Anything else, or unset, is
    // production (confidence disabled). The hybrid's instance never enables it.
    char relaxHistoryConfidence[2]{};

    if (GetEnvironmentVariableA("ENGINE_NRD_RELAX_HISTORY_CONFIDENCE", relaxHistoryConfidence,
        sizeof(relaxHistoryConfidence)) == 1 && (relaxHistoryConfidence[0] == '1' || relaxHistoryConfidence[0] == '2'))
    {
        m_nrdRelaxHistoryConfidence = static_cast<std::uint32_t>(relaxHistoryConfidence[0] - '0');
    }

    // NRD RELAX R.9B diagnostics: ENGINE_NRD_RELAX_CONFIDENCE_PROBES exactly "1" generates the paired-visibility history
    // confidence for signal views 4, 5 and 6. It is bound to NRD only under ENGINE_NRD_RELAX_GENERATED_CONFIDENCE.
    // Anything else, or unset, is off.
    char confidenceProbes[2]{};
    m_nrdConfidenceProbesEnabled = GetEnvironmentVariableA("ENGINE_NRD_RELAX_CONFIDENCE_PROBES", confidenceProbes,
        sizeof(confidenceProbes)) == 1 && confidenceProbes[0] == '1';

    // NRD RELAX R.9C diagnostics: ENGINE_NRD_RELAX_GENERATED_CONFIDENCE exactly "1" binds the probe confidence as regular
    // RELAX's diffuse history confidence (specular 1). It enables the probes, and replaces the R.9A constant confidence
    // rather than combining with it. Anything else, or unset, is off.
    char generatedConfidence[2]{};
    m_nrdRelaxGeneratedConfidence = GetEnvironmentVariableA("ENGINE_NRD_RELAX_GENERATED_CONFIDENCE",
        generatedConfidence, sizeof(generatedConfidence)) == 1 && generatedConfidence[0] == '1';

    if(m_nrdRelaxGeneratedConfidence)
    {
        if(m_nrdRelaxHistoryConfidence != 0)
        {
            Log::Warning("NRD RELAX history confidence: ENGINE_NRD_RELAX_HISTORY_CONFIDENCE=" +
                std::to_string(m_nrdRelaxHistoryConfidence) + " ignored; ENGINE_NRD_RELAX_GENERATED_CONFIDENCE=1 "
                "takes precedence (the sources are exclusive).");
            m_nrdRelaxHistoryConfidence = 0;
        }

        m_nrdConfidenceProbesEnabled = true;
        Log::Info("NRD RELAX history confidence: generated (ENGINE_NRD_RELAX_GENERATED_CONFIDENCE=1), diffuse from the "
            "confidence probes (1.0 on frames without them), specular 1.0; regular RELAX only");
    }
    else
    {
        Log::Info(m_nrdRelaxHistoryConfidence == 0 ? std::string("NRD RELAX history confidence: disabled (production)") :
            std::string("NRD RELAX history confidence: enabled, diffuse ") +
            (m_nrdRelaxHistoryConfidence == 1 ? "1.0" : "0.1 (R16F 0.0999756)") +
            ", specular 1.0 (ENGINE_NRD_RELAX_HISTORY_CONFIDENCE=" + std::to_string(m_nrdRelaxHistoryConfidence) + ")");
    }

    // R.9F: strict single-character selector; never enables generated confidence or probe allocation by itself.
    char confidenceStrength[2]{};
    const DWORD confidenceStrengthLength = GetEnvironmentVariableA("ENGINE_NRD_RELAX_CONFIDENCE_STRENGTH",
        confidenceStrength, sizeof(confidenceStrength));

    if(confidenceStrengthLength != 0)
    {
        if(confidenceStrengthLength != 1 || (confidenceStrength[0] != '1' && confidenceStrength[0] != '2' &&
            confidenceStrength[0] != '4' && confidenceStrength[0] != '6'))
        {
            Log::Warning("NRD RELAX confidence strength: unsupported ENGINE_NRD_RELAX_CONFIDENCE_STRENGTH; "
                "expected exactly 1, 2, 4 or 6; override ignored.");
        }
        else if(confidenceStrength[0] != '1' && !m_nrdRelaxGeneratedConfidence)
        {
            Log::Warning("NRD RELAX confidence strength: override ignored; "
                "ENGINE_NRD_RELAX_GENERATED_CONFIDENCE=1 is required.");
        }
        else
        {
            m_nrdRelaxConfidenceStrength = static_cast<std::uint32_t>(confidenceStrength[0] - '0');
        }
    }

    Log::Info("NRD RELAX confidence strength: " + std::to_string(m_nrdRelaxConfidenceStrength) +
        (m_nrdRelaxConfidenceStrength == 1 ? " (original R.9C mapping)" :
        " (filtered confidence exponent; View 5 displays strengthened confidence, NRD samples it bilinearly)"));

    Log::Info(m_nrdConfidenceProbesEnabled ?
        std::string("NRD RELAX confidence probes: enabled, one probe per 2x2 "
            "pixels, records 32 B, signal RG32F, confidence R16F, " +
            std::to_string(NrdHistoryConfidencePass::RaysPerProbe) + " shadow rays per probe, " +
            std::to_string(NrdHistoryConfidencePass::FilterPasses) + " filter passes; " +
            (m_nrdRelaxGeneratedConfidence ? "bound to NRD as IN_DIFF_CONFIDENCE" : "not bound to NRD")) :
        std::string("NRD RELAX confidence probes: disabled"));

    // NRD RELAX R.4 diagnostics: ENGINE_NRD_RELAX_SIGNAL_VIEW exactly "1" (RTXDI direct diffuse), "2" (RELAX's diffuse
    // input) or "3" (RELAX's denoised diffuse) presents that signal in place of RELAX's production resolve. R.9B: "4" (raw
    // probe visibility change), "5" (probe confidence) and (R.9B.2) "6" (probe state), shown only while the probes run.
    // Anything else, or unset, is production.
    char relaxSignalView[2]{};

    if (GetEnvironmentVariableA("ENGINE_NRD_RELAX_SIGNAL_VIEW", relaxSignalView, sizeof(relaxSignalView)) == 1 &&
        relaxSignalView[0] >= '1' && relaxSignalView[0] <= '6')
    {
        m_nrdRelaxSignalView = static_cast<std::uint32_t>(relaxSignalView[0] - '0');
        constexpr const char* signalNames[] = {"", "A, RTXDI direct diffuse", "B, RELAX diffuse input",
            "C, RELAX denoised diffuse", "4, raw probe visibility change (previous-frame space)",
            "5, probe history confidence (previous-frame space)",
            "6, probe state before filtering (previous-frame space)"};
        Log::Info(std::string("NRD RELAX signal view: ") + signalNames[m_nrdRelaxSignalView] +
            " (ENGINE_NRD_RELAX_SIGNAL_VIEW)");
    }

    m_applicationWidth = width;
    m_applicationHeight = height;

    bool initialPresetSupported = true;
    const SceneResolution initialResolution = ResolveSceneResolution(width, height, m_renderScale,
        initialPresetSupported);
    m_sceneWidth = initialResolution.renderWidth;
    m_sceneHeight = initialResolution.renderHeight;
    m_outputWidth = initialResolution.outputWidth;
    m_outputHeight = initialResolution.outputHeight;

    if (!InitializeDevice())
    {
        return false;
    }

    // UPSCALING.3A: capability only; any DLSS failure is reported, never fatal. The packaged runtime does not use DLSS.
    if(!m_packagedRuntime)
    {
        m_dlssBackend.Initialize(m_graphicsDevice.GetDevice());
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

    SelectPathTraceLibraries();

    // Packaged bytecode never compiles, so only the Editor prepares DXR libraries asynchronously. Without
    // the worker, lighting modes keep their synchronous first-use compilation.
    m_lightingModeTransition.Reset();
    m_preparedDxrLibraries.clear();
    m_denoiserPreparation.clear();
    m_sharcPreparation.clear();
    m_shaderCompileWorkerRunning = false;

    if(!m_packagedRuntime)
    {
        m_shaderCompileWorker = std::make_unique<ShaderLibraryCompileWorker>();
        m_shaderCompileWorkerRunning = m_shaderCompileWorker->Start();

        if(!m_shaderCompileWorkerRunning)
        {
            Log::Warning("DXR shader compile worker unavailable; lighting modes compile synchronously.");
        }

        // ER.1 Phase 4: DXR state objects and shader tables are created on their own worker once the libraries are ready.
        m_pipelineWorker = std::make_unique<RayTracingPipelineWorker>();
        m_pipelineWorkerRunning = m_shaderCompileWorkerRunning && m_pipelineWorker->Start();

        if(m_shaderCompileWorkerRunning && !m_pipelineWorkerRunning)
        {
            Log::Warning("DXR pipeline worker unavailable; DXR pipelines are created on the render thread.");
        }
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

    if(!m_packagedRuntime && !m_editorGridPass.Initialize(device, m_shaderLibrary))
    {
        Log::Error("Failed to initialize editor viewport grid pass.");
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

    // Optional for Legacy: unavailable tables only disable the path-traced reference mode.
    LoadMaterialTables(packagedMaterialTables);

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

    // NRD.2: the hybrid mode's NRD instances are created on its first NRD frame (EnsureHybridNrdDenoisers).

    if (!m_shadowDenoisePass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap, m_sceneWidth, m_sceneHeight))
    {
        Log::Error("Failed to init shadow denoise pass.");

        return false;
    }

    if(m_packagedRuntime)
    {
        const bool none = environmentIdentity == SceneDocument::NoneEnvironmentReference;
        m_availableEnvironments = {{none ? "None" : (initialEnvironment ? "Packaged scene HDRI" : "Embedded neutral environment"),
            {}, none ? SceneDocument::NoneEnvironmentReference : ""}};
    }
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

    if(!m_finalSceneColor.Initialize(device, m_outputWidth, m_outputHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
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

    if(m_debugUIInitialized)
    {
        const DlssStatus& dlss = m_dlssBackend.GetStatus();
        m_debugUI.SetDlssReadout(DlssAvailabilityName(dlss.availability), dlss.detail);
    }

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

    if(!m_packagedRuntime && !deferEditorResources && !InitializeEditorResources())
    {
        return false;
    }

    m_resizeFailed = false;
    return true;
}

bool Renderer::InitializeEditorResources()
{
    if(m_editorResourcesInitialized)
    {
        return true;
    }

    if(m_packagedRuntime || !m_graphicsDevice.GetDevice() || m_commandRecordingGeneration != 0)
    {
        return false;
    }

    if(!m_materialPreviewRenderer.Initialize(
            m_graphicsDevice.GetDevice(),
            m_shaderLibrary,
            m_resourceUploader,
            m_rtvDescriptorHeap,
            m_dsvDescriptorHeap,
            m_resourceDescriptorHeap))
    {
        Log::Error("Failed to initialize material preview resources.");
        return false;
    }

    m_textureThumbnails.Configure(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap);
    m_editorResourcesInitialized = true;
    Log::Info("Editor preview resources initialized.");
    return true;
}

void Renderer::SelectPathTraceLibraries()
{
    char overrideBuffer[32]{};
    const DWORD overrideLength = GetEnvironmentVariableA("ENGINE_PATH_TRACE_LIBRARIES", overrideBuffer,
        sizeof(overrideBuffer));
    std::string libraryOverride;

    // A value too long for the buffer is not copied; no valid value is that long, so it is reported as invalid.
    if(overrideLength >= sizeof(overrideBuffer))
    {
        libraryOverride = "(value too long)";
    }
    else
    {
        libraryOverride.assign(overrideBuffer, overrideLength);
    }

    PathTraceLibraryCapabilities capabilities{};
    capabilities.rayTracing = m_graphicsDevice.SupportsRayTracing() && m_graphicsDevice.GetRayTracingDevice();
    capabilities.shaderModel69 = m_graphicsDevice.GetHighestShaderModel() >= D3D_SHADER_MODEL_6_9;
    capabilities.rayTracingTier12 = m_graphicsDevice.GetRayTracingTier() >= D3D12_RAYTRACING_TIER_1_2;
    capabilities.serActuallyReorders = m_graphicsDevice.ShaderExecutionReorderingActuallyReorders();
    capabilities.packagedRuntime = m_packagedRuntime;

    const PathTraceLibrarySelection selection = SelectPathTraceLibraryVariant(libraryOverride, capabilities);

    if(!m_pathTraceReferencePass.SelectLibraryVariant(selection.variant))
    {
        Log::Warning(std::string("Path-tracing library variant ") + PathTraceLibraryVariantName(selection.variant) +
            " selected after path-tracing libraries were prepared; keeping " +
            PathTraceLibraryVariantName(m_pathTraceReferencePass.GetLibraryVariant()) + ".");
        return;
    }

    const std::string message = std::string("Path-tracing libraries: ") +
        PathTraceLibraryVariantName(selection.variant) + " (" + selection.reason + ").";

    if(selection.warning)
    {
        Log::Warning(message);
    }
    else
    {
        Log::Info(message);
    }
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

void Renderer::SetRenderScale(RenderScale scale, bool eligible, bool retry, bool normalRequest)
{
    if(scale != m_requestedRenderScale || eligible != m_dlssEligible)
        m_dlssFailure.clear();
    m_requestedRenderScale = scale;
    m_dlssEligible = eligible;
    m_dlssNormalRequest = normalRequest;
    m_dlssRetryPending |= retry;
}

bool Renderer::ResizeScene(unsigned int width, unsigned int height)
{
    if(m_resizeFailed || m_activeRecordingGeneration != 0 || m_displacementExitScopeGeneration != 0)
    {
        return false;
    }

    if(width == 0 || height == 0) return true;
    if(width != m_outputWidth || height != m_outputHeight) m_dlssFailure.clear();
    const auto effectiveScale = [&]()
    {
        return IsDlssRenderScale(m_requestedRenderScale) &&
            (!m_dlssEligible || m_dlssBackend.GetStatus().availability != DlssAvailability::Available || !m_dlssFailure.empty()) ?
            RenderScale::Native : m_requestedRenderScale;
    };
    if(width == m_outputWidth && height == m_outputHeight && effectiveScale() == m_renderScale && !m_dlssRetryPending)
    {
        PublishDlssReadout();
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

    if(m_dlssRetryPending)
    {
        m_dlssBackend.ReleaseFeature();
        if(m_dlssBackend.GetStatus().availability != DlssAvailability::Available)
        {
            m_dlssBackend.Shutdown();
            m_dlssBackend.Initialize(m_graphicsDevice.GetDevice());
        }
        m_dlssFailure.clear();
        m_dlssRetryPending = false;
    }
    bool presetSupported = true;
    RenderScale effective = effectiveScale();
    const SceneResolution resolution = ResolveSceneResolution(width, height, effective, presetSupported);
    if(!presetSupported)
    {
        FailDlss("Optimal settings", m_dlssBackend.GetFeatureStatus().detail);
        effective = RenderScale::Native;
    }

    if(!ResizeSceneResources(resolution))
    {
        return false;
    }

    m_renderScale = effective;
    m_dlssActiveLastFrame = false;
    m_dlssEvaluatedLastFrame = false;
    m_dlssPresetUnsupported = !presetSupported;
    m_resizeFailed = false;
    PublishDlssReadout();
    return true;
}

std::optional<DlssQualityPreset> Renderer::GetDlssPreset(RenderScale scale) noexcept
{
    switch(scale)
    {
    case RenderScale::DlssQuality:
        return DlssQualityPreset::Quality;
    case RenderScale::DlssBalanced:
        return DlssQualityPreset::Balanced;
    case RenderScale::DlssPerformance:
        return DlssQualityPreset::Performance;
    default:
        return std::nullopt;
    }
}

Renderer::SceneResolution Renderer::ResolveSceneResolution(unsigned int outputWidth, unsigned int outputHeight,
    RenderScale scale, bool& presetSupported)
{
    presetSupported = true;

    if(scale == RenderScale::Native)
    {
        return {outputWidth, outputHeight, outputWidth, outputHeight};
    }

    // UPSCALING.3D: a preset renders at NGX's optimal extent for this output extent.
    const std::optional<DlssQualityPreset> preset = GetDlssPreset(scale);

    if(preset)
    {
        unsigned int renderWidth = 0;
        unsigned int renderHeight = 0;
        presetSupported = m_dlssBackend.QueryOptimalRenderExtent(*preset, outputWidth, outputHeight, renderWidth,
            renderHeight);

        if(presetSupported)
        {
            return {renderWidth, renderHeight, outputWidth, outputHeight};
        }

        return {outputWidth, outputHeight, outputWidth, outputHeight};
    }

    // round(0.75 * extent), half up, in integers.
    const auto scaled = [](unsigned int extent)
    {
        return (std::max)(1u, (extent * 3u + 2u) / 4u);
    };

    return {scaled(outputWidth), scaled(outputHeight), outputWidth, outputHeight};
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

bool Renderer::ResizeSceneResources(const SceneResolution& resolution)
{
    // No coherent scene extent is published while resources are partially recreated.
    m_sceneWidth = 0;
    m_sceneHeight = 0;
    m_outputWidth = 0;
    m_outputHeight = 0;

    // Everything below except the final scene image is sized from the render extent.
    const unsigned int width = resolution.renderWidth;
    const unsigned int height = resolution.renderHeight;

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

    // NRD.2: instances that do not exist yet are created at the new extent when first needed.
    if(m_nrdReflectionDenoiser.IsInitialized() &&
        !m_nrdReflectionDenoiser.Resize(m_resourceDescriptorHeap, width, height))
    {
        Log::Error("Failed to resize NRD reflection denoiser.");
        return false;
    }

    if(m_nrdGIDenoiser.IsInitialized() && !m_nrdGIDenoiser.Resize(m_resourceDescriptorHeap, giWidth, giHeight))
    {
        Log::Error("Failed to resize NRD diffuse GI denoiser.");
        return false;
    }

    m_hybridNrdCreationFailed = false;
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

    if(!m_finalSceneColor.Initialize(m_graphicsDevice.GetDevice(), resolution.outputWidth, resolution.outputHeight,
                                    DXGI_FORMAT_R8G8B8A8_UNORM, m_rtvDescriptorHeap, m_resourceDescriptorHeap))
    {
        Log::Error("Failed to resize final scene color.");
        return false;
    }

    // UPSCALING.2C: the presentation resolve's output-extent targets exist only while the extents differ.
    m_presentationColor.Shutdown();
    m_presentationSelectionMask.Shutdown();
    m_presentationDepth.Shutdown();

    const bool presentationResolve = width != resolution.outputWidth || height != resolution.outputHeight;

    if(presentationResolve)
    {
        ID3D12Device* const device = m_graphicsDevice.GetDevice();
        const bool allocated =
            m_presentationColor.Initialize(device, resolution.outputWidth, resolution.outputHeight,
                DXGI_FORMAT_R16G16B16A16_FLOAT, m_rtvDescriptorHeap, m_resourceDescriptorHeap, {0.0f, 0.0f, 0.0f, 1.0f},
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
            m_presentationSelectionMask.Initialize(device, resolution.outputWidth, resolution.outputHeight,
                DXGI_FORMAT_R8_UNORM, m_rtvDescriptorHeap, m_resourceDescriptorHeap) &&
            m_presentationDepth.Initialize(device, resolution.outputWidth, resolution.outputHeight, m_dsvDescriptorHeap,
                m_resourceDescriptorHeap) &&
            m_presentationResolvePass.Initialize(device, m_shaderLibrary);

        if(!allocated)
        {
            Log::Error("Failed to create the presentation resolve's output targets.");
            return false;
        }
    }

    // The reference path tracer is optional: a failed resize disables it but never fails Legacy.
    if(m_pathTraceReferencePass.IsInitialized() &&
        !m_pathTraceReferencePass.Resize(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, width, height))
    {
        m_pathTraceStatus.availability = PathTraceReferenceAvailability::Unavailable;
        m_pathTraceStatus.unavailableReason = "Path tracing resources could not be resized.";
        Log::Error(m_pathTraceStatus.unavailableReason);
    }

    m_pathTraceStatus.resourceBytes = m_pathTraceReferencePass.GetResourceBytes();

    // RTXPT D.2B temporal history is sized for the old extent; the next temporal frame creates it again.
    m_pathTraceTemporalPass.ReleaseHistory();
    m_pathTraceSpatialPass.ReleaseIntermediate();
    // NRD.3: sized for the old extent; the next diagnostic frame creates them again. NRD.RELAX.2: the RELAX preparation too
    // (the raw diffuse hit distance went with the path tracer's resize).
    m_nrdPathTracePreparePass.ReleaseTextures();
    m_nrdRelaxPreparePass.ReleaseTextures();
    // NRD.4B: the GPU is idle here; the next REBLUR frame creates the instance again (CLEAR_AND_RESTART). A new extent
    // also retries a failed creation.
    m_nrdPathTraceDenoiser.Shutdown();
    m_nrdPathTraceValidationTexture.Shutdown();
    m_nrdPathTraceCreationFailed = false;
    m_nrdPathTraceHistoryValid = false;
    m_nrdPathTraceExecutedLastFrame = false;
    // NRD.RELAX.3: the same for the diagnostic RELAX instance (recreated with CLEAR_AND_RESTART by its next frame).
    m_nrdRelaxDenoiser.Shutdown();
    m_nrdRelaxResolvedTexture.Shutdown();
    m_nrdRelaxConfidenceOne.Shutdown();
    m_nrdRelaxConfidenceLow.Shutdown();
    m_nrdConfidencePass.Shutdown();
    m_nrdRelaxCreationFailed = false;
    m_nrdRelaxHistoryValid = false;
    m_nrdRelaxExecutedLastFrame = false;
    // HYBRID.3: the same for the hybrid diagnostic instance and its inputs.
    m_nrdHybridDenoiser.Shutdown();
    m_nrdHybridPreparePass.ReleaseTextures();
    m_nrdHybridCreationFailed = false;
    m_nrdHybridHistoryValid = false;
    m_nrdHybridExecutedLastFrame = false;
    m_nrdHybridDispatchedThisFrame = false;

    // Recreated only if a PT mode already allocated it; a new extent also clears an earlier failure.
    InvalidatePreviousGBufferHistory();
    m_previousGBufferReadSlot = 0;
    m_previousGBufferHistoryFailed = false;

    if(!m_previousGBufferHistory.Resize(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, width, height))
    {
        m_previousGBufferHistoryFailed = true;
        Log::Error("Previous-frame G-buffer history could not be resized; path tracing continues without it.");
    }

    m_rtxdiResources.InvalidateHistory();
    m_rtxdiResourcesFailed = false;

    if(!m_rtxdiResources.Resize(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, width, height))
    {
        m_rtxdiResourcesFailed = true;
        Log::Error("RTXDI resources could not be resized.");
    }

    m_pathTraceResetPending = true;
    m_cameraJitterPhase = 0;
    m_upscalerHistoryResetPending = true;
    // UPSCALING.3B: sized for the old render extent; the next prepared frame creates them again.
    m_dlssInputPreparationPass.ReleaseTextures();
    // UPSCALING.3C: the GPU is idle here (ResizeScene drained it); the next DLSS frame creates the feature again.
    m_dlssBackend.ReleaseFeature();
    // UPSCALING.3C.4: output-extent overlay targets; the next jittered Editor frame creates them again.
    m_overlayDepth.Shutdown();
    m_overlaySelectionMask.Shutdown();
    m_dlssEvaluatedLastFrame = false;

    m_sceneWidth = width;
    m_sceneHeight = height;
    m_outputWidth = resolution.outputWidth;
    m_outputHeight = resolution.outputHeight;
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

    selectedMaterial.pipeline->Bind(commandList, HasMirroredWinding(objectConstants.model));

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
    D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress,
    bool depthPrepass)
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

    const bool mirroredWinding = HasMirroredWinding(objectConstants.model);

    if(depthPrepass)
    {
        m_selectionMaskPass.BindDepthPrepass(commandList, mirroredWinding);
    }
    else
    {
        m_selectionMaskPass.Bind(commandList, mirroredWinding);
    }

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

        InvalidateSceneLighting(SceneLightingInvalidation::SceneLifetime);
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
    InvalidateSceneLighting(SceneLightingInvalidation::SceneRelease);
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

    InvalidateSceneLighting(SceneLightingInvalidation::SceneRelease);
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
    m_referenceRayCounts.Invalidate();
    m_debugUI.SetReferenceRayCounts(m_referenceRayCounts.Get());
    ++m_pathTraceStatus.visibilityTimingEpoch;
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
    m_hasPreviousGILocalLights = false;
    m_hasPreviousNrdCamera = false;

    m_nrdExecutedLastFrame = false;
    m_nativeReflectionExecutedLastFrame = false;
    m_aoExecutedLastFrame = false;

    m_shadowSampleIndex = 0;

    m_reflectionSampleIndex = 0;
    m_nrdFrameIndex = 0;

    m_pathTraceResetPending = true;
    m_pathTraceSequenceOrigin = m_gpuProfilerFrameSerial;
    m_cameraJitterPhase = 0;
    m_upscalerHistoryResetPending = true;
    InvalidatePreviousGBufferHistory();
    m_rtxdiResources.InvalidateHistory();
}

void Renderer::LoadMaterialTables(std::span<const std::byte> packagedMaterialTables)
{
    std::vector<std::byte> developmentBytes;
    std::span<const std::byte> bytes = packagedMaterialTables;
    std::string failureReason;

    if(!m_packagedRuntime)
    {
        const std::filesystem::path path = m_assetRoot / MaterialTables::BundledTableRelativePath;

        if(!MaterialTables::ReadMaterialTableBytes(path, developmentBytes, &failureReason))
        {
            m_materialTableUnavailableReason = failureReason;
            Log::Warning("Path-tracing material tables unavailable: " + failureReason);
            return;
        }

        bytes = developmentBytes;
    }
    else if(bytes.empty())
    {
        m_materialTableUnavailableReason = "This packaged archive predates the bundled material tables.";
        Log::Info("Path-tracing material tables unavailable: " + m_materialTableUnavailableReason);
        return;
    }

    if(!m_materialTables.Initialize(m_graphicsDevice.GetDevice(), m_resourceUploader, m_resourceDescriptorHeap,
        bytes, failureReason))
    {
        m_materialTableUnavailableReason = failureReason;
        Log::Warning("Path-tracing material tables unavailable: " + failureReason);
        return;
    }

    m_materialTableUnavailableReason.clear();
    Log::Info("Path-tracing material tables published (SHA-256 " + std::string(MaterialTables::BundledTableSha256) + ").");
}

bool Renderer::EnsurePathTraceReference()
{
    if(m_pathTraceReferencePass.IsInitialized())
    {
        return true;
    }

    // A failure is sticky for this renderer lifetime so a missing shader is not retried every frame.
    if(m_pathTraceStatus.availability == PathTraceReferenceAvailability::Unavailable)
    {
        return false;
    }

    std::string failureReason;

    if(!m_materialTables.IsAvailable())
    {
        m_pathTraceStatus.availability = PathTraceReferenceAvailability::Unavailable;
        m_pathTraceStatus.unavailableReason = "Material tables unavailable: " + m_materialTableUnavailableReason;
        Log::Error("Path traced reference mode unavailable: " + m_pathTraceStatus.unavailableReason);
        return false;
    }

    if(!m_pathTraceReferencePass.Initialize(m_graphicsDevice, m_shaderLibrary, m_resourceDescriptorHeap,
        m_sceneWidth, m_sceneHeight, failureReason))
    {
        m_pathTraceStatus.availability = PathTraceReferenceAvailability::Unavailable;
        m_pathTraceStatus.unavailableReason = failureReason;
        Log::Error("Path traced reference mode unavailable: " + failureReason);
        return false;
    }

    m_pathTraceStatus.availability = PathTraceReferenceAvailability::Available;
    m_pathTraceStatus.unavailableReason.clear();
    m_pathTraceStatus.resourceBytes = m_pathTraceReferencePass.GetResourceBytes();
    m_pathTraceResetPending = true;
    return true;
}

void Renderer::EnsurePreviousGBufferHistory()
{
    if(m_previousGBufferHistory.IsInitialized() || m_previousGBufferHistoryFailed)
    {
        return;
    }

    // Creation only: no GPU work is recorded, so allocating during recording is safe. A failure leaves the
    // path-traced modes rendering without history.
    if(!m_previousGBufferHistory.Initialize(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, m_sceneWidth,
        m_sceneHeight))
    {
        m_previousGBufferHistoryFailed = true;
        Log::Error("Previous-frame G-buffer history unavailable; path tracing continues without it.");
        return;
    }

    Log::Info("Previous-frame G-buffer history allocated (" +
        std::to_string(m_previousGBufferHistory.GetResourceBytes()) + " bytes).");
}

bool Renderer::EnsureHybridNrdDenoisers()
{
    if(m_nrdReflectionDenoiser.IsInitialized() && m_nrdGIDenoiser.IsInitialized())
    {
        return true;
    }

    if(m_hybridNrdCreationFailed)
    {
        return false;
    }

    ID3D12Device* device = m_graphicsDevice.GetDevice();
    const uint32_t giWidth = (m_sceneWidth + 1u) / 2u;
    const uint32_t giHeight = (m_sceneHeight + 1u) / 2u;

    if(!m_nrdReflectionDenoiser.Initialize(device, m_commandContext.GetCommandQueue(), m_resourceDescriptorHeap,
        m_sceneWidth, m_sceneHeight, HybridNrdReflectionDescription()))
    {
        Log::Error("Failed to init NRD reflection denoiser.");
        m_hybridNrdCreationFailed = true;
        return false;
    }

    if(!m_nrdGIDenoiser.Initialize(device, m_commandContext.GetCommandQueue(), m_resourceDescriptorHeap, giWidth,
        giHeight, HybridNrdGIDescription()))
    {
        Log::Error("Failed to init NRD diffuse GI denoiser.");
        m_nrdReflectionDenoiser.Shutdown();
        m_hybridNrdCreationFailed = true;
        return false;
    }

    // No history reset is needed here: a frame that creates the instances follows one that did not run NRD, so its
    // reset flag (m_nrdExecutedLastFrame) already restarts them.
    Log::Info("NRD REBLUR initialized.");
    Log::Info("NRD persistent memory: " + std::to_string(m_nrdReflectionDenoiser.GetPersistentMemoryUsageInMb()) +
        " MiB");
    Log::Info("NRD transient memory: " + std::to_string(m_nrdReflectionDenoiser.GetAliasableMemoryUsageInMb()) +
        " MiB");
    Log::Info("NRD total memory: " + std::to_string(m_nrdReflectionDenoiser.GetTotalMemoryUsageInMb()) + " MiB");
    Log::Info("NRD GI total memory: " + std::to_string(m_nrdGIDenoiser.GetTotalMemoryUsageInMb()) + " MiB");
    return true;
}

void Renderer::ReleaseHybridNrdDenoisers()
{
    TransitionTiming::Scope er1Scope("ReleaseHybridNrdDenoisers");
    if(!m_nrdReflectionDenoiser.IsInitialized() && !m_nrdGIDenoiser.IsInitialized())
    {
        return;
    }

    // Submitted frames may still read or write the instances' resources.
    if(!m_commandContext.WaitForGPU())
    {
        Log::Error("Failed to wait for the GPU before releasing the hybrid NRD denoisers; they are kept.");
        return;
    }

    m_nrdReflectionDenoiser.Shutdown();
    m_nrdGIDenoiser.Shutdown();
    m_nrdExecutedLastFrame = false;
    Log::Info("NRD REBLUR released (not used by path tracing).");
}

void Renderer::ReleaseNrdPathTraceDiagnostic()
{
    TransitionTiming::Scope er1Scope("ReleaseNrdPathTraceDiagnostic");
    if(!m_nrdPathTracePreparePass.HasTextures())
    {
        return;
    }

    // Submitted frames may still read or write the textures.
    if(!m_commandContext.WaitForGPU())
    {
        Log::Error("Failed to wait for the GPU before releasing the path-traced NRD preparation; it is kept.");
        return;
    }

    m_nrdPathTracePreparePass.ReleaseTextures();
}

void Renderer::ReleaseNrdRelaxPrepareDiagnostic()
{
    TransitionTiming::Scope er1Scope("ReleaseNrdRelaxPrepareDiagnostic");
    const bool releaseRawDistance = !m_nrdHybridDispatchDiagnostic &&
        m_pathTraceDenoiserMode != PathTraceDenoiserMode::NrdHybrid &&
        m_pathTraceReferencePass.HasRawDiffuseHitDistanceResources();

    if(!m_nrdRelaxPreparePass.IsInitialized() && !m_nrdRelaxPreparePass.HasTextures() &&
        !releaseRawDistance)
    {
        return;
    }

    // Submitted frames may still use the pipelines or textures.
    if(!m_commandContext.WaitForGPU())
    {
        Log::Error("Failed to wait for the GPU before releasing the RELAX preparation; it is kept.");
        return;
    }

    // NRD.RELAX.2B: the raw-distance extraction pipelines go with it, so ordinary rendering holds none.
    m_nrdRelaxPreparePass.Shutdown();
    // HYBRID.3 (diagnostic) and HYBRID.5 (selected) consume the same optional raw-distance producer independently of
    // standalone RELAX.
    if(releaseRawDistance)
    {
        m_pathTraceReferencePass.ReleaseRawDiffuseHitDistanceResources();
    }
}

bool Renderer::IsNrdRetentionGroupAllocated(NrdRetentionGroup group) const
{
    switch(group)
    {
    case NrdRetentionGroup::Relax:
        return m_nrdRelaxDenoiser.IsInitialized() || m_nrdRelaxResolvedTexture.GetResource() ||
            m_nrdRelaxPreparePass.IsInitialized() || m_nrdRelaxPreparePass.HasTextures() ||
            m_pathTraceReferencePass.HasRawDiffuseHitDistanceResources();
    case NrdRetentionGroup::PathTraceReblur:
        return m_nrdPathTraceDenoiser.IsInitialized() || m_nrdPathTraceValidationTexture.GetResource() ||
            m_nrdPathTracePreparePass.HasTextures();
    case NrdRetentionGroup::Legacy:
        return m_nrdReflectionDenoiser.IsInitialized() || m_nrdGIDenoiser.IsInitialized();
    case NrdRetentionGroup::Count:
        break;
    }

    return false;
}

// Whether this recording may reference the group. Selection counts even on frames the denoiser does not dispatch, and
// the raw diffuse hit distance is shared with the hybrid (selected or diagnostic), so RELAX's group stays active then.
bool Renderer::IsNrdRetentionGroupActive(NrdRetentionGroup group, bool runPathTrace) const
{
    switch(group)
    {
    case NrdRetentionGroup::Relax:
        return m_pathTraceDenoiserMode == PathTraceDenoiserMode::NrdRelax ||
            m_pathTraceDenoiserMode == PathTraceDenoiserMode::NrdHybrid || m_nrdRelaxPrepareDiagnostic ||
            m_nrdRelaxDispatchDiagnostic || m_nrdHybridDispatchDiagnostic;
    case NrdRetentionGroup::PathTraceReblur:
        return m_pathTraceDenoiserMode == PathTraceDenoiserMode::NrdReblur || m_nrdPathTraceDiagnostic;
    case NrdRetentionGroup::Legacy:
        return !runPathTrace;
    case NrdRetentionGroup::Count:
        break;
    }

    return true;
}

// DXGI local-segment headroom, (budget - usage) / budget. False when the adapter or its budget is unavailable.
bool Renderer::QueryVideoMemoryHeadroom(double& headroom)
{
    if(!m_videoMemoryAdapterQueried)
    {
        m_videoMemoryAdapterQueried = true;
        IDXGIFactory4* factory = m_graphicsDevice.GetFactory();
        ID3D12Device* device = m_graphicsDevice.GetDevice();

        if(!factory || !device || FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(),
            IID_PPV_ARGS(&m_videoMemoryAdapter))))
        {
            m_videoMemoryAdapter = nullptr;
            Log::Warning("Video-memory budget unavailable; at most one inactive NRD group is retained.");
        }
    }

    DXGI_QUERY_VIDEO_MEMORY_INFO info{};

    if(!m_videoMemoryAdapter ||
        FAILED(m_videoMemoryAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) || info.Budget == 0)
    {
        m_nrdRetentionStatus.budgetAvailable = false;
        return false;
    }

    m_nrdRetentionStatus.budgetAvailable = true;
    m_nrdRetentionStatus.budgetBytes = info.Budget;
    m_nrdRetentionStatus.usageBytes = info.CurrentUsage;
    headroom = (static_cast<double>(info.Budget) - static_cast<double>(info.CurrentUsage)) /
        static_cast<double>(info.Budget);
    return true;
}

// Called before this recording records anything that could reference an NRD group (where the groups used to be released
// with a GPU wait). Thresholds: below 10% headroom the budget is close enough that the OS may demote or the next
// allocation may fail; 20% to resume keeps an evicted group's own size (~50-150 MiB at 1600x900, a few percent of a
// typical budget) from flipping the state back at once. One eviction per frame lets DXGI usage settle between them.
void Renderer::UpdateNrdRetention(bool runPathTrace)
{
    ++m_nrdRetentionFrame;
    const std::uint64_t completedFence = m_commandContext.GetCompletedFenceValue();
    std::optional<NrdRetentionGroup> leastRecent;
    std::uint32_t inactiveGroups = 0;

    for(size_t index = 0; index < m_nrdRetention.size(); ++index)
    {
        const NrdRetentionGroup group = static_cast<NrdRetentionGroup>(index);
        NrdRetentionState& state = m_nrdRetention[index];

        // A previous recording that referenced the group never reached CommitNrdRetentionSubmission.
        if(state.usedThisRecording)
        {
            state.usedThisRecording = false;
            state.submissionUncertain = true;
        }

        if(!IsNrdRetentionGroupAllocated(group))
        {
            state = NrdRetentionState{false, false, 0, 0, state.evictions, state.deferrals};
            continue;
        }

        if(IsNrdRetentionGroupActive(group, runPathTrace))
        {
            state.usedThisRecording = true;
            state.lastActiveFrame = m_nrdRetentionFrame;
            continue;
        }

        ++inactiveGroups;

        if(!state.submissionUncertain && (!leastRecent ||
            state.lastActiveFrame < m_nrdRetention[static_cast<size_t>(*leastRecent)].lastActiveFrame))
        {
            leastRecent = group;
        }
    }

    if(!leastRecent)
    {
        return;
    }

    double headroom = 1.0;
    bool evict = false;

    if(!m_nrdRetentionBudgetUnavailableForTesting &&
        (m_nrdRetentionHeadroomOverride >= 0.0 || QueryVideoMemoryHeadroom(headroom)))
    {
        if(m_nrdRetentionHeadroomOverride >= 0.0)
        {
            headroom = m_nrdRetentionHeadroomOverride;
        }

        if(headroom < NrdRetentionEvictHeadroom)
        {
            m_nrdRetentionStatus.underPressure = true;
        }
        else if(headroom > NrdRetentionResumeHeadroom)
        {
            m_nrdRetentionStatus.underPressure = false;
        }

        evict = m_nrdRetentionStatus.underPressure;
    }
    else
    {
        // Without a budget, at most one inactive group is retained.
        evict = inactiveGroups > 1;
    }

    if(!evict)
    {
        return;
    }

    NrdRetentionState& state = m_nrdRetention[static_cast<size_t>(*leastRecent)];

    // Its last submitted use is still in flight: postponed to a later frame, never waited for.
    if(completedFence < state.lastUseFence)
    {
        ++state.deferrals;
        return;
    }

    if(EvictNrdRetentionGroup(*leastRecent))
    {
        ++state.evictions;
    }
}

void Renderer::CommitNrdRetentionSubmission()
{
    const std::uint64_t submissionFence = m_commandContext.GetLastSubmissionFenceValue();

    for(NrdRetentionState& state : m_nrdRetention)
    {
        if(state.usedThisRecording)
        {
            state.usedThisRecording = false;
            state.lastUseFence = submissionFence;
        }
    }
}

// No wait: UpdateNrdRetention proved the group inactive in this recording, confirmed every recording that used it
// submitted, and saw its last submission's fence complete. The instances are externally synchronized, so NRD does not
// wait either.
bool Renderer::EvictNrdRetentionGroup(NrdRetentionGroup group)
{
    TransitionTiming::Scope er1Scope("EvictNrdRetentionGroup");
    const auto synchronized = [](const NrdDenoiser& denoiser)
    {
        return !denoiser.IsInitialized() || denoiser.IsExternallySynchronized();
    };

    // An instance that would wait for the GPU in Destroy is never evicted here; the release paths that drain own it.
    const bool external = synchronized(m_nrdRelaxDenoiser) && synchronized(m_nrdPathTraceDenoiser) &&
        synchronized(m_nrdReflectionDenoiser) && synchronized(m_nrdGIDenoiser);

    if(!external)
    {
        Log::Error("An NRD instance eligible for eviction is not externally synchronized; it is kept.");
        return false;
    }

    const char* name = "";

    switch(group)
    {
    case NrdRetentionGroup::Relax:
        m_nrdRelaxDenoiser.Shutdown();
        m_nrdRelaxResolvedTexture.Shutdown();
        m_nrdRelaxConfidenceOne.Shutdown();
        m_nrdRelaxConfidenceLow.Shutdown();
        m_nrdConfidencePass.Shutdown();
        m_nrdRelaxPreparePass.Shutdown();
        m_pathTraceReferencePass.ReleaseRawDiffuseHitDistanceResources();
        m_nrdRelaxCreationFailed = false;
        m_nrdRelaxHistoryValid = false;
        m_nrdRelaxExecutedLastFrame = false;
        m_nrdRelaxFailure.clear();
        name = "RELAX";
        break;
    case NrdRetentionGroup::PathTraceReblur:
        m_nrdPathTraceDenoiser.Shutdown();
        m_nrdPathTraceValidationTexture.Shutdown();
        m_nrdPathTracePreparePass.ReleaseTextures();
        m_nrdPathTraceHistoryValid = false;
        m_nrdPathTraceExecutedLastFrame = false;
        name = "path-traced REBLUR";
        break;
    case NrdRetentionGroup::Legacy:
        m_nrdReflectionDenoiser.Shutdown();
        m_nrdGIDenoiser.Shutdown();
        m_nrdExecutedLastFrame = false;
        name = "legacy REBLUR";
        break;
    case NrdRetentionGroup::Count:
        return false;
    }

    Log::Info(std::string("Evicted the inactive ") + name + " NRD resources (retention policy).");
    return true;
}

void Renderer::EnsureRtxdiResources()
{
    TransitionTiming::Scope er1Scope("EnsureRtxdiResources");
    if(m_rtxdiResources.IsInitialized() || m_rtxdiResourcesFailed)
    {
        return;
    }

    // Creation only: no GPU work is recorded, so allocating during recording is safe.
    if(!m_rtxdiResources.Initialize(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, m_sceneWidth,
        m_sceneHeight))
    {
        m_rtxdiResourcesFailed = true;
        Log::Error("RTXDI resources unavailable.");
        return;
    }

    Log::Info("RTXDI resources allocated (" + std::to_string(m_rtxdiResources.GetResourceBytes()) + " bytes).");
}

// The primary constructor whose pipeline a DXR lighting mode records with.
PathTracePrimary Renderer::GetLightingModePrimary(RendererLightingMode mode) const
{
    switch(mode)
    {
    case RendererLightingMode::PathTracedRasterPrimary:
        return PathTracePrimary::Raster;
    case RendererLightingMode::PathTracedRasterPrimaryReSTIRDI:
        return PathTracePrimary::RasterReSTIRDI;
    case RendererLightingMode::PathTracedReference:
    case RendererLightingMode::Legacy:
        break;
    }

    return m_pathTraceReferencePrimary;
}

bool Renderer::IsLightingModeReady(RendererLightingMode mode) const
{
    if(!m_pathTraceReferencePass.IsPrimaryReady(GetLightingModePrimary(mode)))
    {
        return false;
    }

    return mode != RendererLightingMode::PathTracedRasterPrimaryReSTIRDI || m_rtxdiResources.IsInitialized();
}

// Polls the worker for every DXR library the mode needs, never blocking. ER.1 Phase 2: that is the whole set its
// pipelines acquire (GetRequiredLibraries: RayQuery twins, the ReSTIR DI direct and extraction libraries, and those of
// the effective denoiser and SHARC), so CreateLightingModeResources compiles nothing on the render thread.
ShaderLibraryPreparation Renderer::PrepareLightingModeLibraries(RendererLightingMode mode)
{
    TransitionTiming::Scope er1Scope("PrepareLightingModeLibraries", 0.5);
    return PrepareDxrLibraries(GetPathTraceLibraryNeeds(mode, m_pathTraceDenoiserMode, m_sharcSettings.enabled));
}

PathTraceLibraryNeeds Renderer::GetPathTraceLibraryNeeds(RendererLightingMode mode, PathTraceDenoiserMode denoiser,
    bool sharc) const
{
    PathTraceLibraryNeeds needs{};
    needs.primary = GetLightingModePrimary(mode);
    const bool split = needs.primary == PathTracePrimary::RasterReSTIRDI;
    const bool rasterPrimary = needs.primary == PathTracePrimary::Raster || split;

    // RecordPathTraceReference: the denoisers record signal extraction; RELAX, the hybrid and their diagnostics also
    // record the raw diffuse hit distance.
    const bool extract = rasterPrimary && denoiser != PathTraceDenoiserMode::Off;
    needs.extraction = extract;
    needs.rawDiffuseHitDistance = extract && (denoiser == PathTraceDenoiserMode::NrdRelax ||
        denoiser == PathTraceDenoiserMode::NrdHybrid || m_nrdHybridDispatchDiagnostic || m_nrdRelaxPrepareDiagnostic ||
        m_nrdRelaxDispatchDiagnostic);
    needs.radianceCache = sharc && rasterPrimary;
    needs.radianceCacheRender = SharcRenderLibrary(extract, split, needs.rawDiffuseHitDistance);
    return needs;
}

ShaderLibraryPreparation Renderer::PrepareDxrLibraries(const PathTraceLibraryNeeds& needs)
{
    ShaderLibraryPreparation combined{};
    combined.state = ShaderLibraryPreparationState::Ready;

    // Without the worker (packaged runtimes) pipelines keep their synchronous first-use compilation.
    if(!m_shaderCompileWorkerRunning)
    {
        return combined;
    }

    const auto readinessRank = [](ShaderLibraryPreparationState state)
    {
        switch(state)
        {
        case ShaderLibraryPreparationState::Ready:
            return 0;
        case ShaderLibraryPreparationState::Queued:
            return 1;
        case ShaderLibraryPreparationState::Compiling:
            return 2;
        case ShaderLibraryPreparationState::PreparingPipelines:
            return 3;
        case ShaderLibraryPreparationState::Failed:
            return 4;
        }

        return 4;
    };

    const std::vector<PathTraceLibraryRequirement> libraries = m_pathTraceReferencePass.GetRequiredLibraries(needs);

    for(size_t index = 0; index < libraries.size(); ++index)
    {
        const PathTraceLibraryRequirement& library = libraries[index];

        if(std::find(m_preparedDxrLibraries.begin(), m_preparedDxrLibraries.end(), library.path) !=
            m_preparedDxrLibraries.end())
        {
            continue;
        }

        ShaderLibraryPreparation preparation =
            m_pathTraceReferencePass.PrepareLibrary(m_shaderLibrary, *m_shaderCompileWorker, library.path);

        // The reference library is listed first. A new revision of it may fix a sticky reference failure; the next
        // gate retries it.
        if(index == 0 && preparation.acceptedNewRevision && !m_pathTraceReferencePass.IsInitialized() &&
            m_pathTraceStatus.availability == PathTraceReferenceAvailability::Unavailable)
        {
            m_pathTraceStatus.availability = PathTraceReferenceAvailability::NotRequested;
            m_pathTraceStatus.unavailableReason.clear();
        }

        // A RayQuery twin that fails leaves its pipeline on TraceRay (InitializeVisibilityPipelines warns), as a
        // synchronous failure always did; it does not hold back activation.
        if(library.optional && preparation.state == ShaderLibraryPreparationState::Failed)
        {
            preparation = {};
            preparation.state = ShaderLibraryPreparationState::Ready;
        }

        if(preparation.state == ShaderLibraryPreparationState::Ready)
        {
            m_preparedDxrLibraries.push_back(library.path);
        }

        // Every library is polled so they compile back to back; the least ready one decides.
        if(readinessRank(preparation.state) > readinessRank(combined.state))
        {
            combined = preparation;
        }
    }

    // ER.1 Phase 4: with every library ready, the configuration's DXR pipelines are created in the background; the
    // creation paths then adopt them instead of calling CreateStateObject while recording.
    if(combined.state == ShaderLibraryPreparationState::Ready && m_pipelineWorkerRunning)
    {
        combined = m_pathTraceReferencePass.PreparePipelines(*m_pipelineWorker, m_graphicsDevice.GetRayTracingDevice(),
            needs);
    }

    return combined;
}

ShaderLibraryPreparation Renderer::PrepareDxrLibraries(RendererLightingMode requestedMode,
    PathTraceDenoiserMode denoiser, bool sharc)
{
    const RendererLightingMode modes[] = {requestedMode, m_lightingModeTransition.GetStatus().active};
    ShaderLibraryPreparation combined{};
    combined.state = ShaderLibraryPreparationState::Ready;

    for(const RendererLightingMode mode : modes)
    {
        if(mode == RendererLightingMode::Legacy)
        {
            continue;
        }

        const ShaderLibraryPreparation preparation = PrepareDxrLibraries(GetPathTraceLibraryNeeds(mode, denoiser, sharc));

        if(preparation.state != ShaderLibraryPreparationState::Ready &&
            combined.state != ShaderLibraryPreparationState::Failed)
        {
            combined = preparation;
        }
    }

    return combined;
}

// ER.1 Phase 2: an NRD denoiser whose libraries (the raw-distance producer for RELAX and the hybrid) are not prepared
// is not switched to: the previous effective denoiser keeps presenting, so no frame records it half-created. A
// request that changes before its libraries are ready is simply never activated.
PathTraceDenoiserMode Renderer::GateDenoiserRequest(RendererLightingMode requestedMode, PathTraceDenoiserMode requested)
{
    const PathTraceDenoiserMode previous = m_pathTraceDenoiserMode;

    if(requested == previous)
    {
        m_denoiserPreparation.clear();
        return requested;
    }

    const ShaderLibraryPreparation preparation =
        PrepareDxrLibraries(requestedMode, requested, m_sharcSettings.enabled);
    std::string status;

    switch(preparation.state)
    {
    case ShaderLibraryPreparationState::Ready:
        if(!m_denoiserPreparation.empty())
        {
            Log::Info("PT denoiser libraries prepared; switching denoiser.");
        }

        m_denoiserPreparation.clear();
        return requested;
    case ShaderLibraryPreparationState::Queued:
    case ShaderLibraryPreparationState::Compiling:
        status = "Preparing the denoiser's shader libraries; the previous denoiser stays active.";
        break;
    case ShaderLibraryPreparationState::PreparingPipelines:
        status = "Preparing the denoiser's DXR pipelines; the previous denoiser stays active.";
        break;
    case ShaderLibraryPreparationState::Failed:
        status = "Denoiser unavailable: its shader libraries or DXR pipelines could not be prepared; the previous denoiser stays active.";
        break;
    }

    if(status != m_denoiserPreparation)
    {
        if(preparation.state == ShaderLibraryPreparationState::Failed)
        {
            Log::Error(status + "\n" + preparation.diagnostic);
        }
        else
        {
            Log::Info(status);
        }
    }

    m_denoiserPreparation = status;
    return previous;
}

// ER.1 Phase 2: SHARC stays effectively off until its training and render libraries are prepared, so no training or
// query pass records with a missing pipeline. Once on, a pending library of a later change keeps it on.
bool Renderer::GateSharcRequest(RendererLightingMode requestedMode, PathTraceDenoiserMode denoiser, bool requested)
{
    const bool previous = m_sharcSettings.enabled;

    if(!requested)
    {
        m_sharcPreparation.clear();
        return false;
    }

    const ShaderLibraryPreparation preparation = PrepareDxrLibraries(requestedMode, denoiser, true);
    std::string status;

    switch(preparation.state)
    {
    case ShaderLibraryPreparationState::Ready:
        if(!m_sharcPreparation.empty())
        {
            Log::Info("SHARC libraries prepared; enabling SHARC.");
        }

        m_sharcPreparation.clear();
        return true;
    case ShaderLibraryPreparationState::Queued:
    case ShaderLibraryPreparationState::Compiling:
        status = "Preparing SHARC's shader libraries; SHARC is not active yet.";
        break;
    case ShaderLibraryPreparationState::PreparingPipelines:
        status = "Preparing SHARC's DXR pipelines; SHARC is not active yet.";
        break;
    case ShaderLibraryPreparationState::Failed:
        status = "SHARC unavailable: its shader libraries or DXR pipelines could not be prepared.";
        break;
    }

    if(status != m_sharcPreparation)
    {
        if(preparation.state == ShaderLibraryPreparationState::Failed)
        {
            Log::Error(status + "\n" + preparation.diagnostic);
        }
        else
        {
            Log::Info(status);
        }
    }

    m_sharcPreparation = status;
    return previous;
}

// The render-thread half of an activation, after the libraries were accepted: the same idempotent creation
// the mode gates perform, ordered so the transition status names each stage. Failures are reported by the
// gates that follow.
void Renderer::CreateLightingModeResources(RendererLightingMode mode)
{
    TransitionTiming::Scope er1Scope("CreateLightingModeResources");
    const PathTracePrimary primary = GetLightingModePrimary(mode);

    if(EnsurePathTraceReference() && primary != PathTracePrimary::Camera)
    {
        std::string ignoredFailure;
        m_pathTraceReferencePass.EnsurePrimaryPipeline(m_graphicsDevice, m_shaderLibrary, primary, ignoredFailure);
    }

    m_lightingModeTransition.SetActivationStage(LightingModeTransitionStage::InitializingResources);

    if(primary == PathTracePrimary::RasterReSTIRDI && m_pathTraceReferencePass.IsPrimaryReady(primary))
    {
        EnsureRtxdiResources();
    }

    m_lightingModeTransition.SetActivationStage(LightingModeTransitionStage::ReadyToActivate);
}

// RTXPT.2: unprojection relative to the camera (the view without its translation), so positions far from
// the world origin keep their precision; callers then place results at the camera position.
static Matrix4 CameraRelativeInverseViewProjection(const Matrix4& view, const Matrix4& projection)
{
    Matrix4 rotationOnlyView = view;
    rotationOnlyView.m[0][3] = 0.0f;
    rotationOnlyView.m[1][3] = 0.0f;
    rotationOnlyView.m[2][3] = 0.0f;
    return (projection * rotationOnlyView).Inverse();
}

void Renderer::InvalidatePreviousGBufferHistory()
{
    m_previousGBufferValid = false;
    m_pendingPreviousGBufferCopy.reset();

    // RTXPT D.2B: temporal history is validated against the previous G-buffer, so it never outlives it (scene
    // history resets, camera cuts, resize, failed frames, path-tracing entry and primary changes). NRD.4B: neither does
    // REBLUR's. NRD.RELAX.3: nor RELAX's.
    m_pathTraceTemporalPass.InvalidateHistory();
    m_nrdPathTraceHistoryValid = false;
    m_nrdRelaxHistoryValid = false;
    m_nrdHybridHistoryValid = false;
}

void Renderer::RecordPreviousGBufferCopy(ID3D12GraphicsCommandList* commandList, const PreviousGBufferCamera& camera)
{
    if(!m_previousGBufferHistory.IsInitialized())
    {
        return;
    }

    // Never the read slot: this frame's consumers may still read it earlier in the same command list.
    const unsigned int writeSlot = 1u - m_previousGBufferReadSlot;

    if(!m_previousGBufferHistory.RecordCopy(commandList, m_gBuffer, writeSlot))
    {
        return;
    }

    m_pendingPreviousGBufferCopy = PendingPreviousGBufferCopy{writeSlot, camera};
    ++m_previousGBufferCopiesRecorded;
}

void Renderer::CommitPreviousGBufferCopy()
{
    if(!m_pendingPreviousGBufferCopy)
    {
        return;
    }

    const PendingPreviousGBufferCopy& pending = *m_pendingPreviousGBufferCopy;
    m_previousGBufferReadSlot = pending.slot;
    m_previousGBufferCameras[pending.slot] = pending.camera;
    m_previousGBufferValid = true;
    ++m_previousGBufferCopiesSubmitted;
    m_pendingPreviousGBufferCopy.reset();
}

bool Renderer::TryGetPreviousGBuffer(PreviousGBufferView& view) const
{
    if(!m_previousGBufferValid || !m_previousGBufferHistory.IsInitialized())
    {
        return false;
    }

    const PreviousGBufferHistory::Slot& slot = m_previousGBufferHistory.GetSlot(m_previousGBufferReadSlot);
    view.depthIndex = slot.depth.GetDescriptorIndex();
    view.surfaceIndex = slot.surface.GetDescriptorIndex();
    view.geometricNormalIndex = slot.geometricNormal.GetDescriptorIndex();
    view.materialIndex = slot.material.GetDescriptorIndex();
    view.camera = m_previousGBufferCameras[m_previousGBufferReadSlot];
    return true;
}

void Renderer::EvaluatePathTraceRasterPrimary()
{
    const PathTraceRasterPrimaryStatus previous = m_rasterPrimaryStatus;

    if(!m_graphicsDevice.SupportsRayTracing())
    {
        m_rasterPrimaryStatus = {PathTraceRasterPrimaryAvailability::Unavailable, "Ray tracing is not supported."};
    }
    else if(m_gBuffer.GetDepth().GetResource() == nullptr || m_gBuffer.GetSurface().GetResource() == nullptr ||
        m_gBuffer.GetMaterial().GetResource() == nullptr || m_gBuffer.GetGeometricNormal().GetResource() == nullptr)
    {
        m_rasterPrimaryStatus = {PathTraceRasterPrimaryAvailability::Unavailable, "The G-buffer is not available."};
    }
    else if(m_materialPublication.desired.size() > PathTraceRasterPrimaryMaximumMaterials)
    {
        m_rasterPrimaryStatus = {PathTraceRasterPrimaryAvailability::Unavailable,
            std::to_string(m_materialPublication.desired.size()) + " materials exceed the " +
            std::to_string(PathTraceRasterPrimaryMaximumMaterials) + " indices the G-buffer stores exactly."};
    }
    // The reference mode's own gate: material tables, the DXR library (absent from historical cooked
    // bundles) and its resources. Its failure is sticky, so this does not retry every frame.
    else if(!EnsurePathTraceReference())
    {
        m_rasterPrimaryStatus = {PathTraceRasterPrimaryAvailability::Unavailable,
            "Reference path tracer unavailable: " + m_pathTraceStatus.unavailableReason};
    }
    else
    {
        // Its own ray-generation entry; a shader bundle from before RTXPT.2 R3 lacks it (sticky failure).
        std::string pipelineFailure;

        if(m_pathTraceReferencePass.EnsurePrimaryPipeline(m_graphicsDevice, m_shaderLibrary, PathTracePrimary::Raster,
            pipelineFailure))
        {
            m_rasterPrimaryStatus = {PathTraceRasterPrimaryAvailability::Available, std::string()};
        }
        else
        {
            m_rasterPrimaryStatus = {PathTraceRasterPrimaryAvailability::Unavailable, pipelineFailure};
        }
    }

    if(m_rasterPrimaryStatus.availability != previous.availability || m_rasterPrimaryStatus.reason != previous.reason)
    {
        if(m_rasterPrimaryStatus.availability == PathTraceRasterPrimaryAvailability::Available)
        {
            Log::Info("Raster-primary path tracing mode available.");
        }
        else
        {
            Log::Info("Raster-primary path tracing mode unavailable, rendering Legacy: " + m_rasterPrimaryStatus.reason);
        }
    }
}

void Renderer::EvaluatePathTraceReSTIRDI()
{
    const PathTraceRasterPrimaryStatus previous = m_restirDIStatus;

    // The ReSTIR DI variant is the raster-primary mode plus RTXDI, so every raster-primary prerequisite applies.
    EvaluatePathTraceRasterPrimary();
    std::string pipelineFailure;

    if(m_rasterPrimaryStatus.availability != PathTraceRasterPrimaryAvailability::Available)
    {
        m_restirDIStatus = {PathTraceRasterPrimaryAvailability::Unavailable,
            "Raster primary unavailable: " + m_rasterPrimaryStatus.reason};
    }
    else if(!m_pathTraceReferencePass.EnsurePrimaryPipeline(m_graphicsDevice, m_shaderLibrary,
        PathTracePrimary::RasterReSTIRDI, pipelineFailure))
    {
        m_restirDIStatus = {PathTraceRasterPrimaryAvailability::Unavailable, pipelineFailure};
    }
    else
    {
        EnsureRtxdiResources();

        if(m_rtxdiResources.IsInitialized())
        {
            m_restirDIStatus = {PathTraceRasterPrimaryAvailability::Available, std::string()};
        }
        else
        {
            m_restirDIStatus = {PathTraceRasterPrimaryAvailability::Unavailable,
                "RTXDI reservoir resources could not be allocated."};
        }
    }

    if(m_restirDIStatus.availability != previous.availability || m_restirDIStatus.reason != previous.reason)
    {
        if(m_restirDIStatus.availability == PathTraceRasterPrimaryAvailability::Available)
        {
            Log::Info("Raster-primary ReSTIR DI path tracing mode available.");
        }
        else
        {
            Log::Info("Raster-primary ReSTIR DI path tracing mode unavailable, rendering Legacy: " +
                m_restirDIStatus.reason);
        }
    }
}

bool Renderer::PrepareRtxdiFrame(ID3D12GraphicsCommandList* commandList, const FrameLighting& frameLighting,
    const PathTraceReferenceConstants& constants, const Matrix4& view,
    D3D12_GPU_VIRTUAL_ADDRESS& rtxdiConstantsAddress)
{
    // The same HDRI distribution the path tracer binds this frame.
    RtxdiEnvironmentSource environment;
    environment.textureIndex = constants.environmentTextureIndex;
    environment.conditionalCDFIndex = constants.environmentConditionalCDFIndex;
    environment.marginalCDFIndex = constants.environmentMarginalCDFIndex;
    environment.width = constants.environmentWidth;
    environment.height = constants.environmentHeight;
    environment.totalWeight = constants.environmentTotalWeight;

    // RTXPT.3 R3.5: previous reservoirs are reusable only against identical visibility geometry and materials
    // (the renderer's accumulation-reset identities) and with the previous frame's surfaces (R4 history).
    RtxdiHistorySignature signature;
    signature.sceneResourceLifetime = m_sceneResourceLifetime;
    signature.topLevelAddress = m_rayTracingScene.GetTopLevel().GetGPUAddress();
    signature.topLevelRecording = m_rayTracingScene.GetTLASRecordingIdentity();
    signature.materialRevision = m_materialPublication.revision;

    PreviousGBufferView previousGBuffer;
    const bool previousSurfacesAvailable = TryGetPreviousGBuffer(previousGBuffer);

    RtxdiLightSet lights;
    RtxdiFrame frame;

    if(!BuildRtxdiLightSet(frameLighting, environment, lights) ||
        !m_rtxdiResources.BeginFrame(lights, signature, previousSurfacesAvailable, frame))
    {
        Log::Error("Failed to prepare the RTXDI frame.");
        return false;
    }

    PathTraceRtxdiConstants rtxdiConstants{};
    rtxdiConstants.lightBuffer = frame.lightBufferParameters;
    rtxdiConstants.reservoirBuffer = frame.reservoirBufferParameters;
    rtxdiConstants.bufferIndices = frame.bufferIndices;
    rtxdiConstants.reservoirBufferIndex = m_rtxdiResources.GetReservoirUAVIndex();
    rtxdiConstants.localLightSamples = m_rtxdiLocalLightSamplesForTesting != 0 ? m_rtxdiLocalLightSamplesForTesting :
        RtxdiInitialLocalLightSamples;
    rtxdiConstants.infiniteLightSamples = m_rtxdiInfiniteLightSamplesForTesting != 0 ?
        m_rtxdiInfiniteLightSamplesForTesting : RtxdiInitialInfiniteLightSamples;

    for(std::size_t index = 0; index < frame.lightRecords.size(); ++index)
    {
        rtxdiConstants.lightRecords[index * 2] = frame.lightRecords[index].kind;
        rtxdiConstants.lightRecords[index * 2 + 1] = frame.lightRecords[index].sourceIndex;
    }

    rtxdiConstants.runtime = frame.runtimeParameters;
    rtxdiConstants.spatioTemporal = frame.spatioTemporalParameters;
    rtxdiConstants.historyValid = frame.historyValid ? 1u : 0u;
    rtxdiConstants.neighborOffsetsIndex = m_rtxdiResources.GetNeighborOffsetsSRVIndex();
    rtxdiConstants.motionIndex = m_gBuffer.GetMotion().GetSRVIndex();
    rtxdiConstants.currentLightCount = static_cast<std::uint32_t>(frame.lightRecords.size());
    rtxdiConstants.previousLightCount = frame.previousLightCount;

    for(std::size_t index = 0; index < frame.lightTranslation.size(); ++index)
    {
        rtxdiConstants.lightTranslation[index] = frame.lightTranslation[index];
    }

    // Linear depth along each camera's forward axis (row 2 of a left-handed view, as Matrix4::LookAt builds it).
    rtxdiConstants.cameraForward[0] = view.m[2][0];
    rtxdiConstants.cameraForward[1] = view.m[2][1];
    rtxdiConstants.cameraForward[2] = view.m[2][2];

    if(frame.historyValid)
    {
        rtxdiConstants.previousDepthIndex = previousGBuffer.depthIndex;
        rtxdiConstants.previousSurfaceIndex = previousGBuffer.surfaceIndex;
        rtxdiConstants.previousGeometricNormalIndex = previousGBuffer.geometricNormalIndex;
        rtxdiConstants.previousMaterialIndex = previousGBuffer.materialIndex;
        rtxdiConstants.previousCameraPosition[0] = previousGBuffer.camera.position.x;
        rtxdiConstants.previousCameraPosition[1] = previousGBuffer.camera.position.y;
        rtxdiConstants.previousCameraPosition[2] = previousGBuffer.camera.position.z;
        rtxdiConstants.previousCameraForward[0] = previousGBuffer.camera.view.m[2][0];
        rtxdiConstants.previousCameraForward[1] = previousGBuffer.camera.view.m[2][1];
        rtxdiConstants.previousCameraForward[2] = previousGBuffer.camera.view.m[2][2];
        rtxdiConstants.previousInverseCameraRelativeViewProjection =
            previousGBuffer.camera.inverseCameraRelativeViewProjection;
    }

    rtxdiConstantsAddress = m_uploadBuffer.Allocate(&rtxdiConstants, sizeof(PathTraceRtxdiConstants),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(rtxdiConstantsAddress == 0)
    {
        Log::Error("Failed to allocate RTXDI constants.");
        return false;
    }

    m_rtxdiResources.GetReservoirBuffer().Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return true;
}

bool Renderer::SamePathTraceAccumulationKey(const PathTraceAccumulationKey& left, const PathTraceAccumulationKey& right)
{
    // Bitwise comparison: any representable change, however small, restarts accumulation.
    const bool sameMatrices = std::memcmp(&left.view, &right.view, sizeof(Matrix4)) == 0 &&
        std::memcmp(&left.projection, &right.projection, sizeof(Matrix4)) == 0;
    const bool sameSun = std::memcmp(&left.sun.direction, &right.sun.direction, sizeof(Vector3)) == 0 &&
        std::memcmp(&left.sun.color, &right.sun.color, sizeof(Vector3)) == 0 &&
        left.sun.intensity == right.sun.intensity && left.sun.angularRadius == right.sun.angularRadius;
    bool sameLights = left.lights.lightCount == right.lights.lightCount;

    for(std::uint32_t index = 0; sameLights && index < left.lights.lightCount; ++index)
    {
        sameLights = left.lights.lights[index] == right.lights.lights[index];
    }

    return left.primary == right.primary && sameMatrices && sameSun && sameLights && left.width == right.width &&
        left.height == right.height &&
        left.settings == right.settings && left.topLevelAddress == right.topLevelAddress &&
        left.topLevelRecording == right.topLevelRecording && left.materialRevision == right.materialRevision &&
        left.sceneResourceLifetime == right.sceneResourceLifetime &&
        left.environmentTextureIndex == right.environmentTextureIndex &&
        left.environmentConditionalCDFIndex == right.environmentConditionalCDFIndex &&
        left.environmentTotalWeight == right.environmentTotalWeight;
}

Renderer::SceneLightingInputs Renderer::MakeSceneLightingInputs(const PathTraceAccumulationKey& key,
    std::uint64_t materialBindingEpoch, PathTraceLibraryVariant libraryVariant,
    const RadianceCacheConfigurationIdentity& cache)
{
    SceneLightingInputs inputs{};
    inputs.key = key;
    // World-space radiance does not depend on the camera, the image size or how many samples a pixel accumulates.
    inputs.key.view = Matrix4{};
    inputs.key.projection = Matrix4{};
    inputs.key.width = 0;
    inputs.key.height = 0;
    inputs.key.settings.samplesPerFrame = 0;
    inputs.key.settings.maxAccumulatedSamples = 0;
    inputs.materialBindingEpoch = materialBindingEpoch;
    inputs.libraryVariant = libraryVariant;
    inputs.cache = cache;
    return inputs;
}

bool Renderer::SameSceneLightingInputs(const SceneLightingInputs& left, const SceneLightingInputs& right)
{
    return SamePathTraceAccumulationKey(left.key, right.key) && left.materialBindingEpoch == right.materialBindingEpoch &&
        left.libraryVariant == right.libraryVariant && left.cache == right.cache;
}

void Renderer::LatchSceneLighting(const PathTraceAccumulationKey& key)
{
    const SceneLightingInputs inputs = MakeSceneLightingInputs(key, m_rayTracingScene.m_materialBindingEpoch,
        m_pathTraceReferencePass.GetLibraryVariant(), m_radianceCacheConfiguration);
    const bool changed = !m_sceneLightingInputs || !SameSceneLightingInputs(*m_sceneLightingInputs, inputs);
    const bool usable = key.sceneResourceLifetime != 0 && m_sceneResourceScene != nullptr;
    m_sceneLightingEpoch.Latch(changed, usable);
    m_sceneLightingInputs = inputs;
}

void Renderer::InvalidateSceneLighting(SceneLightingInvalidation reason) noexcept
{
    m_sceneLightingEpoch.Invalidate(reason);
}

const SceneLightingEpoch::Status& Renderer::GetSceneLightingEpoch() const noexcept
{
    return m_sceneLightingEpoch.GetStatus();
}

void Renderer::UpdateRadianceCacheD6(bool enabled)
{
    TransitionTiming::Scope er1Scope("UpdateRadianceCacheD6", 0.5);
    // A generation recorded by an earlier, unsubmitted recording never ran.
    m_radianceCacheD6.BeginRecording();
    m_radianceCacheD6.ReleaseRetired(m_commandContext.GetCompletedFenceValue());
    const std::uint64_t lastSubmissionFence = m_commandContext.GetLastSubmissionFenceValue();

    if(!enabled || m_packagedRuntime)
    {
        m_radianceCacheD6.Retire(lastSubmissionFence);
        m_radianceCacheD6Failed = false;
    }
    else if(!m_radianceCacheD6Failed)
    {
        const bool failForTesting = std::exchange(m_radianceCacheD6FailAllocationForTesting, false);

        if(!m_radianceCacheD6.EnsureResources(m_graphicsDevice.GetDevice(), m_resourceUploader, m_shaderLibrary,
            m_resourceDescriptorHeap, m_radianceCacheD6Settings, lastSubmissionFence, failForTesting))
        {
            m_radianceCacheD6Failed = true;
        }
    }

    // A change of identity (allocation, reconfiguration, release) is a scene-lighting input change.
    m_radianceCacheConfiguration = m_radianceCacheD6.GetIdentity();
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::PrepareRadianceCacheLearning(ID3D12GraphicsCommandList* commandList,
    std::uint32_t samplesPerFrame)
{
    TransitionTiming::Scope er1Scope("PrepareRadianceCacheLearning", 0.5);
    std::string failure;
    const RadianceCacheD6::Insertion insertion = m_radianceCacheD6.GetInsertion();
    const bool sparse = insertion == RadianceCacheD6::Insertion::Sparse;
    const bool deferred = insertion != RadianceCacheD6::Insertion::Inline;
    m_radianceCacheLearningLibrary = sparse ? PathTraceRadianceCacheLibrary::Training :
        (deferred ? m_radianceCacheDeferredLibrary : PathTraceRadianceCacheLibrary::Inline);

    // SHARC.4D-R: sparse training with no tile size trains nothing this frame.
    if(sparse && m_radianceCacheD6.GetTrainingTile() == 0)
    {
        return 0;
    }

    if(!m_pathTraceReferencePass.EnsureRadianceCacheLearning(m_graphicsDevice, m_shaderLibrary, failure,
        m_radianceCacheLearningLibrary))
    {
        if(!m_radianceCacheLearningFailureLogged)
        {
            Log::Error("D6 radiance cache learning is unavailable; the reference path tracer is unaffected: " + failure);
            m_radianceCacheLearningFailureLogged = true;
        }

        return 0;
    }

    // SHARC.4D-Q: one record per path of the dispatch. Without room for them the frame renders without learning.
    // SHARC.4D-R: a sparse training dispatch's paths are its threads' (one per tile).
    const std::uint64_t paths = sparse ? m_radianceCacheD6.TrainingThreads(m_sceneWidth, m_sceneHeight) :
        static_cast<std::uint64_t>(m_sceneWidth) * m_sceneHeight;
    // SHARC.6A: with the history prior, each training path also stores a primary-class record after the x1 records.
    const bool primaryRecords = sparse && m_radianceCacheD6.UsesHistoryPrior();
    const std::uint64_t recordCount = paths * samplesPerFrame * (primaryRecords ? 2u : 1u);
    m_radianceCacheD6.SetPrimaryRecordOffset(primaryRecords ? static_cast<std::uint32_t>(paths * samplesPerFrame) : 0u);

    if(deferred && !m_radianceCacheD6.EnsureRecords(m_graphicsDevice.GetDevice(), m_shaderLibrary,
        m_resourceDescriptorHeap, recordCount, m_commandContext.GetLastSubmissionFenceValue(),
        std::exchange(m_radianceCacheRecordsFailAllocationForTesting, false)))
    {
        return 0;
    }

    return m_radianceCacheD6.RecordLearningBegin(commandList, m_uploadBuffer, m_sceneLightingEpoch.GetStatus(),
        deferred ? recordCount : 0, sparse ? m_radianceCacheD6.TrainingTilesX(m_sceneWidth) : 0);
}

PathTraceRadianceCacheLibrary Renderer::SharcRenderLibrary(bool extract, bool splitReSTIRDI,
    bool rawDiffuseHitDistance)
{
    const bool raw = extract && rawDiffuseHitDistance;

    if(splitReSTIRDI)
    {
        if(raw)
        {
            return PathTraceRadianceCacheLibrary::RenderSplitIndirectExtractionRawDiffuseHitDistance;
        }

        return extract ? PathTraceRadianceCacheLibrary::RenderSplitIndirectExtraction :
            PathTraceRadianceCacheLibrary::RenderSplitIndirect;
    }

    if(raw)
    {
        return PathTraceRadianceCacheLibrary::RenderExtractionRawDiffuseHitDistance;
    }

    return extract ? PathTraceRadianceCacheLibrary::RenderExtraction : PathTraceRadianceCacheLibrary::Render;
}

PathTraceRadianceCacheLibrary Renderer::RadianceCacheQueryLibrary(bool extract, bool splitReSTIRDI,
    bool rawDiffuseHitDistance) const
{
    if(m_radianceCacheD6.UsesQueryRenderLibrary())
    {
        return SharcRenderLibrary(extract, splitReSTIRDI, rawDiffuseHitDistance);
    }

    return m_radianceCacheD6.UsesQueryTerminationLibrary() ? PathTraceRadianceCacheLibrary::QueryTermination :
        PathTraceRadianceCacheLibrary::Query;
}

void Renderer::ApplySharcSettings(const SharcSettings& requested, bool rasterPrimary)
{
    TransitionTiming::Scope er1Scope("ApplySharcSettings", 0.5);
    SharcSettings settings = requested;
    settings.enabled = requested.enabled && rasterPrimary;
    settings.trainingTile = requested.trainingTile == 8 ? 8u : 5u;
    const SharcSettings previous = std::exchange(m_sharcSettings, settings);

    // A different estimator (on/off, threshold) or a reset cache restarts accumulation; density, freezing and the
    // statistics do not change what a path estimates.
    const bool estimatorChanged = settings.minimumSamples != previous.minimumSamples ||
        settings.resetRequest != previous.resetRequest || settings.adaptiveResolution != previous.adaptiveResolution ||
        settings.historyPrior != previous.historyPrior;

    // SHARC.5F: the mapping is part of the cache's settings and identity, so a switch reallocates and clears it and no
    // entry is read under the other mapping. Only SHARC selects it: a cache kept without SHARC (diagnostics) keeps its
    // mapping, so leaving SHARC does not rebuild it, and diagnostics that never enable SHARC keep the uniform grid.
    if(settings.enabled)
    {
        m_radianceCacheD6Settings.adaptive = settings.adaptiveResolution;
    }

    if(settings.enabled != previous.enabled || (settings.enabled && estimatorChanged))
    {
        // The denoiser's temporal history is kept across accumulation restarts (camera motion); a different estimator
        // must not be blended with it.
        m_pathTraceResetPending = true;
        m_pathTraceTemporalPass.InvalidateHistory();
        m_nrdPathTraceHistoryValid = false;
        m_nrdRelaxHistoryValid = false;
        m_nrdHybridHistoryValid = false;
    }

    RadianceCacheD6& cache = m_radianceCacheD6;

    if(!settings.enabled)
    {
        // Diagnostics drive the cache directly; only leaving SHARC restores the defaults.
        if(previous.enabled)
        {
            cache.SetQueryRender(false, 0.0f, false);
            cache.SetQuery(false, 0);
            cache.SetTrainingTile(5);
            cache.SetInsertion(RadianceCacheD6::Insertion::Inline);
            cache.SetHistoryPrior(false);
        }

        m_sharcStatus = {};
        return;
    }

    // Sparse training (none while frozen), and the render library substituting at the threshold.
    cache.SetInsertion(RadianceCacheD6::Insertion::Sparse);
    cache.SetTrainingTile(settings.freezeTraining ? 0u : settings.trainingTile);
    cache.SetQuery(true, m_sharcDiagnosticQueryMode != 0 ? m_sharcDiagnosticQueryMode : 1u);
    cache.SetQueryRender(true, static_cast<float>(settings.minimumSamples), settings.statistics);
    cache.SetHistoryPrior(settings.historyPrior);

    if(settings.resetRequest != previous.resetRequest)
    {
        cache.RequestClear();
    }
}

void Renderer::ApplyPathTraceDenoiser(PathTraceDenoiserMode requested, bool rasterPrimary)
{
    // NRD.4B: REBLUR is implemented; NRD.RELAX.4: so is RELAX; HYBRID.5: so is the hybrid.
    const PathTraceDenoiserMode mode = rasterPrimary ? requested : PathTraceDenoiserMode::Off;

    // NRD.4B: histories never cross modes; entering or leaving REBLUR restarts its history. NRD.RELAX.4: RELAX's too, and a
    // mode change retries a failed RELAX creation. HYBRID.5: the same for the hybrid.
    if(mode != m_pathTraceDenoiserMode)
    {
        m_nrdPathTraceHistoryValid = false;
        m_nrdPathTraceCreationFailed = false;
        m_nrdRelaxHistoryValid = false;
        m_nrdHybridHistoryValid = false;
        m_nrdRelaxCreationFailed = false;
        m_nrdHybridCreationFailed = false;
    }

    m_pathTraceDenoiserMode = mode;
    // REBLUR, (NRD.RELAX.4) RELAX and (HYBRID.5) the hybrid keep the Engine stages enabled: a frame they cannot denoise
    // falls back to them (RecordPathTraceReference skips them on frames an NRD mode presents).
    const bool denoise = mode == PathTraceDenoiserMode::Engine || mode == PathTraceDenoiserMode::NrdReblur ||
        mode == PathTraceDenoiserMode::NrdRelax || mode == PathTraceDenoiserMode::NrdHybrid;

    if(denoise == m_pathTraceDenoiserOption)
    {
        return;
    }

    // Extraction feeds the temporal pass, whose history the spatial pass filters and presents. Each stage's own
    // history rules apply: a frame without the temporal pass breaks its chain, so turning it back on starts afresh.
    m_pathTraceDenoiserOption = denoise;
    m_pathTraceSignalExtraction = denoise;
    m_pathTraceTemporal = denoise;
    m_pathTraceTemporalPresentation = denoise;
    m_pathTraceSpatial = denoise;
}

void Renderer::ApplyCameraJitter(bool requested)
{
    if(requested == m_cameraJitterOption)
    {
        return;
    }

    // Histories recorded with the other sampling pattern are not reused.
    m_cameraJitterOption = requested;
    m_cameraJitterPhase = 0;
    m_upscalerHistoryResetPending = true;
    m_pathTraceResetPending = true;
    m_pathTraceTemporalPass.InvalidateHistory();
    InvalidatePreviousGBufferHistory();
    m_rtxdiResources.InvalidateHistory();
}

std::uint32_t Renderer::DlssJitterPhaseCount(unsigned int renderWidth, unsigned int renderHeight,
    unsigned int outputWidth, unsigned int outputHeight) noexcept
{
    if(renderWidth == 0 || renderHeight == 0)
    {
        return 16u;
    }

    // The pixel-area ratio: the guide scales the base count by it (8, 18, 24, 32, 72 for 1, 1.5, 1.724, 2, 3 per axis).
    // Rounded to the nearest count, so an odd render extent (853 x 480 for 1280 x 720 Quality) keeps the table's 18.
    const double areaRatio = (static_cast<double>(outputWidth) * outputHeight) /
        (static_cast<double>(renderWidth) * renderHeight);
    const auto scaled = static_cast<std::uint32_t>(std::lround(8.0 * areaRatio));
    return (std::max)(16u, scaled);
}

void Renderer::FailDlss(const std::string& stage, const std::string& detail)
{
    const std::string failure = stage + ": " + (detail.empty() ? "unavailable" : detail);
    if(m_dlssFailure != failure) Log::Warning("DLSS: " + failure + "; Native fallback scheduled.");
    m_dlssFailure = failure;
    m_dlssActiveLastFrame = false;
    m_dlssEvaluatedLastFrame = false;
}

void Renderer::RestoreDlssGraphicsState(ID3D12GraphicsCommandList* commandList)
{
    ID3D12DescriptorHeap* heaps[] = {m_resourceDescriptorHeap.GetHeap()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0, 0, float(m_outputWidth), float(m_outputHeight), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(m_outputWidth), LONG(m_outputHeight)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    // Each following pass binds its own root signature, pipeline and targets.
}

void Renderer::PublishDlssReadout()
{
    if(!m_debugUIInitialized) return;
    const auto& backend = m_dlssBackend.GetStatus();
    const auto& feature = m_dlssBackend.GetFeatureStatus();
    const bool requested = m_dlssNormalRequest || IsDlssRenderScale(m_requestedRenderScale);
    std::string state;
    if(!requested) state = backend.availability == DlssAvailability::Available ? "Available (disabled)" : "Disabled";
    else if(!m_dlssFailure.empty()) state = "Failed";
    else if(backend.availability != DlssAvailability::Available) state = "Unavailable";
    else if(!m_dlssEligible) state = m_debugUI.IsDlssPreparing() ? "Initializing (waiting for mode / denoiser)" : "Unavailable";
    else if(m_dlssActiveLastFrame) state = "Active";
    else state = "Initializing / awaiting eligible frame";
    std::string detail = std::string("Backend: ") + DlssAvailabilityName(backend.availability) + ". " + backend.detail;
    detail += " Internal " + std::to_string(m_sceneWidth) + "x" + std::to_string(m_sceneHeight) +
        "; output " + std::to_string(m_outputWidth) + "x" + std::to_string(m_outputHeight) + ".";
    if(auto preset = GetDlssPreset(m_requestedRenderScale)) detail += std::string(" Requested ") + DlssQualityPresetName(*preset) + ".";
    detail += feature.active ? " Feature created." : " No feature created.";
    if(requested && !m_dlssEligible) detail += " Requires active raster-primary rendering and a ready denoiser.";
    if(!m_dlssFailure.empty()) detail += " " + m_dlssFailure;
    else if(requested && m_dlssInactiveReason[0] != '\0') detail += std::string(" ") + m_dlssInactiveReason;
    if(!feature.detail.empty() && (feature.active || !m_dlssFailure.empty())) detail += " " + feature.detail;
    if(!m_dlssFailure.empty() && PresentationResolveActive())
        detail += " Transitional bilinear output; Native scheduled at the next safe resize boundary.";
    else if(m_renderScale == RenderScale::Native) detail += " Effective Native.";
    if(state != m_dlssPublishedState || detail != m_dlssPublishedDetail)
    {
        Log::Info("DLSS: " + state + ". " + detail);
        m_dlssPublishedState = state;
        m_dlssPublishedDetail = detail;
    }
    const bool canRetry = backend.availability != DlssAvailability::NotCompiled &&
        (!m_dlssFailure.empty() || (requested && backend.availability != DlssAvailability::Available));
    m_debugUI.SetDlssReadout(std::move(state), std::move(detail), canRetry);
}

bool Renderer::RecordEditorOverlayPrepass(ID3D12GraphicsCommandList* commandList, const Scene& scene,
    const RendererOptions& options, D3D12_GPU_VIRTUAL_ADDRESS unjitteredCameraConstantsAddress)
{
    // Stage 1: output-extent targets, created on first use and after each resize.
    ID3D12Device* const device = m_graphicsDevice.GetDevice();

    if(!m_overlayDepth.GetResource())
    {
        const bool created = m_overlayDepth.Initialize(device, m_outputWidth, m_outputHeight, m_dsvDescriptorHeap,
            m_resourceDescriptorHeap) && m_overlaySelectionMask.Initialize(device, m_outputWidth, m_outputHeight,
            DXGI_FORMAT_R8_UNORM, m_rtvDescriptorHeap, m_resourceDescriptorHeap);

        if(!created)
        {
            m_overlayDepth.Shutdown();
            m_overlaySelectionMask.Shutdown();
            return false;
        }
    }

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(m_outputWidth), static_cast<float>(m_outputHeight),
        0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(m_outputWidth), static_cast<LONG>(m_outputHeight)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Stage 2: depth of every mesh object with the unjittered camera (the mask target takes the pipeline's colour
    // writes and is cleared afterwards).
    m_overlayDepth.Transition(commandList, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    m_overlaySelectionMask.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->ClearDepthStencilView(m_overlayDepth.GetDSV(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    const D3D12_CPU_DESCRIPTOR_HANDLE maskRTV = m_overlaySelectionMask.GetRTV();
    const D3D12_CPU_DESCRIPTOR_HANDLE depthDSV = m_overlayDepth.GetDSV();
    commandList->OMSetRenderTargets(1, &maskRTV, FALSE, &depthDSV);

    for(const RenderObject& object : scene.GetRenderObjects())
    {
        DrawSelectionMask(commandList, object, unjitteredCameraConstantsAddress, true);
    }

    // Stage 3: the selected objects into the mask, depth-tested (LESS_EQUAL, read-only) as the geometry pass's mask.
    const float maskClearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
    commandList->ClearRenderTargetView(maskRTV, maskClearColor, 0, nullptr);
    m_overlayDepth.Transition(commandList, D3D12_RESOURCE_STATE_DEPTH_READ);
    const D3D12_CPU_DESCRIPTOR_HANDLE readOnlyDSV = m_overlayDepth.GetReadOnlyDSV();
    commandList->OMSetRenderTargets(1, &maskRTV, FALSE, &readOnlyDSV);
    const auto& renderObjects = scene.GetRenderObjects();
    const size_t selectionCount = options.selectedObjects.empty() ? 1 : options.selectedObjects.size();

    for(size_t selection = 0; selection < selectionCount; ++selection)
    {
        const size_t index = options.selectedObjects.empty() ? options.selectedObject : options.selectedObjects[selection];

        if(index < renderObjects.size())
        {
            DrawSelectionMask(commandList, renderObjects[index], unjitteredCameraConstantsAddress);
        }
    }

    // The tone map reads both; the overlays test the depth (they transition it themselves).
    m_overlaySelectionMask.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_overlayDepth.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

bool Renderer::RecordDlssEvaluation(ID3D12GraphicsCommandList* commandList, unsigned int frameIndex)
{
    const UpscalerFrameInputs& inputs = m_upscalerInputs;

    // Stage 1: DLSS needs this frame's denoised, changing image and the prepared inputs.
    if(m_dlssEvaluationFailureForTesting)
    {
        FailDlss("Evaluation", "forced diagnostic failure");
        m_dlssInactiveReason = "DLSS evaluation failed (diagnostic)";
        return false;
    }

    if(!inputs.prepared)
    {
        FailDlss("Input preparation", "inputs not prepared");
        m_dlssInactiveReason = "inputs not prepared";
        return false;
    }

    if(!inputs.colorIsPerFrameDenoised && !m_dlssColorOverrideForTesting)
    {
        FailDlss("Color input", "no fresh per-frame denoised color");
        m_dlssInactiveReason = "the path-tracing denoiser is off (no per-frame denoised colour)";
        return false;
    }

    const bool featureReady = m_dlssBackend.EnsureFeature(commandList, inputs.renderWidth, inputs.renderHeight, inputs.outputWidth,
        inputs.outputHeight, GetDlssPreset(m_renderScale));
    RestoreDlssGraphicsState(commandList);
    if(!featureReady)
    {
        FailDlss("Feature creation", m_dlssBackend.GetFeatureStatus().detail);
        m_dlssInactiveReason = m_dlssBackend.GetStatus().availability != DlssAvailability::Available ?
            "DLSS unavailable" : "DLSS feature creation failed";
        return false;
    }

    // Stage 2: the states DLSS requires (Programming Guide 3.4); it returns the resources to them.
    RenderTarget& color = m_dlssColorOverrideForTesting ? *m_dlssColorOverrideForTesting : *inputs.color;
    constexpr D3D12_RESOURCE_STATES input = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    color.Transition(commandList, input);
    inputs.depth->Transition(commandList, input);
    inputs.dlssMotionVectors->Transition(commandList, input);
    inputs.exposure->Transition(commandList, input);
    m_presentationColor.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Stage 3: the evaluation. Jitter converts from Engine's convention (sample = pixel centre + offset) to NGX's.
    const float signX = m_dlssJitterSignForTesting ? (*m_dlssJitterSignForTesting)[0] : DlssJitterSignX;
    const float signY = m_dlssJitterSignForTesting ? (*m_dlssJitterSignForTesting)[1] : DlssJitterSignY;
    DlssEvaluation evaluation{};
    evaluation.color = color.GetResource();
    evaluation.output = m_presentationColor.GetResource();
    evaluation.depth = inputs.depth->GetResource();
    evaluation.motionVectors = inputs.dlssMotionVectors->GetResource();
    evaluation.exposure = inputs.exposure->GetResource();
    evaluation.jitterOffsetX = signX * inputs.jitterX;
    evaluation.jitterOffsetY = signY * inputs.jitterY;
    evaluation.renderWidth = inputs.renderWidth;
    evaluation.renderHeight = inputs.renderHeight;
    // A history discontinuity, a fresh feature or a gap in DLSS frames starts DLSS history again.
    evaluation.reset = inputs.reset || !m_dlssEvaluatedLastFrame || m_dlssBackend.GetFeatureStatus().evaluations == 0;

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::DlssEvaluate);
    const bool evaluated = m_dlssBackend.Evaluate(commandList, evaluation);
    RestoreDlssGraphicsState(commandList);
    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::DlssEvaluate);

    if(!evaluated)
    {
        // The caller overwrites the complete output with a transitional resolve, even if NGX partially wrote it.
        FailDlss("Evaluation", m_dlssBackend.GetFeatureStatus().detail);
        m_dlssInactiveReason = "DLSS evaluation failed";
        return false;
    }

    m_dlssInactiveReason = "";
    return true;
}

void Renderer::ApplyDlssInputPreparation(bool requested)
{
    if(requested == m_dlssInputPreparation)
    {
        return;
    }

    m_dlssInputPreparation = requested;
    m_upscalerHistoryResetPending = true;
    m_upscalerInputs = {};

    if(!requested)
    {
        // Only the textures; the pipeline stays for the next time. No frame in flight reads them after the
        // recording that stopped preparing, and Texture::Shutdown defers nothing, so wait for the GPU first.
        if(m_dlssInputPreparationPass.GetMotionVectors().GetResource() && m_commandContext.WaitForGPU())
        {
            m_dlssInputPreparationPass.ReleaseTextures();
        }
    }
}

CameraJitterStatus Renderer::NextCameraJitter(bool applies, std::uint32_t phaseCount)
{
    CameraJitterStatus jitter{};

    if(!applies)
    {
        return jitter;
    }

    if(phaseCount != m_cameraJitterPhaseCount)
    {
        m_cameraJitterPhaseCount = phaseCount;
        m_cameraJitterPhase = 0;
    }

    jitter.active = true;
    jitter.phase = m_cameraJitterPhase;
    jitter.phaseCount = phaseCount;

    if(m_cameraJitterForTesting)
    {
        jitter.x = (*m_cameraJitterForTesting)[0];
        jitter.y = (*m_cameraJitterForTesting)[1];
    }
    else
    {
        // Indices 1..8: index 0 would be the pixel's corner in both bases.
        jitter.x = Halton(jitter.phase + 1u, 2u) - 0.5f;
        jitter.y = Halton(jitter.phase + 1u, 3u) - 0.5f;
    }

    m_cameraJitterPhase = (m_cameraJitterPhase + 1u) % phaseCount;
    return jitter;
}

void Renderer::ApplyPathTraceDenoiserStages(const PathTraceDenoiserStages& stages)
{
    const PathTraceDenoiserStages previous = m_pathTraceDenoiserStages;
    m_pathTraceDenoiserStages = stages;

    // Only a change applies, so diagnostics that set the members directly keep their values.
    if(stages.spatialFilter != previous.spatialFilter)
    {
        m_pathTraceSpatialBypass = stages.spatialFilter == PathTraceSpatialFilter::Off;

        if(!m_pathTraceSpatialBypass)
        {
            m_pathTraceSpatialIterations = stages.spatialFilter == PathTraceSpatialFilter::Atrous1 ? 1u :
                stages.spatialFilter == PathTraceSpatialFilter::Atrous2 ? 2u : 0u;
        }
    }

    if(stages.extendedDiffuseHistory != previous.extendedDiffuseHistory)
    {
        m_pathTraceExtendedDiffuse = stages.extendedDiffuseHistory;
        m_pathTraceExtendedFrames = 0;
    }

    // The spatial pass never writes history, so only the temporal choice and a reset discard it.
    if(stages.temporalHistory != previous.temporalHistory || stages.historyResetRequest != previous.historyResetRequest)
    {
        m_pathTraceTemporalHistoryDisabled = !stages.temporalHistory;
        m_pathTraceTemporalPass.InvalidateHistory();
        // NRD.4B: the explicit history reset (historyResetRequest) restarts REBLUR too, and (NRD.RELAX.3) RELAX.
        m_nrdPathTraceHistoryValid = false;
        m_nrdRelaxHistoryValid = false;
        m_nrdHybridHistoryValid = false;
    }
}

void Renderer::UpdateSharcStatus(unsigned int frameSlot)
{
    m_sharcStatus.active = m_sharcSettings.enabled && m_radianceCacheD6.HasResources() && m_sharcRenderedLastFrame;
    m_sharcStatus.trainedFrames = m_radianceCacheD6.GetTrainedFrames();
    m_sharcStatus.cacheBytes = 0;

    if(m_radianceCacheD6.HasResources())
    {
        m_sharcStatus.cacheBytes = RadianceCacheD6::ResourceBytes(m_radianceCacheD6Settings) +
            m_radianceCacheD6.GetRecordBufferBytes();
    }

    std::uint32_t words[RadianceCacheD6::ReadbackWords]{};
    m_sharcStatus.statisticsValid = m_sharcStatus.active && m_sharcSettings.statistics &&
        m_radianceCacheD6.ReadStatistics(frameSlot, words);

    if(m_sharcStatus.statisticsValid)
    {
        m_sharcStatus.eligibleQueries = words[RadianceCacheD6::QueryEligible];
        m_sharcStatus.lookupHits = words[RadianceCacheD6::QueryFound];
        m_sharcStatus.substitutions = words[RadianceCacheD6::QueryTerminated];
        m_sharcStatus.occupiedEntries = words[RadianceCacheD6::QueryWords + RadianceCacheD6::StatisticOccupied];
    }
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::PrepareRadianceCacheQuery(ID3D12GraphicsCommandList* commandList, bool extract,
    bool splitReSTIRDI, bool rawDiffuseHitDistance)
{
    TransitionTiming::Scope er1Scope("PrepareRadianceCacheQuery", 0.5);
    std::string failure;

    if(!m_radianceCacheD6.UsesQueryLibrary() || !m_pathTraceReferencePass.EnsureRadianceCacheLearning(m_graphicsDevice,
        m_shaderLibrary, failure, RadianceCacheQueryLibrary(extract, splitReSTIRDI, rawDiffuseHitDistance)))
    {
        return 0;
    }

    if(m_radianceCacheD6.QueryCounts() &&
        !m_radianceCacheD6.EnsureQueryStatistics(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap))
    {
        return 0;
    }

    // SHARC.6A: extraction frames with the history prior write it for the temporal pass.
    Texture* prior = nullptr;

    if(extract && m_pathTraceTemporal && m_radianceCacheD6.UsesHistoryPrior())
    {
        prior = m_pathTraceReferencePass.EnsureHistoryPriorTexture(m_graphicsDevice.GetDevice(),
            m_resourceDescriptorHeap);
    }

    if(prior && m_radianceCacheD6.QueryCounts())
    {
        m_radianceCacheD6.EnsurePriorCounters(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap);
    }

    if(prior)
    {
        prior->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    m_radianceCacheD6.SetHistoryPriorOutput(prior ? prior->GetUAVIndex() : 0u);
    const D3D12_GPU_VIRTUAL_ADDRESS address =
        m_radianceCacheD6.RecordQueryBegin(commandList, m_uploadBuffer, m_sceneLightingEpoch.GetStatus());
    m_sharcHistoryPriorWritten = prior && address != 0;
    m_sharcHistoryPriorBinding = m_sharcHistoryPriorWritten ? address : 0;
    m_radianceCacheD6.SetHistoryPriorOutput(0u);
    return address;
}

bool Renderer::RecordPathTraceReference(
    ID3D12GraphicsCommandList* commandList,
    unsigned int frameIndex,
    PathTracePrimary primary,
    const Matrix4& view,
    const Matrix4& projection,
    const Matrix4& cameraProjection,
    const Matrix4& inverseViewProjection,
    const Vector3& cameraPosition,
    const FrameLighting& frameLighting,
    D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstantsAddress,
    const PathTraceReferenceSettings& settings)
{
    m_nrdPathTraceDiagnosticRanLastFrame = false;
    m_pathTraceRawDiffuseHitDistanceLastFrame = false;
    m_nrdRelaxPrepareDiagnosticRanLastFrame = false;

    // Stage 1: decide whether the running mean is still valid for this frame's inputs. The camera is identified by its
    // unjittered projection, so jitter alone never restarts accumulation (UPSCALING.2B).
    PathTraceAccumulationKey key{};
    key.primary = primary;
    key.view = view;
    key.projection = cameraProjection;
    key.width = m_sceneWidth;
    key.height = m_sceneHeight;
    key.settings = settings;
    key.sun = frameLighting.sun;
    key.lights = frameLighting.table;
    key.topLevelAddress = m_rayTracingScene.GetTopLevel().GetGPUAddress();
    key.topLevelRecording = m_rayTracingScene.GetTLASRecordingIdentity();
    key.materialRevision = m_materialPublication.revision;
    key.sceneResourceLifetime = m_sceneResourceLifetime;
    key.environmentTextureIndex = m_environment.GetSourceTexture().GetDescriptorIndex();
    key.environmentConditionalCDFIndex = m_environment.GetImportanceConditionalCDF().GetDescriptorIndex();
    key.environmentTotalWeight = m_environment.GetImportanceTotalWeight();

    // SHARC.4B: this frame's material table, TLAS update and environment are recorded by now.
    LatchSceneLighting(key);

    // SHARC.4C: the D6 cache's generation for the latched epoch, ahead of any cache access in this list.
    m_radianceCacheLearnedLastFrame = false;

    if(m_radianceCacheD6.HasResources())
    {
        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::RadianceCacheGeneration);
        m_radianceCacheD6.RecordGeneration(commandList, m_uploadBuffer, m_sceneLightingEpoch.GetStatus());
        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::RadianceCacheGeneration);
    }

    const bool keyChanged = !m_pathTraceAccumulationKey || !SamePathTraceAccumulationKey(*m_pathTraceAccumulationKey, key);

    if(m_pathTraceResetPending || keyChanged)
    {
        m_referenceRayCounts.Invalidate();
        m_pathTraceStatus.accumulatedSamples = 0;
        ++m_pathTraceStatus.accumulationResets;
        m_pathTraceResetPending = false;

        // RTXPT D.1A: each run draws its own sequence. The serial advances only once a frame is submitted, so a
        // recording that fails and is retried latches the same seed again.
        if(m_pathTraceRandomSeed)
        {
            m_pathTraceRunSeed = *m_pathTraceRandomSeed;
        }
        else
        {
            m_pathTraceRunSeed = PathTraceSequenceSeed(m_gpuProfilerFrameSerial - m_pathTraceSequenceOrigin);
        }
    }

    m_pathTraceAccumulationKey = key;

    const std::uint32_t accumulated = m_pathTraceStatus.accumulatedSamples;
    const std::uint32_t remaining = settings.maxAccumulatedSamples > accumulated ?
        settings.maxAccumulatedSamples - accumulated : 0u;
    // UPSCALING.3C: DLSS mode never holds a converged image; it traces every frame. NRD.4B: neither does NRD REBLUR,
    // which needs a fresh sample every frame (a held frame would break its history).
    // NRD.RELAX.4: RELAX, like REBLUR, needs fresh samples every frame (its history, not the progressive mean, converges).
    // HYBRID.5: so does the hybrid.
    const bool traceEveryFrame = m_dlssModeThisFrame || m_pathTraceDenoiserMode == PathTraceDenoiserMode::NrdReblur ||
        m_pathTraceDenoiserMode == PathTraceDenoiserMode::NrdRelax ||
        m_pathTraceDenoiserMode == PathTraceDenoiserMode::NrdHybrid;
    const std::uint32_t frameSamples = traceEveryFrame ? settings.samplesPerFrame :
        (std::min)(settings.samplesPerFrame, remaining);

    // RTXPT.3 R3.5: a ReSTIR DI frame that traces nothing writes no reservoirs, so the committed ones would no
    // longer belong to the immediately preceding frame. They are dropped.
    if(primary == PathTracePrimary::RasterReSTIRDI && frameSamples == 0)
    {
        m_rtxdiResources.InvalidateHistory();
    }

    m_pathTraceExtractedLastFrame = false;
    m_pathTraceTemporalRanLastFrame = false;
    m_nrdPathTracePresentedLastFrame = false;
    m_nrdRelaxPresentedLastFrame = false;
    m_nrdHybridPresentedLastFrame = false;
    m_pathTraceSpatialRanLastFrame = false;
    m_pathTraceSpatialRequested = false;
    m_pathTraceScalarViewLastFrame = false;
    m_pathTraceSpatialIterationsLastFrame = 0;

    if(primary != PathTracePrimary::Camera || frameSamples == 0 || !m_debugUI.ReferenceRayCountsEnabled())
        m_referenceRayCounts.Invalidate();
    m_debugUI.SetReferenceRayCounts(m_referenceRayCounts.Get());

    // Stage 2: one progressive iteration. A converged image is held without tracing.
    if(frameSamples > 0)
    {
#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
        // Held converged frames keep the last diagnostic in sceneColor; a new dispatch must prove it wrote one.
        m_sharcDiagnosticPresentedThisFrame = false;
#endif
        constexpr D3D12_RESOURCE_STATES shaderReadState =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        m_environment.GetSourceTexture().Transition(commandList, shaderReadState);
        m_materialTables.GetMissingEnergyTexture().Transition(commandList, shaderReadState);
        m_materialTables.GetAverageTexture().Transition(commandList, shaderReadState);

        PathTraceReferenceConstants constants{};
        constants.inverseViewProjection = inverseViewProjection;
        constants.resolution[0] = m_sceneWidth;
        constants.resolution[1] = m_sceneHeight;
        constants.accumulatedSamples = accumulated;
        constants.samplesPerFrame = frameSamples;
        constants.maxPathDepth = settings.maxPathDepth;
        constants.russianRouletteDepth = settings.russianRouletteDepth;
        if(primary == PathTracePrimary::Camera && m_debugUI.ReferenceRayCountsEnabled())
        {
            ReferenceRayCountResult metadata{};
            metadata.frame = m_gpuProfilerFrameSerial;
            metadata.width = m_sceneWidth;
            metadata.height = m_sceneHeight;
            metadata.spp = frameSamples;
            metadata.depth = settings.maxPathDepth;
            constants.referenceRayCountIndex = m_referenceRayCounts.Begin(m_graphicsDevice.GetDevice(),
                m_resourceDescriptorHeap, commandList, frameIndex, m_activeRecordingGeneration, metadata);
            m_debugUI.SetReferenceRayCounts(m_referenceRayCounts.Get());
        }

        constants.instanceDataIndex = m_rayTracingScene.GetInstanceDataDescriptorIndex();
        constants.materialBufferIndex = CurrentMaterialBuffer().GetDescriptorIndex();
        constants.environmentTextureIndex = key.environmentTextureIndex;
        constants.environmentConditionalCDFIndex = key.environmentConditionalCDFIndex;
        constants.environmentMarginalCDFIndex = m_environment.GetImportanceMarginalCDF().GetDescriptorIndex();
        constants.environmentWidth = m_environment.GetImportanceWidth();
        constants.environmentHeight = m_environment.GetImportanceHeight();
        constants.environmentTotalWeight = key.environmentTotalWeight;
        constants.materialTableIndex = m_materialTables.GetMissingEnergyTexture().GetDescriptorIndex();
        constants.materialAverageTableIndex = m_materialTables.GetAverageTexture().GetDescriptorIndex();
        constants.materialTableCosineMapping = static_cast<std::uint32_t>(m_materialTables.GetCosineMapping());

        // DirectionalLight::direction is the direction light travels.
        const DirectionalLight& sun = frameLighting.sun;
        const Vector3 directionToSun = sun.direction.Normalized() * -1.0f;
        const Vector3 sunIrradiance = sun.color * sun.intensity;
        constants.sunDirectionToLight[0] = directionToSun.x;
        constants.sunDirectionToLight[1] = directionToSun.y;
        constants.sunDirectionToLight[2] = directionToSun.z;
        constants.sunCosAngularRadius = sun.angularRadius > 0.0f ? std::cos(sun.angularRadius) : 1.0f;
        constants.sunIrradiance[0] = sunIrradiance.x;
        constants.sunIrradiance[1] = sunIrradiance.y;
        constants.sunIrradiance[2] = sunIrradiance.z;
        constants.randomSeed = m_pathTraceRunSeed;

        constants.inverseCameraRelativeViewProjection = CameraRelativeInverseViewProjection(view, projection);
        constants.cameraPosition[0] = cameraPosition.x;
        constants.cameraPosition[1] = cameraPosition.y;
        constants.cameraPosition[2] = cameraPosition.z;
        constants.gBufferDepthIndex = m_gBuffer.GetDepth().GetSRVIndex();
        constants.gBufferMaterialIndex = m_gBuffer.GetMaterial().GetSRVIndex();
        constants.gBufferSurfaceIndex = m_gBuffer.GetSurface().GetSRVIndex();
        constants.gBufferGeometricNormalIndex = m_gBuffer.GetGeometricNormal().GetSRVIndex();

        // EndGeometryPass already left the G-buffer readable; restating it keeps the raster primary's
        // reads independent of that ordering.
        if(primary == PathTracePrimary::Raster || primary == PathTracePrimary::RasterReSTIRDI)
        {
            m_gBuffer.GetDepth().Transition(commandList, shaderReadState);
            m_gBuffer.GetMaterial().Transition(commandList, shaderReadState);
            m_gBuffer.GetSurface().Transition(commandList, shaderReadState);
            m_gBuffer.GetGeometricNormal().Transition(commandList, shaderReadState);
        }

        // RTXPT.3: the per-sample direct radiance the ReSTIR DI passes exchange, sized for the configured samples
        // per frame (every frame of a configuration records at most that many). Nothing is recorded without it.
        const bool splitReSTIRDI = primary == PathTracePrimary::RasterReSTIRDI;

        if(splitReSTIRDI)
        {
            constants.splitDirectRadianceIndex = m_pathTraceReferencePass.EnsureSplitDirectRadiance(
                m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, settings.samplesPerFrame,
                m_commandContext.GetLastSubmissionFenceValue(), m_commandContext.GetCompletedFenceValue());

            if(constants.splitDirectRadianceIndex == UINT_MAX)
            {
                return false;
            }
        }

        // RTXPT D.1B.2: diagnostics-requested signal extraction records the extraction exports, which accumulate
        // exactly as the production ones. Anything it cannot create leaves the frame on the production exports.
        bool extract = false;
        // NRD.RELAX.2B: whether this frame records the raw-distance extraction pipelines.
        bool rawExtraction = false;

        if(m_pathTraceSignalExtraction &&
            (primary == PathTracePrimary::Raster || primary == PathTracePrimary::RasterReSTIRDI))
        {
            std::string extractionFailure;
            extract = m_pathTraceReferencePass.EnsureSignalExtraction(m_graphicsDevice, m_shaderLibrary,
                m_resourceDescriptorHeap, primary, extractionFailure);

            if(extract && splitReSTIRDI)
            {
                constants.splitDirectDiffuseIndex = m_pathTraceReferencePass.EnsureSplitDirectDiffuse(
                    m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, settings.samplesPerFrame,
                    m_commandContext.GetLastSubmissionFenceValue(), m_commandContext.GetCompletedFenceValue());
                extract = constants.splitDirectDiffuseIndex != UINT_MAX;
                extractionFailure = extract ? "" : "The split direct diffuse radiance could not be allocated.";
            }

            if(extract)
            {
                constants.diffuseOutputIndex = m_pathTraceReferencePass.GetDiffuseTexture().GetUAVIndex();
                constants.specularOutputIndex = m_pathTraceReferencePass.GetSpecularTexture().GetUAVIndex();

                // NRD.RELAX.2: the raw diffuse hit distance, beside the unchanged extraction outputs. NRD.RELAX.2B: it
                // comes from the raw-distance extraction pipelines (their own libraries), created here on first use.
                // Without them or the texture the frame records the legacy extraction and the RELAX preparation does
                // not run.
                if(m_nrdHybridDispatchDiagnostic || m_nrdRelaxPrepareDiagnostic || m_nrdRelaxDispatchDiagnostic ||
                    m_pathTraceDenoiserMode == PathTraceDenoiserMode::NrdRelax ||
                    m_pathTraceDenoiserMode == PathTraceDenoiserMode::NrdHybrid)
                {
                    std::string rawFailure;
                    Texture* rawDiffuseHitDistance = nullptr;

                    if(m_pathTraceReferencePass.EnsureSignalExtraction(m_graphicsDevice, m_shaderLibrary,
                        m_resourceDescriptorHeap, primary, rawFailure, true))
                    {
                        rawDiffuseHitDistance = m_pathTraceReferencePass.EnsureRawDiffuseHitDistanceTexture(
                            m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap);
                    }
                    else if(rawFailure != m_rawDiffuseHitDistanceFailure)
                    {
                        // NRD.RELAX.4: a sticky failure is reported once per distinct reason, not every frame.
                        Log::Error("Raw diffuse hit distance extraction is unavailable: " + rawFailure);
                        m_rawDiffuseHitDistanceFailure = rawFailure;
                    }

                    if(rawDiffuseHitDistance)
                    {
                        m_rawDiffuseHitDistanceFailure.clear();
                        rawDiffuseHitDistance->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        constants.rawDiffuseHitDistanceIndex = rawDiffuseHitDistance->GetUAVIndex();
                        rawExtraction = true;
                    }
                }
            }
            else
            {
                Log::Error("Path tracing signal extraction is unavailable: " + extractionFailure);
            }
        }

        m_pathTraceExtractedLastFrame = extract;
        const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress = m_uploadBuffer.Allocate(
            &constants, sizeof(PathTraceReferenceConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(constantsAddress == 0)
        {
            Log::Error("Failed to allocate path tracing constants.");
            return false;
        }

        // RTXPT.3: the ReSTIR DI variant's RTXDI constants and reservoir buffer.
        D3D12_GPU_VIRTUAL_ADDRESS rtxdiConstantsAddress = 0;
        const bool usesRtxdi = primary == PathTracePrimary::RasterReSTIRDI;

        if(usesRtxdi && !PrepareRtxdiFrame(commandList, frameLighting, constants, view, rtxdiConstantsAddress))
        {
            return false;
        }

        // The motion vectors are read by the ReSTIR DI variant's reprojection.
        if(usesRtxdi)
        {
            m_gBuffer.GetMotion().Transition(commandList, shaderReadState);
        }

        // SHARC.4D learning: a usable cache's Raster Primary frames without extraction trace through the cache
        // library, which renders the same image and inserts D6 training samples. Every other frame is unchanged.
        // SHARC.4D-R (diagnostics): with sparse training the image renders with the reference library, or the query
        // library, and a separate training dispatch follows it. SHARC.4E.1: sparse training does not touch the image's
        // dispatch, so it also runs on the denoiser's extraction frames (the render library's extraction export).
        D3D12_GPU_VIRTUAL_ADDRESS radianceCacheAddress = 0;
        D3D12_GPU_VIRTUAL_ADDRESS renderCacheAddress = 0;
        m_sharcHistoryPriorWritten = false;
        PathTraceRadianceCacheLibrary renderCacheLibrary = PathTraceRadianceCacheLibrary::Inline;
        const bool sparseTraining = m_radianceCacheD6.GetInsertion() == RadianceCacheD6::Insertion::Sparse;

        // SHARC.5B: sparse SHARC also serves ReSTIR DI frames, through the indirect pass (the direct pass is unchanged).
        const bool cachePrimary = primary == PathTracePrimary::Raster ||
            (sparseTraining && primary == PathTracePrimary::RasterReSTIRDI);

        if(m_radianceCacheD6.IsUsable() && cachePrimary && (!extract || sparseTraining))
        {
            m_radianceCacheD6.SetSpatialAnchor(cameraPosition.x, cameraPosition.y, cameraPosition.z);
            radianceCacheAddress = PrepareRadianceCacheLearning(commandList, settings.samplesPerFrame);

            if(!sparseTraining)
            {
                renderCacheAddress = radianceCacheAddress;
                renderCacheLibrary = m_radianceCacheLearningLibrary;
            }
            else
            {
                // Without a query binding (0) the reference pipeline renders.
                renderCacheAddress = PrepareRadianceCacheQuery(commandList, extract, splitReSTIRDI, rawExtraction);
                renderCacheLibrary = RadianceCacheQueryLibrary(extract, splitReSTIRDI, rawExtraction);
            }
        }

        bool referenceDispatched = false;
        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::PathTraceReference);

        if(splitReSTIRDI)
        {
            m_pathTraceReferencePass.ExecuteReSTIRDISplit(m_commandContext.GetRayTracingCommandList(),
                m_gpuTimestampProfiler, frameIndex, m_resourceDescriptorHeap, m_rayTracingScene, constantsAddress,
                sceneLightConstantsAddress, m_sceneWidth, m_sceneHeight, rtxdiConstantsAddress, extract,
                renderCacheAddress, renderCacheLibrary, rawExtraction);
        }
        else
        {
            referenceDispatched = m_pathTraceReferencePass.Execute(primary, m_commandContext.GetRayTracingCommandList(),
                m_resourceDescriptorHeap, m_rayTracingScene, constantsAddress, sceneLightConstantsAddress, m_sceneWidth,
                m_sceneHeight, extract, renderCacheAddress, renderCacheLibrary, rawExtraction);
        }

        if(primary == PathTracePrimary::Camera && m_debugUI.ReferenceRayCountsEnabled())
        {
            if(referenceDispatched)
                m_referenceRayCounts.Copy(commandList, frameIndex, m_activeRecordingGeneration);
            else
                m_referenceRayCounts.Cancel(commandList, frameIndex, m_activeRecordingGeneration);
        }

        // NRD RELAX R.9B diagnostics: the confidence probes, on raster ReSTIR DI frames under the RELAX denoiser.
        if(m_nrdConfidenceProbesEnabled && usesRtxdi && rtxdiConstantsAddress != 0 &&
            m_pathTraceDenoiserMode == PathTraceDenoiserMode::NrdRelax)
        {
            RecordNrdConfidenceProbes(commandList, constants, frameLighting, settings, primary,
                sceneLightConstantsAddress, rtxdiConstantsAddress);
        }

        // NRD.RELAX.2B: only a dispatch that actually wrote the raw distance lets this frame read it.
#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
        if(m_pathTraceReferencePass.WroteSharcDiagnostic())
        {
            // Preserve display-only RGB before Engine/NRD resolves overwrite the separate path-trace output.
            // No subsequent path-traced stage writes sceneColor; the final copy is skipped below.
            m_pathTraceReferencePass.CopyToSceneColor(commandList, m_sceneColor);
            m_sharcDiagnosticPresentedThisFrame = true;
        }
#endif
        m_pathTraceRawDiffuseHitDistanceLastFrame = m_pathTraceReferencePass.WroteRawDiffuseHitDistance();

        // NRD.4B: REBLUR denoises this frame's extraction in place of the Engine stages. Everything that can fail is
        // done here, before anything is recorded; a frame it cannot denoise falls back to the Engine stages below.
        const bool nrdFrame = m_pathTraceDenoiserMode == PathTraceDenoiserMode::NrdReblur &&
            m_pathTraceExtractedLastFrame && BeginPathTraceNrdFrame(primary, view, cameraProjection, frameLighting, settings);

        // NRD.RELAX.4: the same for RELAX, which also needs this frame's raw diffuse hit distance. Without it, or when
        // anything fallible fails, the Engine stages present this frame.
        bool relaxFrame = false;

        if(m_pathTraceDenoiserMode == PathTraceDenoiserMode::NrdRelax && m_pathTraceExtractedLastFrame)
        {
            if(!m_pathTraceRawDiffuseHitDistanceLastFrame)
            {
                ReportNrdRelaxUnavailable("the raw diffuse hit distance was not produced");
            }
            else
            {
                relaxFrame = BeginNrdRelaxFrame(primary, view, cameraProjection, frameLighting, settings);
            }
        }

        // HYBRID.5: the same for the hybrid (RELAX diffuse and REBLUR specular from one instance).
        bool hybridFrame = false;

        if(m_pathTraceDenoiserMode == PathTraceDenoiserMode::NrdHybrid && m_pathTraceExtractedLastFrame)
        {
            if(!m_pathTraceRawDiffuseHitDistanceLastFrame)
            {
                ReportNrdHybridUnavailable("the raw diffuse hit distance was not produced");
            }
            else
            {
                hybridFrame = BeginNrdHybridFrame(primary, view, cameraProjection, frameLighting, settings);
            }
        }

        // RTXPT D.2B: temporal reconstruction consumes this frame's extraction outputs, so it runs only on frames
        // that extracted, before the presentation copy below. It is timed inside the path-tracing region, which
        // therefore includes it in temporal frames (diagnostics only; without it the region is unchanged).
        if(m_pathTraceTemporal && m_pathTraceExtractedLastFrame && !nrdFrame && !relaxFrame && !hybridFrame)
        {
            // RTXPT D.3B: decided first, because the spatial pass replaces the temporal pass's own presentation. A
            // D.3B.0 diagnostic view also runs it, for display only.
            m_pathTraceVisualization = m_debugUI.GetPathTraceDenoiserVisualization();
            const bool diagnosticView = m_pathTraceVisualization.view != PathTraceDenoiserView::Normal;
            m_pathTraceSpatialRequested = ((m_pathTraceSpatial && !m_pathTraceSpatialBypass) || diagnosticView) &&
                m_pathTraceSpatialPass.Initialize(m_graphicsDevice, m_shaderLibrary);
            m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::PathTraceTemporal);
            m_pathTraceTemporalRanLastFrame = RecordPathTraceTemporal(commandList, primary, view, projection,
                cameraPosition, frameLighting, settings);
            m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::PathTraceTemporal);
        }

        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::PathTraceReference);

        // SHARC.4D: the frame's samples become cache history after the dispatch that inserted them. SHARC.4D-Q: a
        // deferred learning frame inserts its records first.
        if(radianceCacheAddress != 0)
        {
            if(sparseTraining)
            {
                m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex,
                    GpuTimestampProfiler::Region::RadianceCacheTraining);
                m_pathTraceReferencePass.ExecuteRadianceCacheTraining(m_commandContext.GetRayTracingCommandList(),
                    m_resourceDescriptorHeap, m_rayTracingScene, constantsAddress, sceneLightConstantsAddress,
                    radianceCacheAddress,
                    static_cast<std::uint32_t>(m_radianceCacheD6.TrainingThreads(m_sceneWidth, m_sceneHeight)));
                m_gpuTimestampProfiler.EndRegion(commandList, frameIndex,
                    GpuTimestampProfiler::Region::RadianceCacheTraining);
            }

            if(m_radianceCacheD6.GetLearningBinding().deferred != 0)
            {
                m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex,
                    GpuTimestampProfiler::Region::RadianceCacheInsertion);
                m_radianceCacheD6.RecordDeferredInsertion(commandList, m_uploadBuffer);
                m_gpuTimestampProfiler.EndRegion(commandList, frameIndex,
                    GpuTimestampProfiler::Region::RadianceCacheInsertion);
            }

            m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::RadianceCacheResolve);
            m_radianceCacheLearnedLastFrame = m_radianceCacheD6.RecordResolve(commandList, m_uploadBuffer,
                m_sceneLightingEpoch.GetStatus());
            m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::RadianceCacheResolve);
        }

        m_sharcRenderedLastFrame = renderCacheAddress != 0 &&
            (renderCacheLibrary == PathTraceRadianceCacheLibrary::Render ||
            renderCacheLibrary == PathTraceRadianceCacheLibrary::RenderExtraction ||
            renderCacheLibrary == PathTraceRadianceCacheLibrary::RenderSplitIndirect ||
            renderCacheLibrary == PathTraceRadianceCacheLibrary::RenderSplitIndirectExtraction ||
            renderCacheLibrary == PathTraceRadianceCacheLibrary::RenderExtractionRawDiffuseHitDistance ||
            renderCacheLibrary == PathTraceRadianceCacheLibrary::RenderSplitIndirectExtractionRawDiffuseHitDistance);

        // SHARC.4E statistics: this frame's counters, read back when its slot comes round again.
        if(m_sharcSettings.enabled && m_sharcSettings.statistics && renderCacheAddress != 0)
        {
            m_radianceCacheD6.RecordStatisticsReadback(m_graphicsDevice.GetDevice(), commandList, frameIndex);
        }

        if(m_pathTraceSpatialRequested && m_pathTraceTemporalRanLastFrame)
        {
            m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::PathTraceSpatial);
            RecordPathTraceSpatial(commandList, view, projection, m_pathTraceVisualization);
            m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::PathTraceSpatial);
            m_pathTraceSpatialRanLastFrame = true;
            m_pathTraceScalarViewLastFrame = IsScalarPathTraceDenoiserView(m_pathTraceVisualization.view);
        }

        // NRD.4B: REBLUR's denoised radiance replaces the presentation's surface pixels; the background keeps the path
        // tracer's own value.
        if(nrdFrame)
        {
            RecordPathTraceNrd(commandList, frameIndex, view, inverseViewProjection, cameraPosition);
            m_nrdPathTracePresentedLastFrame = true;
        }

        // NRD.RELAX.4: RELAX's denoised radiance replaces the presentation's surface pixels, as REBLUR's does. A dispatch
        // that is not recorded leaves the path tracer's own frame mean presented (never an earlier RELAX output) and
        // commits no history.
        if(relaxFrame)
        {
            m_nrdRelaxPresentedLastFrame = RecordNrdRelax(commandList, frameIndex, view, inverseViewProjection,
                cameraPosition, &m_pathTraceReferencePass.GetOutputTexture());
        }

        // HYBRID.5: the hybrid's denoised radiance replaces the presentation's surface pixels, as RELAX's does. Only a
        // complete pair is resolved; otherwise the path tracer's own frame mean stays presented and no history commits.
        if(hybridFrame)
        {
            m_nrdHybridPresentedLastFrame = RecordNrdHybrid(commandList, frameIndex, view, inverseViewProjection,
                cameraPosition, &m_pathTraceReferencePass.GetOutputTexture());
        }

        // NRD.3 diagnostics: reads this frame's raw extraction and G-buffer and writes only its own textures.
        if(m_nrdPathTraceDiagnostic && m_pathTraceExtractedLastFrame)
        {
            m_nrdPathTraceDiagnosticRanLastFrame = RecordNrdPathTraceDiagnostic(commandList, frameIndex, view,
                inverseViewProjection, cameraPosition);
        }

        // NRD.RELAX.2 diagnostics: the same reads, its own textures; nothing is dispatched to RELAX.
        if(m_nrdRelaxPrepareDiagnostic && m_pathTraceExtractedLastFrame && m_pathTraceRawDiffuseHitDistanceLastFrame)
        {
            m_nrdRelaxPrepareDiagnosticRanLastFrame = RecordNrdRelaxPrepareDiagnostic(commandList, view,
                inverseViewProjection, cameraPosition);
        }

        // NRD.RELAX.3 diagnostics: RELAX on this frame's RELAX inputs, into its own textures only. A frame without a
        // fresh raw diffuse hit distance records nothing, which is a gap for RELAX's history.
        // NRD.RELAX.4: never as well as the selected RELAX, which already resolves into the diagnostic texture.
        if(m_nrdRelaxDispatchDiagnostic && m_pathTraceExtractedLastFrame && m_pathTraceRawDiffuseHitDistanceLastFrame &&
            m_pathTraceDenoiserMode != PathTraceDenoiserMode::NrdRelax)
        {
            RecordNrdRelaxDispatchDiagnostic(commandList, frameIndex, primary, view, cameraProjection,
                inverseViewProjection, cameraPosition, frameLighting, settings);
        }

        // HYBRID.5: never as well as the selected hybrid, which owns the same instance (one NewFrame per frame).
        if(m_nrdHybridDispatchDiagnostic && !m_nrdHybridSkipFrame && m_pathTraceExtractedLastFrame &&
            m_pathTraceRawDiffuseHitDistanceLastFrame && m_pathTraceDenoiserMode != PathTraceDenoiserMode::NrdHybrid)
        {
            RecordNrdHybridDiagnostic(commandList, frameIndex, primary, view, cameraProjection,
                inverseViewProjection, cameraPosition, frameLighting, settings);
        }

        if(usesRtxdi)
        {
            // Orders this frame's reservoir writes before any later access to the buffer.
            D3D12_RESOURCE_BARRIER reservoirBarrier{};
            reservoirBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            reservoirBarrier.UAV.pResource = m_rtxdiResources.GetReservoirBuffer().GetResource();
            commandList->ResourceBarrier(1, &reservoirBarrier);
        }
    }

    // A path-tracing frame without temporal processing (disabled, no extraction, a held converged image or a
    // failure above) breaks the chain of reprojected frames: the next temporal frame starts again.
    if(!m_pathTraceTemporalRanLastFrame)
    {
        m_pathTraceTemporalPass.InvalidateHistory();
    }

    m_pathTracePendingSamples = frameSamples;

    // Stage 3: present the running mean through the unchanged tone-map and overlay path.
#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
    if(!m_sharcDiagnosticPresentedThisFrame)
#endif
    m_pathTraceReferencePass.CopyToSceneColor(commandList, m_sceneColor);
    return true;
}

Renderer::PathTraceTemporalKey Renderer::MakePathTraceTemporalKey(
    PathTracePrimary primary,
    const FrameLighting& frameLighting,
    const PathTraceReferenceSettings& settings)
{
    PathTraceTemporalKey key;
    key.primary = primary;
    key.samplesPerFrame = settings.samplesPerFrame;
    key.maxPathDepth = settings.maxPathDepth;
    key.russianRouletteDepth = settings.russianRouletteDepth;
    key.sun = frameLighting.sun;
    key.lights = frameLighting.table;
    return key;
}

bool Renderer::SamePathTraceTemporalKey(const PathTraceTemporalKey& previous, const PathTraceTemporalKey& key)
{
    bool sameLights = previous.lights.lightCount == key.lights.lightCount;

    for(std::uint32_t index = 0; sameLights && index < key.lights.lightCount; ++index)
    {
        sameLights = previous.lights.lights[index] == key.lights.lights[index];
    }

    const bool sameSun = std::memcmp(&previous.sun.direction, &key.sun.direction, sizeof(Vector3)) == 0 &&
        std::memcmp(&previous.sun.color, &key.sun.color, sizeof(Vector3)) == 0 &&
        previous.sun.intensity == key.sun.intensity && previous.sun.angularRadius == key.sun.angularRadius;
    return previous.primary == key.primary && previous.samplesPerFrame == key.samplesPerFrame &&
        previous.maxPathDepth == key.maxPathDepth && previous.russianRouletteDepth == key.russianRouletteDepth &&
        sameSun && sameLights;
}

bool Renderer::RecordPathTraceTemporal(
    ID3D12GraphicsCommandList* commandList,
    PathTracePrimary primary,
    const Matrix4& view,
    const Matrix4& projection,
    const Vector3& cameraPosition,
    const FrameLighting& frameLighting,
    const PathTraceReferenceSettings& settings)
{
    if(!m_pathTraceTemporalPass.Initialize(m_graphicsDevice, m_shaderLibrary) ||
        !m_pathTraceTemporalPass.EnsureHistory(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, m_sceneWidth,
            m_sceneHeight))
    {
        return false;
    }

    // History built under other settings or lighting is not this frame's signal.
    const PathTraceTemporalKey key = MakePathTraceTemporalKey(primary, frameLighting, settings);

    if(m_pathTraceTemporalKey && !SamePathTraceTemporalKey(*m_pathTraceTemporalKey, key))
    {
        m_pathTraceTemporalPass.InvalidateHistory();
    }

    m_pathTraceTemporalKey = key;

    // Without the previous frame's surfaces no tap can be validated: this frame starts new history.
    PreviousGBufferView previous;

    // DENOISE.1: without temporal history (diagnostics) every frame starts new history.
    if(!TryGetPreviousGBuffer(previous) || m_pathTraceTemporalHistoryDisabled)
    {
        m_pathTraceTemporalPass.InvalidateHistory();
    }

    constexpr D3D12_RESOURCE_STATES shaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Texture& diffuse = m_pathTraceReferencePass.GetDiffuseTexture();
    Texture& specular = m_pathTraceReferencePass.GetSpecularTexture();
    diffuse.Transition(commandList, shaderRead);
    specular.Transition(commandList, shaderRead);
    m_gBuffer.GetDepth().Transition(commandList, shaderRead);
    m_gBuffer.GetSurface().Transition(commandList, shaderRead);
    m_gBuffer.GetGeometricNormal().Transition(commandList, shaderRead);
    m_gBuffer.GetMaterial().Transition(commandList, shaderRead);
    m_gBuffer.GetMotion().Transition(commandList, shaderRead);
    m_pathTraceReferencePass.GetOutputTexture().Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    PathTraceTemporalConstants constants{};
    constants.inverseCameraRelativeViewProjection = CameraRelativeInverseViewProjection(view, projection);
    // Linear depth along each camera's forward axis (row 2 of a left-handed view, as Matrix4::LookAt builds it).
    constants.cameraForward[0] = view.m[2][0];
    constants.cameraForward[1] = view.m[2][1];
    constants.cameraForward[2] = view.m[2][2];
    constants.currentDiffuseIndex = diffuse.GetDescriptorIndex();
    constants.currentSpecularIndex = specular.GetDescriptorIndex();
    constants.depthIndex = m_gBuffer.GetDepth().GetSRVIndex();
    constants.surfaceIndex = m_gBuffer.GetSurface().GetSRVIndex();
    constants.geometricNormalIndex = m_gBuffer.GetGeometricNormal().GetSRVIndex();
    constants.materialIndex = m_gBuffer.GetMaterial().GetSRVIndex();
    constants.motionIndex = m_gBuffer.GetMotion().GetSRVIndex();
    constants.presentationIndex = m_pathTraceReferencePass.GetOutputTexture().GetUAVIndex();
    constants.present = m_pathTraceTemporalPresentation && !m_pathTraceSpatialRequested ? 1u : 0u;

    // With invalid history these are never read; the current frame's own G-buffer stands in.
    const bool havePrevious = m_pathTraceTemporalPass.HasHistory();
    const PreviousGBufferCamera previousCamera = havePrevious ? previous.camera :
        PreviousGBufferCamera{view, projection, constants.inverseCameraRelativeViewProjection, cameraPosition};
    constants.previousInverseCameraRelativeViewProjection = previousCamera.inverseCameraRelativeViewProjection;
    constants.previousCameraForward[0] = previousCamera.view.m[2][0];
    constants.previousCameraForward[1] = previousCamera.view.m[2][1];
    constants.previousCameraForward[2] = previousCamera.view.m[2][2];
    constants.previousCameraOffset[0] = previousCamera.position.x - cameraPosition.x;
    constants.previousCameraOffset[1] = previousCamera.position.y - cameraPosition.y;
    constants.previousCameraOffset[2] = previousCamera.position.z - cameraPosition.z;
    constants.previousDepthIndex = havePrevious ? previous.depthIndex : m_gBuffer.GetDepth().GetSRVIndex();
    constants.previousSurfaceIndex = havePrevious ? previous.surfaceIndex : m_gBuffer.GetSurface().GetSRVIndex();
    constants.previousGeometricNormalIndex = havePrevious ? previous.geometricNormalIndex :
        m_gBuffer.GetGeometricNormal().GetSRVIndex();

    // SHARC.6A: this frame's history prior, when its render dispatch wrote one (the transition orders the writes).
    if(m_sharcHistoryPriorWritten)
    {
        Texture& prior = m_pathTraceReferencePass.GetHistoryPriorTexture();
        prior.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        constants.priorIndex = prior.GetDescriptorIndex();

        if(m_radianceCacheD6.PriorCountedThisFrame())
        {
            constants.priorCountersIndex = m_radianceCacheD6.GetPriorCounters().GetUAVDescriptorIndex();
        }
    }

    // DENOISE.3B: the extended diffuse history restarts unless this frame continues an unchanged scene and view
    // (accumulation not restarted, temporal history valid, the same valid scene-lighting epoch, the previous frame
    // updated it); it is used only after PathTraceExtendedStableFrames such frames.
    m_pathTraceExtendedUsed = false;
    // UPSCALING.3C: DLSS keeps its own history; the extended accumulator is left out in DLSS mode.
    const bool extended = m_pathTraceExtendedDiffuse && !m_dlssModeThisFrame &&
        m_pathTraceTemporalPass.EnsureExtended(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap);

    if(extended)
    {
        const SceneLightingEpoch::Status& epoch = m_sceneLightingEpoch.GetStatus();
        const bool restart = m_pathTraceExtendedFrames == 0 || m_pathTraceStatus.accumulatedSamples == 0 ||
            !m_pathTraceTemporalPass.HasHistory() || !epoch.valid || epoch.epoch != m_pathTraceExtendedEpoch;
        m_pathTraceExtendedFrames = restart ? 1u : m_pathTraceExtendedFrames + 1u;
        m_pathTraceExtendedEpoch = epoch.epoch;
        m_pathTraceExtendedUsed = m_pathTraceExtendedFrames > PathTraceExtendedStableFrames;
        constants.extendedRestart = restart ? 1u : 0u;
        constants.extendedMaximum = PathTraceExtendedMaximum;
        constants.extendedPresent = m_pathTraceExtendedUsed ? 1u : 0u;
    }
    else
    {
        m_pathTraceExtendedFrames = 0;
    }

    m_pathTraceTemporalPass.Execute(commandList, m_uploadBuffer, constants, m_sceneWidth, m_sceneHeight,
        constants.priorIndex != 0 ? m_sharcHistoryPriorBinding : 0, extended);
    return true;
}

NrdPathTracePrepareConstants Renderer::MakeNrdPathTracePrepareConstants(
    ID3D12GraphicsCommandList* commandList,
    const Matrix4& view,
    const Matrix4& inverseViewProjection,
    const Vector3& cameraPosition)
{
    constexpr D3D12_RESOURCE_STATES shaderRead =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    m_gBuffer.GetDepth().Transition(commandList, shaderRead);
    m_gBuffer.GetMaterial().Transition(commandList, shaderRead);
    m_gBuffer.GetSurface().Transition(commandList, shaderRead);
    m_gBuffer.GetGeometricNormal().Transition(commandList, shaderRead);
    m_gBuffer.GetMotion().Transition(commandList, shaderRead);
    Texture& rawDiffuse = m_pathTraceReferencePass.GetDiffuseTexture();
    Texture& rawSpecular = m_pathTraceReferencePass.GetSpecularTexture();
    rawDiffuse.Transition(commandList, shaderRead);
    rawSpecular.Transition(commandList, shaderRead);

    NrdPathTracePrepareConstants constants{};
    constants.inverseViewProjection = inverseViewProjection;
    constants.view = view;
    constants.cameraPosition[0] = cameraPosition.x;
    constants.cameraPosition[1] = cameraPosition.y;
    constants.cameraPosition[2] = cameraPosition.z;
    constants.denoisingRange = PathTraceNrdDenoisingRange;
    constants.radianceScale = m_nrdPathTraceRadianceScale;
    constants.specularHitDistanceMode = m_nrdPathTraceSpecularHitDistanceMode;
    constants.depthIndex = m_gBuffer.GetDepth().GetSRVIndex();
    constants.materialIndex = m_gBuffer.GetMaterial().GetSRVIndex();
    constants.surfaceIndex = m_gBuffer.GetSurface().GetSRVIndex();
    constants.geometricNormalIndex = m_gBuffer.GetGeometricNormal().GetSRVIndex();
    constants.motionIndex = m_gBuffer.GetMotion().GetSRVIndex();
    constants.materialBufferIndex = CurrentMaterialBuffer().GetDescriptorIndex();
    constants.rawDiffuseIndex = rawDiffuse.GetDescriptorIndex();
    constants.rawSpecularIndex = rawSpecular.GetDescriptorIndex();
    return constants;
}

bool Renderer::RecordNrdPathTraceDiagnostic(
    ID3D12GraphicsCommandList* commandList,
    unsigned int frameIndex,
    const Matrix4& view,
    const Matrix4& inverseViewProjection,
    const Vector3& cameraPosition)
{
    if(!m_nrdPathTracePreparePass.Initialize(m_graphicsDevice, m_shaderLibrary) ||
        !m_nrdPathTracePreparePass.EnsureTextures(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, m_sceneWidth,
            m_sceneHeight, true))
    {
        return false;
    }

    const NrdPathTracePrepareConstants constants = MakeNrdPathTracePrepareConstants(commandList, view,
        inverseViewProjection, cameraPosition);

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdPathTracePrepare);
    m_nrdPathTracePreparePass.ExecutePrepare(commandList, m_uploadBuffer, constants, m_sceneWidth, m_sceneHeight);
    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdPathTracePrepare);

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdPathTraceResolve);
    m_nrdPathTracePreparePass.ExecuteResolve(commandList, m_uploadBuffer, constants, m_sceneWidth, m_sceneHeight);
    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdPathTraceResolve);
    return true;
}

bool Renderer::RecordNrdRelaxPrepareDiagnostic(
    ID3D12GraphicsCommandList* commandList,
    const Matrix4& view,
    const Matrix4& inverseViewProjection,
    const Vector3& cameraPosition)
{
    if(!m_nrdRelaxPreparePass.Initialize(m_graphicsDevice, m_shaderLibrary, NrdPathTracePreparePass::Encoding::Relax) ||
        !m_nrdRelaxPreparePass.EnsureTextures(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, m_sceneWidth,
            m_sceneHeight, true))
    {
        return false;
    }

    NrdPathTracePrepareConstants constants = MakeNrdPathTracePrepareConstants(commandList, view, inverseViewProjection,
        cameraPosition);
    Texture& rawDiffuseHitDistance = m_pathTraceReferencePass.GetRawDiffuseHitDistanceTexture();
    rawDiffuseHitDistance.Transition(commandList,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    constants.rawDiffuseHitDistanceIndex = rawDiffuseHitDistance.GetDescriptorIndex();

    m_nrdRelaxPreparePass.ExecutePrepare(commandList, m_uploadBuffer, constants, m_sceneWidth, m_sceneHeight);
    m_nrdRelaxPreparePass.ExecuteResolve(commandList, m_uploadBuffer, constants, m_sceneWidth, m_sceneHeight);
    return true;
}

bool Renderer::EnsureNrdHybridDenoiser()
{
    if(m_nrdHybridDenoiser.IsInitialized())
    {
        return true;
    }

    if(m_nrdHybridCreationFailed)
    {
        return false;
    }

    NrdDenoiser::Description description = PathTraceHybridDescription();

    if(std::exchange(m_nrdHybridFailCreationForTesting, false) ||
        !m_nrdHybridDenoiser.Initialize(m_graphicsDevice.GetDevice(), m_commandContext.GetCommandQueue(),
            m_resourceDescriptorHeap, m_sceneWidth, m_sceneHeight, description))
    {
        // NrdDenoiser::Initialize leaves nothing behind on failure (NRD.RELAX.1).
        m_nrdHybridCreationFailed = true;
        return false;
    }

    ++m_nrdHybridCreations;
    m_nrdHybridInstanceNew = true;
    Log::Info("NRD hybrid diagnostic initialized at " + std::to_string(m_sceneWidth) + "x" +
        std::to_string(m_sceneHeight) + ": persistent " + std::to_string(m_nrdHybridDenoiser.GetPersistentMemoryUsageInMb()) +
        " MiB, transient " + std::to_string(m_nrdHybridDenoiser.GetAliasableMemoryUsageInMb()) + " MiB.");
    return true;
}

void Renderer::ReleaseNrdHybridDenoiser()
{
    TransitionTiming::Scope er1Scope("ReleaseNrdHybridDenoiser");
    if(!m_nrdHybridDenoiser.IsInitialized() && !m_nrdHybridPreparePass.IsInitialized() && !m_nrdHybridCreationFailed)
    {
        return;
    }

    // Lifecycle-only drain. There is no wait between filters or on steady frames.
    if(!m_commandContext.WaitForGPU())
    {
        Log::Error("Failed to wait before releasing the NRD hybrid denoiser; resources retained.");
        return;
    }

    m_nrdHybridDenoiser.Shutdown();
    m_nrdHybridPreparePass.Shutdown();
    m_nrdHybridCreationFailed = false;
    m_nrdHybridHistoryValid = false;
    m_nrdHybridExecutedLastFrame = false;
    m_nrdHybridDispatchedThisFrame = false;
    m_nrdHybridKey.reset();
}

void Renderer::ReportNrdHybridUnavailable(const std::string& reason)
{
    // HYBRID.5, as RELAX's (NRD.RELAX.4): as the selected denoiser the Engine stages present instead; as the diagnostic
    // nothing is presented. Either way the frame is a gap in the hybrid's history.
    const bool selected = m_pathTraceDenoiserMode == PathTraceDenoiserMode::NrdHybrid;
    m_nrdHybridHistoryValid = false;

    if(reason != m_nrdHybridFailure)
    {
        Log::Warning(selected ? "NRD Hybrid could not denoise this frame (" + reason + "); the Engine denoiser is used." :
            "NRD hybrid diagnostic could not run this frame (" + reason + ").");
    }

    m_nrdHybridFailure = reason;

    if(selected)
    {
        m_debugUI.SetPathTraceNrdStatus("NRD Hybrid unavailable (" + reason + "); using Engine.");
    }
}

bool Renderer::BeginNrdHybridFrame(
    PathTracePrimary primary,
    const Matrix4& view,
    const Matrix4& cameraProjection,
    const FrameLighting& frameLighting,
    const PathTraceReferenceSettings& settings)
{
    if(!EnsureNrdHybridDenoiser())
    {
        ReportNrdHybridUnavailable("the hybrid instance could not be created");
        return false;
    }

    // The NRD inputs only; the hybrid has no bypass reconstructions.
    if(!m_nrdHybridPreparePass.Initialize(m_graphicsDevice, m_shaderLibrary, NrdPathTracePreparePass::Encoding::Hybrid) ||
        !m_nrdHybridPreparePass.EnsureTextures(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, m_sceneWidth,
            m_sceneHeight, false))
    {
        ReportNrdHybridUnavailable("the hybrid input textures could not be created");
        return false;
    }

    // RESTART sources as REBLUR's: the temporal key and a frame gap; the shared invalidations clear the flag directly.
    const PathTraceTemporalKey key = MakePathTraceTemporalKey(primary, frameLighting, settings);

    if(!m_nrdHybridKey || !SamePathTraceTemporalKey(*m_nrdHybridKey, key) || !m_nrdHybridExecutedLastFrame)
    {
        m_nrdHybridHistoryValid = false;
    }

    m_nrdHybridKey = key;
    m_nrdHybridAccumulationMode = m_nrdHybridInstanceNew ? nrd::AccumulationMode::CLEAR_AND_RESTART :
        m_nrdHybridHistoryValid ? nrd::AccumulationMode::CONTINUE : nrd::AccumulationMode::RESTART;
    const bool continuing = m_nrdHybridAccumulationMode == nrd::AccumulationMode::CONTINUE;
    const float jitter[2] = {m_cameraJitterStatus.active ? m_cameraJitterStatus.x : 0.0f,
        m_cameraJitterStatus.active ? m_cameraJitterStatus.y : 0.0f};

    // REBLUR's validated conventions (NRD.3 section 8, NRDRelax2.md section 7): unjittered matrices, jitter separately,
    // G-buffer motion scaled by (-1, -1, 1), the preparation's denoising range. Without history the previous frame is
    // this one.
    NrdDenoiser::FrameSettings frameSettings;
    frameSettings.view = view;
    frameSettings.projection = cameraProjection;
    frameSettings.previousView = continuing ? m_nrdHybridPreviousView : view;
    frameSettings.previousProjection = continuing ? m_nrdHybridPreviousProjection : cameraProjection;
    frameSettings.jitter[0] = jitter[0];
    frameSettings.jitter[1] = jitter[1];
    frameSettings.previousJitter[0] = continuing ? m_nrdHybridPreviousJitter[0] : jitter[0];
    frameSettings.previousJitter[1] = continuing ? m_nrdHybridPreviousJitter[1] : jitter[1];
    frameSettings.motionVectorScale[0] = -1.0f;
    frameSettings.motionVectorScale[1] = -1.0f;
    frameSettings.motionVectorScale[2] = 1.0f;
    frameSettings.frameIndex = m_nrdHybridFrameIndex;
    frameSettings.denoisingRange = PathTraceNrdDenoisingRange;
    frameSettings.accumulationMode = m_nrdHybridAccumulationMode;

    m_nrdHybridDenoiser.NewFrame();

    if(!m_nrdHybridDenoiser.SetCommonSettings(frameSettings) ||
        !m_nrdHybridDenoiser.SetDenoiserSettings(PathTraceHybridDiffuseIdentifier,
            m_nrdRelaxSettingsOverride ? *m_nrdRelaxSettingsOverride : PathTraceRelaxSettings()) ||
        !m_nrdHybridDenoiser.SetDenoiserSettings(PathTraceHybridSpecularIdentifier, PathTraceNrdSettings()))
    {
        ReportNrdHybridUnavailable("NRD rejected the hybrid frame settings");
        return false;
    }

    // Committed only if RecordNrdHybrid records a complete pair and the frame is submitted.
    m_nrdHybridPendingView = view;
    m_nrdHybridPendingProjection = cameraProjection;
    m_nrdHybridPendingJitter[0] = jitter[0];
    m_nrdHybridPendingJitter[1] = jitter[1];
    return true;
}

bool Renderer::RecordNrdHybridDiagnostic(
    ID3D12GraphicsCommandList* commandList,
    unsigned int frameIndex,
    PathTracePrimary primary,
    const Matrix4& view,
    const Matrix4& cameraProjection,
    const Matrix4& inverseViewProjection,
    const Vector3& cameraPosition,
    const FrameLighting& frameLighting,
    const PathTraceReferenceSettings& settings)
{
    // Everything that can fail, before anything is recorded; then preparation and both filters, never presented.
    return BeginNrdHybridFrame(primary, view, cameraProjection, frameLighting, settings) &&
        RecordNrdHybrid(commandList, frameIndex, view, inverseViewProjection, cameraPosition, nullptr);
}

bool Renderer::RecordNrdHybrid(
    ID3D12GraphicsCommandList* commandList,
    unsigned int frameIndex,
    const Matrix4& view,
    const Matrix4& inverseViewProjection,
    const Vector3& cameraPosition,
    Texture* presentation)
{
    // HYBRID.1's validated packing: radiance scale 64 and the raw (not normalized) diffuse hit distance.
    auto constants = MakeNrdPathTracePrepareConstants(commandList, view, inverseViewProjection, cameraPosition);
    constants.radianceScale = 64.0f;
    constants.specularHitDistanceMode = 0;
    auto& rawDistance = m_pathTraceReferencePass.GetRawDiffuseHitDistanceTexture();
    rawDistance.Transition(commandList,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    constants.rawDiffuseHitDistanceIndex = rawDistance.GetDescriptorIndex();

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdHybridPrepare);
    m_nrdHybridPreparePass.ExecutePrepare(commandList, m_uploadBuffer, constants, m_sceneWidth, m_sceneHeight);
    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdHybridPrepare);

    NrdDenoiser::Inputs inputs;
    inputs.motion = m_nrdHybridPreparePass.GetMotion().GetResource();
    inputs.normalRoughness = &m_nrdHybridPreparePass.GetNormalRoughness();
    inputs.viewZ = &m_nrdHybridPreparePass.GetViewZ();
    inputs.diffuseRadianceHitDistance = &m_nrdHybridPreparePass.GetDiffuse();
    inputs.specularRadianceHitDistance = &m_nrdHybridPreparePass.GetSpecular();

    // Each Denoise call constructs a fresh SDK snapshot. Shared scratch is ordered on this command list.
    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdHybridDiffuse);
    const bool diffuseRecorded = m_nrdHybridDenoiser.Denoise(commandList, PathTraceHybridDiffuseIdentifier, inputs);
    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdHybridDiffuse);

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdHybridSpecular);
    const bool specularRecorded = diffuseRecorded && !std::exchange(m_nrdHybridFailSpecularForTesting, false) &&
        m_nrdHybridDenoiser.Denoise(commandList, PathTraceHybridSpecularIdentifier, inputs);
    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdHybridSpecular);

    // NRD binds its own descriptor heap and root signature.
    ID3D12DescriptorHeap* heaps[] = {m_resourceDescriptorHeap.GetHeap()};
    commandList->SetDescriptorHeaps(1, heaps);
    m_nrdHybridDispatchedThisFrame = diffuseRecorded && specularRecorded;

    if(!m_nrdHybridDispatchedThisFrame)
    {
        // Even a recorded diffuse half is neither presented nor committed (camera and history stay as they were).
        ReportNrdHybridUnavailable("the hybrid did not record a complete pair");
        return false;
    }

    // HYBRID.5 production resolve: surface pixels of the presentation, as RELAX's and REBLUR's. The path tracer wrote the
    // presentation earlier in this list, so that write is ordered first.
    if(presentation)
    {
        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdHybridResolve);
        presentation->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_RESOURCE_BARRIER presentationBarrier{};
        presentationBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        presentationBarrier.UAV.pResource = presentation->GetResource();
        commandList->ResourceBarrier(1, &presentationBarrier);
        m_nrdHybridPreparePass.ExecuteDenoisedResolve(commandList, m_uploadBuffer, constants,
            m_nrdHybridDenoiser.GetDiffuseOutput(PathTraceHybridDiffuseIdentifier),
            m_nrdHybridDenoiser.GetSpecularOutput(PathTraceHybridSpecularIdentifier), *presentation, m_sceneWidth,
            m_sceneHeight);
        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdHybridResolve);
        m_debugUI.SetPathTraceNrdStatus("");
    }

    m_nrdHybridFailure.clear();
    return true;
}

bool Renderer::EnsureNrdRelaxDenoiser()
{
    TransitionTiming::Scope er1Scope("EnsureNrdRelaxDenoiser", 0.5);
    // ER.1 Phase 3: a retained instance is reused only while its description still matches; otherwise it is released
    // (after the GPU drains) and created again.
    if(m_nrdRelaxDenoiser.IsInitialized() && m_nrdRelaxCreatedPromoteFloat16To32 != m_nrdRelaxPromoteFloat16To32)
    {
        ReleaseNrdRelaxDenoiser();
    }

    if(m_nrdRelaxDenoiser.IsInitialized())
    {
        return true;
    }

    if(m_nrdRelaxCreationFailed)
    {
        return false;
    }

    NrdDenoiser::Description description = PathTraceRelaxDescription();
    description.promoteFloat16to32 = m_nrdRelaxPromoteFloat16To32;

    if(std::exchange(m_nrdRelaxFailCreationForTesting, false) ||
        !m_nrdRelaxDenoiser.Initialize(m_graphicsDevice.GetDevice(), m_commandContext.GetCommandQueue(),
            m_resourceDescriptorHeap, m_sceneWidth, m_sceneHeight, description))
    {
        // NrdDenoiser::Initialize leaves nothing behind on failure (NRD.RELAX.1).
        m_nrdRelaxCreationFailed = true;
        return false;
    }

    ++m_nrdRelaxCreations;
    m_nrdRelaxCreatedPromoteFloat16To32 = description.promoteFloat16to32;
    m_nrdRelaxInstanceNew = true;
    Log::Info("NRD RELAX diagnostic initialized at " + std::to_string(m_sceneWidth) + "x" +
        std::to_string(m_sceneHeight) + ": persistent " + std::to_string(m_nrdRelaxDenoiser.GetPersistentMemoryUsageInMb()) +
        " MiB, transient " + std::to_string(m_nrdRelaxDenoiser.GetAliasableMemoryUsageInMb()) + " MiB.");
    return true;
}

void Renderer::RecordNrdConfidenceProbes(
    ID3D12GraphicsCommandList* commandList,
    const PathTraceReferenceConstants& constants,
    const FrameLighting& frameLighting,
    const PathTraceReferenceSettings& settings,
    PathTracePrimary primary,
    D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstants,
    D3D12_GPU_VIRTUAL_ADDRESS rtxdiConstants)
{
    bool recreated = false;

    if(!m_nrdConfidencePass.Initialize(m_graphicsDevice, m_shaderLibrary) ||
        !m_nrdConfidencePass.EnsureResources(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, m_sceneWidth,
            m_sceneHeight, recreated))
    {
        return;
    }

    NrdConfidenceProbeIdentity identity;
    identity.sceneResourceLifetime = m_sceneResourceLifetime;
    identity.lights = MakePathTraceTemporalKey(primary, frameLighting, settings);
    identity.environment[0] = constants.environmentTextureIndex;
    identity.environment[1] = constants.environmentConditionalCDFIndex;
    identity.environment[2] = constants.environmentMarginalCDFIndex;
    identity.environment[3] = constants.environmentWidth;
    identity.environment[4] = constants.environmentHeight;
    identity.environmentTotalWeight = constants.environmentTotalWeight;
    identity.width = m_sceneWidth;
    identity.height = m_sceneHeight;

    const NrdConfidenceProbeIdentity* committed = m_nrdConfidenceCommittedIdentity ?
        &*m_nrdConfidenceCommittedIdentity : nullptr;
    const char* state = "valid";

    if(recreated)
    {
        state = "reset: probe resources (re)created";
    }
    else if(!m_nrdConfidenceExecutedLastFrame || !committed)
    {
        state = "reset: no probe frame immediately before";
    }
    else if(committed->sceneResourceLifetime != identity.sceneResourceLifetime)
    {
        state = "reset: scene resources changed";
    }
    else if(!SamePathTraceTemporalKey(committed->lights, identity.lights))
    {
        state = "reset: lights, sun or path settings changed";
    }
    else if(std::memcmp(committed->environment, identity.environment, sizeof(identity.environment)) != 0 ||
        committed->environmentTotalWeight != identity.environmentTotalWeight)
    {
        state = "reset: environment changed";
    }
    else if(committed->width != identity.width || committed->height != identity.height)
    {
        state = "reset: render extent changed";
    }

    const bool previousValid = std::strcmp(state, "valid") == 0;

    // Changes only: a steady state logs once.
    if(m_nrdConfidenceHistoryState != state)
    {
        m_nrdConfidenceHistoryState = state;
        Log::Info("NRD RELAX confidence probes: " + std::to_string(m_nrdConfidencePass.GetProbeWidth()) + "x" +
            std::to_string(m_nrdConfidencePass.GetProbeHeight()) + " probes, record history " + state);
    }

    constexpr D3D12_RESOURCE_STATES shaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_gBuffer.GetDepth().Transition(commandList, shaderRead);
    m_gBuffer.GetSurface().Transition(commandList, shaderRead);
    m_gBuffer.GetGeometricNormal().Transition(commandList, shaderRead);

    NrdHistoryConfidenceProbeConstants probe;
    probe.inverseViewProjection = constants.inverseViewProjection;
    std::memcpy(probe.cameraPosition, constants.cameraPosition, sizeof(probe.cameraPosition));
    probe.depthIndex = m_gBuffer.GetDepth().GetSRVIndex();
    probe.surfaceIndex = m_gBuffer.GetSurface().GetSRVIndex();
    probe.geometricNormalIndex = m_gBuffer.GetGeometricNormal().GetSRVIndex();
    probe.frameIndex = m_nrdConfidenceFrameIndex;
    probe.previousValid = previousValid ? 1u : 0u;
    std::memcpy(probe.sunDirectionToLight, constants.sunDirectionToLight, sizeof(probe.sunDirectionToLight));
    probe.sunCosAngularRadius = constants.sunCosAngularRadius;
    std::memcpy(probe.sunIrradiance, constants.sunIrradiance, sizeof(probe.sunIrradiance));
    probe.environmentTextureIndex = constants.environmentTextureIndex;
    probe.environmentConditionalCDFIndex = constants.environmentConditionalCDFIndex;
    probe.environmentMarginalCDFIndex = constants.environmentMarginalCDFIndex;
    probe.environmentWidth = constants.environmentWidth;
    probe.environmentHeight = constants.environmentHeight;
    probe.environmentTotalWeight = constants.environmentTotalWeight;

    // The filter measures plane distances from the camera the evaluated records were made with.
    const float* recordCamera = previousValid ? m_nrdConfidenceCommittedCamera : constants.cameraPosition;

    if(!m_nrdConfidencePass.Execute(commandList, m_uploadBuffer, probe, sceneLightConstants, rtxdiConstants,
        m_rayTracingScene.GetTopLevel().GetGPUAddress(), recordCamera, m_nrdRelaxConfidenceStrength))
    {
        return;
    }

    // Committed only once the frame is submitted (CommitSubmittedFrameState's RELAX block).
    m_nrdConfidencePendingIdentity = identity;
    std::memcpy(m_nrdConfidencePendingCamera, constants.cameraPosition, sizeof(m_nrdConfidencePendingCamera));
    m_nrdConfidenceDispatchedThisFrame = true;
}

void Renderer::ReleaseNrdRelaxDenoiser()
{
    TransitionTiming::Scope er1Scope("ReleaseNrdRelaxDenoiser");
    if(!m_nrdRelaxDenoiser.IsInitialized() && !m_nrdRelaxResolvedTexture.GetResource() && !m_nrdRelaxCreationFailed &&
        !m_nrdRelaxConfidenceOne.GetResource())
    {
        return;
    }

    // Submitted frames may still read or write the instance's resources.
    if(!m_commandContext.WaitForGPU())
    {
        Log::Error("Failed to wait for the GPU before releasing the NRD RELAX diagnostic; it is kept.");
        return;
    }

    m_nrdRelaxDenoiser.Shutdown();
    m_nrdRelaxResolvedTexture.Shutdown();
    m_nrdRelaxConfidenceOne.Shutdown();
    m_nrdRelaxConfidenceLow.Shutdown();
    m_nrdConfidencePass.Shutdown();
    m_nrdRelaxCreationFailed = false;
    m_nrdRelaxHistoryValid = false;
    m_nrdRelaxExecutedLastFrame = false;
    m_nrdRelaxFailure.clear();
    Log::Info("NRD RELAX diagnostic released.");
}

void Renderer::ReleaseNrdRelaxDiagnosticTexture()
{
    TransitionTiming::Scope er1Scope("ReleaseNrdRelaxDiagnosticTexture");
    if(!m_nrdRelaxResolvedTexture.GetResource())
    {
        return;
    }

    // Submitted frames may still write it.
    if(!m_commandContext.WaitForGPU())
    {
        Log::Error("Failed to wait for the GPU before releasing the NRD RELAX diagnostic texture; it is kept.");
        return;
    }

    m_nrdRelaxResolvedTexture.Shutdown();
}

void Renderer::ReportNrdRelaxUnavailable(const std::string& reason)
{
    // NRD.RELAX.4: as the selected denoiser the Engine stages present instead; as the diagnostic nothing is recorded.
    const bool selected = m_pathTraceDenoiserMode == PathTraceDenoiserMode::NrdRelax;

    if(reason != m_nrdRelaxFailure)
    {
        Log::Warning(selected ? "NRD RELAX could not denoise this frame (" + reason + "); the Engine denoiser is used." :
            "NRD RELAX diagnostic could not run this frame (" + reason + "); nothing was recorded.");
    }

    m_nrdRelaxFailure = reason;

    if(selected)
    {
        m_debugUI.SetPathTraceNrdStatus("NRD RELAX unavailable (" + reason + "); using Engine.");
    }
}

bool Renderer::BeginNrdRelaxFrame(
    PathTracePrimary primary,
    const Matrix4& view,
    const Matrix4& cameraProjection,
    const FrameLighting& frameLighting,
    const PathTraceReferenceSettings& settings)
{
    TransitionTiming::Scope er1Scope("BeginNrdRelaxFrame", 0.5);
    if(!EnsureNrdRelaxDenoiser())
    {
        ReportNrdRelaxUnavailable("the RELAX instance could not be created");
        return false;
    }

    // The RELAX.2 bypass reconstructions only while that diagnostic runs.
    if(!m_nrdRelaxPreparePass.Initialize(m_graphicsDevice, m_shaderLibrary, NrdPathTracePreparePass::Encoding::Relax) ||
        !m_nrdRelaxPreparePass.EnsureTextures(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, m_sceneWidth,
            m_sceneHeight, m_nrdRelaxPrepareDiagnostic))
    {
        ReportNrdRelaxUnavailable("the RELAX input textures could not be created");
        return false;
    }

    // NRD.RELAX.3: the diagnostic resolved texture only while the dispatch diagnostic is requested.
    if(m_nrdRelaxDispatchDiagnostic && !m_nrdRelaxResolvedTexture.GetResource())
    {
        ID3D12Device* device = m_graphicsDevice.GetDevice();

        if(!m_nrdRelaxResolvedTexture.Initialize(device, m_sceneWidth, m_sceneHeight, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
            !m_nrdRelaxResolvedTexture.CreateSRV(device, m_resourceDescriptorHeap) ||
            !m_nrdRelaxResolvedTexture.CreateUAV(device, m_resourceDescriptorHeap))
        {
            m_nrdRelaxResolvedTexture.Shutdown();
            ReportNrdRelaxUnavailable("the RELAX resolved texture could not be created");
            return false;
        }

        m_nrdRelaxResolvedTexture.GetResource()->SetName(L"NRD RELAX Resolved (diagnostic)");
    }

    // RESTART sources as REBLUR's: the temporal key and a frame gap; the shared invalidations clear the flag directly.
    const PathTraceTemporalKey key = MakePathTraceTemporalKey(primary, frameLighting, settings);

    if(!m_nrdRelaxKey || !SamePathTraceTemporalKey(*m_nrdRelaxKey, key) || !m_nrdRelaxExecutedLastFrame)
    {
        m_nrdRelaxHistoryValid = false;
    }

    m_nrdRelaxKey = key;
    m_nrdRelaxAccumulationMode = m_nrdRelaxInstanceNew ? nrd::AccumulationMode::CLEAR_AND_RESTART :
        m_nrdRelaxHistoryValid ? nrd::AccumulationMode::CONTINUE : nrd::AccumulationMode::RESTART;
    const bool continuing = m_nrdRelaxAccumulationMode == nrd::AccumulationMode::CONTINUE;
    const float jitter[2] = {m_cameraJitterStatus.active ? m_cameraJitterStatus.x : 0.0f,
        m_cameraJitterStatus.active ? m_cameraJitterStatus.y : 0.0f};

    // REBLUR's validated conventions (NRD.3 section 8, NRDRelax2.md section 7): unjittered matrices, jitter separately,
    // G-buffer motion scaled by (-1, -1, 1), the preparation's denoising range. Without history the previous frame is
    // this one.
    NrdDenoiser::FrameSettings frameSettings;
    frameSettings.view = view;
    frameSettings.projection = cameraProjection;
    frameSettings.previousView = continuing ? m_nrdRelaxPreviousView : view;
    frameSettings.previousProjection = continuing ? m_nrdRelaxPreviousProjection : cameraProjection;
    frameSettings.jitter[0] = jitter[0];
    frameSettings.jitter[1] = jitter[1];
    frameSettings.previousJitter[0] = continuing ? m_nrdRelaxPreviousJitter[0] : jitter[0];
    frameSettings.previousJitter[1] = continuing ? m_nrdRelaxPreviousJitter[1] : jitter[1];
    frameSettings.motionVectorScale[0] = -1.0f;
    frameSettings.motionVectorScale[1] = -1.0f;
    frameSettings.motionVectorScale[2] = 1.0f;
    frameSettings.frameIndex = m_nrdRelaxFrameIndex;
    frameSettings.denoisingRange = PathTraceNrdDenoisingRange;
    frameSettings.accumulationMode = m_nrdRelaxAccumulationMode;

    // NRD RELAX R.9A: the constant confidence textures, unordered-access capable as every NRD input must be. R.9C uses
    // the 1 for the specular confidence and for frames without probes.
    const bool historyConfidence = m_nrdRelaxHistoryConfidence != 0 || m_nrdRelaxGeneratedConfidence;

    if(historyConfidence && !m_nrdRelaxConfidenceOne.GetResource())
    {
        ID3D12Device* device = m_graphicsDevice.GetDevice();
        const bool created = m_nrdRelaxConfidenceOne.Initialize(device, 1, 1, DXGI_FORMAT_R16_FLOAT, m_rtvDescriptorHeap,
            m_resourceDescriptorHeap, {1.0f, 1.0f, 1.0f, 1.0f}, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
            (m_nrdRelaxGeneratedConfidence ||
            m_nrdRelaxConfidenceLow.Initialize(device, 1, 1, DXGI_FORMAT_R16_FLOAT, m_rtvDescriptorHeap,
            m_resourceDescriptorHeap, {0.1f, 0.1f, 0.1f, 0.1f}, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));

        if(!created)
        {
            m_nrdRelaxConfidenceOne.Shutdown();
            m_nrdRelaxConfidenceLow.Shutdown();
            ReportNrdRelaxUnavailable("the history confidence textures could not be created");
            return false;
        }

        m_nrdRelaxConfidenceOne.GetResource()->SetName(L"NRD RELAX History Confidence 1 (diagnostic)");
        if(m_nrdRelaxConfidenceLow.GetResource())
        {
            m_nrdRelaxConfidenceLow.GetResource()->SetName(L"NRD RELAX History Confidence 0.1 (diagnostic)");
        }
    }

    frameSettings.historyConfidence = historyConfidence;

    m_nrdRelaxDenoiser.NewFrame();

    if(!m_nrdRelaxDenoiser.SetCommonSettings(frameSettings) ||
        !m_nrdRelaxDenoiser.SetDenoiserSettings(PathTraceRelaxIdentifier,
            m_nrdRelaxSettingsOverride ? *m_nrdRelaxSettingsOverride : PathTraceRelaxSettings()))
    {
        ReportNrdRelaxUnavailable("NRD rejected the RELAX frame settings");
        return false;
    }

    // Committed only if RecordNrdRelax records the dispatch and the frame is submitted.
    m_nrdRelaxPendingView = view;
    m_nrdRelaxPendingProjection = cameraProjection;
    m_nrdRelaxPendingJitter[0] = jitter[0];
    m_nrdRelaxPendingJitter[1] = jitter[1];
    return true;
}

bool Renderer::RecordNrdRelax(
    ID3D12GraphicsCommandList* commandList,
    unsigned int frameIndex,
    const Matrix4& view,
    const Matrix4& inverseViewProjection,
    const Vector3& cameraPosition,
    Texture* presentation)
{
    const NrdPathTracePrepareConstants constants = MakeNrdPathTracePrepareConstants(commandList, view,
        inverseViewProjection, cameraPosition);
    NrdPathTracePrepareConstants relaxConstants = constants;
    Texture& rawDiffuseHitDistance = m_pathTraceReferencePass.GetRawDiffuseHitDistanceTexture();
    rawDiffuseHitDistance.Transition(commandList,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    relaxConstants.rawDiffuseHitDistanceIndex = rawDiffuseHitDistance.GetDescriptorIndex();

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdRelaxPrepare);
    m_nrdRelaxPreparePass.ExecutePrepare(commandList, m_uploadBuffer, relaxConstants, m_sceneWidth, m_sceneHeight);
    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdRelaxPrepare);

    NrdDenoiser::Inputs inputs;
    inputs.motion = m_nrdRelaxPreparePass.GetMotion().GetResource();
    inputs.normalRoughness = &m_nrdRelaxPreparePass.GetNormalRoughness();
    inputs.viewZ = &m_nrdRelaxPreparePass.GetViewZ();
    inputs.diffuseRadianceHitDistance = &m_nrdRelaxPreparePass.GetDiffuse();
    inputs.specularRadianceHitDistance = &m_nrdRelaxPreparePass.GetSpecular();

    // NRD RELAX R.9A: refill the 1x1 constants (no reliance on earlier contents) and leave them shader-readable.
    if(m_nrdRelaxHistoryConfidence != 0 || m_nrdRelaxGeneratedConfidence)
    {
        constexpr float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        constexpr float low[4] = {0.1f, 0.1f, 0.1f, 0.1f};
        constexpr D3D12_RESOURCE_STATES shaderRead =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        RenderTarget& diffuseConfidence = m_nrdRelaxHistoryConfidence == 2 ? m_nrdRelaxConfidenceLow :
            m_nrdRelaxConfidenceOne;

        m_nrdRelaxConfidenceOne.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
        commandList->ClearRenderTargetView(m_nrdRelaxConfidenceOne.GetRTV(), one, 0, nullptr);
        m_nrdRelaxConfidenceOne.Transition(commandList, shaderRead);

        // Generated confidence needs only the neutral constant (specular and missing-probe fallback).
        if(!m_nrdRelaxGeneratedConfidence)
        {
            m_nrdRelaxConfidenceLow.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
            commandList->ClearRenderTargetView(m_nrdRelaxConfidenceLow.GetRTV(), low, 0, nullptr);
            m_nrdRelaxConfidenceLow.Transition(commandList, shaderRead);
        }

        inputs.diffuseConfidence = diffuseConfidence.GetResource();
        inputs.specularConfidence = m_nrdRelaxConfidenceOne.GetResource();

        // R.9C: only this frame's probe confidence, which is already in the previous frame's pixel space NRD samples it
        // in (NRD 4.17.3 RELAX_TemporalAccumulation.cs.hlsl: prevUVSMB, bilinear). Reprojecting here would apply motion
        // twice. Without it, 1: no evidence of change.
        const bool generated = m_nrdRelaxGeneratedConfidence && m_nrdConfidenceDispatchedThisFrame;

        if(generated)
        {
            Texture& probeConfidence = m_nrdConfidencePass.GetConfidence();
            probeConfidence.Transition(commandList, shaderRead);
            inputs.diffuseConfidence = probeConfidence.GetResource();
        }
        else if(m_nrdRelaxGeneratedConfidence && !m_nrdRelaxGeneratedConfidenceFallbackReported)
        {
            Log::Warning("NRD RELAX generated confidence: no confidence probes this frame (select Raster ReSTIR DI); "
                "IN_DIFF_CONFIDENCE 1.0 is bound instead.");
            m_nrdRelaxGeneratedConfidenceFallbackReported = true;
        }

        if(!m_nrdRelaxConfidenceReported && (generated || !m_nrdRelaxGeneratedConfidence))
        {
            Log::Info(std::string("NRD RELAX history confidence: first confidence dispatch, isHistoryConfidenceAvailable "
                "1, IN_DIFF_CONFIDENCE ") + (generated ? "generated (" +
                std::to_string(m_nrdConfidencePass.GetProbeWidth()) + "x" +
                std::to_string(m_nrdConfidencePass.GetProbeHeight()) + " R16F)" :
                m_nrdRelaxHistoryConfidence == 1 ? "1.0" : "0.1") + ", IN_SPEC_CONFIDENCE 1.0 (1x1 R16F)");
            m_nrdRelaxConfidenceReported = true;
        }
    }

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdRelaxDenoise);
    const bool recorded = m_nrdRelaxDenoiser.Denoise(commandList, PathTraceRelaxIdentifier, inputs);
    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdRelaxDenoise);

    // NRD binds its own descriptor heap and root signature.
    ID3D12DescriptorHeap* descriptorHeaps[] = {m_resourceDescriptorHeap.GetHeap()};
    commandList->SetDescriptorHeaps(1, descriptorHeaps);

    if(!recorded)
    {
        ReportNrdRelaxUnavailable("the RELAX dispatch was not recorded");
        return false;
    }

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdRelaxResolve);
    Texture& diffuseOutput = m_nrdRelaxDenoiser.GetDiffuseOutput(PathTraceRelaxIdentifier);
    Texture& specularOutput = m_nrdRelaxDenoiser.GetSpecularOutput(PathTraceRelaxIdentifier);

    // NRD.RELAX.4 production resolve: surface pixels of the presentation, as REBLUR's. The path tracer wrote the
    // presentation earlier in this list, so that write is ordered first.
    if(presentation)
    {
        presentation->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_RESOURCE_BARRIER presentationBarrier{};
        presentationBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        presentationBarrier.UAV.pResource = presentation->GetResource();
        commandList->ResourceBarrier(1, &presentationBarrier);

        NrdPathTracePrepareConstants presentConstants = relaxConstants;
        presentConstants.relaxDiagnosticResolve = 0;

        // NRD RELAX R.4 diagnostics: one diffuse signal in place of the production resolve. A needs this frame's split
        // direct diffuse buffer (raster ReSTIR DI with extraction); without it, or without the pipeline, the production
        // resolve presents.
        // R.9B: views 4, 5 and 6 need this frame's confidence probes.
        const bool directAvailable = m_pathTraceReferencePass.WroteSplitDirectDiffuse();
        const bool probeView = m_nrdRelaxSignalView >= 4 && m_nrdRelaxSignalView <= 6;
        const bool signalView = m_nrdRelaxSignalView != 0 &&
            (m_nrdRelaxSignalView != 1 || directAvailable) &&
            (!probeView || m_nrdConfidenceDispatchedThisFrame) &&
            m_nrdRelaxPreparePass.EnsureRelaxSignalView(m_graphicsDevice, m_shaderLibrary);

        if(signalView)
        {
            NrdRelaxSignalViewConstants signalViewConstants;
            signalViewConstants.mode = m_nrdRelaxSignalView;
            signalViewConstants.splitDirectDiffuseIndex = directAvailable ?
                m_pathTraceReferencePass.GetSplitDirectDiffuseUAVIndex() : 0;
            signalViewConstants.samplesPerFrame = m_pathTraceReferencePass.GetSplitDirectDiffuseSamples();

            if(probeView)
            {
                signalViewConstants.probeTextureIndex = m_nrdRelaxSignalView == 4 ?
                    m_nrdConfidencePass.GetSignal().GetDescriptorIndex() : m_nrdRelaxSignalView == 5 ?
                    m_nrdConfidencePass.GetConfidence().GetDescriptorIndex() :
                    m_nrdConfidencePass.GetState().GetDescriptorIndex();
            }

            m_nrdRelaxPreparePass.ExecuteRelaxSignalView(commandList, m_uploadBuffer, presentConstants,
                signalViewConstants, diffuseOutput, *presentation, m_sceneWidth, m_sceneHeight);
            // The production resolve leaves both outputs shader-readable; so does this frame.
            specularOutput.Transition(commandList,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        else
        {
            if(m_nrdRelaxSignalView != 0 && !m_nrdRelaxSignalViewFallbackReported)
            {
                const char* reason = m_nrdRelaxSignalView == 1 && !directAvailable ?
                    "no split direct diffuse; select Raster ReSTIR DI" : probeView && !m_nrdConfidenceDispatchedThisFrame ?
                    "no confidence probes this frame; set ENGINE_NRD_RELAX_CONFIDENCE_PROBES=1 and select Raster ReSTIR DI" :
                    "pipeline not created";
                Log::Warning("NRD RELAX signal view unavailable this frame (" + std::string(reason) +
                    "); the production resolve presents.");
                m_nrdRelaxSignalViewFallbackReported = true;
            }

            m_nrdRelaxPreparePass.ExecuteDenoisedResolve(commandList, m_uploadBuffer, presentConstants, diffuseOutput,
                specularOutput, *presentation, m_sceneWidth, m_sceneHeight);
        }
    }

    // NRD.RELAX.3 diagnostic resolve (every pixel, never presented), also on production frames while requested.
    if(m_nrdRelaxDispatchDiagnostic && m_nrdRelaxResolvedTexture.GetResource())
    {
        NrdPathTracePrepareConstants diagnosticConstants = relaxConstants;
        diagnosticConstants.relaxDiagnosticResolve = 1;
        m_nrdRelaxPreparePass.ExecuteDenoisedResolve(commandList, m_uploadBuffer, diagnosticConstants, diffuseOutput,
            specularOutput, m_nrdRelaxResolvedTexture, m_sceneWidth, m_sceneHeight);
    }

    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdRelaxResolve);

    m_nrdRelaxFailure.clear();
    m_nrdRelaxDispatchedThisFrame = true;

    if(presentation)
    {
        m_debugUI.SetPathTraceNrdStatus("");
    }

    return true;
}

bool Renderer::RecordNrdRelaxDispatchDiagnostic(
    ID3D12GraphicsCommandList* commandList,
    unsigned int frameIndex,
    PathTracePrimary primary,
    const Matrix4& view,
    const Matrix4& cameraProjection,
    const Matrix4& inverseViewProjection,
    const Vector3& cameraPosition,
    const FrameLighting& frameLighting,
    const PathTraceReferenceSettings& settings)
{
    // Everything that can fail, before anything is recorded; then preparation, RELAX and the diagnostic resolve only.
    return BeginNrdRelaxFrame(primary, view, cameraProjection, frameLighting, settings) &&
        RecordNrdRelax(commandList, frameIndex, view, inverseViewProjection, cameraPosition, nullptr);
}

nrd::ReblurSettings Renderer::PathTraceNrdProductionSettings()
{
    return PathTraceNrdSettings();
}

bool Renderer::EnsurePathTraceNrdDenoiser()
{
    TransitionTiming::Scope er1Scope("EnsurePathTraceNrdDenoiser", 0.5);
    // ER.1 Phase 3: as RELAX's, a retained instance is reused only with an unchanged description.
    if(m_nrdPathTraceDenoiser.IsInitialized() &&
        m_nrdPathTraceCreatedPromoteFloat16To32 != m_nrdPathTracePromoteFloat16To32)
    {
        ReleasePathTraceNrdDenoiser();
    }

    if(m_nrdPathTraceDenoiser.IsInitialized())
    {
        return true;
    }

    if(m_nrdPathTraceCreationFailed)
    {
        return false;
    }

    NrdDenoiser::Description description = PathTraceNrdDescription();
    description.promoteFloat16to32 = m_nrdPathTracePromoteFloat16To32;

    if(!m_nrdPathTraceDenoiser.Initialize(m_graphicsDevice.GetDevice(), m_commandContext.GetCommandQueue(),
        m_resourceDescriptorHeap, m_sceneWidth, m_sceneHeight, description))
    {
        m_nrdPathTraceCreationFailed = true;
        return false;
    }

    ++m_nrdPathTraceCreations;
    m_nrdPathTraceCreatedPromoteFloat16To32 = description.promoteFloat16to32;
    m_nrdPathTraceInstanceNew = true;
    Log::Info("NRD path-traced REBLUR initialized at " + std::to_string(m_sceneWidth) + "x" +
        std::to_string(m_sceneHeight) + ": persistent " +
        std::to_string(m_nrdPathTraceDenoiser.GetPersistentMemoryUsageInMb()) + " MiB, transient " +
        std::to_string(m_nrdPathTraceDenoiser.GetAliasableMemoryUsageInMb()) + " MiB.");
    return true;
}

void Renderer::ReleasePathTraceNrdDenoiser()
{
    TransitionTiming::Scope er1Scope("ReleasePathTraceNrdDenoiser");
    if(!m_nrdPathTraceDenoiser.IsInitialized() && !m_nrdPathTraceValidationTexture.GetResource())
    {
        return;
    }

    // Submitted frames may still read or write the instance's resources.
    if(!m_commandContext.WaitForGPU())
    {
        Log::Error("Failed to wait for the GPU before releasing NRD path-traced REBLUR; it is kept.");
        return;
    }

    m_nrdPathTraceDenoiser.Shutdown();
    m_nrdPathTraceValidationTexture.Shutdown();
    m_nrdPathTraceHistoryValid = false;
    m_nrdPathTraceExecutedLastFrame = false;
    Log::Info("NRD path-traced REBLUR released.");
}

void Renderer::ReportPathTraceNrdUnavailable(const std::string& reason)
{
    if(reason != m_nrdPathTraceFailure)
    {
        Log::Warning("NRD REBLUR could not denoise this frame (" + reason + "); the Engine denoiser is used.");
    }

    m_nrdPathTraceFailure = reason;
    m_debugUI.SetPathTraceNrdStatus("NRD REBLUR unavailable (" + reason + "); using Engine.");
}

bool Renderer::BeginPathTraceNrdFrame(
    PathTracePrimary primary,
    const Matrix4& view,
    const Matrix4& cameraProjection,
    const FrameLighting& frameLighting,
    const PathTraceReferenceSettings& settings)
{
    if(!EnsurePathTraceNrdDenoiser())
    {
        ReportPathTraceNrdUnavailable("the NRD instance could not be created");
        return false;
    }

    // The NRD.3 diagnostic's reconstructions are created only while it runs.
    if(!m_nrdPathTracePreparePass.Initialize(m_graphicsDevice, m_shaderLibrary) ||
        !m_nrdPathTracePreparePass.EnsureTextures(m_graphicsDevice.GetDevice(), m_resourceDescriptorHeap, m_sceneWidth,
            m_sceneHeight, m_nrdPathTraceDiagnostic))
    {
        ReportPathTraceNrdUnavailable("the NRD input textures could not be created");
        return false;
    }

    if(m_nrdPathTraceValidation && !m_nrdPathTraceValidationTexture.GetResource())
    {
        ID3D12Device* device = m_graphicsDevice.GetDevice();

        if(!m_nrdPathTraceValidationTexture.Initialize(device, m_sceneWidth, m_sceneHeight, 1, 1,
            DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS) || !m_nrdPathTraceValidationTexture.CreateSRV(device,
            m_resourceDescriptorHeap) || !m_nrdPathTraceValidationTexture.CreateUAV(device, m_resourceDescriptorHeap))
        {
            m_nrdPathTraceValidationTexture.Shutdown();
            ReportPathTraceNrdUnavailable("the NRD validation texture could not be created");
            return false;
        }
    }

    // RESTART sources checked here: the temporal key (as the Engine temporal pass) and a frame gap.
    const PathTraceTemporalKey key = MakePathTraceTemporalKey(primary, frameLighting, settings);

    if(!m_nrdPathTraceKey || !SamePathTraceTemporalKey(*m_nrdPathTraceKey, key) || !m_nrdPathTraceExecutedLastFrame)
    {
        m_nrdPathTraceHistoryValid = false;
    }

    m_nrdPathTraceKey = key;
    m_nrdPathTraceAccumulationMode = m_nrdPathTraceInstanceNew ? nrd::AccumulationMode::CLEAR_AND_RESTART :
        m_nrdPathTraceHistoryValid ? nrd::AccumulationMode::CONTINUE : nrd::AccumulationMode::RESTART;
    const bool continuing = m_nrdPathTraceAccumulationMode == nrd::AccumulationMode::CONTINUE;
    const float jitter[2] = {m_cameraJitterStatus.active ? m_cameraJitterStatus.x : 0.0f,
        m_cameraJitterStatus.active ? m_cameraJitterStatus.y : 0.0f};

    // Unjittered matrices; jitter passes unchanged (NRD.3 section 8). Without history the previous frame is this one.
    NrdDenoiser::FrameSettings frameSettings;
    frameSettings.view = view;
    frameSettings.projection = cameraProjection;
    frameSettings.previousView = continuing ? m_nrdPathTracePreviousView : view;
    frameSettings.previousProjection = continuing ? m_nrdPathTracePreviousProjection : cameraProjection;
    frameSettings.jitter[0] = jitter[0];
    frameSettings.jitter[1] = jitter[1];
    frameSettings.previousJitter[0] = continuing ? m_nrdPathTracePreviousJitter[0] : jitter[0];
    frameSettings.previousJitter[1] = continuing ? m_nrdPathTracePreviousJitter[1] : jitter[1];
    // G-buffer motion is current UV - previous UV and previous - current view Z (NRD.3 section 8.2).
    frameSettings.motionVectorScale[0] = -1.0f;
    frameSettings.motionVectorScale[1] = -1.0f;
    frameSettings.motionVectorScale[2] = 1.0f;
    frameSettings.frameIndex = m_nrdPathTraceFrameIndex;
    frameSettings.denoisingRange = PathTraceNrdDenoisingRange;
    frameSettings.accumulationMode = m_nrdPathTraceAccumulationMode;
    frameSettings.enableValidation = m_nrdPathTraceValidation;

    m_nrdPathTraceDenoiser.NewFrame();

    if(!m_nrdPathTraceDenoiser.SetCommonSettings(frameSettings) ||
        !m_nrdPathTraceDenoiser.SetDenoiserSettings(PathTraceNrdIdentifier,
            m_nrdPathTraceSettingsOverride ? *m_nrdPathTraceSettingsOverride : PathTraceNrdSettings()))
    {
        ReportPathTraceNrdUnavailable("NRD rejected the frame settings");
        return false;
    }

    m_nrdPathTracePendingView = view;
    m_nrdPathTracePendingProjection = cameraProjection;
    m_nrdPathTracePendingJitter[0] = jitter[0];
    m_nrdPathTracePendingJitter[1] = jitter[1];
    m_nrdPathTraceFailure.clear();
    m_debugUI.SetPathTraceNrdStatus("");
    return true;
}

void Renderer::RecordPathTraceNrd(
    ID3D12GraphicsCommandList* commandList,
    unsigned int frameIndex,
    const Matrix4& view,
    const Matrix4& inverseViewProjection,
    const Vector3& cameraPosition)
{
    const NrdPathTracePrepareConstants constants = MakeNrdPathTracePrepareConstants(commandList, view,
        inverseViewProjection, cameraPosition);

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdPathTracePrepare);
    m_nrdPathTracePreparePass.ExecutePrepare(commandList, m_uploadBuffer, constants, m_sceneWidth, m_sceneHeight);
    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdPathTracePrepare);

    NrdDenoiser::Inputs inputs;
    inputs.motion = m_nrdPathTracePreparePass.GetMotion().GetResource();
    inputs.normalRoughness = &m_nrdPathTracePreparePass.GetNormalRoughness();
    inputs.viewZ = &m_nrdPathTracePreparePass.GetViewZ();
    inputs.diffuseRadianceHitDistance = &m_nrdPathTracePreparePass.GetDiffuse();
    inputs.specularRadianceHitDistance = &m_nrdPathTracePreparePass.GetSpecular();
    inputs.validation = m_nrdPathTraceValidation ? &m_nrdPathTraceValidationTexture : nullptr;

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdPathTraceDenoise);
    m_nrdPathTraceDenoiser.Denoise(commandList, PathTraceNrdIdentifier, inputs);
    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdPathTraceDenoise);

    // NRD binds its own descriptor heap and root signature.
    ID3D12DescriptorHeap* descriptorHeaps[] = {m_resourceDescriptorHeap.GetHeap()};
    commandList->SetDescriptorHeaps(1, descriptorHeaps);

    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdPathTraceResolve);
    NrdPathTracePrepareConstants resolveConstants = constants;
    resolveConstants.resolveComponents = m_nrdPathTraceResolveComponents;
    m_nrdPathTracePreparePass.ExecuteDenoisedResolve(commandList, m_uploadBuffer, resolveConstants,
        m_nrdPathTraceDenoiser.GetDiffuseOutput(PathTraceNrdIdentifier),
        m_nrdPathTraceDenoiser.GetSpecularOutput(PathTraceNrdIdentifier), m_pathTraceReferencePass.GetOutputTexture(),
        m_sceneWidth, m_sceneHeight);
    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdPathTraceResolve);

    m_nrdPathTraceDispatchedThisFrame = true;
}

void Renderer::RecordPathTraceSpatial(ID3D12GraphicsCommandList* commandList, const Matrix4& view,
    const Matrix4& projection, const PathTraceDenoiserVisualization& visualization)
{
    // RecordPathTraceTemporal left the G-buffer shader-readable and the presentation an unordered-access view, and
    // the temporal pass left its outputs shader-readable; the transitions below are no-ops that state the contract.
    constexpr D3D12_RESOURCE_STATES shaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Texture& diffuse = m_pathTraceTemporalPass.GetDiffuseOutput();
    Texture& specular = m_pathTraceTemporalPass.GetSpecularOutput();
    Texture& moments = m_pathTraceTemporalPass.GetMomentsOutput();
    Texture& presentation = m_pathTraceReferencePass.GetOutputTexture();
    Texture& rawDiffuse = m_pathTraceReferencePass.GetDiffuseTexture();
    Texture& rawSpecular = m_pathTraceReferencePass.GetSpecularTexture();
    diffuse.Transition(commandList, shaderRead);
    rawDiffuse.Transition(commandList, shaderRead);
    rawSpecular.Transition(commandList, shaderRead);
    specular.Transition(commandList, shaderRead);
    moments.Transition(commandList, shaderRead);
    m_gBuffer.GetDepth().Transition(commandList, shaderRead);
    m_gBuffer.GetSurface().Transition(commandList, shaderRead);
    m_gBuffer.GetGeometricNormal().Transition(commandList, shaderRead);
    m_gBuffer.GetMaterial().Transition(commandList, shaderRead);
    presentation.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // The path tracer wrote this frame's running mean to the presentation texture; order that write first.
    D3D12_RESOURCE_BARRIER presentationBarrier{};
    presentationBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    presentationBarrier.UAV.pResource = presentation.GetResource();
    commandList->ResourceBarrier(1, &presentationBarrier);

    PathTraceSpatialConstants constants{};
    constants.inverseCameraRelativeViewProjection = CameraRelativeInverseViewProjection(view, projection);
    constants.cameraForward[0] = view.m[2][0];
    constants.cameraForward[1] = view.m[2][1];
    constants.cameraForward[2] = view.m[2][2];
    constants.diffuseIndex = diffuse.GetDescriptorIndex();
    constants.specularIndex = specular.GetDescriptorIndex();
    constants.momentsIndex = moments.GetDescriptorIndex();

    // DENOISE.3B: a stable frame filters the extended diffuse history; its length is a plain mean's sample count up to
    // the cap, so the effective count follows it (the moments stay the history's).
    if(m_pathTraceExtendedUsed)
    {
        Texture& extended = m_pathTraceTemporalPass.GetExtendedDiffuse();
        extended.Transition(commandList, shaderRead);
        constants.diffuseIndex = extended.GetDescriptorIndex();
        constants.diffuseMaximumHistory = PathTraceExtendedMaximum;
    }
    constants.depthIndex = m_gBuffer.GetDepth().GetSRVIndex();
    constants.surfaceIndex = m_gBuffer.GetSurface().GetSRVIndex();
    constants.geometricNormalIndex = m_gBuffer.GetGeometricNormal().GetSRVIndex();
    constants.materialIndex = m_gBuffer.GetMaterial().GetSRVIndex();
    constants.outputIndex = presentation.GetUAVIndex();
    constants.visualizationMode = static_cast<std::uint32_t>(visualization.view);
    constants.visualizationSignal = visualization.signal;
    constants.exposureScale = std::exp2(visualization.exposureStops);
    constants.differenceScale = visualization.differenceAmplification;
    constants.uncertaintyFloorLog10 = visualization.uncertaintyFloorLog10;
    constants.rawDiffuseIndex = rawDiffuse.GetDescriptorIndex();
    constants.rawSpecularIndex = rawSpecular.GetDescriptorIndex();

    // RTXPT D.3C.2: the a-trous iterations, for the normal presentation and for the views of the final spatial value
    // (2 and 3); every other view reads temporal inputs only and keeps the 5x5 pass's diagnostic entry point. Without
    // the pipelines or (two iterations) the intermediate, the 5x5 pass runs instead.
    const std::uint32_t iterations = (std::min)(m_pathTraceSpatialIterations, 2u);
    const bool atrousView = visualization.view == PathTraceDenoiserView::Normal ||
        visualization.view == PathTraceDenoiserView::SpatialRadiance ||
        visualization.view == PathTraceDenoiserView::Difference;
    const bool atrous = iterations > 0u && atrousView &&
        m_pathTraceSpatialPass.InitializeAtrous(m_graphicsDevice, m_shaderLibrary) &&
        (iterations < 2u || m_pathTraceSpatialPass.EnsureIntermediate(m_graphicsDevice.GetDevice(),
            m_resourceDescriptorHeap, m_sceneWidth, m_sceneHeight));

    if(atrous)
    {
        m_pathTraceSpatialPass.ExecuteAtrous(commandList, m_uploadBuffer, constants, iterations, m_sceneWidth,
            m_sceneHeight);
        m_pathTraceSpatialIterationsLastFrame = iterations;
        return;
    }

    m_pathTraceSpatialPass.Execute(commandList, m_uploadBuffer, constants, m_sceneWidth, m_sceneHeight);
    m_pathTraceSpatialIterationsLastFrame = 0;
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

bool Renderer::RenderUIOnly()
{
    m_dlssActiveLastFrame = false;
    PublishDlssReadout();
    m_referenceRayCounts.Invalidate();
    m_debugUI.SetReferenceRayCounts(m_referenceRayCounts.Get());
    if(m_packagedRuntime || m_resizeFailed || m_applicationWidth == 0 || m_applicationHeight == 0 ||
        m_sceneInitializationAttempt.candidate || !m_swapChain.GetCurrentBackBuffer())
    {
        return false;
    }

    // Same allocator/fence and submission association as scene frames. No scene histories,
    // upload regions, thumbnail requests or material publications are touched here.
    RecordingEligibilityScope recordingEligibility{*this};
    const auto slot = m_swapChain.GetCurrentBackBufferIndex();

    if(!BeginTrackedRecording(slot))
    {
        return false;
    }

    auto* commands = m_commandContext.GetCommandList();
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_swapChain.GetCurrentBackBuffer();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commands->ResourceBarrier(1, &barrier);

    const auto target = m_swapChain.GetCurrentRTV();
    constexpr float background[]{0.035f, 0.04f, 0.05f, 1.0f};
    commands->ClearRenderTargetView(target, background, 0, nullptr);
    commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(m_applicationWidth),
        static_cast<float>(m_applicationHeight), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(m_applicationWidth), static_cast<LONG>(m_applicationHeight)};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[]{m_resourceDescriptorHeap.GetHeap()};
    commands->SetDescriptorHeaps(1, heaps);
    m_debugUI.EndFrame(commands);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commands->ResourceBarrier(1, &barrier);

    // Submission failure may follow execution. Keep all owners alive and let the caller
    // fail closed; never retry/reset on an assumed unsubmitted list.
    if(!EndTrackedRecording() || !ExecuteTrackedRecording(slot))
    {
        return false;
    }

    return m_swapChain.Present();
}

Renderer::FrameRenderResult Renderer::Render(
    const Scene& scene,
    const RendererOptions& options)
{
    m_referenceRayCounts.StartFrame();
    struct RayCountFrameScope
    {
        ReferenceRayCounts& counts;
        DebugUI& ui;
        Renderer& renderer;
        ~RayCountFrameScope()
        {
            counts.FinishFrame();
            ui.SetReferenceRayCounts(counts.Get());
            if(!renderer.m_dlssFrameSubmitted) renderer.m_dlssActiveLastFrame = false;
            renderer.PublishDlssReadout();
        }
    } rayCountFrameScope{m_referenceRayCounts, m_debugUI, *this};
    m_dlssActiveLastFrame = false;
    m_dlssFrameSubmitted = false;

    TransitionTiming::Scope er1Scope("Frame Render", 0.0);
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
        // A failed result can follow GPU execution: never continue a running mean whose last
        // iteration may or may not have been written.
        m_pathTraceResetPending = true;
        m_pathTracePendingSamples = 0;
        InvalidatePreviousGBufferHistory();
        m_rtxdiResources.InvalidateHistory();
        LogAsyncDisplacementError();
        return FrameRenderResult::Error;
    }

    m_pathTraceStatus.accumulatedSamples += m_pathTracePendingSamples;
    m_pathTracePendingSamples = 0;
    m_pathTraceStatus.activeLastFrame = m_pathTraceRenderedLastFrame;

    return FrameRenderResult::Rendered;
}

RenderView Renderer::DefaultFlyRenderView(const Camera& camera)
{
    RenderView view;
    view.identity = {RenderViewSource::EditorFly};
    view.view = camera.GetViewMatrix();
    view.projection = camera.GetProjectionMatrix();
    view.position = camera.GetPosition();
    view.verticalFov = camera.GetVerticalFov();
    view.nearPlane = camera.GetNearPlane();
    view.farPlane = camera.GetFarPlane();
    view.aspectRatio = camera.GetAspectRatio();
    return view;
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
    UpdateSharcStatus(frameIndex);
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

    if(!m_debugUI.ReferenceRayCountsEnabled() || options.lightingMode != RendererLightingMode::PathTracedReference)
        m_referenceRayCounts.Invalidate();
    if(m_debugUI.ReferenceRayCountsEnabled()) m_referenceRayCounts.Poll(m_commandContext);
    m_debugUI.SetReferenceRayCounts(m_referenceRayCounts.Get());

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
    // SHARC.4C: before the material table and the scene-lighting latch, whose inputs include the cache identity.
    // SHARC and the denoiser option cover both raster-primary path-traced modes (SHARC.5B adds ReSTIR DI).
    const bool rasterPrimaryMode = options.lightingMode == RendererLightingMode::PathTracedRasterPrimary ||
        options.lightingMode == RendererLightingMode::PathTracedRasterPrimaryReSTIRDI;
    // ER.1 Phase 2: the requested denoiser and SHARC become effective only once their DXR libraries are prepared.
    const PathTraceDenoiserMode denoiser = GateDenoiserRequest(options.lightingMode,
        rasterPrimaryMode ? options.pathTraceDenoiserMode : PathTraceDenoiserMode::Off);
    SharcSettings sharc = options.sharc;
    sharc.enabled = GateSharcRequest(options.lightingMode, denoiser, options.sharc.enabled && rasterPrimaryMode);
    ApplySharcSettings(sharc, rasterPrimaryMode);
    m_sharcStatus.preparation = m_sharcPreparation;
    m_debugUI.SetPathTraceDenoiserPreparation(m_denoiserPreparation);
    ApplyPathTraceDenoiser(denoiser, rasterPrimaryMode);
    ApplyPathTraceDenoiserStages(options.pathTraceDenoiserStages);
    const bool queryReady = m_pathTraceReferencePass.IsSecondaryShadowRayQueryReady();
    const bool queryActive = options.pathTraceReference.secondaryShadowRayQuery && queryReady;
    const bool primaryQueryReady = m_pathTraceReferencePass.IsPrimaryDiShadowRayQueryReady();
    const bool primaryQueryActive = options.pathTraceReference.primaryDiShadowRayQuery && primaryQueryReady;
    m_pathTraceStatus.secondaryShadowRayQueryReady = queryReady;
    m_pathTraceStatus.primaryDiShadowRayQueryReady = primaryQueryReady;
    m_pathTraceStatus.primaryDiShadowRayQueryFailed = m_pathTraceReferencePass.IsPrimaryDiShadowRayQueryFailed();

    if(queryActive != m_pathTraceStatus.secondaryShadowRayQueryActive ||
        primaryQueryActive != m_pathTraceStatus.primaryDiShadowRayQueryActive)
    {
        // CPU selection only: both state objects and tables stay alive across in-flight submissions.
        // Reset before jitter, G-buffer reuse, NRD and DLSS consume this frame's history flags.
        ResetSceneHistory();
        m_pathTraceStatus.secondaryShadowRayQueryActive = queryActive;
        m_pathTraceStatus.primaryDiShadowRayQueryActive = primaryQueryActive;
    }

    m_pathTraceReferencePass.SelectSecondaryShadowRayQuery(queryActive);
    m_pathTraceReferencePass.SelectPrimaryDiShadowRayQuery(primaryQueryActive);

    ApplyCameraJitter(options.pathTraceCameraJitter);
    ApplyDlssInputPreparation(options.dlssInputPreparation);
    UpdateRadianceCacheD6(options.experimentalD6RadianceCache || m_sharcSettings.enabled);
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
        InvalidateSceneLighting(SceneLightingInvalidation::Material);
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
    // One coherent view feeds every view-dependent consumer below. Without an explicit view the
    // Editor fly camera is used exactly as before CAM.4 (same matrices, no new admission).
    const RenderView frameView = options.view ? *options.view : DefaultFlyRenderView(scene.GetCamera());

    // One coherent lighting source per frame: the legacy sun or the component shadow owner, never
    // both, plus the unshadowed Light table. A rejected snapshot publishes no component Lights.
    FrameLighting frameLighting;
    std::string lightingError;

    DirectionalLight legacySun;
    if(scene.UsesObjectLighting())
    {
        legacySun.intensity = 0.0f;
        legacySun.color = {};
        legacySun.ambientStrength = scene.GetLightingSettings().ambientStrength;
        legacySun.angularRadius = 0.0f;
    }
    else legacySun = scene.GetDirectionalLight();
    if(!BuildFrameLighting(legacySun, options.lighting ? &*options.lighting : nullptr,
        frameLighting, &lightingError))
    {
        Log::Error("Scene lighting rejected for this frame: " + lightingError);
        frameLighting = FrameLighting{};
        frameLighting.sun = legacySun;

        if(options.lighting)
        {
            frameLighting.sun.intensity = 0.0f;
            frameLighting.sun.color = {0.0f, 0.0f, 0.0f};
        }
    }

    const DirectionalLight& light = frameLighting.sun;
    m_recordedSun = frameLighting.sun;
    m_recordedAdditionalLightCount = frameLighting.table.lightCount;

    // A new view identity is a camera cut. Deferred frames return before recording, so a pending
    // cut is consumed only here, with the existing stateful history policy of this recording.
    if(m_renderedViewIdentity && *m_renderedViewIdentity != frameView.identity)
    {
        ResetSceneHistory();
        ++m_cameraCutCount;
    }

    m_renderedViewIdentity = frameView.identity;
    m_recordedView = frameView.view;
    m_recordedProjection = frameView.projection;

    // RTXPT.1 mode selection for this recording. An unavailable or invalid request renders Legacy.
    // Legacy temporal histories do not advance while the path tracer renders, so leaving the
    // reference mode invalidates them at this same stateful boundary as a camera cut.
    // RTXPT.2: the raster-primary mode is the same progressive path tracer with its first vertex taken
    // from this frame's G-buffer; it shares the settings, accumulation and resets.
    // RTXPT.3: the raster-primary mode with RTXDI direct light at its first vertex; same settings and resets.
    const bool pathTraceSettingsValid = hasRenderableMeshes && IsValidPathTraceReferenceSettings(options.pathTraceReference);

    // Evaluates one mode's gates for this recording, creating what it needs on first use. Returns whether
    // it can run, its primary constructor and, when it cannot, why.
    const auto evaluateLightingMode = [&](RendererLightingMode mode, PathTracePrimary& primary, std::string& failure)
    {
        const bool referenceRequested = mode == RendererLightingMode::PathTracedReference;
        const bool rasterPrimaryRequested = mode == RendererLightingMode::PathTracedRasterPrimary;
        const bool restirDIRequested = mode == RendererLightingMode::PathTracedRasterPrimaryReSTIRDI;

        if(rasterPrimaryRequested)
        {
            EvaluatePathTraceRasterPrimary();
        }

        if(restirDIRequested)
        {
            EvaluatePathTraceReSTIRDI();
        }

        const bool rasterPrimaryAvailable =
            m_rasterPrimaryStatus.availability == PathTraceRasterPrimaryAvailability::Available;
        const bool restirDIAvailable = m_restirDIStatus.availability == PathTraceRasterPrimaryAvailability::Available;
        primary = m_pathTraceReferencePrimary;

        if(rasterPrimaryRequested)
        {
            primary = PathTracePrimary::Raster;
        }
        else if(restirDIRequested)
        {
            primary = PathTracePrimary::RasterReSTIRDI;
        }

        std::string referenceFailure;
        const auto referenceRunnable = [&]()
        {
            return EnsurePathTraceReference() && (primary == PathTracePrimary::Camera ||
                m_pathTraceReferencePass.EnsurePrimaryPipeline(m_graphicsDevice, m_shaderLibrary, primary,
                    referenceFailure));
        };
        const bool runnable = pathTraceSettingsValid &&
            ((referenceRequested && referenceRunnable()) || (rasterPrimaryRequested && rasterPrimaryAvailable) ||
            (restirDIRequested && restirDIAvailable));

        if(runnable || mode == RendererLightingMode::Legacy)
        {
            failure.clear();
        }
        else if(!pathTraceSettingsValid)
        {
            failure = "The scene has no renderable meshes or the path tracing settings are invalid.";
        }
        else if(referenceRequested)
        {
            failure = !m_pathTraceStatus.unavailableReason.empty() ? m_pathTraceStatus.unavailableReason :
                referenceFailure;
        }
        else if(rasterPrimaryRequested)
        {
            failure = m_rasterPrimaryStatus.reason;
        }
        else
        {
            failure = m_restirDIStatus.reason;
        }

        return runnable;
    };

    // Async DXR preparation (Gate 2): options.lightingMode is the requested mode. While its DXR libraries
    // compile on the worker, the active mode keeps rendering; the requested one is attempted only once they
    // are accepted, and its pipelines and resources are then created here on the render thread.
    const LightingModeTransition::Clock::time_point transitionTime = LightingModeTransition::Clock::now();
    const RendererLightingMode candidateMode = m_lightingModeTransition.SelectCandidate(
        options.lightingMode,
        m_shaderCompileWorkerRunning,
        transitionTime,
        [&]()
        {
            return IsLightingModeReady(options.lightingMode);
        },
        [&]()
        {
            return PrepareLightingModeLibraries(options.lightingMode);
        });

    if(m_lightingModeTransition.GetStatus().stage == LightingModeTransitionStage::CreatingPipeline)
    {
        CreateLightingModeResources(candidateMode);
    }

    PathTracePrimary pathTracePrimary = m_pathTraceReferencePrimary;
    std::string candidateFailure;
    bool runPathTrace = evaluateLightingMode(candidateMode, pathTracePrimary, candidateFailure);
    const RendererLightingMode recordedMode = m_lightingModeTransition.Resolve(
        candidateMode,
        candidateMode == RendererLightingMode::Legacy || runPathTrace,
        candidateFailure,
        transitionTime,
        [&](RendererLightingMode mode)
        {
            PathTracePrimary ignoredPrimary = m_pathTraceReferencePrimary;
            std::string ignoredFailure;
            return evaluateLightingMode(mode, ignoredPrimary, ignoredFailure);
        });

    // A failed activation falls back to the previous active mode (or Legacy) in this same recording.
    if(recordedMode != candidateMode)
    {
        std::string ignoredFailure;
        runPathTrace = evaluateLightingMode(recordedMode, pathTracePrimary, ignoredFailure);
    }

    m_lightingModeTransition.CommitRendered(runPathTrace ? recordedMode : RendererLightingMode::Legacy);

    if(m_pathTraceRenderedLastFrame && !runPathTrace)
    {
        ResetSceneHistory();
    }

    // Set before recording: a failure later in this frame keeps both invalidations conservative.
    const bool pathTraceEnteredThisFrame = runPathTrace && !m_pathTraceRenderedLastFrame;
    m_pathTraceRenderedLastFrame = runPathTrace;
    m_pathTracePendingSamples = 0;

    if(pathTraceEnteredThisFrame)
    {
        m_pathTraceResetPending = true;
    }

    // A copy left from an earlier, unsubmitted recording never carries over, nor does its temporal history write.
    m_pendingPreviousGBufferCopy.reset();
    m_pathTraceTemporalPass.AbandonPendingHistory();
    m_nrdPathTraceDispatchedThisFrame = false;
    if(m_nrdHybridDispatchedThisFrame)
    {
        m_nrdHybridHistoryValid = false;
    }

    m_nrdHybridDispatchedThisFrame = false;
    m_nrdRelaxDispatchedThisFrame = false;
    m_nrdConfidenceDispatchedThisFrame = false;
    m_previousGBufferAvailableThisFrame = false;
    m_rtxdiResources.AbandonFrame();

    const bool pathTracePrimaryChanged = runPathTrace && m_previousGBufferPrimary &&
        *m_previousGBufferPrimary != pathTracePrimary;

    if(pathTraceEnteredThisFrame || pathTracePrimaryChanged)
    {
        InvalidatePreviousGBufferHistory();
        m_rtxdiResources.InvalidateHistory();
    }

    if(runPathTrace)
    {
        m_previousGBufferPrimary = pathTracePrimary;
        EnsurePreviousGBufferHistory();
    }

    const Matrix4 currentView = frameView.view;
    const Matrix4 currentProjection = frameView.projection;
    const Matrix4 currentViewProjection = currentProjection * currentView;

    // UPSCALING.2B: the unjittered matrices above identify the camera (accumulation key, motion vectors, overlays,
    // previous-frame state); the render matrices rasterize the G-buffer and reconstruct positions from it.
    const bool jitterApplies = runPathTrace &&
        (pathTracePrimary == PathTracePrimary::Raster || pathTracePrimary == PathTracePrimary::RasterReSTIRDI);
    // UPSCALING.3C: DLSS mode jitters and uses the DLSS phase policy. UPSCALING.3C.4: only while DLSS can evaluate
    // (available, and its feature not failed for these extents); otherwise the frame is the plain diagnostic resolve,
    // unjittered unless the experimental jitter option asks for it.
    const DlssFeatureStatus& dlssFeature = m_dlssBackend.GetFeatureStatus();
    const bool dlssFeatureFailed = dlssFeature.createResult != 0 && !dlssFeature.active &&
        dlssFeature.renderWidth == m_sceneWidth && dlssFeature.renderHeight == m_sceneHeight &&
        dlssFeature.outputWidth == m_outputWidth && dlssFeature.outputHeight == m_outputHeight;
    const bool dlssAvailable = m_dlssBackend.GetStatus().availability == DlssAvailability::Available &&
        !m_dlssBackend.availabilityOverrideForTesting;
    const bool dlssDenoisedColor = m_pathTraceTemporal || m_dlssColorOverrideForTesting;
    m_dlssFallbackReason = "";

    if(!dlssAvailable)
    {
        m_dlssFallbackReason = "DLSS unavailable";
    }
    else if(m_dlssPresetUnsupported)
    {
        m_dlssFallbackReason = "the preset is not supported at this output resolution";
    }
    else if(dlssFeatureFailed)
    {
        m_dlssFallbackReason = "DLSS feature creation failed";
    }
    else if(!dlssDenoisedColor)
    {
        m_dlssFallbackReason = "the path-tracing denoiser is off (no per-frame denoised colour)";
    }

    m_dlssModeThisFrame = jitterApplies && IsDlssRenderScale(m_renderScale) && m_dlssFailure.empty() && m_dlssFallbackReason[0] == '\0';
    if(IsDlssRenderScale(m_renderScale) && !m_dlssModeThisFrame && m_dlssFailure.empty())
        FailDlss("Frame eligibility", jitterApplies ? m_dlssFallbackReason : "not an active raster-primary frame");
    const bool dlssPhases = m_dlssInputPreparation || m_dlssModeThisFrame;
    const std::uint32_t jitterPhaseCount = dlssPhases ?
        DlssJitterPhaseCount(m_sceneWidth, m_sceneHeight, m_outputWidth, m_outputHeight) : CameraJitterPhaseCount;
    m_cameraJitterStatus = NextCameraJitter(jitterApplies && (m_cameraJitterOption || m_dlssModeThisFrame),
        jitterPhaseCount);
    m_debugUI.SetPathTraceCameraJitterReadout(m_cameraJitterStatus.active, m_cameraJitterStatus.phase,
        m_cameraJitterStatus.phaseCount, m_cameraJitterStatus.x, m_cameraJitterStatus.y);
    const Matrix4 renderProjection = m_cameraJitterStatus.active ?
        JitterProjection(currentProjection, m_cameraJitterStatus.x, m_cameraJitterStatus.y, m_sceneWidth, m_sceneHeight) :
        currentProjection;
    const Matrix4 renderViewProjection = m_cameraJitterStatus.active ? renderProjection * currentView :
        currentViewProjection;

    CameraConstants cameraConstants = BuildFrameCameraConstants(
        currentView, currentViewProjection, m_previousView, m_previousViewProjection, m_hasPreviousViewProjection);
    cameraConstants.unjitteredViewProjection = currentViewProjection;
    cameraConstants.viewProjection = renderViewProjection;
    // UPSCALING.3D: DLSS reconstructs output-resolution texture detail from biased render samples.
    cameraConstants.textureMipBias = 0.0f;

    if(m_dlssModeThisFrame)
    {
        const float offset = m_dlssMipBiasOffsetForTesting ? *m_dlssMipBiasOffsetForTesting : DlssTextureMipBiasOffset;
        cameraConstants.textureMipBias = std::isnan(offset) ? 0.0f :
            std::log2(static_cast<float>(m_sceneWidth) / static_cast<float>(m_outputWidth)) + offset;
    }

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

    const Vector3& cameraPosition = frameView.position;

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

    // The whole table (about 4 KB) is uploaded so the bound buffer always covers the declared
    // cbuffer; unused records are zero and the shader reads no further than lightCount.
    const D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstantsAddress = m_uploadBuffer.Allocate(
        &frameLighting.table, sizeof(SceneLightConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if (sceneLightConstantsAddress == 0)
    {
        Log::Error("Failed to allocate scene light constants.");
        return false;
    }

    // These history resets/writes intentionally precede later uploads and recording failures.
    const Vector3 currentLightDirection = light.direction.Normalized();
    bool giLightingChanged = m_hasPreviousGILocalLights &&
        !SameLocalLightInputs(m_previousGILocalLights, frameLighting.table);

    if(giLightingChanged)
    {
        m_giSignedCorrectionPass.ResetHistory();
    }

    m_previousGILocalLights = frameLighting.table;
    m_hasPreviousGILocalLights = true;

    if (m_hasPreviousShadowLightDirection)
    {
        const float directionDifference = (currentLightDirection - m_previousShadowLightDirection).Length();

        if (directionDifference > 0.0001f || light.angularRadius != m_previousShadowLightAngularRadius)
        {
            m_rayTracedShadowPass.ResetHistory();
            m_giSignedCorrectionPass.ResetHistory();
            giLightingChanged = true;
        }
    }

    m_previousShadowLightDirection = currentLightDirection;
    m_previousShadowLightAngularRadius = light.angularRadius;

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
            giLightingChanged = true;
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

    // Unjittered: the next frame's motion vectors exclude jitter.
    m_previousViewProjection = currentViewProjection;
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
    giConstants.importanceConditionalCDFIndex = m_environment.GetImportanceConditionalCDF().GetDescriptorIndex();
    giConstants.importanceMarginalCDFIndex = m_environment.GetImportanceMarginalCDF().GetDescriptorIndex();
    giConstants.importanceWidth = m_environment.GetImportanceWidth();
    giConstants.importanceHeight = m_environment.GetImportanceHeight();
    giConstants.importanceTotalWeight = m_environment.GetImportanceTotalWeight();

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
#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
    if(!runPathTrace || !m_sharcSettings.enabled)
    {
        m_sharcDiagnosticPresentedThisFrame = false;
    }
#endif

    // NRD.3: a cleared diagnostic request frees its textures (one GPU wait, before this frame records anything that
    // could reference them). NRD.4B: path-traced REBLUR owns the same preparation textures while it is the denoiser, so
    // they are kept then (released on the first frame of another mode).
    // ER.1 Phase 3: a retained path-traced REBLUR keeps them for its next activation.
    if(!m_nrdPathTraceDiagnostic && m_pathTraceDenoiserMode != PathTraceDenoiserMode::NrdReblur &&
        !m_nrdPathTraceDenoiser.IsInitialized())
    {
        ReleaseNrdPathTraceDiagnostic();
    }

    // HYBRID.3: the same for the hybrid diagnostic's instance and preparation. HYBRID.5: the selected hybrid holds them as
    // well.
    const bool hybridSelected = m_pathTraceDenoiserMode == PathTraceDenoiserMode::NrdHybrid;

    if(!hybridSelected)
    {
        m_nrdHybridPresentedLastFrame = false;
    }

    if(!m_nrdHybridDispatchDiagnostic && !hybridSelected)
    {
        ReleaseNrdHybridDenoiser();
    }

    // NRD.RELAX.2: the same for the RELAX preparation diagnostic. NRD.RELAX.3: the RELAX dispatch diagnostic uses the same
    // preparation and raw distance, and holds its own instance.
    // NRD.RELAX.4: the selected RELAX holds the instance and the preparation as well.
    const bool relaxSelected = m_pathTraceDenoiserMode == PathTraceDenoiserMode::NrdRelax;

    if(!relaxSelected)
    {
        m_nrdRelaxPresentedLastFrame = false;
    }

    // ER.1 Phase 3: an inactive RELAX keeps its instance, preparation and raw-distance producer until resize, an
    // incompatible configuration or shutdown. It is marked new, so its next frame starts with CLEAR_AND_RESTART exactly as
    // a recreated instance did; nothing of its earlier history is reused.
    if(!m_nrdRelaxDispatchDiagnostic && !relaxSelected && m_nrdRelaxDenoiser.IsInitialized())
    {
        m_nrdRelaxInstanceNew = true;
        m_nrdRelaxHistoryValid = false;
    }

    if(!m_nrdRelaxDispatchDiagnostic)
    {
        ReleaseNrdRelaxDiagnosticTexture();
    }

    // NRD.4B, ER.1 Phase 3: an inactive path-traced REBLUR is kept the same way.
    if(m_pathTraceDenoiserMode != PathTraceDenoiserMode::NrdReblur)
    {
        m_nrdPathTracePresentedLastFrame = false;

        if(m_nrdPathTraceDenoiser.IsInitialized())
        {
            m_nrdPathTraceInstanceNew = true;
            m_nrdPathTraceHistoryValid = false;
        }
    }

    // ER.1 Phase 3.1: this recording's NRD groups are marked used; an inactive one may be evicted (never waiting).
    UpdateNrdRetention(runPathTrace);

    if(runPathTrace)
    {
        // The path tracer replaces lighting only; the G-buffer above still feeds selection outlines and
        // overlay depth testing, and is the raster primary's first vertex (EndGeometryPass left it readable).
        // No legacy lighting pass is recorded; legacy histories reset when the mode is left.
        // reconstructionConstants.inverseViewProjection inverts the render (rasterization) view-projection.
        // NRD.2: path tracing never uses the hybrid mode's NRD instances. ER.1 Phase 3: they are kept for the next Legacy
        // frame, which restarts them with CLEAR_AND_RESTART (m_nrdExecutedLastFrame is false after a path-traced frame).

        if(!RecordPathTraceReference(commandList, frameIndex, pathTracePrimary, currentView, renderProjection,
            currentProjection, reconstructionConstants.inverseViewProjection, frameView.position, frameLighting,
            sceneLightConstantsAddress, options.pathTraceReference))
        {
            return false;
        }

        // The G-buffer is final here; the copy precedes tone mapping and the Editor overlays and restores
        // every G-buffer state it changes. Its camera reconstructs positions from it, so it is the render projection.
        const PreviousGBufferCamera copiedCamera{currentView, renderProjection,
            CameraRelativeInverseViewProjection(currentView, renderProjection), frameView.position};
        m_previousGBufferAvailableThisFrame = m_previousGBufferValid;
        RecordPreviousGBufferCopy(commandList, copiedCamera);
    }
    // Preserve the fixed AO -> shadows -> reflections -> GI -> denoise -> lighting order.
    else if (hasRenderableMeshes)
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
                                  giConstantsAddress, lightConstantsAddress, sceneLightConstantsAddress, giWidth, giHeight);

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
        const bool resetGIHistory = resetNrdHistory || giLightingChanged ||
            runSignedCorrection != m_signedCorrectionExecutedLastFrame;

        if (runNrd)
        {
            if (!EnsureHybridNrdDenoisers())
            {
                return false;
            }

            m_nrdReflectionDenoiser.NewFrame();
            m_nrdGIDenoiser.NewFrame();

            // The hybrid mode never jitters; NRD reads its motion as stored by NrdPreparePass (scale 1, 2.5D).
            NrdDenoiser::FrameSettings nrdSettings;
            nrdSettings.view = currentView;
            nrdSettings.projection = currentProjection;
            nrdSettings.previousView = previousNrdView;
            nrdSettings.previousProjection = previousNrdProjection;
            nrdSettings.frameIndex = m_nrdFrameIndex;
            nrdSettings.denoisingRange = nrdDenoisingRange;
            nrdSettings.accumulationMode =
                resetNrdHistory ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;

            if (!m_nrdReflectionDenoiser.SetCommonSettings(nrdSettings))
            {
                return false;
            }

            if (!m_nrdReflectionDenoiser.SetDenoiserSettings(HybridNrdReflectionIdentifier, HybridNrdReflectionSettings()))
            {
                Log::Error("Failed to configure NRD specular REBLUR settings.");
                return false;
            }

            nrdSettings.accumulationMode =
                resetGIHistory ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;

            if (!m_nrdGIDenoiser.SetCommonSettings(nrdSettings))
            {
                return false;
            }

            if (!m_nrdGIDenoiser.SetDenoiserSettings(HybridNrdGIIdentifier, HybridNrdGISettings()))
            {
                Log::Error("Failed to configure NRD diffuse REBLUR settings.");
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

            NrdDenoiser::Inputs reflectionInputs;
            reflectionInputs.motion = m_nrdReflectionPreparePass.GetMotionTexture().GetResource();
            reflectionInputs.normalRoughness = &m_nrdReflectionPreparePass.GetNormalRoughnessTexture();
            reflectionInputs.viewZ = &m_nrdReflectionPreparePass.GetViewZTexture();
            reflectionInputs.specularRadianceHitDistance = &m_nrdReflectionPreparePass.GetSpecRadianceHitDistTexture();
            m_nrdReflectionDenoiser.Denoise(commandList, HybridNrdReflectionIdentifier, reflectionInputs);

            m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdReblurSpecular);

            m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdReblurDiffuse);

            NrdDenoiser::Inputs giInputs;
            giInputs.motion = m_nrdGIPreparePass.GetMotionTexture().GetResource();
            giInputs.normalRoughness = &m_nrdGIPreparePass.GetNormalRoughnessTexture();
            giInputs.viewZ = &m_nrdGIPreparePass.GetViewZTexture();
            giInputs.diffuseRadianceHitDistance = &m_nrdGIPreparePass.GetDiffRadianceHitDistTexture();
            m_nrdGIDenoiser.Denoise(commandList, HybridNrdGIIdentifier, giInputs);

            m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdReblurDiffuse);

            commandList->SetDescriptorHeaps(1, descriptorHeaps);

            m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdResolve);

            m_nrdReflectionResolvePass.Execute(commandList, m_gBuffer,
                                               m_nrdReflectionDenoiser.GetSpecularOutput(HybridNrdReflectionIdentifier),
                                               m_uploadBuffer, reconstructionConstants.inverseViewProjection, currentView,
                                               CurrentMaterialBuffer().GetDescriptorIndex(), m_sceneWidth, m_sceneHeight);

            m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdResolve);

            m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::NrdDiffuseResolve);
            m_nrdDiffuseResolvePass.Execute(commandList, m_gBuffer, m_nrdGIDenoiser.GetDiffuseOutput(HybridNrdGIIdentifier),
                                            m_nrdGIPreparePass.GetViewZTexture(), *giNrdInput, m_uploadBuffer, m_sceneWidth, m_sceneHeight,
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
            lightConstantsAddress, sceneLightConstantsAddress, static_cast<unsigned int>(debugView),
            m_debugUI.IsShadingNormalCorrectionEnabled());

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

    // UPSCALING.3B: DLSS inputs from the final G-buffer and scene colour, only while preparation is on. Nothing reads
    // them yet (UPSCALING.3C evaluates DLSS here, in place of the diagnostic resolve's colour).
    m_upscalerInputs.prepared = false;

    if(((m_dlssInputPreparation && m_dlssFailure.empty()) || m_dlssModeThisFrame)
#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
        && !m_sharcDiagnosticPresentedThisFrame
#endif
        )
    {
        do
        {
            if(!m_dlssInputPreparationPass.Ensure(m_graphicsDevice.GetDevice(), m_shaderLibrary, m_resourceDescriptorHeap,
                m_sceneWidth, m_sceneHeight))
            {
                FailDlss("Input preparation", m_dlssInputPreparationPass.FailureReason());
                m_dlssModeThisFrame = false;
                break;
            }

            DlssInputPreparationPass::Matrices matrices{};
            matrices.inverseCameraRelativeRenderViewProjection = CameraRelativeInverseViewProjection(currentView, renderProjection);
            matrices.viewProjection = cameraConstants.unjitteredViewProjection;
            matrices.previousViewProjection = cameraConstants.previousViewProjection;
            // The tone mapper's fixed exposure (ToneMapPass); DLSS reads the same value from the 1x1 texture.
            constexpr float toneMapExposure = 1.0f;

            m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::DlssInputPreparation);
            const bool prepared = m_dlssInputPreparationPass.Record(commandList, m_uploadBuffer, m_gBuffer.GetMotion(),
                m_gBuffer.GetDepth(), matrices, toneMapExposure);
            m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::DlssInputPreparation);

            if(!prepared)
            {
                FailDlss("Input preparation", m_dlssInputPreparationPass.FailureReason());
                m_dlssModeThisFrame = false;
                break;
            }

            UpscalerFrameInputs& inputs = m_upscalerInputs;
            inputs.prepared = true;
            inputs.renderWidth = m_sceneWidth;
            inputs.renderHeight = m_sceneHeight;
            inputs.outputWidth = m_outputWidth;
            inputs.outputHeight = m_outputHeight;
            inputs.color = &m_sceneColor;
            // NRD.4B: REBLUR's presentation is per-frame denoised colour, as the Engine denoiser's is; NRD.RELAX.4: RELAX's too;
            // HYBRID.5: the hybrid's too.
            inputs.colorIsPerFrameDenoised = runPathTrace && m_pathTracePendingSamples > 0 && ((m_pathTraceTemporalRanLastFrame &&
                (m_pathTraceSpatialRanLastFrame || m_pathTraceTemporalPresentation)) || m_nrdPathTracePresentedLastFrame ||
                m_nrdRelaxPresentedLastFrame || m_nrdHybridPresentedLastFrame);
            inputs.depth = &m_gBuffer.GetDepth();
            inputs.engineMotion = &m_gBuffer.GetMotion();
            inputs.dlssMotionVectors = &m_dlssInputPreparationPass.GetMotionVectors();
            inputs.exposure = &m_dlssInputPreparationPass.GetExposure();
            inputs.exposureValue = toneMapExposure;
            inputs.jitterX = m_cameraJitterStatus.x;
            inputs.jitterY = m_cameraJitterStatus.y;
            inputs.jitterPhaseCount = m_cameraJitterStatus.phaseCount;
            inputs.reset = m_upscalerHistoryResetPending;
            m_upscalerHistoryResetPending = false;
        } while(false);
    }

#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
    if(m_sharcDiagnosticPresentedThisFrame)
    {
        // No diagnostic RGB ever enters NGX. The next normal evaluation must restart its color history.
        m_upscalerHistoryResetPending = true;
    }
#endif
    if(!PresentationResolveActive() || !m_dlssModeThisFrame
#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
        || m_sharcDiagnosticPresentedThisFrame
#endif
        )
    {
        m_dlssActiveLastFrame = false;
        m_dlssEvaluatedLastFrame = false;
        m_dlssInactiveReason = "";

        if(IsDlssRenderScale(m_renderScale))
        {
            m_dlssInactiveReason = jitterApplies ? m_dlssFallbackReason : "not a raster-primary path-traced frame";
#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
            if(m_sharcDiagnosticPresentedThisFrame)
            {
                m_dlssInactiveReason = "SHARC diagnostic presentation bypasses DLSS";
            }
#endif
        }
    }

    // UPSCALING.2C: at a reduced render extent the diagnostic resolve first brings scene colour, selection mask and
    // depth to the output extent, and everything from tone mapping on runs there as in Native. Debug views that read
    // render-extent textures are not shown then. Native records none of this and keeps its viewport.
    RenderTarget* presentedColor = &m_sceneColor;
    RenderTarget* presentedSelectionMask = &m_selectionMask;
    DepthBuffer* presentedDepth = &m_gBuffer.GetDepth();
    Texture* presentedDebugTexture = debugTexture;
    RenderTarget* presentedDebugRenderTarget = debugRenderTarget;

    if(PresentationResolveActive())
    {
        const D3D12_VIEWPORT outputViewport{0.0f, 0.0f, static_cast<float>(m_outputWidth),
            static_cast<float>(m_outputHeight), 0.0f, 1.0f};
        const D3D12_RECT outputScissor{0, 0, static_cast<LONG>(m_outputWidth), static_cast<LONG>(m_outputHeight)};
        commandList->RSSetViewports(1, &outputViewport);
        commandList->RSSetScissorRects(1, &outputScissor);

        // UPSCALING.3D: when the Editor overlay prepass supplies depth and mask (every jittered Editor frame, so every
        // Editor DLSS frame), DLSS is evaluated first and the resolve, whose colour DLSS would overwrite and whose
        // depth and mask nothing would read, runs only if DLSS did not evaluate. Otherwise (the packaged runtime) the
        // resolve runs first for its depth and mask and DLSS replaces its colour. Either way the output is written.
        const bool overlayPrepass = !m_packagedRuntime && m_cameraJitterStatus.active;
        const auto recordResolve = [&]()
        {
            RestoreDlssGraphicsState(commandList);
            m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex,
                GpuTimestampProfiler::Region::PresentationResolve);
            // The G-buffer and mask were rasterized with this frame's jitter; the overlays drawn after use the
            // unjittered camera (UPSCALING.3C.3).
            m_presentationResolvePass.Execute(commandList, m_sceneColor, m_gBuffer.GetDepth(), m_selectionMask,
                m_presentationColor, m_presentationSelectionMask, m_presentationDepth, m_cameraJitterStatus.x,
                m_cameraJitterStatus.y
#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
                , m_sharcDiagnosticPresentedThisFrame
#endif
                );
            m_gpuTimestampProfiler.EndRegion(commandList, frameIndex,
                GpuTimestampProfiler::Region::PresentationResolve);
        };
        const bool dlssFirst = m_dlssModeThisFrame && overlayPrepass
#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
            && !m_sharcDiagnosticPresentedThisFrame
#endif
            ;

        if(!dlssFirst)
        {
            recordResolve();
        }

        // UPSCALING.3C: NGX leaves the command list's state undefined, so everything the following passes rely on is
        // set again.
        const bool dlssEvaluated = m_dlssModeThisFrame &&
#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
            !m_sharcDiagnosticPresentedThisFrame &&
#endif
            RecordDlssEvaluation(commandList, frameIndex);
        m_dlssActiveLastFrame = dlssEvaluated;
        m_dlssEvaluatedLastFrame = dlssEvaluated;
        m_presentationResolveSkippedLastFrame = dlssFirst && dlssEvaluated;

        if(dlssEvaluated)
        {
            ID3D12DescriptorHeap* heaps[] = {m_resourceDescriptorHeap.GetHeap()};
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->RSSetViewports(1, &outputViewport);
            commandList->RSSetScissorRects(1, &outputScissor);
        }

        // DLSS did not evaluate (creation or evaluation failed, or inputs missing): the resolve's colour is presented.
        if(!dlssEvaluated && (dlssFirst || m_dlssModeThisFrame))
        {
            recordResolve();
        }

        presentedColor = &m_presentationColor;
        presentedSelectionMask = &m_presentationSelectionMask;
        presentedDepth = &m_presentationDepth;
        presentedDebugTexture = nullptr;
        presentedDebugRenderTarget = nullptr;
    }

    // UPSCALING.3C.4: a jittered Editor frame gets unjittered overlay depth and selection mask at the output extent.
    if(!m_packagedRuntime && m_cameraJitterStatus.active)
    {
        CameraConstants overlayCamera = cameraConstants;
        overlayCamera.viewProjection = overlayCamera.unjitteredViewProjection;
        const D3D12_GPU_VIRTUAL_ADDRESS overlayCameraAddress = m_uploadBuffer.Allocate(&overlayCamera,
            sizeof(CameraConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::EditorOverlayPrepass);
        const bool prepassed = overlayCameraAddress != 0 &&
            RecordEditorOverlayPrepass(commandList, scene, options, overlayCameraAddress);
        m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::EditorOverlayPrepass);

        if(!prepassed)
        {
            Log::Error("Editor overlay prepass failed.");
            return false;
        }

        presentedSelectionMask = &m_overlaySelectionMask;
        presentedDepth = &m_overlayDepth;
    }

    // Final composition precedes collider lines and the distinct Editor/packaged presentation paths.
    m_gpuTimestampProfiler.BeginRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ToneMap);

    // Legacy diagnostic views describe legacy passes; the reference image is always shown as is, except a D.3B.0 scalar
    // denoiser view, whose colours are already display-encoded.
    const ToneMapDebugView pathTraceDebugView = (m_pathTraceScalarViewLastFrame
#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
        || m_sharcDiagnosticPresentedThisFrame
#endif
        ) ?
        ToneMapDebugView::PathTraceDenoiserScalar : ToneMapDebugView::None;
    const ToneMapDebugView presentedDebugView = runPathTrace ? pathTraceDebugView : debugView;

    m_finalSceneColor.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_toneMapPass.Execute(commandList, *presentedColor, m_finalSceneColor.GetRTV(), presentedDebugTexture,
                          hasRenderableMeshes ? presentedDebugView : ToneMapDebugView::None, nrdDenoisingRange,
                          presentedDebugRenderTarget, presentedSelectionMask, options.selectionOutlineColor,
                          presentedDebugView == ToneMapDebugView::None ? options.selectionOutlineThickness : 0,
                          presentedDepth->GetSRVIndex());

    m_gpuTimestampProfiler.EndRegion(commandList, frameIndex, GpuTimestampProfiler::Region::ToneMap);

    // Diagnostics only: the image before the overlays, for overlay-contribution measurements.
    if(m_preOverlayCopyForTesting && m_preOverlayCopyForTesting->GetWidth() == m_finalSceneColor.GetWidth() &&
        m_preOverlayCopyForTesting->GetHeight() == m_finalSceneColor.GetHeight())
    {
        m_finalSceneColor.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_preOverlayCopyForTesting->Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyResource(m_preOverlayCopyForTesting->GetResource(), m_finalSceneColor.GetResource());
        m_preOverlayCopyForTesting->Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_finalSceneColor.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    // Draw only into the final editor image: temporal history and lighting must never see these lines.
    // The grid goes first so collider lines stay on top of it.
    // Editor overlays are composed only into the Editor fly view; a Game Camera frame never shows them.
    const bool editorOverlayFrame = !m_packagedRuntime && frameView.identity.source == RenderViewSource::EditorFly;

    if (editorOverlayFrame && options.editorGrid)
    {
        m_editorGridPass.Execute(commandList, m_finalSceneColor, *presentedDepth, currentView,
            currentProjection, frameView.position, *options.editorGrid);
    }

    const EditorColliderOverlay* colliderOverlay = options.editorColliderOverlay;
    if (editorOverlayFrame && colliderOverlay)
    {
        if (colliderOverlay->Matches(scene, m_sceneResourceLifetime))
        {
            m_editorColliderPass.Execute(commandList, m_finalSceneColor, *presentedDepth,
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
        m_toneMapPass.Execute(commandList, *presentedColor, rtvHandle, nullptr, ToneMapDebugView::None,
            nrdDenoisingRange, nullptr, presentedSelectionMask, options.selectionOutlineColor, 0,
            presentedDepth->GetSRVIndex());
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
        m_referenceRayCounts.Invalidate();
        m_debugUI.SetReferenceRayCounts(m_referenceRayCounts.Get());
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
        m_referenceRayCounts.Invalidate();
        m_debugUI.SetReferenceRayCounts(m_referenceRayCounts.Get());
        m_materialDependencies.MarkUncertain(frameIndex);
        m_radianceCacheD6.MarkSubmissionUncertain();

        for (auto& entry : m_preparedTextureUploads)
        {
            entry.upload->MarkUncertain(textureUploadGeneration);
        }

        m_gpuTimestampProfiler.BreakFrameSequence();

        Log::Error("Failed to submit command list or signal its fence; GPU work may already be in flight.");
        return false;
    }

    m_dlssFrameSubmitted = true;
    m_referenceRayCounts.Commit(frameIndex, textureUploadGeneration, m_commandContext);

    // The copy is now ordered on the queue ahead of every later frame; it becomes the previous frame. So does the
    // temporal history written by this frame (RTXPT D.2B), which is validated against that copy.
    CommitPreviousGBufferCopy();
    m_pathTraceTemporalPass.CommitHistory();
    // ER.1 Phase 3.1: the NRD groups this frame used were last used by this submission.
    CommitNrdRetentionSubmission();

    // NRD.4B: a submitted REBLUR frame is the previous frame of the next; any other submitted frame is a gap.
    if(m_nrdPathTraceDispatchedThisFrame)
    {
        m_nrdPathTracePreviousView = m_nrdPathTracePendingView;
        m_nrdPathTracePreviousProjection = m_nrdPathTracePendingProjection;
        m_nrdPathTracePreviousJitter[0] = m_nrdPathTracePendingJitter[0];
        m_nrdPathTracePreviousJitter[1] = m_nrdPathTracePendingJitter[1];
        m_nrdPathTraceHistoryValid = true;
        m_nrdPathTraceInstanceNew = false;
        ++m_nrdPathTraceFrameIndex;
    }

    m_nrdPathTraceExecutedLastFrame = m_nrdPathTraceDispatchedThisFrame;
    m_nrdPathTraceDispatchedThisFrame = false;

    // NRD.RELAX.3: the same for the diagnostic RELAX instance.
    if(m_nrdRelaxDispatchedThisFrame)
    {
        m_nrdRelaxPreviousView = m_nrdRelaxPendingView;
        m_nrdRelaxPreviousProjection = m_nrdRelaxPendingProjection;
        m_nrdRelaxPreviousJitter[0] = m_nrdRelaxPendingJitter[0];
        m_nrdRelaxPreviousJitter[1] = m_nrdRelaxPendingJitter[1];
        m_nrdRelaxHistoryValid = true;
        m_nrdRelaxInstanceNew = false;
        ++m_nrdRelaxFrameIndex;
        ++m_nrdRelaxSubmittedDispatches;
    }

    m_nrdRelaxExecutedLastFrame = m_nrdRelaxDispatchedThisFrame;
    m_nrdRelaxDispatchedThisFrame = false;

    // NRD RELAX R.9B: this submitted frame's probe records become the next frame's previous records.
    if(m_nrdConfidenceDispatchedThisFrame)
    {
        m_nrdConfidenceCommittedIdentity = m_nrdConfidencePendingIdentity;
        std::memcpy(m_nrdConfidenceCommittedCamera, m_nrdConfidencePendingCamera, sizeof(m_nrdConfidenceCommittedCamera));
        ++m_nrdConfidenceFrameIndex;
    }

    m_nrdConfidenceExecutedLastFrame = m_nrdConfidenceDispatchedThisFrame;
    m_nrdConfidenceDispatchedThisFrame = false;
    // HYBRID.3: the complete pair commits as one observation after submission.
    if(m_nrdHybridDispatchedThisFrame)
    {
        m_nrdHybridPreviousView = m_nrdHybridPendingView;
        m_nrdHybridPreviousProjection = m_nrdHybridPendingProjection;
        m_nrdHybridPreviousJitter[0] = m_nrdHybridPendingJitter[0];
        m_nrdHybridPreviousJitter[1] = m_nrdHybridPendingJitter[1];
        m_nrdHybridHistoryValid = true;
        m_nrdHybridInstanceNew = false;
        ++m_nrdHybridFrameIndex;
        ++m_nrdHybridSubmittedDispatches;
    }

    m_nrdHybridExecutedLastFrame = m_nrdHybridDispatchedThisFrame;
    m_nrdHybridDispatchedThisFrame = false;
    m_rtxdiResources.CommitFrame();
    m_radianceCacheD6.CommitSubmission();

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

    // No DLSS work can be in flight after the drain; NGX must be released before the device.
    m_dlssBackend.Shutdown();
    m_dlssInputPreparationPass.Shutdown();

    Log::Info("Renderer shutdown: GPU finished.");
    ShutdownMaterialTables();

    m_materialPreviewRenderer.Shutdown();
    m_editorResourcesInitialized = false;
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
    m_editorGridPass.Shutdown();
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
    // Joined first: no compilation result may outlive the pass and library it was prepared for.
    if(m_shaderCompileWorker)
    {
        m_shaderCompileWorker->Shutdown();
        m_shaderCompileWorker.reset();
    }

    m_shaderCompileWorkerRunning = false;

    // ER.1 Phase 4: joined before the pass and the device go; untaken pipelines (never used by the GPU) are destroyed.
    if(m_pipelineWorker)
    {
        m_pipelineWorker->Shutdown();
        m_pipelineWorker.reset();
    }

    m_pipelineWorkerRunning = false;
    m_lightingModeTransition.Reset();
    m_preparedDxrLibraries.clear();
    m_denoiserPreparation.clear();
    m_sharcPreparation.clear();
    m_referenceRayCounts.Shutdown();
    m_pathTraceReferencePass.Shutdown();
    m_pathTraceTemporalPass.Shutdown();
    m_pathTraceSpatialPass.Shutdown();
    m_nrdPathTracePreparePass.Shutdown();
    m_nrdRelaxPreparePass.Shutdown();
    m_nrdHybridDenoiser.Shutdown();
    m_nrdHybridPreparePass.Shutdown();
    m_nrdRelaxDenoiser.Shutdown();
    m_nrdRelaxResolvedTexture.Shutdown();
    m_nrdRelaxConfidenceOne.Shutdown();
    m_nrdRelaxConfidenceLow.Shutdown();
    m_nrdConfidencePass.Shutdown();
    m_nrdPathTraceDenoiser.Shutdown();
    m_nrdPathTraceValidationTexture.Shutdown();
    // ER.1 Phase 3.1: everything above was released after the drain; nothing remains to retire.
    m_nrdRetention = {};
    m_nrdRetentionStatus = {};

    if(m_videoMemoryAdapter)
    {
        m_videoMemoryAdapter->Release();
        m_videoMemoryAdapter = nullptr;
    }

    m_videoMemoryAdapterQueried = false;
    m_pathTraceStatus = {};
    InvalidatePreviousGBufferHistory();
    m_previousGBufferHistory.Shutdown();
    m_previousGBufferHistoryFailed = false;
    m_previousGBufferReadSlot = 0;
    m_previousGBufferPrimary.reset();
    m_rtxdiResources.Shutdown();
    m_rtxdiResourcesFailed = false;
    m_rasterPrimaryStatus = {};
    m_restirDIStatus = {};
    m_materialTables.Shutdown();
    m_materialTableUnavailableReason.clear();
    m_pathTraceAccumulationKey.reset();
    m_pathTraceResetPending = true;
    m_sceneLightingEpoch.Restart();
    m_sceneLightingInputs.reset();
    // After the drain above: nothing can still use the cache or its retired resources.
    m_radianceCacheD6.Shutdown();
    m_radianceCacheD6Failed = false;
    m_radianceCacheConfiguration = {};
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
    m_presentationColor.Shutdown();
    m_presentationSelectionMask.Shutdown();
    m_presentationDepth.Shutdown();
    m_presentationResolvePass.Shutdown();
    m_overlayDepth.Shutdown();
    m_overlaySelectionMask.Shutdown();

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

    InvalidateSceneLighting(SceneLightingInvalidation::Geometry);
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
    InvalidateSceneLighting(SceneLightingInvalidation::Geometry);
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

    // The scene behind the image needs a coherent render extent too.
    if(m_sceneWidth == 0 || m_sceneHeight == 0 || m_outputWidth == 0 || m_outputHeight == 0 ||
       m_finalSceneColor.GetWidth() != m_outputWidth || m_finalSceneColor.GetHeight() != m_outputHeight)
    {
        return false;
    }

    image.srv = m_resourceDescriptorHeap.GetGPUHandle(m_finalSceneColor.GetSRVIndex());
    image.width = m_outputWidth;
    image.height = m_outputHeight;
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

    // Append so existing demo/default HDRI ordering and bootstrap selection are preserved.
    m_availableEnvironments.push_back({"None", {}, SceneDocument::NoneEnvironmentReference});
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

    if(m_availableEnvironments[index].reference == SceneDocument::NoneEnvironmentReference)
        return SetEnvironmentImage(nullptr,SceneDocument::NoneEnvironmentReference,"None",SceneDocument::NoneEnvironmentReference);

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

    InvalidateSceneLighting(SceneLightingInvalidation::Environment);
    ResetSceneHistory();

    m_environmentIdentity = "loose:" + asset.relativePath.generic_string();
    Log::Info("Environment changed to: " + asset.displayName);

    return true;
}

bool Renderer::SetEnvironmentImage(const HDRImageData* image,std::string identity,std::string displayName,std::string reference)
{
    if(identity==m_environmentIdentity) return true;
    if(m_activeRecordingGeneration || !m_commandContext.WaitForGPU() || !m_commandContext.DiscardCommandsAfterGPUDrain())
    { Log::Error("Environment replacement requires a confirmed GPU drain outside recording."); return false; }
    m_environment.Shutdown();
    HDRImageData black;
    if(!image && identity == SceneDocument::NoneEnvironmentReference)
    {
        black=MakeBlackEnvironmentImage();
        image=&black;
    }
    const bool loaded=image ? m_environment.Initialize(m_graphicsDevice.GetDevice(),*image,m_resourceUploader,m_resourceDescriptorHeap) :
        m_environment.Initialize(m_graphicsDevice.GetDevice(),std::filesystem::path{},m_resourceUploader,m_resourceDescriptorHeap);
    if(!loaded || !PreprocessEnvironment(m_environment)) { Log::Error("Required environment upload/conversion failed."); return false; }
    m_environmentIdentity=std::move(identity);
    auto entry=std::find_if(m_availableEnvironments.begin(),m_availableEnvironments.end(),[&](const auto& e){return e.relativePath.empty() && e.reference==reference && e.displayName==displayName;});
    if(entry==m_availableEnvironments.end()) { m_availableEnvironments.push_back({std::move(displayName),{},std::move(reference)}); m_currentEnvironmentIndex=m_availableEnvironments.size()-1; }
    else m_currentEnvironmentIndex=static_cast<size_t>(entry-m_availableEnvironments.begin());
    InvalidateSceneLighting(SceneLightingInvalidation::Environment);
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
