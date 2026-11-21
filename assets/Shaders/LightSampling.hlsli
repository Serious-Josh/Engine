#ifndef LIGHT_SAMPLING_HLSLI
#define LIGHT_SAMPLING_HLSLI

// M4.1 shared light sampling: the shader side of LightSamplingData (src/Renderer/LightSampling). Usable from compute and
// DXR shaders; reads one raw buffer through LightSamplingConstants.bufferIndex. Not used by any production shader in M4.1.
//
// Pools at a shading point (disjoint by construction, Scalable_Light_Sampling_M4_0_Validation.md section 2):
// - inside the grid: its cell's binned lights (the nominal part of the list), the wide pool, the directional lights;
// - outside the grid (cell -1): the wide pool and the directional lights; no binned light reaches there.
// For a candidate drawn from pool t with probability p_t, a fixed-allocation RIS weight is w = p_hat / (K_t p_t); a pool
// that may contain a contributing light must receive at least one candidate. The global distribution covers every finite
// light (binned and wide): it overlaps the cell and wide pools, so a consumer mixing it with them needs the balance
// heuristic over all overlapping pools instead.
//
// Every probability returned here is the probability the draw procedure realizes for the returned light:
// - list draws use index = mulhi(random, count), whose exact realized probability LightSamplingUniformProbability computes;
// - alias draws use bin = mulhi(r0, count), then the bin if r1 < threshold, else its alias; the table stores each light's
//   realized probability (computed on the CPU from the uint32 thresholds).
// Random inputs are uniform uint32 values.
//
// The padded part of a list (lights reaching only the cell box widened by paddedMargin) exists for RTXDI ReGIR's jittered
// cell lookup. Indirect NEE samples the nominal part: the padded members contribute nothing at points inside the cell.
//
// M4.2A: the buffer begins with a LIGHT_SAMPLING_HEADER_BYTES header holding the published constants
// (LightSamplingLoadConstants); every section offset in the constants is relative to the end of the header.

#define LIGHT_SAMPLING_HEADER_BYTES 128u

// Identical in layout to LightSamplingConstants in ShaderData.h.
struct LightSamplingConstants
{
    float3 gridOrigin;
    float cellSize;

    uint3 gridDimensions;
    uint cellCount;

    float inverseCellSize;
    float paddedMargin;
    uint bufferIndex;
    uint valid;

    uint cellsOffset;
    uint listsOffset;
    uint wideOffset;
    uint wideCount;

    uint globalOffset;
    uint globalCount;
    uint directionalOffset;
    uint directionalCount;

    uint tableLightCount;
    uint revisionLow;
    uint revisionHigh;
    uint padding;
};

// One cell's list: lights [offset, offset + nominalCount) reach the cell box; [offset + nominalCount, offset + paddedCount)
// reach only the padded box.
struct LightSamplingCell
{
    uint offset;
    uint nominalCount;
    uint paddedCount;
};

// A drawn light and the probability with which the draw returns it. valid is false only for an empty pool.
struct LightSamplingSample
{
    uint light;
    float probability;
    bool valid;
};

// The pools at one shading point.
struct LightSamplingPools
{
    // -1 outside the grid (or with no binned lights); cell holds an empty list then.
    int cellIndex;
    LightSamplingCell cell;
    uint wideCount;
    uint directionalCount;
};

bool LightSamplingAvailable(LightSamplingConstants constants)
{
    return constants.valid != 0u;
}

ByteAddressBuffer LightSamplingBuffer(LightSamplingConstants constants)
{
    return ResourceDescriptorHeap[constants.bufferIndex];
}

// The constants published with the buffer at `bufferIndex` (its header). A consumer must still check valid and that
// tableLightCount equals the scene light count it binds (same publication).
LightSamplingConstants LightSamplingLoadConstants(uint bufferIndex)
{
    ByteAddressBuffer buffer = ResourceDescriptorHeap[bufferIndex];
    const uint4 row0 = buffer.Load4(0u);
    const uint4 row1 = buffer.Load4(16u);
    const uint4 row2 = buffer.Load4(32u);
    const uint4 row3 = buffer.Load4(48u);
    const uint4 row4 = buffer.Load4(64u);
    const uint4 row5 = buffer.Load4(80u);
    LightSamplingConstants constants;
    constants.gridOrigin = asfloat(row0.xyz);
    constants.cellSize = asfloat(row0.w);
    constants.gridDimensions = row1.xyz;
    constants.cellCount = row1.w;
    constants.inverseCellSize = asfloat(row2.x);
    constants.paddedMargin = asfloat(row2.y);
    constants.bufferIndex = row2.z;
    constants.valid = row2.w;
    constants.cellsOffset = row3.x;
    constants.listsOffset = row3.y;
    constants.wideOffset = row3.z;
    constants.wideCount = row3.w;
    constants.globalOffset = row4.x;
    constants.globalCount = row4.y;
    constants.directionalOffset = row4.z;
    constants.directionalCount = row4.w;
    constants.tableLightCount = row5.x;
    constants.revisionLow = row5.y;
    constants.revisionHigh = row5.z;
    constants.padding = row5.w;
    return constants;
}

