// SHARC.2 World Radiance Cache primitives: Engine's own spatially hashed cache of world-space values, keyed by a fixed
// world grid and the side of the surface being viewed. Experimental and diagnostics-only: no path-tracing library
// includes this file. RadianceCacheResolve.hlsl publishes and clears; later ray generations update and query through
// the functions below.
//
// Storage (all raw buffers, slot s of bucket b = s / 8):
//   tags         uint per slot. 0 = empty. Written only by a claim (0 -> tag, InterlockedCompareExchange during an
//                update dispatch) and by the resolve pass (eviction and compaction). A non-zero tag never changes during
//                an update dispatch, so a tag seen there is final for the dispatch.
//   keys         uint2 per slot: the full 64-bit key. Written once per claim, by the claiming thread only, and by
//                resolve. Never read during an update dispatch, so the claim's plain store needs no ordering beyond the
//                UAV barrier before resolve.
//   accumulation 8 words per slot: three signed 64-bit fixed-point sums (lo, hi), the sample count and the checksum
//                (sum of the key's odd checksum term). Written only with atomics during an update; read and zeroed by
//                resolve.
//   published    8 words per slot: float3 value, float sample count, float relative variance, last update frame, first
//                frame, flags. Written only by resolve; the only data a query reads.
//   bucketFlags  uint per bucket: bit 0 set when a sample of this frame found no slot in the bucket.
//   statistics   RADIANCE_CACHE_STATISTIC_COUNT counters.
//
// Phases (one queue, separated by UAV barriers or transitions): clear -> update -> resolve -> query. Queries read only
// published entries, which only resolve writes, so a query cannot observe a partially accumulated or partially claimed
// entry. Resolve sorts each bucket's surviving entries by key, so the cache contents after resolve depend only on the
// multiset of samples, not on the order in which threads claimed slots.

#ifndef RADIANCE_CACHE_HLSLI
#define RADIANCE_CACHE_HLSLI

static const uint RADIANCE_CACHE_BUCKET_SLOTS = 8u;
static const uint RADIANCE_CACHE_NO_SLOT = 0xFFFFFFFFu;
static const uint RADIANCE_CACHE_ACCUMULATION_WORDS = 8u;
static const uint RADIANCE_CACHE_PUBLISHED_WORDS = 8u;

// Fixed point: value * 2^20, rounded to nearest even, as a signed 64-bit integer. Samples are clamped to
// +-2^23, so one sample is at most 2^43 and a frame of at most 2^19 samples per slot sums to at most 2^62.
static const float RADIANCE_CACHE_FIXED_POINT_SCALE = 1048576.0f;
static const float RADIANCE_CACHE_SAMPLE_LIMIT = 8388608.0f;

// Key layout: x = cellX (18) | cellY low 14 << 18; y = cellY high 4 | cellZ (18) << 4 | normal bin (4) << 22 |
// normal-axis sub-cell (2) << 26 | version (4) << 28. Cells are biased by 2^17; the version is never zero, so no
// valid key is zero.
static const int RADIANCE_CACHE_CELL_BIAS = 131072;
static const int RADIANCE_CACHE_CELL_LIMIT = 131071;
static const uint RADIANCE_CACHE_KEY_VERSION = 1u;

static const uint RADIANCE_CACHE_FLAG_PUBLISHED = 1u;
static const uint RADIANCE_CACHE_FLAG_VARIANCE_VALID = 2u;

// Statistics counters (uint each).
static const uint RADIANCE_CACHE_STATISTIC_SAMPLE_ATTEMPTS = 0u;
static const uint RADIANCE_CACHE_STATISTIC_REJECTED_SAMPLES = 1u;
static const uint RADIANCE_CACHE_STATISTIC_CLAMPED_SAMPLES = 2u;
static const uint RADIANCE_CACHE_STATISTIC_EXISTING_SLOT = 3u;
static const uint RADIANCE_CACHE_STATISTIC_CLAIMED_SLOTS = 4u;
static const uint RADIANCE_CACHE_STATISTIC_JOINED_CLAIMS = 5u;
static const uint RADIANCE_CACHE_STATISTIC_BUCKET_FULL = 6u;
static const uint RADIANCE_CACHE_STATISTIC_CLAIM_ATTEMPTS = 7u;
static const uint RADIANCE_CACHE_STATISTIC_OCCUPIED = 8u;
static const uint RADIANCE_CACHE_STATISTIC_PUBLISHED = 9u;
static const uint RADIANCE_CACHE_STATISTIC_EVICTED = 10u;
static const uint RADIANCE_CACHE_STATISTIC_CHECKSUM_REJECTED = 11u;
static const uint RADIANCE_CACHE_STATISTIC_OVERFLOW_DISCARDED = 12u;
static const uint RADIANCE_CACHE_STATISTIC_MAXIMUM_SLOT_SAMPLES = 13u;
static const uint RADIANCE_CACHE_STATISTIC_RESOLVED_SAMPLES = 14u;
static const uint RADIANCE_CACHE_STATISTIC_OVERFLOWED_BUCKETS = 15u;
static const uint RADIANCE_CACHE_STATISTIC_COUNT = 16u;

