// AIR.1 camera-centred air field: binding and addressing shared by the air kernels and future consumers. Layout matches
// AirFieldBinding (src/Renderer/AirField.h); keep them identical.
//
// The field covers cells [minimum, minimum + dimensions) of a world-aligned lattice of cubes of edge cellSize (cell c covers
// world [c * cellSize, (c + 1) * cellSize)). Storage is toroidal: a cell lives at slot ((c mod N) + N) mod N per axis, so
// recentring moves `minimum` without moving data. Indices are read-only views; after the frame's air work the buffers are in a
// shader-read state.
//   velocity: float4 per slot (xyz in m/s, w reserved), index slot.x + N.x * (slot.y + N.y * slot.z)
//   obstacle: 2 bits per slot, 16 slots per u32 along x: bit 0 blocked, bit 1 known. Unknown (00) is free air.
//   pieces (AIR.4D): 16 bytes per slot, index as velocity: two 64-bit masks of the cell's 4 x 4 x 4 sub-cells, the sub-cells of
//     fluid piece 0 and of piece 1 (bit x + 4y + 16z). A piece is a set of free sub-cells connected face to face inside the cell;
//     piece 0 holds the lowest free sub-cell, piece 1 the lowest of the rest. Further pieces are in neither mask. A sub-cell is
//     free unless a voxel it covers is known blocked (unknown is free).
//   piece-1 velocity (AIR.4D D2): a second float4 pair in the velocity layout, the velocity of each cell's piece 1. The velocity
//     buffers above hold piece 0, so a consumer reading `velocityIndex` sees piece 0 of a split cell (the one holding its lowest
//     free sub-cell). A slot without a piece is never read; the step writes it as zero.

#ifndef AIR_HLSLI
#define AIR_HLSLI

struct AirFieldBinding
{
    uint valid;
    uint velocityIndex;
    uint obstacleIndex;
    uint generation;

    int3 minimum;
    float cellSize;

    uint3 dimensions;
    float inverseCellSize;

    uint velocityOtherIndex;
    uint pieceIndex;
    uint velocityPiece1Index;
    uint velocityPiece1OtherIndex;
};

static const uint AirSlotsPerObstacleWord = 16;
static const uint AirObstacleBlocked = 1;
static const uint AirObstacleKnown = 2;

uint3 AirSlotOf(AirFieldBinding field, int3 cell)
{
    const int3 size = int3(field.dimensions);
    return uint3(((cell % size) + size) % size);
}

// The cell a slot holds now: the one in [minimum, minimum + dimensions) congruent to it.
int3 AirCellOfSlot(AirFieldBinding field, uint3 slot)
{
    const int3 size = int3(field.dimensions);
    const int3 offset = ((int3(slot) - field.minimum) % size + size) % size;
    return field.minimum + offset;
}

bool AirContains(AirFieldBinding field, int3 cell)
{
    return all(cell >= field.minimum) && all(cell < field.minimum + int3(field.dimensions));
}

uint AirVelocityAddress(AirFieldBinding field, uint3 slot)
{
    return (slot.x + field.dimensions.x * (slot.y + field.dimensions.y * slot.z)) * 16;
}

uint AirObstacleWord(AirFieldBinding field, uint3 slot)
{
    return slot.x / AirSlotsPerObstacleWord + (field.dimensions.x / AirSlotsPerObstacleWord) * (slot.y + field.dimensions.y * slot.z);
}

// The two obstacle bits of a cell in the field: bit 0 blocked, bit 1 known. Outside the field: 0 (unknown, free).
uint AirObstacle(AirFieldBinding field, int3 cell)
{
    if(field.valid == 0 || !AirContains(field, cell))
    {
        return 0;
    }

    const uint3 slot = AirSlotOf(field, cell);
    ByteAddressBuffer obstacles = ResourceDescriptorHeap[NonUniformResourceIndex(field.obstacleIndex)];
    return (obstacles.Load(AirObstacleWord(field, slot) * 4) >> ((slot.x % AirSlotsPerObstacleWord) * 2)) & 3u;
}

// The latest velocity of a cell in the field; outside it, nothing is known and the caller supplies the ambient.
float3 AirVelocity(AirFieldBinding field, int3 cell)
{
    ByteAddressBuffer velocity = ResourceDescriptorHeap[NonUniformResourceIndex(field.velocityIndex)];
    return asfloat(velocity.Load3(AirVelocityAddress(field, AirSlotOf(field, cell))));
}

