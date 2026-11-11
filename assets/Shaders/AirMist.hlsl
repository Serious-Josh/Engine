// AIR.4F F1: passive mist transport. One step per air solver step, after it: every fluid piece's mist is carried by the latest
// velocity, by the solver's own geometry rules, then dissipated. Mist never feeds back into the air. See
// Documentation/Air/AirArchitecture.md, AIR.4F.
//
// Per piece, from its anchor (AirPieceAnchor, where the solver keeps the piece's velocity):
//   1. backtrace along -v dt (the piece's latest velocity), at most 8 cells, through open fluid only (AirBacktrace: an exact
//      sub-cell traversal that stops before the first closed sub-cell, however many cells the path crosses);
//   2. sample the mist there by D2's corner rules (AirMistSample.hlsli): one piece per corner, never two pieces of one cell;
//      corners outside the window are clear air; with no corner left, the piece keeps its own mist;
//   3. dissipation: all four premultiplied components scale by dissipationFactor = exp(-rate * dt).
// A cell without pieces, and an absent piece 1, are written as zero.
//
// Two passes, as the solver's step, for the same reason (the split-cell sample's register use would slow every thread):
//   - CSMistOpen settles each cell that is one whole piece whose backtrace passes AirOpenBacktrace and whose 8 sample corners are
//     open. For such a cell the full path computes exactly this (AirBacktrace returns start + displacement; AirSampleMist takes
//     the open sample), so the lean pass and the full path agree. Every other cell is listed. The classification is mist's own:
//     it uses the velocity after the solver's step, which the solver's list was not built from.
//   - CSMistPrepare turns the count into CSMistSplit's dispatch and zeroes the counter;
//   - CSMistSplit runs the full path for the listed cells.
// The work list and counter buffers are the solver's (AirField): the mist step runs after the solver's step has finished with
// them (UAV barriers between), and CSMistPrepare leaves the counter zeroed as CSPrepareSplit does.
//
// AIR.4F F2 emission (CSMistEmit), after each step's transport, into the step's new mist: one dispatch per emitter over the cells
// of its bounding box, in emitter order with a UAV barrier between, so overlapping emitters add in a fixed order. Per fluid piece of
// a cell (MistEmitterComponent.h):
//   - nothing if the origin is outside the window or in a sub-cell no piece holds (solid voxel geometry is never skipped);
//   - c: the piece's sub-cells whose centres lie in the shape, plus the origin's own sub-cell (so a shape smaller than a sub-cell
//     still emits); nothing if none;
//   - the piece must be seen from the origin: the same piece in the origin's cell, or a straight path through open fluid
//     (AirTraverse, the backtrace's rules) from the origin to its sub-cell of c nearest the origin (lowest bit on a tie);
//   - then mist += (colour * a, a) with a = densityPerStep * c / n, n the piece's sub-cells.
// The straight path is conservative: air around a corner from the origin receives nothing, though it is connected.

#define AIR_PATH_WRITABLE 1
#include "AirPath.hlsli"
#include "AirMistSample.hlsli"

cbuffer AirMistConstants : register(b0)
{
    AirFieldBinding field;
    uint obstacleIndex;
    uint pieceIndex;
    // The latest velocity of each piece (after the solver's step).
    uint velocityIndex;
    uint velocityPiece1Index;
    // The mist pairs: read the current buffer of each, write the other.
    uint mistRead0;
    uint mistRead1;
    uint mistWrite0;
    uint mistWrite1;
    float timeStep;
    // exp(-dissipationRate * timeStep)
    float dissipationFactor;
    uint splitListIndex;
    uint splitArgumentsIndex;
    // CSMistClear: all four mist buffers.
    uint clear0;
    uint clear1;
    uint clear2;
    uint clear3;
    // AIR.4F F2: the emitter of a CSMistEmit dispatch, the frame's emitter count, whether this step counts receiving pieces into the
    // status (the frame's first step), and the writable status buffer (one uint4 per emitter).
    uint emitterIndex;
    uint emitterCount;
    uint countPieces;
    uint statusIndex;
    // CSMistEmit: the cells it covers, from emitBoxMinimum, emitBoxSize along each axis (within the window).
    int3 emitBoxMinimum;
    uint emitPadding0;
    uint3 emitBoxSize;
    uint emitPadding1;
};

