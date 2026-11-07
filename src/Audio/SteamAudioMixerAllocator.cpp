#include "Audio/SteamAudioMixerAllocator.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <malloc.h>
#include <memory>

namespace Audio::Internal::SteamAudioMixerAllocator
{
namespace
{
    constexpr std::size_t PoolAlignment = 64;
    constexpr std::size_t MinimumHeapBlock = sizeof(void*);
    constexpr std::uint32_t EmptyList = 0;
    // A list only changes under this CAS when another thread frees, so the limit is never reached in practice. It keeps the mixer's
    // path bounded: past it, the allocation is treated as exhausted.
    constexpr int MaximumPopAttempts = 256;
    // One active region, and any from earlier sessions still waiting for blocks. Descriptors are never destroyed, so any thread may read them.
    constexpr int RegionSlots = 4;
    constexpr unsigned char BlockFree = 0;
    constexpr unsigned char BlockInUse = 1;
    constexpr unsigned char FreedFill = 0xFF;

    constexpr std::array<std::size_t, ClassCount> MakeClassSizes()
    {
        std::array<std::size_t, ClassCount> sizes{};
        std::size_t power = PoolAlignment;
        sizes[0] = power;
        sizes[1] = power * 2;
        sizes[2] = power * 3;
        power *= 4;

        for(int index = 3; index < ClassCount; index += 2)
        {
            sizes[static_cast<std::size_t>(index)] = power;

            if(index + 1 < ClassCount)
            {
                sizes[static_cast<std::size_t>(index) + 1] = power + power / 2;
            }

            power *= 2;
        }

        return sizes;
    }

    constexpr std::array<std::size_t, ClassCount> ClassSizes = MakeClassSizes();

    int ClassForSize(std::size_t size) noexcept
    {
        const auto found = std::lower_bound(ClassSizes.begin(), ClassSizes.end(), size);
        return found == ClassSizes.end() ? ClassCount : static_cast<int>(found - ClassSizes.begin());
    }

    struct PoolClass
    {
        // Low half: index of the first free block + 1. High half: a tag bumped by every pop, so a stale head can never match (ABA).
        std::atomic<std::uint64_t> head{0};
        std::atomic<std::size_t> used{0};
        std::atomic<std::size_t> peak{0};
        std::unique_ptr<std::atomic<std::uint32_t>[]> next;
        std::unique_ptr<std::atomic<unsigned char>[]> state;
        std::size_t firstByte = 0;
        std::size_t blockCount = 0;
    };

    struct Region
    {
        std::atomic<char*> begin{nullptr};
        std::atomic<char*> end{nullptr};
        std::array<PoolClass, ClassCount> classes;
        std::atomic<std::size_t> outstandingBlocks{0};
        std::atomic<std::size_t> usedBytes{0};
        // Allocate and Free calls inside this region now. A count of zero blocks alone does not free it: a Free has returned its block
        // before it finishes pushing it, and still reads the lists afterwards.
        std::atomic<int> callers{0};
        // Set when begin is withdrawn but a caller was still inside, so the mapping waits for a later pass.
        char* withdrawnMemory = nullptr;
        std::size_t capacityBytes = 0;
        bool fillFreed = false;
    };

    Region regions[RegionSlots];
    std::atomic<Region*> activeRegion{nullptr};

    // Heap blocks freed during a mix, linked through their own first bytes; only ever pushed to, then taken whole.
    std::atomic<void*> deferredHeapBlocks{nullptr};

    thread_local bool insideMix = false;
    thread_local ClassCounts* profile = nullptr;
    ClassCounts profileCounts{};

    std::atomic<std::uint64_t> poolAllocations{0};
    std::atomic<std::uint64_t> poolFrees{0};
    std::atomic<std::uint64_t> mixerHeapAllocations{0};
    std::atomic<std::uint64_t> mixerHeapFrees{0};
    std::atomic<std::uint64_t> largerClassAllocations{0};
    std::atomic<std::size_t> largestHeapRequest{0};
    std::atomic<std::uint64_t> doubleFrees{0};
    std::atomic<std::uint64_t> invalidFrees{0};
    std::atomic<std::uint64_t> regionsRetained{0};
    std::atomic<std::uint64_t> regionsReclaimed{0};
    std::atomic<std::size_t> peakBytes{0};

    // Holds a region against reclamation. The reclaimer withdraws begin and then reads callers; this raises callers and then rereads
    // begin. In seq_cst order at least one of them sees the other, so neither can miss.
    bool EnterRegion(Region& region, const char* expectedBegin) noexcept
    {
        region.callers.fetch_add(1, std::memory_order_seq_cst);

        if(region.begin.load(std::memory_order_seq_cst) == expectedBegin)
        {
            return true;
        }

        region.callers.fetch_sub(1, std::memory_order_seq_cst);
        return false;
    }

