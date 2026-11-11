// AIR.4D D3: piece-aware velocity sampling, shared by the solver (AirSolver.hlsl, its step 2) and the field's consumers (the
// Editor's vectors, AirVisualization.hlsli). Moved unchanged from the D2 solver; see Documentation/Air/AirArchitecture.md, AIR.4D D2
// and D3.
//
// A sample at a point blends the 8 cells whose centres surround it, one piece of each, never two pieces of one cell:
//   - the piece a straight path from the point enters that cell by;
//   - else the piece reachable through linked pieces of the 8 cells (AirPieceLink), with fewest links, then the nearest sub-cell,
//     then piece 0;
//   - else the corner is left out and the weights renormalized. Corners outside the window read the ambient velocity. With no
//     corner left, the caller's fallback (normally the point's own piece's velocity) is returned.
// Where all 8 corners are whole cells (or outside the window) this is plain trilinear.
//
// The buffers are read through AirPathField (AirPath.hlsli): writable views in the solver, read-only views elsewhere.

#ifndef AIR_SAMPLE_HLSLI
#define AIR_SAMPLE_HLSLI

#include "AirPath.hlsli"

// A corner's straight path from the sample point crosses at most one cell face per axis.
static const uint AirMaximumCornerCrossings = 4;

// A piece's latest velocity; outside the window, the ambient.
float3 AirPieceVelocity(AirPathField path, int3 cell, uint piece)
{
    if(!AirContains(path.field, cell))
    {
        return path.ambient;
    }

    AirPathBuffer velocity = ResourceDescriptorHeap[NonUniformResourceIndex(piece == 0 ? path.velocityIndex : path.velocityPiece1Index)];
    return asfloat(velocity.Load3(AirVelocityAddress(path.field, AirSlotOf(path.field, cell))));
}

// The squared distance from `position` to the nearest sub-cell centre of `piece` in the cell at `local`.
float AirNearestSubcellDistance(float3 position, int3 local, uint64_t piece)
{
    float nearest = 1e30;
    uint64_t remaining = piece;

    while(remaining != 0)
    {
        const uint bit = AirLowestSubcell(remaining);
        remaining &= remaining - 1;
        const float3 offset = position - (float3(local) + (float3(AirSubcellOfBit(bit)) + 0.5) * 0.25);
        nearest = min(nearest, dot(offset, offset));
    }

    return nearest;
}

// Links from the sample's own piece through the 8 corner cells: for each node (corner * 2 + piece), the fewest links to reach it
// (255: unreachable). Corners outside the window are not traversed.
void AirSearchCorners(uint homeCorner, uint homePiece, AirCell corners[8], out uint hops[16])
{
    for(uint node = 0; node < 16; ++node)
    {
        hops[node] = 255;
    }

    if(homeCorner >= 8)
    {
        return;
    }

    uint reached = 1u << (homeCorner * 2 + homePiece);
    uint frontier = reached;
    hops[homeCorner * 2 + homePiece] = 0;

    for(uint layer = 1; frontier != 0 && layer < 16; ++layer)
    {
        uint grown = 0;

        for(uint node = 0; node < 16; ++node)
        {
            if(((frontier >> node) & 1u) == 0)
            {
                continue;
            }

            const uint corner = node >> 1;
            const uint64_t piece = AirPieceMask(corners[corner], node & 1u);

            for(uint axis = 0; axis < 3; ++axis)
            {
                const uint other = corner ^ (1u << axis);
                // The corner whose offset along `axis` is 0 is the lower cell of the pair.
                const bool lower = ((corner >> axis) & 1u) == 0;

                for(uint otherPiece = 0; otherPiece < 2 && corners[other].inside; ++otherPiece)
                {
                    const uint target = other * 2 + otherPiece;
                    const uint64_t neighbour = AirPieceMask(corners[other], otherPiece);

                    if(((reached >> target) & 1u) != 0 || neighbour == 0)
                    {
                        continue;
                    }

                    const uint area = lower ? AirPieceLink(piece, neighbour, axis) : AirPieceLink(neighbour, piece, axis);
                    grown |= area > 0 ? 1u << target : 0u;
                }
            }
        }

        for(uint node = 0; node < 16; ++node)
        {
            if(((grown >> node) & 1u) != 0)
            {
                hops[node] = layer;
            }
        }

        reached |= grown;
        frontier = grown;
    }
}

