#pragma once

#include "RayTracingAccelerationStructure.h"

#include <d3d12.h>
#include <vector>
#include <cstdint>

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
    // Borrow for independent candidate lifetime pinning; registry retains ownership.
    ID3D12Resource* GetResource() const { return m_resource; }

    private:
    ID3D12Resource* m_resource = nullptr;
    ID3D12Resource* m_scratchResource = nullptr;

    struct RetiredBatch
    {
        BLASRetirementBatchIdentity identity{};

        ID3D12Resource* result = nullptr;
        ID3D12Resource* scratch = nullptr;
    };

    std::uint64_t m_retirementLifetime = 0;

    std::vector<RetiredBatch> m_retiredBatches;
};
