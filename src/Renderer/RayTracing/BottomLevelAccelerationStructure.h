#pragma once

#include "RayTracingAccelerationStructure.h"

#include <d3d12.h>
#include <vector>
#include <cstdint>
#include <memory>

class BLASMemoryAllocation;

struct BLASRetirementBatchIdentity
{
    std::uint64_t ownerLifetime = 0;
    std::uint64_t batchID = 0;
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
    // The buffer that holds the result: a BLAS memory pool block shared with other BLASes, or a committed buffer of its
    // own without a pool. Not an identity: compare GetGPUAddress() instead.
    ID3D12Resource* GetResource() const;
    // A reference for independent candidate lifetime pinning; the registry retains ownership of the BLAS.
    const std::shared_ptr<const BLASMemoryAllocation>& GetResultAllocation() const { return m_result; }

    private:
    bool InitializeBuild(
        ID3D12Device5* device,
        ID3D12GraphicsCommandList4* commandList,
        const RayTracingBottomLevelDescription& description);

    std::shared_ptr<const BLASMemoryAllocation> m_result;
    ID3D12Resource* m_scratchResource = nullptr;

    struct RetiredBatch
    {
        BLASRetirementBatchIdentity identity{};

        std::shared_ptr<const BLASMemoryAllocation> result;
        ID3D12Resource* scratch = nullptr;
    };

    std::uint64_t m_retirementLifetime = 0;

    std::vector<RetiredBatch> m_retiredBatches;
};
