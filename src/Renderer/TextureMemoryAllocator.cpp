#include "Renderer/TextureMemoryAllocator.h"

#include "Core/Log.h"
#include "Renderer/ResourceStateTransaction.h"

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
    // {A4E3C1B2-5D6F-4E7A-8B9C-0D1E2F3A4B5C}
    constexpr GUID PlacedTextureGuid{0xa4e3c1b2, 0x5d6f, 0x4e7a, {0x8b, 0x9c, 0x0d, 0x1e, 0x2f, 0x3a, 0x4b, 0x5c}};
    constexpr std::uint64_t PageBytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    constexpr D3D12_RESOURCE_FLAGS RenderableFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    // The textures whose placed memory starts with undefined compression metadata: DiscardResource on activation.
    constexpr D3D12_RESOURCE_FLAGS DiscardedFlags = RenderableFlags | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    constexpr D3D12_RESOURCE_FLAGS AcceptedTextureFlags = DiscardedFlags | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    thread_local std::uint32_t t_activatedPlacementScopes = 0;
    thread_local std::uint32_t t_activatedBufferScopes = 0;
    // PlaceRenderThreadTargets: set once on the renderer's thread.
    thread_local bool t_renderThreadTargets = false;
    thread_local std::uint32_t t_committedTargetScopes = 0;
    // The zero fill's source: a committed (zeroed) buffer copied as often as the filled buffer needs.
    constexpr std::uint64_t ZeroBufferBytes = 16ull * 1024ull * 1024ull;

    // A Debug A/B switch: "0" turns the feature off, anything else (or nothing) leaves it on. Release: always on.
    bool DebugSwitchEnabled(const char* name)
    {
#ifdef NDEBUG
        (void)name;
        return true;
#else
        const char* value = std::getenv(name);
        return !(value && value[0] == '0');
#endif
    }

    bool RenderThreadTargetsEnabled()
    {
        static const bool enabled = DebugSwitchEnabled("ENGINE_TEXTURE_HEAP_POOL_TARGETS");
        return enabled;
    }

    std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    std::string FormatMebibytes(std::uint64_t bytes)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        return text;
    }

    using TextureMemoryAllocator::HeapKind;
    using TextureMemoryAllocator::HeapKindCount;

    bool EagerReclaimEnabled()
    {
        static const bool enabled = DebugSwitchEnabled("ENGINE_TEXTURE_HEAP_EAGER_RECLAIM");
        return enabled;
    }

    bool UnifiedHeapsEnabled()
    {
        static const bool enabled = DebugSwitchEnabled("ENGINE_TEXTURE_HEAP_UNIFIED");
        return enabled;
    }

    struct Heap
    {
        ComPtr<ID3D12Heap> heap;
        HeapKind kind = HeapKind::Textures;
        std::uint64_t capacity = 0;
        bool dedicated = false;
        // Free ranges by offset (coalesced) and the same ranges by size, for best fit.
        std::map<std::uint64_t, std::uint64_t> freeByOffset;
        std::multimap<std::uint64_t, std::uint64_t> freeBySize;
        std::uint32_t liveRanges = 0;
        std::uint32_t pendingRanges = 0;

        bool IsUnused() const
        {
            return liveRanges == 0 && pendingRanges == 0;
        }

        void EraseFreeBySize(std::uint64_t size, std::uint64_t offset)
        {
            auto [first, last] = freeBySize.equal_range(size);

            for(auto entry = first; entry != last; ++entry)
            {
                if(entry->second == offset)
                {
                    freeBySize.erase(entry);
                    return;
                }
            }
        }

        void TakeFree(std::uint64_t offset, std::uint64_t size, std::uint64_t taken)
        {
            freeByOffset.erase(offset);
            EraseFreeBySize(size, offset);

            if(size > taken)
            {
                freeByOffset.emplace(offset + taken, size - taken);
                freeBySize.emplace(size - taken, offset + taken);
            }
        }

        void ReturnFree(std::uint64_t offset, std::uint64_t size)
        {
            auto next = freeByOffset.lower_bound(offset);

            if(next != freeByOffset.end() && offset + size == next->first)
            {
                EraseFreeBySize(next->second, next->first);
                size += next->second;
                next = freeByOffset.erase(next);
            }

            if(next != freeByOffset.begin())
            {
                const auto previous = std::prev(next);

                if(previous->first + previous->second == offset)
                {
                    EraseFreeBySize(previous->second, previous->first);
                    offset = previous->first;
                    size += previous->second;
                    freeByOffset.erase(previous);
                }
            }

            freeByOffset.emplace(offset, size);
            freeBySize.emplace(size, offset);
        }
    };

    struct LiveAllocation
    {
        TextureMemoryAllocator::Category category = TextureMemoryAllocator::Category::SampledTextures;
        Heap* heap = nullptr;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        ID3D12Resource* resource = nullptr;
    };

    struct PendingRange
    {
        Heap* heap = nullptr;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        bool stamped = false;
        std::uint64_t submissionLifetime = 0;
        std::uint64_t reusableAfterFence = 0;
    };

    // A render-thread target that the allocator activates: not yet (recording 0), or in that frame recording, which
    // decides at the next BeginFrameRecording whether it counted (submitted) or not (discarded: activated again).
    struct PendingActivation
    {
        ID3D12Resource* resource = nullptr;
        // The state every subresource is in before the activation (creation state; a discard rolls it back to it).
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        std::uint64_t recording = 0;
    };

    // A resource the allocator's own activation commands reference: kept alive until the frame queue has completed the
    // recording that holds them, because an owner may release it in that same recording before using it (a buffer grown
    // twice in one frame) and a command list must not reference a destroyed resource.
    struct KeptAlive
    {
        ComPtr<ID3D12Resource> resource;
        std::uint64_t recording = 0;
        bool stamped = false;
        std::uint64_t submissionLifetime = 0;
        std::uint64_t releaseAfterFence = 0;
    };

    struct AllocatorState
    {
        std::mutex mutex;
        ID3D12Device* device = nullptr;
        bool deviceMismatchReported = false;
        // Heaps are erased only by Trim (or after it, once idle) and only when unused, so allocations may hold pointers.
        std::vector<std::unique_ptr<Heap>> heaps;
        std::vector<PendingRange> pendingRanges;
        std::unordered_map<std::uint64_t, LiveAllocation> liveAllocations;
        // Render-thread targets by allocation identifier; an entry goes with its texture.
        std::unordered_map<std::uint64_t, PendingActivation> pendingActivations;
        // The open frame recording (BeginFrameRecording to EndFrameRecording) and the thread that records it.
        ID3D12GraphicsCommandList* frameList = nullptr;
        // The frame command list of the latest Begin (kept after End: its recording resolves at submission or discard).
        ID3D12GraphicsCommandList* lastFrameList = nullptr;
        std::thread::id frameThread;
        // Created at the first zero fill; released by Trim.
        ComPtr<ID3D12Resource> zeroBuffer;
        std::vector<KeptAlive> keptAlive;
        std::uint64_t nextIdentifier = 1;
        // Set by Trim: the GPU is idle, so released ranges are free at once and emptied heaps go.
        bool idle = false;
        // Allocator P6b: decided at the first placement from the device's resource heap tier.
        bool heapTierKnown = false;
        bool unifiedHeaps = false;
        // Allocator P6a: the live ReclaimScope's queues (the frame queue first) and the thread it was created on.
        bool reclaimActive = false;
        std::vector<TextureMemoryAllocator::TrackedQueue> reclaimQueues;
        std::thread::id reclaimThread;
        std::uint32_t heapsCreatedWhilePending = 0;
        std::uint64_t pendingBytesAtHeapCreations = 0;
        std::array<D3D12_RESIDENCY_PRIORITY, HeapKindCount> priorities{D3D12_RESIDENCY_PRIORITY_NORMAL,
            D3D12_RESIDENCY_PRIORITY_NORMAL, D3D12_RESIDENCY_PRIORITY_NORMAL, D3D12_RESIDENCY_PRIORITY_NORMAL};
        std::array<std::uint64_t, TextureMemoryAllocator::CategoryCount> budgets{};
        std::array<std::uint64_t, TextureMemoryAllocator::CategoryCount> liveBytes{};
        std::array<std::uint64_t, TextureMemoryAllocator::CategoryCount> peakLiveBytes{};
        std::array<std::uint64_t, TextureMemoryAllocator::CategoryCount> liveTextures{};
        std::uint64_t reservedBytes = 0;
        std::uint64_t peakReservedBytes = 0;
        std::uint64_t totalLiveBytes = 0;
        std::uint64_t peakTotalLiveBytes = 0;
        std::uint64_t texturesPlaced = 0;
        std::uint64_t texturesPlacedInReusedMemory = 0;

        std::uint64_t PendingBytes() const
        {
            std::uint64_t bytes = 0;

            for(const auto& range : pendingRanges)
            {
                bytes += range.size;
            }

            return bytes;
        }

        void ReleaseUnusedHeaps()
        {
            std::erase_if(heaps, [&](const std::unique_ptr<Heap>& heap)
            {
                if(!heap->IsUnused())
                {
                    return false;
                }

                reservedBytes -= heap->capacity;
                return true;
            });
        }

        TextureMemoryAllocator::Statistics Gather() const
        {
            TextureMemoryAllocator::Statistics statistics;
            statistics.heaps = static_cast<std::uint32_t>(heaps.size());
            statistics.reservedBytes = reservedBytes;
            statistics.peakReservedBytes = peakReservedBytes;
            statistics.liveBytes = totalLiveBytes;
            statistics.peakLiveBytes = peakTotalLiveBytes;
            statistics.texturesPlaced = texturesPlaced;
            statistics.texturesPlacedInReusedMemory = texturesPlacedInReusedMemory;
            statistics.heapsCreatedWhilePending = heapsCreatedWhilePending;
            statistics.pendingBytesAtHeapCreations = pendingBytesAtHeapCreations;

            for(const auto& heap : heaps)
            {
                auto& kind = statistics.kinds[static_cast<std::size_t>(heap->kind)];
                ++kind.heaps;
                kind.reservedBytes += heap->capacity;

                for(const auto& [offset, size] : heap->freeByOffset)
                {
                    statistics.freeBytes += size;
                    statistics.largestFreeRange = (std::max)(statistics.largestFreeRange, size);
                    kind.freeBytes += size;
                    kind.largestFreeRange = (std::max)(kind.largestFreeRange, size);
                }
            }

            statistics.pendingBytes = PendingBytes();

            for(std::size_t index = 0; index < TextureMemoryAllocator::CategoryCount; ++index)
            {
                auto& category = statistics.categories[index];
                category.liveBytes = liveBytes[index];
                category.peakLiveBytes = peakLiveBytes[index];
                category.liveTextures = liveTextures[index];
                category.budgetBytes = budgets[index];
            }

            return statistics;
        }
    };

    // Allocator P6: each heap kind's reservation and free memory, and the heaps created while released memory was pending.
    std::string DescribeKinds(const TextureMemoryAllocator::Statistics& statistics)
    {
        constexpr std::array<const char*, HeapKindCount> names{"textures", "renderable", "buffers", "unified"};
        std::string text = "; heap kinds:";

        for(std::size_t index = 0; index < HeapKindCount; ++index)
        {
            const auto& kind = statistics.kinds[index];

            if(kind.heaps != 0)
            {
                text += std::string(" ") + names[index] + " " + FormatMebibytes(kind.reservedBytes) + " in " +
                    std::to_string(kind.heaps) + " (free " + FormatMebibytes(kind.freeBytes) + ", largest " +
                    FormatMebibytes(kind.largestFreeRange) + ");";
            }
        }

        return text + " " + std::to_string(statistics.heapsCreatedWhilePending) + " heaps created while " +
            FormatMebibytes(statistics.pendingBytesAtHeapCreations) + " was pending";
    }

    std::string Describe(const TextureMemoryAllocator::Statistics& statistics)
    {
        using TextureMemoryAllocator::Category;
        const double fragmentation = statistics.freeBytes == 0 ? 0.0 :
            100.0 * (1.0 - static_cast<double>(statistics.largestFreeRange) / static_cast<double>(statistics.freeBytes));
        char percent[16];
        std::snprintf(percent, sizeof(percent), "%.0f%%", fragmentation);
        const auto& sampled = statistics.categories[static_cast<std::size_t>(Category::SampledTextures)];
        const auto& renderable = statistics.categories[static_cast<std::size_t>(Category::RenderableTextures)];
        const auto& buffers = statistics.categories[static_cast<std::size_t>(Category::Buffers)];
        const auto& unordered = statistics.categories[static_cast<std::size_t>(Category::UnorderedTextures)];

        return std::to_string(statistics.heaps) + " heaps, " + FormatMebibytes(statistics.reservedBytes) + " reserved (peak " +
            FormatMebibytes(statistics.peakReservedBytes) + "), " + FormatMebibytes(statistics.liveBytes) + " in use (peak " +
            FormatMebibytes(statistics.peakLiveBytes) + "), free " + FormatMebibytes(statistics.freeBytes) + " (largest " +
            FormatMebibytes(statistics.largestFreeRange) + ", fragmentation " + percent + "), pending " +
            FormatMebibytes(statistics.pendingBytes) + "; sampled " + FormatMebibytes(sampled.liveBytes) + " in " +
            std::to_string(sampled.liveTextures) + " textures, renderable " + FormatMebibytes(renderable.liveBytes) +
            " in " + std::to_string(renderable.liveTextures) + ", unordered " + FormatMebibytes(unordered.liveBytes) +
            " in " + std::to_string(unordered.liveTextures) + ", buffers " + FormatMebibytes(buffers.liveBytes) +
            " in " + std::to_string(buffers.liveTextures) + "; " + std::to_string(statistics.texturesPlaced) +
            " textures placed, " + std::to_string(statistics.texturesPlacedInReusedMemory) + " of them in an existing heap" +
            DescribeKinds(statistics);
    }

    std::shared_ptr<AllocatorState>& GetState()
    {
        static std::shared_ptr<AllocatorState> state = std::make_shared<AllocatorState>();
        return state;
    }

    // Under the allocator's lock, inside a ReclaimScope: whether every queue after the first (the frame queue) has completed
    // all of its submissions. Those queues' ranges carry no fence stamps, so only an idle queue counts as done.
    bool OtherReclaimQueuesIdle(const AllocatorState& state)
    {
        for(std::size_t index = 1; index < state.reclaimQueues.size(); ++index)
        {
            const TextureMemoryAllocator::TrackedQueue& queue = state.reclaimQueues[index];
            const std::uint64_t completed = queue.fence->GetCompletedValue();

            if(completed == UINT64_MAX || completed < queue.lastSubmittedFence())
            {
                return false;
            }
        }

        return true;
    }

    // Under the allocator's lock, inside a ReclaimScope: whether a range stamped with the frame queue's values (as Collect
    // stamps) is complete on every tracked queue, by fresh completed values.
    bool ReclaimableNow(const AllocatorState& state, const PendingRange& range)
    {
        const TextureMemoryAllocator::TrackedQueue& frameQueue = state.reclaimQueues.front();
        const std::uint64_t completed = frameQueue.fence->GetCompletedValue();

        if(completed == UINT64_MAX)
        {
            return false;
        }

        const bool frameQueueDone = range.submissionLifetime != frameQueue.submissionLifetime ||
            completed >= range.reusableAfterFence;
        return frameQueueDone && OtherReclaimQueuesIdle(state);
    }

    // Owned by the texture's private-data store. The D3D12 runtime releases it when the texture is destroyed, whoever
    // dropped the last reference, which returns the texture's range to the allocator.
    class PlacedTextureSentinel final : public IUnknown
    {
    public:
        PlacedTextureSentinel(std::shared_ptr<AllocatorState> state, std::uint64_t identifier, bool discarded)
            : m_state(std::move(state)), m_identifier(identifier), m_discarded(discarded)
        {
        }

        std::uint64_t GetIdentifier() const
        {
            return m_identifier;
        }

        // Render-target, depth or unordered-access: DiscardResource on activation.
        bool IsDiscardedOnActivation() const
        {
            return m_discarded;
        }

        // Under the allocator's lock. True until an activation was recorded, and again when the frame recording that held
        // it was discarded (R.1). recording: the frame recording the new activation is in, or 0 (final at once).
        bool BeginActivation(std::uint64_t recording)
        {
            if(m_activated && !(m_activationRecording != 0 && ResourceStateTransaction::IsDiscarded(m_activationRecording)))
            {
                return false;
            }

            m_activated = true;
            m_activationRecording = recording;
            return true;
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID identifier, void** object) override
        {
            if(object == nullptr)
            {
                return E_POINTER;
            }

            if(identifier == __uuidof(IUnknown))
            {
                *object = static_cast<IUnknown*>(this);
                AddRef();
                return S_OK;
            }

            *object = nullptr;
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return m_references.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG references = m_references.fetch_sub(1, std::memory_order_acq_rel) - 1;

            if(references == 0)
            {
                delete this;
            }

            return references;
        }

    private:
        ~PlacedTextureSentinel()
        {
            AllocatorState& state = *m_state;
            const std::lock_guard lock(state.mutex);
            const auto found = state.liveAllocations.find(m_identifier);

            if(found == state.liveAllocations.end())
            {
                return;
            }

            const LiveAllocation allocation = found->second;
            state.liveAllocations.erase(found);
            state.pendingActivations.erase(m_identifier);
            const auto category = static_cast<std::size_t>(allocation.category);
            state.liveBytes[category] -= allocation.size;
            --state.liveTextures[category];
            state.totalLiveBytes -= allocation.size;
            --allocation.heap->liveRanges;

            if(state.idle)
            {
                allocation.heap->ReturnFree(allocation.offset, allocation.size);
                state.ReleaseUnusedHeaps();
                return;
            }

            PendingRange range{allocation.heap, allocation.offset, allocation.size};

            // Allocator P6a: inside a ReclaimScope, on its thread and with no frame recording open or awaiting submission
            // (it may still use the texture), stamped now and free at once if every tracked queue has completed it.
            if(state.reclaimActive && state.reclaimThread == std::this_thread::get_id() && !state.frameList &&
                ResourceStateTransaction::RecordingFor(state.lastFrameList) == 0)
            {
                const TextureMemoryAllocator::TrackedQueue& frameQueue = state.reclaimQueues.front();
                range.stamped = true;
                range.submissionLifetime = frameQueue.submissionLifetime;
                range.reusableAfterFence = frameQueue.lastSubmittedFence();

                if(ReclaimableNow(state, range))
                {
                    allocation.heap->ReturnFree(allocation.offset, allocation.size);
                    return;
                }
            }

            ++allocation.heap->pendingRanges;
            state.pendingRanges.push_back(range);
        }

        std::atomic<ULONG> m_references{1};
        std::shared_ptr<AllocatorState> m_state;
        std::uint64_t m_identifier = 0;
        bool m_discarded = false;
        // Guarded by the allocator's lock.
        bool m_activated = false;
        std::uint64_t m_activationRecording = 0;
    };

    PlacedTextureSentinel* FindSentinel(ID3D12Resource* resource)
    {
        IUnknown* unknown = nullptr;
        UINT size = sizeof(unknown);

        if(!resource || FAILED(resource->GetPrivateData(PlacedTextureGuid, &size, &unknown)) || !unknown)
        {
            return nullptr;
        }

        // Only CreateTexture stores data under PlacedTextureGuid. The resource keeps the sentinel alive.
        unknown->Release();
        return static_cast<PlacedTextureSentinel*>(unknown);
    }

    HeapKind KindFor(TextureMemoryAllocator::Category category, bool unifiedHeaps)
    {
        if(unifiedHeaps)
        {
            return HeapKind::Unified;
        }

        switch(category)
        {
            case TextureMemoryAllocator::Category::RenderableTextures:
                return HeapKind::RenderableTextures;
            case TextureMemoryAllocator::Category::Buffers:
                return HeapKind::Buffers;
            default:
                return HeapKind::Textures;
        }
    }

    D3D12_HEAP_FLAGS HeapFlagsFor(HeapKind kind)
    {
        switch(kind)
        {
            case HeapKind::RenderableTextures:
                return D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
            case HeapKind::Buffers:
                return D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
            case HeapKind::Unified:
                return D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
            default:
                return D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
        }
    }

    std::uint64_t HeapBytesFor(HeapKind kind)
    {
        switch(kind)
        {
            case HeapKind::Buffers:
                return TextureMemoryAllocator::BufferHeapBytes;
            case HeapKind::Unified:
                return TextureMemoryAllocator::UnifiedHeapBytes;
            default:
                return TextureMemoryAllocator::HeapBytes;
        }
    }

    // Under the allocator's lock, once per device (allocator P6b): one heap kind for every resource on resource heap tier 2.
    void DecideHeapKinds(AllocatorState& state, ID3D12Device* device)
    {
        if(state.heapTierKnown)
        {
            return;
        }

        state.heapTierKnown = true;
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        const bool tier2 = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
            options.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2;
        state.unifiedHeaps = tier2 && UnifiedHeapsEnabled();
        Log::Info(state.unifiedHeaps ? "Texture memory: unified heaps (resource heap tier 2; " +
            FormatMebibytes(TextureMemoryAllocator::UnifiedHeapBytes) + " each)." :
            std::string("Texture memory: per-kind heaps (") + (tier2 ? "ENGINE_TEXTURE_HEAP_UNIFIED=0" :
                "resource heap tier 1") + ").");
    }

    void ApplyResidencyPriority(ID3D12Device* device, ID3D12Heap* heap, D3D12_RESIDENCY_PRIORITY priority)
    {
        ComPtr<ID3D12Device1> device1;

        if(priority == D3D12_RESIDENCY_PRIORITY_NORMAL || !device || FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))))
        {
            return;
        }

        ID3D12Pageable* pageable = heap;
        device1->SetResidencyPriority(1, &pageable, &priority);
    }

    // Places a resource of allocationBytes in a heap of the category's kind: best fit over the free ranges, or a new heap
    // (a dedicated one above the kind's heap size), with a sentinel that returns the range when the resource is destroyed.
    HRESULT PlaceResource(ID3D12Device* device, const D3D12_RESOURCE_DESC& description, D3D12_RESOURCE_STATES initialState,
        const D3D12_CLEAR_VALUE* optimizedClearValue, std::uint64_t allocationBytes, TextureMemoryAllocator::Category category,
        ID3D12Resource** resource)
    {
        using TextureMemoryAllocator::Category;
        const std::uint64_t size = AlignUp(allocationBytes, PageBytes);
        const bool discarded = description.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
            (description.Flags & DiscardedFlags) != 0;
        const std::shared_ptr<AllocatorState> stateOwner = GetState();
        AllocatorState& state = *stateOwner;
        Heap* chosenHeap = nullptr;
        std::uint64_t chosenOffset = 0;
        std::uint64_t identifier = 0;
        bool createdHeap = false;

        {
            const std::lock_guard lock(state.mutex);

            if(!state.device)
            {
                state.device = device;
            }

            if(state.device != device)
            {
                if(!state.deviceMismatchReported)
                {
                    state.deviceMismatchReported = true;
                    Log::Warning("Texture memory: a second device creates textures; they are committed resources.");
                }

                return E_NOTIMPL;
            }

            state.idle = false;
            DecideHeapKinds(state, device);
            const HeapKind kind = KindFor(category, state.unifiedHeaps);
            std::uint64_t chosenRangeSize = 0;

            for(const auto& heap : state.heaps)
            {
                if(heap->kind != kind)
                {
                    continue;
                }

                const auto candidate = heap->freeBySize.lower_bound(size);

                if(candidate != heap->freeBySize.end() && (!chosenHeap || candidate->first < chosenRangeSize))
                {
                    chosenHeap = heap.get();
                    chosenOffset = candidate->second;
                    chosenRangeSize = candidate->first;
                }
            }

            if(!chosenHeap)
            {
                const std::uint64_t heapBytes = HeapBytesFor(kind);
                const bool dedicated = size > heapBytes;
                const std::uint64_t pendingBytes = state.PendingBytes();

                if(pendingBytes != 0)
                {
                    ++state.heapsCreatedWhilePending;
                    state.pendingBytesAtHeapCreations += pendingBytes;
                }
                auto heap = std::make_unique<Heap>();
                D3D12_HEAP_DESC heapDescription{};
                heapDescription.SizeInBytes = dedicated ? size : heapBytes;
                heapDescription.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
                heapDescription.Alignment = PageBytes;
                heapDescription.Flags = HeapFlagsFor(kind);

                if(FAILED(device->CreateHeap(&heapDescription, IID_PPV_ARGS(&heap->heap))))
                {
                    return E_OUTOFMEMORY;
                }

                heap->heap->SetName(dedicated ? L"Texture memory dedicated heap" : L"Texture memory heap");
                ApplyResidencyPriority(device, heap->heap.Get(), state.priorities[static_cast<std::size_t>(kind)]);
                heap->kind = kind;
                heap->capacity = heapDescription.SizeInBytes;
                heap->dedicated = dedicated;
                heap->freeByOffset.emplace(0, heap->capacity);
                heap->freeBySize.emplace(heap->capacity, 0);
                state.reservedBytes += heap->capacity;
                state.peakReservedBytes = (std::max)(state.peakReservedBytes, state.reservedBytes);
                chosenHeap = heap.get();
                chosenOffset = 0;
                chosenRangeSize = heap->capacity;
                state.heaps.push_back(std::move(heap));
                createdHeap = true;
            }

            chosenHeap->TakeFree(chosenOffset, chosenRangeSize, size);
            ++chosenHeap->liveRanges;
            identifier = state.nextIdentifier++;
        }

        ComPtr<ID3D12Resource> placed;
        const HRESULT result = device->CreatePlacedResource(chosenHeap->heap.Get(), chosenOffset, &description, initialState,
            optimizedClearValue, IID_PPV_ARGS(&placed));
        auto* sentinel = SUCCEEDED(result) ? new PlacedTextureSentinel(stateOwner, identifier, discarded) : nullptr;
        bool attached = false;

        if(sentinel)
        {
            // Registered before attaching, so a release through the sentinel always finds its allocation.
            {
                const std::lock_guard lock(state.mutex);
                state.liveAllocations[identifier] = {category, chosenHeap, chosenOffset, size, placed.Get()};
            }

            attached = SUCCEEDED(placed->SetPrivateDataInterface(PlacedTextureGuid, sentinel));

            if(!attached)
            {
                const std::lock_guard lock(state.mutex);
                state.liveAllocations.erase(identifier);
            }

            // Once attached, the private-data store holds the sentinel's only remaining reference.
            sentinel->Release();
        }

        if(!attached)
        {
            // Never used by the GPU: the range is free again at once. The resource carries no sentinel, so releasing it
            // touches no allocator state.
            placed.Reset();
            const std::lock_guard lock(state.mutex);
            --chosenHeap->liveRanges;
            chosenHeap->ReturnFree(chosenOffset, size);
            return FAILED(result) ? result : E_FAIL;
        }

        {
            const std::lock_guard lock(state.mutex);
            const auto index = static_cast<std::size_t>(category);
            state.liveBytes[index] += size;
            state.peakLiveBytes[index] = (std::max)(state.peakLiveBytes[index], state.liveBytes[index]);
            ++state.liveTextures[index];
            state.totalLiveBytes += size;
            state.peakTotalLiveBytes = (std::max)(state.peakTotalLiveBytes, state.totalLiveBytes);
            ++state.texturesPlaced;
            state.texturesPlacedInReusedMemory += createdHeap ? 0 : 1;

            if(createdHeap)
            {
                Log::Info("Texture memory: created a heap of " + FormatMebibytes(chosenHeap->capacity) + "; " +
                    Describe(state.Gather()) + ".");
            }
        }

        *resource = placed.Detach();
        return S_OK;
    }

    // The activation's commands: the aliasing barrier that makes the texture its range's active resource (the previous
    // occupant finished on the GPU before the range was reused) and, for render-target, depth and unordered-access
    // textures, DiscardResource of every subresource in the state it requires, returning to currentState.
    void RecordActivationCommands(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES currentState, bool discarded)
    {
        D3D12_RESOURCE_BARRIER aliasing{};
        aliasing.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
        aliasing.Aliasing.pResourceBefore = nullptr;
        aliasing.Aliasing.pResourceAfter = resource;
        commands->ResourceBarrier(1, &aliasing);

        if(!discarded)
        {
            return;
        }

        // Placed render-target, depth and unordered-access textures start with undefined compression metadata.
        const D3D12_RESOURCE_FLAGS flags = resource->GetDesc().Flags;
        const D3D12_RESOURCE_STATES targetState = (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 ?
            D3D12_RESOURCE_STATE_DEPTH_WRITE : (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 ?
            D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_BARRIER transition{};
        transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        transition.Transition.pResource = resource;
        transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        transition.Transition.StateBefore = currentState;
        transition.Transition.StateAfter = targetState;

        if(currentState != targetState)
        {
            commands->ResourceBarrier(1, &transition);
        }

        commands->DiscardResource(resource, nullptr);

        if(currentState != targetState)
        {
            std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
            commands->ResourceBarrier(1, &transition);
        }
    }

    // Under the allocator's lock: zeroes a placed buffer that no command has used yet, so it starts as a committed buffer
    // does. A buffer is in COMMON until its first use, whatever state it was created in; it returns there.
    void RecordZeroFill(AllocatorState& state, ID3D12GraphicsCommandList* commands, ID3D12Resource* buffer)
    {
        if(!state.zeroBuffer)
        {
            D3D12_HEAP_PROPERTIES heapProperties{};
            heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC description{};
            description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            description.Width = ZeroBufferBytes;
            description.Height = 1;
            description.DepthOrArraySize = 1;
            description.MipLevels = 1;
            description.SampleDesc.Count = 1;
            description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            if(!state.device || FAILED(state.device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE,
                &description, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&state.zeroBuffer))))
            {
                state.zeroBuffer.Reset();
                Log::Error("Texture memory: the zero-fill source buffer could not be created; a placed buffer starts "
                    "with undefined contents.");
                return;
            }

            state.zeroBuffer->SetName(L"Texture memory zero-fill source");
        }

        D3D12_RESOURCE_BARRIER transition{};
        transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        transition.Transition.pResource = buffer;
        transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        transition.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        commands->ResourceBarrier(1, &transition);
        const std::uint64_t bytes = buffer->GetDesc().Width;

        for(std::uint64_t offset = 0; offset < bytes; offset += ZeroBufferBytes)
        {
            // The source is promoted from COMMON to COPY_SOURCE implicitly, and stays readable for the list.
            commands->CopyBufferRegion(buffer, offset, state.zeroBuffer.Get(), 0, (std::min)(ZeroBufferBytes, bytes - offset));
        }

        std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
        commands->ResourceBarrier(1, &transition);
    }

    // Under the allocator's lock: activates a render-thread target or buffer on the open frame recording, as a pending
    // activation of that recording. Buffers are zero-filled too, except acceleration-structure memory (it never leaves its
    // state, and builds write it in full).
    // AIR async compute: activations recorded on frame recordings (GetFrameRecordingActivationCount).
    std::atomic<std::uint64_t> g_frameRecordingActivations{0};

    void ActivateOnFrameRecording(AllocatorState& state, PlacedTextureSentinel& sentinel, PendingActivation& activation)
    {
        const std::uint64_t recording = ResourceStateTransaction::RecordingFor(state.frameList);

        if(!sentinel.BeginActivation(recording))
        {
            return;
        }

        g_frameRecordingActivations.fetch_add(1, std::memory_order_relaxed);

        activation.recording = recording;
        state.keptAlive.push_back({activation.resource, recording});
        RecordActivationCommands(state.frameList, activation.resource, activation.state, sentinel.IsDiscardedOnActivation());

        if(activation.resource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            activation.state != D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
        {
            RecordZeroFill(state, state.frameList, activation.resource);
        }
    }
    // Under the allocator's lock: Collect's work. Kept-alive resources due for release are moved to `released`, which the
    // caller destroys after the lock.
    void CollectLocked(AllocatorState& state, std::uint64_t submissionLifetime, std::uint64_t lastSubmittedFence,
        std::uint64_t completedFence, std::vector<ComPtr<ID3D12Resource>>& released)
    {
        const bool completedFenceKnown = completedFence != UINT64_MAX;
        // The frame recording's transaction stays open from Begin until it is submitted or discarded (after End too).
        const std::uint64_t openRecording = ResourceStateTransaction::RecordingFor(state.lastFrameList);

        std::erase_if(state.keptAlive, [&](KeptAlive& kept)
        {
            // Stamped once its recording is resolved: submitted (its fence) or discarded (never runs).
            if(kept.recording == openRecording && openRecording != 0)
            {
                return false;
            }

            if(!kept.stamped)
            {
                kept.stamped = true;
                kept.submissionLifetime = submissionLifetime;
                kept.releaseAfterFence = lastSubmittedFence;
            }

            if(kept.submissionLifetime == submissionLifetime &&
                !(completedFenceKnown && completedFence >= kept.releaseAfterFence))
            {
                return false;
            }

            released.push_back(std::move(kept.resource));
            return true;
        });

        if(state.pendingRanges.empty())
        {
            return;
        }

        const auto reusable = [&](PendingRange& range)
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

            range.heap->ReturnFree(range.offset, range.size);
            --range.heap->pendingRanges;
            return true;
        };

        std::erase_if(state.pendingRanges, reusable);
    }

    // Under the allocator's lock, inside a ReclaimScope (allocator P6a): Collect against fresh completed values of the
    // frame queue, when every other tracked queue has completed all of its submissions (otherwise nothing changes).
    void ReclaimAtOnce(AllocatorState& state, std::vector<ComPtr<ID3D12Resource>>& released)
    {
        if(!OtherReclaimQueuesIdle(state))
        {
            return;
        }

        const TextureMemoryAllocator::TrackedQueue& frameQueue = state.reclaimQueues.front();
        CollectLocked(state, frameQueue.submissionLifetime, frameQueue.lastSubmittedFence(),
            frameQueue.fence->GetCompletedValue(), released);
    }
} // namespace

