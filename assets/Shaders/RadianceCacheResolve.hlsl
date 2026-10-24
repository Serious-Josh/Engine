// SHARC.2 World Radiance Cache resolve (publication) and clear passes. See RadianceCache.hlsli for the storage and
// phase contracts.
//
// CSResolve, one thread per bucket, runs after the update dispatch and a UAV barrier, and before any query. It folds
// each slot's exact frame accumulation into the published entry, discards accumulations it cannot trust, evicts stale
// entries, then writes the bucket back with its surviving entries sorted by key and its accumulation zeroed. Sorting
// makes the resolved contents independent of which slot a thread happened to claim.
//
// CSClear zero-fills clearWordCount words of the raw buffer clearIndex (all-zero is the valid empty cache).

#include "RadianceCache.hlsli"

cbuffer RadianceCacheConstants : register(b0)
{
    RadianceCacheBinding cache;

    uint clearIndex;
    uint clearWordCount;
    uint2 clearPadding;
};

struct RadianceCacheSlot
{
    uint tag;
    uint2 key;
    uint4 publishedFirst;
    uint4 publishedSecond;
};

// Key order: high word, then low word.
bool RadianceCacheKeyLess(uint2 left, uint2 right)
{
    return left.y < right.y || (left.y == right.y && left.x < right.x);
}

RadianceCacheSlot RadianceCacheEmptySlot()
{
    return (RadianceCacheSlot)0;
}

// Folds a trusted, non-empty frame accumulation into the published entry. precise keeps the arithmetic exactly as
// written (no contraction), so a CPU model performing the same float operations reproduces it.
void RadianceCachePublish(inout RadianceCacheSlot slot, uint4 sumsLow, uint4 sumsHigh, uint count)
{
    precise float frameScale = float(count) * RADIANCE_CACHE_FIXED_POINT_SCALE;
    precise float3 frameMean = float3(RadianceCacheFixedPointToFloat(uint2(sumsLow.x, sumsHigh.x)),
        RadianceCacheFixedPointToFloat(uint2(sumsLow.y, sumsHigh.y)),
        RadianceCacheFixedPointToFloat(uint2(sumsLow.z, sumsHigh.z))) / frameScale;

    float3 previousValue = asfloat(slot.publishedFirst.xyz);
    float previousCount = asfloat(slot.publishedFirst.w);
    float relativeVariance = asfloat(slot.publishedSecond.x);
    uint flags = slot.publishedSecond.w;
    bool firstPublication = (flags & RADIANCE_CACHE_FLAG_PUBLISHED) == 0u;

    // History weight: the previous count, already capped, against this frame's samples. The first publication is the
    // frame mean exactly.
    precise float frameCount = float(count);
    precise float blend = firstPublication ? 1.0f : frameCount / (previousCount + frameCount);
    precise float3 value = previousValue + (frameMean - previousValue) * blend;
    precise float sampleCount = min(previousCount + frameCount, cache.maximumSampleCount);

    // Relative variance of one sample, from how far this frame's mean landed from the history (count * deviation^2
    // estimates the per-sample variance). Undefined until a second frame arrives.
    if(!firstPublication)
    {
        const float3 luminanceWeights = float3(0.2126f, 0.7152f, 0.0722f);
        precise float deviation = (dot(frameMean, luminanceWeights) - dot(previousValue, luminanceWeights)) /
            max(abs(dot(value, luminanceWeights)), 1.0e-6f);
        precise float frameVariance = frameCount * deviation * deviation;
        bool varianceValid = (flags & RADIANCE_CACHE_FLAG_VARIANCE_VALID) != 0u;
        relativeVariance = varianceValid ? relativeVariance + (frameVariance - relativeVariance) * cache.varianceBlend :
            frameVariance;
        flags |= RADIANCE_CACHE_FLAG_VARIANCE_VALID;
    }

    if(firstPublication)
    {
        slot.publishedSecond.z = cache.frameIndex;
    }

    slot.publishedFirst = uint4(asuint(value), asuint(sampleCount));
    slot.publishedSecond.x = asuint(relativeVariance);
    slot.publishedSecond.y = cache.frameIndex;
    slot.publishedSecond.w = flags | RADIANCE_CACHE_FLAG_PUBLISHED;
}