// The piece of corner `corner` (cell `cell`) the sample at `at` blends (see the header). AirNoPiece leaves the corner out.
uint AirCornerPiece(AirPathField path, AirPathEnd at, uint corner, int3 cell, AirCell corners[8], uint homeCorner, inout bool searched,
    inout uint hops[16])
{
    if(corner == homeCorner)
    {
        return at.piece;
    }

    // The straight path from the point towards the corner's centre, through open fluid.
    const float3 centre = float3(cell - path.field.minimum) + 0.5;
    const AirPathEnd straight = AirTraverse(path, at.position, at.cell, at.piece, centre - at.position, true, cell,
        AirMaximumCornerCrossings);

    if(straight.reached)
    {
        return straight.piece;
    }

    if(!searched)
    {
        AirSearchCorners(homeCorner, at.piece, corners, hops);
        searched = true;
    }

    const uint hops0 = hops[corner * 2];
    const uint hops1 = hops[corner * 2 + 1];

    if(hops0 == 255 && hops1 == 255)
    {
        return AirNoPiece;
    }

    if(hops0 != hops1)
    {
        return hops0 < hops1 ? 0u : 1u;
    }

    const int3 local = cell - path.field.minimum;
    const float distance0 = AirNearestSubcellDistance(at.position, local, corners[corner].piece0);
    const float distance1 = AirNearestSubcellDistance(at.position, local, corners[corner].piece1);
    return distance1 < distance0 ? 1u : 0u;
}

// AIR.4F: the corners a sample beside a split cell blends, shared by every field sampled with D2's rules (velocity below, mist in
// AirMistSample.hlsli). Per corner: its trilinear weight and the piece chosen as the header describes, AirNoPiece leaving it out. A
// corner of zero weight is never resolved (AirNoPiece, weight 0); a corner outside the window is piece 0, which the caller reads as
// its outside value.
struct AirCornerChoice
{
    uint piece[8];
    float weight[8];
};

AirCornerChoice AirChooseCorners(AirPathField path, AirPathEnd at, int3 base, float3 fraction)
{
    AirCell corners[8];

    for(uint corner = 0; corner < 8; ++corner)
    {
        const int3 offset = int3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
        corners[corner] = AirLoadCell(path, path.field.minimum + base + offset);
    }

    // The point's own cell is one of the corners (or the point lies outside the window: 8).
    const int3 home = at.cell - path.field.minimum - base;
    const uint homeCorner = at.inside ? uint(home.x + 2 * home.y + 4 * home.z) : 8u;
    bool searched = false;
    uint hops[16];
    AirCornerChoice choice;

    for(uint corner = 0; corner < 8; ++corner)
    {
        const int3 offset = int3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
        const int3 cell = path.field.minimum + base + offset;
        const float3 axisWeight = lerp(1.0 - fraction, fraction, float3(offset));
        const float weight = axisWeight.x * axisWeight.y * axisWeight.z;
        choice.piece[corner] = AirNoPiece;
        choice.weight[corner] = 0.0;

        if(weight <= 0.0)
        {
            continue;
        }

        choice.piece[corner] = !corners[corner].inside ? 0u : AirCornerPiece(path, at, corner, cell, corners, homeCorner, searched, hops);
        choice.weight[corner] = weight;
    }

    return choice;
}