TextureMemoryAllocator::CommittedTargetScope::CommittedTargetScope()
{
    ++t_committedTargetScopes;
}

TextureMemoryAllocator::CommittedTargetScope::~CommittedTargetScope()
{
    --t_committedTargetScopes;
}

TextureMemoryAllocator::ActivatedBufferScope::ActivatedBufferScope()
{
    ++t_activatedBufferScopes;
}

TextureMemoryAllocator::ActivatedBufferScope::~ActivatedBufferScope()
{
    --t_activatedBufferScopes;
}

void TextureMemoryAllocator::PlaceRenderThreadTargets()
{
    t_renderThreadTargets = true;
}

bool TextureMemoryAllocator::BufferPoolEnabled()
{
    static const bool enabled = DebugSwitchEnabled("ENGINE_BUFFER_HEAP_POOL");
    return enabled;
}

std::uint64_t TextureMemoryAllocator::GetFrameRecordingActivationCount()
{
    return g_frameRecordingActivations.load(std::memory_order_relaxed);
}

void TextureMemoryAllocator::BeginFrameRecording(ID3D12GraphicsCommandList* commandList)
{
    AllocatorState& state = *GetState();
    const std::lock_guard lock(state.mutex);
    state.frameList = commandList;
    state.lastFrameList = commandList ? commandList : state.lastFrameList;
    state.frameThread = std::this_thread::get_id();

    if(!commandList)
    {
        return;
    }

    // Every earlier recording is submitted or discarded by now: a submitted activation is final, a discarded one is
    // recorded again here, with the targets created since the last recording.
    for(auto entry = state.pendingActivations.begin(); entry != state.pendingActivations.end();)
    {
        PendingActivation& activation = entry->second;

        if(activation.recording != 0 && !ResourceStateTransaction::IsDiscarded(activation.recording))
        {
            entry = state.pendingActivations.erase(entry);
            continue;
        }

        PlacedTextureSentinel* sentinel = FindSentinel(activation.resource);

        if(sentinel)
        {
            ActivateOnFrameRecording(state, *sentinel, activation);
        }

        ++entry;
    }
}