// The high 32 bits of a * b: maps a uniform uint32 onto [0, count) with probabilities differing from 1/count by at most
// count / 2^32 (LightSamplingUniformProbability gives them exactly).
uint LightSamplingMulHi(uint a, uint b)
{
    return uint((uint64_t(a) * uint64_t(b)) >> 32);
}

// The exact probability that LightSamplingMulHi(random, count) returns `index` for a uniform uint32 random: the number of
// uint32 values mapping to it, ceil((index + 1) 2^32 / count) - ceil(index 2^32 / count), over 2^32.
float LightSamplingUniformProbability(uint index, uint count)
{
    const uint64_t scale = uint64_t(1) << 32;
    const uint64_t first = (uint64_t(index) * scale + uint64_t(count) - 1u) / uint64_t(count);
    const uint64_t next = ((uint64_t(index) + 1u) * scale + uint64_t(count) - 1u) / uint64_t(count);
    return float(next - first) * (1.0f / 4294967296.0f);
}

// The linear index of the cell containing `position`, or -1 outside the grid (or for a non-finite position). The CPU
// build widens every range by a bound on this float lookup's rounding, so a point near a cell boundary finds every light
// that reaches it in whichever adjacent cell the rounding selects.
int LightSamplingCellIndex(LightSamplingConstants constants, float3 position)
{
    if(constants.valid == 0u || constants.cellCount == 0u)
    {
        return -1;
    }

    const float3 local = (position - constants.gridOrigin) * constants.inverseCellSize;

    if(any(!isfinite(local)) || any(local < 0.0f))
    {
        return -1;
    }

    const uint3 cell = uint3(floor(local));

    if(any(cell >= constants.gridDimensions))
    {
        return -1;
    }

    return int(cell.x + constants.gridDimensions.x * (cell.y + constants.gridDimensions.y * cell.z));
}

LightSamplingCell LightSamplingLoadCell(LightSamplingConstants constants, int cellIndex)
{
    LightSamplingCell cell = (LightSamplingCell)0;

    if(cellIndex >= 0)
    {
        const uint3 words = LightSamplingBuffer(constants).Load3(LIGHT_SAMPLING_HEADER_BYTES + constants.cellsOffset +
            uint(cellIndex) * 12u);
        cell.offset = words.x;
        cell.nominalCount = words.y;
        cell.paddedCount = words.z;
    }

    return cell;
}

LightSamplingPools LightSamplingPoolsAt(LightSamplingConstants constants, float3 position)
{
    LightSamplingPools pools;
    pools.cellIndex = LightSamplingCellIndex(constants, position);
    pools.cell = LightSamplingLoadCell(constants, pools.cellIndex);
    pools.wideCount = constants.valid != 0u ? constants.wideCount : 0u;
    pools.directionalCount = constants.valid != 0u ? constants.directionalCount : 0u;
    return pools;
}

// A uniform draw from the cell's list: the nominal part, or (padded) the whole list.
LightSamplingSample LightSamplingDrawCellLight(LightSamplingConstants constants, LightSamplingCell cell, bool padded,
    uint random)
{
    LightSamplingSample result = (LightSamplingSample)0;
    const uint count = padded ? cell.paddedCount : cell.nominalCount;

    if(count == 0u)
    {
        return result;
    }

    const uint index = LightSamplingMulHi(random, count);
    result.light = LightSamplingBuffer(constants).Load(LIGHT_SAMPLING_HEADER_BYTES + constants.listsOffset +
        (cell.offset + index) * 4u);
    result.probability = LightSamplingUniformProbability(index, count);
    result.valid = true;
    return result;
}

