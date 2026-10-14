#include "Scene/MeshResidencyPublication.h"

#include "Renderer/Renderer.h"
#include "Scene/FileSceneAssetCatalog.h"
#include "Scene/FileSceneRuntime.h"

#include <exception>
#include <cassert>
#include <new>
#include <string_view>

MeshResidencyPublication::Readiness MeshResidencyPublication::Check(Renderer& renderer,
    const FileSceneRuntime& runtime, FileSceneAssetCatalog& catalog, const MeshResidencyToken& sceneToken,
    const RayTracingSceneResources::ReplacementCandidate& candidate, unsigned int frameSlot)
{
    return CheckPrepared(renderer, runtime, catalog, sceneToken, candidate, frameSlot, nullptr, nullptr);
}

MeshResidencyPublication::Readiness MeshResidencyPublication::CheckPrepared(Renderer& renderer,
    const FileSceneRuntime& runtime, FileSceneAssetCatalog& catalog, const MeshResidencyToken& sceneToken,
    const RayTracingSceneResources::ReplacementCandidate& candidate, unsigned int frameSlot,
    std::optional<MeshResidencySourceLock>* sourcePin,
    RayTracingSceneResources::AdoptionEvidence* adoptionEvidence)
{
    const auto result = [](Status status, std::string message)
    {
        Readiness readiness;
        readiness.status = status;
        readiness.message = std::move(message);
        return readiness;
    };

    if (!runtime.m_meshResidency || runtime.m_meshResidency->token != sceneToken ||
        sceneToken.runtimeIdentity != runtime.GetRuntimeIdentity() || !renderer.m_sceneResourceLifetime ||
        renderer.m_sceneResourceScene != &runtime.GetScene())
    {
        return result(Status::Stale, "Scene preparation token or active renderer scene session changed.");
    }

    if (frameSlot >= CommandContext::FrameCount)
    {
        return result(Status::Unsupported, "Publication readiness requires a valid renderer frame slot.");
    }

    if (renderer.m_commandSubmissionAttempted || renderer.m_resizeFailed ||
        renderer.m_sceneInitializationAttempt.failed)
    {
        return result(Status::Failed, "Renderer submission or resource state requires checked recovery.");
    }

    if (runtime.HasPreparedScene() || runtime.HasMaterialAssignment() || renderer.HasUnresolvedDisplacementUpdate() ||
        renderer.m_sceneInitializationAttempt.candidate || renderer.m_displacementExitScopeGeneration ||
        renderer.m_commandRecordingGeneration || renderer.m_activeRecordingGeneration)
    {
        return result(Status::Retry, "Finish the outstanding scene transaction or command recording before publication.");
    }

    if (renderer.m_geometryUpdateFailed || !renderer.m_materialPublication.revision)
    {
        return result(Status::Failed, "Renderer geometry state requires checked recovery.");
    }

    auto& resources = renderer.m_rayTracingScene;

    if (!resources.m_membership || resources.m_membership->submissionUncertain)
    {
        return result(Status::Failed, "Active membership submission certainty is unavailable.");
    }

    if (!resources.CanRetireActiveMembership())
    {
        return result(Status::Retry, "Active membership is still recorded or the snapshot retirement queue is full.");
    }

    std::string error;

    try
    {
        // Reject a superseded scene binding even while GPU preparation is pending.
        // This identity check acquires no source lock and performs no GPU work.
        if (!runtime.ValidatePreparedMeshResidencyIdentity(sceneToken, catalog, &error))
        {
            return result(Status::Stale, std::move(error));
        }

        const auto status = renderer.PollDXRMembershipCandidate(candidate, runtime, catalog, error);

        if (status == RayTracingSceneResources::CandidateStatus::Pending)
        {
            return result(Status::Retry, "Await candidate GPU completion before publication validation.");
        }

        if (status == RayTracingSceneResources::CandidateStatus::Stale)
        {
            return result(Status::Stale, std::move(error));
        }

        if (status == RayTracingSceneResources::CandidateStatus::Failed)
        {
            return result(Status::Failed, std::move(error));
        }

        if (status == RayTracingSceneResources::CandidateStatus::NeedsTransformRefresh)
        {
            // Poll has released every source lock. Never carry one across this
            // submission or the frames spent waiting for its completion.
            if (renderer.RefreshDXRMembershipCandidateTransforms(candidate, runtime, catalog, frameSlot, error))
            {
                return result(Status::Retry, "Candidate transforms refreshed; await its new GPU completion fence.");
            }

            const auto refreshed = renderer.PollDXRMembershipCandidate(candidate, runtime, catalog, error);

            if (refreshed == RayTracingSceneResources::CandidateStatus::Stale)
            {
                return result(Status::Stale, std::move(error));
            }

            if (renderer.m_commandSubmissionAttempted || refreshed == RayTracingSceneResources::CandidateStatus::Failed)
            {
                return result(Status::Failed, std::move(error));
            }

            return result(Status::Retry, "Deferred: candidate transform refresh is not ready.");
        }

        // All GPU work is complete. Validation may allocate/hash, but cannot
        // load material resources or submit work. This pin dies on every exit.
        auto source = runtime.ValidatePreparedMeshResidency(sceneToken, catalog, &error);

        if (!source)
        {
            return result(Status::Stale, std::move(error));
        }

        const auto& prepared = *runtime.m_meshResidency;
        RayTracingSceneResources::AdoptionEvidence evidence;
        const auto adoption = resources.PrepareCandidateAdoption(candidate, prepared.selection.objectID,
            prepared.selection.runtimeIndex, prepared.candidate, prepared.fallback, prepared.runtimeSlots,
            renderer.m_materialPublication.desired.size(), evidence, error);

        if (adoption == RayTracingSceneResources::AdoptionReadiness::Unsupported)
        {
            return result(Status::Unsupported, std::move(error));
        }

        if (adoption != RayTracingSceneResources::AdoptionReadiness::Ready)
        {
            return result(Status::Stale, std::move(error));
        }

        const auto& layout = prepared.candidate.mesh->GetVertexLayout();

        if (renderer.m_selectionMaskPass.IsInitialized())
        {
            if (!renderer.m_selectionMaskPass.IsCompatible(layout))
            {
                return result(Status::Unsupported, "Arriving mesh is incompatible with the active selection-mask pipeline.");
            }
        }
        else if (!renderer.m_selectionMaskCandidate ||
            renderer.m_selectionMaskCandidateLifetime != renderer.m_sceneResourceLifetime ||
            !renderer.m_selectionMaskCandidate->IsCompatible(layout))
        {
            return result(Status::Retry, "Prepare a compatible selection-mask pipeline for this renderer lifetime.");
        }

        // Material parameter/texture revisions preserve the index layout and
        // are compatible when current binding epochs, pointers, GPU indices and
        // table cardinality still match. First use must upload the latest desired
        // material revision through the existing per-frame material-table path.
        Readiness readiness;
        readiness.status = Status::Ready;
        readiness.runtimeIdentity = runtime.GetRuntimeIdentity();
        readiness.rendererLifetime = renderer.m_sceneResourceLifetime;

        readiness.materialRevision = renderer.m_materialPublication.revision;
        readiness.candidateFence = evidence.fence;
        readiness.fenceLifetime = evidence.fenceLifetime;

        if (sourcePin)
        {
            sourcePin->emplace(std::move(source));
        }

        if (adoptionEvidence)
        {
            *adoptionEvidence = evidence;
        }

        return readiness;
    }
    catch (const std::bad_alloc&)
    {
        return result(Status::Retry, "Deferred: publication storage reservation failed; active bindings are unchanged.");
    }
    catch (const std::exception& exception)
    {
        return result(Status::Stale, "Publication readiness validation failed: " + std::string(exception.what()));
    }
}

