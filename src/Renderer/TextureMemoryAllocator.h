#pragma once

#include <d3d12.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Engine-owned video memory for large textures (Documentation/Rendering/FgTiming_ManyLight_Switch_Hangs.md §7).
//
// Why: on the RTX 4080 with driver 621.01, returning large amounts of GPU-used video memory to the OS (releasing committed
// resources) can hang the first ray traversals of newly built acceleration structures that follow; no release pattern was
// safe in every test. So textures of at least MinimumPooledBytes are placed resources in heaps this allocator owns. A
// released texture returns its range to the allocator, a later texture reuses it, and the heaps themselves are never
// released or evicted during a session: only Trim, at a full-idle point such as renderer shutdown, gives memory back.
// Owners that record activation (ActivatedPlacementScope) also place their render-target textures, small textures and
// buffers: the environment places everything it allocates, so an environment change frees nothing to the OS.
// Render-target, depth and unordered-access textures and default-heap buffers the renderer creates on its own thread
// (PlaceRenderThreadTargets) are placed too, whatever their size, and the allocator activates them itself on the frame
// recording, so a resize, a mode retirement or a denoiser change frees nothing to the OS either. Uploaded buffers and
// acceleration-structure memory are placed on any thread (ActivatedBufferScope), activated by the upload or the build.
//
// Sub-allocation: best fit over 64 KiB pages with coalescing, indexed by size (O(log n) per allocation and free). The
// engine's large textures number in the hundreds, are 4-200 MiB and vary in size, which best fit handles with little
// fragmentation; a buddy allocator would round a 21 MiB texture up to 32 MiB, and size classes would strand memory
// between classes. Textures larger than a heap get a dedicated heap of their size. MSAA textures (4 MiB placement) are not
// pooled.
//
// Reuse: a released range becomes reusable only after the frame queue has completed every submission made up to the
// Collect that first sees it, so the previous occupant's last GPU use is finished. A texture placed in a range starts from
// undefined contents. Before its first use it is activated: the aliasing barrier and, for render-target, depth and
// unordered-access textures, the DiscardResource that placed resources require (D3D12 resource barriers, legacy). Sampled
// textures are then initialized by copies that write every subresource; the others by their passes, whose histories start
// invalid.
// Who activates: an owner inside an ActivatedPlacementScope records RecordActivation itself before the first use. Render
// thread targets are activated by the allocator: at creation when the frame recording is open (BeginFrameRecording), or
// at the start of the next one. Texture wrappers also call RecordActivation before their first transition, which covers
// placed textures that no one activated yet. An activation recorded on the frame recording counts only once that recording
// is submitted: if it is discarded (R.1), the texture is activated again at the start of the next one.
//
// Heaps (allocator P6b): on resource heap tier 2, one heap kind holds every resource (ALLOW_ALL_BUFFERS_AND_TEXTURES,
// UnifiedHeapBytes), so memory a buffer frees serves a texture and the reverse. On tier 1, or with the Debug switch
// ENGINE_TEXTURE_HEAP_UNIFIED=0, render-target/depth textures, other textures (sampled and unordered-access) and buffers
// use separate heaps (ALLOW_ONLY_RT_DS_TEXTURES, ALLOW_ONLY_NON_RT_DS_TEXTURES, ALLOW_ONLY_BUFFERS). Activation is the same
// in either: the aliasing barrier makes any previous occupant inactive, whatever its kind. A buffer needs only the
// aliasing barrier on activation; the render thread's buffers are also zero-filled then, because owners written for
// committed buffers may rely on zeroed contents (AIR counters, ReSTIR PT stagnancy maps).
//
// For later (not implemented): per-category budgets are stored and reported, not enforced; each live allocation has a
// stable identifier and can be enumerated, so a defragmenter can re-place a texture and hand its owner the new resource;
// SetResidencyPriority applies to a category's heaps.
namespace TextureMemoryAllocator
{
    // 64 MiB: a few 4096² BC textures per heap. Larger heaps leave more reserved but unused memory per heap kind (256 MiB
    // heaps reserved about 300 MiB more than the live peak on Tri-Test and Demo); textures above a heap get their own.
    inline constexpr std::uint64_t HeapBytes = 64ull * 1024ull * 1024ull;
    // Buffer heaps: the renderer's large buffers (ReSTIR PT reservoirs, scratch and pair records, 81-122 MiB at 1912x697;
    // RTXDI reservoirs, 93 MiB) share heaps instead of each holding a dedicated heap of its exact size; above it, dedicated.
    inline constexpr std::uint64_t BufferHeapBytes = 256ull * 1024ull * 1024ull;
    // Allocator P6b: unified heaps (resource heap tier 2) hold those buffers and the 24-48 MiB render-resolution targets
    // alike; above it, dedicated.
    inline constexpr std::uint64_t UnifiedHeapBytes = 128ull * 1024ull * 1024ull;
    // 1 MiB: a 2048² BC texture is 2.7 MiB and a scene has many, so they are pooled too; below it, one committed resource
    // per texture costs little and the frees are small.
    inline constexpr std::uint64_t MinimumPooledBytes = 1024ull * 1024ull;

