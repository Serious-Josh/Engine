// AIR.4D D2: the fluid-piece geometry the solver works in, shared by the step and motor visibility (AirSolver.hlsl). See
// Documentation/Air/AirArchitecture.md, AIR.4D D2.
//
// Positions are in cell units relative to the window's minimum (cell c spans [c - minimum, c - minimum + 1)), so precision does not
// depend on how far the window is from the world origin. A sub-cell is a quarter cell (Air.hlsli).
//
// A cell outside the window is open air at the ambient velocity with no pieces. A cell inside it whose obstacle bit is clear is one
// whole piece (piece 0 is every sub-cell). Otherwise its piece masks say which sub-cells hold piece 0 and piece 1; any other
// sub-cell (solid, or a dropped further piece) is closed.

#ifndef AIR_PATH_HLSLI
#define AIR_PATH_HLSLI

#include "Air.hlsli"

static const uint AirNoPiece = 2;
// How far a clamped point is pulled inside a region's upper bound, relative to the bound (at least 1 cell): about two units in the
// last place, so the clamp only ever undoes rounding.
static const float AirRegionInset = 1.0 / 4194304.0;
// Faces a path crosses within this t of each other are crossed together, through their shared edge or corner (AirTraverse).
static const float AirTieEpsilon = 1.0 / 65536.0;

// AIR.4D D3: the buffers' views. The air kernels define AIR_PATH_WRITABLE as 1 before including this file: during the air work the
// buffers are in the UAV state and the indices are UAV descriptors. Every other consumer (after the air work, buffers in a shader-read
// state, the published binding's SRV indices) uses read-only views. Chosen at compile time, so the solver's code is unchanged.
#ifndef AIR_PATH_WRITABLE
#define AIR_PATH_WRITABLE 0
#endif

#if AIR_PATH_WRITABLE
#define AirPathBuffer RWByteAddressBuffer
#else
#define AirPathBuffer ByteAddressBuffer
#endif

// The air buffers a path or sample reads: the obstacle mask and piece masks, and the velocity of piece 0 and piece 1 (AIR.4D D3:
// AirSample.hlsli), with the ambient velocity outside the window.
struct AirPathField
{
    AirFieldBinding field;
    uint obstacleIndex;
    uint pieceIndex;
    uint velocityIndex;
    uint velocityPiece1Index;
    float3 ambient;
};

struct AirCell
{
    bool inside;
    uint64_t piece0;
    uint64_t piece1;
};

AirCell AirLoadCell(AirPathField path, int3 cell)
{
    AirCell result;
    result.inside = AirContains(path.field, cell);
    result.piece0 = 0;
    result.piece1 = 0;

    if(!result.inside)
    {
        return result;
    }

    const uint3 slot = AirSlotOf(path.field, cell);
    AirPathBuffer obstacles = ResourceDescriptorHeap[NonUniformResourceIndex(path.obstacleIndex)];
    const uint bits = (obstacles.Load(AirObstacleWord(path.field, slot) * 4) >> ((slot.x % AirSlotsPerObstacleWord) * 2)) & 3u;

    // CSBuildPieces settles a cell with no blocked voxel as one whole piece, using the query that clears its blocked bit, so only
    // flagged cells need their masks.
    if((bits & AirObstacleBlocked) == 0)
    {
        result.piece0 = AirSubcellsAll;
        return result;
    }

    AirPathBuffer pieces = ResourceDescriptorHeap[NonUniformResourceIndex(path.pieceIndex)];
    const uint4 words = pieces.Load4(AirVelocityAddress(path.field, slot));
    result.piece0 = uint64_t(words.x) | (uint64_t(words.y) << 32);
    result.piece1 = uint64_t(words.z) | (uint64_t(words.w) << 32);
    return result;
}

// Outside the window, or one whole piece: nothing inside the cell can stop a path or separate corners.
bool AirCellOpen(AirCell cell)
{
    return !cell.inside || cell.piece0 == AirSubcellsAll;
}

// AirCellOpen from the obstacle bit alone, without the cell's masks.
bool AirCellIsOpen(AirPathField path, int3 cell)
{
    if(!AirContains(path.field, cell))
    {
        return true;
    }

    const uint3 slot = AirSlotOf(path.field, cell);
    AirPathBuffer obstacles = ResourceDescriptorHeap[NonUniformResourceIndex(path.obstacleIndex)];
    return ((obstacles.Load(AirObstacleWord(path.field, slot) * 4) >> ((slot.x % AirSlotsPerObstacleWord) * 2)) & AirObstacleBlocked) == 0;
}

