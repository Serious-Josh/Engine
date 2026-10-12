#include "Renderer/Displacement/DisplacementUpdateCoordinator.h"

#include "Renderer/Displacement/DisplacementUpdateState.h"
#include "Renderer/RenderObject.h"
#include "Renderer/RayTracing/RayTracingSceneResources.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <climits>

bool RendererDisplacement::HasUnresolvedState(
    const PendingDisplacementUpdate& pending,
    const DisplacementSubmissionCoverage& coverage)
{
    // Completed/discarded records must also be explicitly finalized and reset.
    return coverage.enabled || pending.status != DisplacementUpdateStatus::Empty ||
           pending.submission.fenceValue != 0 || pending.submission.lifetime != 0 ||
           pending.submission.retentionID != 0 || pending.reservation.lifetime != 0 ||
           pending.reservation.reservationID != 0 ||
           pending.reservationOwnership != DisplacementReservationOwnership::None;
}

bool RendererDisplacement::ValidateIdentity(
    const PendingDisplacementUpdate& pending,
    const ValidationContext& context)
{
    if (pending.status == DisplacementUpdateStatus::Empty || context.sceneResourceLifetime == 0 ||
        pending.sceneResourceLifetime != context.sceneResourceLifetime || !context.scene)
    {
        return false;
    }

    if (pending.scene != context.scene || !pending.mesh)
    {
        return false;
    }

    // Engine keeps the caller-owned scene alive until release succeeds. Match
    // the guarded lifetime before dereferencing the current scene, never Mesh.

    if (pending.sceneRevision != context.scene->GetStructureRevision())
    {
        return false;
    }

    const auto& objects = context.scene->GetRenderObjects();

    return std::any_of(objects.begin(), objects.end(),
                       [&pending](const RenderObject& object) { return object.mesh == pending.mesh; });
}

bool RendererDisplacement::ValidateReservation(
    const PendingDisplacementUpdate& pending,
    const ValidationContext& context)
{
    if (pending.status == DisplacementUpdateStatus::Prepared)
    {
        return context.recordingGeneration == 0 && pending.recordingGeneration == 0 && pending.frameSlot == UINT_MAX &&
               !context.geometryUpdateFailed && pending.requestRevision != 0 &&
               pending.reservationOwnership == DisplacementReservationOwnership::Reserved &&
               pending.reservation.lifetime != 0 && pending.reservation.reservationID != 0 &&
               pending.submission.fenceValue == 0 && pending.submission.lifetime == 0 &&
               pending.submission.retentionID == 0 && ValidateIdentity(pending, context);
    }

    // Local exclusive ownership, not a new CommandContext provenance query.
    // Only acquisition/cancellation/result transitions below may change it.

    if (pending.status != DisplacementUpdateStatus::Recording ||

        pending.reservationOwnership != DisplacementReservationOwnership::Reserved)
    {
        return false;
    }

    if (pending.reservation.lifetime == 0 || pending.reservation.reservationID == 0)
    {
        return false;
    }

    if (pending.submission.fenceValue != 0 || pending.submission.lifetime != 0 || pending.submission.retentionID != 0)
    {
        return false;
    }

    if (pending.requestRevision == 0 || pending.frameSlot >= context.frameSlotCount || !context.geometryUpdateFailed)
    {
        return false;
    }

    return ValidateIdentity(pending, context);
}

bool RendererDisplacement::ValidateRecording(

    const PendingDisplacementUpdate& pending,
    const ValidationContext& context,
    unsigned int activeFrameSlot,
    unsigned int requestedFrameSlot)
{
    if (pending.status != DisplacementUpdateStatus::Recording || !context.geometryUpdateFailed)
    {
        return false;
    }

    if (requestedFrameSlot >= context.frameSlotCount || pending.frameSlot != requestedFrameSlot ||

        activeFrameSlot != requestedFrameSlot)
    {
        return false;
    }

    if (context.recordingGeneration == 0 || pending.recordingGeneration != context.recordingGeneration)
    {
        return false;
    }

    if (pending.submission.fenceValue != 0 || pending.submission.lifetime != 0 || pending.submission.retentionID != 0 ||

        pending.requestRevision == 0)
    {
        return false;
    }

    if (pending.reservationOwnership == DisplacementReservationOwnership::None)
    {
        if (pending.reservation.lifetime != 0 || pending.reservation.reservationID != 0)
        {
            return false;
        }
    }
    else if (!ValidateReservation(pending, context))
    {
        return false;
    }

    return ValidateIdentity(pending, context);
}

