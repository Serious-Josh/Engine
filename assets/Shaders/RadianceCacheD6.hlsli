// SHARC.4C/4D production D6 radiance cache: layout, generation header, keys, atomic insertion and resolve
// (RadianceCacheD6.h owns the buffers). Independent of SHARC.2's RadianceCache.hlsli. The algorithms are SHARC.3a-V's
// validated diagnostic ones (src/Engine/tests/shaders/RadianceCacheMoments.hlsli with RADIANCE_MOMENT_COUNT 6, and
// the key, hash, fixed-point and claim code it includes), ported unchanged under RadianceCacheD6 names.
//
// Per slot (raw buffers, 16-byte rows):
//   tags          1 word: 0 = empty
//   keys          2 words: the 64-bit world-space key
//   accumulation  RADIANCE_CACHE_D6_ACCUMULATION_WORDS: 18 signed 64-bit fixed-point sums (lo, hi), sample count,
//                 checksum, 2 padding words; one frame's samples, zeroed by resolve
//   published     RADIANCE_CACHE_D6_PUBLISHED_WORDS: 18 floats, then sample count, relative variance, last update frame,
//                 first frame, flags, and the query keep-alive frame (the padding word; unused until refresh exists)
// Per bucket of RADIANCE_CACHE_D6_BUCKET_SLOTS slots: 1 flag word. Statistics: RADIANCE_CACHE_D6_STATISTIC_WORDS words.
//
// Generation header (4 words): scene-lighting epoch (lo, hi), configuration identity, state. Written last by the clear
// (after every entry buffer is zeroed and a UAV barrier), so a header that names an epoch proves the clear for that
// epoch completed before any later work on the queue. Every access must first check the header with its frame's epoch
// and identity (RadianceCacheD6HeaderMatches); any mismatch means stale or uncleared contents.
//
// Moments (D6): c_mk = Int g_mk(mu) cos(m phi) L dw and s_mk with sin, phi in the entry's canonical frame. One sample of
// direction w, mixture pdf p and hit-only incident radiance estimate S adds g_mk(mu_w) trig(m phi_w) S / p.

#ifndef RADIANCE_CACHE_D6_HLSLI
#define RADIANCE_CACHE_D6_HLSLI

static const uint RADIANCE_CACHE_D6_MOMENTS = 6u;
static const uint RADIANCE_CACHE_D6_VALUES = 18u;
static const uint RADIANCE_CACHE_D6_ACCUMULATION_WORDS = 40u;
static const uint RADIANCE_CACHE_D6_COUNT_WORD = 36u;
static const uint RADIANCE_CACHE_D6_PUBLISHED_WORDS = 24u;
static const uint RADIANCE_CACHE_D6_META_WORD = 18u;
static const uint RADIANCE_CACHE_D6_KEEP_ALIVE_WORD = 23u;
static const uint RADIANCE_CACHE_D6_BUCKET_SLOTS = 8u;
static const uint RADIANCE_CACHE_D6_STATISTIC_WORDS = 16u;
static const uint RADIANCE_CACHE_D6_HEADER_WORDS = 4u;
// Header state: 0 never cleared (a new allocation), 1 cleared for the named epoch.
static const uint RADIANCE_CACHE_D6_HEADER_CLEARED = 1u;
static const uint RADIANCE_CACHE_D6_NO_SLOT = 0xFFFFFFFFu;

static const float RADIANCE_CACHE_D6_FIXED_POINT_SCALE = 1048576.0f;
static const float RADIANCE_CACHE_D6_SAMPLE_LIMIT = 8388608.0f;
static const int RADIANCE_CACHE_D6_CELL_BIAS = 131072;
static const int RADIANCE_CACHE_D6_CELL_LIMIT = 131071;
static const uint RADIANCE_CACHE_D6_KEY_VERSION = 1u;
// SHARC.5F adaptive resolution (binding spatialMode 1): the key's version nibble is this flag | the level, so keys of
// different levels, and adaptive and uniform keys, never alias. Level l's cell edge is the binding's cell size * 2^l.
static const uint RADIANCE_CACHE_D6_ADAPTIVE_KEY_FLAG = 8u;
static const uint RADIANCE_CACHE_D6_ADAPTIVE_LEVELS = 4u;
// SHARC.6A: version-nibble bit of a primary-class (x0) entry, trained with the same estimator at path vertex 0 for the
// history prior. Its suffix holds one more bounce than an x1 entry's, so the two classes never share a key.
static const uint RADIANCE_CACHE_D6_PRIMARY_KEY_FLAG = 4u;
static const uint RADIANCE_CACHE_D6_FLAG_PUBLISHED = 1u;
static const uint RADIANCE_CACHE_D6_FLAG_VARIANCE_VALID = 2u;
static const uint RADIANCE_CACHE_D6_BASIS_SIZE = 129u;

// SHARC.4D-Q deferred insertion: training records, one per path of a learning dispatch, indexed by (pixel, path) and
// read by CSInsertRecords (RadianceCacheD6Insert.hlsl). The record buffer holds recordCapacity records of
// RADIANCE_CACHE_D6_RECORD_WORDS words, then one stamp word per record:
//   record  key (2), facing geometric normal (3), continuation direction (3), mixture pdf, hit-only suffix radiance (3)
//   stamp   (sequence << 1) | sampled, written after the record; sequence names the learning frame (never 0)
// A record counts only if its stamp names the binding's sequence, so stale records (an earlier frame, an inactive path,
// a recording never submitted) are never consumed, and a zero-valued sample is told apart from a missing one.
static const uint RADIANCE_CACHE_D6_RECORD_WORDS = 12u;
static const uint RADIANCE_CACHE_D6_RECORD_NORMAL_WORD = 2u;
static const uint RADIANCE_CACHE_D6_RECORD_DIRECTION_WORD = 5u;
static const uint RADIANCE_CACHE_D6_RECORD_RADIANCE_WORD = 9u;