uint64_t AirPieceMask(AirCell cell, uint piece)
{
    return piece == 0 ? cell.piece0 : (piece == 1 ? cell.piece1 : 0);
}

uint AirSubcellBit(int3 subcell)
{
    return uint(subcell.x + 4 * subcell.y + 16 * subcell.z);
}

int3 AirSubcellOfBit(uint bit)
{
    return int3(bit & 3u, (bit >> 2) & 3u, bit >> 4);
}

uint AirPieceOfSubcell(AirCell cell, int3 subcell)
{
    const uint64_t flag = uint64_t(1) << AirSubcellBit(subcell);

    if((cell.piece0 & flag) != 0)
    {
        return 0;
    }

    return (cell.piece1 & flag) != 0 ? 1u : AirNoPiece;
}

// Where a piece's velocity lives, in cell units within its cell. A whole cell: its centre, exactly as before D2. A partial piece:
// the centre of its sub-cell nearest the piece's centroid (the lowest such bit on a tie), so the anchor always lies in the piece.
float3 AirPieceAnchor(uint64_t piece)
{
    if(piece == AirSubcellsAll)
    {
        return float3(0.5, 0.5, 0.5);
    }

    // Integer sums of sub-cell coordinates per axis, plane by plane.
    const int count = int(AirSubcellCount(piece));
    int3 sum = int3(0, 0, 0);

    for(uint plane = 1; plane < AirSubcellsPerAxis; ++plane)
    {
        sum.x += int(plane) * int(AirSubcellCount(piece & (AirSubcellsLowX << plane)));
        sum.y += int(plane) * int(AirSubcellCount(piece & (AirSubcellsLowY << (4 * plane))));
        sum.z += int(plane) * int(AirSubcellCount(piece & (AirSubcellsLowZ << (16 * plane))));
    }

    // Distance to the centroid scaled by the count, so it stays exact in integers: count * position - sum.
    uint best = 0;
    int bestDistance = 0x7FFFFFFF;
    uint64_t remaining = piece;

    while(remaining != 0)
    {
        const uint bit = AirLowestSubcell(remaining);
        remaining &= remaining - 1;
        const int3 offset = count * AirSubcellOfBit(bit) - sum;
        const int distance = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;

        if(distance < bestDistance)
        {
            bestDistance = distance;
            best = bit;
        }
    }

    return (float3(AirSubcellOfBit(best)) + 0.5) * 0.25;
}

// The sub-cells on the face of a cell towards `direction` (+1 or -1) along `axis`.
uint64_t AirFaceSubcells(uint axis, int direction)
{
    if(axis == 0)
    {
        return direction > 0 ? AirSubcellsHighX : AirSubcellsLowX;
    }

    if(axis == 1)
    {
        return direction > 0 ? AirSubcellsHighY : AirSubcellsLowY;
    }

    return direction > 0 ? AirSubcellsHighZ : AirSubcellsLowZ;
}

// Whether `piece` (a mask of `cell`'s sub-cells) shares at least one open sub-face with the neighbour across its face towards
// `direction` along `axis`. The window's edge is open (ambient air lies beyond it).
bool AirFaceOpen(AirPathField path, int3 cell, uint64_t piece, uint axis, int direction)
{
    int3 next = cell;
    next[axis] += direction;
    const AirCell neighbour = AirLoadCell(path, next);

    if(!neighbour.inside)
    {
        return true;
    }

    const uint64_t face = piece & AirFaceSubcells(axis, direction);
    // The distance between a sub-cell on one face and its partner on the opposite face: 3 sub-cells along the axis.
    const uint shift = axis == 0 ? 3u : (axis == 1 ? 12u : 48u);
    const uint64_t across = direction > 0 ? face >> shift : face << shift;
    return (across & (neighbour.piece0 | neighbour.piece1)) != 0;
}

struct AirPathEnd
{
    // Where the path ended, in cell units from the minimum, inside the region (cell, and sub-cell in a split cell) that holds it.
    float3 position;
    // A closed sub-cell (solid or a dropped piece) ended the path early.
    bool stopped;
    // AirTraverse with a target: the path entered the target cell.
    bool reached;
    int3 cell;
    bool inside;
    uint piece;
};

// `position` clamped into the box [low, low + size) if it lies outside it (a rounding disagreement with the traversal, or a stop on
// the box's face), so that floor() of the result names the region the traversal tracked.
float3 AirClampIntoRegion(float3 position, float3 low, float size)
{
    const float3 high = low + size;

    if(all(position >= low) && all(position < high))
    {
        return position;
    }

    return clamp(position, low, high - max(high, 1.0) * AirRegionInset);
}

