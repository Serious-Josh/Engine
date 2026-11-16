// AIR.4F F1: passive mist transport. One step per air solver step, after it: every fluid piece's mist is carried by the latest
// velocity, by the solver's own geometry rules, then dissipated. Mist never feeds back into the air. See
// Documentation/Air/AirArchitecture.md, AIR.4F.
//
// Per piece, from its anchor (AirPieceAnchor, where the solver keeps the piece's velocity):
//   1. backtrace along -v dt (the piece's latest velocity), at most 8 cells, through open fluid only (AirBacktrace: an exact
//      sub-cell traversal that stops before the first closed sub-cell, however many cells the path crosses);
//   2. sample the mist there by D2's corner rules (AirMistSample.hlsli): one piece per corner, never two pieces of one cell;
//      corners outside the window are clear air; with no corner left, the piece keeps its own mist. AIR.4F.2a: the weights are
//      evaluated at the end moved back by the anchor's offset from the cell centre, so a still partial piece keeps its own mist;
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
//
// AIR.4F.2b Point emission (Documentation/Air/AirPointEmitters4F2b.md): a fixed quantity Q dt of mist per step (density x m^3),
// spread by the kernel k(r) = (1 - r / R)^2 (R the footprint radius, r a sub-cell centre's distance from the origin), quantized to
// 16-bit fixed point so both passes and every thread agree exactly. Two passes per emitter over its footprint's cells, which are not
// clipped to the window:
//   - CSMistPointSum: the step's total weight D = the weights of the visible pieces' sub-cells (visibility as above, tested at the
//     piece's weighted sub-cell nearest the origin) + the weights of every sub-cell centre outside the window, which counts as open
//     air. Integer atomics into the emitter's status words, so the order of threads never changes D. Solid and unseen sub-cells are
//     left out of D: the quantity is renormalized over the air the origin sees. Outside the window it is not: that share is lost.
//   - CSMistPointDeposit: each visible piece p with weight W_p gains density Q dt (W_p / D) / V_p (V_p its fluid volume), so the
//     added quantity sums to Q dt times the share inside the window, wherever the origin is. Heating (temperature != 0) relaxes the
//     piece toward the emitter's ΔT as Legacy does, scaled by the piece's largest kernel value instead of its share inside a shape.
// All sums of an emitter's step complete (one UAV barrier) before any deposit; deposits run in emitter order with a barrier
// between, as Legacy emission does.

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
    // CSMistEmit: the cells it covers, from emitBoxMinimum, emitBoxSize along each axis (within the window). AIR.4F.2b, the Point
    // passes: the footprint's cells (not clipped to the window), and the step of the frame (0 or 1), which picks its weight words.
    int3 emitBoxMinimum;
    uint emitStep;
    uint3 emitBoxSize;
    uint emitPadding1;
};

// Matches GpuMistEmitter (AirField.cpp) and MistEmitterShape (MistEmitterComponent.h).
struct MistEmitter
{
    float3 position;
    uint shape;
    float3 axisX;
    // Legacy: the sphere's radius. AIR.4F.2b, Point: the footprint radius R in metres.
    float radius;
    float3 axisY;
    // Legacy: density added per step where the shape covers a whole piece: rate * density multiplier * dt. Point: the quantity per
    // step, Q dt (density x m^3).
    float densityPerStep;
    float3 axisZ;
    // Cell crossings a straight path inside the emitter's bounding sphere can need.
    uint maximumCrossings;
    float3 halfExtents;
    // AIR.4F F4.5.1b: the temperature the emitter brings the air in its shape toward, as ΔT in kelvin (0: no heating).
    float temperature;
    float3 color;
    // AIR.4F.2b: MistEmitterMode.
    uint mode;
    // AIR.4F.2b.1, Point: the temperature response rate, per second. AIR.4F.2c, Volume: its temperature response.
    float temperatureRate;
    // AIR.4F.2c, Volume: 1 - exp(-replenishment rate x dt), the edge softness and the fill height (m); densityPerStep is the target.
    float replenishment;
    float edgeSoftness;
    float fillHeight;
};