// Matches GpuMistEmitter (AirField.cpp) and MistEmitterShape (MistEmitterComponent.h).
struct MistEmitter
{
    float3 position;
    uint shape;
    float3 axisX;
    float radius;
    float3 axisY;
    // Density added per step where the shape covers a whole piece: rate * density multiplier * dt.
    float densityPerStep;
    float3 axisZ;
    // Cell crossings a straight path inside the emitter's bounding sphere can need.
    uint maximumCrossings;
    float3 halfExtents;
    uint padding0;
    float3 color;
    float padding1;
};

StructuredBuffer<MistEmitter> emitters : register(t0);

static const uint EmitterSphere = 0;
// Matches MistEmitterStatus::Origin.
static const uint OriginOpen = 0;
static const uint OriginSolid = 1;
static const uint OriginUnknown = 2;
static const uint OriginOutside = 3;

AirPathField Path()
{
    AirPathField path;
    path.field = field;
    path.obstacleIndex = obstacleIndex;
    path.pieceIndex = pieceIndex;
    path.velocityIndex = velocityIndex;
    path.velocityPiece1Index = velocityPiece1Index;
    // Not read: mist samples no velocity outside the window.
    path.ambient = float3(0.0, 0.0, 0.0);
    return path;
}

AirMistField ReadMist()
{
    AirMistField mist;
    mist.piece0Index = mistRead0;
    mist.piece1Index = mistRead1;
    return mist;
}

// Steps 1 to 3 for one piece, the full path.
float4 StepPiece(int3 cell, uint64_t mask, uint piece)
{
    const float3 anchor = AirPieceAnchor(mask);
    const float3 start = float3(cell - field.minimum) + anchor;
    const float3 velocity = AirPieceVelocity(Path(), cell, piece);
    const float3 displacement = AirBacktraceDisplacement(velocity, timeStep, field.inverseCellSize);

    const AirPathEnd from = AirBacktrace(Path(), cell, mask, piece, start, displacement);
    const float4 own = AirPieceMist(Path(), ReadMist(), cell, piece);
    return AirSampleMist(Path(), ReadMist(), from, own) * dissipationFactor;
}

void StoreMist(uint3 slot, float4 mist0, float4 mist1)
{
    RWByteAddressBuffer output0 = ResourceDescriptorHeap[NonUniformResourceIndex(mistWrite0)];
    RWByteAddressBuffer output1 = ResourceDescriptorHeap[NonUniformResourceIndex(mistWrite1)];
    const uint address = AirVelocityAddress(field, slot);
    output0.Store4(address, asuint(mist0));
    output1.Store4(address, asuint(mist1));
}

[numthreads(8, 8, 8)]
void CSMistOpen(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const int3 cell = AirCellOfSlot(field, slot);

    if(AirCellIsOpen(Path(), cell))
    {
        const float3 start = float3(cell - field.minimum) + 0.5;
        const float3 velocity = AirPieceVelocity(Path(), cell, 0);
        const float3 end = start + AirBacktraceDisplacement(velocity, timeStep, field.inverseCellSize);
        const float3 shifted = end - 0.5;
        const int3 base = int3(floor(shifted));

        if(AirOpenBacktrace(Path(), start, end) && AirCornersOpen(Path(), base))
        {
            const float4 own = AirPieceMist(Path(), ReadMist(), cell, 0);
            const float4 mist = AirSampleMistOpen(Path(), ReadMist(), base, shifted - float3(base), own) * dissipationFactor;
            StoreMist(slot, mist, float4(0.0, 0.0, 0.0, 0.0));
            return;
        }
    }

    RWByteAddressBuffer arguments = ResourceDescriptorHeap[NonUniformResourceIndex(splitArgumentsIndex)];
    RWByteAddressBuffer list = ResourceDescriptorHeap[NonUniformResourceIndex(splitListIndex)];
    uint entry = 0;
    arguments.InterlockedAdd(0, 1, entry);
    list.Store((entry + 1) * 4, slot.x + field.dimensions.x * (slot.y + field.dimensions.y * slot.z));
}

