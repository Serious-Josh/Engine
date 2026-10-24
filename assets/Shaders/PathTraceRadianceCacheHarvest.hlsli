// SHARC.4D learning: harvests one D6 training sample per raster-primary path from the production path loop
// (PathTraceTracePath), in the cache library only (PATH_TRACE_RADIANCE_CACHE_LIBRARY). The harvest observes the path;
// it never changes what the path traces or adds to the image, and it reads nothing cached.
//
// Estimator (build/sharc-4d/REPORT.md section 1). The cache vertex is path vertex 1 (x1). If x1 is eligible (not a
// delta surface, material model 0 or 1, metallic <= 0.1) and keyed, the sample is
//   g_mk(mu_w) trig(m phi_w) S / p
// where w and p are the direction and mixture pdf x1's BSDF sample chose, the frame is x1's facing geometric normal, and
// S is the hit-only incident radiance along w: everything the path adds after x1, divided by the throughput at x1's
// continuation (x1's own BSDF weight excluded; roulette at x1 included as 1/survival). The miss of the segment leaving
// x1 is x1's direct light and is excluded; next-event estimation at vertex 2 and later, and misses of later segments,
// are included. An invalid or delta continuation, roulette termination at x1, or a path whose suffix adds nothing is a
// zero-valued sample (still counted). Every eligible path inserts exactly one sample, so each entry's samples are i.i.d.
//
// SHARC.4D-Q deferred insertion. With the binding's deferred flag, or always in the deferred learning libraries
// (PATH_TRACE_RADIANCE_CACHE_DEFERRED, PathTraceReferenceRadianceCacheDeferred*.hlsl), a path stores its raw sample as
// a record (RadianceCacheD6.hlsli) instead of inserting it; CSInsertRecords inserts the records after the dispatch with
// the same header check, values and atomic insertion. The deferred library has no insertion, statistics or lookup code,
// and stores x1's part of the record at vertex 1 so that it is not kept across later traces;
// PATH_TRACE_RADIANCE_CACHE_DEFERRED_LATE keeps it and stores the whole record at the end of the path instead.

// SHARC.4D-R: the query library (PATH_TRACE_RADIANCE_CACHE_QUERY) has query-only hooks instead
// (PathTraceRadianceCacheQuery.hlsli); the training library (PATH_TRACE_RADIANCE_CACHE_TRAINING,
// PathTraceRadianceCacheTraining.hlsli) is the deferred library whose records are indexed by training thread.
#ifdef PATH_TRACE_RADIANCE_CACHE_QUERY
#include "PathTraceRadianceCacheQuery.hlsli"
#else

#include "RadianceCacheD6.hlsli"

#if defined(PATH_TRACE_RADIANCE_CACHE_DEFERRED) && !defined(PATH_TRACE_RADIANCE_CACHE_DEFERRED_LATE)
#define PATH_TRACE_RADIANCE_CACHE_EARLY_RECORD 1
#else
#define PATH_TRACE_RADIANCE_CACHE_EARLY_RECORD 0
#endif

cbuffer RadianceCacheD6LearningConstants : register(b3)
{
    RadianceCacheD6Binding radianceCache;
};

// Paths this ray generation thread has begun: the current path's index among the pixel's samples of the dispatch.
static uint g_pathTraceRadianceCachePaths = 0u;

// The current path's deferred training record, or RADIANCE_CACHE_D6_NO_SLOT if the binding's buffer has no room for it
// (it is then never written).
uint PathTraceRadianceCacheRecord()
{
    uint path = g_pathTraceRadianceCachePaths - 1u;
#ifdef PATH_TRACE_RADIANCE_CACHE_TRAINING
    // A compact record per training thread.
    uint record = DispatchRaysIndex().x * samplesPerFrame + path;
#else
    uint2 pixel = DispatchRaysIndex().xy;
    uint record = (pixel.y * resolution.x + pixel.x) * samplesPerFrame + path;
#endif
    return path < samplesPerFrame && record < radianceCache.recordCapacity ? record : RADIANCE_CACHE_D6_NO_SLOT;
}

