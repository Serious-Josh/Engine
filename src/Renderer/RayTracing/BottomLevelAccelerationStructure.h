#pragma once

#include "RayTracingAccelerationStructure.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>

struct BLASRetirementBatchIdentity
{
    std::uint64_t ownerLifetime = 0;
    std::uint64_t batchID = 0;
};

// One recorded build. ownerLifetime is fixed for an owner across Rebuild;
// buildID is per-owner monotonic and never reset, including by Shutdown.
// Neither a GPU address nor an object pointer participates in identity.
struct BLASBuildIdentity
{
    std::uint64_t ownerLifetime = 0;
    std::uint64_t buildID = 0;
};

// Checked monotonic allocation; refuses zero and exhaustion without advancing.
inline bool AllocateBLASBuildID(std::uint64_t& next, std::uint64_t& buildID) noexcept
{
    if (next == 0 || next == UINT64_MAX)
    {
        return false;
    }

    buildID = next++;
    return true;
}

// Descriptive evidence of exactly what one build recorded: the native values and
// the resources read or written at recording time. It carries no execution,
// completion or eligibility state, and COM pins prove lifetime, not GPU safety.
struct BLASBuildCapture
{
    BLASBuildIdentity identity{};

    // Native descriptor and input pins are captured only for single-geometry
    // builds (every current production caller); geometryCount records the total.
    std::uint32_t geometryCount = 0;
    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags{};
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    bool countOverride = false;

    Microsoft::WRL::ComPtr<ID3D12Resource> vertices;
    Microsoft::WRL::ComPtr<ID3D12Resource> indices;
    Microsoft::WRL::ComPtr<ID3D12Resource> result;
    Microsoft::WRL::ComPtr<ID3D12Resource> scratch;
};

// The tracked recording that contains a build command. A renderer generation is
// only unique together with the recording owner (scene resource lifetime).
struct BLASRecordingProvenance
{
    std::uint64_t recordingOwner = 0;
    std::uint64_t generation = 0;

    bool operator==(const BLASRecordingProvenance&) const = default;
};

// SubmissionUncertain is armed before Execute and leaves only through an
// authoritative fence confirmation or known non-submission. Completed requires a
// valid completed fence in the submitting context lifetime. Unattributed builds
// (no truthful recording context) never transition.
enum class BLASRecordingState : std::uint8_t
{
    Unattributed,
    Recorded,
    SubmissionUncertain,
    NotSubmitted,
    Submitted,
    Completed,
    Discarded
};

struct BLASBuildRecording
{
    BLASRecordingProvenance provenance{};
    BLASRecordingState state = BLASRecordingState::Unattributed;

    // Set only by ConfirmRecording; borrowed fence values mean nothing outside
    // the CommandContext submission lifetime they were issued in.
    std::uint64_t submissionFence = 0;
    std::uint64_t submissionLifetime = 0;
};

class BottomLevelAccelerationStructure
{
    public:
    BottomLevelAccelerationStructure() = default;

    BottomLevelAccelerationStructure(const BottomLevelAccelerationStructure&) = delete;
    BottomLevelAccelerationStructure& operator=(const BottomLevelAccelerationStructure&) = delete;

    bool Initialize(
        ID3D12Device5* device,
        ID3D12GraphicsCommandList4* commandList,
        const RayTracingBottomLevelDescription& description);
    bool Rebuild(
        ID3D12Device5* device,
        ID3D12GraphicsCommandList4* commandList,
        const RayTracingBottomLevelDescription& description);

    void ReleaseRetiredResources();
    // UNUSED selective path. Rejects overlapping batches, stale identities,
    // and duplicate releases. Identity is NOT GPU/reference-safety proof.
    BLASRetirementBatchIdentity GetExclusiveRetirementBatch() const;
    bool ValidateRetirementBatch(BLASRetirementBatchIdentity identity) const;
    bool ReleaseRetirementBatch(
        BLASRetirementBatchIdentity identity,
        bool gpuCompletionConfirmed,
        bool referencesSafe);

    size_t GetRetirementBatchCount() const
    {
        return m_retiredBatches.size();
    }

    void Shutdown();

    D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const;
    // Borrow for independent candidate lifetime pinning; registry retains ownership.
    ID3D12Resource* GetResource() const { return m_resource; }

    // Capture paired with the current result; empty identity when uninitialized.
    const BLASBuildCapture& GetCurrentBuildCapture() const
    {
        return m_currentCapture;
    }

    // Capture retired together with its result/scratch pair, or null.
    const BLASBuildCapture* GetRetiredBuildCapture(size_t index) const
    {
        return index < m_retiredBatches.size() ? &m_retiredBatches[index].capture : nullptr;
    }

    const BLASBuildRecording& GetCurrentBuildRecording() const
    {
        return m_currentRecording;
    }

    const BLASBuildRecording* GetRetiredBuildRecording(size_t index) const
    {
        return index < m_retiredBatches.size() ? &m_retiredBatches[index].recording : nullptr;
    }

    // Attributes the current build to its recording, once. Refused unless the
    // current identity is the expected (just recorded) build and provenance is nonzero.
    bool AttributeCurrentBuild(BLASBuildIdentity expected, BLASRecordingProvenance provenance) noexcept;
    // Applies one legal transition to every current or retired build recorded
    // under provenance; illegal transitions leave a build unchanged. Returns the count.
    // Arm/not-submitted/discard only; Submitted and Completed have dedicated entry points.
    std::size_t TransitionRecording(BLASRecordingProvenance provenance, BLASRecordingState next) noexcept;
    // SubmissionUncertain -> Submitted for builds under provenance, with the fence
    // and lifetime from the matching confirmation. Invalid evidence changes nothing.
    std::size_t ConfirmRecording(
        BLASRecordingProvenance provenance,
        std::uint64_t submissionFence,
        std::uint64_t submissionLifetime) noexcept;
    // Submitted -> Completed when completedFence is valid, the lifetime matches and
    // completedFence >= the build's submission fence. No other state is promoted.
    std::size_t ObserveCompletion(std::uint64_t completedFence, std::uint64_t contextLifetime) noexcept;

    private:
    bool InitializeBuild(
        ID3D12Device5* device,
        ID3D12GraphicsCommandList4* commandList,
        const RayTracingBottomLevelDescription& description,
        BLASBuildIdentity identity);

    ID3D12Resource* m_resource = nullptr;
    ID3D12Resource* m_scratchResource = nullptr;
    BLASBuildCapture m_currentCapture{};
    BLASBuildRecording m_currentRecording{};

    struct RetiredBatch
    {
        BLASRetirementBatchIdentity identity{};

        ID3D12Resource* result = nullptr;
        ID3D12Resource* scratch = nullptr;
        BLASBuildCapture capture{};
        BLASBuildRecording recording{};
    };

    std::uint64_t m_retirementLifetime = 0;
    std::uint64_t m_nextBuildID = 1;

    std::vector<RetiredBatch> m_retiredBatches;
};