void TextureMemoryAllocator::SwitchFrameRecordingList(ID3D12GraphicsCommandList* commandList)
{
    AllocatorState& state = *GetState();
    const std::lock_guard lock(state.mutex);

    if(state.frameList != nullptr && commandList != nullptr)
    {
        state.frameList = commandList;
        state.lastFrameList = commandList;
    }
}

void TextureMemoryAllocator::EndFrameRecording()
{
    AllocatorState& state = *GetState();
    const std::lock_guard lock(state.mutex);
    state.frameList = nullptr;
}

TextureMemoryAllocator::ActivatedPlacementScope::ActivatedPlacementScope()
{
    ++t_activatedPlacementScopes;
}

TextureMemoryAllocator::ActivatedPlacementScope::~ActivatedPlacementScope()
{
    --t_activatedPlacementScopes;
}

bool TextureMemoryAllocator::IsEnabled()
{
#ifdef NDEBUG
    return true;
#else
    static const bool enabled = []
    {
        const char* value = std::getenv("ENGINE_TEXTURE_HEAP_POOL");
        return !(value && value[0] == '0');
    }();

    return enabled;
#endif
}

HRESULT TextureMemoryAllocator::CreateTexture(ID3D12Device* device, const D3D12_RESOURCE_DESC& description,
    D3D12_RESOURCE_STATES initialState, ID3D12Resource** resource, const D3D12_CLEAR_VALUE* optimizedClearValue)
{
    const bool renderable = (description.Flags & RenderableFlags) != 0;
    const bool target = (description.Flags & DiscardedFlags) != 0;
    const bool otherFlags = (description.Flags & ~AcceptedTextureFlags) != 0;

    // Render-target, depth and unordered-access textures need DiscardResource before first use: an owner that records
    // activation opts them in (ActivatedPlacementScope), or the renderer's thread, whose targets the allocator activates.
    const bool activated = t_activatedPlacementScopes > 0;
    const bool renderThreadTarget = target && !activated && t_renderThreadTargets && t_committedTargetScopes == 0 &&
        RenderThreadTargetsEnabled();
    const bool eligible = IsEnabled() && device && resource && description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        description.SampleDesc.Count == 1 && !otherFlags && (!target || activated || renderThreadTarget);

    if(!eligible)
    {
        return E_NOTIMPL;
    }

    const D3D12_RESOURCE_ALLOCATION_INFO information = device->GetResourceAllocationInfo(0, 1, &description);
    // Owners that activate their textures, and the render thread's targets, place small textures too, so none of their
    // memory is freed to the OS.
    const bool sizeAccepted = information.SizeInBytes != UINT64_MAX &&
        (information.SizeInBytes >= MinimumPooledBytes || activated || renderThreadTarget) &&
        information.Alignment <= PageBytes;

    if(!sizeAccepted)
    {
        return E_NOTIMPL;
    }

    const Category category = renderable ? Category::RenderableTextures :
        target ? Category::UnorderedTextures : Category::SampledTextures;
    const HRESULT result = PlaceResource(device, description, initialState, optimizedClearValue, information.SizeInBytes,
        category, resource);

    if(FAILED(result) || !renderThreadTarget)
    {
        return result;
    }

    // Activated by the allocator: at once on the open frame recording (this thread records it, so the activation precedes
    // every use of the texture in it), otherwise at the start of the next one.
    PlacedTextureSentinel* sentinel = FindSentinel(*resource);
    AllocatorState& state = *GetState();
    const std::lock_guard lock(state.mutex);

    if(sentinel)
    {
        PendingActivation& activation = state.pendingActivations[sentinel->GetIdentifier()];
        activation.resource = *resource;
        activation.state = initialState;

        if(state.frameList && state.frameThread == std::this_thread::get_id())
        {
            ActivateOnFrameRecording(state, *sentinel, activation);
        }
    }

    return result;
}