// AIR.4F F4.5.1b: per second, the rate at which air wholly inside an emitter's shape approaches its temperature (time constant 1 s).
static const float HeatingRate = 1.0;

StructuredBuffer<MistEmitter> emitters : register(t0);

static const uint EmitterSphere = 0;
// Matches MistEmitterMode.
static const uint EmitterPoint = 1;
static const uint EmitterVolume = 2;
// Matches MistStatusBytesPerEmitter (AirField.cpp): per emitter, uint words 0 origin state, 1 receiving pieces, then for each step s
// of the frame (AIR.4F.2b, Point) words 2 + 2s the total weight D and 3 + 2s the part of it outside the window; 6 and 7 unused.
static const uint StatusStride = 32;
// AIR.4F.2b: kernel weights in fixed point. A sub-cell weighs at most 65536; a 2 m footprint's weights sum to about 1.2e8, far below
// 2^32.
static const float PointWeightScale = 65536.0;
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
    return AirSampleMist(Path(), ReadMist(), from, anchor - 0.5, own) * dissipationFactor;
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
    status.Store4(thread.x * StatusStride, uint4(OriginOf(emitters[thread.x]).state, 0, 0, 0));
    // AIR.4F.2b: both steps' Point weight sums start from zero.
    status.Store4(thread.x * StatusStride + 16, uint4(0, 0, 0, 0));
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

        const float share = float(inside) / float(AirSubcellCount(mask));
        const float added = emitter.densityPerStep * share;
        RWByteAddressBuffer mist = ResourceDescriptorHeap[NonUniformResourceIndex(piece == 0 ? mistWrite0 : mistWrite1)];
        const float4 value = asfloat(mist.Load4(address)) + float4(emitter.color * added, added);
        mist.Store4(address, asuint(value));

        // AIR.4F F4.5.1b: heating or cooling, independent of the mist: the piece's ΔT (the latest velocity's w) moves toward the
        // emitter's by the share of the gap the rate closes in a step, scaled by the piece's share inside the shape. The new value lies
        // between the old and the emitter's, so it never overshoots; overlapping emitters apply in order, each toward its own value.
        if(emitter.temperature != 0.0)
        {
            RWByteAddressBuffer velocity = ResourceDescriptorHeap[NonUniformResourceIndex(piece == 0 ? velocityIndex : velocityPiece1Index)];
            const float temperature = asfloat(velocity.Load(address + 12));
            const float approach = (1.0 - exp(-HeatingRate * timeStep)) * share;
            velocity.Store(address + 12, asuint(temperature + (emitter.temperature - temperature) * approach));
        }

        if(countPieces != 0)
        {
            RWByteAddressBuffer status = ResourceDescriptorHeap[NonUniformResourceIndex(statusIndex)];
            status.InterlockedAdd(emitterIndex * StatusStride + 4, 1);
        }
    }
}

// AIR.4F.2b: the kernel weight of a sub-cell centre (window cell units) for a Point emitter whose origin is at `originPosition`.
uint PointWeight(MistEmitter emitter, float3 centre, float3 originPosition)
{
    const float distance = length(centre - originPosition) * field.cellSize;
    const float closeness = saturate(1.0 - distance / emitter.radius);
    return uint(closeness * closeness * PointWeightScale);
}

// A piece's part of a Point footprint: its sub-cells' total and largest weight, and its weighted sub-cell nearest the origin.
struct PointPiece
{
    uint weight;
    uint peak;
    uint nearest;
};

