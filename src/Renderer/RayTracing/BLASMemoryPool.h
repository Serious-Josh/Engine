#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>

class BLASMemoryPool;

// One bottom-level acceleration structure's result memory: a 256-byte-aligned range of a pool block, or a committed
// buffer of its own when it was allocated without a pool. Reference counted: the BLAS holds one reference and every
// membership candidate that pins the BLAS holds another. The range returns to its pool when the last reference drops,
// which the Engine already does only once the GPU has finished with the BLAS.
class BLASMemoryAllocation
{
public:
    BLASMemoryAllocation(const BLASMemoryAllocation&) = delete;
    BLASMemoryAllocation& operator=(const BLASMemoryAllocation&) = delete;
    ~BLASMemoryAllocation();

    // The container buffer the range lives in. Builds write through GetGPUAddress(); UAV barriers that order a build
    // before its first use name this resource, because the debug layer and the driver track the container, not ranges.
    ID3D12Resource* GetResource() const
    {
        return m_resource.Get();
    }

    D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const
    {
        return m_address;
    }

    std::uint64_t GetSize() const
    {
        return m_size;
    }

private:
    friend class BLASMemoryPool;
    struct PoolState;
    struct Block;

    BLASMemoryAllocation() = default;

    std::shared_ptr<PoolState> m_pool;
    Block* m_block = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    D3D12_GPU_VIRTUAL_ADDRESS m_address = 0;
    std::uint64_t m_offset = 0;
    std::uint64_t m_size = 0;
    std::uint64_t m_requestedBytes = 0;
};

// Bottom-level acceleration-structure result memory, sub-allocated from large container buffers as NVIDIA's DXR
// acceleration-structure memory guidance recommends. A committed resource per BLAS rounds every result up to 64 KiB and
// hands its memory back to the OS whenever a mesh leaves residency or a scene is switched. Here a result is a
// 256-byte-aligned range of an 8 MiB block. Only a BLAS larger than half a block gets a dedicated block, sized to it.
//
// Reuse: a freed range becomes reusable only after the frame queue has completed every submission made up to the
// Collect that first sees it. So a new build can never overwrite memory that an in-flight frame might still traverse,
// even if a reference was dropped early. A range is only ever reused by another BLAS in the same container buffer.
// That buffer stays in RAYTRACING_ACCELERATION_STRUCTURE state for its whole life, so there is no aliasing between
// resources and no aliasing barriers are needed.
//
// Blocks: standard blocks are kept for the session, so a scene switch reuses the previous scene's BLAS memory instead
// of freeing and reallocating it. Up to RetainedEmptyBlocks empty blocks are kept in reserve; beyond that, empty blocks
// are released once their ranges' fences have completed. A dedicated block is released as soon as its BLAS's range is
// reusable, just as its committed resource would have been.
//
// Compaction-ready: ranges of any size can be allocated and freed independently. A compacted copy can take a new range
// and free the original one, with no block-level bookkeeping.
class BLASMemoryPool
{
public:
    static constexpr std::uint64_t BlockBytes = 8ull * 1024ull * 1024ull;
    static constexpr std::uint64_t DedicatedThresholdBytes = BlockBytes / 2;
    static constexpr std::uint32_t RetainedEmptyBlocks = 8;

    BLASMemoryPool();
    BLASMemoryPool(const BLASMemoryPool&) = delete;
    BLASMemoryPool& operator=(const BLASMemoryPool&) = delete;
    ~BLASMemoryPool();

    // Allocates resultBytes of BLAS result memory. Without a pool, the result is a committed buffer of its own, which
    // was the only path before the pool. Returns null if the memory cannot be created.
    static std::shared_ptr<const BLASMemoryAllocation> Allocate(BLASMemoryPool* pool, ID3D12Device* device,
        std::uint64_t resultBytes, std::string& error);

    // Called once per frame, and after GPU drains, by the owner of the frame queue. It stamps newly freed ranges with
    // the last submitted fence, returns ranges whose fence has completed to their blocks, and releases empty blocks the
    // retention policy does not keep. A new submission lifetime means the previous context was drained, so ranges
    // stamped in an earlier lifetime are reusable.
    void Collect(std::uint64_t submissionLifetime, std::uint64_t lastSubmittedFence, std::uint64_t completedFence);

    // After the final GPU drain: releases every empty block and leaves an empty pool for a later reinitialization. A
    // block that still holds live ranges stays alive until its last range is dropped, and a warning says how many
    // there are.
    void Shutdown();

    std::string DescribeStatistics() const;

private:
    std::shared_ptr<BLASMemoryAllocation::PoolState> m_state;
};