// The buffers and parameters of one cache. Layout matches RadianceCacheConstants (RadianceCache.h).
struct RadianceCacheBinding
{
    uint tagsIndex;
    uint keysIndex;
    uint accumulationIndex;
    uint publishedIndex;

    uint bucketFlagsIndex;
    uint statisticsIndex;
    uint bucketCount;
    uint frameIndex;

    // Read-only views for queries.
    uint tagsReadIndex;
    uint keysReadIndex;
    uint publishedReadIndex;
    uint staleFrames;

    float cellSize;
    float inverseCellSize;
    float maximumSampleCount;
    float varianceBlend;
};

// -----------------------------------------------------------------------------
// Keys
// -----------------------------------------------------------------------------

// Which quarter of [-extent, extent] coordinate lies in.
uint RadianceCacheQuarter(float coordinate, float extent)
{
    if(coordinate < -0.5f * extent)
    {
        return 0u;
    }

    if(coordinate < 0.0f)
    {
        return 1u;
    }

    if(coordinate < 0.5f * extent)
    {
        return 2u;
    }

    return 3u;
}

// The 16 octahedral bins of a normal (any length): 4 x 4 cells of the octahedral map. The map's coordinates are
// compared scaled by the L1 norm instead of divided by it, and precise fixes the operation order, so the bin is
// exactly reproducible (GPU division is not correctly rounded).
uint RadianceCacheNormalBin(float3 normal)
{
    float3 absolute = abs(normal);
    precise float extent = absolute.x + absolute.y + absolute.z;
    precise float u = normal.x;
    precise float v = normal.y;

    // Lower hemisphere: the octahedral fold, (1 - |p.yx|) * sign(p.xy) in L1-scaled coordinates.
    if(normal.z < 0.0f)
    {
        float signU = normal.x >= 0.0f ? 1.0f : -1.0f;
        float signV = normal.y >= 0.0f ? 1.0f : -1.0f;
        u = (extent - absolute.y) * signU;
        v = (extent - absolute.x) * signV;
    }

    return RadianceCacheQuarter(v, extent) * 4u + RadianceCacheQuarter(u, extent);
}

// The axis of largest magnitude; ties prefer x, then y.
uint RadianceCacheDominantAxis(float3 normal)
{
    float3 absolute = abs(normal);

    if(absolute.x >= absolute.y && absolute.x >= absolute.z)
    {
        return 0u;
    }

    if(absolute.y >= absolute.z)
    {
        return 1u;
    }

    return 2u;
}

// position in world space; normalFacingRay the surface normal turned towards the arriving ray (the side being
// viewed). False when the position is not finite or outside the addressable grid. Denormal positions flush to zero
// (D3D fp32 arithmetic), so they share cell 0 with +-0.
bool RadianceCacheMakeKey(float3 position, float3 normalFacingRay, float inverseCellSize, out uint2 key)
{
    key = uint2(0u, 0u);

    if(!all(isfinite(position)) || !all(isfinite(normalFacingRay)) || !any(normalFacingRay != 0.0f))
    {
        return false;
    }

    float3 scaled = position * inverseCellSize;
    float3 cellFloor = floor(scaled);

    if(any(cellFloor < float(-RADIANCE_CACHE_CELL_BIAS)) || any(cellFloor > float(RADIANCE_CACHE_CELL_LIMIT)))
    {
        return false;
    }

    int3 cell = int3(cellFloor);
    uint3 biased = uint3(cell + RADIANCE_CACHE_CELL_BIAS);

    // Quarter-cell position along the normal's dominant axis separates parallel surfaces closer than a cell.
    uint axis = RadianceCacheDominantAxis(normalFacingRay);
    float along = scaled[axis] - cellFloor[axis];
    uint subCell = uint(min(floor(along * 4.0f), 3.0f));
    uint normalBin = RadianceCacheNormalBin(normalFacingRay);

    key.x = biased.x | (biased.y << 18u);
    key.y = (biased.y >> 14u) | (biased.z << 4u) | (normalBin << 22u) | (subCell << 26u) |
        (RADIANCE_CACHE_KEY_VERSION << 28u);
    return true;
}

