#pragma once

#include "Renderer/CommandContext.h"
#include "Renderer/DisplacementGPUIndexLayout.h"
#include "Renderer/RayTracing/RayTracingSceneResources.h"

#include <d3d12.h>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <vector>

class Mesh;
class Scene;
class Texture;

// Renderer implementation state. Resource pointers are borrowed; lifecycle
// resolution remains explicit and Renderer retains sole ownership of the records.
namespace RendererDisplacement
{
    enum class DisplacementUpdateStatus
    {
        Empty,
        Prepared, // Owned CPU request/reservation; no command recording or GPU mutation.
        Recording,
        Submitting, // Reserved Execute attempt armed; no render-visible provenance yet.
        Submitted,
        Finalizing,
        Completed,
        Discarded,
        FailedUncertain
    };

    enum class DisplacementHistoryPreparation
    {
        NotRequested,
        Requested,
        Performed
    };

    enum class DisplacementTLASPreparation
    {
        NotRequested,
        Requested,
        Recorded // A future merged slot build was recorded, not GPU-completed.
    };

    enum class DisplacementRecordingStage
    {
        None,
        Compute,
        BLAS,
        MergedTLAS
    };

    enum class DisplacementReservationOwnership
    {
        None,
        Reserved,
        Attempting,
        Uncertain
    };

    struct DisplacementTLASUse
    {
        TLASGeometryRecordingEvidence evidence{};

        std::uint64_t consumerGeneration = 0;
        // Borrowed exact-submission receipt, not a second token owner.
        // Future frame owner must retain it through validation/resolution.
        SubmissionFenceToken submission{};
    };

    enum class DisplacementFrameExitState
    {
        Empty,
        Recording,
        Ended,
        Discarded,
        Submitted,
        Uncertain
    };

    struct DisplacementFrameRecovery
    {
        DisplacementFrameExitState state = DisplacementFrameExitState::Empty;

        std::uint64_t generation = 0;
        std::uint64_t sceneLifetime = 0;
        unsigned int frameSlot = UINT_MAX;
        Mesh* mesh = nullptr;

        Texture* heightmap = nullptr;

        ID3D12Resource* patchResource = nullptr;

        D3D12_RESOURCE_STATES vertexState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES indexState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES patchState = D3D12_RESOURCE_STATE_COMMON;

        std::vector<D3D12_RESOURCE_STATES> heightmapStates;

        bool asMutationStarted = false;
    };

    struct PendingDisplacementUpdate
    {
        PendingDisplacementUpdate() = default;
        PendingDisplacementUpdate(const PendingDisplacementUpdate&) = delete;
        PendingDisplacementUpdate& operator=(const PendingDisplacementUpdate&) = delete;

        SubmissionFenceToken submission{};

        SubmissionFenceReservation reservation{};

        DisplacementReservationOwnership reservationOwnership = DisplacementReservationOwnership::None;
        const Scene* scene = nullptr;
        std::uint64_t sceneRevision = 0;
        std::uint64_t sceneResourceLifetime = 0;
        std::uint64_t requestRevision = 0;

        Mesh* mesh = nullptr;

        RayTracingGeometryCounts targetCounts{};
        // Owned snapshot in patchX * patchesPerAxis + patchZ order, not a span.
        std::vector<std::uint32_t> patchLODs;

        DisplacementMeshSettings settings{}; // Scalars only; patchLODs stays empty here.

        DisplacementGPUIndexLayout layout{};

        ID3D12Resource* preparedPatchResource = nullptr; // Borrowed persistent identity.
        unsigned int preparedPatchDescriptor = UINT_MAX;
        unsigned int frameSlot = UINT_MAX;
        std::uint64_t recordingGeneration = 0;

        DisplacementRecordingStage recordingStage = DisplacementRecordingStage::None;

        DisplacementUpdateStatus status = DisplacementUpdateStatus::Empty;

        DisplacementHistoryPreparation historyPreparation = DisplacementHistoryPreparation::NotRequested;
        DisplacementTLASPreparation tlasPreparation[2]{};
        SceneBLASRetirementBatchIdentity retirementBatch{};

        D3D12_GPU_VIRTUAL_ADDRESS replacementBLASAddress = 0;
        DisplacementTLASUse tlasUses[2]{};
        // Whole-frame snapshots survive the recording helper. They do not own
        // resources; registration, pending ownership and coverage prevent teardown.
        DisplacementFrameRecovery frameRecovery{};

        // No destructor-driven token/resource release. Future integration
        // must resolve completion/discard and explicitly release/clear the
        // retained token before reset. Uncertain work cannot be reset.
        bool ResetResolved();
    };

    enum class DisplacementRecordingOutcome
    {
        Empty,
        Active,
        Recorded,
        Discarded,
        Submitted,
        SubmissionUncertain
    };

    struct DisplacementCoverageReceipt
    {
        std::uint64_t generation = 0;
        unsigned int frameSlot = UINT_MAX;
        TLASGeometryRecordingEvidence geometry{};

        SubmissionFenceReservation reservation{}; // Preserved after an uncertain attempt.

        SubmissionFenceToken submission{};        // Immutable receipt even after checked release.
        // Original submission ownership stays in PendingDisplacementUpdate.
        // Later ownership moves from current into submitted[], never original.
        bool ownsReservation = false;
        bool ownsSubmission = false;

        DisplacementRecordingOutcome outcome = DisplacementRecordingOutcome::Empty;
    };

    struct DisplacementSubmissionCoverage
    {
        bool enabled = false;
        bool admissionClosed = false; // Completion/capacity frontier; never reopened for this update.
        bool failed = false;
        bool resolving = false; // One-shot freeze; only successful finalization clears coverage.

        std::uint64_t sceneLifetime = 0;
        std::uint64_t requestRevision = 0;
        const Mesh* mesh = nullptr;
        RayTracingGeometryCounts targetCounts{};

        DisplacementCoverageReceipt current{};
        DisplacementCoverageReceipt original{}; // Immutable after first successful submission.
        // No eviction. Exhaustion rejects before submission. Historical entries
        // attest validation at submission time; never query mutable TLAS state
        // to retrospectively revalidate them. Later entries own their retained
        // tokens until checked retirement-proof cleanup; original is a borrowed copy.
        std::array<DisplacementCoverageReceipt, 8> submitted{};

        size_t submittedCount = 0;
    };

    struct FrameDisplacementPreparation
    {
        std::uint64_t generation = 0;
        unsigned int slot = UINT_MAX;
        bool slotAcquired = false;
        unsigned int uploadResetCount = 0;
        bool diagnosticConsistent = false;
        bool mergedTLASRecorded = false;
        bool ordinaryTLASRecorded = false;
    };
}
