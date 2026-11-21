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
    uint headerPadding;
};

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
    const float target = luminance * window / max(distanceSquared, halfCell * halfCell);
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
    ByteAddressBuffer structures = LightSamplingBuffer(sampling);
    const uint windowCell = slot / lightsPerCell;
    const int3 local = int3(windowCell % windowCells.x, (windowCell / windowCells.x) % windowCells.y,
        windowCell / (windowCells.x * windowCells.y));
    const int3 gridCell = originCell + local;
    const bool inGrid = all(gridCell >= 0) && all(gridCell < int3(sampling.gridDimensions));

    // The lookup region: the cell box widened by samplingJitter / 2 cells and the precision guard per side.
    const float margin = 0.5f * samplingJitter * sampling.cellSize + precisionGuard;
    const float3 base = sampling.gridOrigin + float3(gridCell) * sampling.cellSize;
    const float3 low = base - margin;
    const float3 high = base + sampling.cellSize + margin;

    uint listOffset = 0u;
    uint paddedCount = 0u;

    if(inGrid)
    {
        const uint cellIndex = uint(gridCell.x) + sampling.gridDimensions.x * (uint(gridCell.y) +
            sampling.gridDimensions.y * uint(gridCell.z));
        const uint3 cell = structures.Load3(LIGHT_SAMPLING_HEADER_BYTES + sampling.cellsOffset + cellIndex * 12u);
        listOffset = cell.x;
        paddedCount = cell.z;
    }

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

    uint2 word = uint2(0u, 0u);

    if(selectedLight != REGIR_NO_LIGHT && selectedTarget > 0.0f)
    {
        const uint rtxdiIndex = structures.Load(LIGHT_SAMPLING_HEADER_BYTES + sampling.padding + selectedLight * 4u);

        // A light without an RTXDI local index (directional; excluded above by its zero target) never enters a slot.
        if(rtxdiIndex != REGIR_NO_LIGHT)
        {
            word = uint2(rtxdiIndex, asuint(weightSum / selectedTarget));
        }
    }

    RWByteAddressBuffer slots = ResourceDescriptorHeap[outputIndex];
    slots.Store2(REGIR_SLOT_HEADER_BYTES + (outputOffset + slot) * 8u, word);

    if(slot == 0u)
    {
        slots.Store4(0u, uint4(REGIR_SLOT_MAGIC, asuint(windowCenter)));
        slots.Store4(16u, uint4(windowCells, lightsPerCell));
        slots.Store4(32u, uint4(asuint(sampling.cellSize), asuint(samplingJitter), frameSeed, 0u));
    }
}