struct PathTraceRadianceCacheHarvest
{
    // x1 is an eligible, keyed cache vertex of a raster-primary path in a learning frame.
    bool active;
    // x1's continuation direction was sampled (otherwise the sample is zero).
    bool sampled;
    uint2 key;
    // x1's facing geometric normal: the entry's frame.
    float3 normal;
    float3 direction;
    float pdf;
    // The path throughput after x1's continuation, relative to the throughput at it.
    float3 suffixThroughput;
    float3 suffixRadiance;
#ifdef PATH_TRACE_RADIANCE_CACHE_TRAINING
    // SHARC.6A primary-class sample: the same estimator at x0 (binding primaryRecordOffset != 0). Its suffix is
    // everything the path adds after x0's continuation except that segment's miss (x0's direct light).
    bool primaryActive;
    bool primarySampled;
    uint2 primaryKey;
    float3 primaryNormal;
    float3 primaryDirection;
    float primaryPdf;
    float3 primaryThroughput;
    float3 primaryRadiance;
#endif
};

PathTraceRadianceCacheHarvest PathTraceRadianceCacheHarvestBegin()
{
    PathTraceRadianceCacheHarvest harvest = (PathTraceRadianceCacheHarvest)0;
    harvest.suffixThroughput = float3(1.0f, 1.0f, 1.0f);
#ifdef PATH_TRACE_RADIANCE_CACHE_TRAINING
    harvest.primaryThroughput = float3(1.0f, 1.0f, 1.0f);
#endif
    ++g_pathTraceRadianceCachePaths;
    return harvest;
}

// After vertex `vertex`'s surface is built.
void PathTraceRadianceCacheHarvestVertex(inout PathTraceRadianceCacheHarvest harvest, uint primary, uint vertex,
    float3 position, PathTraceSurface surface)
{
#ifdef PATH_TRACE_RADIANCE_CACHE_TRAINING
    // SHARC.6A: x0's eligibility is x1's rule; its key is the primary class's.
    if(vertex == 0u && primary == PATH_TRACE_PRIMARY_RASTER && radianceCache.learning != 0u &&
        radianceCache.primaryRecordOffset != 0u && !surface.deltaSpecular && surface.material.model <= 1u &&
        surface.material.metallic <= 0.1f)
    {
        uint level;
        harvest.primaryActive = RadianceCacheD6MakeClassKey(radianceCache, position, surface.geometricNormal, true,
            harvest.primaryKey, level);
        harvest.primaryNormal = surface.geometricNormal;
    }
#endif

    if(vertex != 1u || primary != PATH_TRACE_PRIMARY_RASTER || radianceCache.learning == 0u)
    {
        return;
    }

    // SHARC.4D-P profiling: 1 in learningThinning 8 x 8 pixel blocks learn this frame. The choice depends only on the
    // pixel block and the frame, never on the path, so the selected samples keep their meaning.
    if(radianceCache.learningThinning > 1u)
    {
        uint2 block = DispatchRaysIndex().xy >> 3u;
        uint blockHash = RadianceCacheD6Mix(block.x ^ RadianceCacheD6Mix(block.y ^
            RadianceCacheD6Mix(radianceCache.frameIndex ^ 0x3C6EF372u)));

        if(blockHash % radianceCache.learningThinning != 0u)
        {
            return;
        }
    }

    if(radianceCache.harvestStop == 4u || radianceCache.harvestStop == 5u)
    {
        return;
    }

    if(surface.deltaSpecular || surface.material.model > 1u || surface.material.metallic > 0.1f)
    {
        return;
    }

    harvest.active = RadianceCacheD6MakeBindingKey(radianceCache, position, surface.geometricNormal, harvest.key);
    harvest.normal = surface.geometricNormal;

#if PATH_TRACE_RADIANCE_CACHE_EARLY_RECORD
    uint record = PathTraceRadianceCacheRecord();

    if(harvest.active && record != RADIANCE_CACHE_D6_NO_SLOT)
    {
        RWByteAddressBuffer records = ResourceDescriptorHeap[radianceCache.recordsIndex];
        uint base = RadianceCacheD6RecordAddress(record);
        records.Store4(base, uint4(harvest.key, asuint(harvest.normal.xy)));
        records.Store(base + 16u, asuint(harvest.normal.z));
    }
#endif

#ifndef PATH_TRACE_RADIANCE_CACHE_DEFERRED
    // SHARC.4D-P profiling, stop 6: what a vertex-1 lookup of the published entry costs here (tags, full key, 24
    // published words). The result only feeds a never-taken store; the path is then inactive, so nothing is inserted.
    if(radianceCache.harvestStop == 6u && harvest.active)
    {
        RWByteAddressBuffer tags = ResourceDescriptorHeap[radianceCache.tagsIndex];
        RWByteAddressBuffer keys = ResourceDescriptorHeap[radianceCache.keysIndex];
        RWByteAddressBuffer published = ResourceDescriptorHeap[radianceCache.publishedIndex];
        uint firstSlot = RadianceCacheD6Bucket(harvest.key, radianceCache.bucketCount) * RADIANCE_CACHE_D6_BUCKET_SLOTS;
        uint tag = RadianceCacheD6Tag(harvest.key);
        float found = 0.0f;

        for(uint index = 0u; index < RADIANCE_CACHE_D6_BUCKET_SLOTS; ++index)
        {
            uint slot = firstSlot + index;

            if(tags.Load(slot * 4u) == tag && all(keys.Load2(slot * 8u) == harvest.key))
            {
                [unroll]
                for(uint word = 0u; word < RADIANCE_CACHE_D6_PUBLISHED_WORDS; word += 4u)
                {
                    uint4 words = published.Load4((slot * RADIANCE_CACHE_D6_PUBLISHED_WORDS + word) * 4u);
                    found += asfloat(words.x) + asfloat(words.y) + asfloat(words.z) + asfloat(words.w);
                }

                break;
            }
        }

        if(found == -1234.5f)
        {
            RWByteAddressBuffer statistics = ResourceDescriptorHeap[radianceCache.statisticsIndex];
            statistics.InterlockedAdd(RADIANCE_CACHE_D6_STATISTIC_REJECTED_SAMPLES * 4u, 1u);
        }

        harvest.active = false;
    }
#endif
}