// -----------------------------------------------------------------------------
// Hashes: bucket, tag (never 0) and checksum term (always odd), independently mixed from the key.
// -----------------------------------------------------------------------------

uint RadianceCacheMix(uint value)
{
    value ^= value >> 16u;
    value *= 0x7FEB352Du;
    value ^= value >> 15u;
    value *= 0x846CA68Bu;
    value ^= value >> 16u;
    return value;
}

uint RadianceCacheBucketHash(uint2 key)
{
    return RadianceCacheMix(key.x ^ RadianceCacheMix(key.y ^ 0x9E3779B9u));
}

uint RadianceCacheTagHash(uint2 key)
{
    uint tag = RadianceCacheMix(key.y ^ RadianceCacheMix(key.x + 0x85EBCA6Bu));
    return max(tag, 1u);
}

uint RadianceCacheChecksumTerm(uint2 key)
{
    return RadianceCacheMix((key.x * 0x27D4EB2Du) ^ RadianceCacheMix(key.y ^ 0x165667B1u)) | 1u;
}

// Diagnostics may replace the bucket and tag functions (deliberate collisions); resolve never rehashes keys.
#ifndef RADIANCE_CACHE_BUCKET_OF
#define RADIANCE_CACHE_BUCKET_OF(key, bucketCount) (RadianceCacheBucketHash(key) & ((bucketCount) - 1u))
#endif

#ifndef RADIANCE_CACHE_TAG_OF
#define RADIANCE_CACHE_TAG_OF(key) RadianceCacheTagHash(key)
#endif

// -----------------------------------------------------------------------------
// Fixed point
// -----------------------------------------------------------------------------

// Two's complement 64-bit integer (x = low word, y = high word) of round(value * 2^20). |value| <= 2^23.
uint2 RadianceCacheToFixedPoint(float value)
{
    float magnitude = round(abs(value) * RADIANCE_CACHE_FIXED_POINT_SCALE);

    // magnitude < 2^44 holds at most 24 significant bits, so both parts are exact.
    float high = floor(magnitude * (1.0f / 4294967296.0f));
    float low = magnitude - high * 4294967296.0f;
    uint2 fixedPoint = uint2(uint(low), uint(high));

    if(value < 0.0f && any(fixedPoint != 0u))
    {
        fixedPoint.x = ~fixedPoint.x + 1u;
        fixedPoint.y = ~fixedPoint.y + (fixedPoint.x == 0u ? 1u : 0u);
    }

    return fixedPoint;
}

float RadianceCacheFixedPointToFloat(uint2 fixedPoint)
{
    bool negative = (fixedPoint.y & 0x80000000u) != 0u;

    if(negative)
    {
        fixedPoint.x = ~fixedPoint.x + 1u;
        fixedPoint.y = ~fixedPoint.y + (fixedPoint.x == 0u ? 1u : 0u);
    }

    float magnitude = float(fixedPoint.y) * 4294967296.0f + float(fixedPoint.x);
    return negative ? -magnitude : magnitude;
}

// Exact 64-bit addition from 32-bit atomics: each adder detects the wrap its own low-word addition caused and carries
// it, so the final (low, high) pair is the exact sum modulo 2^64 for any interleaving.
void RadianceCacheAtomicAdd64(RWByteAddressBuffer buffer, uint byteOffset, uint2 addend)
{
    uint originalLow;
    buffer.InterlockedAdd(byteOffset, addend.x, originalLow);
    uint carry = originalLow + addend.x < originalLow ? 1u : 0u;
    uint high = addend.y + carry;

    if(high != 0u)
    {
        buffer.InterlockedAdd(byteOffset + 4u, high);
    }
}

// -----------------------------------------------------------------------------
// Update phase
// -----------------------------------------------------------------------------

// Adds count to a statistic once per wave.
void RadianceCacheCountWave(RadianceCacheBinding binding, uint statistic, bool condition)
{
    uint count = WaveActiveCountBits(condition);

    if(WaveIsFirstLane() && count > 0u)
    {
        RWByteAddressBuffer statistics = ResourceDescriptorHeap[binding.statisticsIndex];
        statistics.InterlockedAdd(statistic * 4u, count);
    }
}