PointPiece PointPieceOf(MistEmitter emitter, EmitterOrigin origin, int3 local, uint64_t mask)
{
    PointPiece result;
    result.weight = 0;
    result.peak = 0;
    result.nearest = 0;
    float nearestDistance = 1e30;
    uint64_t remaining = mask;

    while(remaining != 0)
    {
        const uint bit = AirLowestSubcell(remaining);
        remaining &= remaining - 1;
        const float3 centre = float3(local) + (float3(AirSubcellOfBit(bit)) + 0.5) * 0.25;
        const uint weight = PointWeight(emitter, centre, origin.position);

        if(weight == 0)
        {
            continue;
        }

        result.weight += weight;
        result.peak = max(result.peak, weight);
        const float3 offset = centre - origin.position;
        const float distance = dot(offset, offset);

        if(distance < nearestDistance)
        {
            nearestDistance = distance;
            result.nearest = bit;
        }
    }

    return result;
}

// Whether the origin sees piece `piece` of a window cell (Legacy emission's rule): the origin's own piece of its cell, or a straight
// path through open fluid from the origin to the piece's sub-cell `nearest`.
bool PointPieceVisible(MistEmitter emitter, EmitterOrigin origin, int3 cell, uint piece, uint nearest)
{
    if(all(cell == origin.cell))
    {
        return piece == origin.piece;
    }

    const float3 target = float3(cell - field.minimum) + (float3(AirSubcellOfBit(nearest)) + 0.5) * 0.25;
    const AirPathEnd path = AirTraverse(Path(), origin.position, origin.cell, origin.piece, target - origin.position, true, cell,
        emitter.maximumCrossings);
    return path.reached && path.piece == piece;
}

// The address of the emitter's weight words for this step: the total D, then the part outside the window.
uint PointSumAddress()
{
    return emitterIndex * StatusStride + 8 + emitStep * 8;
}

// One thread per footprint cell: adds the cell's weights to D (see the header).
[numthreads(4, 4, 4)]
void CSMistPointSum(uint3 thread : SV_DispatchThreadID)
{
    const MistEmitter emitter = emitters[emitterIndex];
    const EmitterOrigin origin = OriginOf(emitter);
    const bool emitting = origin.state == OriginOpen || origin.state == OriginUnknown;
    uint total = 0;
    uint outside = 0;

    if(all(thread < emitBoxSize) && emitting)
    {
        const int3 cell = emitBoxMinimum + int3(thread);
        const int3 local = cell - field.minimum;

        if(!AirContains(field, cell))
        {
            // Beyond the window: every sub-cell centre counts as open air, so its share is lost rather than moved inside.
            for(uint bit = 0; bit < 64; ++bit)
            {
                outside += PointWeight(emitter, float3(local) + (float3(AirSubcellOfBit(bit)) + 0.5) * 0.25, origin.position);
            }

            total = outside;
        }
        else
        {
            const AirCell own = AirLoadCell(Path(), cell);

            for(uint piece = 0; piece < 2; ++piece)
            {
                const uint64_t mask = AirPieceMask(own, piece);

                if(mask == 0)
                {
                    continue;
                }

                const PointPiece share = PointPieceOf(emitter, origin, local, mask);

                if(share.weight != 0 && PointPieceVisible(emitter, origin, cell, piece, share.nearest))
                {
                    total += share.weight;
                }
            }
        }
    }

    // One atomic per wave; integer sums do not depend on the order threads or waves add in.
    const uint waveTotal = WaveActiveSum(total);
    const uint waveOutside = WaveActiveSum(outside);

    if(WaveIsFirstLane() && waveTotal != 0)
    {
        RWByteAddressBuffer status = ResourceDescriptorHeap[NonUniformResourceIndex(statusIndex)];
        status.InterlockedAdd(PointSumAddress(), waveTotal);

        if(waveOutside != 0)
        {
            status.InterlockedAdd(PointSumAddress() + 4, waveOutside);
        }
    }
}

