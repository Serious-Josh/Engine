// M4.3 ReGIR presampling (experimental, off by default): fills RTXDI's ReGIR slots for the camera-centred window of M4.1
// cells from the shared light-sampling structure. One thread per slot. The GPU twin of ReGIRPresampleSlot
// (src/Renderer/LightSampling/ReGIRPresampling.cpp); the estimator and its proof: Documentation/Rendering/ReGIR_M4_3_Math.md.
//
// Each slot of window cell c draws `buildSamples` candidates from c's source and keeps one by RIS on the cell target t_c:
// - window cell inside the M4.1 grid: with probability s = wideThreshold / 2^32 (1 with an empty list, 0 with an empty
//   wide pool) the wide pool, else the cell's padded list (uniform). The wide pool is drawn in proportion to t_c over its
//   members (with a defensive uniform share over the members that reach the cell) when it has at most wideTargetLimit
//   members, else by its power alias table;
// - window cell outside the M4.1 grid: the global alias table.
// The slot is uint2(RTXDI local index of the light, asuint(W)), W = sum(w) / t_c(Y) with w = t_c / (M p), or (0, 0).
// The output buffer begins with a 64-byte header (thread 0 writes it; REGIR_SLOT_HEADER_BYTES) that the ReSTIR DI direct
// pass reads to locate cells exactly as this pass laid them out: {magic, window centre, window cells, slots per cell, cell
// size, sampling jitter, frame seed}; slot s is at byte 64 + 8 s.
//
// REGIR_PRESAMPLE_DEBUG (verification only): also writes, per slot and candidate, {table light, source, p bits, t bits}
// to debugIndex.
//
// mainExact (point-light noise fix #1, ReSTIR PT only; one 64-thread group per window cell): a cell inside the M4.1 grid
// whose sources are its padded list and a wide pool of at most wideTargetLimit lights (disjoint, every light once) draws
// each slot exactly in proportion to the cell target over those lights, q(l) = w_l / 2^32 with integer widths w_l >= 1
// for every light with t_c(l) > 0 summing to 2^32 - 1 (a slot whose random falls in the last value stays empty), and
// stores W = 2^32 / w_l = 1 / q(l), so E[W 1{Y = l}] = 1 for every light the cell can need, exactly as for the RIS draw.
// It changes the slot distribution, not only its weight: the RIS draw's slot marginal has no closed form, and its W is a
// 4-candidate estimate of 1 / p, whose noise is shared by every pixel of the cell. Other cells (outside the grid, where
// binned lights still reach the jitter margin; a larger wide pool; more members than REGIR_EXACT_CAPACITY) keep the RIS
// draw, slot for slot as main (each slot's W is unbiased for its own draw, so cells may mix the two).

#include "LightSampling.hlsli"
#include "SceneLights.hlsli"

#define REGIR_PRESAMPLE_ROOT_SIGNATURE "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED), CBV(b0)"

cbuffer ReGIRPresampleConstants : register(b0)
{
    // The published light-sampling buffer (raw SRV, LightSamplingLoadConstants header; its `padding` word is the byte offset
    // of the table -> RTXDI local index translation, one uint per table light) and the scene light table.
    uint lightSamplingIndex;
    uint sceneLightBufferIndex;
    uint sceneLightCount;
    uint reservedIndex;

    // The slots (raw UAV of uint2 at outputOffset slots), slots per cell and RIS candidates per slot.
    uint outputIndex;
    uint outputOffset;
    uint lightsPerCell;
    uint buildSamples;

    // The window: its first M4.1 cell and its size in cells.
    int3 originCell;
    uint frameSeed;
    uint3 windowCells;
    uint wideThreshold;

    uint wideTargetLimit;
    float samplingJitter;
    float precisionGuard;
    uint debugIndex;

    // The window as RTXDI's Grid mode locates it (ReGIR_CommonParameters center): its grid origin is
    // windowCenter - windowCells * cellSize / 2.
    float3 windowCenter;
    // M5.6 O0.2 flags (0 for ReSTIR DI, unchanged): REGIR_FLAG_CONE, REGIR_FLAG_TABLE_INDEX. Echoed in header word 11.
    uint headerPadding;
};