// Next-event estimation at vertex `vertex` (added to the image as throughput * direct).
void PathTraceRadianceCacheHarvestDirect(inout PathTraceRadianceCacheHarvest harvest, uint vertex, float3 direct)
{
#ifdef PATH_TRACE_RADIANCE_CACHE_TRAINING
    if(harvest.primaryActive && vertex >= 1u)
    {
        harvest.primaryRadiance += harvest.primaryThroughput * direct;
    }
#endif

    if(harvest.active && vertex >= 2u)
    {
        harvest.suffixRadiance += harvest.suffixThroughput * direct;
    }
}

// A miss found while looking for vertex `vertex` (the segment leaving vertex - 1), added as throughput * radiance.
void PathTraceRadianceCacheHarvestMiss(inout PathTraceRadianceCacheHarvest harvest, uint vertex, float3 missRadiance)
{
#ifdef PATH_TRACE_RADIANCE_CACHE_TRAINING
    if(harvest.primaryActive && vertex >= 2u)
    {
        harvest.primaryRadiance += harvest.primaryThroughput * missRadiance;
    }
#endif

    if(harvest.active && vertex >= 3u)
    {
        harvest.suffixRadiance += harvest.suffixThroughput * missRadiance;
    }
}

// Vertex `vertex`'s BSDF sample, after the path throughput took its weight.
void PathTraceRadianceCacheHarvestContinue(inout PathTraceRadianceCacheHarvest harvest, uint vertex,
    PathTraceBSDFSample bsdfSample)
{
#ifdef PATH_TRACE_RADIANCE_CACHE_TRAINING
    if(harvest.primaryActive)
    {
        if(vertex == 0u)
        {
            harvest.primarySampled = !bsdfSample.delta;
            harvest.primaryDirection = bsdfSample.direction;
            harvest.primaryPdf = bsdfSample.pdf;
        }
        else
        {
            harvest.primaryThroughput *= bsdfSample.weight;
        }
    }
#endif

    if(!harvest.active)
    {
        return;
    }

    if(vertex == 1u)
    {
        harvest.sampled = !bsdfSample.delta;
        harvest.direction = bsdfSample.direction;
        harvest.pdf = bsdfSample.pdf;

#if PATH_TRACE_RADIANCE_CACHE_EARLY_RECORD
        uint record = PathTraceRadianceCacheRecord();

        if(record != RADIANCE_CACHE_D6_NO_SLOT)
        {
            RWByteAddressBuffer records = ResourceDescriptorHeap[radianceCache.recordsIndex];
            records.Store4(RadianceCacheD6RecordAddress(record) + RADIANCE_CACHE_D6_RECORD_DIRECTION_WORD * 4u,
                uint4(asuint(harvest.direction), asuint(harvest.pdf)));
        }
#endif
    }
    else if(vertex >= 2u)
    {
        harvest.suffixThroughput *= bsdfSample.weight;
    }
}