    void LeaveRegion(Region& region) noexcept
    {
        region.callers.fetch_sub(1, std::memory_order_seq_cst);
    }

    void* PopFrom(Region& region, int classIndex) noexcept
    {
        PoolClass& poolClass = region.classes[static_cast<std::size_t>(classIndex)];
        std::uint64_t head = poolClass.head.load(std::memory_order_acquire);

        for(int attempt = 0; attempt < MaximumPopAttempts; ++attempt)
        {
            const auto first = static_cast<std::uint32_t>(head);

            if(first == EmptyList)
            {
                return nullptr;
            }

            const std::uint32_t index = first - 1;
            const std::uint32_t second = poolClass.next[index].load(std::memory_order_relaxed);
            const std::uint64_t tag = (head >> 32) + 1;
            const std::uint64_t replacement = (tag << 32) | second;

            if(poolClass.head.compare_exchange_weak(head, replacement, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                poolClass.state[index].store(BlockInUse, std::memory_order_relaxed);
                region.outstandingBlocks.fetch_add(1, std::memory_order_relaxed);
                const std::size_t used = poolClass.used.fetch_add(1, std::memory_order_relaxed) + 1;

                // Only the mixer takes blocks, so nothing else raises these peaks.
                if(used > poolClass.peak.load(std::memory_order_relaxed))
                {
                    poolClass.peak.store(used, std::memory_order_relaxed);
                }

                const std::size_t bytes = region.usedBytes.fetch_add(ClassSizes[static_cast<std::size_t>(classIndex)], std::memory_order_relaxed) +
                    ClassSizes[static_cast<std::size_t>(classIndex)];

                if(bytes > peakBytes.load(std::memory_order_relaxed))
                {
                    peakBytes.store(bytes, std::memory_order_relaxed);
                }

                poolAllocations.fetch_add(1, std::memory_order_relaxed);
                return region.begin.load(std::memory_order_relaxed) + poolClass.firstByte + index * ClassSizes[static_cast<std::size_t>(classIndex)];
            }
        }

        return nullptr;
    }

    void PushTo(Region& region, int classIndex, std::uint32_t index, void* block) noexcept
    {
        PoolClass& poolClass = region.classes[static_cast<std::size_t>(classIndex)];

        if(poolClass.state[index].exchange(BlockFree, std::memory_order_acq_rel) != BlockInUse)
        {
            doubleFrees.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if(region.fillFreed)
        {
            std::memset(block, FreedFill, ClassSizes[static_cast<std::size_t>(classIndex)]);
        }

        poolClass.used.fetch_sub(1, std::memory_order_relaxed);
        region.usedBytes.fetch_sub(ClassSizes[static_cast<std::size_t>(classIndex)], std::memory_order_relaxed);
        region.outstandingBlocks.fetch_sub(1, std::memory_order_relaxed);
        poolFrees.fetch_add(1, std::memory_order_relaxed);
        std::uint64_t head = poolClass.head.load(std::memory_order_acquire);
        std::uint64_t replacement = 0;

        do
        {
            poolClass.next[index].store(static_cast<std::uint32_t>(head), std::memory_order_relaxed);
            replacement = (((head >> 32) + 1) << 32) | (index + 1);
        }
        while(!poolClass.head.compare_exchange_weak(head, replacement, std::memory_order_acq_rel, std::memory_order_acquire));
    }

    // A block's region cannot be reclaimed while the block is outstanding, and EnterRegion holds it for the rest of this call. A pointer
    // freed twice after its region was reclaimed cannot be recognised: the SDK has never been seen to do it.
    bool FreeIntoPool(void* block) noexcept
    {
        char* const address = static_cast<char*>(block);

        for(Region& region : regions)
        {
            char* const begin = region.begin.load(std::memory_order_acquire);

            if(!begin || address < begin || address >= region.end.load(std::memory_order_acquire))
            {
                continue;
            }

            if(!EnterRegion(region, begin))
            {
                continue;
            }

            const auto offset = static_cast<std::size_t>(address - begin);

            for(int classIndex = ClassCount - 1; classIndex >= 0; --classIndex)
            {
                const PoolClass& poolClass = region.classes[static_cast<std::size_t>(classIndex)];

                if(poolClass.blockCount != 0 && offset >= poolClass.firstByte)
                {
                    const std::size_t classSize = ClassSizes[static_cast<std::size_t>(classIndex)];

                    if((offset - poolClass.firstByte) % classSize != 0)
                    {
                        invalidFrees.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }

                    PushTo(region, classIndex, static_cast<std::uint32_t>((offset - poolClass.firstByte) / classSize), block);
                    break;
                }
            }

            LeaveRegion(region);
            return true;
        }

        return false;
    }

    // Main thread. Frees a region no longer active once nothing can reach it; never waits. Returns whether the slot is free.
    bool TryReclaim(Region& region) noexcept
    {
        if(&region == activeRegion.load(std::memory_order_acquire))
        {
            return false;
        }

        if(!region.withdrawnMemory)
        {
            char* const begin = region.begin.load(std::memory_order_seq_cst);

            if(!begin)
            {
                return true;
            }

            if(region.outstandingBlocks.load(std::memory_order_seq_cst) != 0)
            {
                return false;
            }

            region.withdrawnMemory = begin;
            region.begin.store(nullptr, std::memory_order_seq_cst);
        }

        if(region.callers.load(std::memory_order_seq_cst) != 0)
        {
            return false;
        }

        VirtualFree(region.withdrawnMemory, 0, MEM_RELEASE);
        region.withdrawnMemory = nullptr;
        region.end.store(nullptr, std::memory_order_release);

        for(PoolClass& poolClass : region.classes)
        {
            poolClass.next.reset();
            poolClass.state.reset();
            poolClass.blockCount = 0;
        }

        regionsReclaimed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void ReclaimAll() noexcept
    {
        for(Region& region : regions)
        {
            TryReclaim(region);
        }
    }
}

std::size_t ClassSize(int classIndex) noexcept
{
    return ClassSizes[static_cast<std::size_t>(classIndex)];
}

void* Allocate(std::size_t size, std::size_t alignment)
{
    if(profile)
    {
        const int classIndex = ClassForSize(size);

        if(classIndex < ClassCount)
        {
            ++(*profile)[static_cast<std::size_t>(classIndex)];
        }
    }

    const std::size_t heapAlignment = std::max(alignment, PoolAlignment);

    if(!insideMix)
    {
        return _aligned_malloc(std::max(size, MinimumHeapBlock), heapAlignment);
    }

    Region* const region = activeRegion.load(std::memory_order_acquire);
    const char* const begin = region ? region->begin.load(std::memory_order_acquire) : nullptr;

    if(begin && alignment <= PoolAlignment && EnterRegion(*region, begin))
    {
        const int exactClass = ClassForSize(size);

        // A larger class if this one is empty: wasteful, but still no heap, and counted as the warning before exhaustion.
        for(int classIndex = exactClass; classIndex < ClassCount; ++classIndex)
        {
            if(region->classes[static_cast<std::size_t>(classIndex)].blockCount == 0)
            {
                continue;
            }

            if(void* const block = PopFrom(*region, classIndex))
            {
                LeaveRegion(*region);

                if(classIndex != exactClass)
                {
                    largerClassAllocations.fetch_add(1, std::memory_order_relaxed);
                }

                return block;
            }
        }

        LeaveRegion(*region);
    }

    // The emergency path, and this allocator's one real-time compromise: never null, because the plugin does not survive every failed
    // effect (see the header). Always counted, and reported by the runtime on the main thread.
    mixerHeapAllocations.fetch_add(1, std::memory_order_relaxed);

    if(size > largestHeapRequest.load(std::memory_order_relaxed))
    {
        largestHeapRequest.store(size, std::memory_order_relaxed);
    }

    return _aligned_malloc(std::max(size, MinimumHeapBlock), heapAlignment);
}

void Free(void* block)
{
    if(!block || FreeIntoPool(block))
    {
        return;
    }

    if(!insideMix)
    {
        _aligned_free(block);
        return;
    }

    mixerHeapFrees.fetch_add(1, std::memory_order_relaxed);
    void* head = deferredHeapBlocks.load(std::memory_order_relaxed);

    do
    {
        *static_cast<void**>(block) = head;
    }
    while(!deferredHeapBlocks.compare_exchange_weak(head, block, std::memory_order_release, std::memory_order_relaxed));
}

void EnterMix() noexcept
{
    insideMix = true;
}

void LeaveMix() noexcept
{
    insideMix = false;
}

void BeginProfile() noexcept
{
    profileCounts = {};
    profile = &profileCounts;
}

ClassCounts EndProfile() noexcept
{
    profile = nullptr;
    return profileCounts;
}

bool Arm(const ClassCounts& blocks, bool fillFreedBlocks)
{
    Region* slot = nullptr;
    insideMix = false;
    ReclaimAll();

    for(Region& region : regions)
    {
        if(region.begin.load(std::memory_order_acquire) == nullptr && !region.withdrawnMemory)
        {
            slot = &region;
            break;
        }
    }

    if(!slot || activeRegion.load(std::memory_order_acquire))
    {
        return false;
    }

    std::size_t totalBytes = 0;

    for(int classIndex = 0; classIndex < ClassCount; ++classIndex)
    {
        totalBytes += blocks[static_cast<std::size_t>(classIndex)] * ClassSizes[static_cast<std::size_t>(classIndex)];
    }

    if(totalBytes == 0)
    {
        return false;
    }

    auto* const memory = static_cast<char*>(VirtualAlloc(nullptr, totalBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

    if(!memory)
    {
        return false;
    }

    // Touched now: a first touch on the mixer would be a page fault, which can wait on the kernel's memory manager.
    std::memset(memory, 0, totalBytes);
    std::size_t offset = 0;

    for(int classIndex = 0; classIndex < ClassCount; ++classIndex)
    {
        PoolClass& poolClass = slot->classes[static_cast<std::size_t>(classIndex)];
        const std::size_t count = blocks[static_cast<std::size_t>(classIndex)];
        poolClass.firstByte = offset;
        poolClass.blockCount = count;
        poolClass.used.store(0, std::memory_order_relaxed);
        poolClass.peak.store(0, std::memory_order_relaxed);
        poolClass.next.reset(count ? new std::atomic<std::uint32_t>[count] : nullptr);
        poolClass.state.reset(count ? new std::atomic<unsigned char>[count] : nullptr);

        for(std::size_t index = 0; index < count; ++index)
        {
            // Lowest index on top, each pointing to the next; the last ends the list.
            poolClass.next[index].store(index + 1 < count ? static_cast<std::uint32_t>(index + 2) : EmptyList, std::memory_order_relaxed);
            poolClass.state[index].store(BlockFree, std::memory_order_relaxed);
        }

        poolClass.head.store(count ? 1 : EmptyList, std::memory_order_relaxed);
        offset += count * ClassSizes[static_cast<std::size_t>(classIndex)];
    }

    slot->capacityBytes = totalBytes;
    slot->fillFreed = fillFreedBlocks;
    slot->outstandingBlocks.store(0, std::memory_order_relaxed);
    slot->usedBytes.store(0, std::memory_order_relaxed);
    poolAllocations = 0;
    poolFrees = 0;
    mixerHeapAllocations = 0;
    mixerHeapFrees = 0;
    largerClassAllocations = 0;
    largestHeapRequest = 0;
    doubleFrees = 0;
    invalidFrees = 0;
    peakBytes = 0;
    slot->end.store(memory + totalBytes, std::memory_order_release);
    slot->begin.store(memory, std::memory_order_seq_cst);
    activeRegion.store(slot, std::memory_order_release);
    return true;
}

void Drain() noexcept
{
    void* block = deferredHeapBlocks.exchange(nullptr, std::memory_order_acquire);

    while(block)
    {
        void* const next = *static_cast<void**>(block);
        _aligned_free(block);
        block = next;
    }

    ReclaimAll();
}

void Disarm() noexcept
{
    insideMix = false;
    Region* const region = activeRegion.exchange(nullptr, std::memory_order_acq_rel);
    Drain();

    if(region && !TryReclaim(*region))
    {
        regionsRetained.fetch_add(1, std::memory_order_relaxed);
    }
}

SteamAudioMixerPoolStats GetStats() noexcept
{
    SteamAudioMixerPoolStats stats;
    const Region* const region = activeRegion.load(std::memory_order_acquire);
    stats.armed = region != nullptr;

    if(region)
    {
        stats.capacityBytes = region->capacityBytes;
        stats.outstandingBlocks = region->outstandingBlocks.load(std::memory_order_relaxed);
        stats.usedBytes = region->usedBytes.load(std::memory_order_relaxed);
    }

    for(const Region& each : regions)
    {
        stats.regionsHeld += each.begin.load(std::memory_order_acquire) || each.withdrawnMemory ? 1 : 0;
    }

    stats.peakBytes = peakBytes.load(std::memory_order_relaxed);
    stats.poolAllocations = poolAllocations.load(std::memory_order_relaxed);
    stats.poolFrees = poolFrees.load(std::memory_order_relaxed);
    stats.mixerHeapAllocations = mixerHeapAllocations.load(std::memory_order_relaxed);
    stats.mixerHeapFrees = mixerHeapFrees.load(std::memory_order_relaxed);
    stats.largerClassAllocations = largerClassAllocations.load(std::memory_order_relaxed);
    stats.largestHeapRequest = largestHeapRequest.load(std::memory_order_relaxed);
    stats.doubleFrees = doubleFrees.load(std::memory_order_relaxed);
    stats.invalidFrees = invalidFrees.load(std::memory_order_relaxed);
    stats.regionsRetained = regionsRetained.load(std::memory_order_relaxed);
    stats.regionsReclaimed = regionsReclaimed.load(std::memory_order_relaxed);
    return stats;
}
}
