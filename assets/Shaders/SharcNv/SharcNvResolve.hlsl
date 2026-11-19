// NVIDIA SHARC v1.8.3 cache maintenance (Renderer/Sharc/SharcCache), compiled with -enable-16bit-types (SharcNv/):
//   CSClear              zeroes every entry of the three cache buffers (a new allocation, or an invalidation).
//   CSClearStatistics    zeroes the statistics words.
//   CSAccumulationStatistics  before the resolve: occupancy and this frame's accumulation range (overflow headroom).
//   CSResolve            NVIDIA's SharcResolveEntry for every entry.
//   CSResolvedStatistics after the resolve: occupied entries and saturated (fp16-limit) resolved values.
// One thread per entry; the only root parameter is the SharcNvConstants buffer at b0.

#include "../SharcNvCommon.hlsli"

cbuffer SharcNvResolveConstants : register(b0)
{
    SharcNvConstants sharcNv;
};

static const uint SHARC_NV_GROUP_SIZE = 256u;

[numthreads(SHARC_NV_GROUP_SIZE, 1, 1)]
void CSClear(uint3 thread : SV_DispatchThreadID)
{
    uint entry = thread.x;

    if(entry >= sharcNv.capacity)
    {
        return;
    }

    RWStructuredBuffer<uint64_t> hashEntries = ResourceDescriptorHeap[sharcNv.hashEntriesIndex];
    RWStructuredBuffer<SharcAccumulationData> accumulation = ResourceDescriptorHeap[sharcNv.accumulationIndex];
    RWStructuredBuffer<SharcPackedData> resolved = ResourceDescriptorHeap[sharcNv.resolvedIndex];
    hashEntries[entry] = HASH_GRID_INVALID_HASH_KEY;
    accumulation[entry] = SharcZeroAccumulationData();
    resolved[entry] = SharcZeroPackedData();
}

[numthreads(SHARC_NV_STATISTIC_COUNT, 1, 1)]
void CSClearStatistics(uint3 thread : SV_DispatchThreadID)
{
    RWByteAddressBuffer statistics = ResourceDescriptorHeap[sharcNv.statisticsIndex];
    statistics.Store(thread.x * 4u, 0u);
}

[numthreads(SHARC_NV_GROUP_SIZE, 1, 1)]
void CSAccumulationStatistics(uint3 thread : SV_DispatchThreadID)
{
    uint entry = thread.x;
    bool inside = entry < sharcNv.capacity;
    bool occupied = false;
    uint4 data = uint4(0u, 0u, 0u, 0u);

    if(inside)
    {
        RWStructuredBuffer<uint64_t> hashEntries = ResourceDescriptorHeap[sharcNv.hashEntriesIndex];
        RWStructuredBuffer<SharcAccumulationData> accumulation = ResourceDescriptorHeap[sharcNv.accumulationIndex];
        occupied = hashEntries[entry] != HASH_GRID_INVALID_HASH_KEY;
        data = accumulation[entry].data;
    }

    // data.xyz: this frame's scaled radiance sums (uint32 each), data.w: its sample count. A sum at or above 2^31 has
    // less than a factor of two of headroom before it wraps.
    uint maximumChannel = max(data.x, max(data.y, data.z));
    SharcNvCount(sharcNv, SHARC_NV_STATISTIC_OCCUPIED, occupied);
    SharcNvCount(sharcNv, SHARC_NV_STATISTIC_SAMPLED, occupied && data.w > 0u);
    SharcNvCount(sharcNv, SHARC_NV_STATISTIC_HIGH_ACCUMULATION, maximumChannel >= 0x80000000u);

    uint waveMaximumChannel = WaveActiveMax(maximumChannel);
    uint waveMaximumSamples = WaveActiveMax(data.w);

    if(WaveIsFirstLane())
    {
        RWByteAddressBuffer statistics = ResourceDescriptorHeap[sharcNv.statisticsIndex];
        statistics.InterlockedMax(SHARC_NV_STATISTIC_MAXIMUM_ACCUMULATION * 4u, waveMaximumChannel);
        statistics.InterlockedMax(SHARC_NV_STATISTIC_MAXIMUM_SAMPLES * 4u, waveMaximumSamples);
    }
}

[numthreads(SHARC_NV_GROUP_SIZE, 1, 1)]
void CSResolve(uint3 thread : SV_DispatchThreadID)
{
    SharcResolveParameters resolveParameters;
    resolveParameters.cameraPositionPrev = sharcNv.previousCameraPosition;
    resolveParameters.accumulationFrameNum = sharcNv.accumulationFrames;
    resolveParameters.responsiveFrameNum = sharcNv.accumulationFrames;
    resolveParameters.staleFrameNumMax = sharcNv.staleFrames;
    resolveParameters.frameIndex = sharcNv.frameIndex;
    SharcResolveEntry(thread.x, SharcNvParameters(sharcNv), resolveParameters);
}

[numthreads(SHARC_NV_GROUP_SIZE, 1, 1)]
void CSResolvedStatistics(uint3 thread : SV_DispatchThreadID)
{
    uint entry = thread.x;
    bool occupied = false;
    bool saturated = false;

    if(entry < sharcNv.capacity)
    {
        RWStructuredBuffer<uint64_t> hashEntries = ResourceDescriptorHeap[sharcNv.hashEntriesIndex];
        RWStructuredBuffer<SharcPackedData> resolved = ResourceDescriptorHeap[sharcNv.resolvedIndex];
        occupied = hashEntries[entry] != HASH_GRID_INVALID_HASH_KEY;
        float4 value = float4(resolved[entry].radianceData);
        saturated = occupied && (any(value.xyz >= 65504.0f) || !all(isfinite(value)));
    }

    SharcNvCount(sharcNv, SHARC_NV_STATISTIC_RESOLVED_OCCUPIED, occupied);
    SharcNvCount(sharcNv, SHARC_NV_STATISTIC_SATURATED, saturated);
}