HRESULT TextureMemoryAllocator::CreateBuffer(ID3D12Device* device, const D3D12_RESOURCE_DESC& description,
    D3D12_RESOURCE_STATES initialState, ID3D12Resource** resource)
{
    // Owners that activate their buffers (the environment; uploads and acceleration-structure builds), or the render thread,
    // whose buffers the allocator activates and zero-fills.
    const bool activated = t_activatedPlacementScopes > 0 || (t_activatedBufferScopes > 0 && BufferPoolEnabled());
    const bool renderThreadBuffer = !activated && t_renderThreadTargets && t_committedTargetScopes == 0 &&
        BufferPoolEnabled();
    const bool eligible = IsEnabled() && device && resource && description.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        (activated || renderThreadBuffer);

    if(!eligible)
    {
        return E_NOTIMPL;
    }

    const D3D12_RESOURCE_ALLOCATION_INFO information = device->GetResourceAllocationInfo(0, 1, &description);

    if(information.SizeInBytes == UINT64_MAX || information.Alignment > PageBytes)
    {
        return E_NOTIMPL;
    }

    const HRESULT result = PlaceResource(device, description, initialState, nullptr, information.SizeInBytes,
        Category::Buffers, resource);

    if(FAILED(result) || !renderThreadBuffer)
    {
        return result;
    }

    // As render-thread targets (CreateTexture): at once on the open frame recording, otherwise at the start of the next.
    PlacedTextureSentinel* sentinel = FindSentinel(*resource);
    AllocatorState& state = *GetState();
    const std::lock_guard lock(state.mutex);

    if(sentinel)
    {
        PendingActivation& activation = state.pendingActivations[sentinel->GetIdentifier()];
        activation.resource = *resource;
        activation.state = initialState;

        if(state.frameList && state.frameThread == std::this_thread::get_id())
        {
            ActivateOnFrameRecording(state, *sentinel, activation);
        }
    }

    return result;
}