// Statistics words (cleared at the start of every learning frame).
static const uint RADIANCE_CACHE_D6_STATISTIC_SAMPLE_ATTEMPTS = 0u;
static const uint RADIANCE_CACHE_D6_STATISTIC_REJECTED_SAMPLES = 1u;
static const uint RADIANCE_CACHE_D6_STATISTIC_CLAMPED_SAMPLES = 2u;
static const uint RADIANCE_CACHE_D6_STATISTIC_STORED_SAMPLES = 3u;
static const uint RADIANCE_CACHE_D6_STATISTIC_CLAIMED_SLOTS = 4u;
static const uint RADIANCE_CACHE_D6_STATISTIC_HEADER_REJECTED = 5u;
static const uint RADIANCE_CACHE_D6_STATISTIC_BUCKET_FULL = 6u;
static const uint RADIANCE_CACHE_D6_STATISTIC_ZERO_SAMPLES = 7u;
static const uint RADIANCE_CACHE_D6_STATISTIC_OCCUPIED = 8u;
static const uint RADIANCE_CACHE_D6_STATISTIC_PUBLISHED = 9u;
static const uint RADIANCE_CACHE_D6_STATISTIC_EVICTED = 10u;
static const uint RADIANCE_CACHE_D6_STATISTIC_CHECKSUM_REJECTED = 11u;
static const uint RADIANCE_CACHE_D6_STATISTIC_OVERFLOW_DISCARDED = 12u;
static const uint RADIANCE_CACHE_D6_STATISTIC_MAXIMUM_SLOT_SAMPLES = 13u;
static const uint RADIANCE_CACHE_D6_STATISTIC_RESOLVED_SAMPLES = 14u;
static const uint RADIANCE_CACHE_D6_STATISTIC_OVERFLOWED_BUCKETS = 15u;

// Must match RadianceCacheD6::Binding. The path tracer's cache library and resolve receive it as constants. The
// insertion binding never carries the published buffer: training cannot read cached radiance.
struct RadianceCacheD6Binding
{
    uint tagsIndex;
    uint keysIndex;
    uint accumulationIndex;
    uint publishedIndex;

    uint bucketFlagsIndex;
    uint statisticsIndex;
    uint headerIndex;
    uint basisIndex;

    uint bucketCount;
    uint frameIndex;
    uint epochLow;
    uint epochHigh;

    uint configurationIdentity;
    float inverseCellSize;
    float maximumSampleCount;
    float varianceBlend;

    uint staleFrames;
    uint learning;
    // SHARC.4D-P profiling controls (diagnostics; 0 = production behaviour). learningThinning: 1 in N 8 x 8 pixel
    // blocks learn per frame. harvestStop: 1 claims slots without accumulating, 2 computes the sample without
    // claiming, 3 tracks the harvest state only, 4 makes no path active, 5 also skips the end-of-path epilogue,
    // 6 looks the vertex-1 entry up (result discarded, nothing inserted), 7 stores each path's raw sample record with
    // plain stores (the cost of deferring insertion to another pass; the accumulation buffer is scratch).
    uint learningThinning;
    uint harvestStop;

    // SHARC.4D-Q deferred insertion: the record buffer (UAV), its capacity in records, this learning frame's stamp
    // sequence, and whether the ray generation stores records (1) instead of inserting at the end of each path (0).
    uint recordsIndex;
    uint recordCapacity;
    uint recordSequence;
    uint deferred;

    // SHARC.4D-R (diagnostics). queryMode, in the query library's binding: 0 the query hook returns at once, 1 it
    // keys eligible vertex-1 surfaces without a lookup, 2 it looks the entry up (result discarded), 3 it also counts
    // coverage into queryStatisticsIndex (RADIANCE_CACHE_D6_QUERY_*). trainingTile, in the training dispatch's binding:
    // one path per trainingTile x trainingTile pixel tile, trainingTilesX tiles per row (RadianceCacheD6TrainingPixel).
    uint queryMode;
    uint queryStatisticsIndex;
    uint trainingTile;
    uint trainingTilesX;

    // SHARC.4D-S termination probe (diagnostics; the query-termination library only). queryMode 4 ends an eligible
    // raster-primary path after x1's BSDF sample when its published entry has at least queryThreshold samples
    // (infinity: never, the probe's control); queryMode 5 ends every eligible path (the termination point's ceiling).
    // queryCounters 1 also counts coverage and terminations (RADIANCE_CACHE_D6_QUERY_*) in modes 4 and 5.
    float queryThreshold;
    uint queryCounters;
    uint2 queryPadding;

    // SHARC.5F: spatialMode 0 keys every surface on the uniform grid; 1 picks a level by the distance from anchor (the
    // frame's camera position, the same for the frame's training and queries; RadianceCacheD6MakeBindingKey).
    float3 anchor;
    uint spatialMode;

    // SHARC.6A history prior (0 = off). primaryRecordOffset, in a learning binding: where the training dispatch's
    // primary-class records start (one per path after the x1 records). priorOutputIndex, in a render query binding: the
    // prior texture (UAV) the extraction ray generation writes; priorWeight its full weight in history frames.
    // priorCounters 1 counts the prior's outcomes (RADIANCE_CACHE_D6_PRIOR_*) into priorCountersIndex.
    uint primaryRecordOffset;
    uint priorOutputIndex;
    float priorWeight;
    uint priorCountersIndex;
};

