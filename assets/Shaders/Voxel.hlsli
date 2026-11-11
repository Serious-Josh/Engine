// VX.5 static voxel obstacle queries (VoxelArchitecture.md §6.10). The GPU equivalent of QueryVoxel, QueryCell and QueryBox in
// src/Voxel/VoxelResidency.cpp, over the buffers VoxelResidencyResources publishes. Keep the layouts identical to
// VoxelWorldBinding (VoxelResidencyResources.h), PackedBrick and TableEntry (VoxelResidency.h).
//
// Every query returns { blocked, known }:
//   known blocked: some covered voxel is known blocked;
//   known free:    every covered voxel is known free;
//   unknown:       otherwise (then blocked is false).
// Unknown covers an unpublished binding, space outside the manifest's extent (unbaked), regions not resident, and failed regions.
//
// Integer voxel coordinates are authoritative. VoxelWorldToVoxel is exact only while |position| / voxelSize < 2^24 (float).
// Boxes must span fewer than 2^31 voxels per axis. Requires SM 6.6 (ResourceDescriptorHeap).

#ifndef VOXEL_HLSLI
#define VOXEL_HLSLI

struct VoxelWorldBinding
{
    uint valid;
    uint window;
    uint tableIndex;
    uint tableBase;

    int regionBricksLog2;
    int baseExponent;
    uint pagesPerChunk;
    uint slotsPerChunk;

    float voxelSize;
    float inverseVoxelSize;
    uint generation;
    uint padding;

    uint4 brickChunks[2];
    uint4 indexChunks[2];
    uint4 reserved;
};

struct VoxelSample
{
    bool blocked;
    bool known;
};

static const uint VoxelBrickLevel = 3;
static const uint VoxelBricksPerPage = 64;
static const uint VoxelPageBytes = 64 * 80;
static const uint VoxelBrickBytes = 80;
static const uint VoxelBlocked2Word = 16;
static const uint VoxelBlocked4Word = 18;
static const uint VoxelUniformOutside = 0xFFFFFFFFu;
static const uint VoxelUniformInside = 0xFFFFFFFEu;

static const uint VoxelStateUnknown = 0;
static const uint VoxelStateKnownEmpty = 1;
static const uint VoxelStateUniformInside = 2;
static const uint VoxelStatePopulated = 3;

VoxelSample VoxelMakeSample(bool blocked, bool known)
{
    VoxelSample sample;
    sample.blocked = blocked && known;
    sample.known = known;
    return sample;
}

// An OR of parts: blocked when any part is known blocked; known when that, or when every part is known.
struct VoxelAccumulator
{
    bool anyBlocked;
    bool allKnown;
};

VoxelAccumulator VoxelBeginOr()
{
    VoxelAccumulator accumulator;
    accumulator.anyBlocked = false;
    accumulator.allKnown = true;
    return accumulator;
}

void VoxelAddOr(inout VoxelAccumulator accumulator, VoxelSample sample)
{
    accumulator.anyBlocked = accumulator.anyBlocked || (sample.known && sample.blocked);
    accumulator.allKnown = accumulator.allKnown && sample.known;
}

VoxelSample VoxelEndOr(VoxelAccumulator accumulator)
{
    return VoxelMakeSample(accumulator.anyBlocked, accumulator.anyBlocked || accumulator.allKnown);
}

uint VoxelChunkDescriptor(uint4 chunks[2], uint chunk)
{
    return chunks[chunk >> 2][chunk & 3];
}

int VoxelRegionLevel(VoxelWorldBinding binding)
{
    return int(VoxelBrickLevel) + binding.regionBricksLog2;
}

// The table slot of a region key: its key modulo the window per axis, non-negative for negative keys.
uint VoxelTableSlot(int3 key, uint window)
{
    const int size = int(window);
    const uint3 wrapped = uint3(((key % size) + size) % size);
    return wrapped.x + window * (wrapped.y + window * wrapped.z);
}

