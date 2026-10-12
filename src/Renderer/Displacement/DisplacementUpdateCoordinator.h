#pragma once

#include <cstdint>

class Scene;
class RayTracingSceneResources;

namespace RendererDisplacement
{
    enum class DisplacementRecordingStage;
    struct FrameDisplacementPreparation;
    struct PendingDisplacementUpdate;
    struct DisplacementSubmissionCoverage;

    // Call-local borrowed state. No validation context is cached or retained.
    struct ValidationContext
    {
        const Scene* scene = nullptr;
        std::uint64_t sceneResourceLifetime = 0;
        std::uint64_t recordingGeneration = 0;
        unsigned int frameSlotCount = 0;
        bool geometryUpdateFailed = false;
    };

    bool HasUnresolvedState(
        const PendingDisplacementUpdate& pending,
        const DisplacementSubmissionCoverage& coverage);
    bool ValidateIdentity(
        const PendingDisplacementUpdate& pending,
        const ValidationContext& context);
    // Local consistency only; does not establish CommandContext submission provenance.
    bool ValidateReservation(
        const PendingDisplacementUpdate& pending,
        const ValidationContext& context);

    bool ValidateRecording(
        const PendingDisplacementUpdate& pending,
        const ValidationContext& context,
        unsigned int activeFrameSlot,
        unsigned int requestedFrameSlot);
    // Initializes local coverage bookkeeping only, not TLAS or submission evidence.
    // Validation failure leaves coverage unchanged. Neither input is retained.
    bool BeginCoverage(
        const PendingDisplacementUpdate& pending,
        DisplacementSubmissionCoverage& coverage,
        const ValidationContext& context,
        unsigned int activeFrameSlot);

    // Borrowed recording identity only; no state is changed or retained.
    bool ValidateFramePreparation(
        const FrameDisplacementPreparation& preparation,
        std::uint64_t recordingGeneration,
        unsigned int activeFrameSlot,
        unsigned int frameSlot,
        unsigned int frameSlotCount);

    // Commits only checked local milestones. Renderer invalidates eligibility on failure.
    // Current TLAS evidence is validated lazily at the merged-TLAS transition.
    bool AdvanceRecording(
        PendingDisplacementUpdate& pending,
        FrameDisplacementPreparation& preparation,
        const ValidationContext& context,
        unsigned int activeFrameSlot,
        unsigned int frameSlot,
        DisplacementRecordingStage stage,
        bool recordingSucceeded,
        const RayTracingSceneResources& rayTracingScene);

    // Caller must first pass Renderer activation guards. Only local activation
    // and limited rollback occur here; reservation ownership is unchanged.
    bool ActivateRecording(
        PendingDisplacementUpdate& pending,
        DisplacementSubmissionCoverage& coverage,
        const ValidationContext& context,
        unsigned int activeFrameSlot,
        unsigned int requestedFrameSlot,
        bool& geometryUpdateFailed);

    bool ValidateCoverage(
        const PendingDisplacementUpdate& pending,
        const DisplacementSubmissionCoverage& coverage,
        const ValidationContext& context);
    // Caller first validates coverage bounds and original submission/retirement
    // ownership. Frozen receipts are checked without querying current TLAS state.
    bool ValidateHistoricalRetirementCoverage(
        const DisplacementSubmissionCoverage& coverage,
        unsigned int frameSlotCount);

    // Caller first proves completion, coverage bounds/nonempty history, pending
    // slot validity, current evidence, and resolved ownership. No GPU queries.
    bool ValidateCoverageResolutionRecords(
        const PendingDisplacementUpdate& pending,
        const DisplacementSubmissionCoverage& coverage);
}