// Roulette survival before continuing past vertex `vertex`, after the path throughput was divided by it.
void PathTraceRadianceCacheHarvestSurvive(inout PathTraceRadianceCacheHarvest harvest, uint vertex, float survival)
{
#ifdef PATH_TRACE_RADIANCE_CACHE_TRAINING
    if(harvest.primaryActive)
    {
        harvest.primaryThroughput /= survival;
    }
#endif

    if(harvest.active && vertex >= 1u)
    {
        harvest.suffixThroughput /= survival;
    }
}

// SHARC.4D-Q deferred insertion: the path's training record with plain stores, its stamp last. The deferred library
// stored x1's part at vertex 1 already.
void PathTraceRadianceCacheHarvestStoreRecord(PathTraceRadianceCacheHarvest harvest)
{
    uint record = PathTraceRadianceCacheRecord();

#ifdef PATH_TRACE_RADIANCE_CACHE_TRAINING
    // SHARC.6A: the primary-class record, whole, at the same index past the x1 records.
    uint primaryRecord = record + radianceCache.primaryRecordOffset;

    if(harvest.primaryActive && record != RADIANCE_CACHE_D6_NO_SLOT && primaryRecord < radianceCache.recordCapacity)
    {
        RWByteAddressBuffer primaryRecords = ResourceDescriptorHeap[radianceCache.recordsIndex];
        uint primaryBase = RadianceCacheD6RecordAddress(primaryRecord);
        primaryRecords.Store4(primaryBase, uint4(harvest.primaryKey, asuint(harvest.primaryNormal.xy)));
        primaryRecords.Store4(primaryBase + 16u, uint4(asuint(harvest.primaryNormal.z), asuint(harvest.primaryDirection)));
        primaryRecords.Store4(primaryBase + 32u, uint4(asuint(harvest.primaryPdf), asuint(harvest.primaryRadiance)));
        primaryRecords.Store(RadianceCacheD6StampAddress(radianceCache, primaryRecord),
            (radianceCache.recordSequence << 1u) | (harvest.primarySampled ? 1u : 0u));
    }
#endif

    if(!harvest.active || record == RADIANCE_CACHE_D6_NO_SLOT)
    {
        return;
    }

    RWByteAddressBuffer records = ResourceDescriptorHeap[radianceCache.recordsIndex];
    uint base = RadianceCacheD6RecordAddress(record);

#if PATH_TRACE_RADIANCE_CACHE_EARLY_RECORD
    records.Store3(base + RADIANCE_CACHE_D6_RECORD_RADIANCE_WORD * 4u, asuint(harvest.suffixRadiance));
#else
    records.Store4(base, uint4(harvest.key, asuint(harvest.normal.xy)));
    records.Store4(base + 16u, uint4(asuint(harvest.normal.z), asuint(harvest.direction)));
    records.Store4(base + 32u, uint4(asuint(harvest.pdf), asuint(harvest.suffixRadiance)));
#endif

    records.Store(RadianceCacheD6StampAddress(radianceCache, record),
        (radianceCache.recordSequence << 1u) | (harvest.sampled ? 1u : 0u));
}

