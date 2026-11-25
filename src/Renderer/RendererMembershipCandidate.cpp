#include "Renderer.h"
#include "Renderer/TextureMemoryAllocator.h"
#include "Scene/FileSceneRuntime.h"
#include "Scene/FileSceneAssetCatalog.h"
#include "Debug/StreamingTelemetry.h"

#include <exception>

void Renderer::CollectBLASMemory()
{
    if (!m_commandContextInitialized)
    {
        return;
    }

    m_blasMemoryPool.Collect(m_commandContext.GetSubmissionLifetime(), m_commandContext.GetLastSubmissionFenceValue(),
        m_commandContext.GetCompletedFenceValue());
}

void Renderer::CollectTextureMemory()
{
    if (!m_commandContextInitialized)
    {
        return;
    }

    TextureMemoryAllocator::Collect(m_commandContext.GetSubmissionLifetime(),
        m_commandContext.GetLastSubmissionFenceValue(), m_commandContext.GetCompletedFenceValue());
}

size_t Renderer::ReclaimIdleEditorDescriptors()
{
    // Never during recording: recorded but unsubmitted work may still reference an SRV.
    if (m_commandRecordingGeneration || m_activeRecordingGeneration)
    {
        return 0;
    }

    // Refresh both caches' view of completed GPU work so retirement checks use the latest fence.
    const auto completed = m_commandContext.GetCompletedFenceValue();
    m_textureThumbnails.SetCompletedFence(completed);
    m_materialPreviewRenderer.PollMeshThumbnailGPUCompletion(completed);

    const size_t textureThumbnails = m_textureThumbnails.ReleaseIdleDescriptors();
    const size_t meshThumbnails = m_materialPreviewRenderer.ReleaseIdleMeshThumbnailImages();
    return textureThumbnails + meshThumbnails;
}

std::optional<RayTracingSceneResources::ReplacementCandidate> Renderer::PrepareDXRMembershipCandidate(
    const FileSceneRuntime& runtime, FileSceneAssetCatalog& catalog,
    const std::vector<RayTracingSceneResources::ResidencyProposal>& proposals,
    unsigned int frameSlot, std::string& error, const FileSceneRuntime* membershipSource,
    RayTracingSceneResources::CandidateFailure* failure)
{
    // S0.0 diagnostics: the whole synchronous preparation (slot wait, allocation, uploads, recording, submission).
    const StreamingTelemetry::Scope prepareTiming(StreamingTelemetry::Metric::CandidatePrepareNs,
        StreamingTelemetry::Metric::CandidatePrepares);
    StreamingTelemetry::Add(StreamingTelemetry::Metric::CandidateProposals, proposals.size());
    error.clear();

    // Deferrals and rejections below are ordinary failures; only the candidate itself
    // classifies descriptor exhaustion.
    if (failure)
    {
        *failure = RayTracingSceneResources::CandidateFailure::Other;
    }

    if (m_sceneResourceScene != &(membershipSource ? membershipSource->GetScene() : runtime.GetScene()) || !m_sceneResourceLifetime ||
        m_sceneInitializationAttempt.candidate || m_commandRecordingGeneration || m_resizeFailed ||
        m_geometryUpdateFailed || m_displacementExitScopeGeneration || HasUnresolvedDisplacementUpdate() ||
        runtime.HasPreparedScene() || runtime.HasMaterialAssignment() || frameSlot >= CommandContext::FrameCount)
    {
        error = "Candidate preparation requires the active file scene and no conflicting transaction or recording.";
        return std::nullopt;
    }

    // The frame would wait on this slot fence in Begin moments later. Waiting here
    // instead of deferring keeps GPU-bound frames from never observing it complete.
    bool slotCompleted = false;

    {
        const StreamingTelemetry::Scope slotWaitTiming(StreamingTelemetry::Metric::CandidateSlotWaitNs);
        slotCompleted = m_commandContext.WaitForFrameFence(frameSlot);
    }

    if (!slotCompleted)
    {
        error = "Candidate allocator fence wait failed; renderer requires checked recovery.";
        return std::nullopt;
    }

    m_rayTracingScene.CollectReplacementCandidate(m_commandContext);

    try
    {
        std::optional<StreamingTelemetry::Scope> detailTiming(std::in_place, StreamingTelemetry::Metric::CandidateBuildNs);
        auto candidate = m_rayTracingScene.PrepareReplacementCandidate(m_graphicsDevice.GetRayTracingDevice(),
            m_resourceDescriptorHeap, runtime, catalog, proposals, m_materialPublication.desired.size(), error, membershipSource,
            failure);
        detailTiming.emplace(StreamingTelemetry::Metric::CandidateRecordNs);

        if (!candidate)
        {
            m_rayTracingScene.CollectReplacementCandidate(m_commandContext);
            return std::nullopt;
        }

        candidate->m_rendererLifetime = m_sceneResourceLifetime;

        if (!BeginTrackedRecording(frameSlot) ||
            !m_rayTracingScene.RecordReplacementCandidate(m_graphicsDevice.GetRayTracingDevice(),
                m_commandContext.GetRayTracingCommandList(), m_commandRecordingGeneration, error) ||
            !EndTrackedRecording())
        {
            if (m_commandRecordingGeneration && !m_commandSubmissionAttempted)
            {
                AbortTrackedRecording(); // Failed discard retains the candidate manager pin.
            }

            if (error.empty())
            {
                error = "Candidate command recording failed; resources remain retained until safe recovery.";
            }

            return std::nullopt;
        }

        if (!ExecuteTrackedRecording(frameSlot))
        {
            error = "Candidate submission failed or is uncertain; retaining resources for checked recovery.";
            return std::nullopt;
        }

        return candidate;
    }
    catch (const std::exception& exception)
    {
        if (m_commandRecordingGeneration && !m_commandSubmissionAttempted)
        {
            AbortTrackedRecording();
        }

        error = "Candidate preparation failed: " + std::string(exception.what());
        m_rayTracingScene.CollectReplacementCandidate(m_commandContext);
        return std::nullopt;
    }
}