bool MeshResidencyPublication::CanPrepareGeometry(const Renderer& renderer, const FileSceneRuntime& runtime) noexcept
{
    return renderer.m_sceneResourceLifetime && renderer.m_sceneResourceScene == &runtime.GetScene() &&
        !renderer.m_sceneInitializationAttempt.candidate && !renderer.m_sceneInitializationAttempt.failed &&
        !renderer.m_commandRecordingGeneration && !renderer.m_activeRecordingGeneration &&
        !renderer.m_commandSubmissionAttempted && !renderer.m_resizeFailed && !renderer.m_geometryUpdateFailed &&
        !renderer.m_displacementExitScopeGeneration && !renderer.HasUnresolvedDisplacementUpdate() &&
        !runtime.HasPreparedScene() && !runtime.HasMaterialAssignment();
}

bool MeshResidencyPublication::PrepareSelectionMask(Renderer& renderer, const FileSceneRuntime& runtime,
    FileSceneAssetCatalog& catalog, const MeshResidencyToken& sceneToken, std::string& error)
{
    error.clear();

    if (!CanPrepareGeometry(renderer, runtime))
    {
        error = "Selection-mask preparation requires the active scene and an idle, valid renderer boundary.";
        return false;
    }

    try
    {
        auto source = runtime.ValidatePreparedMeshResidency(sceneToken, catalog, &error);

        if (!source)
        {
            return false;
        }

        const auto& layout = runtime.m_meshResidency->candidate.mesh->GetVertexLayout();
        bool position = false;

        for (const auto& attribute : layout.GetAttributes())
        {
            const auto format = VertexLayout::GetDXGIFormat(attribute.type);
            const unsigned int size = attribute.type == VertexAttributeType::Float ? 4 :
                attribute.type == VertexAttributeType::Float2 ? 8 :
                attribute.type == VertexAttributeType::Float3 ? 12 : 16;

            if (!attribute.semantic || format == DXGI_FORMAT_UNKNOWN || attribute.offset > layout.GetStride() ||
                size > layout.GetStride() - attribute.offset)
            {
                error = "Invalid selection-mask vertex layout.";
                return false;
            }

            if (std::string_view(attribute.semantic) == "POSITION" && attribute.semanticIndex == 0 &&
                attribute.type == VertexAttributeType::Float3)
            {
                position = true;
            }
        }

        if (!position)
        {
            error = "Selection-mask layout requires float3 POSITION0.";
            return false;
        }

        if (renderer.m_selectionMaskPass.IsInitialized())
        {
            if (!renderer.m_selectionMaskPass.IsCompatible(layout))
            {
                error = "Arriving mesh is incompatible with the active selection-mask pipeline.";
                return false;
            }

            return true;
        }

        if (renderer.m_selectionMaskCandidateLifetime == renderer.m_sceneResourceLifetime &&
            renderer.m_selectionMaskCandidate && renderer.m_selectionMaskCandidate->IsCompatible(layout))
        {
            return true;
        }

        auto candidate = std::make_unique<Renderer::SelectionMaskPipelineCandidate>();

        if (!candidate->Initialize(renderer.m_graphicsDevice.GetDevice(), renderer.m_shaderLibrary, layout))
        {
            error = "Selection-mask pipeline preparation failed; active renderer is unchanged.";
            return false;
        }

        renderer.m_selectionMaskCandidate.swap(candidate);
        renderer.m_selectionMaskCandidateLifetime = renderer.m_sceneResourceLifetime;
        return true;
    }
    catch (const std::exception& exception)
    {
        error = "Selection-mask preparation failed: " + std::string(exception.what());
        return false;
    }
}