// The slot that owns key's tag in its bucket, claimed if absent; RADIANCE_CACHE_NO_SLOT when the bucket is full.
// Lock-free and bounded: at most 8 plain loads and 8 compare-exchanges, no thread ever waits for another.
//
// Every thread with a given tag probes the bucket's slots in the same order with compare-exchange, and slots only go
// from 0 to a tag during an update dispatch, so all of them stop at the same slot: the first whose value at its
// compare-exchange is 0 (claimed) or the tag (joined). The plain-load pre-scan finds tags published by earlier frames
// before any hole left by eviction can be claimed, so a tag occupies at most one slot.
uint RadianceCacheFindOrClaimSlot(RadianceCacheBinding binding, uint2 key, out uint claimAttempts, out bool claimed,
    out bool joined)
{
    claimAttempts = 0u;
    claimed = false;
    joined = false;

    RWByteAddressBuffer tags = ResourceDescriptorHeap[binding.tagsIndex];
    uint bucket = RADIANCE_CACHE_BUCKET_OF(key, binding.bucketCount);
    uint tag = RADIANCE_CACHE_TAG_OF(key);
    uint firstSlot = bucket * RADIANCE_CACHE_BUCKET_SLOTS;

    // Stage 1: a non-zero tag is final for this dispatch, so a match needs no atomic.
    for(uint index = 0u; index < RADIANCE_CACHE_BUCKET_SLOTS; ++index)
    {
        if(tags.Load((firstSlot + index) * 4u) == tag)
        {
            return firstSlot + index;
        }
    }

    // Stage 2: claim the first empty slot, or join the thread that claimed this tag first.
    for(uint index = 0u; index < RADIANCE_CACHE_BUCKET_SLOTS; ++index)
    {
        uint slot = firstSlot + index;
        uint previous;
        tags.InterlockedCompareExchange(slot * 4u, 0u, tag, previous);
        ++claimAttempts;

        if(previous == 0u)
        {
            // Only the claiming thread writes the key, exactly once, so the stored key is never torn.
            RWByteAddressBuffer keys = ResourceDescriptorHeap[binding.keysIndex];
            keys.Store2(slot * 8u, key);
            claimed = true;
            return slot;
        }

        if(previous == tag)
        {
            joined = true;
            return slot;
        }
    }

    RWByteAddressBuffer bucketFlags = ResourceDescriptorHeap[binding.bucketFlagsIndex];
    bucketFlags.InterlockedOr(bucket * 4u, 1u);
    return RADIANCE_CACHE_NO_SLOT;
}

// Adds one sample to key's frame accumulation. False when the value is not finite or the bucket is full. Values beyond
// RADIANCE_CACHE_SAMPLE_LIMIT are clamped (a bias, counted). Every lane of the wave must call this (wave statistics).
bool RadianceCacheAddKeyedSample(RadianceCacheBinding binding, bool active, uint2 key, float3 value)
{
    bool valid = active && all(isfinite(value));
    float3 clampedValue = clamp(value, -RADIANCE_CACHE_SAMPLE_LIMIT, RADIANCE_CACHE_SAMPLE_LIMIT);
    bool clamped = valid && any(clampedValue != value);
    uint slot = RADIANCE_CACHE_NO_SLOT;
    uint claimAttempts = 0u;
    bool claimed = false;
    bool joined = false;

    if(valid)
    {
        slot = RadianceCacheFindOrClaimSlot(binding, key, claimAttempts, claimed, joined);
    }

    bool stored = slot != RADIANCE_CACHE_NO_SLOT;

    if(stored)
    {
        RWByteAddressBuffer accumulation = ResourceDescriptorHeap[binding.accumulationIndex];
        uint base = slot * RADIANCE_CACHE_ACCUMULATION_WORDS * 4u;
        RadianceCacheAtomicAdd64(accumulation, base + 0u, RadianceCacheToFixedPoint(clampedValue.x));
        RadianceCacheAtomicAdd64(accumulation, base + 8u, RadianceCacheToFixedPoint(clampedValue.y));
        RadianceCacheAtomicAdd64(accumulation, base + 16u, RadianceCacheToFixedPoint(clampedValue.z));
        accumulation.InterlockedAdd(base + 24u, 1u);
        accumulation.InterlockedAdd(base + 28u, RadianceCacheChecksumTerm(key));
    }

    RadianceCacheCountWave(binding, RADIANCE_CACHE_STATISTIC_SAMPLE_ATTEMPTS, active);
    RadianceCacheCountWave(binding, RADIANCE_CACHE_STATISTIC_REJECTED_SAMPLES, active && !valid);
    RadianceCacheCountWave(binding, RADIANCE_CACHE_STATISTIC_CLAMPED_SAMPLES, clamped);
    RadianceCacheCountWave(binding, RADIANCE_CACHE_STATISTIC_EXISTING_SLOT, stored && !claimed && !joined);
    RadianceCacheCountWave(binding, RADIANCE_CACHE_STATISTIC_CLAIMED_SLOTS, claimed);
    RadianceCacheCountWave(binding, RADIANCE_CACHE_STATISTIC_JOINED_CLAIMS, joined);
    RadianceCacheCountWave(binding, RADIANCE_CACHE_STATISTIC_BUCKET_FULL, valid && !stored);

    uint waveClaimAttempts = WaveActiveSum(claimAttempts);

    if(WaveIsFirstLane() && waveClaimAttempts > 0u)
    {
        RWByteAddressBuffer statistics = ResourceDescriptorHeap[binding.statisticsIndex];
        statistics.InterlockedAdd(RADIANCE_CACHE_STATISTIC_CLAIM_ATTEMPTS * 4u, waveClaimAttempts);
    }

    return stored;
}