// AIR.4D sub-cell masks: 4 x 4 x 4 sub-cells per cell, bit x + 4y + 16z.
static const uint AirSubcellsPerAxis = 4;
static const uint64_t AirSubcellsAll = 0xFFFFFFFFFFFFFFFFull;
// The sub-cells on each face of the cell: x = 0, x = 3, y = 0, y = 3, z = 0, z = 3.
static const uint64_t AirSubcellsLowX = 0x1111111111111111ull;
static const uint64_t AirSubcellsHighX = 0x8888888888888888ull;
static const uint64_t AirSubcellsLowY = 0x000F000F000F000Full;
static const uint64_t AirSubcellsHighY = 0xF000F000F000F000ull;
static const uint64_t AirSubcellsLowZ = 0x000000000000FFFFull;
static const uint64_t AirSubcellsHighZ = 0xFFFF000000000000ull;

struct AirPieces
{
    uint64_t piece0;
    uint64_t piece1;
};

uint AirSubcellCount(uint64_t mask)
{
    return countbits(uint(mask)) + countbits(uint(mask >> 32));
}

// The index of the lowest set bit; 64 when none is.
uint AirLowestSubcell(uint64_t mask)
{
    const uint low = uint(mask);
    const uint high = uint(mask >> 32);
    return low != 0 ? firstbitlow(low) : (high != 0 ? 32 + firstbitlow(high) : 64);
}

// Every sub-cell face-adjacent to one in `mask`, within the cell, plus `mask` itself.
uint64_t AirDilateSubcells(uint64_t mask)
{
    return mask | ((mask & ~AirSubcellsHighX) << 1) | ((mask & ~AirSubcellsLowX) >> 1) | ((mask & ~AirSubcellsHighY) << 4) |
        ((mask & ~AirSubcellsLowY) >> 4) | (mask << 16) | (mask >> 16);
}

// The piece of `free` holding sub-cell `seed`: the free sub-cells reached from it face to face.
uint64_t AirFloodSubcells(uint64_t free, uint seed)
{
    uint64_t piece = uint64_t(1) << seed;

    // A path through 64 sub-cells takes at most 63 steps.
    for(uint step = 0; step < 63; ++step)
    {
        const uint64_t grown = AirDilateSubcells(piece) & free;

        if(grown == piece)
        {
            break;
        }

        piece = grown;
    }

    return piece;
}

// The two labelled pieces of a free sub-cell mask (AIR.4D layout above).
AirPieces AirLabelPieces(uint64_t free)
{
    AirPieces pieces;
    pieces.piece0 = free != 0 ? AirFloodSubcells(free, AirLowestSubcell(free)) : 0;
    const uint64_t rest = free & ~pieces.piece0;
    pieces.piece1 = rest != 0 ? AirFloodSubcells(rest, AirLowestSubcell(rest)) : 0;
    return pieces;
}

// The pieces of a cell in the window; outside it, none.
AirPieces AirPiecesOf(AirFieldBinding field, int3 cell)
{
    AirPieces pieces;
    pieces.piece0 = 0;
    pieces.piece1 = 0;

    if(field.valid == 0 || !AirContains(field, cell))
    {
        return pieces;
    }

    ByteAddressBuffer buffer = ResourceDescriptorHeap[NonUniformResourceIndex(field.pieceIndex)];
    const uint4 words = buffer.Load4(AirVelocityAddress(field, AirSlotOf(field, cell)));
    pieces.piece0 = uint64_t(words.x) | (uint64_t(words.y) << 32);
    pieces.piece1 = uint64_t(words.z) | (uint64_t(words.w) << 32);
    return pieces;
}

// The open area, in sub-cell faces (0 to 16), between `lower`'s piece and the piece of the next cell up `axis` (0 x, 1 y, 2 z):
// the matched sub-cell pairs across the shared face that both pieces hold.
uint AirPieceLink(uint64_t lower, uint64_t upper, uint axis)
{
    if(axis == 0)
    {
        return AirSubcellCount(((lower & AirSubcellsHighX) >> 3) & upper & AirSubcellsLowX);
    }

    if(axis == 1)
    {
        return AirSubcellCount(((lower & AirSubcellsHighY) >> 12) & upper & AirSubcellsLowY);
    }

    return AirSubcellCount(((lower & AirSubcellsHighZ) >> 48) & upper & AirSubcellsLowZ);
}

#endif