// M5.6 O0.2 (ReSTIR PT builds only): the cell target includes an upper bound of a spot light's cone factor over the lookup
// region, and slots hold scene-table indices instead of RTXDI local indices.
static const uint REGIR_FLAG_CONE = 1u;
static const uint REGIR_FLAG_TABLE_INDEX = 2u;
// Point-light noise fix #2 (ReSTIR PT builds only): a spot light's cone factor in the cell target is its mean over a
// 3 x 3 x 3 grid of the lookup region, at least REGIR_SPOT_FLOOR times the conservative bound (so positive exactly where
// the bound is: coverage unchanged). The bound alone credits a spot whose cone only grazes the region with its full axis
// intensity (18x a Point of equal flux here), so spots took most slots where they light little. Any positive target is
// unbiased (it only shapes the proposal).
static const uint REGIR_FLAG_SPOT_MEAN = 8u;
// ReGIR architecture investigation (experimental): write the header only (the per-receiver list prototype reads no slots).
static const uint REGIR_FLAG_HEADER_ONLY = 16u;
// ReGIR architecture investigation (experimental): stratified exact slots. Slot k of a cell takes the random value
// (k 2^32 + r) / S for one uniform r per cell and frame, instead of an independent value: a uniformly chosen slot still
// has the marginal q (the strata partition [0, 2^32)), so W = 1 / q is unchanged, but each light holds floor or ceil of
// S q slots rather than a multinomial count, which removes most of the cell-wide frame-to-frame fluctuation. Exact when S
// divides 2^32 (powers of two); otherwise the strata edges differ by one value (relative 1e-7).
static const uint REGIR_FLAG_STRATIFIED = 32u;
static const float REGIR_SPOT_FLOOR = 0.05f;

static const uint REGIR_SLOT_HEADER_BYTES = 64u;
static const uint REGIR_SLOT_MAGIC = 0x52654749u;

static const uint REGIR_NO_LIGHT = 0xFFFFFFFFu;
static const uint REGIR_WIDE_DEFENSIVE_THRESHOLD = 1u << 26;