bool TextureMemoryAllocator::IsPlaced(ID3D12Resource* resource)
{
    return FindSentinel(resource) != nullptr;
}

void TextureMemoryAllocator::RecordActivation(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES currentState)
{
    PlacedTextureSentinel* sentinel = commands ? FindSentinel(resource) : nullptr;

    if(!sentinel)
    {
        return;
    }

    AllocatorState& state = *GetState();
    const std::lock_guard lock(state.mutex);
    const std::uint64_t recording = ResourceStateTransaction::RecordingFor(commands);

    if(!sentinel->BeginActivation(recording))
    {
        return;
    }

    // A render-thread target activated by its owner: the activation counts once its recording is submitted.
    const auto pending = state.pendingActivations.find(sentinel->GetIdentifier());

    if(pending != state.pendingActivations.end())
    {
        pending->second.state = currentState;
        pending->second.recording = recording;

        if(recording == 0)
        {
            state.pendingActivations.erase(pending);
        }
    }

    RecordActivationCommands(commands, resource, currentState, sentinel->IsDiscardedOnActivation());
}

void TextureMemoryAllocator::Collect(std::uint64_t submissionLifetime, std::uint64_t lastSubmittedFence,
    std::uint64_t completedFence)
{
    AllocatorState& state = *GetState();
    // Released after the lock: a last reference destroys the resource, whose sentinel then takes the lock.
    std::vector<ComPtr<ID3D12Resource>> released;
    const std::lock_guard lock(state.mutex);
    CollectLocked(state, submissionLifetime, lastSubmittedFence, completedFence, released);
}