void MeshResidencyPublication::CancelSelectionMask(Renderer& renderer) noexcept
{
    renderer.m_selectionMaskCandidate.reset();
    renderer.m_selectionMaskCandidateLifetime = 0;
}

bool MeshResidencyPublication::Prepare(Renderer& renderer, const FileSceneRuntime& runtime,
    FileSceneAssetCatalog& catalog, const MeshResidencyToken& sceneToken, std::string& error)
{
    error.clear();

    if (std::this_thread::get_id() != m_thread || m_candidate)
    {
        error = "Publication preparation requires the coordinator thread and no outstanding candidate.";
        return false;
    }

    if (!PrepareSelectionMask(renderer, runtime, catalog, sceneToken, error))
    {
        return false;
    }

    try
    {
        std::vector<RayTracingSceneResources::ResidencyProposal> proposals;

        {
            auto source = runtime.ValidatePreparedMeshResidency(sceneToken, catalog, &error);

            if (!source)
            {
                return false;
            }

            const auto& prepared = *runtime.m_meshResidency;
            proposals.push_back({prepared.selection.objectID, prepared.candidate});
        } // Release source pin before command recording/submission.

        auto candidate = renderer.PrepareDXRMembershipCandidate(runtime, catalog, proposals,
            renderer.m_swapChain.GetCurrentBackBufferIndex(), error);

        if (!candidate)
        {
            return false;
        }

        m_candidate = std::move(candidate);
        m_sceneToken = sceneToken;
        m_rendererLifetime = renderer.m_sceneResourceLifetime;
        return true;
    }
    catch (const std::exception& exception)
    {
        error = "Publication coordinator preparation failed: " + std::string(exception.what());
        return false;
    }
}