// An alias-table draw: entries are {threshold, alias entry, realized probability (float bits), light index}.
LightSamplingSample LightSamplingDrawAlias(LightSamplingConstants constants, uint tableOffset, uint count, uint random0,
    uint random1)
{
    LightSamplingSample result = (LightSamplingSample)0;

    if(constants.valid == 0u || count == 0u)
    {
        return result;
    }

    ByteAddressBuffer buffer = LightSamplingBuffer(constants);
    const uint bin = LightSamplingMulHi(random0, count);
    const uint tableStart = LIGHT_SAMPLING_HEADER_BYTES + tableOffset;
    uint4 entry = buffer.Load4(tableStart + bin * 16u);

    if(random1 >= entry.x)
    {
        entry = buffer.Load4(tableStart + entry.y * 16u);
    }

    result.light = entry.w;
    result.probability = asfloat(entry.z);
    result.valid = true;
    return result;
}

LightSamplingSample LightSamplingDrawWide(LightSamplingConstants constants, uint random0, uint random1)
{
    return LightSamplingDrawAlias(constants, constants.wideOffset, constants.wideCount, random0, random1);
}

LightSamplingSample LightSamplingDrawGlobal(LightSamplingConstants constants, uint random0, uint random1)
{
    return LightSamplingDrawAlias(constants, constants.globalOffset, constants.globalCount, random0, random1);
}

// The index-th directional table light (index < directionalCount); enumerated, not sampled.
uint LightSamplingDirectionalLight(LightSamplingConstants constants, uint index)
{
    return LightSamplingBuffer(constants).Load(LIGHT_SAMPLING_HEADER_BYTES + constants.directionalOffset + index * 4u);
}

// -----------------------------------------------------------------------------
// M4.2A: fixed-allocation RIS over the disjoint finite-light pools
// -----------------------------------------------------------------------------
//
// Candidates per pool at a shading point (deterministic in the pools, never in random outcomes), for K candidates:
// - outside the grid: K from the global distribution (it alone covers every finite light there);
// - inside the grid, cell list and wide pool both non-empty: K - 1 from the cell list, 1 from the wide pool;
// - inside the grid, only one of them non-empty: all K from it;
// - inside the grid, both empty: none (no finite light reaches the point).
// Every pool that can hold a contributing light receives at least one candidate (K >= 2 when both are non-empty), and the
// pools are disjoint, so a candidate y from pool t has RIS weight w = p_hat(y) / (K_t p_t(y)); with Y selected in
// proportion to w, F = f(Y) / p_hat(Y) * sum(w) is unbiased (Scalable_Light_Sampling_M4_0_Validation.md section 2.3). There is
// no further division by K: the K_t are inside the weights.

struct LightSamplingAllocation
{
    uint cellCandidates;
    uint wideCandidates;
    uint globalCandidates;
};

LightSamplingAllocation LightSamplingAllocate(LightSamplingConstants constants, LightSamplingPools pools, uint candidates)
{
    LightSamplingAllocation allocation = (LightSamplingAllocation)0;
    candidates = max(candidates, 2u);

    if(pools.cellIndex < 0)
    {
        allocation.globalCandidates = constants.globalCount != 0u ? candidates : 0u;
        return allocation;
    }

    const bool hasCell = pools.cell.nominalCount != 0u;
    const bool hasWide = pools.wideCount != 0u;
    allocation.wideCandidates = hasWide ? (hasCell ? 1u : candidates) : 0u;
    allocation.cellCandidates = hasCell ? candidates - allocation.wideCandidates : 0u;
    return allocation;
}

uint LightSamplingCandidateCount(LightSamplingAllocation allocation)
{
    return allocation.cellCandidates + allocation.wideCandidates + allocation.globalCandidates;
}

// Candidate `index` of the allocation (cell candidates first, then wide, then global): its light and the RIS weight's
// denominator K_t p_t(light). valid is false only for an empty pool, which the allocation never assigns.
struct LightSamplingCandidate
{
    uint light;
    float density;
    bool valid;
};

LightSamplingCandidate LightSamplingDrawCandidate(LightSamplingConstants constants, LightSamplingPools pools,
    LightSamplingAllocation allocation, uint index, uint random0, uint random1)
{
    LightSamplingSample sample;
    uint poolCandidates;

    if(index < allocation.cellCandidates)
    {
        sample = LightSamplingDrawCellLight(constants, pools.cell, false, random0);
        poolCandidates = allocation.cellCandidates;
    }
    else if(index < allocation.cellCandidates + allocation.wideCandidates)
    {
        sample = LightSamplingDrawWide(constants, random0, random1);
        poolCandidates = allocation.wideCandidates;
    }
    else
    {
        sample = LightSamplingDrawGlobal(constants, random0, random1);
        poolCandidates = allocation.globalCandidates;
    }

    LightSamplingCandidate candidate;
    candidate.light = sample.light;
    candidate.density = float(poolCandidates) * sample.probability;
    candidate.valid = sample.valid && candidate.density > 0.0f;
    return candidate;
}

#endif
