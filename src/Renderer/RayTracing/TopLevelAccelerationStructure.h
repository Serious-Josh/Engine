#pragma once

#include "RayTracingAccelerationStructure.h"
#include "BottomLevelAccelerationStructure.h"

#include <d3d12.h>
#include <vector>
#include <climits>
#include <array>
#include <atomic>
#include <memory>

// Shared checked allocator used by the owner and by isolated exhaustion tests.
inline std::uint64_t AllocateTLASIdentity(std::atomic<std::uint64_t>& next) noexcept
{
    auto value = next.load(std::memory_order_relaxed);

    while(value != 0 && value != UINT64_MAX)
    {
        if(next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
        {
            return value;
        }
    }

    return 0;
}

struct TLASRecordingCapture
{
    struct BLASReference
    {
        BLASBuildIdentity identity{};
        Microsoft::WRL::ComPtr<ID3D12Resource> result;
    };

    std::uint64_t ownerLifetime = 0;
    std::uint64_t recordingIdentity = 0;
    unsigned int uploadSlot = UINT_MAX;
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
    std::vector<BLASReference> bottomLevels; // Parallel to compact native instances.
    bool completeBLASReferences = false;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    Microsoft::WRL::ComPtr<ID3D12Resource> result;
    Microsoft::WRL::ComPtr<ID3D12Resource> scratch;
    Microsoft::WRL::ComPtr<ID3D12Resource> instanceUpload;
    std::uint64_t resourceCharge = 0; // Conservative sum, including repeated BLAS pins.
};

using TLASRecordingHandle = std::shared_ptr<const TLASRecordingCapture>;
enum class TLASCaptureStatus { Unavailable, Ready, InstanceLimit, CapacityExceeded, ResourceBudgetExceeded, AllocationFailed };

// The same tracked recording state machine/provenance used by production BLAS.
// Reusing its value types does not associate a TLAS with BLAS completion.
using TLASRecordingState = BLASRecordingState;
using TLASRecordingProvenance = BLASRecordingProvenance;

struct TLASSubmissionEvidence
{
    std::uint64_t ownerLifetime = 0;
    std::uint64_t recordingIdentity = 0;
    TLASRecordingProvenance provenance{};
    TLASRecordingState state = TLASRecordingState::Unattributed;
    std::uint64_t submissionFence = 0;
    std::uint64_t submissionLifetime = 0;
};

class BottomLevelAccelerationStructure;

class TopLevelAccelerationStructure
{
    public:
    TopLevelAccelerationStructure() = default;

    TopLevelAccelerationStructure(const TopLevelAccelerationStructure&) = delete;
    TopLevelAccelerationStructure& operator=(const TopLevelAccelerationStructure&) = delete;

    bool Initialize(
        ID3D12Device5* device,
        ID3D12GraphicsCommandList4* commandList,
        const RayTracingTopLevelDescription& description,
        const std::vector<const BottomLevelAccelerationStructure*>& bottomLevelStructures);
    bool Update(
        ID3D12GraphicsCommandList4* commandList,
        unsigned int frameIndex,
        const std::vector<RayTracingInstanceDescription>& instances,
        const std::vector<std::size_t>& changedInstanceIndices,
        const std::vector<const BottomLevelAccelerationStructure*>& bottomLevelStructures);

    bool RefreshBLASReferences(
        ID3D12GraphicsCommandList4* commandList,
        unsigned int frameIndex,
        const std::vector<RayTracingInstanceDescription>& instances,
        const std::vector<const BottomLevelAccelerationStructure*>& bottomLevelStructures);

    void Shutdown();

    D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const;

    // Read-only CPU recording evidence, not submission/completion proof.
    uint64_t GetRecordingIdentity() const
    {
        return m_recordingIdentity;
    }

    unsigned int GetRecordingSlot() const
    {
        return m_recordingSlot;
    }

    // Immutable native recording evidence only. The result is updated IN PLACE;
    // pins do not preserve historical TLAS contents or authorize a GPU reader.
    TLASRecordingHandle GetRecordingCapture() const noexcept
    {
        return m_capture;
    }

    TLASCaptureStatus GetCaptureStatus() const noexcept
    {
        return m_captureStatus;
    }

    // Owner-held lifecycle evidence for this exact retained capture. Output is
    // unchanged on failure, including checked owner teardown. No acceptance or
    // historical TLAS traversal permission follows from Completed.
    bool GetSubmissionEvidence(const TLASRecordingHandle& capture, TLASSubmissionEvidence& output) const noexcept;
    static constexpr std::size_t MaxCaptureInstances = 4096;
    static constexpr std::size_t MaxLiveCaptures = 4;
    static constexpr std::uint64_t MaxCaptureResourceBytes = 16ull * 1024 * 1024;

    bool RecordedInstanceUsesBLAS(
        uint64_t recordingIdentity,
        size_t instanceIndex,
        D3D12_GPU_VIRTUAL_ADDRESS address) const;

    private:
    friend class RayTracingSceneResources;
    friend class RayTracingRecordingDiagnostic;
    bool AttributeCurrentCapture(const TLASRecordingHandle& expected, TLASRecordingProvenance provenance) noexcept;
    void TransitionCaptures(TLASRecordingProvenance provenance, TLASRecordingState next) noexcept;
    void ConfirmCaptures(TLASRecordingProvenance provenance, std::uint64_t fence, std::uint64_t lifetime) noexcept;
    void ObserveCaptureCompletion(std::uint64_t completed, std::uint64_t lifetime, std::uint64_t recordingOwner) noexcept;
    std::shared_ptr<TLASRecordingCapture> PrepareCapture(unsigned int slot, std::uint64_t recording,
        const std::vector<const BottomLevelAccelerationStructure*>& bottomLevels) noexcept;
    ID3D12Resource* m_resource = nullptr;
    ID3D12Resource* m_scratchResource = nullptr;

    static constexpr unsigned int FrameCount = 2;
    ID3D12Resource* m_instanceBuffers[FrameCount]{};

    std::size_t m_instanceCount = 0;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS m_buildFlags =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;

    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> m_instanceDescriptions;
    std::vector<bool> m_pendingInstanceUpdates[FrameCount];
    std::vector<std::size_t> m_pendingInstanceIndices[FrameCount];
    uint64_t m_recordingIdentity = 0;
    unsigned int m_recordingSlot = UINT_MAX;
    std::uint64_t m_ownerLifetime = 0;
    // IDs are captured when the native descriptor is assigned, never inferred
    // later from a possibly replaced registry BLAS or from an address alone.
    std::vector<BLASBuildIdentity> m_instanceBuildIdentities;
    TLASRecordingHandle m_capture;
    TLASCaptureStatus m_captureStatus = TLASCaptureStatus::Unavailable;
    // Survives Shutdown so externally held historical captures remain charged.
    struct CaptureSlot
    {
        std::weak_ptr<const TLASRecordingCapture> tracked;
        TLASRecordingHandle unresolved; // Strong until proven discard/completion or checked teardown.
        TLASSubmissionEvidence evidence;
    };
    std::array<CaptureSlot, MaxLiveCaptures> m_captureSlots;
};