TextureMemoryAllocator::ReclaimScope::ReclaimScope(std::vector<TrackedQueue> queues)
{
    const bool queuesValid = !queues.empty() && std::all_of(queues.begin(), queues.end(), [](const TrackedQueue& queue)
    {
        return queue.fence != nullptr && static_cast<bool>(queue.lastSubmittedFence);
    });

    if(!EagerReclaimEnabled() || !queuesValid)
    {
        return;
    }

    AllocatorState& state = *GetState();
    // Released after the lock (declared before it), like Collect's; their ranges then take the scope's eager path.
    std::vector<ComPtr<ID3D12Resource>> released;
    const std::lock_guard lock(state.mutex);

    if(state.reclaimActive)
    {
        return;
    }

    state.reclaimActive = true;
    state.reclaimQueues = std::move(queues);
    state.reclaimThread = std::this_thread::get_id();
    m_active = true;
    ReclaimAtOnce(state, released);
}

TextureMemoryAllocator::ReclaimScope::~ReclaimScope()
{
    if(!m_active)
    {
        return;
    }

    AllocatorState& state = *GetState();
    std::vector<ComPtr<ID3D12Resource>> released;
    const std::lock_guard lock(state.mutex);
    // What other threads released meanwhile, or what was not complete at its release.
    ReclaimAtOnce(state, released);
    state.reclaimActive = false;
    state.reclaimQueues.clear();
}