    enum class Category : std::uint8_t
    {
        // Sampled textures without render-target or depth flags: scene and project textures.
        SampledTextures,
        // Render-target and depth textures: scene targets, the environment source and cubes.
        RenderableTextures,
        // Buffers: the environment's importance CDFs, uploaded buffers (meshes, tables), acceleration-structure memory and
        // the renderer's GPU-writable buffers.
        Buffers,
        // Unordered-access textures without render-target or depth flags: pass outputs and histories.
        UnorderedTextures,
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

    // The heap kinds: per-kind heaps (tier 1, or the Debug switch), or the unified kind (tier 2).
    enum class HeapKind : std::uint8_t
    {
        Textures,
        RenderableTextures,
        Buffers,
        Unified,
        Count
    };

    inline constexpr std::size_t HeapKindCount = static_cast<std::size_t>(HeapKind::Count);

    struct HeapKindStatistics
    {
        std::uint32_t heaps = 0;
        std::uint64_t reservedBytes = 0;
        std::uint64_t freeBytes = 0;
        std::uint64_t largestFreeRange = 0;
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
        // Heaps created while released memory was still pending (not yet reusable), and that memory summed over them.
        std::uint32_t heapsCreatedWhilePending = 0;
        std::uint64_t pendingBytesAtHeapCreations = 0;
        std::array<CategoryStatistics, CategoryCount> categories{};
        std::array<HeapKindStatistics, HeapKindCount> kinds{};
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

    // The calling thread (the renderer's) places its render-target, depth and unordered-access textures from now on, and
    // its default-heap buffers, and the allocator activates them on the frame recording: textures as below, buffers with
    // the aliasing barrier and a zero fill (they start zeroed, as committed buffers do), except acceleration-structure
    // memory. Debug: ENGINE_TEXTURE_HEAP_POOL_TARGETS=0 keeps the textures committed, ENGINE_BUFFER_HEAP_POOL=0 the
    // buffers (A/B of each alone).
    void PlaceRenderThreadTargets();

    // False when the Debug switch ENGINE_BUFFER_HEAP_POOL=0 keeps every buffer except the environment's committed.
    bool BufferPoolEnabled();

    // While one is alive on the creating thread, buffers that thread creates are placed (unless BufferPoolEnabled is false)
    // and their owner records RecordActivation (aliasing barrier only) before their first use, on any thread: buffers that
    // an upload or an acceleration-structure build writes in full right away.
    class ActivatedBufferScope
    {
    public:
        ActivatedBufferScope();
        ~ActivatedBufferScope();
        ActivatedBufferScope(const ActivatedBufferScope&) = delete;
        ActivatedBufferScope& operator=(const ActivatedBufferScope&) = delete;
    };

    // While one is alive on the creating thread, render-thread targets and buffers are committed resources, as before: for
    // owners that allocate once per session and rely on committed memory (the physical sky's persistent sets).
    class CommittedTargetScope
    {
    public:
        CommittedTargetScope();
        ~CommittedTargetScope();
        CommittedTargetScope(const CommittedTargetScope&) = delete;
        CommittedTargetScope& operator=(const CommittedTargetScope&) = delete;
    };