// At the end of the path, by every lane that traced one (wave statistics): inserts the sample if the generation header
// names this frame's epoch and identity. Deferred learning stores the sample's record instead.
void PathTraceRadianceCacheHarvestEnd(PathTraceRadianceCacheHarvest harvest)
{
#ifdef PATH_TRACE_RADIANCE_CACHE_DEFERRED
    PathTraceRadianceCacheHarvestStoreRecord(harvest);
#else
    if(radianceCache.deferred != 0u)
    {
        PathTraceRadianceCacheHarvestStoreRecord(harvest);
        return;
    }

    // SHARC.4D-P profiling, stop 7: the raw sample record (key, direction, normal, pdf, suffix radiance; 12 words)
    // with plain stores at the path's pixel, as a deferred insertion pass would read it. Nothing else runs.
    if(radianceCache.harvestStop == 7u)
    {
        if(harvest.active)
        {
            RWByteAddressBuffer scratch = ResourceDescriptorHeap[radianceCache.accumulationIndex];
            uint2 pixel = DispatchRaysIndex().xy;
            uint records = radianceCache.bucketCount * RADIANCE_CACHE_D6_BUCKET_SLOTS * RADIANCE_CACHE_D6_ACCUMULATION_WORDS / 12u;
            uint base = ((pixel.y * DispatchRaysDimensions().x + pixel.x) % records) * 48u;
            scratch.Store4(base, uint4(harvest.key, asuint(harvest.direction.xy)));
            scratch.Store4(base + 16u, uint4(asuint(harvest.direction.z), asuint(harvest.normal)));
            scratch.Store4(base + 32u, uint4(asuint(harvest.pdf), asuint(harvest.suffixRadiance)));
        }

        return;
    }

    // SHARC.4D-P profiling, stops 5 and 6: no insertion and no epilogue.
    if(radianceCache.harvestStop >= 5u)
    {
        return;
    }

    // SHARC.4D-P profiling, stop 3: the harvest state is kept (one never-taken store depends on all of it) but no
    // header check, weights or insertion run.
    if(radianceCache.harvestStop == 3u)
    {
        float state = harvest.suffixRadiance.x + harvest.suffixRadiance.y + harvest.suffixRadiance.z +
            harvest.suffixThroughput.x + harvest.direction.x + harvest.normal.y + harvest.pdf + float(harvest.key.x ^ harvest.key.y);

        if(harvest.active && harvest.sampled && state == -1234.5f)
        {
            RWByteAddressBuffer statistics = ResourceDescriptorHeap[radianceCache.statisticsIndex];
            statistics.InterlockedAdd(RADIANCE_CACHE_D6_STATISTIC_REJECTED_SAMPLES * 4u, 1u);
        }

        return;
    }

    bool accepted = RadianceCacheD6BindingAccepted(radianceCache);
    bool active = harvest.active && accepted;
    bool sampled = active && harvest.sampled;
    float3 values[RADIANCE_CACHE_D6_MOMENTS];
    RadianceCacheD6SampleValues(radianceCache, sampled, harvest.normal, harvest.direction, harvest.pdf,
        harvest.suffixRadiance, values);

    bool zero = !sampled || all(harvest.suffixRadiance == 0.0f);
    RadianceCacheD6CountWave(radianceCache, RADIANCE_CACHE_D6_STATISTIC_HEADER_REJECTED, harvest.active && !accepted);

    // SHARC.4D-P profiling, stop 2: the sample is computed (one never-taken store depends on it), not inserted.
    if(radianceCache.harvestStop == 2u)
    {
        float sample = 0.0f;

        [unroll]
        for(uint moment = 0u; moment < RADIANCE_CACHE_D6_MOMENTS; ++moment)
        {
            sample += values[moment].x + values[moment].y + values[moment].z;
        }

        if(active && sample == -1234.5f)
        {
            RWByteAddressBuffer statistics = ResourceDescriptorHeap[radianceCache.statisticsIndex];
            statistics.InterlockedAdd(RADIANCE_CACHE_D6_STATISTIC_REJECTED_SAMPLES * 4u, 1u);
        }

        return;
    }

    // Stop 1 claims the slot but adds nothing to it.
    RadianceCacheD6AddSample(radianceCache, active, harvest.key, values, zero, radianceCache.harvestStop != 1u);
#endif
}

#endif