// One thread per footprint cell inside the window: deposits each visible piece's share of the step's quantity, and heats it.
[numthreads(4, 4, 4)]
void CSMistPointDeposit(uint3 thread : SV_DispatchThreadID)
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

    RWByteAddressBuffer status = ResourceDescriptorHeap[NonUniformResourceIndex(statusIndex)];
    // 0 when the step has no quantity to deposit (CSMistPointSum was not run): heating only.
    const uint total = status.Load(PointSumAddress());
    const float subcellVolume = pow(field.cellSize * 0.25, 3.0);
    const AirCell own = AirLoadCell(Path(), cell);
    const int3 local = cell - field.minimum;
    const uint address = AirVelocityAddress(field, AirSlotOf(field, cell));

    for(uint piece = 0; piece < 2; ++piece)
    {
        const uint64_t mask = AirPieceMask(own, piece);

        if(mask == 0)
        {
            continue;
        }

        const PointPiece share = PointPieceOf(emitter, origin, local, mask);

        if(share.weight == 0 || !PointPieceVisible(emitter, origin, cell, piece, share.nearest))
        {
            continue;
        }

        if(total != 0)
        {
            const float volume = float(AirSubcellCount(mask)) * subcellVolume;
            const float added = emitter.densityPerStep * (float(share.weight) / float(total)) / volume;
            RWByteAddressBuffer mist = ResourceDescriptorHeap[NonUniformResourceIndex(piece == 0 ? mistWrite0 : mistWrite1)];
            const float4 value = asfloat(mist.Load4(address)) + float4(emitter.color * added, added);
            mist.Store4(address, asuint(value));
        }

        // A bounded relaxation toward the emitter's ΔT. AIR.4F.2b.1: at the emitter's own rate, scaled inside the exponent by the
        // piece's largest kernel value (1 at the origin, falling smoothly to 0 at the footprint's edge), so it is consistent in the
        // time step and never overshoots, whatever the rate.
        if(emitter.temperature != 0.0)
        {
            RWByteAddressBuffer velocity = ResourceDescriptorHeap[NonUniformResourceIndex(piece == 0 ? velocityIndex : velocityPiece1Index)];
            const float temperature = asfloat(velocity.Load(address + 12));
            const float approach = 1.0 - exp(-emitter.temperatureRate * (float(share.peak) / PointWeightScale) * timeStep);
            velocity.Store(address + 12, asuint(temperature + (emitter.temperature - temperature) * approach));
        }

        if(countPieces != 0)
        {
            status.InterlockedAdd(emitterIndex * StatusStride + 4, 1);
        }
    }
}

// AIR.4F.2c Volume emission (CSMistVolume; Documentation/Air/AirVolumeEmitters4F2c.md): one shared pass per step over the cells of every
// Volume emitter's box, after transport and before every other emitter. A Volume never transports and never removes mist; per fluid
// piece and volume v:
//   - p_v: the volume's profile averaged over the piece's sub-cell centres (so a piece only partly inside has a partial target). In the
//     box's local frame (its axes, about the emission origin), the profile is S(hx - |x|) S(hz - |z|) S(top - y) inside the box below
//     top = min(-hy + fillHeight, hy), 0 elsewhere, with S the smoothstep from 0 to the edge softness (a step when it is 0). The bottom
//     is not faded, so floor fog reaches the floor;
//   - its increment a_v max(target_v p_v - density, 0), a_v = 1 - exp(-replenishment rate x dt).
// The piece gains the largest increment (in that volume's colour), so overlapping volumes hold the largest target, never their sum,
// whatever their order; it never passes the target, and never removes mist above it. Temperature: of the volumes with a non-zero ΔT and
// p_v > 0, the one with the largest p_v (then the faster response, then the larger ΔT) relaxes the piece by 1 - exp(-k p_v dt).
// Each thread owns its cell, so the pass needs no atomics but the status counts (one per wave and volume, on the frame's first step).