RayTracingSceneResources::CandidateStatus Renderer::PollDXRMembershipCandidate(
    const RayTracingSceneResources::ReplacementCandidate& candidate, const FileSceneRuntime& runtime,
    FileSceneAssetCatalog& catalog, std::string& error) const
{
    error.clear();

    if (!candidate.m_rendererLifetime || candidate.m_rendererLifetime != m_sceneResourceLifetime ||
        m_sceneResourceScene != &runtime.GetScene())
    {
        error = "Candidate belongs to an obsolete renderer scene-resource lifetime.";
        return RayTracingSceneResources::CandidateStatus::Stale;
    }

    try
    {
        return m_rayTracingScene.PollReplacementCandidate(candidate, m_commandContext, runtime, catalog, error);
    }
    catch (const std::exception& exception)
    {
        error = "Candidate validation failed: " + std::string(exception.what());
        return RayTracingSceneResources::CandidateStatus::Stale;
    }
}

bool Renderer::RefreshDXRMembershipCandidateTransforms(const RayTracingSceneResources::ReplacementCandidate& candidate,
    const FileSceneRuntime& runtime, FileSceneAssetCatalog& catalog, unsigned int frameSlot, std::string& error)
{
    if (m_commandRecordingGeneration || m_sceneInitializationAttempt.candidate || m_resizeFailed ||
        m_geometryUpdateFailed || m_displacementExitScopeGeneration || HasUnresolvedDisplacementUpdate() ||
        runtime.HasPreparedScene() || runtime.HasMaterialAssignment() || frameSlot >= CommandContext::FrameCount)
    {
        error = "Candidate transform refresh requires no conflicting transaction or recording.";
        return false;
    }

    StreamingTelemetry::Add(StreamingTelemetry::Metric::CandidateTransformRefreshes, 1);

    const auto status = PollDXRMembershipCandidate(candidate, runtime, catalog, error);

    if (status != RayTracingSceneResources::CandidateStatus::Ready &&
        status != RayTracingSceneResources::CandidateStatus::NeedsTransformRefresh)
    {
        return false;
    }

    if (!m_commandContext.WaitForFrameFence(frameSlot))
    {
        error = "Candidate transform allocator fence wait failed; renderer requires checked recovery.";
        return false;
    }

    try
    {
        if (!BeginTrackedRecording(frameSlot) ||
            !m_rayTracingScene.RefreshReplacementTransforms(candidate, m_commandContext.GetRayTracingCommandList(),
                frameSlot, m_commandRecordingGeneration, runtime, error) || !EndTrackedRecording())
        {
            if (m_commandRecordingGeneration && !m_commandSubmissionAttempted)
            {
                AbortTrackedRecording();
            }

            if (error.empty())
            {
                error = "Candidate transform recording failed; retaining resources until safe recovery.";
            }

            return false;
        }

        if (!ExecuteTrackedRecording(frameSlot))
        {
            error = "Candidate transform submission failed or is uncertain; retaining resources.";
            return false;
        }

        return true;
    }
    catch (const std::exception& exception)
    {
        if (m_commandRecordingGeneration && !m_commandSubmissionAttempted)
        {
            AbortTrackedRecording();
        }

        error = "Candidate transform refresh failed: " + std::string(exception.what());
        return false;
    }
}
