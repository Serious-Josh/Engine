// NVIDIA SHARC v1.8.3 cache maintenance (Renderer/Sharc/SharcCache), compiled with -enable-16bit-types (SharcNv/):
//   CSClear              zeroes every entry of the three cache buffers (a new allocation, or an invalidation).
//   CSClearStatistics    zeroes the statistics words.
//   CSInvalidateRegions  before the update: forgets the cached radiance of entries inside this frame's lighting
//                        discontinuities (SharcNvInvalidationConstants at b1).
//   CSAccumulationStatistics  before the resolve: occupancy and this frame's accumulation range (overflow headroom).
//   CSResolve            NVIDIA's SharcResolveEntry for every entry.
//   CSResolvedStatistics after the resolve: occupied entries and saturated (fp16-limit) resolved values.
// One thread per entry; the root parameter is the SharcNvConstants buffer at b0 (CSInvalidateRegions adds b1).

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

// Must match SharcCache::InvalidationConstants: a count, then spheres (centre xyz, radius w) in world space.
static const uint SHARC_NV_MAXIMUM_INVALIDATION_REGIONS = 64u;

cbuffer SharcNvInvalidationConstants : register(b1)
{
    uint4 invalidationCount;
    float4 invalidationRegions[SHARC_NV_MAXIMUM_INVALIDATION_REGIONS];
};

// Lighting discontinuities (FrameLighting.h DetectLightingDiscontinuities; SHARC_ReSTIR_PT_Investigation.md §9.5): an
// entry whose voxel touches a region keeps its key but loses its radiance and samples, so queries refuse it (the path
// tracer truncates there, as on a cold cache) until this frame's training refills it. Recorded before the update.
[numthreads(SHARC_NV_GROUP_SIZE, 1, 1)]
void CSInvalidateRegions(uint3 thread : SV_DispatchThreadID)
{
    uint entry = thread.x;

    if(entry >= sharcNv.capacity)
    {
        return;
    }

    RWStructuredBuffer<uint64_t> hashEntries = ResourceDescriptorHeap[sharcNv.hashEntriesIndex];
    uint64_t key = hashEntries[entry];

    if(key == HASH_GRID_INVALID_HASH_KEY)
    {
        return;
    }

    HashGridParameters grid = SharcNvGridParameters(sharcNv);
    uint level = uint((key >> HASH_GRID_LEVEL_BIT_OFFSET) & HASH_GRID_LEVEL_BIT_MASK);
    // The voxel's centre, and its half diagonal as the margin.
    float3 centre = HashGridGetPositionFromKey(key, grid) - HASH_GRID_POSITION_BIAS;
    float margin = 0.8660254f * HashGridGetVoxelSize(level, grid);
    uint count = min(invalidationCount.x, SHARC_NV_MAXIMUM_INVALIDATION_REGIONS);

    for(uint region = 0u; region < count; ++region)
    {
        float4 sphere = invalidationRegions[region];
        float3 offset = centre - sphere.xyz;
        float reach = sphere.w + margin;

        if(dot(offset, offset) <= reach * reach)
        {
            RWStructuredBuffer<SharcAccumulationData> accumulation = ResourceDescriptorHeap[sharcNv.accumulationIndex];
            RWStructuredBuffer<SharcPackedData> resolved = ResourceDescriptorHeap[sharcNv.resolvedIndex];
            accumulation[entry] = SharcZeroAccumulationData();
            resolved[entry] = SharcZeroPackedData();
            return;
        }
    }
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
