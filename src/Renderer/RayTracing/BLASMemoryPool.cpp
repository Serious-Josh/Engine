#include "BLASMemoryPool.h"

#include "Core/Log.h"
#include "Debug/MemoryRegistry.h"
#include "Renderer/TextureMemoryAllocator.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr std::uint64_t CommittedResourceGranularity = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

    std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    std::string FormatMebibytes(std::uint64_t bytes)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        return text;
    }

    // Acceleration-structure memory must be a buffer that allows unordered access and stays in
    // RAYTRACING_ACCELERATION_STRUCTURE state.
    HRESULT CreateResultBuffer(ID3D12Device* device, std::uint64_t bytes, ComPtr<ID3D12Resource>& resource)
    {
        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = bytes;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = DXGI_FORMAT_UNKNOWN;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        // Engine-owned memory (TextureMemoryAllocator.h), activated by the first BLAS built in it; committed otherwise. A block
        // the pool releases returns its range to that allocator.
        ID3D12Resource* placed = nullptr;

        if(SUCCEEDED(TextureMemoryAllocator::CreateBuffer(device, description,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, &placed)))
        {
            resource.Attach(placed);
            return S_OK;
        }

        return device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&resource));
    }
} // namespace

struct BLASMemoryAllocation::Block
{
    ComPtr<ID3D12Resource> resource;
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
    std::uint64_t capacity = 0;
    bool dedicated = false;

    // Free ranges by offset, coalesced, so no two are adjacent.
    std::map<std::uint64_t, std::uint64_t> freeRanges;
    std::uint32_t liveRanges = 0;
    std::uint32_t pendingRanges = 0;

    bool IsEmpty() const
    {
        return liveRanges == 0 && pendingRanges == 0;
    }
};

struct BLASMemoryAllocation::PoolState
{
    // A freed range that is not reusable yet. Collect stamps it with the last submitted fence the first time it sees
    // it; the range is reusable once that fence has completed.
    struct PendingRange
    {
        Block* block = nullptr;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        bool stamped = false;
        std::uint64_t submissionLifetime = 0;
        std::uint64_t reusableAfterFence = 0;
    };

    mutable std::mutex mutex;
    std::vector<std::unique_ptr<Block>> blocks;
    std::vector<PendingRange> pendingRanges;
    bool shutDown = false;

    // Live results: their count, the bytes the driver asked for, and what one committed resource per result would
    // reserve, for the statistics line.
    std::uint64_t liveAllocations = 0;
    std::uint64_t requestedBytes = 0;
    std::uint64_t committedEquivalentBytes = 0;

    // Session peaks of the same, and of the bytes the blocks reserve, for the shutdown line.
    std::uint64_t peakLiveAllocations = 0;
    std::uint64_t peakRequestedBytes = 0;
    std::uint64_t peakCommittedEquivalentBytes = 0;
    std::uint64_t reservedBytes = 0;
    std::uint64_t peakReservedBytes = 0;

    // The caller holds the mutex in every member below.
    Block* CreateBlock(ID3D12Device* device, std::uint64_t capacity, bool dedicated, std::string& error);
    void ReturnRange(Block& block, std::uint64_t offset, std::uint64_t size);
    std::uint32_t ReleaseUnretainedBlocks(std::uint64_t& releasedBytes);
    std::string Describe() const;
    std::string DescribePeaks() const;

    void Free(Block* block, std::uint64_t offset, std::uint64_t size, std::uint64_t requested);
};

BLASMemoryAllocation::Block* BLASMemoryAllocation::PoolState::CreateBlock(ID3D12Device* device, std::uint64_t capacity,
    bool dedicated, std::string& error)
{
    auto block = std::make_unique<Block>();
    const HRESULT result = CreateResultBuffer(device, capacity, block->resource);

    if(FAILED(result))
    {
        error = "Failed to allocate a BLAS memory pool block of " + FormatMebibytes(capacity) + ". HRESULT: " +
            std::to_string(result);
        return nullptr;
    }

    block->resource->SetName(dedicated ? L"BLAS memory pool dedicated block" : L"BLAS memory pool block");
    MemoryRegistry::TrackResource(block->resource.Get(), MemoryCategory::AccelerationStructures);

    block->address = block->resource->GetGPUVirtualAddress();
    block->capacity = capacity;
    block->dedicated = dedicated;
    block->freeRanges.emplace(0, capacity);

    reservedBytes += capacity;
    peakReservedBytes = (std::max)(peakReservedBytes, reservedBytes);

    blocks.push_back(std::move(block));
    return blocks.back().get();
}

void BLASMemoryAllocation::PoolState::ReturnRange(Block& block, std::uint64_t offset, std::uint64_t size)
{
    auto next = block.freeRanges.lower_bound(offset);

    if(next != block.freeRanges.end() && offset + size == next->first)
    {
        size += next->second;
        next = block.freeRanges.erase(next);
    }

    if(next != block.freeRanges.begin())
    {
        const auto previous = std::prev(next);

        if(previous->first + previous->second == offset)
        {
            previous->second += size;
            return;
        }
    }

    block.freeRanges.emplace_hint(next, offset, size);
}