// The profile at a point (metres, the volume's local frame).
float VolumeProfile(MistEmitter emitter, float3 local)
{
    const float3 half = emitter.halfExtents;
    const float top = min(-half.y + emitter.fillHeight, half.y);

    if(abs(local.x) > half.x || abs(local.z) > half.z || local.y < -half.y || local.y > top)
    {
        return 0.0;
    }

    if(emitter.edgeSoftness <= 0.0)
    {
        return 1.0;
    }

    return smoothstep(0.0, emitter.edgeSoftness, half.x - abs(local.x)) * smoothstep(0.0, emitter.edgeSoftness, half.z - abs(local.z)) *
        smoothstep(0.0, emitter.edgeSoftness, top - local.y);
}

float3 VolumeLocal(MistEmitter emitter, float3 world)
{
    const float3 offset = world - emitter.position;
    return float3(dot(offset, emitter.axisX), dot(offset, emitter.axisY), dot(offset, emitter.axisZ));
}

// Whether a whole cell (centre `local`, half-diagonal `reach`, both in the volume's frame) lies where the profile is 1 (1), 0 (0), or
// neither (-1): a cell entirely in the interior needs no sub-cell evaluation, and one entirely outside none at all.
int VolumeCellClass(MistEmitter emitter, float3 local, float reach)
{
    const float3 half = emitter.halfExtents;
    const float top = min(-half.y + emitter.fillHeight, half.y);

    if(abs(local.x) > half.x + reach || abs(local.z) > half.z + reach || local.y < -half.y - reach || local.y > top + reach)
    {
        return 0;
    }

    const float margin = emitter.edgeSoftness + reach;

    if(half.x - abs(local.x) >= margin && half.z - abs(local.z) >= margin && top - local.y >= margin && local.y + half.y >= reach)
    {
        return 1;
    }

    return -1;
}

// The piece's mean profile: its sub-cell centres' profile values, averaged.
float VolumePieceProfile(MistEmitter emitter, int3 local, uint64_t mask)
{
    float total = 0.0;
    uint count = 0;
    uint64_t remaining = mask;

    while(remaining != 0)
    {
        const uint bit = AirLowestSubcell(remaining);
        remaining &= remaining - 1;
        const float3 centre = float3(local) + (float3(AirSubcellOfBit(bit)) + 0.5) * 0.25;
        total += VolumeProfile(emitter, VolumeLocal(emitter, (centre + float3(field.minimum)) * field.cellSize));
        ++count;
    }

    return count > 0 ? total / float(count) : 0.0;
}

// Whether colour `a` wins a tie of increments over `b` (lexicographic), so even equal increments do not depend on the emitters' order.
bool VolumeColourWins(float3 a, float3 b)
{
    if(a.x != b.x)
    {
        return a.x > b.x;
    }

    if(a.y != b.y)
    {
        return a.y > b.y;
    }

    return a.z > b.z;
}

// A piece's best volume so far: the density increment and its colour, and the dominant temperature volume.
struct VolumePieceState
{
    float density;
    float increment;
    float3 color;
    float profile;
    float temperatureWeight;
    float temperatureRate;
    float temperatureTarget;
};

void VolumeVisit(MistEmitter emitter, float profile, inout VolumePieceState state)
{
    if(profile <= 0.0)
    {
        return;
    }

    const float increment = emitter.replenishment * max(emitter.densityPerStep * profile - state.density, 0.0);

    if(increment > state.increment || (increment == state.increment && increment > 0.0 && VolumeColourWins(emitter.color, state.color)))
    {
        state.increment = increment;
        state.color = emitter.color;
    }

    if(emitter.temperature == 0.0 || emitter.temperatureRate <= 0.0)
    {
        return;
    }

    const bool dominant = profile > state.temperatureWeight ||
        (profile == state.temperatureWeight && (emitter.temperatureRate > state.temperatureRate ||
        (emitter.temperatureRate == state.temperatureRate && emitter.temperature > state.temperatureTarget)));

    if(dominant)
    {
        state.temperatureWeight = profile;
        state.temperatureRate = emitter.temperatureRate;
        state.temperatureTarget = emitter.temperature;
    }
}