// Groups of CSMistSplit along x per row of the dispatch (within D3D12's 65535), as the solver's split pass.
static const uint SplitGroupsPerRow = 32768;
static const uint SplitThreads = 64;

[numthreads(1, 1, 1)]
void CSMistPrepare()
{
    RWByteAddressBuffer arguments = ResourceDescriptorHeap[NonUniformResourceIndex(splitArgumentsIndex)];
    RWByteAddressBuffer list = ResourceDescriptorHeap[NonUniformResourceIndex(splitListIndex)];
    const uint count = arguments.Load(0);
    const uint groups = (count + SplitThreads - 1) / SplitThreads;
    list.Store(0, count);
    arguments.Store3(4, uint3(min(groups, SplitGroupsPerRow), (groups + SplitGroupsPerRow - 1) / SplitGroupsPerRow, 1));
    arguments.Store(0, 0);
}

[numthreads(64, 1, 1)]
void CSMistSplit(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    RWByteAddressBuffer list = ResourceDescriptorHeap[NonUniformResourceIndex(splitListIndex)];
    const uint entry = (group.y * SplitGroupsPerRow + group.x) * SplitThreads + thread.x;

    if(entry >= list.Load(0))
    {
        return;
    }

    const uint index = list.Load((entry + 1) * 4);
    const uint3 slot = uint3(index % field.dimensions.x, (index / field.dimensions.x) % field.dimensions.y,
        index / (field.dimensions.x * field.dimensions.y));
    const int3 cell = AirCellOfSlot(field, slot);
    const AirCell own = AirLoadCell(Path(), cell);

    const float4 mist0 = own.piece0 != 0 ? StepPiece(cell, own.piece0, 0) : float4(0.0, 0.0, 0.0, 0.0);
    const float4 mist1 = own.piece1 != 0 ? StepPiece(cell, own.piece1, 1) : float4(0.0, 0.0, 0.0, 0.0);
    StoreMist(slot, mist0, mist1);
}

// AIR.4F F2: where an emitter's origin lies: its state, and in fluid its position (cell units from the window minimum), cell, piece
// and sub-cell bit.
struct EmitterOrigin
{
    uint state;
    float3 position;
    int3 cell;
    uint piece;
    uint bit;
};

EmitterOrigin OriginOf(MistEmitter emitter)
{
    EmitterOrigin origin;
    origin.position = emitter.position * field.inverseCellSize - float3(field.minimum);
    origin.cell = field.minimum + int3(floor(origin.position));
    origin.piece = AirNoPiece;
    origin.bit = 0;
    const AirCell holder = AirLoadCell(Path(), origin.cell);

    if(!holder.inside)
    {
        origin.state = OriginOutside;
        return origin;
    }

    const int3 subcell = clamp(int3(floor(origin.position * 4.0)) - (origin.cell - field.minimum) * 4, 0, 3);
    origin.bit = uint(subcell.x + 4 * subcell.y + 16 * subcell.z);
    origin.piece = AirPieceOfSubcell(holder, subcell);

    if(origin.piece == AirNoPiece)
    {
        origin.state = OriginSolid;
        return origin;
    }

    origin.state = AirPathCellKnown(Path(), origin.cell) ? OriginOpen : OriginUnknown;
    return origin;
}

bool InsideShape(MistEmitter emitter, float3 world)
{
    const float3 offset = world - emitter.position;

    if(emitter.shape == EmitterSphere)
    {
        return dot(offset, offset) <= emitter.radius * emitter.radius;
    }

    const float3 local = float3(dot(offset, emitter.axisX), dot(offset, emitter.axisY), dot(offset, emitter.axisZ));
    return all(abs(local) <= emitter.halfExtents);
}