bool RendererDisplacement::BeginCoverage(
    const PendingDisplacementUpdate& pending,
    DisplacementSubmissionCoverage& coverage,
    const ValidationContext& context,
    unsigned int activeFrameSlot)
{
    if (coverage.enabled || !ValidateRecording(pending, context, activeFrameSlot, pending.frameSlot) ||

        pending.recordingStage != DisplacementRecordingStage::None)
    {
        return false;
    }

    coverage.enabled = true;
    coverage.sceneLifetime = pending.sceneResourceLifetime;
    coverage.requestRevision = pending.requestRevision;

    coverage.mesh = pending.mesh;
    coverage.targetCounts = pending.targetCounts;
    coverage.current.generation = context.recordingGeneration;
    coverage.current.frameSlot = activeFrameSlot;

    coverage.current.outcome = DisplacementRecordingOutcome::Active;
    return true;
}

bool RendererDisplacement::ValidateFramePreparation(
    const FrameDisplacementPreparation& preparation,
    std::uint64_t recordingGeneration,
    unsigned int activeFrameSlot,
    unsigned int frameSlot,
    unsigned int frameSlotCount)
{
    if (frameSlot >= frameSlotCount || activeFrameSlot != frameSlot || preparation.slot != frameSlot)
    {
        return false;
    }

    if (recordingGeneration == 0 || preparation.generation != recordingGeneration)
    {
        return false;
    }

    return preparation.slotAcquired && preparation.uploadResetCount == 1 && preparation.diagnosticConsistent;
}

bool RendererDisplacement::AdvanceRecording(

    PendingDisplacementUpdate& pending,
    FrameDisplacementPreparation& preparation,
    const ValidationContext& context,
    unsigned int activeFrameSlot,
    unsigned int frameSlot,
    DisplacementRecordingStage stage,
    bool recordingSucceeded,
    const RayTracingSceneResources& rayTracingScene)
{
    DisplacementRecordingStage requiredPreviousStage;

    switch (stage)
    {
    case DisplacementRecordingStage::Compute:
        requiredPreviousStage = DisplacementRecordingStage::None;
        break;

    case DisplacementRecordingStage::BLAS:
        requiredPreviousStage = DisplacementRecordingStage::Compute;
        break;

    case DisplacementRecordingStage::MergedTLAS:
        requiredPreviousStage = DisplacementRecordingStage::BLAS;
        break;

    default:
        return false;
    }

    if (!recordingSucceeded || pending.recordingStage != requiredPreviousStage ||
        !ValidateRecording(pending, context, activeFrameSlot, frameSlot))
    {
        return false;
    }

    if (!ValidateFramePreparation(preparation, context.recordingGeneration, activeFrameSlot, frameSlot,
                                  context.frameSlotCount) ||

        preparation.mergedTLASRecorded)
    {
        return false;
    }

    if (pending.historyPreparation != DisplacementHistoryPreparation::Performed ||
        pending.tlasPreparation[frameSlot] != DisplacementTLASPreparation::Requested)
    {
        return false;
    }

    if (stage == DisplacementRecordingStage::MergedTLAS &&
        (pending.tlasUses[frameSlot].consumerGeneration != context.recordingGeneration ||

         !rayTracingScene.ValidateTLASGeometryEvidence(pending.mesh, pending.tlasUses[frameSlot].evidence)))
    {
        return false;
    }

    pending.recordingStage = stage;

    if (stage == DisplacementRecordingStage::MergedTLAS)
    {
        pending.tlasPreparation[frameSlot] = DisplacementTLASPreparation::Recorded;
        preparation.mergedTLASRecorded = true;
    }

    return true;
}

bool RendererDisplacement::ActivateRecording(
    PendingDisplacementUpdate& pending,
    DisplacementSubmissionCoverage& coverage,
    const ValidationContext& context,
    unsigned int activeFrameSlot,
    unsigned int requestedFrameSlot,
    bool& geometryUpdateFailed)
{
    pending.frameSlot = requestedFrameSlot;
    pending.recordingGeneration = context.recordingGeneration;
    pending.status = DisplacementUpdateStatus::Recording;
    geometryUpdateFailed = true;

    ValidationContext recordingContext = context;
    recordingContext.geometryUpdateFailed = geometryUpdateFailed;

    if (!BeginCoverage(pending, coverage, recordingContext, activeFrameSlot))
    {
        // No GPU commands or AS mutations occurred. Keep the reserved request
        // owned for explicit resolution; never silently drop token capacity.
        pending.frameSlot = UINT_MAX;
        pending.recordingGeneration = 0;
        pending.status = DisplacementUpdateStatus::Prepared;
        geometryUpdateFailed = false;

        return false;
    }

    return true;
}

