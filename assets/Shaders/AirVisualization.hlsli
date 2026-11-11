// AIR.4A/AIR.4B velocity-vector visualization, piece-aware since AIR.4D D3: where each vector starts, the velocity it shows and where
// it ends. Shared by EditorAir.hlsl and the verification's compute shader; mirrored by AirVectorParameters
// (src/Renderer/Passes/EditorAirPass.h).
//
// Sampling is independent of the simulation's cells. World space is divided into world-aligned blocks quarterCells / 4 cells on a
// side (a power of two, 1/4 to 8 cells), and each block chooses a point inside it by hashing its coordinates. The point depends only
// on the block, never on the toroidal slot or the camera, so vectors stay put as the window recentres and the box follows the camera.
// Two placements (AIR.4D D3):
//   - hashed points (the default): one vector per block, starting at its point. The point's sub-cell decides the fluid piece it is
//     in (AirSampleAt); a point in solid, or in a sub-cell no piece holds, draws nothing. The velocity is that piece's sample (the D2
//     solver's rules, AirSample.hlsli): never a blend of two pieces of one cell; corners outside the window read the ambient;
//   - piece anchors: two vectors per block (blocks of at least one cell), for piece 0 and piece 1 of the cell holding the block's
//     point, each starting at its piece's anchor (AirPieceAnchor, where the solver keeps that piece's velocity) and showing that
//     piece's own velocity. An absent piece draws nothing.
//
// A vector runs along its velocity for min(speed * lengthScale, maximumLength) metres, cut short where the line would enter a
// closed sub-cell (AirTraverse: solid, or a piece the cell drops), so no line crosses solid however thin. Slower than minimumSpeed,
// it is not drawn.
//
// Two passes draw hashed points (EditorAirVectors.hlsli): the lean one samples only points whose eight corners are whole cells (or
// outside the window), a plain trilinear blend; the split pass samples the rest with the full D2 rules. A file defines
// AIR_VECTOR_SPLIT as 1 to compile the full sampling at all; without it, AirVectorAt hides the points beside a split cell.

#ifndef AIR_VISUALIZATION_HLSLI
#define AIR_VISUALIZATION_HLSLI

#ifndef AIR_VECTOR_SPLIT
#define AIR_VECTOR_SPLIT 0
#endif

#include "AirSample.hlsli"

static const uint AirVectorPlacementHashed = 0;
static const uint AirVectorPlacementAnchors = 1;

struct AirVectorParameters
{
    // Blocks per axis in the drawn box, and the block edge in quarter cells (1 to 32).
    uint3 blocks;
    uint quarterCells;

    // The box's first block (block b covers quarter cells [b * quarterCells, (b + 1) * quarterCells)).
    int3 firstBlock;
    float lengthScale;

    float maximumLength;
    float colorMaximumSpeed;
    float minimumSpeed;
    // AirVectorPlacementHashed or AirVectorPlacementAnchors; instances per block are 1 or 2.
    uint placement;

    // AIR.4D D3: the velocity outside the window, which samples near its edge blend.
    float3 ambientVelocity;
    uint vectorsPerBlock;
};

struct AirVector
{
    float3 start;
    float3 end;
    float3 color;
    bool visible;
};

// The most cell faces a vector's line is followed across; longer lines stop there.
static const uint AirVectorMaximumCrossings = 64;