// SHARC.4D-R query coverage counters (one frame's eligible vertex-1 queries).
static const uint RADIANCE_CACHE_D6_QUERY_ELIGIBLE = 0u;
static const uint RADIANCE_CACHE_D6_QUERY_FOUND = 1u;
static const uint RADIANCE_CACHE_D6_QUERY_COUNT_64 = 2u;
static const uint RADIANCE_CACHE_D6_QUERY_COUNT_256 = 3u;
static const uint RADIANCE_CACHE_D6_QUERY_COUNT_1024 = 4u;
static const uint RADIANCE_CACHE_D6_QUERY_COUNT_4096 = 5u;
static const uint RADIANCE_CACHE_D6_QUERY_AGE_1 = 6u;
static const uint RADIANCE_CACHE_D6_QUERY_AGE_16 = 7u;
static const uint RADIANCE_CACHE_D6_QUERY_AGE_64 = 8u;
static const uint RADIANCE_CACHE_D6_QUERY_HEADER_REJECTED = 9u;
// SHARC.4D-S: hits with at least 16 samples, and paths the probe terminated.
static const uint RADIANCE_CACHE_D6_QUERY_COUNT_16 = 10u;
static const uint RADIANCE_CACHE_D6_QUERY_TERMINATED = 11u;
// SHARC.4E render: substitutions whose reconstruction had a negative channel (clamped to 0), and substitutions whose
// continuation missed (the miss term was added).
static const uint RADIANCE_CACHE_D6_QUERY_CLAMPED = 12u;
static const uint RADIANCE_CACHE_D6_QUERY_MISSED = 13u;
// SHARC.5E render (diagnostics): trusted eligible lookups that found no entry, split by whether the key's bucket had all
// RADIANCE_CACHE_D6_BUCKET_SLOTS slots taken by other keys (full) or a free slot (the key was never stored or evicted).
static const uint RADIANCE_CACHE_D6_QUERY_ABSENT_FULL = 14u;
static const uint RADIANCE_CACHE_D6_QUERY_ABSENT_FREE = 15u;
static const uint RADIANCE_CACHE_D6_QUERY_WORDS = 16u;

// SHARC.6A history-prior counters (priorCountersIndex). The ray generation counts surface pixels and unsupported ones;
// the temporal pass counts, among pixels whose diffuse history it rejected, the prior's use and absence reasons
// (PATH_TRACE_TEMPORAL_PRIOR_*, PathTraceTemporal.hlsl). Words 1 and 3-6 are unused.
static const uint RADIANCE_CACHE_D6_PRIOR_SURFACES = 0u;
static const uint RADIANCE_CACHE_D6_PRIOR_AVAILABLE = 1u;
static const uint RADIANCE_CACHE_D6_PRIOR_UNSUPPORTED = 2u;
static const uint RADIANCE_CACHE_D6_PRIOR_ABSENT = 3u;
static const uint RADIANCE_CACHE_D6_PRIOR_LOW_CONFIDENCE = 4u;
static const uint RADIANCE_CACHE_D6_PRIOR_COARSE = 5u;
static const uint RADIANCE_CACHE_D6_PRIOR_NONFINITE = 6u;
static const uint RADIANCE_CACHE_D6_PRIOR_REJECTED_HISTORY = 7u;
static const uint RADIANCE_CACHE_D6_PRIOR_USED = 8u;
// The used priors' weights, summed in 1/1024 units.
static const uint RADIANCE_CACHE_D6_PRIOR_WEIGHT_SUM = 9u;
static const uint RADIANCE_CACHE_D6_PRIOR_WORDS = 16u;
// The prior texture's alpha: the weight (> 0) or the negated reason it is absent.
static const float RADIANCE_CACHE_D6_PRIOR_CODE_UNSUPPORTED = -1.0f;
static const float RADIANCE_CACHE_D6_PRIOR_CODE_ABSENT = -2.0f;
static const float RADIANCE_CACHE_D6_PRIOR_CODE_LOW_CONFIDENCE = -3.0f;
static const float RADIANCE_CACHE_D6_PRIOR_CODE_COARSE = -4.0f;
static const float RADIANCE_CACHE_D6_PRIOR_CODE_NONFINITE = -5.0f;

uint RadianceCacheD6RecordAddress(uint record)
{
    return record * RADIANCE_CACHE_D6_RECORD_WORDS * 4u;
}

uint RadianceCacheD6StampAddress(RadianceCacheD6Binding binding, uint record)
{
    return (binding.recordCapacity * RADIANCE_CACHE_D6_RECORD_WORDS + record) * 4u;
}

bool RadianceCacheD6HeaderMatches(uint4 header, uint2 epoch, uint configurationIdentity)
{
    return header.w == RADIANCE_CACHE_D6_HEADER_CLEARED && header.x == epoch.x && header.y == epoch.y &&
        header.z == configurationIdentity;
}

bool RadianceCacheD6HeaderAccepts(ByteAddressBuffer header, uint2 epoch, uint configurationIdentity)
{
    return RadianceCacheD6HeaderMatches(header.Load4(0), epoch, configurationIdentity);
}

// The binding's header check (every access goes through it; the header stays in the UAV state).
bool RadianceCacheD6BindingAccepted(RadianceCacheD6Binding binding)
{
    RWByteAddressBuffer header = ResourceDescriptorHeap[binding.headerIndex];
    return RadianceCacheD6HeaderMatches(header.Load4(0), uint2(binding.epochLow, binding.epochHigh),
        binding.configurationIdentity);
}

// -----------------------------------------------------------------------------
// Keys (SHARC.2's key, unchanged)
// -----------------------------------------------------------------------------

// Which quarter of [-extent, extent] coordinate lies in.
uint RadianceCacheD6Quarter(float coordinate, float extent)
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

// The 16 octahedral bins of a normal (any length), compared in L1-scaled coordinates with a fixed operation order so
// the bin is exactly reproducible.
uint RadianceCacheD6NormalBin(float3 normal)
{
    float3 absolute = abs(normal);
    precise float extent = absolute.x + absolute.y + absolute.z;
    precise float u = normal.x;
    precise float v = normal.y;

    if(normal.z < 0.0f)
    {
        float signU = normal.x >= 0.0f ? 1.0f : -1.0f;
        float signV = normal.y >= 0.0f ? 1.0f : -1.0f;
        u = (extent - absolute.y) * signU;
        v = (extent - absolute.x) * signV;
    }

    return RadianceCacheD6Quarter(v, extent) * 4u + RadianceCacheD6Quarter(u, extent);
}

// The axis of largest magnitude; ties prefer x, then y.
uint RadianceCacheD6DominantAxis(float3 normal)
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