// One thread per emitter: its origin's state, and a zero count of receiving pieces for CSMistEmit to add to.
[numthreads(32, 1, 1)]
void CSMistEmitterStatus(uint3 thread : SV_DispatchThreadID)
{
    if(thread.x >= emitterCount)
    {
        return;
    }

    RWByteAddressBuffer status = ResourceDescriptorHeap[NonUniformResourceIndex(statusIndex)];
    status.Store4(thread.x * 16, uint4(OriginOf(emitters[thread.x]).state, 0, 0, 0));
}

// One thread per cell of the emitter's box (see the header).
[numthreads(4, 4, 4)]
void CSMistEmit(uint3 thread : SV_DispatchThreadID)
{
    if(any(thread >= emitBoxSize))
    {
        return;
    }

    const int3 cell = emitBoxMinimum + int3(thread);
    const MistEmitter emitter = emitters[emitterIndex];
    const EmitterOrigin origin = OriginOf(emitter);

    if(!AirContains(field, cell) || (origin.state != OriginOpen && origin.state != OriginUnknown))
    {
        return;
    }

    const AirCell own = AirLoadCell(Path(), cell);
    const int3 local = cell - field.minimum;
    const bool originCell = all(cell == origin.cell);
    const uint address = AirVelocityAddress(field, AirSlotOf(field, cell));

    for(uint piece = 0; piece < 2; ++piece)
    {
        const uint64_t mask = AirPieceMask(own, piece);

        if(mask == 0)
        {
            continue;
        }

        // The piece's sub-cells in the shape, and the one nearest the origin.
        uint inside = 0;
        uint nearest = 0;
        float nearestDistance = 1e30;
        uint64_t remaining = mask;

        while(remaining != 0)
        {
            const uint bit = AirLowestSubcell(remaining);
            remaining &= remaining - 1;
            const float3 centre = float3(local) + (float3(AirSubcellOfBit(bit)) + 0.5) * 0.25;
            const bool isOrigin = originCell && bit == origin.bit;

            if(!isOrigin && !InsideShape(emitter, (centre + float3(field.minimum)) * field.cellSize))
            {
                continue;
            }

            ++inside;
            const float3 offset = centre - origin.position;
            const float distance = dot(offset, offset);

            if(distance < nearestDistance)
            {
                nearestDistance = distance;
                nearest = bit;
            }
        }

        if(inside == 0)
        {
            continue;
        }

        bool visible = piece == origin.piece;

        if(!originCell)
        {
            const float3 target = float3(local) + (float3(AirSubcellOfBit(nearest)) + 0.5) * 0.25;
            const AirPathEnd path = AirTraverse(Path(), origin.position, origin.cell, origin.piece, target - origin.position, true, cell,
                emitter.maximumCrossings);
            visible = path.reached && path.piece == piece;
        }

        if(!visible)
        {
            continue;
        }

        const float added = emitter.densityPerStep * float(inside) / float(AirSubcellCount(mask));
        RWByteAddressBuffer mist = ResourceDescriptorHeap[NonUniformResourceIndex(piece == 0 ? mistWrite0 : mistWrite1)];
        const float4 value = asfloat(mist.Load4(address)) + float4(emitter.color * added, added);
        mist.Store4(address, asuint(value));

        if(countPieces != 0)
        {
            RWByteAddressBuffer status = ResourceDescriptorHeap[NonUniformResourceIndex(statusIndex)];
            status.InterlockedAdd(emitterIndex * 16 + 4, 1);
        }
    }
}

// Zeroes every cell of all four mist buffers (new storage, or a clear).
[numthreads(8, 8, 8)]
void CSMistClear(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const uint address = AirVelocityAddress(field, slot);
    const uint buffers[4] = {clear0, clear1, clear2, clear3};

    for(uint index = 0; index < 4; ++index)
    {
        RWByteAddressBuffer mist = ResourceDescriptorHeap[NonUniformResourceIndex(buffers[index])];
        mist.Store4(address, uint4(0, 0, 0, 0));
    }
}