// Step 2 beside a split cell: each corner's piece chosen as step 2 describes. Kept apart from the open case, whose sample is plain
// trilinear, so that its per-corner masks and link search cost nothing where there are no obstacles.
float3 AirSampleSplit(AirPathField path, AirPathEnd at, int3 base, float3 fraction, float3 fallback)
{
    const AirCornerChoice choice = AirChooseCorners(path, at, base, fraction);
    float3 sum = 0.0;
    float total = 0.0;

    for(uint corner = 0; corner < 8; ++corner)
    {
        const int3 offset = int3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);

        // The outside reads the ambient velocity (AirPieceVelocity).
        if(choice.piece[corner] != AirNoPiece)
        {
            sum += AirPieceVelocity(path, path.field.minimum + base + offset, choice.piece[corner]) * choice.weight[corner];
            total += choice.weight[corner];
        }
    }

    return total > 0.0 ? sum / total : fallback;
}

// Whether all 8 corners of the sample at `base` are open (whole cells, or outside the window).
bool AirCornersOpen(AirPathField path, int3 base)
{
    bool open = true;

    for(uint corner = 0; corner < 8; ++corner)
    {
        const int3 offset = int3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
        open = open && AirCellIsOpen(path, path.field.minimum + base + offset);
    }

    return open;
}

// Step 2 where every corner is open (whole cells and the outside, the ambient): plain trilinear, exactly as before D2.
float3 AirSampleOpen(AirPathField path, int3 base, float3 fraction, float3 fallback)
{
    float3 sum = 0.0;
    float total = 0.0;

    for(uint corner = 0; corner < 8; ++corner)
    {
        const int3 offset = int3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
        const int3 cell = path.field.minimum + base + offset;
        const float3 axisWeight = lerp(1.0 - fraction, fraction, float3(offset));
        const float weight = axisWeight.x * axisWeight.y * axisWeight.z;

        if(weight > 0.0)
        {
            sum += AirPieceVelocity(path, cell, 0) * weight;
            total += weight;
        }
    }

    return total > 0.0 ? sum / total : fallback;
}

float3 AirSampleVelocity(AirPathField path, AirPathEnd at, float3 fallback)
{
    // Trilinear over the cells whose centres surround the point.
    const float3 shifted = at.position - 0.5;
    const int3 base = int3(floor(shifted));
    const float3 fraction = shifted - float3(base);

    if(!AirCornersOpen(path, base))
    {
        return AirSampleSplit(path, at, base, fraction, fallback);
    }

    return AirSampleOpen(path, base, fraction, fallback);
}

// AIR.4F: the solver's step-1 backtrace, shared with mist transport (AirMist.hlsl). Moved unchanged from AirSolver.hlsl.

// The longest backtrace, in cells; faster air is traced this far along its direction.
static const float AirMaximumBacktraceCells = 8.0;
// Cell crossings a backtrace of 8 cells can need (at most 8 * sqrt(3) + 3), with a margin.
static const uint AirMaximumBacktraceCrossings = 24;

// The backtrace's displacement in cells: -v dt, at most AirMaximumBacktraceCells long.
float3 AirBacktraceDisplacement(float3 velocity, float timeStep, float inverseCellSize)
{
    float3 displacement = -velocity * (timeStep * inverseCellSize);
    const float reach = length(displacement);

    if(reach > AirMaximumBacktraceCells)
    {
        displacement *= AirMaximumBacktraceCells / reach;
    }

    return displacement;
}

// The backtrace's shortcut: a backtrace within 2 cells per axis, all of them open, cannot meet a closed sub-cell (it ends at
// start + displacement, as the traversal would).
bool AirOpenBacktrace(AirPathField path, float3 start, float3 end)
{
    const int3 low = int3(floor(min(start, end)));
    const int3 high = int3(floor(max(start, end)));

    if(any(high - low > 1))
    {
        return false;
    }

    bool open = true;

    for(uint corner = 0; corner < 8; ++corner)
    {
        const int3 offset = min(int3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u), high - low);
        open = open && AirCellIsOpen(path, path.field.minimum + low + offset);
    }

    return open;
}