// Follows the straight path start -> start + displacement through open fluid, one sub-cell face at a time, and stops before the
// first closed sub-cell (solid, or a dropped piece). Cells outside the window and cells of one whole piece are open throughout, so
// they cost arithmetic only; piece masks are loaded only when the path enters a cell. Moving between two open sub-cells never needs a
// link check: they share an open sub-face.
//
// Faces crossed within AirTieEpsilon of each other are crossed together, through the edge or corner they share, and the path goes on
// only if every sub-cell around it that the path could touch is open. So a path never slips between two sub-cells that meet only
// along an edge (they may be different pieces), and rounding cannot change which way a path through an edge goes.
//
// With `hasTarget`, it returns as soon as the path enters `target` (the piece it enters by is `piece`). A path needing more than
// `maximumCrossings` cell crossings is treated as stopped there, so the work is bounded.
AirPathEnd AirTraverse(AirPathField path, float3 start, int3 startCell, uint startPiece, float3 displacement, bool hasTarget, int3 target,
    uint maximumCrossings)
{
    const int3 minimum = path.field.minimum;
    const int3 direction = int3(sign(displacement));
    const int3 upper = int3(direction > 0);
    // The path's cell, and its sub-cell, both relative to the window minimum (sub-cells in quarter cells).
    int3 local = startCell - minimum;
    int3 subcell = clamp(int3(floor(start * 4.0)), local * 4, local * 4 + 3);
    AirCell cell = AirLoadCell(path, startCell);
    uint piece = startPiece;
    uint crossings = 0;
    float stopT = 1.0;

    AirPathEnd end;
    end.stopped = false;
    end.reached = false;
    // A path crosses at most 9 faces inside a cell before it leaves it, so this bound is never reached by a valid path; it only keeps
    // the work bounded whatever the input.
    const uint maximumIterations = (maximumCrossings + 1) * 12;

    for(uint iteration = 0;; ++iteration)
    {
        if(iteration >= maximumIterations)
        {
            end.stopped = true;
            stopT = 0.0;
            break;
        }

        // The t of the next sub-cell face per axis (2: never, beyond the end at t = 1).
        float3 next = float3(2.0, 2.0, 2.0);

        for(uint axis = 0; axis < 3; ++axis)
        {
            if(direction[axis] != 0)
            {
                next[axis] = (float(subcell[axis] + upper[axis]) * 0.25 - start[axis]) / displacement[axis];
            }
        }

        const float nearest = min(min(next.x, next.y), next.z);

        if(nearest >= 1.0)
        {
            break;
        }

        const int3 tied = int3(next <= nearest + AirTieEpsilon);
        const int3 entered = subcell + tied * direction;
        const int3 enteredLocal = entered >> 2;
        const bool crossesCell = any(enteredLocal != local);

        if(crossesCell && crossings >= maximumCrossings)
        {
            end.stopped = true;
            stopT = nearest;
            break;
        }

        // Every sub-cell reached by part of the tied steps, and the one reached by all of them, must be open.
        bool open = true;
        AirCell enteredCell = cell;

        for(uint combination = 1; combination < 8 && open; ++combination)
        {
            const int3 partial = int3(combination & 1u, (combination >> 1) & 1u, combination >> 2);

            if(any(partial > tied))
            {
                continue;
            }

            const int3 touched = subcell + partial * direction;
            const int3 touchedLocal = touched >> 2;
            AirCell holder = cell;

            if(any(touchedLocal != local))
            {
                holder = AirLoadCell(path, minimum + touchedLocal);
            }

            open = !holder.inside || AirPieceOfSubcell(holder, touched - touchedLocal * 4) != AirNoPiece;

            if(all(partial == tied))
            {
                enteredCell = holder;
            }
        }

        if(!open)
        {
            end.stopped = true;
            stopT = nearest;
            break;
        }

        crossings += crossesCell ? 1u : 0u;
        local = enteredLocal;
        subcell = entered;
        cell = enteredCell;
        piece = cell.inside ? AirPieceOfSubcell(cell, subcell - local * 4) : 0u;

        if(hasTarget && crossesCell && all(minimum + local == target))
        {
            end.reached = true;
            stopT = nearest;
            break;
        }
    }

    const float3 position = start + displacement * stopT;
    end.position = AirCellOpen(cell) ? AirClampIntoRegion(position, float3(local), 1.0) :
        AirClampIntoRegion(position, float3(subcell) * 0.25, 0.25);
    end.cell = minimum + local;
    end.inside = cell.inside;
    end.piece = piece;
    return end;
}

#endif