std::uint32_t BLASMemoryAllocation::PoolState::ReleaseUnretainedBlocks(std::uint64_t& releasedBytes)
{
    std::uint32_t retainedEmptyBlocks = 0;
    std::uint32_t releasedBlocks = 0;

    const auto unretained = [&](const std::unique_ptr<Block>& block)
    {
        if(!block->IsEmpty())
        {
            return false;
        }

        const bool retained = !shutDown && !block->dedicated && retainedEmptyBlocks < BLASMemoryPool::RetainedEmptyBlocks;

        if(retained)
        {
            ++retainedEmptyBlocks;
            return false;
        }

        releasedBytes += block->capacity;
        reservedBytes -= block->capacity;
        ++releasedBlocks;
        return true;
    };

    blocks.erase(std::remove_if(blocks.begin(), blocks.end(), unretained), blocks.end());
    return releasedBlocks;
}

std::string BLASMemoryAllocation::PoolState::Describe() const
{
    std::size_t dedicatedBlocks = 0;

    for(const auto& block : blocks)
    {
        dedicatedBlocks += block->dedicated ? 1 : 0;
    }

    return std::to_string(blocks.size()) + " blocks (" + std::to_string(dedicatedBlocks) + " dedicated), " +
        FormatMebibytes(reservedBytes) + " reserved, " + FormatMebibytes(requestedBytes) + " in use by " +
        std::to_string(liveAllocations) + " BLAS results; one committed resource per result would reserve " +
        FormatMebibytes(committedEquivalentBytes);
}

std::string BLASMemoryAllocation::PoolState::DescribePeaks() const
{
    return "peaks " + FormatMebibytes(peakReservedBytes) + " reserved, " + FormatMebibytes(peakRequestedBytes) +
        " in use by " + std::to_string(peakLiveAllocations) + " BLAS results; one committed resource per result " +
        "would have peaked at " + FormatMebibytes(peakCommittedEquivalentBytes);
}

void BLASMemoryAllocation::PoolState::Free(Block* block, std::uint64_t offset, std::uint64_t size,
    std::uint64_t requested)
{
    const std::lock_guard lock(mutex);

    --block->liveRanges;
    --liveAllocations;
    requestedBytes -= requested;
    committedEquivalentBytes -= AlignUp(requested, CommittedResourceGranularity);

    // After Shutdown the GPU has been drained for good, so the range is reusable at once and an emptied block goes.
    if(shutDown)
    {
        ReturnRange(*block, offset, size);
        std::uint64_t releasedBytes = 0;
        ReleaseUnretainedBlocks(releasedBytes);
        return;
    }

    ++block->pendingRanges;
    pendingRanges.push_back({block, offset, size});
}

BLASMemoryAllocation::~BLASMemoryAllocation()
{
    if(m_pool)
    {
        m_pool->Free(m_block, m_offset, m_size, m_requestedBytes);
    }
}

BLASMemoryPool::BLASMemoryPool()
    : m_state(std::make_shared<BLASMemoryAllocation::PoolState>())
{
}

BLASMemoryPool::~BLASMemoryPool() = default;