// The key of position on the grid of inverseCellSize, with the given version nibble (never 0).
bool RadianceCacheD6MakeVersionedKey(float3 position, float3 normalFacingRay, float inverseCellSize, uint version,
    out uint2 key)
{
    key = uint2(0u, 0u);

    if(!all(isfinite(position)) || !all(isfinite(normalFacingRay)) || !any(normalFacingRay != 0.0f))
    {
        return false;
    }

    float3 scaled = position * inverseCellSize;
    float3 cellFloor = floor(scaled);

    if(any(cellFloor < float(-RADIANCE_CACHE_D6_CELL_BIAS)) || any(cellFloor > float(RADIANCE_CACHE_D6_CELL_LIMIT)))
    {
        return false;
    }

    int3 cell = int3(cellFloor);
    uint3 biased = uint3(cell + RADIANCE_CACHE_D6_CELL_BIAS);

    // Quarter-cell position along the normal's dominant axis separates parallel surfaces closer than a cell.
    uint axis = RadianceCacheD6DominantAxis(normalFacingRay);
    float along = scaled[axis] - cellFloor[axis];
    uint subCell = uint(min(floor(along * 4.0f), 3.0f));
    uint normalBin = RadianceCacheD6NormalBin(normalFacingRay);

    key.x = biased.x | (biased.y << 18u);
    key.y = (biased.y >> 14u) | (biased.z << 4u) | (normalBin << 22u) | (subCell << 26u) | (version << 28u);
    return true;
}

// position in world space; normalFacingRay the surface normal turned towards the arriving ray. False when the position
// is not finite or outside the addressable grid.
bool RadianceCacheD6MakeKey(float3 position, float3 normalFacingRay, float inverseCellSize, out uint2 key)
{
    return RadianceCacheD6MakeVersionedKey(position, normalFacingRay, inverseCellSize, RADIANCE_CACHE_D6_KEY_VERSION,
        key);
}

// SHARC.5F: the adaptive level of a surface at distance metres from the anchor (cell edges 1, 2, 4, 8 x the base).
uint RadianceCacheD6AdaptiveLevel(float distance)
{
    if(distance < 5.0f)
    {
        return 0u;
    }

    if(distance < 15.0f)
    {
        return 1u;
    }

    return distance < 30.0f ? 2u : 3u;
}

// The key of an entry class (primaryClass: SHARC.6A's x0 entries) and its adaptive level (0 on the uniform grid).
bool RadianceCacheD6MakeClassKey(RadianceCacheD6Binding binding, float3 position, float3 normalFacingRay,
    bool primaryClass, out uint2 key, out uint level)
{
    uint classFlag = primaryClass ? RADIANCE_CACHE_D6_PRIMARY_KEY_FLAG : 0u;
    level = 0u;

    if(binding.spatialMode == 0u)
    {
        return RadianceCacheD6MakeVersionedKey(position, normalFacingRay, binding.inverseCellSize,
            RADIANCE_CACHE_D6_KEY_VERSION | classFlag, key);
    }

    static const float scales[RADIANCE_CACHE_D6_ADAPTIVE_LEVELS] = {1.0f, 0.5f, 0.25f, 0.125f};
    level = RadianceCacheD6AdaptiveLevel(length(position - binding.anchor));
    return RadianceCacheD6MakeVersionedKey(position, normalFacingRay, binding.inverseCellSize * scales[level],
        RADIANCE_CACHE_D6_ADAPTIVE_KEY_FLAG | classFlag | level, key);
}

// The key every x1 cache access uses: the uniform key, or with spatialMode 1 the key of the surface's adaptive level.
// Training and queries of a frame share the binding's anchor, so both pick the same level for a position; deferred
// records carry the finished key, so insertion and resolve keep it.
bool RadianceCacheD6MakeBindingKey(RadianceCacheD6Binding binding, float3 position, float3 normalFacingRay,
    out uint2 key)
{
    uint level;
    return RadianceCacheD6MakeClassKey(binding, position, normalFacingRay, false, key, level);
}

// -----------------------------------------------------------------------------
// Hashes: bucket, tag (never 0) and checksum term (always odd), independently mixed from the key.
// -----------------------------------------------------------------------------

uint RadianceCacheD6Mix(uint value)
{
    value ^= value >> 16u;
    value *= 0x7FEB352Du;
    value ^= value >> 15u;
    value *= 0x846CA68Bu;
    value ^= value >> 16u;
    return value;
}

uint RadianceCacheD6Bucket(uint2 key, uint bucketCount)
{
    return RadianceCacheD6Mix(key.x ^ RadianceCacheD6Mix(key.y ^ 0x9E3779B9u)) & (bucketCount - 1u);
}

uint RadianceCacheD6Tag(uint2 key)
{
    return max(RadianceCacheD6Mix(key.y ^ RadianceCacheD6Mix(key.x + 0x85EBCA6Bu)), 1u);
}

uint RadianceCacheD6ChecksumTerm(uint2 key)
{
    return RadianceCacheD6Mix((key.x * 0x27D4EB2Du) ^ RadianceCacheD6Mix(key.y ^ 0x165667B1u)) | 1u;
}

// SHARC.4D-R sparse training: the pixel trained by training thread `thread`, one per tile. The pixel within its tile is
// a hash of the tile and the frame's record sequence, so coverage moves across every pixel of the tile over time.
// False for a tile's pixel outside the image (the thread traces nothing).
bool RadianceCacheD6TrainingPixel(RadianceCacheD6Binding binding, uint thread, uint2 resolution, out uint2 pixel)
{
    uint tile = max(binding.trainingTile, 1u);
    uint2 tileIndex = uint2(thread % binding.trainingTilesX, thread / binding.trainingTilesX);
    uint offset = 0u;

    if(tile > 1u)
    {
        offset = RadianceCacheD6Mix(tileIndex.x ^ RadianceCacheD6Mix(tileIndex.y ^
            RadianceCacheD6Mix(binding.recordSequence ^ 0x5BD1E995u))) % (tile * tile);
    }

    pixel = tileIndex * tile + uint2(offset % tile, offset / tile);
    return all(pixel < resolution);
}

// -----------------------------------------------------------------------------
// Fixed point
// -----------------------------------------------------------------------------