// The world-space form: keys the sample by position and the normal facing the arriving ray.
bool RadianceCacheAddSample(RadianceCacheBinding binding, bool active, float3 position, float3 normalFacingRay,
    float3 value)
{
    uint2 key;
    bool keyed = active && RadianceCacheMakeKey(position, normalFacingRay, binding.inverseCellSize, key);
    bool stored = RadianceCacheAddKeyedSample(binding, keyed, key, value);

    // The keyed form counted the keyed samples' attempts; samples without a key are attempts rejected here.
    RadianceCacheCountWave(binding, RADIANCE_CACHE_STATISTIC_REJECTED_SAMPLES, active && !keyed);
    RadianceCacheCountWave(binding, RADIANCE_CACHE_STATISTIC_SAMPLE_ATTEMPTS, active && !keyed);
    return stored;
}

// -----------------------------------------------------------------------------
// Query phase (read-only views of published data)
// -----------------------------------------------------------------------------

struct RadianceCacheEntry
{
    // A slot holds this key's tag; keyMatched only when the stored full key also equals the key.
    bool tagMatched;
    bool keyMatched;
    // keyMatched and published data exists.
    bool hit;
    uint slot;
    float3 value;
    float sampleCount;
    float relativeVariance;
    uint lastUpdateFrame;
    uint firstFrame;
    uint flags;
};

RadianceCacheEntry RadianceCacheFind(RadianceCacheBinding binding, uint2 key)
{
    RadianceCacheEntry entry = (RadianceCacheEntry)0;
    entry.slot = RADIANCE_CACHE_NO_SLOT;

    ByteAddressBuffer tags = ResourceDescriptorHeap[binding.tagsReadIndex];
    ByteAddressBuffer keys = ResourceDescriptorHeap[binding.keysReadIndex];
    uint bucket = RADIANCE_CACHE_BUCKET_OF(key, binding.bucketCount);
    uint tag = RADIANCE_CACHE_TAG_OF(key);
    uint firstSlot = bucket * RADIANCE_CACHE_BUCKET_SLOTS;

    for(uint index = 0u; index < RADIANCE_CACHE_BUCKET_SLOTS; ++index)
    {
        uint slot = firstSlot + index;

        if(tags.Load(slot * 4u) != tag)
        {
            continue;
        }

        entry.tagMatched = true;

        // The full key decides: an unrelated surface that shares the tag is a miss, never a hit.
        if(any(keys.Load2(slot * 8u) != key))
        {
            continue;
        }

        ByteAddressBuffer published = ResourceDescriptorHeap[binding.publishedReadIndex];
        uint base = slot * RADIANCE_CACHE_PUBLISHED_WORDS * 4u;
        uint4 first = published.Load4(base);
        uint4 second = published.Load4(base + 16u);
        entry.keyMatched = true;
        entry.slot = slot;
        entry.value = asfloat(first.xyz);
        entry.sampleCount = asfloat(first.w);
        entry.relativeVariance = asfloat(second.x);
        entry.lastUpdateFrame = second.y;
        entry.firstFrame = second.z;
        entry.flags = second.w;
        entry.hit = (entry.flags & RADIANCE_CACHE_FLAG_PUBLISHED) != 0u;
        return entry;
    }

    return entry;
}

#endif