[numthreads(4, 4, 4)]
void CSMistVolume(uint3 thread : SV_DispatchThreadID)
{
    const int3 cell = emitBoxMinimum + int3(thread);
    const bool active = all(thread < emitBoxSize) && AirContains(field, cell);
    const int3 local = cell - field.minimum;
    const AirCell own = AirLoadCell(Path(), active ? cell : field.minimum);
    const uint64_t masks[2] = {active ? AirPieceMask(own, 0) : 0, active ? AirPieceMask(own, 1) : 0};
    const uint address = AirVelocityAddress(field, AirSlotOf(field, active ? cell : field.minimum));
    RWByteAddressBuffer mist0 = ResourceDescriptorHeap[NonUniformResourceIndex(mistWrite0)];
    RWByteAddressBuffer mist1 = ResourceDescriptorHeap[NonUniformResourceIndex(mistWrite1)];
    const float4 before[2] = {masks[0] != 0 ? asfloat(mist0.Load4(address)) : float4(0.0, 0.0, 0.0, 0.0),
        masks[1] != 0 ? asfloat(mist1.Load4(address)) : float4(0.0, 0.0, 0.0, 0.0)};
    VolumePieceState states[2];

    for(uint piece = 0; piece < 2; ++piece)
    {
        states[piece] = (VolumePieceState)0;
        states[piece].density = before[piece].w;
        states[piece].temperatureWeight = 0.0;
    }

    const float3 centre = (float3(cell) + 0.5) * field.cellSize;
    // Half a cell's diagonal: the farthest a sub-cell centre lies from the cell's centre, in any frame.
    const float reach = 0.8660254 * field.cellSize;

    // Volume-outer, so every lane of a wave runs the same iterations and the receiving pieces can be counted per wave.
    for(uint index = 0; index < emitterCount; ++index)
    {
        const MistEmitter emitter = emitters[index];

        if(emitter.mode != EmitterVolume)
        {
            continue;
        }

        const int cellClass = active ? VolumeCellClass(emitter, VolumeLocal(emitter, centre), reach) : 0;
        uint receiving = 0;

        for(uint piece = 0; piece < 2; ++piece)
        {
            if(masks[piece] == 0 || cellClass == 0)
            {
                continue;
            }

            const float profile = cellClass == 1 ? 1.0 : VolumePieceProfile(emitter, local, masks[piece]);
            VolumeVisit(emitter, profile, states[piece]);
            receiving += profile > 0.0 ? 1 : 0;
        }

        if(countPieces != 0)
        {
            const uint waveReceiving = WaveActiveSum(receiving);

            if(WaveIsFirstLane() && waveReceiving != 0)
            {
                RWByteAddressBuffer status = ResourceDescriptorHeap[NonUniformResourceIndex(statusIndex)];
                status.InterlockedAdd(index * StatusStride + 4, waveReceiving);
            }
        }
    }

    for(uint piece = 0; piece < 2; ++piece)
    {
        const VolumePieceState state = states[piece];

        if(masks[piece] == 0)
        {
            continue;
        }

        if(state.increment > 0.0)
        {
            const float4 value = before[piece] + float4(state.color * state.increment, state.increment);

            if(piece == 0)
            {
                mist0.Store4(address, asuint(value));
            }
            else
            {
                mist1.Store4(address, asuint(value));
            }
        }

        if(state.temperatureWeight > 0.0)
        {
            RWByteAddressBuffer velocity = ResourceDescriptorHeap[NonUniformResourceIndex(piece == 0 ? velocityIndex : velocityPiece1Index)];
            const float temperature = asfloat(velocity.Load(address + 12));
            const float approach = 1.0 - exp(-state.temperatureRate * state.temperatureWeight * timeStep);
            velocity.Store(address + 12, asuint(temperature + (state.temperatureTarget - temperature) * approach));
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