void TextureMemoryAllocator::Trim()
{
    AllocatorState& state = *GetState();
    // Released after the lock (declared before it), like Collect's.
    std::vector<KeptAlive> released;
    const std::lock_guard lock(state.mutex);
    // The GPU is idle: the zero-fill source and the kept-alive resources go too.
    state.zeroBuffer.Reset();
    released.swap(state.keptAlive);

    if(state.heaps.empty())
    {
        return;
    }

    Log::Info("Texture memory at trim: " + Describe(state.Gather()) + ".");

    for(const auto& range : state.pendingRanges)
    {
        range.heap->ReturnFree(range.offset, range.size);
        --range.heap->pendingRanges;
    }

    state.pendingRanges.clear();
    state.idle = true;
    state.ReleaseUnusedHeaps();

    if(!state.heaps.empty())
    {
        Log::Info("Texture memory: " + std::to_string(state.heaps.size()) + " heaps still hold " +
            std::to_string(state.liveAllocations.size()) + " live textures; each heap goes with its last texture.");
    }
}

TextureMemoryAllocator::Statistics TextureMemoryAllocator::GetStatistics()
{
    AllocatorState& state = *GetState();
    const std::lock_guard lock(state.mutex);
    return state.Gather();
}

std::string TextureMemoryAllocator::DescribeStatistics()
{
    return Describe(GetStatistics());
}