uint ReGIRNextRandom(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

uint ReGIRSlotSeed(uint slot, uint seed)
{
    uint state = seed ^ 0x52654749u;
    ReGIRNextRandom(state);
    state ^= slot * 0x9E3779B9u;
    ReGIRNextRandom(state);
    return state;
}

// The cell target over the lookup region [low, high]: luminance x range window(d) / max(d^2, (cellSize / 2)^2), d the
// distance from the light to the region; 0 for directional lights and lights that reach no point of the region.
float ReGIRCellTarget(GPULight light, float3 low, float3 high, float cellSize)
{
    const float luminance = 0.2126f * light.radiance.x + 0.7152f * light.radiance.y + 0.0722f * light.radiance.z;

    if(light.type == SCENE_LIGHT_DIRECTIONAL || !(luminance > 0.0f) || !isfinite(luminance))
    {
        return 0.0f;
    }

    const float3 offset = light.position - clamp(light.position, low, high);
    const float distanceSquared = dot(offset, offset);
    const float distance = sqrt(distanceSquared);
    const float range = abs(light.range);

    if(!(distance < range))
    {
        return 0.0f;
    }

    float window = 1.0f;

    if(light.range > 0.0f)
    {
        const float ratio = distance / range;
        const float fade = saturate(1.0f - ratio * ratio * ratio * ratio);
        window = fade * fade;
    }

    const float halfCell = 0.5f * cellSize;
    float target = luminance * window / max(distanceSquared, halfCell * halfCell);

    // M5.6 O0.2: a spot light's cone factor, t^2 with t = saturate((cos - cosOuter) / (cosInner - cosOuter)), is at most its
    // value at the smallest angle between the spot axis and any point of the region's bounding sphere: max(theta - alpha,
    // 0), with theta the axis angle to the sphere centre and alpha the sphere's angular radius (widened by 1e-3 rad
    // against rounding). The bound is 0 exactly when no point of the region lies inside the outer cone, so every light
    // that can reach a point of the region keeps a positive target (coverage unchanged).
    if((headerPadding & (REGIR_FLAG_CONE | REGIR_FLAG_SPOT_MEAN)) != 0u && light.type == SCENE_LIGHT_SPOT)
    {
        const float3 centre = 0.5f * (low + high);
        const float radius = 0.5f * length(high - low);
        const float3 toCentre = centre - light.position;
        const float centreDistance = length(toCentre);
        const float3 axis = normalize(light.direction);
        const float coneWidth = max(light.cosInnerCone - light.cosOuterCone, 1.0e-4f);
        float coneFactor = 1.0f;

        if(centreDistance > radius)
        {
            const float axisAngle = acos(clamp(dot(toCentre / centreDistance, axis), -1.0f, 1.0f));
            const float angularRadius = asin(saturate(radius / centreDistance)) + 1.0e-3f;
            const float smallestCosine = cos(max(axisAngle - angularRadius, 0.0f));
            const float coneBound = saturate((smallestCosine - light.cosOuterCone) / coneWidth);
            coneFactor = coneBound * coneBound;
        }

        // Point-light noise fix #2: the representative factor (mean over the region grid, floored at a share of the bound).
        if((headerPadding & REGIR_FLAG_SPOT_MEAN) != 0u && coneFactor > 0.0f)
        {
            float sum = 0.0f;

            for(uint gridPoint = 0u; gridPoint < 27u; ++gridPoint)
            {
                const float3 fraction = float3(gridPoint % 3u, (gridPoint / 3u) % 3u, gridPoint / 9u) * 0.5f;
                const float3 offset = low + (high - low) * fraction - light.position;
                const float pointDistance = length(offset);
                const float cosine = pointDistance > 0.0f ? dot(offset / pointDistance, axis) : 1.0f;
                const float factor = saturate((cosine - light.cosOuterCone) / coneWidth);
                sum += factor * factor;
            }

            coneFactor = max(sum * (1.0f / 27.0f), REGIR_SPOT_FLOOR * coneFactor);
        }

        target *= coneFactor;
    }

    return target > 0.0f ? target : 0.0f;
}

float ReGIRTargetOf(uint light, float3 low, float3 high, float cellSize)
{
    return light < sceneLightCount ?
        ReGIRCellTarget(SceneLightLoad(sceneLightBufferIndex, light), low, high, cellSize) : 0.0f;
}

// A wide draw in proportion to the cell target over a small wide pool, with the defensive uniform share over the members
// that reach the cell. False when no member reaches it.
bool ReGIRDrawWideByTarget(LightSamplingConstants sampling, float3 low, float3 high, uint random1, uint random2,
    out uint light, out float probability)
{
    ByteAddressBuffer structures = LightSamplingBuffer(sampling);
    const uint entries = LIGHT_SAMPLING_HEADER_BYTES + sampling.wideOffset;
    light = REGIR_NO_LIGHT;
    probability = 0.0f;
    float total = 0.0f;
    uint reaching = 0u;

    for(uint entry = 0u; entry < sampling.wideCount; ++entry)
    {
        const float target = ReGIRTargetOf(structures.Load(entries + entry * 16u + 12u), low, high, sampling.cellSize);
        total += target;
        reaching += target > 0.0f ? 1u : 0u;
    }

    if(reaching == 0u || !(total > 0.0f))
    {
        return false;
    }

    const bool uniformDraw = random2 < REGIR_WIDE_DEFENSIVE_THRESHOLD;
    const uint uniformRank = LightSamplingMulHi(random1, reaching);
    const float threshold = float(random1 >> 8) * (1.0f / 16777216.0f) * total;
    float cumulative = 0.0f;
    uint rank = 0u;

    for(uint member = 0u; member < sampling.wideCount; ++member)
    {
        const uint candidate = structures.Load(entries + member * 16u + 12u);
        const float target = ReGIRTargetOf(candidate, low, high, sampling.cellSize);

        if(!(target > 0.0f))
        {
            continue;
        }

        cumulative += target;
        const bool last = rank + 1u == reaching;

        if(uniformDraw ? rank == uniformRank : (threshold < cumulative || last))
        {
            const float defensive = float(REGIR_WIDE_DEFENSIVE_THRESHOLD) * (1.0f / 4294967296.0f);
            light = candidate;
            probability = defensive * LightSamplingUniformProbability(rank, reaching) + (1.0f - defensive) * (target / total);
            return true;
        }

        ++rank;
    }

    return false;
}

// A window cell: its M4.1 cell, list and lookup region (the cell box widened by samplingJitter / 2 cells and the precision
// guard per side).
struct ReGIRCellInfo
{
    bool inGrid;
    uint listOffset;
    uint paddedCount;
    float3 low;
    float3 high;
};

ReGIRCellInfo ReGIRCellOf(LightSamplingConstants sampling, uint windowCell)
{
    ByteAddressBuffer structures = LightSamplingBuffer(sampling);
    const int3 local = int3(windowCell % windowCells.x, (windowCell / windowCells.x) % windowCells.y,
        windowCell / (windowCells.x * windowCells.y));
    const int3 gridCell = originCell + local;
    ReGIRCellInfo info;
    info.inGrid = all(gridCell >= 0) && all(gridCell < int3(sampling.gridDimensions));
    const float margin = 0.5f * samplingJitter * sampling.cellSize + precisionGuard;
    const float3 base = sampling.gridOrigin + float3(gridCell) * sampling.cellSize;
    info.low = base - margin;
    info.high = base + sampling.cellSize + margin;
    info.listOffset = 0u;
    info.paddedCount = 0u;

    if(info.inGrid)
    {
        const uint cellIndex = uint(gridCell.x) + sampling.gridDimensions.x * (uint(gridCell.y) +
            sampling.gridDimensions.y * uint(gridCell.z));
        const uint3 cell = structures.Load3(LIGHT_SAMPLING_HEADER_BYTES + sampling.cellsOffset + cellIndex * 12u);
        info.listOffset = cell.x;
        info.paddedCount = cell.z;
    }

    return info;
}

// The slot word for a selected scene-table light and its W, (0, 0) for none.
uint2 ReGIRSlotWord(LightSamplingConstants sampling, uint selectedLight, float weight)
{
    if(selectedLight == REGIR_NO_LIGHT || !(weight > 0.0f))
    {
        return uint2(0u, 0u);
    }

    // M5.6 O0.2: ReSTIR PT builds may store the scene-table index itself (no translation for its consumer).
    const uint rtxdiIndex = (headerPadding & REGIR_FLAG_TABLE_INDEX) != 0u ? selectedLight :
        LightSamplingBuffer(sampling).Load(LIGHT_SAMPLING_HEADER_BYTES + sampling.padding + selectedLight * 4u);

    // A light without an RTXDI local index (directional; excluded above by its zero target) never enters a slot.
    return rtxdiIndex != REGIR_NO_LIGHT ? uint2(rtxdiIndex, asuint(weight)) : uint2(0u, 0u);
}

// One slot by M-candidate RIS on the cell target (the M4.3 draw).
uint2 ReGIRPresampleRISSlot(LightSamplingConstants sampling, ReGIRCellInfo info, uint slot)
{
    ByteAddressBuffer structures = LightSamplingBuffer(sampling);
    const bool inGrid = info.inGrid;
    const float3 low = info.low;
    const float3 high = info.high;
    const uint listOffset = info.listOffset;
    const uint paddedCount = info.paddedCount;
    const bool hasCell = inGrid && paddedCount != 0u;
    const bool hasWide = inGrid && sampling.wideCount != 0u;
    const float wideShare = !hasWide ? 0.0f : (!hasCell ? 1.0f : float(wideThreshold) * (1.0f / 4294967296.0f));
    const float candidates = float(buildSamples);
    uint random = ReGIRSlotSeed(slot, frameSeed);
    uint selectedLight = REGIR_NO_LIGHT;
    float selectedTarget = 0.0f;
    float weightSum = 0.0f;

    for(uint sample = 0u; sample < buildSamples; ++sample)
    {
        const uint random0 = ReGIRNextRandom(random);
        const uint random1 = ReGIRNextRandom(random);
        const uint random2 = ReGIRNextRandom(random);
        const uint random3 = ReGIRNextRandom(random);
        uint light = REGIR_NO_LIGHT;
        float probability = 0.0f;
        uint source = 0u;

        if(!inGrid)
        {
            if(sampling.globalCount != 0u)
            {
                const LightSamplingSample drawn = LightSamplingDrawGlobal(sampling, random1, random2);
                light = drawn.light;
                probability = drawn.probability;
                source = 3u;
            }
        }
        else if(hasWide && (!hasCell || random0 < wideThreshold))
        {
            if(sampling.wideCount <= wideTargetLimit)
            {
                ReGIRDrawWideByTarget(sampling, low, high, random1, random2, light, probability);
                probability *= wideShare;
                source = 2u;
            }
            else
            {
                const LightSamplingSample drawn = LightSamplingDrawWide(sampling, random1, random2);
                light = drawn.light;
                probability = drawn.probability * wideShare;
                source = 1u;
            }
        }
        else if(hasCell)
        {
            const uint index = LightSamplingMulHi(random1, paddedCount);
            light = structures.Load(LIGHT_SAMPLING_HEADER_BYTES + sampling.listsOffset + (listOffset + index) * 4u);
            probability = (1.0f - wideShare) * LightSamplingUniformProbability(index, paddedCount);
        }

        const bool usable = light != REGIR_NO_LIGHT && probability > 0.0f && light < sceneLightCount;
        const float target = usable ? ReGIRTargetOf(light, low, high, sampling.cellSize) : 0.0f;

#ifdef REGIR_PRESAMPLE_DEBUG
        RWByteAddressBuffer debug = ResourceDescriptorHeap[debugIndex];
        debug.Store4((slot * 8u + sample) * 16u, uint4(usable ? light : REGIR_NO_LIGHT, source, asuint(probability),
            asuint(target)));
#endif

        if(!usable)
        {
            continue;
        }

        const float weight = target / (candidates * probability);

        if(!(weight > 0.0f) || !isfinite(weight))
        {
            continue;
        }

        weightSum += weight;
        const float selection = float(random3 >> 8) * (1.0f / 16777216.0f);

        if(selection * weightSum < weight)
        {
            selectedLight = light;
            selectedTarget = target;
        }
    }

    return selectedTarget > 0.0f ? ReGIRSlotWord(sampling, selectedLight, weightSum / selectedTarget) : uint2(0u, 0u);
}

void ReGIRWriteHeader(LightSamplingConstants sampling)
{
    RWByteAddressBuffer slots = ResourceDescriptorHeap[outputIndex];
    slots.Store4(0u, uint4(REGIR_SLOT_MAGIC, asuint(windowCenter)));
    slots.Store4(16u, uint4(windowCells, lightsPerCell));
    slots.Store4(32u, uint4(asuint(sampling.cellSize), asuint(samplingJitter), frameSeed, headerPadding));
}

[RootSignature(REGIR_PRESAMPLE_ROOT_SIGNATURE)]
[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    const uint windowCellCount = windowCells.x * windowCells.y * windowCells.z;
    const uint slot = thread.x;

    if(slot >= windowCellCount * lightsPerCell)
    {
        return;
    }

    const LightSamplingConstants sampling = LightSamplingLoadConstants(lightSamplingIndex);
    const ReGIRCellInfo info = ReGIRCellOf(sampling, slot / lightsPerCell);
    RWByteAddressBuffer slots = ResourceDescriptorHeap[outputIndex];
    slots.Store2(REGIR_SLOT_HEADER_BYTES + (outputOffset + slot) * 8u, ReGIRPresampleRISSlot(sampling, info, slot));

    if(slot == 0u)
    {
        ReGIRWriteHeader(sampling);
    }
}

// ---- mainExact (point-light noise fix #1): the cell's slots drawn exactly from the cell target. ----
// Members: the padded list, then the wide pool; at most REGIR_EXACT_CAPACITY (the M4.1 lists hold at most 118 in
// ManyLight_1024). gsExactEnd holds the cumulative integer widths (exclusive ends) of the lights with a positive target.
static const uint REGIR_EXACT_CAPACITY = 1024u;
groupshared uint gsExactLight[REGIR_EXACT_CAPACITY];
groupshared float gsExactTarget[REGIR_EXACT_CAPACITY];
groupshared uint gsExactEnd[REGIR_EXACT_CAPACITY];
groupshared uint gsExactCount;

[RootSignature(REGIR_PRESAMPLE_ROOT_SIGNATURE)]
[numthreads(64, 1, 1)]
void mainExact(uint3 group : SV_GroupID, uint3 groupThread : SV_GroupThreadID)
{
    const uint windowCell = group.x + windowCells.x * (group.y + windowCells.y * group.z);
    const uint lane = groupThread.x;
    const LightSamplingConstants sampling = LightSamplingLoadConstants(lightSamplingIndex);
    ByteAddressBuffer structures = LightSamplingBuffer(sampling);
    const ReGIRCellInfo info = ReGIRCellOf(sampling, windowCell);
    const uint wideMembers = info.inGrid ? sampling.wideCount : 0u;
    const uint members = info.paddedCount + wideMembers;
    const bool exact = info.inGrid && wideMembers <= wideTargetLimit && members <= REGIR_EXACT_CAPACITY;
    RWByteAddressBuffer slots = ResourceDescriptorHeap[outputIndex];

    if(windowCell == 0u && lane == 0u)
    {
        ReGIRWriteHeader(sampling);
    }

    if((headerPadding & REGIR_FLAG_HEADER_ONLY) != 0u)
    {
        return;
    }

    if(!exact)
    {
        for(uint slot = lane; slot < lightsPerCell; slot += 64u)
        {
            const uint index = windowCell * lightsPerCell + slot;
            slots.Store2(REGIR_SLOT_HEADER_BYTES + (outputOffset + index) * 8u, ReGIRPresampleRISSlot(sampling, info, index));
        }

        return;
    }

    for(uint member = lane; member < members; member += 64u)
    {
        const uint light = member < info.paddedCount ?
            structures.Load(LIGHT_SAMPLING_HEADER_BYTES + sampling.listsOffset + (info.listOffset + member) * 4u) :
            structures.Load(LIGHT_SAMPLING_HEADER_BYTES + sampling.wideOffset + (member - info.paddedCount) * 16u + 12u);
        gsExactLight[member] = light;
        gsExactTarget[member] = ReGIRTargetOf(light, info.low, info.high, sampling.cellSize);
    }

    GroupMemoryBarrierWithGroupSync();

    // Integer widths: every positive-target light gets 1 + its share of the rest of 2^32 - 1, in member order; the last
    // takes the remainder, so the widths sum to exactly 2^32 - 1 (the rounding of the share only changes q, never its
    // exactness). Compacted in place to the positive-target lights.
    if(lane == 0u)
    {
        float total = 0.0f;
        uint positive = 0u;

        for(uint member = 0u; member < members; ++member)
        {
            const float target = gsExactTarget[member];
            total += target > 0.0f && isfinite(target) ? target : 0.0f;
            positive += target > 0.0f && isfinite(target) ? 1u : 0u;
        }

        uint end = 0u;
        uint count = 0u;

        if(positive != 0u && total > 0.0f && isfinite(total))
        {
            const uint budget = 0xFFFFFFFFu - positive;
            const float scale = float(budget) / total;
            uint spent = 0u;

            for(uint member = 0u; member < members; ++member)
            {
                const float target = gsExactTarget[member];

                if(!(target > 0.0f) || !isfinite(target))
                {
                    continue;
                }

                const uint light = gsExactLight[member];
                const uint share = count + 1u == positive ? budget - spent :
                    min(uint(min(target * scale, 4294967040.0f)), budget - spent);
                spent += share;
                end += 1u + share;
                gsExactLight[count] = light;
                gsExactEnd[count] = end;
                ++count;
            }
        }

        gsExactCount = count;
    }

    GroupMemoryBarrierWithGroupSync();
    const uint count = gsExactCount;
    uint cellState = ReGIRSlotSeed(windowCell, frameSeed ^ 0x53545241u);
    const uint cellRandom = ReGIRNextRandom(cellState);
    const bool stratified = (headerPadding & REGIR_FLAG_STRATIFIED) != 0u;

    for(uint slot = lane; slot < lightsPerCell; slot += 64u)
    {
        const uint index = windowCell * lightsPerCell + slot;
        uint random = ReGIRSlotSeed(index, frameSeed);
        const uint value = stratified ? uint(((uint64_t(slot) << 32) + uint64_t(cellRandom)) / uint64_t(lightsPerCell)) :
            ReGIRNextRandom(random);
        uint2 word = uint2(0u, 0u);

        // The first light whose exclusive end exceeds the random value (none for the final value 2^32 - 1).
        if(count != 0u && value < gsExactEnd[count - 1u])
        {
            uint first = 0u;
            uint last = count - 1u;

            while(first < last)
            {
                const uint middle = (first + last) >> 1u;

                if(value < gsExactEnd[middle])
                {
                    last = middle;
                }
                else
                {
                    first = middle + 1u;
                }
            }

            const uint width = gsExactEnd[first] - (first != 0u ? gsExactEnd[first - 1u] : 0u);
            word = ReGIRSlotWord(sampling, gsExactLight[first], 4294967296.0f / float(width));
        }

        slots.Store2(REGIR_SLOT_HEADER_BYTES + (outputOffset + index) * 8u, word);
    }
}
