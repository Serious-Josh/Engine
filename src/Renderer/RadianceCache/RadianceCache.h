#pragma once

#include "Renderer/Buffer.h"
#include "Renderer/ComputePipeline.h"

#include <cstdint>
#include <vector>

class DescriptorHeap;
class GraphicsDevice;
class ShaderLibrary;
class UploadBuffer;

// SHARC.2 World Radiance Cache parameters. Experimental test values, not defaults of any rendering mode.
struct RadianceCacheSettings
{
    // A power of two, at least RadianceCache::MinimumSlotCount.
    std::uint32_t slotCount = 1u << 18;
    // World-space cell edge in metres. Cells are fixed in world space and never depend on the camera.
    float cellSize = 0.1f;
    // A published entry without samples for more than this many frames is evicted.
    std::uint32_t staleFrames = 300;
    // Cap on the published sample count: the history weight never exceeds it.
    float maximumSampleCount = 256.0f;
    // Exponential blend of the relative-variance estimate.
    float varianceBlend = 0.1f;
};

// Must match RadianceCacheBinding (RadianceCache.hlsli).
struct RadianceCacheBinding
{
    std::uint32_t tagsIndex = 0;
    std::uint32_t keysIndex = 0;
    std::uint32_t accumulationIndex = 0;
    std::uint32_t publishedIndex = 0;

    std::uint32_t bucketFlagsIndex = 0;
    std::uint32_t statisticsIndex = 0;
    std::uint32_t bucketCount = 0;
    std::uint32_t frameIndex = 0;

    std::uint32_t tagsReadIndex = 0;
    std::uint32_t keysReadIndex = 0;
    std::uint32_t publishedReadIndex = 0;
    std::uint32_t staleFrames = 0;

    float cellSize = 0.0f;
    float inverseCellSize = 0.0f;
    float maximumSampleCount = 0.0f;
    float varianceBlend = 0.0f;
};

static_assert(sizeof(RadianceCacheBinding) == 4 * 16);

// Must match the RadianceCacheConstants cbuffer of RadianceCacheResolve.hlsl.
struct RadianceCacheConstants
{
    RadianceCacheBinding cache;

    std::uint32_t clearIndex = 0;
    std::uint32_t clearWordCount = 0;
    std::uint32_t clearPadding[2]{};
};

static_assert(sizeof(RadianceCacheConstants) == 5 * 16);

// SHARC.2 World Radiance Cache: Engine's own spatially hashed cache of world-space values (RadianceCache.hlsli has the
// storage, key and phase contracts). Experimental and diagnostics-only: the Renderer does not own one, and no
// path-tracing library reads or writes it.
//
// A frame is clear (when requested) -> update -> resolve -> query, all on one queue:
//   BeginUpdate   cache buffers to unordered access (the update writes tags, keys, accumulation, flags, statistics).
//   RecordResolve UAV barrier, resolve dispatch, UAV barrier: publication. After it, published entries are complete.
//   BeginQuery    tags, keys and published entries to shader-resource state: queries can only read.
// Cross-frame persistence needs no fence: frames execute in submission order on the queue. Replacing the buffers does:
// CreateResources retires the old buffers and their descriptors until the GPU completes the last submission that may
// use them.
class RadianceCache
{
    public:
    static constexpr std::uint32_t BucketSlots = 8;
    static constexpr std::uint32_t MinimumSlotCount = 32;
    static constexpr std::uint32_t StatisticCount = 16;
    // tags 4 + keys 8 + accumulation 32 + published 32 per slot, plus 4 bytes of bucket flags per bucket.
    static constexpr std::uint64_t BytesPerSlot = 4 + 8 + 32 + 32;

    // The clear and resolve pipelines. Created on first use; a failure is sticky.
    bool Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary);

    // Allocates the buffers and descriptors for settings, retiring any current ones until completedFence reaches
    // lastSubmissionFence. The new cache is not cleared: record RecordClear before its first update.
    bool CreateResources(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        const RadianceCacheSettings& settings,
        std::uint64_t lastSubmissionFence,
        std::uint64_t completedFence);

    // Releases retired buffers whose last possible user has completed.
    void ReleaseRetired(std::uint64_t completedFence);

    // Releases everything. Caller guarantees the queue is idle.
    void Shutdown();

    // Zero-fills the cache (every slot empty, nothing published); statistics too when includeStatistics.
    bool RecordClear(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer, bool includeStatistics);

    void BeginUpdate(ID3D12GraphicsCommandList* commandList);

    bool RecordResolve(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer, std::uint32_t frameIndex);

    void BeginQuery(ID3D12GraphicsCommandList* commandList);

    RadianceCacheBinding GetBinding(std::uint32_t frameIndex) const;

    bool HasResources() const
    {
        return m_resources.tags.GetResource() != nullptr;
    }

    const RadianceCacheSettings& GetSettings() const
    {
        return m_settings;
    }

    std::uint64_t GetResourceBytes() const;

    std::size_t GetRetiredCount() const
    {
        return m_retired.size();
    }

    // Diagnostics read the raw buffers.
    Buffer& GetTags()
    {
        return m_resources.tags;
    }

    Buffer& GetKeys()
    {
        return m_resources.keys;
    }

    Buffer& GetAccumulation()
    {
        return m_resources.accumulation;
    }

    Buffer& GetPublished()
    {
        return m_resources.published;
    }

    Buffer& GetBucketFlags()
    {
        return m_resources.bucketFlags;
    }

    Buffer& GetStatistics()
    {
        return m_resources.statistics;
    }

    private:
    struct Resources
    {
        Buffer tags;
        Buffer keys;
        Buffer accumulation;
        Buffer published;
        Buffer bucketFlags;
        Buffer statistics;
    };

    struct RetiredResources
    {
        Resources resources;
        std::uint64_t submissionFence = 0;
    };

    static void ReleaseResources(Resources& resources);

    bool RecordClearBuffer(
        ID3D12GraphicsCommandList* commandList,
        UploadBuffer& uploadBuffer,
        const Buffer& buffer,
        std::uint64_t bytes);

    void RecordUAVBarriers(ID3D12GraphicsCommandList* commandList);

    ComputePipeline m_resolvePipeline;
    ComputePipeline m_clearPipeline;
    bool m_initialized = false;
    bool m_initializationFailed = false;

    Resources m_resources;
    std::vector<RetiredResources> m_retired;
    RadianceCacheSettings m_settings;
};