void TextureMemoryAllocator::SetBudget(Category category, std::uint64_t bytes)
{
    AllocatorState& state = *GetState();
    const std::lock_guard lock(state.mutex);
    state.budgets[static_cast<std::size_t>(category)] = bytes;
}

void TextureMemoryAllocator::SetResidencyPriority(Category category, D3D12_RESIDENCY_PRIORITY priority)
{
    AllocatorState& state = *GetState();
    const std::lock_guard lock(state.mutex);
    const HeapKind kind = KindFor(category, state.unifiedHeaps);
    state.priorities[static_cast<std::size_t>(kind)] = priority;

    for(const auto& heap : state.heaps)
    {
        if(heap->kind == kind)
        {
            ApplyResidencyPriority(state.device, heap->heap.Get(), priority);
        }
    }
}

void TextureMemoryAllocator::ForEachLiveAllocation(const std::function<void(const AllocationInfo&)>& visit)
{
    AllocatorState& state = *GetState();
    const std::lock_guard lock(state.mutex);

    for(const auto& [identifier, allocation] : state.liveAllocations)
    {
        AllocationInfo information;
        information.identifier = identifier;
        information.category = allocation.category;
        information.heap = allocation.heap->heap.Get();
        information.offset = allocation.offset;
        information.size = allocation.size;
        information.resource = allocation.resource;
        visit(information);
    }
}