MeshResidencyPublication::AcceptanceResult MeshResidencyPublication::PublishAtBoundary(
    Renderer& renderer, FileSceneRuntime& runtime, FileSceneAssetCatalog& catalog)
{
    if (std::this_thread::get_id() != m_thread)
    {
        return {Status::Failed, "Final acceptance requires the coordinator thread."};
    }

    if (!m_candidate)
    {
        return {Status::Retry, "No coordinated mesh residency candidate is prepared."};
    }

    if (runtime.GetRuntimeIdentity() != m_sceneToken.runtimeIdentity ||
        renderer.m_sceneResourceLifetime != m_rendererLifetime ||
        renderer.m_sceneResourceScene != &runtime.GetScene())
    {
        Cancel();
        return {Status::Stale, "Coordinated candidate belongs to an obsolete scene or renderer lifetime."};
    }

    // Check repeats identity, GPU provenance/completion, every effective binding,
    // current transforms, selection layout and ownership/retirement prerequisites.
    // Transform refresh may submit candidate-only work and necessarily returns Retry.
    std::optional<MeshResidencySourceLock> sourcePin;
    RayTracingSceneResources::AdoptionEvidence evidence;
    auto readiness = CheckPrepared(renderer, runtime, catalog, m_sceneToken, *m_candidate,
        renderer.m_swapChain.GetCurrentBackBufferIndex(), &sourcePin, &evidence);
    AcceptanceResult result{readiness.status, std::move(readiness.message)};

    if (result.status == Status::Stale || result.status == Status::Unsupported || result.status == Status::Failed)
    {
        Cancel();
    }

    if (result.status != Status::Ready)
    {
        return result;
    }

    // Everything fallible is complete. Keep the source pinned across the entire
    // noexcept commit. Even the result string has already been constructed.
    assert(sourcePin && static_cast<bool>(*sourcePin));
    result.published = true;
    ApplyPreparedPublication(renderer, runtime, evidence);
    return result;
}

void MeshResidencyPublication::Cancel() noexcept
{
    // Dropping the handle never frees recorded/in-flight candidate resources:
    // RayTracingSceneResources retains its manager pin until checked collection.
    m_candidate.reset();
    m_sceneToken = {};
    m_rendererLifetime = 0;
}

void MeshResidencyPublication::ApplyPreparedPublication(Renderer& renderer, FileSceneRuntime& runtime,
    const RayTracingSceneResources::AdoptionEvidence& evidence) noexcept
{
    // Exclusive main-thread pre-recording boundary. No callbacks, validation,
    // allocation, GPU submission/wait or recoverable failure after this point.
    assert(std::this_thread::get_id() == m_thread);
    assert(!renderer.m_commandRecordingGeneration && !renderer.m_activeRecordingGeneration);
    renderer.m_rayTracingScene.AdoptPreparedCandidate(*m_candidate, evidence);

    if (!renderer.m_selectionMaskPass.IsInitialized())
    {
        renderer.AdoptSelectionMaskCandidate();
    }

    runtime.ApplyPreparedMeshResidency();

    // The prepared pass is either empty after adoption or was never recorded.
    renderer.m_selectionMaskCandidate.reset();
    renderer.m_selectionMaskCandidateLifetime = 0;
    Cancel();
}
