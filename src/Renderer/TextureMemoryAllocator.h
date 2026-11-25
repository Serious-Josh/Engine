#pragma once

#include <d3d12.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>

// Engine-owned video memory for large textures (Documentation/Rendering/FgTiming_ManyLight_Switch_Hangs.md §7).
//
// Why: on the RTX 4080 with driver 621.01, returning large amounts of GPU-used video memory to the OS (releasing committed
// resources) can hang the first ray traversals of newly built acceleration structures that follow; no release pattern was
// safe in every test. So textures of at least MinimumPooledBytes are placed resources in heaps this allocator owns. A
// released texture returns its range to the allocator, a later texture reuses it, and the heaps themselves are never
// released or evicted during a session: only Trim, at a full-idle point such as renderer shutdown, gives memory back.
//
// Sub-allocation: best fit over 64 KiB pages with coalescing, indexed by size (O(log n) per allocation and free). The
// engine's large textures number in the hundreds, are 4-200 MiB and vary in size, which best fit handles with little
// fragmentation; a buddy allocator would round a 21 MiB texture up to 32 MiB, and size classes would strand memory
// between classes. Textures larger than a heap get a dedicated heap of their size. MSAA textures (4 MiB placement) are not
// pooled.
//
// Reuse: a released range becomes reusable only after the frame queue has completed every submission made up to the
// Collect that first sees it, so the previous occupant's last GPU use is finished. A texture placed in a range starts from
// undefined contents. Before its first use the owner records RecordActivation, which issues the aliasing barrier and, for
// render-target or depth textures, the DiscardResource that placed resources require (D3D12 resource barriers, legacy).
// Non-render-target textures are then initialized by copies that write every subresource.
//
// Heaps: render-target/depth textures and other textures use separate heaps (ALLOW_ONLY_RT_DS_TEXTURES and
// ALLOW_ONLY_NON_RT_DS_TEXTURES), which every resource heap tier supports.
//
// For later (not implemented): per-category budgets are stored and reported, not enforced; each live allocation has a
// stable identifier and can be enumerated, so a defragmenter can re-place a texture and hand its owner the new resource;
// SetResidencyPriority applies to a category's heaps.
namespace TextureMemoryAllocator
{
    // 64 MiB: a few 4096² BC textures per heap. Larger heaps leave more reserved but unused memory per heap kind (256 MiB
    // heaps reserved about 300 MiB more than the live peak on Tri-Test and Demo); textures above a heap get their own.
    inline constexpr std::uint64_t HeapBytes = 64ull * 1024ull * 1024ull;
    // 1 MiB: a 2048² BC texture is 2.7 MiB and a scene has many, so they are pooled too; below it, one committed resource
    // per texture costs little and the frees are small.
    inline constexpr std::uint64_t MinimumPooledBytes = 1024ull * 1024ull;

    enum class Category : std::uint8_t
    {
        // Sampled textures without render-target or depth flags: scene and project textures.
        SampledTextures,
        // Textures the GPU renders into: environment source, prefiltered cube.
        RenderableTextures,
        Count
    };

    inline constexpr std::size_t CategoryCount = static_cast<std::size_t>(Category::Count);

    struct CategoryStatistics
    {
        std::uint64_t liveBytes = 0;
        std::uint64_t peakLiveBytes = 0;
        std::uint64_t liveTextures = 0;
        std::uint64_t reservedBytes = 0;
        // 0: no budget. Reported only.
        std::uint64_t budgetBytes = 0;
    };

    struct Statistics
    {
        std::uint32_t heaps = 0;
        std::uint64_t reservedBytes = 0;
        std::uint64_t peakReservedBytes = 0;
        std::uint64_t liveBytes = 0;
        std::uint64_t peakLiveBytes = 0;
        std::uint64_t freeBytes = 0;
        std::uint64_t largestFreeRange = 0;
        std::uint64_t pendingBytes = 0;
        std::uint64_t texturesPlaced = 0;
        std::uint64_t texturesPlacedInReusedMemory = 0;
        std::array<CategoryStatistics, CategoryCount> categories{};
    };

    // A live allocation, for diagnostics and a future defragmenter.
    struct AllocationInfo
    {
        std::uint64_t identifier = 0;
        Category category = Category::SampledTextures;
        ID3D12Heap* heap = nullptr;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        // Borrowed; valid only inside ForEachLiveAllocation's callback.
        ID3D12Resource* resource = nullptr;
    };

    // False when the Debug switch ENGINE_TEXTURE_HEAP_POOL=0 restores one committed resource per texture (A/B).
    bool IsEnabled();

    // Render-target and depth textures need DiscardResource before their first use, so they are placed only while one of
    // these is alive on the creating thread; their owner then records RecordActivation. Others stay committed.
    class RenderablePlacementScope
    {
    public:
        RenderablePlacementScope();
        ~RenderablePlacementScope();
        RenderablePlacementScope(const RenderablePlacementScope&) = delete;
        RenderablePlacementScope& operator=(const RenderablePlacementScope&) = delete;
    };

    // A placed texture for the description, or a failed HRESULT when the allocator does not take it: too small, MSAA, not
    // a 2D texture, or the switch is off. The caller then creates a committed resource as before. Any thread.
    HRESULT CreateTexture(ID3D12Device* device, const D3D12_RESOURCE_DESC& description, D3D12_RESOURCE_STATES initialState,
        ID3D12Resource** resource);

    // True for a texture this allocator placed.
    bool IsPlaced(ID3D12Resource* resource);

    // Before a placed texture's first use, on the command list that uses it first: the aliasing barrier and, for render
    // target or depth textures, DiscardResource of every subresource (the texture is in currentState and stays in it).
    // Does nothing for any other resource, or after the first call for this texture.
    void RecordActivation(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES currentState);

    // Once per frame and after GPU drains, by the owner of the frame queue (as BLASMemoryPool::Collect).
    void Collect(std::uint64_t submissionLifetime, std::uint64_t lastSubmittedFence, std::uint64_t completedFence);

    // At a full-idle point after the final GPU drain: returns pending ranges and releases every heap that holds no live
    // texture. Heaps that still hold textures stay until their last texture is released.
    void Trim();

    Statistics GetStatistics();
    std::string DescribeStatistics();

    void SetBudget(Category category, std::uint64_t bytes);
    void SetResidencyPriority(Category category, D3D12_RESIDENCY_PRIORITY priority);
    // Visits every live allocation under the allocator's lock: the callback must not call back into the allocator.
    void ForEachLiveAllocation(const std::function<void(const AllocationInfo&)>& visit);
}
