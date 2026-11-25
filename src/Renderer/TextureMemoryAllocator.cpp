#include "Renderer/TextureMemoryAllocator.h"

#include "Core/Log.h"

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
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

    thread_local std::uint32_t t_renderablePlacementScopes = 0;

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

    // Render-target/depth textures and other textures live in separate heaps. Tier 1 requires it; on tier 2 it keeps
    // the two kinds' placement and initialization rules apart at no cost.
    enum class HeapKind : std::uint8_t
    {
        Textures,
        RenderableTextures,
        Count
    };

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

    struct AllocatorState
    {
        std::mutex mutex;
        ID3D12Device* device = nullptr;
        bool deviceMismatchReported = false;
        // Heaps are erased only by Trim (or after it, once idle) and only when unused, so allocations may hold pointers.
        std::vector<std::unique_ptr<Heap>> heaps;
        std::vector<PendingRange> pendingRanges;
        std::unordered_map<std::uint64_t, LiveAllocation> liveAllocations;
        std::uint64_t nextIdentifier = 1;
        // Set by Trim: the GPU is idle, so released ranges are free at once and emptied heaps go.
        bool idle = false;
        std::array<D3D12_RESIDENCY_PRIORITY, static_cast<std::size_t>(HeapKind::Count)> priorities{
            D3D12_RESIDENCY_PRIORITY_NORMAL, D3D12_RESIDENCY_PRIORITY_NORMAL};
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

            for(const auto& heap : heaps)
            {
                for(const auto& [offset, size] : heap->freeByOffset)
                {
                    statistics.freeBytes += size;
                    statistics.largestFreeRange = (std::max)(statistics.largestFreeRange, size);
                }
            }

            for(const auto& range : pendingRanges)
            {
                statistics.pendingBytes += range.size;
            }

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

    std::string Describe(const TextureMemoryAllocator::Statistics& statistics)
    {
        using TextureMemoryAllocator::Category;
        const double fragmentation = statistics.freeBytes == 0 ? 0.0 :
            100.0 * (1.0 - static_cast<double>(statistics.largestFreeRange) / static_cast<double>(statistics.freeBytes));
        char percent[16];
        std::snprintf(percent, sizeof(percent), "%.0f%%", fragmentation);
        const auto& sampled = statistics.categories[static_cast<std::size_t>(Category::SampledTextures)];
        const auto& renderable = statistics.categories[static_cast<std::size_t>(Category::RenderableTextures)];

        return std::to_string(statistics.heaps) + " heaps, " + FormatMebibytes(statistics.reservedBytes) + " reserved (peak " +
            FormatMebibytes(statistics.peakReservedBytes) + "), " + FormatMebibytes(statistics.liveBytes) + " in use (peak " +
            FormatMebibytes(statistics.peakLiveBytes) + "), free " + FormatMebibytes(statistics.freeBytes) + " (largest " +
            FormatMebibytes(statistics.largestFreeRange) + ", fragmentation " + percent + "), pending " +
            FormatMebibytes(statistics.pendingBytes) + "; sampled " + FormatMebibytes(sampled.liveBytes) + " in " +
            std::to_string(sampled.liveTextures) + " textures, renderable " + FormatMebibytes(renderable.liveBytes) +
            " in " + std::to_string(renderable.liveTextures) + "; " + std::to_string(statistics.texturesPlaced) +
            " textures placed, " + std::to_string(statistics.texturesPlacedInReusedMemory) + " of them in an existing heap";
    }

    std::shared_ptr<AllocatorState>& GetState()
    {
        static std::shared_ptr<AllocatorState> state = std::make_shared<AllocatorState>();
        return state;
    }

    // Owned by the texture's private-data store. The D3D12 runtime releases it when the texture is destroyed, whoever
    // dropped the last reference, which returns the texture's range to the allocator.
    class PlacedTextureSentinel final : public IUnknown
    {
    public:
        PlacedTextureSentinel(std::shared_ptr<AllocatorState> state, std::uint64_t identifier, bool renderable)
            : m_state(std::move(state)), m_identifier(identifier), m_renderable(renderable)
        {
        }

        bool IsRenderable() const
        {
            return m_renderable;
        }

        // True for the first caller only.
        bool BeginActivation()
        {
            return !m_activated.exchange(true, std::memory_order_acq_rel);
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

            ++allocation.heap->pendingRanges;
            state.pendingRanges.push_back({allocation.heap, allocation.offset, allocation.size});
        }

        std::atomic<ULONG> m_references{1};
        std::atomic<bool> m_activated{false};
        std::shared_ptr<AllocatorState> m_state;
        std::uint64_t m_identifier = 0;
        bool m_renderable = false;
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

    HeapKind KindFor(bool renderable)
    {
        return renderable ? HeapKind::RenderableTextures : HeapKind::Textures;
    }

    D3D12_HEAP_FLAGS HeapFlagsFor(HeapKind kind)
    {
        return kind == HeapKind::RenderableTextures ? D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES :
            D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
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
} // namespace

TextureMemoryAllocator::RenderablePlacementScope::RenderablePlacementScope()
{
    ++t_renderablePlacementScopes;
}

TextureMemoryAllocator::RenderablePlacementScope::~RenderablePlacementScope()
{
    --t_renderablePlacementScopes;
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
    D3D12_RESOURCE_STATES initialState, ID3D12Resource** resource)
{
    const bool renderable = (description.Flags & RenderableFlags) != 0;
    const bool otherFlags = (description.Flags & ~(RenderableFlags | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) != 0;

    // Render-target and depth textures need DiscardResource before first use, so only owners that record activation
    // opt them in (RenderablePlacementScope). Unordered-access textures stay committed until their owners do too.
    const bool eligible = IsEnabled() && device && resource && description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        description.SampleDesc.Count == 1 && !otherFlags && (!renderable || t_renderablePlacementScopes > 0);

    if(!eligible)
    {
        return E_NOTIMPL;
    }

    const D3D12_RESOURCE_ALLOCATION_INFO information = device->GetResourceAllocationInfo(0, 1, &description);
    const bool sizeAccepted = information.SizeInBytes != UINT64_MAX && information.SizeInBytes >= MinimumPooledBytes &&
        information.Alignment <= PageBytes;

    if(!sizeAccepted)
    {
        return E_NOTIMPL;
    }

    const std::uint64_t size = AlignUp(information.SizeInBytes, PageBytes);
    const Category category = renderable ? Category::RenderableTextures : Category::SampledTextures;
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
        const HeapKind kind = KindFor(renderable);
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
            const bool dedicated = size > HeapBytes;
            auto heap = std::make_unique<Heap>();
            D3D12_HEAP_DESC heapDescription{};
            heapDescription.SizeInBytes = dedicated ? size : HeapBytes;
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
        nullptr, IID_PPV_ARGS(&placed));
    auto* sentinel = SUCCEEDED(result) ? new PlacedTextureSentinel(stateOwner, identifier, renderable) : nullptr;
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

bool TextureMemoryAllocator::IsPlaced(ID3D12Resource* resource)
{
    return FindSentinel(resource) != nullptr;
}

void TextureMemoryAllocator::RecordActivation(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES currentState)
{
    PlacedTextureSentinel* sentinel = commands ? FindSentinel(resource) : nullptr;

    if(!sentinel || !sentinel->BeginActivation())
    {
        return;
    }

    // The previous occupant of the range finished on the GPU before it was reused; the barrier makes this texture the
    // range's active resource.
    D3D12_RESOURCE_BARRIER aliasing{};
    aliasing.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    aliasing.Aliasing.pResourceBefore = nullptr;
    aliasing.Aliasing.pResourceAfter = resource;
    commands->ResourceBarrier(1, &aliasing);

    if(!sentinel->IsRenderable())
    {
        return;
    }

    // Placed render-target and depth textures start with undefined compression metadata: discard every subresource
    // in the target state before any other use, and return to the state the owner tracks.
    const bool depth = (resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    const D3D12_RESOURCE_STATES targetState = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE :
        D3D12_RESOURCE_STATE_RENDER_TARGET;
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

void TextureMemoryAllocator::Collect(std::uint64_t submissionLifetime, std::uint64_t lastSubmittedFence,
    std::uint64_t completedFence)
{
    AllocatorState& state = *GetState();
    const std::lock_guard lock(state.mutex);

    if(state.pendingRanges.empty())
    {
        return;
    }

    const bool completedFenceKnown = completedFence != UINT64_MAX;

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

void TextureMemoryAllocator::Trim()
{
    AllocatorState& state = *GetState();
    const std::lock_guard lock(state.mutex);

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
    const HeapKind kind = KindFor(category == Category::RenderableTextures);
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