// A deterministic 32-bit hash of block coordinates (integer only, identical on CPU and GPU).
uint AirVectorHash(int3 block, uint salt)
{
    uint h = asuint(block.x) * 0x8DA6B343u ^ asuint(block.y) * 0xD8163841u ^ asuint(block.z) * 0xCB1AB31Fu ^ salt * 0x9E3779B9u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

// Blue -> cyan -> yellow -> red over [0, colorMaximumSpeed].
float3 AirSpeedColor(float speed, float maximumSpeed)
{
    const float t = saturate(speed / max(maximumSpeed, 1e-6));

    if(t < 1.0 / 3.0)
    {
        return lerp(float3(0.1, 0.3, 1.0), float3(0.0, 1.0, 1.0), t * 3.0);
    }

    if(t < 2.0 / 3.0)
    {
        return lerp(float3(0.0, 1.0, 1.0), float3(1.0, 1.0, 0.0), t * 3.0 - 1.0);
    }

    return lerp(float3(1.0, 1.0, 0.0), float3(1.0, 0.1, 0.0), t * 3.0 - 2.0);
}

// The block's point in cell units relative to the window's minimum (cell c spans [c - minimum, c - minimum + 1)). The jitter is
// 10 bits per axis (1/1024 of a block), so every step here is exact in float: the point, and everything sampled from it, is
// bit-identical in whichever window holds it.
float3 AirVectorStart(AirFieldBinding field, AirVectorParameters parameters, int3 block)
{
    const int3 quarters = block * int(parameters.quarterCells) - field.minimum * 4;
    const float3 jitter = float3(AirVectorHash(block, 4u) >> 22, AirVectorHash(block, 5u) >> 22, AirVectorHash(block, 6u) >> 22) * (1.0 / 1024.0);
    return (float3(quarters) + jitter * float(parameters.quarterCells)) * 0.25;
}

// The published field read through read-only views (the buffers are in a shader-read state after the air work).
AirPathField AirVectorPath(AirFieldBinding field, AirVectorParameters parameters)
{
    AirPathField path;
    path.field = field;
    path.obstacleIndex = field.obstacleIndex;
    path.pieceIndex = field.pieceIndex;
    path.velocityIndex = field.velocityIndex;
    path.velocityPiece1Index = field.velocityPiece1Index;
    path.ambient = parameters.ambientVelocity;
    return path;
}

// Where a vector starts and what it shows, before its line: `position` in cell units from the window minimum, inside piece `piece`
// of `cell`.
struct AirVectorSource
{
    bool valid;
    float3 position;
    int3 cell;
    uint piece;
    float3 velocity;
};

// The hashed point of a block: the fluid piece holding it (AirNoPiece in a closed sub-cell), and whether its sample is plain (all
// eight corners whole cells or outside the window).
struct AirHashedPoint
{
    float3 position;
    int3 cell;
    uint piece;
    int3 base;
    bool plain;
};

AirHashedPoint AirHashedPointOf(AirPathField path, AirVectorParameters parameters, int3 block)
{
    AirHashedPoint hashed;
    hashed.position = AirVectorStart(path.field, parameters, block);
    hashed.cell = path.field.minimum + int3(floor(hashed.position));
    const AirCell holder = AirLoadCell(path, hashed.cell);
    const int3 subcell = clamp(int3(floor(hashed.position * 4.0)) - (hashed.cell - path.field.minimum) * 4, 0, 3);
    hashed.piece = holder.inside ? AirPieceOfSubcell(holder, subcell) : AirNoPiece;
    hashed.base = int3(floor(hashed.position - 0.5));
    hashed.plain = AirCornersOpen(path, hashed.base);
    return hashed;
}

// Hashed placement: the block's point, in whatever piece holds it. `full`: the split pass, which samples any point by the full D2
// rules (AirSampleAtPosition); otherwise only plain points are sampled (plain trilinear, which the full rules reduce to there) and
// the others are left to the split pass.
AirVectorSource AirHashedSource(AirPathField path, AirVectorParameters parameters, int3 block, bool full)
{
    AirVectorSource source;
    source.valid = false;
    source.velocity = float3(0.0, 0.0, 0.0);
    const AirHashedPoint hashed = AirHashedPointOf(path, parameters, block);
    source.position = hashed.position;
    source.cell = hashed.cell;
    source.piece = hashed.piece;

#if AIR_VECTOR_SPLIT
    if(full)
    {
        const AirPointSample sample = AirSampleAtPosition(path, source.position);
        source.valid = sample.state == AirPointFluid;
        source.velocity = sample.velocity;
        return source;
    }
#endif

    if(hashed.piece == AirNoPiece || !hashed.plain)
    {
        return source;
    }

    source.valid = true;
    source.velocity = AirSampleOpen(path, hashed.base, hashed.position - 0.5 - float3(hashed.base), float3(0.0, 0.0, 0.0));
    return source;
}

// Whether the lean pass leaves instance `instance` to the split pass: a hashed point in fluid whose sample is beside a split cell.
bool AirVectorNeedsSplit(AirFieldBinding field, AirVectorParameters parameters, uint instance)
{
    if(field.valid == 0 || parameters.placement != AirVectorPlacementHashed)
    {
        return false;
    }

    const uint blockIndex = instance / parameters.vectorsPerBlock;
    const uint3 index = uint3(blockIndex % parameters.blocks.x, (blockIndex / parameters.blocks.x) % parameters.blocks.y,
        blockIndex / (parameters.blocks.x * parameters.blocks.y));
    const AirHashedPoint hashed = AirHashedPointOf(AirVectorPath(field, parameters), parameters, parameters.firstBlock + int3(index));
    return hashed.piece != AirNoPiece && !hashed.plain;
}

// Anchor placement: piece `piece` of the cell holding the block's point, at its anchor, with its own velocity.
AirVectorSource AirAnchorSource(AirPathField path, AirVectorParameters parameters, int3 block, uint piece)
{
    AirVectorSource source;
    source.valid = false;
    source.position = AirVectorStart(path.field, parameters, block);
    source.cell = path.field.minimum + int3(floor(source.position));
    source.piece = piece;
    source.velocity = float3(0.0, 0.0, 0.0);
    const AirCell holder = AirLoadCell(path, source.cell);
    const uint64_t mask = AirPieceMask(holder, piece);

    if(!holder.inside || mask == 0)
    {
        return source;
    }

    source.valid = true;
    source.position = float3(source.cell - path.field.minimum) + AirPieceAnchor(mask);
    source.velocity = AirPieceVelocity(path, source.cell, piece);
    return source;
}

// Whether the whole line `origin` -> `origin + displacement` (cell units from the window minimum) stays in whole cells or outside the
// window, walking cell faces in order. Such a line cannot meet a closed sub-cell, so it needs no sub-cell traversal; the walk reads
// only obstacle bits. False as soon as a cell holding solid is touched, or past the crossing limit.
bool AirVectorLineOpen(AirPathField path, float3 origin, float3 displacement)
{
    int3 cell = int3(floor(origin));
    const int3 step = int3(sign(displacement));
    // Parameters along the line of the next face crossed on each axis, and between faces; an axis it never crosses stays beyond 1.
    float3 next = 2.0;
    float3 across = 2.0;

    for(uint component = 0; component < 3; ++component)
    {
        if(step[component] != 0)
        {
            const float boundary = float(cell[component] + (step[component] > 0 ? 1 : 0));
            next[component] = (boundary - origin[component]) / displacement[component];
            across[component] = abs(1.0 / displacement[component]);
        }
    }

    if(!AirCellIsOpen(path, path.field.minimum + cell))
    {
        return false;
    }

    for(uint crossing = 0; crossing < AirVectorMaximumCrossings; ++crossing)
    {
        const float t = min(next.x, min(next.y, next.z));

        if(t >= 1.0)
        {
            return true;
        }

        // Faces crossed together (through an edge or corner, within AirTraverse's tie tolerance) are settled by its rules, which
        // look at every cell around the edge: leave those lines to it.
        const uint tied = uint(next.x <= t + AirTieEpsilon) + uint(next.y <= t + AirTieEpsilon) + uint(next.z <= t + AirTieEpsilon);

        if(tied > 1)
        {
            return false;
        }

        const uint axis = next.x == t ? 0 : (next.y == t ? 1 : 2);
        cell[axis] += step[axis];
        next[axis] += across[axis];

        if(!AirCellIsOpen(path, path.field.minimum + cell))
        {
            return false;
        }
    }

    return false;
}

// `withEnd` false skips the line walk: the tail vertex needs only the start, colour and visibility. `full`: the split pass (see
// AirHashedSource); it needs AIR_VECTOR_SPLIT.
AirVector AirVectorAt(AirFieldBinding field, AirVectorParameters parameters, uint instance, bool withEnd, bool full)
{
    AirVector result;
    result.start = 0.0;
    result.end = 0.0;
    result.color = 0.0;
    result.visible = false;

    if(field.valid == 0)
    {
        return result;
    }

    // Instances run through the pieces of a block, then x fastest, then y, then z.
    const uint blockIndex = instance / parameters.vectorsPerBlock;
    const uint3 index = uint3(blockIndex % parameters.blocks.x, (blockIndex / parameters.blocks.x) % parameters.blocks.y,
        blockIndex / (parameters.blocks.x * parameters.blocks.y));
    const int3 block = parameters.firstBlock + int3(index);
    const AirPathField path = AirVectorPath(field, parameters);
    AirVectorSource source;

    if(parameters.placement == AirVectorPlacementAnchors)
    {
        source = AirAnchorSource(path, parameters, block, instance % parameters.vectorsPerBlock);
    }
    else
    {
        source = AirHashedSource(path, parameters, block, full);
    }

    const float speed = length(source.velocity);

    if(!source.valid || !(speed >= parameters.minimumSpeed))
    {
        return result;
    }

    const float3 origin = float3(field.minimum) * field.cellSize;
    result.start = origin + source.position * field.cellSize;
    result.color = AirSpeedColor(speed, parameters.colorMaximumSpeed);
    result.visible = true;

    if(withEnd)
    {
        // The line through open fluid only: it stops before the first closed sub-cell it would enter.
        const float3 delta = source.velocity * (min(speed * parameters.lengthScale, parameters.maximumLength) / speed);
        const float3 displacement = delta * field.inverseCellSize;
        // A whole line ends at start + delta exactly, in world space, so it is the same in every window that holds it; a stopped one
        // ends where the traversal stopped. Lines through whole cells only (most of them) skip the sub-cell traversal.
        result.end = result.start + delta;

        if(!AirVectorLineOpen(path, source.position, displacement))
        {
            const AirPathEnd end = AirTraverse(path, source.position, source.cell, source.piece, displacement, false, source.cell,
                AirVectorMaximumCrossings);
            result.end = end.stopped ? origin + end.position * field.cellSize : result.end;
        }
    }

    return result;
}

#endif