[numthreads(64, 1, 1)]
void CSResolve(uint3 dispatchThread : SV_DispatchThreadID)
{
    uint bucket = dispatchThread.x;

    if(bucket >= cache.bucketCount)
    {
        return;
    }

    RWByteAddressBuffer tags = ResourceDescriptorHeap[cache.tagsIndex];
    RWByteAddressBuffer keys = ResourceDescriptorHeap[cache.keysIndex];
    RWByteAddressBuffer accumulation = ResourceDescriptorHeap[cache.accumulationIndex];
    RWByteAddressBuffer published = ResourceDescriptorHeap[cache.publishedIndex];
    RWByteAddressBuffer bucketFlags = ResourceDescriptorHeap[cache.bucketFlagsIndex];
    RWByteAddressBuffer statistics = ResourceDescriptorHeap[cache.statisticsIndex];

    uint firstSlot = bucket * RADIANCE_CACHE_BUCKET_SLOTS;
    bool overflowed = (bucketFlags.Load(bucket * 4u) & 1u) != 0u;

    RadianceCacheSlot survivors[RADIANCE_CACHE_BUCKET_SLOTS];
    uint survivorCount = 0u;
    uint occupied = 0u;
    uint publishedSlots = 0u;
    uint evicted = 0u;
    uint checksumRejected = 0u;
    uint overflowDiscarded = 0u;
    uint resolvedSamples = 0u;
    uint maximumSlotSamples = 0u;

    // Stage 1: decide each slot's fate.
    for(uint index = 0u; index < RADIANCE_CACHE_BUCKET_SLOTS; ++index)
    {
        uint slotIndex = firstSlot + index;
        RadianceCacheSlot slot = RadianceCacheEmptySlot();
        slot.tag = tags.Load(slotIndex * 4u);

        if(slot.tag == 0u)
        {
            continue;
        }

        slot.key = keys.Load2(slotIndex * 8u);
        uint publishedBase = slotIndex * RADIANCE_CACHE_PUBLISHED_WORDS * 4u;
        slot.publishedFirst = published.Load4(publishedBase);
        slot.publishedSecond = published.Load4(publishedBase + 16u);

        uint accumulationBase = slotIndex * RADIANCE_CACHE_ACCUMULATION_WORDS * 4u;
        uint4 accumulationFirst = accumulation.Load4(accumulationBase);
        uint4 accumulationSecond = accumulation.Load4(accumulationBase + 16u);
        uint4 sumsLow = uint4(accumulationFirst.x, accumulationFirst.z, accumulationSecond.x, 0u);
        uint4 sumsHigh = uint4(accumulationFirst.y, accumulationFirst.w, accumulationSecond.y, 0u);
        uint count = accumulationSecond.z;
        uint checksum = accumulationSecond.w;

        bool wasPublished = (slot.publishedSecond.w & RADIANCE_CACHE_FLAG_PUBLISHED) != 0u;
        bool keep = true;

        if(count > 0u)
        {
            resolvedSamples += count;
            maximumSlotSamples = max(maximumSlotSamples, count);

            // Every sample adds its own key's odd checksum term, so a different key that shared the tag (or a torn key)
            // breaks count * term with overwhelming probability. Its frame is discarded; a new slot goes with it.
            bool checksumValid = checksum == count * RadianceCacheChecksumTerm(slot.key);

            if(!checksumValid)
            {
                ++checksumRejected;
                keep = wasPublished;
            }
            else if(overflowed && !wasPublished)
            {
                // Which new keys won the bucket's last slots depended on thread order; dropping every new key of an
                // overflowed bucket keeps the resolved contents order-independent.
                ++overflowDiscarded;
                keep = false;
            }
            else
            {
                RadianceCachePublish(slot, sumsLow, sumsHigh, count);
                ++publishedSlots;
            }
        }
        else if(!wasPublished || cache.frameIndex - slot.publishedSecond.y > cache.staleFrames)
        {
            // An unpublished slot without samples cannot be produced by an update; it is dropped defensively.
            ++evicted;
            keep = false;
        }

        if(keep)
        {
            survivors[survivorCount] = slot;
            ++survivorCount;
        }
    }

    // Stage 2: sort the survivors by key (insertion sort over at most eight entries).
    for(uint sorted = 1u; sorted < survivorCount; ++sorted)
    {
        RadianceCacheSlot moving = survivors[sorted];
        uint position = sorted;

        while(position > 0u && RadianceCacheKeyLess(moving.key, survivors[position - 1u].key))
        {
            survivors[position] = survivors[position - 1u];
            --position;
        }

        survivors[position] = moving;
    }

    // Stage 3: write the bucket back, survivors first, and zero every accumulation and the overflow flag.
    for(uint index = 0u; index < RADIANCE_CACHE_BUCKET_SLOTS; ++index)
    {
        uint slotIndex = firstSlot + index;
        RadianceCacheSlot slot = RadianceCacheEmptySlot();

        if(index < survivorCount)
        {
            slot = survivors[index];
        }

        uint publishedBase = slotIndex * RADIANCE_CACHE_PUBLISHED_WORDS * 4u;
        uint accumulationBase = slotIndex * RADIANCE_CACHE_ACCUMULATION_WORDS * 4u;

        tags.Store(slotIndex * 4u, slot.tag);
        keys.Store2(slotIndex * 8u, slot.key);
        published.Store4(publishedBase, slot.publishedFirst);
        published.Store4(publishedBase + 16u, slot.publishedSecond);
        accumulation.Store4(accumulationBase, uint4(0u, 0u, 0u, 0u));
        accumulation.Store4(accumulationBase + 16u, uint4(0u, 0u, 0u, 0u));
    }

    bucketFlags.Store(bucket * 4u, 0u);
    occupied = survivorCount;

    // Stage 4: statistics, once per wave.
    uint waveOccupied = WaveActiveSum(occupied);
    uint wavePublished = WaveActiveSum(publishedSlots);
    uint waveEvicted = WaveActiveSum(evicted);
    uint waveChecksumRejected = WaveActiveSum(checksumRejected);
    uint waveOverflowDiscarded = WaveActiveSum(overflowDiscarded);
    uint waveResolvedSamples = WaveActiveSum(resolvedSamples);
    uint waveMaximumSlotSamples = WaveActiveMax(maximumSlotSamples);
    uint waveOverflowedBuckets = WaveActiveCountBits(overflowed);

    if(WaveIsFirstLane())
    {
        statistics.InterlockedAdd(RADIANCE_CACHE_STATISTIC_OCCUPIED * 4u, waveOccupied);
        statistics.InterlockedAdd(RADIANCE_CACHE_STATISTIC_PUBLISHED * 4u, wavePublished);
        statistics.InterlockedAdd(RADIANCE_CACHE_STATISTIC_EVICTED * 4u, waveEvicted);
        statistics.InterlockedAdd(RADIANCE_CACHE_STATISTIC_CHECKSUM_REJECTED * 4u, waveChecksumRejected);
        statistics.InterlockedAdd(RADIANCE_CACHE_STATISTIC_OVERFLOW_DISCARDED * 4u, waveOverflowDiscarded);
        statistics.InterlockedAdd(RADIANCE_CACHE_STATISTIC_RESOLVED_SAMPLES * 4u, waveResolvedSamples);
        statistics.InterlockedMax(RADIANCE_CACHE_STATISTIC_MAXIMUM_SLOT_SAMPLES * 4u, waveMaximumSlotSamples);
        statistics.InterlockedAdd(RADIANCE_CACHE_STATISTIC_OVERFLOWED_BUCKETS * 4u, waveOverflowedBuckets);
    }
}

[numthreads(64, 1, 1)]
void CSClear(uint3 dispatchThread : SV_DispatchThreadID)
{
    uint firstWord = dispatchThread.x * 4u;

    if(firstWord >= clearWordCount)
    {
        return;
    }

    RWByteAddressBuffer target = ResourceDescriptorHeap[clearIndex];

    // Word counts are multiples of four (every buffer is a whole number of 16-byte rows).
    target.Store4(firstWord * 4u, uint4(0u, 0u, 0u, 0u));
}