// The table entry of a region, if the table holds that region: state in bits 0-1, index slot above. Unknown otherwise.
uint VoxelFindRegion(VoxelWorldBinding binding, int3 key, out uint indexSlot)
{
    indexSlot = 0;
    ByteAddressBuffer table = ResourceDescriptorHeap[NonUniformResourceIndex(binding.tableIndex)];
    const uint4 entry = table.Load4((binding.tableBase + VoxelTableSlot(key, binding.window)) * 16);

    if(any(int3(entry.xyz) != key))
    {
        return VoxelStateUnknown;
    }

    indexSlot = entry.w >> 2;
    return entry.w & 3u;
}

// A whole region: a populated region holds a surface voxel, so it is blocked.
VoxelSample VoxelRegionSummary(uint state)
{
    if(state == VoxelStateKnownEmpty)
    {
        return VoxelMakeSample(false, true);
    }

    if(state == VoxelStateUniformInside || state == VoxelStatePopulated)
    {
        return VoxelMakeSample(true, true);
    }

    return VoxelMakeSample(false, false);
}

uint VoxelIndexEntry(VoxelWorldBinding binding, uint indexSlot, uint3 brick)
{
    const uint bricks = 1u << uint(binding.regionBricksLog2);
    const uint sliceBytes = bricks * bricks * bricks * 4;
    const uint chunk = indexSlot / binding.slotsPerChunk;
    ByteAddressBuffer slots = ResourceDescriptorHeap[NonUniformResourceIndex(VoxelChunkDescriptor(binding.indexChunks, chunk))];
    const uint cell = brick.x + bricks * (brick.y + bricks * brick.z);
    return slots.Load((indexSlot % binding.slotsPerChunk) * sliceBytes + cell * 4);
}

bool VoxelBrickBit(VoxelWorldBinding binding, uint poolBrick, uint firstWord, uint bit)
{
    const uint page = poolBrick / VoxelBricksPerPage;
    const uint chunk = page / binding.pagesPerChunk;
    ByteAddressBuffer pages = ResourceDescriptorHeap[NonUniformResourceIndex(VoxelChunkDescriptor(binding.brickChunks, chunk))];
    const uint offset = (page % binding.pagesPerChunk) * VoxelPageBytes + (poolBrick % VoxelBricksPerPage) * VoxelBrickBytes +
        (firstWord + (bit >> 5)) * 4;
    return ((pages.Load(offset) >> (bit & 31u)) & 1u) != 0;
}