    // The frame recording's command list, by its owner (CommandContext): open from Begin (after the list is reset) to End
    // or abort. Begin first activates the render-thread targets created since the last recording, and those whose
    // activation was in a discarded recording.
    void BeginFrameRecording(ID3D12GraphicsCommandList* commandList);
    void EndFrameRecording();

    // While one is alive on the creating thread, its owner records RecordActivation before first use, so more of what that
    // thread creates is placed: render-target, depth and unordered-access textures (they need DiscardResource), textures
    // below MinimumPooledBytes and buffers (CreateBuffer). Without one, only large sampled textures and render-thread
    // targets are placed.
    class ActivatedPlacementScope
    {
    public:
        ActivatedPlacementScope();
        ~ActivatedPlacementScope();
        ActivatedPlacementScope(const ActivatedPlacementScope&) = delete;
        ActivatedPlacementScope& operator=(const ActivatedPlacementScope&) = delete;
    };

    // A placed texture for the description, or a failed HRESULT when the allocator does not take it: MSAA, not a 2D
    // texture, the switch is off, a sampled texture below MinimumPooledBytes outside a scope, or a render-target, depth or
    // unordered-access texture outside an ActivatedPlacementScope and off the render-thread target path. The caller then
    // creates a committed resource as before. optimizedClearValue: as for CreateCommittedResource. Any thread.
    HRESULT CreateTexture(ID3D12Device* device, const D3D12_RESOURCE_DESC& description, D3D12_RESOURCE_STATES initialState,
        ID3D12Resource** resource, const D3D12_CLEAR_VALUE* optimizedClearValue = nullptr);

    // A placed default-heap buffer inside an ActivatedPlacementScope or ActivatedBufferScope, or on the render thread (see
    // PlaceRenderThreadTargets); a failed HRESULT otherwise: the caller then creates a committed resource. Buffers use heaps
    // of their own (ALLOW_ONLY_BUFFERS, BufferHeapBytes) and any size, in 64 KiB pages.
    HRESULT CreateBuffer(ID3D12Device* device, const D3D12_RESOURCE_DESC& description, D3D12_RESOURCE_STATES initialState,
        ID3D12Resource** resource);

    // True for a texture or buffer this allocator placed.
    bool IsPlaced(ID3D12Resource* resource);

    // Before a placed texture's first use, on the command list that uses it first: the aliasing barrier and, for render
    // target, depth or unordered-access textures, DiscardResource of every subresource (the texture is in currentState,
    // every subresource alike, and stays in it). Does nothing for any other resource, or once the texture was activated
    // (an activation in a discarded frame recording does not count).
    void RecordActivation(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES currentState);

    // Once per frame and after GPU drains, by the owner of the frame queue (as BLASMemoryPool::Collect).
    void Collect(std::uint64_t submissionLifetime, std::uint64_t lastSubmittedFence, std::uint64_t completedFence);

    // A queue whose submissions may use placed memory: its fence, its last submitted fence value (read whenever needed, so
    // a submission during a ReclaimScope counts) and its submission lifetime, as Collect takes them.
    struct TrackedQueue
    {
        ID3D12Fence* fence = nullptr;
        std::function<std::uint64_t()> lastSubmittedFence;
        std::uint64_t submissionLifetime = 0;
    };

    // Allocator P6a: eager reclaim where many targets are released and recreated at once (ResizeScene). On construction it
    // reclaims every pending range against fresh completed values of the queues; while it is alive, a range released on
    // this thread is checked at once: it is free when the first queue (the frame queue, whose fence Collect uses) has
    // completed every submission made up to the release and every other queue has completed all of its submissions;
    // otherwise it stays pending, as without the scope. It assumes nothing about GPU idleness. With a frame recording open,
    // released ranges stay pending (that recording may still use them). One scope at a time: a nested one does nothing.
    // Debug: ENGINE_TEXTURE_HEAP_EAGER_RECLAIM=0 makes every scope do nothing (A/B).
    class ReclaimScope
    {
    public:
        explicit ReclaimScope(std::vector<TrackedQueue> queues);
        ~ReclaimScope();
        ReclaimScope(const ReclaimScope&) = delete;
        ReclaimScope& operator=(const ReclaimScope&) = delete;

    private:
        bool m_active = false;
    };

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