// Two's complement 64-bit integer (x = low word, y = high word) of round(value * 2^20). |value| <= 2^23.
uint2 RadianceCacheD6ToFixedPoint(float value)
{
    float magnitude = round(abs(value) * RADIANCE_CACHE_D6_FIXED_POINT_SCALE);

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

float RadianceCacheD6FixedPointToFloat(uint2 fixedPoint)
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

// Exact 64-bit addition from 32-bit atomics: each adder carries the wrap its own low-word addition caused.
void RadianceCacheD6AtomicAdd64(RWByteAddressBuffer buffer, uint byteOffset, uint2 addend)
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
// D6 directional basis
// -----------------------------------------------------------------------------

// (harmonic m, radial row, sine) of each D6 moment: m0k0 m0k1 m1c m1s m2c m2s.
void RadianceCacheD6Descriptor(uint moment, out uint harmonic, out uint row, out bool sine)
{
    static const uint harmonics[6] = {0u, 0u, 1u, 1u, 2u, 2u};
    static const uint rows[6] = {0u, 1u, 3u, 3u, 5u, 5u};
    static const uint sines[6] = {0u, 0u, 0u, 1u, 0u, 1u};
    harmonic = harmonics[moment];
    row = rows[moment];
    sine = sines[moment] != 0u;
}

float RadianceCacheD6Basis(ByteAddressBuffer table, uint row, float mu)
{
    float position = saturate(mu) * float(RADIANCE_CACHE_D6_BASIS_SIZE - 1u);
    uint lower = min(uint(position), RADIANCE_CACHE_D6_BASIS_SIZE - 2u);
    float blend = position - float(lower);
    uint base = row * RADIANCE_CACHE_D6_BASIS_SIZE + lower;
    return lerp(asfloat(table.Load(base * 4u)), asfloat(table.Load(base * 4u + 4u)), blend);
}

float3 RadianceCacheD6BinCentre(uint normalBin)
{
    float u = (float(normalBin & 3u) + 0.5f) * 0.5f - 1.0f;
    float v = (float(normalBin >> 2u) + 0.5f) * 0.5f - 1.0f;
    float3 normal = float3(u, v, 1.0f - abs(u) - abs(v));

    if(normal.z < 0.0f)
    {
        normal.xy = (1.0f - abs(normal.yx)) * float2(u >= 0.0f ? 1.0f : -1.0f, v >= 0.0f ? 1.0f : -1.0f);
    }

    return normalize(normal);
}

// Canonical frame of an entry: the azimuth reference is the world axis least aligned with the key's normal-bin centre,
// projected onto the tangent plane, so every sample of one entry shares it.
void RadianceCacheD6Frame(float3 normal, out float3 tangent, out float3 bitangent)
{
    float3 centre = RadianceCacheD6BinCentre(RadianceCacheD6NormalBin(normal));
    float3 absolute = abs(centre);
    float3 axis = absolute.x <= absolute.y && absolute.x <= absolute.z ? float3(1.0f, 0.0f, 0.0f) :
        (absolute.y <= absolute.z ? float3(0.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f));
    tangent = normalize(axis - dot(axis, normal) * normal);
    bitangent = cross(normal, tangent);
}

// Per-moment factor g_mk(mu) trig(m phi) of a world direction in the frame of normal.
void RadianceCacheD6Weights(ByteAddressBuffer table, float3 normal, float3 direction,
    out float weights[RADIANCE_CACHE_D6_MOMENTS])
{
    float3 tangent;
    float3 bitangent;
    RadianceCacheD6Frame(normal, tangent, bitangent);
    float mu = dot(direction, normal);
    float phi = atan2(dot(direction, bitangent), dot(direction, tangent));

    [unroll]
    for(uint moment = 0u; moment < RADIANCE_CACHE_D6_MOMENTS; ++moment)
    {
        uint harmonic;
        uint row;
        bool sine;
        RadianceCacheD6Descriptor(moment, harmonic, row, sine);
        float angle = float(harmonic) * phi;
        weights[moment] = mu > 0.0f ? RadianceCacheD6Basis(table, row, mu) * (sine ? sin(angle) : cos(angle)) : 0.0f;
    }
}

// SHARC.6A: the irradiance Int L (n.l) dl of a published entry's moments about its normal. The cosine lies in the span
// of the m = 0 radial functions (basis rows 0 and 1, residual below 1%), so E = sum_k c_0k Int_0^1 g_0k(mu) mu dmu,
// the integrals taken from the basis table (trapezoidal over its 129 nodes). Clamped to 0 per channel.
float3 RadianceCacheD6Irradiance(float3 m0k0, float3 m0k1)
{
    return max(m0k0 * -0.5773062f + m0k1 * 0.0061562f, 0.0f);
}

// SHARC.6A: the history prior's trust in an adaptive level's cells (0.4 m cells half, 0.8 m cells none: coarse cells
// average across occlusion, SHARC.5F.1 section 5). The uniform grid is level 0.
float RadianceCacheD6PriorLevelTrust(uint level)
{
    static const float trust[RADIANCE_CACHE_D6_ADAPTIVE_LEVELS] = {1.0f, 1.0f, 0.5f, 0.0f};
    return trust[min(level, RADIANCE_CACHE_D6_ADAPTIVE_LEVELS - 1u)];
}

// SHARC.4E: the incident radiance a published entry's moments reconstruct along a world direction, in the frame of
// normal: sum over moments of c_mk g_mk(mu) trig(m phi) / (2 pi) for m = 0 and / pi otherwise (SHARC.3a-V's validated
// RadianceMomentReconstruct, src/Engine/tests/shaders/RadianceCacheMoments.hlsli). It can be negative (SHARC.3a-V
// section 7).
float3 RadianceCacheD6Reconstruct(ByteAddressBuffer table, float3 normal, float3 direction,
    float3 moments[RADIANCE_CACHE_D6_MOMENTS])
{
    static const float inversePi = 0.318309886f;
    float weights[RADIANCE_CACHE_D6_MOMENTS];
    RadianceCacheD6Weights(table, normal, direction, weights);
    float3 radiance = float3(0.0f, 0.0f, 0.0f);

    [unroll]
    for(uint moment = 0u; moment < RADIANCE_CACHE_D6_MOMENTS; ++moment)
    {
        uint harmonic;
        uint row;
        bool sine;
        RadianceCacheD6Descriptor(moment, harmonic, row, sine);
        radiance += moments[moment] * weights[moment] * (harmonic == 0u ? 0.5f * inversePi : inversePi);
    }

    return radiance;
}

// A harvested sample's RGB moment values g_mk(mu_w) trig(m phi_w) S / p in the frame of normal (SHARC.4D section 1);
// zero when its continuation was not sampled. The inline harvest and the deferred insertion both evaluate it.
void RadianceCacheD6SampleValues(RadianceCacheD6Binding binding, bool sampled, float3 normal, float3 direction,
    float pdf, float3 suffixRadiance, out float3 values[RADIANCE_CACHE_D6_MOMENTS])
{
    float weights[RADIANCE_CACHE_D6_MOMENTS];

    [unroll]
    for(uint moment = 0u; moment < RADIANCE_CACHE_D6_MOMENTS; ++moment)
    {
        weights[moment] = 0.0f;
    }

    if(sampled)
    {
        ByteAddressBuffer basis = ResourceDescriptorHeap[binding.basisIndex];
        RadianceCacheD6Weights(basis, normal, direction, weights);
    }

    [unroll]
    for(uint moment = 0u; moment < RADIANCE_CACHE_D6_MOMENTS; ++moment)
    {
        values[moment] = sampled ? weights[moment] * suffixRadiance / pdf : float3(0.0f, 0.0f, 0.0f);
    }
}

// -----------------------------------------------------------------------------
// Insertion (atomic path)
// -----------------------------------------------------------------------------

// RADIANCE_CACHE_D6_WAVE_STATISTICS 0 compiles the insertion statistics out (SHARC.4D-P experiment).
#ifndef RADIANCE_CACHE_D6_WAVE_STATISTICS
#define RADIANCE_CACHE_D6_WAVE_STATISTICS 1
#endif

// Adds count to a statistic once per wave.
void RadianceCacheD6CountWave(RadianceCacheD6Binding binding, uint statistic, bool condition)
{
#if RADIANCE_CACHE_D6_WAVE_STATISTICS == 0
    return;
#endif

    uint count = WaveActiveCountBits(condition);

    if(WaveIsFirstLane() && count > 0u)
    {
        RWByteAddressBuffer statistics = ResourceDescriptorHeap[binding.statisticsIndex];
        statistics.InterlockedAdd(statistic * 4u, count);
    }
}

// SHARC.4D-R: the slot holding key (tag and full key), or RADIANCE_CACHE_D6_NO_SLOT; read-only.
uint RadianceCacheD6FindSlot(RadianceCacheD6Binding binding, uint2 key)
{
    RWByteAddressBuffer tags = ResourceDescriptorHeap[binding.tagsIndex];
    RWByteAddressBuffer keys = ResourceDescriptorHeap[binding.keysIndex];
    uint firstSlot = RadianceCacheD6Bucket(key, binding.bucketCount) * RADIANCE_CACHE_D6_BUCKET_SLOTS;
    uint tag = RadianceCacheD6Tag(key);

    for(uint index = 0u; index < RADIANCE_CACHE_D6_BUCKET_SLOTS; ++index)
    {
        uint slot = firstSlot + index;

        if(tags.Load(slot * 4u) == tag && all(keys.Load2(slot * 8u) == key))
        {
            return slot;
        }
    }

    return RADIANCE_CACHE_D6_NO_SLOT;
}

// The slot that owns key's tag in its bucket, claimed if absent; RADIANCE_CACHE_D6_NO_SLOT when the bucket is full.
// Lock-free and bounded. Slots only go from 0 to a tag during an insertion dispatch, so every thread with a tag stops
// at the same slot; the plain-load pre-scan finds published tags before any hole left by eviction can be claimed.
uint RadianceCacheD6FindOrClaimSlot(RadianceCacheD6Binding binding, uint2 key, out bool claimed)
{
    claimed = false;
    RWByteAddressBuffer tags = ResourceDescriptorHeap[binding.tagsIndex];
    uint bucket = RadianceCacheD6Bucket(key, binding.bucketCount);
    uint tag = RadianceCacheD6Tag(key);
    uint firstSlot = bucket * RADIANCE_CACHE_D6_BUCKET_SLOTS;

    for(uint index = 0u; index < RADIANCE_CACHE_D6_BUCKET_SLOTS; ++index)
    {
        if(tags.Load((firstSlot + index) * 4u) == tag)
        {
            return firstSlot + index;
        }
    }

    for(uint index = 0u; index < RADIANCE_CACHE_D6_BUCKET_SLOTS; ++index)
    {
        uint slot = firstSlot + index;
        uint previous;
        tags.InterlockedCompareExchange(slot * 4u, 0u, tag, previous);

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
            return slot;
        }
    }

    RWByteAddressBuffer bucketFlags = ResourceDescriptorHeap[binding.bucketFlagsIndex];
    bucketFlags.InterlockedOr(bucket * 4u, 1u);
    return RADIANCE_CACHE_D6_NO_SLOT;
}