bool RendererDisplacement::ValidateCoverage(
    const PendingDisplacementUpdate& pending,
    const DisplacementSubmissionCoverage& coverage,
    const ValidationContext& context)
{
    if (!coverage.enabled || coverage.failed || coverage.resolving || !context.geometryUpdateFailed)
    {
        return false;
    }

    if (!ValidateIdentity(pending, context))
    {
        return false;
    }

    if (coverage.sceneLifetime != pending.sceneResourceLifetime ||
        coverage.requestRevision != pending.requestRevision || coverage.mesh != pending.mesh ||

        coverage.submittedCount > coverage.submitted.size())
    {
        return false;
    }

    if (coverage.targetCounts.vertexCount != pending.targetCounts.vertexCount ||
        coverage.targetCounts.indexCount != pending.targetCounts.indexCount)
    {
        return false;
    }

    return pending.status == DisplacementUpdateStatus::Recording ||

           pending.status == DisplacementUpdateStatus::Submitting ||
           pending.status == DisplacementUpdateStatus::Submitted;
}

bool RendererDisplacement::ValidateHistoricalRetirementCoverage(

    const DisplacementSubmissionCoverage& coverage,
    unsigned int frameSlotCount)
{
    const auto& original = coverage.original;
    std::uint64_t previousFence = 0;
    std::uint64_t previousGeneration = 0;

    for (size_t index = 0; index < coverage.submittedCount; ++index)
    {
        const auto& receipt = coverage.submitted[index];
        const auto& geometry = receipt.geometry;

        if (receipt.outcome != DisplacementRecordingOutcome::Submitted || receipt.frameSlot >= frameSlotCount ||
            receipt.generation <= previousGeneration || receipt.submission.fenceValue <= previousFence)
        {
            return false;
        }

        if (receipt.submission.fenceValue == UINT64_MAX || receipt.submission.retentionID == 0 ||
            receipt.ownsReservation || receipt.reservation.lifetime != receipt.submission.lifetime)
        {
            return false;
        }

        if (receipt.reservation.reservationID != receipt.submission.retentionID || geometry.tlasRecording == 0 ||

            geometry.recordingGeneration == 0 || geometry.recordingGeneration > receipt.generation)
        {
            return false;
        }

        if (geometry.frameSlot >= frameSlotCount || receipt.submission.lifetime != original.submission.lifetime ||

            geometry.geometryVersion != coverage.requestRevision ||
            geometry.blasAddress != original.geometry.blasAddress)
        {
            return false;
        }

        if (geometry.retirement.sceneLifetime != original.geometry.retirement.sceneLifetime ||
            geometry.retirement.blas.ownerLifetime != original.geometry.retirement.blas.ownerLifetime ||
            geometry.retirement.blas.batchID != original.geometry.retirement.blas.batchID)
        {
            return false;
        }

        // Receipts were checked before their exact submission. Later TLAS
        // recordings and released borrowed tokens cannot rewrite those facts.
        previousFence = receipt.submission.fenceValue;
        previousGeneration = receipt.generation;
    }

    return true;
}

bool RendererDisplacement::ValidateCoverageResolutionRecords(

    const PendingDisplacementUpdate& pending,
    const DisplacementSubmissionCoverage& coverage)
{
    // Require a closed coverage frontier, not just a completed original fence.
    // An Active/Recorded/uncertain exit must be resolved before retirement.

    const auto& latest = coverage.submitted[coverage.submittedCount - 1];
    const auto& original = coverage.original;
    const auto& first = coverage.submitted[0];

    if (coverage.current.generation != latest.generation || coverage.current.frameSlot != latest.frameSlot ||
        coverage.current.geometry.geometryVersion != latest.geometry.geometryVersion ||
        coverage.current.geometry.recordingGeneration != latest.geometry.recordingGeneration)
    {
        return false;
    }

    if (coverage.current.geometry.frameSlot != latest.geometry.frameSlot ||
        coverage.current.geometry.tlasRecording != latest.geometry.tlasRecording ||
        coverage.current.submission.fenceValue != latest.submission.fenceValue ||
        coverage.current.submission.lifetime != latest.submission.lifetime)
    {
        return false;
    }

    if (coverage.current.submission.retentionID != latest.submission.retentionID)
    {
        return false;
    }

    if (first.generation != original.generation || first.submission.fenceValue != original.submission.fenceValue ||
        first.submission.lifetime != original.submission.lifetime ||

        first.submission.retentionID != original.submission.retentionID)
    {
        return false;
    }

    if (first.geometry.tlasRecording != original.geometry.tlasRecording ||

        original.geometry.recordingGeneration != pending.recordingGeneration ||
        original.geometry.frameSlot != pending.frameSlot)
    {
        return false;
    }

    if (pending.recordingStage != DisplacementRecordingStage::MergedTLAS ||

        pending.historyPreparation != DisplacementHistoryPreparation::Performed ||

        pending.tlasPreparation[pending.frameSlot] != DisplacementTLASPreparation::Recorded)
    {
        return false;
    }

    for (const auto preparation : pending.tlasPreparation)
    {
        if (preparation != DisplacementTLASPreparation::Requested &&

            preparation != DisplacementTLASPreparation::Recorded)
        {
            return false;
        }
    }

    return true;
}