// A piece's backtrace from `start` (its anchor). A whole cell's open backtrace (AirOpenBacktrace) ends at start + displacement
// without walking its sub-cell faces; everything else is traversed and stops before the first closed sub-cell.
AirPathEnd AirBacktrace(AirPathField path, int3 cell, uint64_t mask, uint piece, float3 start, float3 displacement)
{
    const float3 end = start + displacement;

    if(mask == AirSubcellsAll)
    {
        if(AirOpenBacktrace(path, start, end))
        {
            AirPathEnd result;
            result.position = end;
            result.stopped = false;
            result.reached = false;
            result.cell = path.field.minimum + int3(floor(end));
            result.inside = AirContains(path.field, result.cell);
            result.piece = 0;
            return result;
        }
    }

    return AirTraverse(path, start, cell, piece, displacement, false, cell, AirMaximumBacktraceCrossings);
}

// AIR.4D D3: what is at a world point, and the velocity there.
static const uint AirPointFluid = 0;
// In a sub-cell no piece holds: solid voxel geometry, or a third piece of a cell (D1 keeps two).
static const uint AirPointSolid = 1;
// Outside the window: free air at the ambient velocity.
static const uint AirPointOutside = 2;
// The field is not valid, or the position is not finite.
static const uint AirPointInvalid = 3;

struct AirPointSample
{
    uint state;
    // Fluid in a cell without voxel data (free air to the solver, as everywhere in the air system).
    bool unknown;
    // Fluid: the piece holding the point (0 or 1).
    uint piece;
    // The cell holding the point (any state but invalid).
    int3 cell;
    // Fluid: the piece-aware sample (the D2 rules above, with the piece's own velocity as the fallback). Outside: the ambient.
    // Otherwise zero.
    float3 velocity;
};

// Whether a cell in the window has voxel data (its obstacle bits' known bit), read through the path's views.
bool AirPathCellKnown(AirPathField path, int3 cell)
{
    const uint3 slot = AirSlotOf(path.field, cell);
    AirPathBuffer obstacles = ResourceDescriptorHeap[NonUniformResourceIndex(path.obstacleIndex)];
    const uint bits = (obstacles.Load(AirObstacleWord(path.field, slot) * 4) >> ((slot.x % AirSlotsPerObstacleWord) * 2)) & 3u;
    return (bits & AirObstacleKnown) != 0;
}

// The point's state and, in fluid, the velocity a consumer sees there: the piece holding the point is the piece of the sub-cell it
// lies in, and the velocity is that piece's sample. Never a blend of two pieces of one cell, never a velocity in solid. `position` is
// in cell units from the window minimum (as the solver's paths are); AirSampleAt takes world metres.
AirPointSample AirSampleAtPosition(AirPathField path, float3 position)
{
    AirPointSample result;
    result.state = AirPointInvalid;
    result.unknown = false;
    result.piece = AirNoPiece;
    result.cell = int3(0, 0, 0);
    result.velocity = float3(0.0, 0.0, 0.0);

    if(path.field.valid == 0 || any(!isfinite(position)))
    {
        return result;
    }

    result.cell = path.field.minimum + int3(floor(position));
    const AirCell holder = AirLoadCell(path, result.cell);

    if(!holder.inside)
    {
        result.state = AirPointOutside;
        result.velocity = path.ambient;
        return result;
    }

    const int3 subcell = clamp(int3(floor(position * 4.0)) - (result.cell - path.field.minimum) * 4, 0, 3);
    const uint piece = AirPieceOfSubcell(holder, subcell);

    if(piece == AirNoPiece)
    {
        result.state = AirPointSolid;
        return result;
    }

    AirPathEnd at;
    at.position = position;
    at.stopped = false;
    at.reached = false;
    at.cell = result.cell;
    at.inside = true;
    at.piece = piece;
    result.state = AirPointFluid;
    result.piece = piece;
    result.unknown = !AirPathCellKnown(path, result.cell);
    result.velocity = AirSampleVelocity(path, at, AirPieceVelocity(path, result.cell, piece));
    return result;
}

AirPointSample AirSampleAt(AirPathField path, float3 world)
{
    return AirSampleAtPosition(path, world * path.field.inverseCellSize - float3(path.field.minimum));
}

#endif