// Adds one sample's RGB moments to key's frame accumulation. Every lane of the wave must call this (wave statistics).
// Non-finite samples are rejected; values beyond the sample limit are clamped (counted). zero: the sample is a
// zero-valued one (an invalid continuation or a path ended before its suffix contributed), counted for coverage only.
void RadianceCacheD6AddSample(RadianceCacheD6Binding binding, bool active, uint2 key,
    float3 values[RADIANCE_CACHE_D6_MOMENTS], bool zero, bool accumulate = true)
{
    bool valid = active;
    bool clamped = false;

    [unroll]
    for(uint moment = 0u; moment < RADIANCE_CACHE_D6_MOMENTS; ++moment)
    {
        valid = valid && all(isfinite(values[moment]));
        float3 limited = clamp(values[moment], -RADIANCE_CACHE_D6_SAMPLE_LIMIT, RADIANCE_CACHE_D6_SAMPLE_LIMIT);
        clamped = clamped || any(limited != values[moment]);
        values[moment] = limited;
    }

    clamped = clamped && valid;
    uint slot = RADIANCE_CACHE_D6_NO_SLOT;
    bool claimed = false;

    if(valid)
    {
        slot = RadianceCacheD6FindOrClaimSlot(binding, key, claimed);
    }

    bool stored = slot != RADIANCE_CACHE_D6_NO_SLOT;

    if(stored && accumulate)
    {
        RWByteAddressBuffer accumulation = ResourceDescriptorHeap[binding.accumulationIndex];
        uint base = slot * RADIANCE_CACHE_D6_ACCUMULATION_WORDS * 4u;

        [unroll]
        for(uint moment = 0u; moment < RADIANCE_CACHE_D6_MOMENTS; ++moment)
        {
            [unroll]
            for(uint channel = 0u; channel < 3u; ++channel)
            {
                RadianceCacheD6AtomicAdd64(accumulation, base + (moment * 3u + channel) * 8u,
                    RadianceCacheD6ToFixedPoint(values[moment][channel]));
            }
        }

        accumulation.InterlockedAdd(base + RADIANCE_CACHE_D6_COUNT_WORD * 4u, 1u);
        accumulation.InterlockedAdd(base + RADIANCE_CACHE_D6_COUNT_WORD * 4u + 4u, RadianceCacheD6ChecksumTerm(key));
    }

    RadianceCacheD6CountWave(binding, RADIANCE_CACHE_D6_STATISTIC_SAMPLE_ATTEMPTS, active);
    RadianceCacheD6CountWave(binding, RADIANCE_CACHE_D6_STATISTIC_REJECTED_SAMPLES, active && !valid);
    RadianceCacheD6CountWave(binding, RADIANCE_CACHE_D6_STATISTIC_CLAMPED_SAMPLES, clamped);
    RadianceCacheD6CountWave(binding, RADIANCE_CACHE_D6_STATISTIC_STORED_SAMPLES, stored);
    RadianceCacheD6CountWave(binding, RADIANCE_CACHE_D6_STATISTIC_CLAIMED_SLOTS, claimed);
    RadianceCacheD6CountWave(binding, RADIANCE_CACHE_D6_STATISTIC_BUCKET_FULL, valid && !stored);
    RadianceCacheD6CountWave(binding, RADIANCE_CACHE_D6_STATISTIC_ZERO_SAMPLES, stored && zero);
}