// A lattice-aligned cell of 2^level voxels per axis; cell c covers voxels [c * 2^level, (c + 1) * 2^level). The exact OR of
// those voxels. Reads: one bit (levels 0-2), one index entry (level 3), 8^(level - 3) index entries (up to the region level),
// (2^(level - region level))^3 region summaries (at or above it).
VoxelSample VoxelQueryCell(VoxelWorldBinding binding, int3 cell, int level)
{
    if(binding.valid == 0 || binding.window == 0 || level < 0)
    {
        return VoxelMakeSample(false, false);
    }

    const int regionLevel = VoxelRegionLevel(binding);
    uint indexSlot = 0;

    // At or above the region level: OR of whole-region summaries.
    if(level >= regionLevel)
    {
        const int shift = level - regionLevel;
        const int span = int(1) << shift;
        VoxelAccumulator accumulator = VoxelBeginOr();

        for(int z = 0; z < span; ++z)
        {
            for(int y = 0; y < span; ++y)
            {
                for(int x = 0; x < span; ++x)
                {
                    VoxelAddOr(accumulator, VoxelRegionSummary(VoxelFindRegion(binding, (cell << shift) + int3(x, y, z), indexSlot)));
                }
            }
        }

        return VoxelEndOr(accumulator);
    }

    // Below it: the region holding the cell (arithmetic shift floors negative coordinates), then the cell inside it.
    const int toRegion = regionLevel - level;
    const uint state = VoxelFindRegion(binding, cell >> toRegion, indexSlot);

    if(state != VoxelStatePopulated)
    {
        return VoxelRegionSummary(state);
    }

    const uint3 local = uint3(cell & ((int(1) << toRegion) - 1));

    // Brick level and above: an allocated or uniformly inside brick is blocked.
    if(level >= int(VoxelBrickLevel))
    {
        const uint shift = uint(level) - VoxelBrickLevel;
        const uint span = 1u << shift;

        for(uint z = 0; z < span; ++z)
        {
            for(uint y = 0; y < span; ++y)
            {
                for(uint x = 0; x < span; ++x)
                {
                    if(VoxelIndexEntry(binding, indexSlot, (local << shift) + uint3(x, y, z)) != VoxelUniformOutside)
                    {
                        return VoxelMakeSample(true, true);
                    }
                }
            }
        }

        return VoxelMakeSample(false, true);
    }

    // Inside one brick: its mask at the requested level.
    const uint toBrick = VoxelBrickLevel - uint(level);
    const uint value = VoxelIndexEntry(binding, indexSlot, local >> toBrick);

    if(value == VoxelUniformOutside || value == VoxelUniformInside)
    {
        return VoxelMakeSample(value == VoxelUniformInside, true);
    }

    const uint3 inBrick = local & ((1u << toBrick) - 1u);

    if(level == 0)
    {
        return VoxelMakeSample(VoxelBrickBit(binding, value, 0, inBrick.x + 8 * (inBrick.y + 8 * inBrick.z)), true);
    }

    if(level == 1)
    {
        return VoxelMakeSample(VoxelBrickBit(binding, value, VoxelBlocked2Word, inBrick.x + 4 * (inBrick.y + 4 * inBrick.z)), true);
    }

    return VoxelMakeSample(VoxelBrickBit(binding, value, VoxelBlocked4Word, inBrick.x + 2 * (inBrick.y + 2 * inBrick.z)), true);
}

VoxelSample VoxelQueryVoxel(VoxelWorldBinding binding, int3 voxel)
{
    return VoxelQueryCell(binding, voxel, 0);
}

// Conservative box query over the inclusive voxel range [minimum, maximum]: never misses a blocked voxel. The level is the
// finest whose reads fit maximumReads; its cells floor both bounds, so they cover the box and can only add voxels. A box too
// large even in whole regions is unknown.
VoxelSample VoxelQueryBox(VoxelWorldBinding binding, int3 minimum, int3 maximum, uint maximumReads)
{
    if(binding.valid == 0 || binding.window == 0)
    {
        return VoxelMakeSample(false, false);
    }

    if(any(minimum > maximum))
    {
        return VoxelMakeSample(false, true);
    }

    const int regionLevel = VoxelRegionLevel(binding);

    for(int level = 0; level <= regionLevel; ++level)
    {
        const int3 low = minimum >> level;
        const int3 high = maximum >> level;
        const uint3 extent = uint3(high - low) + 1u;
        const bool aboveBricks = level > int(VoxelBrickLevel) && level < regionLevel;
        const uint readsPerCell = aboveBricks ? 1u << (3u * uint(level - int(VoxelBrickLevel))) : 1u;

        // Each factor is checked alone first, so the product below cannot overflow.
        if(any(extent > maximumReads) || readsPerCell > maximumReads || extent.x * extent.y * extent.z * readsPerCell > maximumReads)
        {
            continue;
        }

        VoxelAccumulator accumulator = VoxelBeginOr();

        for(int z = low.z; z <= high.z; ++z)
        {
            for(int y = low.y; y <= high.y; ++y)
            {
                for(int x = low.x; x <= high.x; ++x)
                {
                    VoxelAddOr(accumulator, VoxelQueryCell(binding, int3(x, y, z), level));
                }
            }
        }

        return VoxelEndOr(accumulator);
    }

    return VoxelMakeSample(false, false);
}

// World-position wrapper: floor(position / voxelSize). Exact only while |position| / voxelSize < 2^24.
int3 VoxelWorldToVoxel(VoxelWorldBinding binding, float3 position)
{
    return int3(floor(position * binding.inverseVoxelSize));
}

#endif