std::shared_ptr<const BLASMemoryAllocation> BLASMemoryPool::Allocate(BLASMemoryPool* pool, ID3D12Device* device,
    std::uint64_t resultBytes, std::string& error)
{
    if(!device || resultBytes == 0)
    {
        error = "BLAS result allocation needs a device and a nonzero size.";
        return nullptr;
    }

    std::shared_ptr<BLASMemoryAllocation> allocation(new BLASMemoryAllocation());
    allocation->m_requestedBytes = resultBytes;

    // Without a pool: one committed buffer per result.
    if(!pool)
    {
        const HRESULT result = CreateResultBuffer(device, resultBytes, allocation->m_resource);

        if(FAILED(result))
        {
            error = "Failed to allocate BLAS result resource. HRESULT: " + std::to_string(result);
            return nullptr;
        }

        MemoryRegistry::TrackResource(allocation->m_resource.Get(), MemoryCategory::AccelerationStructures);
        allocation->m_address = allocation->m_resource->GetGPUVirtualAddress();
        allocation->m_size = resultBytes;
        return allocation;
    }

    auto& state = *pool->m_state;
    const std::lock_guard lock(state.mutex);

    const std::uint64_t size = AlignUp(resultBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    BLASMemoryAllocation::Block* chosenBlock = nullptr;
    std::uint64_t chosenOffset = 0;
    std::uint64_t chosenRangeSize = 0;
    bool createdBlock = false;

    if(size > DedicatedThresholdBytes)
    {
        chosenBlock = state.CreateBlock(device, AlignUp(size, CommittedResourceGranularity), true, error);
        createdBlock = true;
    }
    else
    {
        // Best fit over the standard blocks' free ranges keeps large ranges whole for large results.
        for(const auto& block : state.blocks)
        {
            if(block->dedicated)
            {
                continue;
            }

            for(const auto& [offset, rangeSize] : block->freeRanges)
            {
                const bool fits = rangeSize >= size;
                const bool tighter = !chosenBlock || rangeSize < chosenRangeSize;

                if(fits && tighter)
                {
                    chosenBlock = block.get();
                    chosenOffset = offset;
                    chosenRangeSize = rangeSize;
                }
            }
        }

        if(!chosenBlock)
        {
            chosenBlock = state.CreateBlock(device, BlockBytes, false, error);
            createdBlock = true;
        }
    }

    if(!chosenBlock)
    {
        return nullptr;
    }

    if(createdBlock)
    {
        chosenOffset = 0;
        chosenRangeSize = chosenBlock->capacity;
    }

    // Carve the range from the front of the chosen free range; the remainder stays free.
    chosenBlock->freeRanges.erase(chosenOffset);

    if(chosenRangeSize > size)
    {
        chosenBlock->freeRanges.emplace(chosenOffset + size, chosenRangeSize - size);
    }

    ++chosenBlock->liveRanges;
    ++state.liveAllocations;
    state.requestedBytes += resultBytes;
    state.committedEquivalentBytes += AlignUp(resultBytes, CommittedResourceGranularity);
    state.peakLiveAllocations = (std::max)(state.peakLiveAllocations, state.liveAllocations);
    state.peakRequestedBytes = (std::max)(state.peakRequestedBytes, state.requestedBytes);
    state.peakCommittedEquivalentBytes = (std::max)(state.peakCommittedEquivalentBytes,
        state.committedEquivalentBytes);

    allocation->m_pool = pool->m_state;
    allocation->m_block = chosenBlock;
    allocation->m_resource = chosenBlock->resource;
    allocation->m_address = chosenBlock->address + chosenOffset;
    allocation->m_offset = chosenOffset;
    allocation->m_size = size;

    if(createdBlock)
    {
        Log::Info("BLAS memory pool: created a " + std::string(chosenBlock->dedicated ? "dedicated " : "") + "block of " +
            FormatMebibytes(chosenBlock->capacity) + "; " + state.Describe() + ".");
    }

    return allocation;
}

void BLASMemoryPool::Collect(std::uint64_t submissionLifetime, std::uint64_t lastSubmittedFence,
    std::uint64_t completedFence)
{
    auto& state = *m_state;
    const std::lock_guard lock(state.mutex);

    if(state.pendingRanges.empty())
    {
        return;
    }

    const bool completedFenceKnown = completedFence != UINT64_MAX;

    const auto reusable = [&](BLASMemoryAllocation::PoolState::PendingRange& range)
    {
        if(!range.stamped)
        {
            range.stamped = true;
            range.submissionLifetime = submissionLifetime;
            range.reusableAfterFence = lastSubmittedFence;
        }

        const bool earlierLifetime = range.submissionLifetime != submissionLifetime;
        const bool fenceCompleted = completedFenceKnown && completedFence >= range.reusableAfterFence;

        if(!earlierLifetime && !fenceCompleted)
        {
            return false;
        }

        state.ReturnRange(*range.block, range.offset, range.size);
        --range.block->pendingRanges;
        return true;
    };

    state.pendingRanges.erase(std::remove_if(state.pendingRanges.begin(), state.pendingRanges.end(), reusable),
        state.pendingRanges.end());

    std::uint64_t releasedBytes = 0;
    const std::uint32_t releasedBlocks = state.ReleaseUnretainedBlocks(releasedBytes);

    if(releasedBlocks > 0)
    {
        Log::Info("BLAS memory pool: released " + std::to_string(releasedBlocks) + " blocks (" +
            FormatMebibytes(releasedBytes) + "); " + state.Describe() + ".");
    }
}

void BLASMemoryPool::Shutdown()
{
    // A later Initialize of the owner starts again from an empty pool. The old state lives on only as long as a
    // leaked allocation still references it, and it releases that allocation's block when the reference drops.
    const std::shared_ptr<BLASMemoryAllocation::PoolState> previous = std::exchange(m_state,
        std::make_shared<BLASMemoryAllocation::PoolState>());
    auto& state = *previous;
    const std::lock_guard lock(state.mutex);

    if(state.blocks.empty())
    {
        return;
    }

    Log::Info("BLAS memory pool at shutdown: " + state.Describe() + "; " + state.DescribePeaks() + ".");
    state.shutDown = true;

    for(const auto& range : state.pendingRanges)
    {
        state.ReturnRange(*range.block, range.offset, range.size);
        --range.block->pendingRanges;
    }

    state.pendingRanges.clear();

    std::uint64_t releasedBytes = 0;
    state.ReleaseUnretainedBlocks(releasedBytes);

    if(!state.blocks.empty())
    {
        Log::Warning("BLAS memory pool: " + std::to_string(state.liveAllocations) + " BLAS results in " +
            std::to_string(state.blocks.size()) + " blocks are still referenced at shutdown; their blocks are released "
            "with the last reference.");
    }
}

std::string BLASMemoryPool::DescribeStatistics() const
{
    const std::lock_guard lock(m_state->mutex);
    return m_state->Describe();
}