// -----------------------------------------------------------------------------
// Resolve (one thread per bucket)
// -----------------------------------------------------------------------------

struct RadianceCacheD6ResolveCounts
{
    uint occupied;
    uint publishedSlots;
    uint evicted;
    uint checksumRejected;
    uint overflowDiscarded;
    uint resolvedSamples;
    uint maximumSlotSamples;
};

// Folds a trusted frame accumulation into the published entry: a running mean whose history weight is capped at
// maximumSampleCount (past the cap, a moving average). precise keeps the validated operation order.
void RadianceCacheD6Publish(RWByteAddressBuffer accumulation, RWByteAddressBuffer published, uint slot, uint count,
    uint frameIndex, float maximumSampleCount, float varianceBlend)
{
    uint accumulationBase = slot * RADIANCE_CACHE_D6_ACCUMULATION_WORDS * 4u;
    uint publishedBase = slot * RADIANCE_CACHE_D6_PUBLISHED_WORDS * 4u;
    uint metaBase = publishedBase + RADIANCE_CACHE_D6_META_WORD * 4u;
    float previousCount = asfloat(published.Load(metaBase));
    float relativeVariance = asfloat(published.Load(metaBase + 4u));
    uint firstFrame = published.Load(metaBase + 12u);
    uint flags = published.Load(metaBase + 16u);
    bool firstPublication = (flags & RADIANCE_CACHE_D6_FLAG_PUBLISHED) == 0u;

    precise float frameScale = float(count) * RADIANCE_CACHE_D6_FIXED_POINT_SCALE;
    precise float frameCount = float(count);
    precise float blend = firstPublication ? 1.0f : frameCount / (previousCount + frameCount);
    precise float3 leadingMean = float3(0.0f, 0.0f, 0.0f);
    precise float3 leadingPrevious = float3(0.0f, 0.0f, 0.0f);
    precise float3 leadingValue = float3(0.0f, 0.0f, 0.0f);

    for(uint value = 0u; value < RADIANCE_CACHE_D6_VALUES; ++value)
    {
        uint2 sum = accumulation.Load2(accumulationBase + value * 8u);
        precise float frameMean = RadianceCacheD6FixedPointToFloat(sum) / frameScale;
        float previous = asfloat(published.Load(publishedBase + value * 4u));
        precise float blended = previous + (frameMean - previous) * blend;
        published.Store(publishedBase + value * 4u, asuint(blended));

        if(value < 3u)
        {
            leadingMean[value] = frameMean;
            leadingPrevious[value] = previous;
            leadingValue[value] = blended;
        }
    }

    if(!firstPublication)
    {
        const float3 luminanceWeights = float3(0.2126f, 0.7152f, 0.0722f);
        precise float deviation = (dot(leadingMean, luminanceWeights) - dot(leadingPrevious, luminanceWeights)) /
            max(abs(dot(leadingValue, luminanceWeights)), 1.0e-6f);
        precise float frameVariance = frameCount * deviation * deviation;
        bool varianceValid = (flags & RADIANCE_CACHE_D6_FLAG_VARIANCE_VALID) != 0u;
        relativeVariance = varianceValid ? relativeVariance + (frameVariance - relativeVariance) * varianceBlend :
            frameVariance;
        flags |= RADIANCE_CACHE_D6_FLAG_VARIANCE_VALID;
    }

    precise float sampleCount = min(previousCount + frameCount, maximumSampleCount);
    published.Store(metaBase, asuint(sampleCount));
    published.Store(metaBase + 4u, asuint(relativeVariance));
    published.Store(metaBase + 8u, frameIndex);
    published.Store(metaBase + 12u, firstPublication ? frameIndex : firstFrame);
    published.Store(metaBase + 16u, flags | RADIANCE_CACHE_D6_FLAG_PUBLISHED);
}

