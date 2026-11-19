#pragma once

#include "RayTracingAccelerationStructure.h"

#include <d3d12.h>
#include <vector>
#include <climits>
#include <atomic>

// Checked monotonic allocator for TLAS recording identities; refuses zero and exhaustion.
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

    // Identity of the last successful build or update recording, or 0 after a failed one. Diagnostics compare it to
    // detect whether a TLAS was recorded again.
    uint64_t GetRecordingIdentity() const
    {
        return m_recordingIdentity;
    }

    // PT.2A: identity of what the TLAS references, apart from instance transforms. A build allocates one; an update keeps
    // it when every changed instance kept its BLAS, instance ID, mask and flags (a transform-only update) and allocates a
    // new one otherwise (a rebuilt BLAS, displacement, a re-resolved reference). 0 after a failed recording.
    uint64_t GetGeometryIdentity() const
    {
        return m_geometryIdentity;
    }

    // Instance-upload slot of the last successful recording, or UINT_MAX.
    unsigned int GetRecordingSlot() const
    {
        return m_recordingSlot;
    }

    // The recording `recordingIdentity` is still the current one, its upload slot has no pending writes, and the
    // instance's native descriptor references `address` (displacement TLAS-use evidence).
    bool RecordedInstanceUsesBLAS(
        uint64_t recordingIdentity,
        size_t instanceIndex,
        D3D12_GPU_VIRTUAL_ADDRESS address) const;

    private:
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
    uint64_t m_geometryIdentity = 0;
    unsigned int m_recordingSlot = UINT_MAX;
};