void RadianceCacheD6ClearSlot(RWByteAddressBuffer tags, RWByteAddressBuffer keys, RWByteAddressBuffer published,
    uint slot)
{
    tags.Store(slot * 4u, 0u);
    keys.Store2(slot * 8u, uint2(0u, 0u));

    for(uint word = 0u; word < RADIANCE_CACHE_D6_PUBLISHED_WORDS; word += 4u)
    {
        published.Store4((slot * RADIANCE_CACHE_D6_PUBLISHED_WORDS + word) * 4u, uint4(0u, 0u, 0u, 0u));
    }
}

// Per slot: a checksum mismatch (two keys sharing a tag this frame) discards the frame's samples and keeps a published
// entry; a new key of an overflowed bucket is dropped (thread-order independence); an entry without samples for more
// than staleFrames frames is evicted. The frame accumulation and the bucket flag are zeroed for the next frame.
RadianceCacheD6ResolveCounts RadianceCacheD6ResolveBucket(RadianceCacheD6Binding binding, uint bucket)
{
    RadianceCacheD6ResolveCounts counts = (RadianceCacheD6ResolveCounts)0;
    RWByteAddressBuffer tags = ResourceDescriptorHeap[binding.tagsIndex];
    RWByteAddressBuffer keys = ResourceDescriptorHeap[binding.keysIndex];
    RWByteAddressBuffer accumulation = ResourceDescriptorHeap[binding.accumulationIndex];
    RWByteAddressBuffer published = ResourceDescriptorHeap[binding.publishedIndex];
    RWByteAddressBuffer bucketFlags = ResourceDescriptorHeap[binding.bucketFlagsIndex];
    bool overflowed = (bucketFlags.Load(bucket * 4u) & 1u) != 0u;

    for(uint index = 0u; index < RADIANCE_CACHE_D6_BUCKET_SLOTS; ++index)
    {
        uint slot = bucket * RADIANCE_CACHE_D6_BUCKET_SLOTS + index;

        if(tags.Load(slot * 4u) == 0u)
        {
            continue;
        }

        uint2 key = keys.Load2(slot * 8u);
        uint accumulationBase = slot * RADIANCE_CACHE_D6_ACCUMULATION_WORDS * 4u;
        uint2 countAndChecksum = accumulation.Load2(accumulationBase + RADIANCE_CACHE_D6_COUNT_WORD * 4u);
        uint metaBase = (slot * RADIANCE_CACHE_D6_PUBLISHED_WORDS + RADIANCE_CACHE_D6_META_WORD) * 4u;
        uint flags = published.Load(metaBase + 16u);
        uint lastUpdate = published.Load(metaBase + 8u);
        bool wasPublished = (flags & RADIANCE_CACHE_D6_FLAG_PUBLISHED) != 0u;
        bool keep = true;
        uint count = countAndChecksum.x;

        if(count > 0u)
        {
            counts.resolvedSamples += count;
            counts.maximumSlotSamples = max(counts.maximumSlotSamples, count);

            if(countAndChecksum.y != count * RadianceCacheD6ChecksumTerm(key))
            {
                ++counts.checksumRejected;
                keep = wasPublished;
            }
            else if(overflowed && !wasPublished)
            {
                ++counts.overflowDiscarded;
                keep = false;
            }
            else
            {
                RadianceCacheD6Publish(accumulation, published, slot, count, binding.frameIndex,
                    binding.maximumSampleCount, binding.varianceBlend);
                ++counts.publishedSlots;
            }
        }
        else if(!wasPublished || binding.frameIndex - lastUpdate > binding.staleFrames)
        {
            ++counts.evicted;
            keep = false;
        }

        if(keep)
        {
            ++counts.occupied;
        }
        else
        {
            RadianceCacheD6ClearSlot(tags, keys, published, slot);
        }

        for(uint word = 0u; word < RADIANCE_CACHE_D6_ACCUMULATION_WORDS; word += 4u)
        {
            accumulation.Store4(accumulationBase + word * 4u, uint4(0u, 0u, 0u, 0u));
        }
    }

    bucketFlags.Store(bucket * 4u, 0u);
    return counts;
}

void RadianceCacheD6CountResolve(RadianceCacheD6Binding binding, RadianceCacheD6ResolveCounts counts, bool overflowed)
{
    uint waveOccupied = WaveActiveSum(counts.occupied);
    uint wavePublished = WaveActiveSum(counts.publishedSlots);
    uint waveEvicted = WaveActiveSum(counts.evicted);
    uint waveChecksumRejected = WaveActiveSum(counts.checksumRejected);
    uint waveOverflowDiscarded = WaveActiveSum(counts.overflowDiscarded);
    uint waveResolvedSamples = WaveActiveSum(counts.resolvedSamples);
    uint waveMaximumSlotSamples = WaveActiveMax(counts.maximumSlotSamples);
    uint waveOverflowedBuckets = WaveActiveCountBits(overflowed);

    if(WaveIsFirstLane())
    {
        RWByteAddressBuffer statistics = ResourceDescriptorHeap[binding.statisticsIndex];
        statistics.InterlockedAdd(RADIANCE_CACHE_D6_STATISTIC_OCCUPIED * 4u, waveOccupied);
        statistics.InterlockedAdd(RADIANCE_CACHE_D6_STATISTIC_PUBLISHED * 4u, wavePublished);
        statistics.InterlockedAdd(RADIANCE_CACHE_D6_STATISTIC_EVICTED * 4u, waveEvicted);
        statistics.InterlockedAdd(RADIANCE_CACHE_D6_STATISTIC_CHECKSUM_REJECTED * 4u, waveChecksumRejected);
        statistics.InterlockedAdd(RADIANCE_CACHE_D6_STATISTIC_OVERFLOW_DISCARDED * 4u, waveOverflowDiscarded);
        statistics.InterlockedAdd(RADIANCE_CACHE_D6_STATISTIC_RESOLVED_SAMPLES * 4u, waveResolvedSamples);
        statistics.InterlockedMax(RADIANCE_CACHE_D6_STATISTIC_MAXIMUM_SLOT_SAMPLES * 4u, waveMaximumSlotSamples);
        statistics.InterlockedAdd(RADIANCE_CACHE_D6_STATISTIC_OVERFLOWED_BUCKETS * 4u, waveOverflowedBuckets);
    }
}

#endif
