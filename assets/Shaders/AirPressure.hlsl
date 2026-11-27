// AIR.4E.3 geometry-aware pressure solver (Documentation/Air/AirArchitecture.md, AIR.4E.3). Diagnostics only in AIR.4E.3: nothing
// here writes the air's velocity, temperature or mist.
//
// The system (AIR.4E.0): one pressure unknown per fluid piece; K phi = b with K the aperture-weighted graph Laplacian over the AIR.4E.1
// face connections (weight = aperture / 16) plus the open window faces' weights on the diagonal (pressure 0 outside), and b = -outflow
// (AirDivergence.hlsli, the collocated measure).
//
// Fine-level storage is in the velocity buffers' slot order (toroidal), one value per piece:
//   phi, rhs, scratch: float2 per slot (piece 0, piece 1); scratch holds the labels during a rebuild, and residuals afterwards;
//   component: uint2 per slot: the piece's closed-component id, or PressureOpen / PressureIsolated / PressureOverflow;
//   fine map: uint per slot: each piece's aggregate slot on level 1 (bits 0-3 piece 0, 4-7 piece 1; 0xF none); AIR.4E.6: bit 8 + piece
//     set when the piece is solved (FineSolvable), so the fine smoothing reads neither the piece masks nor the component codes.
// Coarse levels are window-local (cell = local, x fastest), PressureSlots nodes per cell, in one hierarchy buffer at the level's offsets:
//   weights: 3 axes x 4 x 4 fp16 couplings (I of this cell, J of its +axis neighbour), 96 bytes per cell;
//   diagonal, phi, rhs: 4 floats per cell; info: present slots (bits 0-3); map: this level's slots' aggregates on the next level.
// The closed-component table: a 256-byte header (counts, maxima, a changed flag per labelling round), then 32 bytes per component:
//   volume (sub-cells), the right-hand side's sum and phi's volume-weighted sum as 64-bit fixed point, then (AIR P1/G1) the largest
//   finite |V phi| of the solve, the scale of the gauge sum.

#include "AirDivergence.hlsli"

struct AirPressureLevel
{
    uint3 dims;
    uint cells;

    uint weights;
    uint diagonal;
    uint info;
    uint map;

    uint phi;
    uint rhs;
    // AIR.4F.3 F1a.3: each aggregate's window-face weight (4 floats per cell), kept from the hierarchy build for its diagonal. It was
    // parked in rhs, which every solve overwrites, so a later local update could not read the unchanged children's weights.
    uint boundary;
    uint levelPadding;
};

cbuffer AirPressureConstants : register(b0)
{
    AirFieldBinding field;

    uint apertureIndex;
    uint phiIndex;
    uint rhsIndex;
    uint scratchIndex;

    uint componentIndex;
    uint fineMapIndex;
    uint tableIndex;
    uint hierarchyIndex;

    float3 ambientVelocity;
    uint colour;

    // The level a coarse kernel works on, and the next coarser one (an aggregation or restriction writes `coarse`).
    AirPressureLevel level;
    AirPressureLevel coarse;

    uint labelRound;
    uint componentCapacity;
    uint sweeps;
    float correctionScale;

    // AIR.4E.4: the current velocity buffers' writable views (piece 0, piece 1), for CSCorrect.
    uint velocityWriteIndex;
    uint piece1WriteIndex;
    // AIR.4E.5b: the labelling passes' indirect dispatch arguments (writable view): the fine grid's group counts, zeroed once a hooking
    // round changes nothing (CSLabelGate).
    uint labelArgumentsIndex;
    // AIR.4E.6: a restriction also performs `coarse`'s first (red) pre-smoothing half-sweep (CSRestrictFine, CSRestrictCoarse).
    uint firstSweepFused;

    // AIR.4E.5b: a rebuild after a recentre keeps phi (the warm start) in the cells that were in the window of the previous build,
    // [preservedMinimum, preservedMinimum + dimensions), when preservePhi is set; every other cell starts from 0.
    int3 preservedMinimum;
    uint preservePhi;

    // AIR.4F.3 F1a.3: the cells a box-driven kernel covers (window-local; on the hierarchy kernels, cells of `coarse`; on the topology
    // check, fine cells), the number of coarse levels (the recovery gate), and the recovery gate's step.
    int3 boxMinimum;
    uint levelCount;
    int3 boxSize;
    uint recoveryStep;
    // AIR.4F.3 F1a.3: the topology buffer (CSTopologyCheck, CSTopologyArguments) and the indirect arguments a gate writes (the relabel's
    // or the recovery cycles').
    uint topologyIndex;
    uint gateArgumentsIndex;
    // F1a.3 closeout: a local hierarchy update (aggregates add the change of their block's overflow; a relabel keeps the header's
    // overflow total), and the relabel predicate (CSTopologyArguments: nonzero when the components must be relabelled).
    uint localHierarchy;
    uint predicateIndex;
    // AIR P1/C1: each piece's gross right-hand-side magnitude G (uint per slot: two truncated floats, CSPrepare), for the residual's
    // rounding allowance (CSResidual).
    uint rhsGrossIndex;
    // AIR solver restructure: how many of the first recovery cycles belong to the step's own cycle budget (the adaptive warm cycles);
    // CSRecoveryGate counts the cycles run beyond them in TopologyRecoveryBeyondBudget.
    uint budgetedRecoveryCycles;
    uint2 constantsPadding;
};

// AIR.4F.1b: buoyancy (AirDivergence.hlsli's AirBuoyancy), two root constants bound for CSPrepare and CSCorrect only, so the shared
// constants (256 bytes, uploaded for every dispatch) are unchanged. An impulse of 0 is no buoyancy: both kernels then compute exactly
// what they did before it existed.
cbuffer AirBuoyancyConstants : register(b1)
{
    float buoyancyImpulse;
    float buoyancyReferenceTemperature;
    // AIR.4F.3 F2b.2 (CSPrepare only): the moving-wall source D (float2 per slot, sub-cell volumes per step; PressureNoWallSource: none),
    // its scale to the right-hand side's units (cell / (64 dt)), and the per-closed-component diagnostic sums (PressureNoWallSource: none).
    uint wallSourceIndex;
    float wallSourceScale;
    uint closedSourceIndex;
    uint wallSourcePadding;
};

static const uint PressureNoWallSource = 0xFFFFFFFFu;
// F2b.2 closed-component diagnostics: per closed code, the net and gross wall source (int64 each, ToFixed of the right-hand-side units).
static const uint ClosedSourceStride = 16;

AirBuoyancy Buoyancy()
{
    AirBuoyancy buoyancy;
    buoyancy.impulse = buoyancyImpulse;
    buoyancy.referenceTemperature = buoyancyReferenceTemperature;
    return buoyancy;
}

static const uint PressureSlots = 4;
static const uint PressureNoSlot = 0xFu;
static const uint PressureAbsent = 0xFFFFFFFFu;
static const uint PressureOpen = 0xFFFFFFFFu;
static const uint PressureOverflow = 0xFFFFFFFEu;
static const uint PressureIsolated = 0xFFFFFFFDu;
static const uint PressureWeightBytes = 96;
// Header words.
static const uint PressureClosedCount = 0;
static const uint PressureOverflowCount = 4;
static const uint PressureIsolatedCount = 8;
static const uint PressureMaximumRhs = 12;
static const uint PressureMaximumResidual = 16;
// AIR A1: the residual before the previous recovery cycle (asfloat; 0 before the first), for the stagnation guard.
static const uint PressurePreviousResidual = 24;
// AIR P1/C1: the largest residual beyond each piece's right-hand-side rounding allowance (CSResidual; asfloat), the recovery gate's measure.
static const uint PressureMaximumExcess = 28;
// Groups of a block past PressureSlots (left off the coarse levels), counted by the aggregation passes. F1a.3 closeout: each block also
// keeps its own count in its info word (bits 4-11), so a local update adds only the change and the total stays the current state's.
static const uint PressureAggregateOverflow = 20;
static const uint PressureInfoOverflowShift = 4;
// AIR.4E.5b's convergence criterion: the largest residual against the largest right-hand side.
static const float PressureRecoveryCriterion = 1.0e-3;
// AIR A1: a recovery cycle continues only if the previous one reduced the residual below this fraction of what it was. A healthy V-cycle
// reduces it to about 0.4-0.6 (AIR.4E.3); a solve that cannot converge (an inconsistent system, a residual at its floor) stays near 1.
static const float PressureStagnationRatio = 0.95;
// AIR P1/C1 (AirRelativeScaleGaugeG1.md): the fp32 rounding a piece's right-hand side carries, in units of the unit roundoff 2^-24 times
// its gross magnitude G (AirDivergence.hlsli's AirPieceFlux): per face term at most 8 (the stored velocities' own rounding 1, the mean's
// add 1, the buoyancy's evaluation 4 and its add 1, the aperture's multiply 1), and the accumulation of up to 12 face terms and the
// moving-wall source 12. A residual within this allowance of a piece is rounding of that piece's fluxes, which no fp32 projection can
// remove (after the air comes to rest the right-hand side is nothing but such rounding: AirStationaryBuoyancyConvergenceP1.md).
static const float PressureRhsRoundingUnits = 20.0;
static const float PressureUnitRoundoff = 5.9604644775390625e-8;
// AIR.4F.3 F1a.3: the topology buffer's words: the moving-solid overlay's lowest and highest component code of the pieces it replaced
// and its net volume change (sub-cells, two's complement), then this check's result, whether it relabelled, and cumulative counts of
// checks and relabels.
static const uint TopologyCodeLow = 0;
static const uint TopologyCodeHigh = 4;
static const uint TopologyVolumeDelta = 8;
static const uint TopologyLocalOk = 12;
static const uint TopologyRelabel = 16;
static const uint TopologyChecks = 20;
static const uint TopologyRelabels = 24;
// AIR.4F.3 F1a.3: recovery cycles run, and solves still above the criterion when the recovery budget ran out (cumulative; kept here
// because a relabel clears the table's header).
static const uint TopologyRecoveryCycles = 28;
static const uint TopologyRecoveryFailures = 32;
// Why the latest check failed (bits): 1 too large, 2 several codes, 4 replaced pieces of another code, 8 not connected, 16 no outer piece,
// 32 window faces could change the component's grounding.
static const uint TopologyFailure = 36;
// Set by the moving-solid overlay when a piece it replaced had a window face.
static const uint TopologyReplacedWindow = 40;
// AIR A1: recovery stopped by the stagnation guard (cumulative; such a solve also counts in TopologyRecoveryFailures).
static const uint TopologyRecoveryStagnations = 44;
// AIR A2/T2a (diagnostics): cumulative relabel reasons. Forced by the caller (several pending updates, or a box past the check's capacity);
// then, for relabels the check decided, each failed condition (a relabel may fail several): the box too large for the check, more than
// one code in the box, replaced pieces of another code, the box's pieces not connected inside it, no piece in the box's outer layer, and
// a possible change of grounding (window faces).
static const uint TopologyReasonForced = 48;
static const uint TopologyReasonSize = 52;
static const uint TopologyReasonCodes = 56;
static const uint TopologyReasonReplaced = 60;
static const uint TopologyReasonDisconnected = 64;
static const uint TopologyReasonShell = 68;
static const uint TopologyReasonGrounding = 72;
// AIR A2/T2a (diagnostics): the latest check's box, as a sample: the lowest and highest code present, its cells and its present pieces.
static const uint TopologySampleCodeLow = 76;
static const uint TopologySampleCodeHigh = 80;
static const uint TopologySampleCells = 84;
static const uint TopologySamplePieces = 88;
// AIR A2/T2a: the lowest and highest code the rewritten cells' new pieces read (AirMovingSolids.hlsl's RecordTopology), low above high
// when none.
static const uint TopologyCurrentLow = 92;
static const uint TopologyCurrentHigh = 96;
// AIR A2/T2b: the code of the component an accepted check found every rewritten piece in (before and after), PressureAbsent when the check
// failed or there is none; CSTopologyArguments gives it the volume change.
static const uint TopologyAcceptedCode = 108;
// AIR A2/T2a (diagnostics): relabels after a two-code check whose rewritten cells' new pieces read another code, or whose labelling did
// not settle.
static const uint TopologyReasonCurrent = 100;
static const uint TopologyReasonUnsettled = 104;
// AIR A2/T2b (diagnostics; AirMovingSolids.hlsl's RecordTopology): the latest update's new pieces at an index that held none (their code
// is whatever the slot last held: 0 after a labelling), and each check-failed relabel by its first matching class: nothing rewritten,
// new pieces at an empty index, anything else.
static const uint TopologyUpdateNewSlots = 156;
static const uint TopologyClassEmpty = 160;
static const uint TopologyClassNewSlots = 164;
static const uint TopologyClassOther = 172;
// AIR P1/C1 (diagnostics): solves the recovery gate accepted only through the rounding allowance (their largest raw residual was above the
// criterion; cumulative).
static const uint TopologyRhsRoundingConverged = 176;
// AIR solver restructure: recovery cycles run beyond the solves' budgeted recovery cycles (cumulative): true recovery, as opposed to the
// adaptive warm cycles the budget itself gates. The adaptive recovery budget (AirField) reads it.
static const uint TopologyRecoveryBeyondBudget = 180;
// The topology check's capacity: pieces of its box held in group shared memory.
static const uint TopologyNodeCapacity = 4096;
static const uint PressureChangedFlags = 64;
static const uint PressureTableHeader = 256;
static const uint PressureEntryBytes = 32;
// AIR P1/G1: an entry's word holding its largest finite |V phi| this solve (CSGaugeScale), the scale of its gauge sum.
static const uint PressureGaugeMagnitude = 24;
// 2^20: sums of up to millions of values of order 1 to 100 stay far inside 64 bits, at about 1e-6 per value.
static const float PressureFixedScale = 1048576.0;

RWByteAddressBuffer Raw(uint index)
{
    return ResourceDescriptorHeap[NonUniformResourceIndex(index)];
}

// ------------------------------------------------------------------------------------------------ fine level

uint FineSlotIndex(int3 cell)
{
    return AirDivergenceSlotIndex(field, cell);
}

uint2 FineRecord(uint slotIndex)
{
    ByteAddressBuffer apertures = ResourceDescriptorHeap[NonUniformResourceIndex(apertureIndex)];
    return apertures.Load2(slotIndex * AirApertureBytesPerSlot);
}

float FineValue(uint bufferIndex, uint slotIndex, uint piece)
{
    return asfloat(Raw(bufferIndex).Load(slotIndex * 8 + piece * 4));
}

uint FineComponent(uint slotIndex, uint piece)
{
    return Raw(componentIndex).Load(slotIndex * 8 + piece * 4);
}

// A piece the solver works on: present, and in an open or closed component (isolated pieces and components past the table's
// capacity keep phi = 0).
bool FineSolvable(uint64_t mask, uint code)
{
    return mask != 0 && code != PressureIsolated && code != PressureOverflow;
}

// AIR.4E.6: a cell's piece masks where FineRowOf uses them, at the window's low faces (AirLowBoundaryAperture); elsewhere they are not
// loaded, and FineRowOf never reads the zeros returned there. The fine smoothing and restriction are bound by their loads (AIR.4E.5d),
// of which the masks were a third.
AirPieces FineLowFacePieces(int3 cell)
{
    AirPieces pieces;
    pieces.piece0 = 0;
    pieces.piece1 = 0;

    if(any(cell == field.minimum))
    {
        pieces = AirPiecesOf(field, cell);
    }

    return pieces;
}

uint64_t PieceMask(AirPieces pieces, uint piece)
{
    return piece == 0 ? pieces.piece0 : pieces.piece1;
}

// One row of K at piece `piece` of `cell`: its diagonal (every open face, window faces included) and the sum of its couplings times
// the neighbours' values in `valueIndex` (a float2-per-slot buffer).
struct FineRow
{
    float diagonal;
    float neighbours;
    float boundary;
};

FineRow FineRowOf(int3 cell, uint piece, uint64_t mask, uint2 ownRecord, uint valueIndex)
{
    uint diagonal = 0;
    uint boundary = 0;
    float neighbours = 0.0;

    [unroll]
    for(uint axis = 0; axis < 3; ++axis)
    {
        if(AirHighBoundary(ownRecord, axis))
        {
            boundary += AirApertureOf(ownRecord, axis, piece, 0);
        }
        else
        {
            int3 next = cell;
            next[axis] += 1;
            const uint nextSlot = FineSlotIndex(next);

            [unroll]
            for(uint other = 0; other < 2; ++other)
            {
                const uint area = AirApertureOf(ownRecord, axis, piece, other);

                if(area != 0)
                {
                    diagonal += area;
                    neighbours += float(area) * FineValue(valueIndex, nextSlot, other);
                }
            }
        }

        int3 previous = cell;
        previous[axis] -= 1;

        if(!AirContains(field, previous))
        {
            boundary += AirLowBoundaryAperture(mask, axis);
        }
        else
        {
            const uint previousSlot = FineSlotIndex(previous);
            const uint2 previousRecord = FineRecord(previousSlot);

            [unroll]
            for(uint other = 0; other < 2; ++other)
            {
                const uint area = AirApertureOf(previousRecord, axis, other, piece);

                if(area != 0)
                {
                    diagonal += area;
                    neighbours += float(area) * FineValue(valueIndex, previousSlot, other);
                }
            }
        }
    }

    FineRow row;
    row.diagonal = float(diagonal + boundary) * AirApertureScale;
    row.boundary = float(boundary) * AirApertureScale;
    row.neighbours = neighbours * AirApertureScale;
    return row;
}

int3 FineCellOfLocal(int3 local)
{
    return field.minimum + local;
}

// ------------------------------------------------------------------------------------------------ coarse levels

uint CoarseCellIndex(AirPressureLevel target, int3 local)
{
    return uint(local.x) + target.dims.x * (uint(local.y) + target.dims.y * uint(local.z));
}

bool CoarseContains(AirPressureLevel target, int3 local)
{
    return all(local >= 0) && all(local < int3(target.dims));
}

float CoarseWeight(AirPressureLevel target, uint cellIndex, uint axis, uint lower, uint upper)
{
    const uint entry = lower * PressureSlots + upper;
    const uint word = Raw(hierarchyIndex).Load(target.weights + cellIndex * PressureWeightBytes + axis * 32 + (entry / 2) * 4);
    return f16tof32((entry & 1) != 0 ? word >> 16 : word & 0xFFFFu);
}

float CoarseValue(uint offset, uint cellIndex, uint slot)
{
    return asfloat(Raw(hierarchyIndex).Load(offset + (cellIndex * PressureSlots + slot) * 4));
}

uint CoarsePresent(AirPressureLevel target, uint cellIndex)
{
    return Raw(hierarchyIndex).Load(target.info + cellIndex * 4) & 0xFu;
}

uint CoarseMap(AirPressureLevel target, uint cellIndex, uint slot)
{
    return (Raw(hierarchyIndex).Load(target.map + cellIndex * 4) >> (slot * 4)) & 0xFu;
}

// AIR.4E.6 C2: one fp16 coupling from a cell's axis rows held as uint4 (two per axis, 16 entries I * PressureSlots + J). `entry` must be
// a compile-time index (every caller's loops are unrolled), so the rows stay in registers.
float CoarseRowCoupling(uint4 words[6], uint axis, uint entry)
{
    const uint word = words[axis * 2 + entry / 8][(entry / 2) % 4];
    return f16tof32((entry & 1) != 0 ? word >> 16 : word & 0xFFFFu);
}

// AIR solver restructure (scope §2.7.1, C2 for levels 1 to 3): what a coarse cell's slots need from its neighbours, loaded once per cell
// instead of once per slot and coupling: per axis, its own couplings toward the +axis neighbour and those of its -axis neighbour toward
// it (each 32 bytes, two uint4), and both neighbours' phi (four slots, one uint4). Window-local neighbours only.
struct CoarseNeighbourhood
{
    uint4 next[6];
    uint4 previous[6];
    float4 nextPhi[3];
    float4 previousPhi[3];
    bool hasNext[3];
    bool hasPrevious[3];
};

CoarseNeighbourhood LoadCoarseNeighbourhood(AirPressureLevel target, int3 local, uint cellIndex)
{
    CoarseNeighbourhood neighbourhood;

    [unroll]
    for(uint axis = 0; axis < 3; ++axis)
    {
        int3 next = local;
        next[axis] += 1;
        int3 previous = local;
        previous[axis] -= 1;
        neighbourhood.hasNext[axis] = CoarseContains(target, next);
        neighbourhood.hasPrevious[axis] = CoarseContains(target, previous);
        neighbourhood.next[axis * 2] = uint4(0, 0, 0, 0);
        neighbourhood.next[axis * 2 + 1] = uint4(0, 0, 0, 0);
        neighbourhood.previous[axis * 2] = uint4(0, 0, 0, 0);
        neighbourhood.previous[axis * 2 + 1] = uint4(0, 0, 0, 0);
        neighbourhood.nextPhi[axis] = float4(0.0, 0.0, 0.0, 0.0);
        neighbourhood.previousPhi[axis] = float4(0.0, 0.0, 0.0, 0.0);

        if(neighbourhood.hasNext[axis])
        {
            const uint own = target.weights + cellIndex * PressureWeightBytes + axis * 32;
            neighbourhood.next[axis * 2] = Raw(hierarchyIndex).Load4(own);
            neighbourhood.next[axis * 2 + 1] = Raw(hierarchyIndex).Load4(own + 16);
            neighbourhood.nextPhi[axis] = asfloat(Raw(hierarchyIndex).Load4(target.phi + CoarseCellIndex(target, next) * PressureSlots * 4));
        }

        if(neighbourhood.hasPrevious[axis])
        {
            const uint previousIndex = CoarseCellIndex(target, previous);
            const uint other = target.weights + previousIndex * PressureWeightBytes + axis * 32;
            neighbourhood.previous[axis * 2] = Raw(hierarchyIndex).Load4(other);
            neighbourhood.previous[axis * 2 + 1] = Raw(hierarchyIndex).Load4(other + 16);
            neighbourhood.previousPhi[axis] = asfloat(Raw(hierarchyIndex).Load4(target.phi + previousIndex * PressureSlots * 4));
        }
    }

    return neighbourhood;
}

// Slot `slot` (a compile-time index) of a coarse cell: the sum of its couplings times the neighbours' phi, per axis the +axis neighbour's
// slots then the -axis neighbour's (the summation order of the scalar loads it replaced, so the result is bit-identical).
float CoarseNeighbours(CoarseNeighbourhood neighbourhood, uint slot)
{
    float sum = 0.0;

    [unroll]
    for(uint axis = 0; axis < 3; ++axis)
    {
        if(neighbourhood.hasNext[axis])
        {
            [unroll]
            for(uint other = 0; other < PressureSlots; ++other)
            {
                sum += CoarseRowCoupling(neighbourhood.next, axis, slot * PressureSlots + other) * neighbourhood.nextPhi[axis][other];
            }
        }

        if(neighbourhood.hasPrevious[axis])
        {
            [unroll]
            for(uint other = 0; other < PressureSlots; ++other)
            {
                sum += CoarseRowCoupling(neighbourhood.previous, axis, other * PressureSlots + slot) * neighbourhood.previousPhi[axis][other];
            }
        }
    }

    return sum;
}

int3 LocalOfCoarse(AirPressureLevel target, uint index)
{
    return int3(index % target.dims.x, (index / target.dims.x) % target.dims.y, index / (target.dims.x * target.dims.y));
}

// ------------------------------------------------------------------------------------------------ closed-component table

uint EntryAddress(uint id)
{
    return PressureTableHeader + id * PressureEntryBytes;
}

int64_t ToFixed(float value)
{
    return int64_t(value * PressureFixedScale);
}

float FromFixed(int64_t value)
{
    return float(double(value) / double(PressureFixedScale));
}

int64_t LoadInt64(RWByteAddressBuffer buffer, uint address)
{
    const uint2 words = buffer.Load2(address);
    return int64_t(uint64_t(words.x) | (uint64_t(words.y) << 32));
}

// ================================================================================================ connected components (rebuild)

// Labels: scratch holds each piece's parent (a piece index, slot * 2 + piece); absent pieces hold PressureAbsent. Every piece starts
// as its own parent; phi (the warm start) is invalidated with the topology.
[numthreads(8, 8, 8)]
void CSLabelInit(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const int3 cell = AirCellOfSlot(field, slot);
    const uint slotIndex = FineSlotIndex(cell);
    const AirPieces pieces = AirPiecesOf(field, cell);
    const uint2 own = uint2(slotIndex * 2, slotIndex * 2 + 1);
    Raw(scratchIndex).Store2(slotIndex * 8, uint2(pieces.piece0 != 0 ? own.x : PressureAbsent, pieces.piece1 != 0 ? own.y : PressureAbsent));
    Raw(componentIndex).Store2(slotIndex * 8, uint2(0, 0));

    // A recentre leaves a retained cell's slot, pieces and phi as they were; an entering cell's slot held another cell.
    const bool retained = preservePhi != 0 && all(cell >= preservedMinimum) && all(cell < preservedMinimum + int3(field.dimensions));

    if(!retained)
    {
        Raw(phiIndex).Store2(slotIndex * 8, uint2(0, 0));
    }

    if(all(slot == 0))
    {
        // The header: counts, maxima and every round's changed flag.
        for(uint word = 0; word < PressureTableHeader / 4; ++word)
        {
            // F1a.3 closeout: a local update's relabel keeps the overflow total its aggregates maintain.
            if(localHierarchy == 0 || word * 4 != PressureAggregateOverflow)
            {
                Raw(tableIndex).Store(word * 4, 0);
            }
        }

        // AIR.4E.5b: the labelling passes cover the whole grid until a round converges.
        Raw(labelArgumentsIndex).Store3(0, (field.dimensions + 7) / 8);
    }
}

uint Parent(uint node)
{
    return Raw(scratchIndex).Load(node * 4);
}

// One hooking pass: for every link between two pieces (an aperture above 0 toward a +axis neighbour), the larger of their parents is
// hooked under the smaller. Parents only decrease, so the final label of a component is its smallest piece index whatever the order.
[numthreads(8, 8, 8)]
void CSLabelHook(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const int3 cell = AirCellOfSlot(field, slot);
    const uint slotIndex = FineSlotIndex(cell);
    const AirPieces pieces = AirPiecesOf(field, cell);
    const uint2 record = FineRecord(slotIndex);
    bool changed = false;

    for(uint piece = 0; piece < 2; ++piece)
    {
        if(PieceMask(pieces, piece) == 0)
        {
            continue;
        }

        for(uint axis = 0; axis < 3; ++axis)
        {
            if(AirHighBoundary(record, axis))
            {
                continue;
            }

            int3 next = cell;
            next[axis] += 1;
            const uint nextSlot = FineSlotIndex(next);

            for(uint other = 0; other < 2; ++other)
            {
                if(AirApertureOf(record, axis, piece, other) == 0)
                {
                    continue;
                }

                const uint parent = Parent(slotIndex * 2 + piece);
                const uint otherParent = Parent(nextSlot * 2 + other);

                if(parent != otherParent)
                {
                    uint previous = 0;
                    Raw(scratchIndex).InterlockedMin(max(parent, otherParent) * 4, min(parent, otherParent), previous);
                    changed = true;
                }
            }
        }
    }

    if(changed)
    {
        Raw(tableIndex).InterlockedOr(PressureChangedFlags + (labelRound % 48) * 4, 1);
    }
}

// AIR.4E.5b: after hooking round `labelRound`, stops the labelling once that round changed nothing: no link joined two different parents,
// so every component already holds one label, its smallest piece index (the converged state; further rounds would change nothing). The
// remaining rounds' indirect dispatches then launch no groups.
[numthreads(1, 1, 1)]
void CSLabelGate(uint3 thread : SV_DispatchThreadID)
{
    if(Raw(tableIndex).Load(PressureChangedFlags + (labelRound % 48) * 4) == 0)
    {
        Raw(labelArgumentsIndex).Store3(0, uint3(0, 0, 0));
    }
}

// Pointer jumping: each piece takes its grandparent.
[numthreads(8, 8, 8)]
void CSLabelJump(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const uint slotIndex = FineSlotIndex(AirCellOfSlot(field, slot));

    for(uint piece = 0; piece < 2; ++piece)
    {
        const uint node = slotIndex * 2 + piece;
        const uint parent = Parent(node);

        if(parent == PressureAbsent)
        {
            continue;
        }

        const uint grandparent = Parent(parent);

        if(grandparent != parent)
        {
            Raw(scratchIndex).Store(node * 4, grandparent);
        }
    }
}

// A piece with an open window face grounds its component: its root's flag (in the component buffer) is set.
[numthreads(8, 8, 8)]
void CSLabelGround(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const int3 cell = AirCellOfSlot(field, slot);
    const uint slotIndex = FineSlotIndex(cell);
    const AirPieces pieces = AirPiecesOf(field, cell);
    const uint2 record = FineRecord(slotIndex);

    for(uint piece = 0; piece < 2; ++piece)
    {
        const uint64_t mask = PieceMask(pieces, piece);

        if(mask == 0)
        {
            continue;
        }

        const FineRow row = FineRowOf(cell, piece, mask, record, phiIndex);

        if(row.boundary > 0.0)
        {
            Raw(componentIndex).InterlockedOr(Parent(slotIndex * 2 + piece) * 4, 1);
        }
    }
}

// Each root decides its component's code: open (grounded), isolated (a piece with no open face at all: nothing to solve), or the next
// closed id (past the table's capacity: overflow, left unsolved and counted).
[numthreads(8, 8, 8)]
void CSLabelCompact(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const int3 cell = AirCellOfSlot(field, slot);
    const uint slotIndex = FineSlotIndex(cell);
    const AirPieces pieces = AirPiecesOf(field, cell);
    const uint2 record = FineRecord(slotIndex);

    for(uint piece = 0; piece < 2; ++piece)
    {
        const uint node = slotIndex * 2 + piece;
        const uint64_t mask = PieceMask(pieces, piece);

        if(mask == 0 || Parent(node) != node)
        {
            continue;
        }

        uint code = PressureOpen;

        if(Raw(componentIndex).Load(node * 4) == 0)
        {
            const FineRow row = FineRowOf(cell, piece, mask, record, phiIndex);

            if(row.diagonal == 0.0)
            {
                code = PressureIsolated;
                Raw(tableIndex).InterlockedAdd(PressureIsolatedCount, 1);
            }
            else
            {
                Raw(tableIndex).InterlockedAdd(PressureClosedCount, 1, code);

                if(code >= componentCapacity)
                {
                    code = PressureOverflow;
                    Raw(tableIndex).InterlockedAdd(PressureOverflowCount, 1);
                }
            }
        }

        Raw(componentIndex).Store(node * 4, code);
    }
}

// Every other piece takes its root's code (roots are final: this pass writes only non-roots).
[numthreads(8, 8, 8)]
void CSLabelFinalize(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const uint slotIndex = FineSlotIndex(AirCellOfSlot(field, slot));

    for(uint piece = 0; piece < 2; ++piece)
    {
        const uint node = slotIndex * 2 + piece;
        const uint parent = Parent(node);

        if(parent != PressureAbsent && parent != node)
        {
            Raw(componentIndex).Store(node * 4, Raw(componentIndex).Load(parent * 4));
        }
    }
}

// Each closed component's volume in sub-cells, and the closed entries' sums zeroed.
[numthreads(8, 8, 8)]
void CSLabelVolume(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const int3 cell = AirCellOfSlot(field, slot);
    const uint slotIndex = FineSlotIndex(cell);
    const AirPieces pieces = AirPiecesOf(field, cell);

    for(uint piece = 0; piece < 2; ++piece)
    {
        const uint64_t mask = PieceMask(pieces, piece);
        const uint code = FineComponent(slotIndex, piece);

        if(mask != 0 && code < componentCapacity)
        {
            Raw(tableIndex).InterlockedAdd(EntryAddress(code), AirSubcellCount(mask));
        }
    }
}

// Zeroes the closed table's entries (before CSLabelVolume).
[numthreads(64, 1, 1)]
void CSClearEntries(uint3 thread : SV_DispatchThreadID)
{
    if(thread.x < componentCapacity)
    {
        Raw(tableIndex).Store4(EntryAddress(thread.x), uint4(0, 0, 0, 0));
        Raw(tableIndex).Store4(EntryAddress(thread.x) + 16, uint4(0, 0, 0, 0));
    }
}

// ================================================================================================ hierarchy (rebuild)

// F1a.3 closeout: a coarse cell's present slots and its block's overflow count; the header's total takes the change (all of it in a full
// rebuild, whose labelling cleared the total; the difference from the block's previous count in a local update).
void StoreAggregateInfo(uint coarseIndex, uint present, uint overflow)
{
    const uint address = coarse.info + coarseIndex * 4;
    const uint previous = localHierarchy != 0 ? (Raw(hierarchyIndex).Load(address) >> PressureInfoOverflowShift) & 0xFFu : 0u;
    Raw(hierarchyIndex).Store(address, present | (overflow << PressureInfoOverflowShift));

    if(overflow != previous)
    {
        uint ignored;
        Raw(tableIndex).InterlockedAdd(PressureAggregateOverflow, overflow - previous, ignored);
    }
}

// AIR.4F.3 F1a.3: the `coarse` cell a hierarchy thread builds, within the box (the whole level for a full rebuild, a dirty footprint for a
// local update); false outside it.
bool BoxBlock(uint3 thread, out int3 block)
{
    block = boxMinimum + int3(thread);
    return all(int3(thread) < boxSize) && all(block >= 0) && all(block < int3(coarse.dims));
}

// One block's nodes: child (0 to 7: x + 2y + 4z) and slot, gathered for local union-find.
static const uint MaximumBlockNodes = 32;

uint FindRoot(inout uint parents[MaximumBlockNodes], uint node)
{
    uint root = node;

    [loop]
    for(uint step = 0; step < MaximumBlockNodes && parents[root] != root; ++step)
    {
        root = parents[root];
    }

    return root;
}

void Unite(inout uint parents[MaximumBlockNodes], uint a, uint b)
{
    const uint rootA = FindRoot(parents, a);
    const uint rootB = FindRoot(parents, b);
    parents[max(rootA, rootB)] = min(rootA, rootB);
}

int3 ChildOffset(uint child)
{
    return int3(child & 1, (child >> 1) & 1, child >> 2);
}

// The level-1 aggregates of a 2 x 2 x 2 block of fine cells: its solvable pieces joined only through links inside the block, so pieces a
// wall separates (or that meet only outside the block) stay apart. The first PressureSlots groups (in child, then piece order) take
// slots; any further group is left out of the coarse levels (counted in the header). Writes the fine map of the block's pieces, the
// coarse cell's present slots, and each aggregate's window-face weight (the sum of its pieces', into the level's boundary weights).
[numthreads(4, 4, 4)]
void CSAggregateFine(uint3 thread : SV_DispatchThreadID)
{
    int3 block;

    if(!BoxBlock(thread, block))
    {
        return;
    }

    const uint coarseIndex = CoarseCellIndex(coarse, block);
    uint parents[MaximumBlockNodes];
    uint nodeChild[MaximumBlockNodes];
    uint nodePiece[MaximumBlockNodes];
    float nodeBoundary[MaximumBlockNodes];
    uint nodeOf[16];
    uint count = 0;

    // Gather.
    for(uint child = 0; child < 8; ++child)
    {
        const int3 local = block * 2 + ChildOffset(child);
        nodeOf[child * 2] = MaximumBlockNodes;
        nodeOf[child * 2 + 1] = MaximumBlockNodes;

        if(any(local >= int3(field.dimensions)))
        {
            continue;
        }

        const int3 cell = FineCellOfLocal(local);
        const uint slotIndex = FineSlotIndex(cell);
        const AirPieces pieces = AirPiecesOf(field, cell);
        const uint2 record = FineRecord(slotIndex);

        for(uint piece = 0; piece < 2; ++piece)
        {
            const uint64_t mask = PieceMask(pieces, piece);

            if(!FineSolvable(mask, FineComponent(slotIndex, piece)))
            {
                continue;
            }

            parents[count] = count;
            nodeChild[count] = child;
            nodePiece[count] = piece;
            nodeBoundary[count] = FineRowOf(cell, piece, mask, record, phiIndex).boundary;
            nodeOf[child * 2 + piece] = count;
            ++count;
        }
    }

    // Join through links inside the block.
    for(uint node = 0; node < count; ++node)
    {
        const int3 local = block * 2 + ChildOffset(nodeChild[node]);
        const uint2 record = FineRecord(FineSlotIndex(FineCellOfLocal(local)));

        for(uint axis = 0; axis < 3; ++axis)
        {
            if(ChildOffset(nodeChild[node])[axis] != 0 || local[axis] + 1 >= int(field.dimensions[axis]))
            {
                continue;
            }

            const uint neighbourChild = nodeChild[node] + (1u << axis);

            for(uint other = 0; other < 2; ++other)
            {
                const uint neighbour = nodeOf[neighbourChild * 2 + other];

                if(neighbour < count && AirApertureOf(record, axis, nodePiece[node], other) != 0)
                {
                    Unite(parents, node, neighbour);
                }
            }
        }
    }

    // Slots, in order of each group's first node.
    uint slotOfRoot[MaximumBlockNodes];
    float boundary[PressureSlots] = {0.0, 0.0, 0.0, 0.0};
    uint present = 0;
    uint used = 0;
    uint overflow = 0;

    for(uint index = 0; index < count; ++index)
    {
        slotOfRoot[index] = PressureNoSlot;
    }

    uint fineMap[8];

    for(uint child = 0; child < 8; ++child)
    {
        fineMap[child] = 0xFFu;
    }

    for(uint node = 0; node < count; ++node)
    {
        const uint root = FindRoot(parents, node);

        if(slotOfRoot[root] == PressureNoSlot && used < PressureSlots)
        {
            slotOfRoot[root] = used;
            present |= 1u << used;
            ++used;
        }

        const uint slotValue = slotOfRoot[root];

        if(slotValue == PressureNoSlot && root == node)
        {
            ++overflow;
        }

        if(slotValue != PressureNoSlot)
        {
            boundary[slotValue] += nodeBoundary[node];
        }

        // Every gathered node is a solvable piece.
        const uint shift = nodePiece[node] * 4;
        fineMap[nodeChild[node]] = (fineMap[nodeChild[node]] & ~(0xFu << shift)) | (slotValue << shift) | (1u << (8 + nodePiece[node]));
    }

    for(uint child = 0; child < 8; ++child)
    {
        const int3 local = block * 2 + ChildOffset(child);

        if(all(local < int3(field.dimensions)))
        {
            Raw(fineMapIndex).Store(FineSlotIndex(FineCellOfLocal(local)) * 4, fineMap[child]);
        }
    }

    StoreAggregateInfo(coarseIndex, present, overflow);
    Raw(hierarchyIndex).Store4(coarse.boundary + coarseIndex * 16, asuint(float4(boundary[0], boundary[1], boundary[2], boundary[3])));
}

// The same for a block of coarse cells of `level` into `coarse`: nodes are the children's present slots, joined through the children's
// own couplings inside the block. The children's window-face weights are read from `level`'s rhs (written by its aggregation).
[numthreads(4, 4, 4)]
void CSAggregateCoarse(uint3 thread : SV_DispatchThreadID)
{
    int3 block;

    if(!BoxBlock(thread, block))
    {
        return;
    }

    const uint coarseIndex = CoarseCellIndex(coarse, block);
    uint parents[MaximumBlockNodes];
    uint nodeChild[MaximumBlockNodes];
    uint nodeSlot[MaximumBlockNodes];
    uint count = 0;

    for(uint child = 0; child < 8; ++child)
    {
        const int3 local = block * 2 + ChildOffset(child);

        if(!CoarseContains(level, local))
        {
            continue;
        }

        const uint present = CoarsePresent(level, CoarseCellIndex(level, local));

        for(uint slot = 0; slot < PressureSlots; ++slot)
        {
            if((present & (1u << slot)) != 0)
            {
                parents[count] = count;
                nodeChild[count] = child;
                nodeSlot[count] = slot;
                ++count;
            }
        }
    }

    for(uint node = 0; node < count; ++node)
    {
        const int3 offset = ChildOffset(nodeChild[node]);
        const int3 local = block * 2 + offset;
        const uint childIndex = CoarseCellIndex(level, local);

        for(uint other = 0; other < count; ++other)
        {
            const int3 otherOffset = ChildOffset(nodeChild[other]);
            const int3 step = otherOffset - offset;

            // The other node lies one cell up exactly one axis.
            if(dot(step, step) != 1 || any(step < 0))
            {
                continue;
            }

            const uint axis = step.y != 0 ? 1 : (step.z != 0 ? 2 : 0);

            if(CoarseWeight(level, childIndex, axis, nodeSlot[node], nodeSlot[other]) > 0.0)
            {
                Unite(parents, node, other);
            }
        }
    }

    uint slotOfRoot[MaximumBlockNodes];
    float boundary[PressureSlots] = {0.0, 0.0, 0.0, 0.0};
    uint present = 0;
    uint used = 0;
    uint overflow = 0;
    uint childMap[8];

    for(uint index = 0; index < count; ++index)
    {
        slotOfRoot[index] = PressureNoSlot;
    }

    for(uint child = 0; child < 8; ++child)
    {
        childMap[child] = 0xFFFFu;
    }

    for(uint node = 0; node < count; ++node)
    {
        const uint root = FindRoot(parents, node);

        if(slotOfRoot[root] == PressureNoSlot && used < PressureSlots)
        {
            slotOfRoot[root] = used;
            present |= 1u << used;
            ++used;
        }

        const uint slotValue = slotOfRoot[root];
        const int3 local = block * 2 + ChildOffset(nodeChild[node]);

        if(slotValue == PressureNoSlot && root == node)
        {
            ++overflow;
        }

        if(slotValue != PressureNoSlot)
        {
            boundary[slotValue] += CoarseValue(level.boundary, CoarseCellIndex(level, local), nodeSlot[node]);
        }

        const uint shift = nodeSlot[node] * 4;
        childMap[nodeChild[node]] = (childMap[nodeChild[node]] & ~(0xFu << shift)) | (slotValue << shift);
    }

    for(uint child = 0; child < 8; ++child)
    {
        const int3 local = block * 2 + ChildOffset(child);

        if(CoarseContains(level, local))
        {
            Raw(hierarchyIndex).Store(level.map + CoarseCellIndex(level, local) * 4, childMap[child]);
        }
    }

    StoreAggregateInfo(coarseIndex, present, overflow);
    Raw(hierarchyIndex).Store4(coarse.boundary + coarseIndex * 16, asuint(float4(boundary[0], boundary[1], boundary[2], boundary[3])));
}

// The fine map's slot for a piece (PressureNoSlot when it is on no coarse level).
// AIR.4E.6: whether a piece is solved (FineSolvable), from its slot's fine-map word.
bool FineMapSolvable(uint fineMapWord, uint piece)
{
    return ((fineMapWord >> (8 + piece)) & 1u) != 0;
}

// AIR.4E.6: which of a slot's pieces are in closed components (bit per piece): present (FineMapSolvable; an absent piece's code is 0, a
// closed id) with a closed id. The compatibility and gauge kernels load masks and rhs or phi only where one is.
uint FineClosedPieces(uint slotIndex, out uint2 codes)
{
    const uint fineMapWord = Raw(fineMapIndex).Load(slotIndex * 4);
    codes = Raw(componentIndex).Load2(slotIndex * 8);
    uint closed = 0;

    [unroll]
    for(uint piece = 0; piece < 2; ++piece)
    {
        if(FineMapSolvable(fineMapWord, piece) && codes[piece] < componentCapacity)
        {
            closed |= 1u << piece;
        }
    }

    return closed;
}

uint FineMapSlot(uint slotIndex, uint piece)
{
    return (Raw(fineMapIndex).Load(slotIndex * 4) >> (piece * 4)) & 0xFu;
}

// A child node's coupling to a node of its +axis neighbour, on the child level (fine when `fine`).
void StoreCouplings(uint coarseIndex, uint axis, float couplings[16])
{
    uint4 words[2];

    [unroll]
    for(uint pair = 0; pair < 8; ++pair)
    {
        const uint packed = f32tof16(couplings[pair * 2]) | (f32tof16(couplings[pair * 2 + 1]) << 16);
        words[pair / 4][pair % 4] = packed;
    }

    const uint address = coarse.weights + coarseIndex * PressureWeightBytes + axis * 32;
    Raw(hierarchyIndex).Store4(address, words[0]);
    Raw(hierarchyIndex).Store4(address + 16, words[1]);
}

// Galerkin couplings of `coarse` from the fine level: for each +axis, the sum of the fine links between the block's high-face cells and
// the neighbouring block's low-face cells, per pair of aggregates (P^T K P with piecewise-constant P). Each coupling is stored once.
[numthreads(4, 4, 4)]
void CSCouplingsFine(uint3 thread : SV_DispatchThreadID)
{
    int3 block;

    if(!BoxBlock(thread, block))
    {
        return;
    }

    const uint coarseIndex = CoarseCellIndex(coarse, block);

    for(uint axis = 0; axis < 3; ++axis)
    {
        float couplings[16];

        for(uint entry = 0; entry < 16; ++entry)
        {
            couplings[entry] = 0.0;
        }

        for(uint child = 0; child < 8; ++child)
        {
            int3 offset = ChildOffset(child);

            if(offset[axis] != 1)
            {
                continue;
            }

            const int3 local = block * 2 + offset;
            int3 next = local;
            next[axis] += 1;

            if(any(next >= int3(field.dimensions)) || any(local >= int3(field.dimensions)))
            {
                continue;
            }

            const uint slotIndex = FineSlotIndex(FineCellOfLocal(local));
            const uint nextSlot = FineSlotIndex(FineCellOfLocal(next));
            const uint2 record = FineRecord(slotIndex);

            for(uint piece = 0; piece < 2; ++piece)
            {
                const uint lower = FineMapSlot(slotIndex, piece);

                for(uint other = 0; other < 2; ++other)
                {
                    const uint upper = FineMapSlot(nextSlot, other);
                    const uint area = AirApertureOf(record, axis, piece, other);

                    if(area != 0 && lower != PressureNoSlot && upper != PressureNoSlot)
                    {
                        couplings[lower * PressureSlots + upper] += float(area) * AirApertureScale;
                    }
                }
            }
        }

        StoreCouplings(coarseIndex, axis, couplings);
    }
}

// The same from a coarse `level` into `coarse`.
[numthreads(4, 4, 4)]
void CSCouplingsCoarse(uint3 thread : SV_DispatchThreadID)
{
    int3 block;

    if(!BoxBlock(thread, block))
    {
        return;
    }

    const uint coarseIndex = CoarseCellIndex(coarse, block);

    for(uint axis = 0; axis < 3; ++axis)
    {
        float couplings[16];

        for(uint entry = 0; entry < 16; ++entry)
        {
            couplings[entry] = 0.0;
        }

        for(uint child = 0; child < 8; ++child)
        {
            int3 offset = ChildOffset(child);

            if(offset[axis] != 1)
            {
                continue;
            }

            const int3 local = block * 2 + offset;
            int3 next = local;
            next[axis] += 1;

            if(!CoarseContains(level, local) || !CoarseContains(level, next))
            {
                continue;
            }

            const uint childIndex = CoarseCellIndex(level, local);
            const uint nextIndex = CoarseCellIndex(level, next);

            for(uint slot = 0; slot < PressureSlots; ++slot)
            {
                const uint lower = CoarseMap(level, childIndex, slot);

                for(uint other = 0; other < PressureSlots; ++other)
                {
                    const uint upper = CoarseMap(level, nextIndex, other);
                    const float weight = CoarseWeight(level, childIndex, axis, slot, other);

                    if(weight > 0.0 && lower != PressureNoSlot && upper != PressureNoSlot)
                    {
                        couplings[lower * PressureSlots + upper] += weight;
                    }
                }
            }
        }

        StoreCouplings(coarseIndex, axis, couplings);
    }
}

// Couplings a coarse node loses because the node across them is on no coarse level (a block past PressureSlots groups): added to the
// diagonal (as if that node held phi = 0), so the coarse operator stays definite wherever the fine one is. `fine`: children are fine.
float DroppedFine(int3 block, uint slot)
{
    float dropped = 0.0;

    for(uint child = 0; child < 8; ++child)
    {
        const int3 local = block * 2 + ChildOffset(child);

        if(any(local >= int3(field.dimensions)))
        {
            continue;
        }

        const uint slotIndex = FineSlotIndex(FineCellOfLocal(local));
        const uint2 record = FineRecord(slotIndex);

        for(uint axis = 0; axis < 3; ++axis)
        {
            // +axis neighbour.
            int3 next = local;
            next[axis] += 1;

            if(all(next < int3(field.dimensions)))
            {
                const uint nextSlot = FineSlotIndex(FineCellOfLocal(next));

                for(uint piece = 0; piece < 2; ++piece)
                {
                    for(uint other = 0; other < 2; ++other)
                    {
                        const uint area = AirApertureOf(record, axis, piece, other);

                        if(area != 0 && FineMapSlot(slotIndex, piece) == slot && FineMapSlot(nextSlot, other) == PressureNoSlot)
                        {
                            dropped += float(area) * AirApertureScale;
                        }
                    }
                }
            }

            // -axis neighbour.
            int3 previous = local;
            previous[axis] -= 1;

            if(all(previous >= 0))
            {
                const uint previousSlot = FineSlotIndex(FineCellOfLocal(previous));
                const uint2 previousRecord = FineRecord(previousSlot);

                for(uint piece = 0; piece < 2; ++piece)
                {
                    for(uint other = 0; other < 2; ++other)
                    {
                        const uint area = AirApertureOf(previousRecord, axis, other, piece);

                        if(area != 0 && FineMapSlot(slotIndex, piece) == slot && FineMapSlot(previousSlot, other) == PressureNoSlot)
                        {
                            dropped += float(area) * AirApertureScale;
                        }
                    }
                }
            }
        }
    }

    return dropped;
}

float DroppedCoarse(int3 block, uint slot)
{
    float dropped = 0.0;

    for(uint child = 0; child < 8; ++child)
    {
        const int3 local = block * 2 + ChildOffset(child);

        if(!CoarseContains(level, local))
        {
            continue;
        }

        const uint childIndex = CoarseCellIndex(level, local);

        for(uint axis = 0; axis < 3; ++axis)
        {
            int3 next = local;
            next[axis] += 1;

            if(CoarseContains(level, next))
            {
                const uint nextIndex = CoarseCellIndex(level, next);

                for(uint lower = 0; lower < PressureSlots; ++lower)
                {
                    for(uint upper = 0; upper < PressureSlots; ++upper)
                    {
                        const float weight = CoarseWeight(level, childIndex, axis, lower, upper);

                        if(weight > 0.0 && CoarseMap(level, childIndex, lower) == slot && CoarseMap(level, nextIndex, upper) == PressureNoSlot)
                        {
                            dropped += weight;
                        }
                    }
                }
            }

            int3 previous = local;
            previous[axis] -= 1;

            if(CoarseContains(level, previous))
            {
                const uint previousIndex = CoarseCellIndex(level, previous);

                for(uint lower = 0; lower < PressureSlots; ++lower)
                {
                    for(uint upper = 0; upper < PressureSlots; ++upper)
                    {
                        const float weight = CoarseWeight(level, previousIndex, axis, lower, upper);

                        if(weight > 0.0 && CoarseMap(level, childIndex, upper) == slot && CoarseMap(level, previousIndex, lower) == PressureNoSlot)
                        {
                            dropped += weight;
                        }
                    }
                }
            }
        }
    }

    return dropped;
}

// The coarse diagonal: the stored (fp16) couplings in all six directions, the aggregate's window-face weight and its dropped couplings,
// so every row sums exactly as a graph Laplacian with boundary terms (zero for a closed pocket's rows). `colour` 0: children are fine.
[numthreads(4, 4, 4)]
void CSDiagonal(uint3 thread : SV_DispatchThreadID)
{
    int3 block;

    if(!BoxBlock(thread, block))
    {
        return;
    }

    const uint coarseIndex = CoarseCellIndex(coarse, block);
    const uint present = CoarsePresent(coarse, coarseIndex);
    float diagonal[PressureSlots] = {0.0, 0.0, 0.0, 0.0};

    for(uint slot = 0; slot < PressureSlots; ++slot)
    {
        if((present & (1u << slot)) == 0)
        {
            continue;
        }

        float sum = CoarseValue(coarse.boundary, coarseIndex, slot);
        sum += colour == 0 ? DroppedFine(block, slot) : DroppedCoarse(block, slot);

        for(uint axis = 0; axis < 3; ++axis)
        {
            int3 next = block;
            next[axis] += 1;
            int3 previous = block;
            previous[axis] -= 1;

            for(uint other = 0; other < PressureSlots; ++other)
            {
                if(CoarseContains(coarse, next))
                {
                    sum += CoarseWeight(coarse, coarseIndex, axis, slot, other);
                }

                if(CoarseContains(coarse, previous))
                {
                    sum += CoarseWeight(coarse, CoarseCellIndex(coarse, previous), axis, other, slot);
                }
            }
        }

        diagonal[slot] = sum;
    }

    Raw(hierarchyIndex).Store4(coarse.diagonal + coarseIndex * 16, asuint(float4(diagonal[0], diagonal[1], diagonal[2], diagonal[3])));
}

// ================================================================================================ solve

// Zeroes the closed entries' sums and gauge magnitudes (volumes kept) and the per-solve maxima.
[numthreads(64, 1, 1)]
void CSClearSums(uint3 thread : SV_DispatchThreadID)
{
    if(thread.x == 0)
    {
        Raw(tableIndex).Store(PressureMaximumRhs, 0);
        Raw(tableIndex).Store(PressureMaximumResidual, 0);
        Raw(tableIndex).Store(PressurePreviousResidual, 0);
        Raw(tableIndex).Store(PressureMaximumExcess, 0);
    }

    if(thread.x < componentCapacity)
    {
        Raw(tableIndex).Store4(EntryAddress(thread.x) + 8, uint4(0, 0, 0, 0));
        // AIR P1/G1: and the gauge's magnitude.
        Raw(tableIndex).Store(EntryAddress(thread.x) + PressureGaugeMagnitude, 0);
    }
}

// The right-hand side, fused with the divergence: b = -outflow (AirDivergence.hlsli's collocated measure, the AIR.4E.2 formulation)
// for every solvable piece, 0 otherwise, and the largest |b|; CSClosedSum then sums each closed component's b. AIR.4F.1b: with
// buoyancy, the vertical faces' velocities include their AirFaceBuoyancy, as CSCorrect's do.
[numthreads(8, 8, 8)]
void CSPrepare(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const int3 cell = AirCellOfSlot(field, slot);
    const uint slotIndex = FineSlotIndex(cell);
    const AirPieces pieces = AirPiecesOf(field, cell);
    const uint2 record = FineRecord(slotIndex);
    AirDivergenceInputs inputs;
    inputs.field = field;
    inputs.apertureIndex = apertureIndex;
    inputs.pressureIndex = AirNoPressure;
    inputs.ambient = ambientVelocity;
    inputs.buoyancy = Buoyancy();
    float2 rhs = float2(0.0, 0.0);
    // AIR P1/C1: each piece's gross magnitude G (0 for pieces not solved, or a non-finite G: no allowance).
    float2 gross = float2(0.0, 0.0);

    for(uint piece = 0; piece < 2; ++piece)
    {
        const uint64_t mask = PieceMask(pieces, piece);
        const uint code = FineComponent(slotIndex, piece);

        if(!FineSolvable(mask, code))
        {
            continue;
        }

        const float3 velocity = AirDivergenceVelocity(field, slotIndex, piece);
        const float temperature = buoyancyImpulse != 0.0 ? AirDivergenceTemperature(field, slotIndex, piece) : 0.0;
        const AirPieceFlux flux = AirPieceFluxOf(inputs, cell, piece, mask, velocity, temperature, 0.0, record);
        float value = -flux.outflow.y;
        float pieceGross = flux.gross;

        // AIR.4F.3 F2b.2: a moving wall takes D of the piece's volume this step, which its open faces must carry away: the solve makes the
        // face outflow (collocated + K phi) equal D's rate, so the right-hand side gains it. A closed component's share is recorded before
        // CSCompatible removes the component's net (physical incompatibility or numerical imbalance, reported, never hidden).
        if(wallSourceIndex != PressureNoWallSource)
        {
            const float source = asfloat(Raw(wallSourceIndex).Load(slotIndex * 8 + piece * 4)) * wallSourceScale;
            value += source;
            pieceGross += abs(source);

            if(source != 0.0 && code < componentCapacity && closedSourceIndex != PressureNoWallSource)
            {
                Raw(closedSourceIndex).InterlockedAdd64(code * ClosedSourceStride, ToFixed(source));
                Raw(closedSourceIndex).InterlockedAdd64(code * ClosedSourceStride + 8, ToFixed(abs(source)));
            }
        }

        rhs[piece] = value;
        gross[piece] = isfinite(pieceGross) ? pieceGross : 0.0;
        Raw(tableIndex).InterlockedMax(PressureMaximumRhs, asuint(abs(value)));
    }

    Raw(rhsIndex).Store2(slotIndex * 8, asuint(rhs));
    // Truncated to their upper 16 bits (sign, exponent and 7 mantissa bits): never above G, so the allowance never grows.
    Raw(rhsGrossIndex).Store(slotIndex * 4, (asuint(gross.x) >> 16) | (asuint(gross.y) & 0xFFFF0000u));
}

// AIR A1 (AirPressureCompatibilityA1.md): the scale of the closed components' right-hand-side sums, relative to the solve's largest
// |b|: the power of two that takes it just below 2^30. Multiplying a float by a power of two is exact, so a term's only error is the
// rounding to the nearest integer (half a quantum, 2^-31 of the largest |b| at most), and a sum of up to 2^21 pieces stays far inside 64
// bits. A fixed scale (AIR.4E.3's 2^20) instead truncates every term at the same absolute quantum: in a settled closed room (terms of a
// few quanta, mixed signs) the discarded fractions accumulated to tens of quanta, the compatibility correction then subtracted that
// artifact from an already compatible right-hand side, and the solve stalled (AirPressureP1Investigation.md). A largest |b| below 2^-97
// (nothing a solve can resolve) takes the largest float scale. AIR P1/G1: the gauge scales each closed component's sum of V phi with it,
// from that component's largest |V phi| (CSGaugeScale).
float ClosedSumScale(float maximumRhs)
{
    const int exponent = int((asuint(maximumRhs) >> 23) & 0xFFu) - 127;
    const int power = clamp(29 - exponent, -126, 127);
    return asfloat(uint(power + 127) << 23);
}

// AIR A1: each closed component's sum of b (CSPrepare's, complete after its barrier) at ClosedSumScale, rounded to nearest; only cells
// holding a closed piece do any work.
[numthreads(8, 8, 8)]
void CSClosedSum(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const int3 cell = AirCellOfSlot(field, slot);
    const uint slotIndex = FineSlotIndex(cell);
    uint2 codes;
    const uint closed = FineClosedPieces(slotIndex, codes);

    if(closed == 0)
    {
        return;
    }

    const float scale = ClosedSumScale(asfloat(Raw(tableIndex).Load(PressureMaximumRhs)));
    const float2 rhs = asfloat(Raw(rhsIndex).Load2(slotIndex * 8));

    for(uint piece = 0; piece < 2; ++piece)
    {
        if((closed & (1u << piece)) != 0)
        {
            Raw(tableIndex).InterlockedAdd64(EntryAddress(codes[piece]) + 8, int64_t(round(rhs[piece] * scale)));
        }
    }
}

// Closed components' compatibility: a closed region cannot hold a net source, so its sum S is removed in proportion to each piece's
// fluid volume (b_i -= V_i S / V), the uniform expansion that a pressure cannot remove.
[numthreads(8, 8, 8)]
void CSCompatible(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const int3 cell = AirCellOfSlot(field, slot);
    const uint slotIndex = FineSlotIndex(cell);
    uint2 codes;
    const uint closed = FineClosedPieces(slotIndex, codes);

    // AIR.4E.6: other cells keep their rhs (no load, no store).
    if(closed == 0)
    {
        return;
    }

    const AirPieces pieces = AirPiecesOf(field, cell);
    float2 rhs = asfloat(Raw(rhsIndex).Load2(slotIndex * 8));
    // AIR A1: CSClosedSum's scale.
    const double scale = double(ClosedSumScale(asfloat(Raw(tableIndex).Load(PressureMaximumRhs))));

    for(uint piece = 0; piece < 2; ++piece)
    {
        const uint64_t mask = PieceMask(pieces, piece);
        const uint code = codes[piece];

        if((closed & (1u << piece)) != 0)
        {
            const uint volume = Raw(tableIndex).Load(EntryAddress(code));
            const float sum = float(double(LoadInt64(Raw(tableIndex), EntryAddress(code) + 8)) / scale);
            rhs[piece] -= float(AirSubcellCount(mask)) * sum / float(volume);
        }
    }

    Raw(rhsIndex).Store2(slotIndex * 8, asuint(rhs));
}

// Red-black Gauss-Seidel on the fine level: the cells of colour `colour` ((x + y + z) mod 2 of the global cell); two pieces of a cell
// never couple, so both update at once. phi = (b + sum of couplings x neighbours' phi) / diagonal.
[numthreads(8, 8, 8)]
void CSSmoothFine(uint3 thread : SV_DispatchThreadID)
{
    const uint3 local = uint3(thread.x * 2, thread.y, thread.z);

    if(any(local >= field.dimensions))
    {
        return;
    }

    int3 cell = FineCellOfLocal(int3(local));
    cell.x += ((cell.x + cell.y + cell.z) & 1) != colour ? 1 : 0;
    const uint slotIndex = FineSlotIndex(cell);
    const AirPieces pieces = FineLowFacePieces(cell);
    const uint2 record = FineRecord(slotIndex);
    const uint fineMapWord = Raw(fineMapIndex).Load(slotIndex * 4);
    float2 phi = asfloat(Raw(phiIndex).Load2(slotIndex * 8));
    const float2 rhs = asfloat(Raw(rhsIndex).Load2(slotIndex * 8));

    for(uint piece = 0; piece < 2; ++piece)
    {
        if(!FineMapSolvable(fineMapWord, piece))
        {
            continue;
        }

        const FineRow row = FineRowOf(cell, piece, PieceMask(pieces, piece), record, phiIndex);

        if(row.diagonal > 0.0)
        {
            phi[piece] = (rhs[piece] + row.neighbours) / row.diagonal;
        }
    }

    Raw(phiIndex).Store2(slotIndex * 8, asuint(phi));
}

// AIR.4E.6: the restrictions run one thread per child (a fine cell, or a cell of `level`) instead of one per parent: each child's
// residual terms go to group memory, then one thread per parent sums its children's terms in the AIR.4E.3 order (child 0 to 7, then
// piece or slot), so every sum is bit-identical to the per-parent loop's. A group's origin is even on every axis, so all eight children
// of each parent are in the parent's group: the group barrier is the only synchronization the sums need.
static const uint RestrictFineGroupCells = 512;
static const uint RestrictCoarseGroupCells = 64;
// Per child: its terms (two per fine cell, PressureSlots per coarse cell), and their targets on the next level, 4 bits each
// (PressureNoSlot: no term).
groupshared float restrictTerms[RestrictFineGroupCells * 2];
groupshared uint restrictTargets[RestrictFineGroupCells];

// A cell of `coarse` after its restriction: its right-hand side, and phi. The restriction leaves `coarse`'s phi at 0, so the first
// pre-smoothing half-sweep (red) reads only black neighbours at 0, and is pointwise: phi = (rhs + 0) / diagonal on red cells, the arithmetic
// of CSSmoothCoarse. AIR.4E.6: with firstSweepFused set, that half-sweep is done here (RecordCycle then skips its dispatch); black cells,
// and every cell when it is not set, keep phi = 0.
void StoreRestricted(int3 block, uint coarseIndex, float residual[PressureSlots])
{
    float4 phi = float4(0.0, 0.0, 0.0, 0.0);

    if(firstSweepFused != 0 && ((block.x + block.y + block.z) & 1) == 0)
    {
        const uint present = CoarsePresent(coarse, coarseIndex);

        [unroll]
        for(uint slot = 0; slot < PressureSlots; ++slot)
        {
            const float diagonal = CoarseValue(coarse.diagonal, coarseIndex, slot);

            if((present & (1u << slot)) != 0 && diagonal > 0.0)
            {
                phi[slot] = (residual[slot] + 0.0) / diagonal;
            }
        }
    }

    Raw(hierarchyIndex).Store4(coarse.rhs + coarseIndex * 16, asuint(float4(residual[0], residual[1], residual[2], residual[3])));
    Raw(hierarchyIndex).Store4(coarse.phi + coarseIndex * 16, asuint(phi));
}

// The fine residual r = b - K phi restricted to level 1 (each aggregate sums its pieces'), and level 1's phi zeroed. One thread per fine
// cell (window-local), in 8 x 8 x 8 groups.
[numthreads(8, 8, 8)]
void CSRestrictFine(uint3 thread : SV_DispatchThreadID, uint3 groupThread : SV_GroupThreadID, uint groupIndex : SV_GroupIndex)
{
    const int3 local = int3(thread);
    float2 terms = float2(0.0, 0.0);
    uint targets = PressureNoSlot | (PressureNoSlot << 4);

    if(all(local < int3(field.dimensions)))
    {
        const int3 cell = FineCellOfLocal(local);
        const uint slotIndex = FineSlotIndex(cell);
        const AirPieces pieces = FineLowFacePieces(cell);
        const uint2 record = FineRecord(slotIndex);
        const float2 phi = asfloat(Raw(phiIndex).Load2(slotIndex * 8));
        const float2 rhs = asfloat(Raw(rhsIndex).Load2(slotIndex * 8));

        for(uint piece = 0; piece < 2; ++piece)
        {
            // Only solvable pieces have an aggregate (CSAggregateFine), so absent pieces have none.
            const uint target = FineMapSlot(slotIndex, piece);

            if(target == PressureNoSlot)
            {
                continue;
            }

            const FineRow row = FineRowOf(cell, piece, PieceMask(pieces, piece), record, phiIndex);
            terms[piece] = rhs[piece] - (row.diagonal * phi[piece] - row.neighbours);
            targets = (targets & ~(0xFu << (piece * 4))) | (target << (piece * 4));
        }
    }

    restrictTerms[groupIndex * 2] = terms.x;
    restrictTerms[groupIndex * 2 + 1] = terms.y;
    restrictTargets[groupIndex] = targets;
    GroupMemoryBarrierWithGroupSync();

    // One thread per parent: the group's even cells.
    const int3 block = local / 2;

    if(any((groupThread & 1) != 0) || any(block >= int3(coarse.dims)))
    {
        return;
    }

    const uint coarseIndex = CoarseCellIndex(coarse, block);
    float residual[PressureSlots] = {0.0, 0.0, 0.0, 0.0};

    for(uint child = 0; child < 8; ++child)
    {
        const uint3 childThread = groupThread + uint3(ChildOffset(child));
        const uint childIndex = childThread.x + 8 * (childThread.y + 8 * childThread.z);
        const uint childTargets = restrictTargets[childIndex];

        for(uint piece = 0; piece < 2; ++piece)
        {
            const uint target = (childTargets >> (piece * 4)) & 0xFu;

            if(target != PressureNoSlot)
            {
                residual[target] += restrictTerms[childIndex * 2 + piece];
            }
        }
    }

    StoreRestricted(block, coarseIndex, residual);
}

// Red-black Gauss-Seidel on coarse `level` (colour by window-local cell parity; a cell's slots never couple).
[numthreads(4, 4, 4)]
void CSSmoothCoarse(uint3 thread : SV_DispatchThreadID)
{
    if(any(thread >= level.dims) || ((thread.x + thread.y + thread.z) & 1) != colour)
    {
        return;
    }

    const int3 local = int3(thread);
    const uint cellIndex = CoarseCellIndex(level, local);
    const uint present = CoarsePresent(level, cellIndex);
    const CoarseNeighbourhood neighbourhood = LoadCoarseNeighbourhood(level, local, cellIndex);

    [unroll]
    for(uint slot = 0; slot < PressureSlots; ++slot)
    {
        const float diagonal = CoarseValue(level.diagonal, cellIndex, slot);

        if((present & (1u << slot)) == 0 || diagonal <= 0.0)
        {
            continue;
        }

        const float value = (CoarseValue(level.rhs, cellIndex, slot) + CoarseNeighbours(neighbourhood, slot)) / diagonal;
        Raw(hierarchyIndex).Store(level.phi + (cellIndex * PressureSlots + slot) * 4, asuint(value));
    }
}

// `level`'s residual restricted to `coarse`, and `coarse`'s phi zeroed. One thread per cell of `level`, in 4 x 4 x 4 groups.
[numthreads(4, 4, 4)]
void CSRestrictCoarse(uint3 thread : SV_DispatchThreadID, uint3 groupThread : SV_GroupThreadID, uint groupIndex : SV_GroupIndex)
{
    const int3 local = int3(thread);
    uint targets = 0xFFFFu;

    if(CoarseContains(level, local))
    {
        const uint childIndex = CoarseCellIndex(level, local);
        const uint present = CoarsePresent(level, childIndex);
        const CoarseNeighbourhood neighbourhood = LoadCoarseNeighbourhood(level, local, childIndex);

        [unroll]
        for(uint slot = 0; slot < PressureSlots; ++slot)
        {
            const uint target = CoarseMap(level, childIndex, slot);
            restrictTerms[groupIndex * PressureSlots + slot] = 0.0;

            if((present & (1u << slot)) == 0 || target == PressureNoSlot)
            {
                continue;
            }

            const float applied = CoarseValue(level.diagonal, childIndex, slot) * CoarseValue(level.phi, childIndex, slot) -
                CoarseNeighbours(neighbourhood, slot);
            restrictTerms[groupIndex * PressureSlots + slot] = CoarseValue(level.rhs, childIndex, slot) - applied;
            targets = (targets & ~(0xFu << (slot * 4))) | (target << (slot * 4));
        }
    }

    restrictTargets[groupIndex] = targets;
    GroupMemoryBarrierWithGroupSync();

    // One thread per parent: the group's even cells.
    const int3 block = local / 2;

    if(any((groupThread & 1) != 0) || !CoarseContains(coarse, block))
    {
        return;
    }

    const uint coarseIndex = CoarseCellIndex(coarse, block);
    float residual[PressureSlots] = {0.0, 0.0, 0.0, 0.0};

    for(uint child = 0; child < 8; ++child)
    {
        const uint3 childThread = groupThread + uint3(ChildOffset(child));
        const uint childIndex = childThread.x + 4 * (childThread.y + 4 * childThread.z);
        const uint childTargets = restrictTargets[childIndex];

        for(uint slot = 0; slot < PressureSlots; ++slot)
        {
            const uint target = (childTargets >> (slot * 4)) & 0xFu;

            if(target != PressureNoSlot)
            {
                residual[target] += restrictTerms[childIndex * PressureSlots + slot];
            }
        }
    }

    StoreRestricted(block, coarseIndex, residual);
}

// The coarsest level in one group (at most 512 cells): `sweeps` red-black Gauss-Seidel sweeps from phi = 0, phi in group memory.
static const uint CoarsestCells = 512;
groupshared float coarsestPhi[CoarsestCells * PressureSlots];

// AIR.4E.6: a coarsest cell's couplings, loaded once before the sweeps instead of in every sweep (they never change during the solve):
// per axis, its own 16 fp16 couplings toward the +axis neighbour (I = this cell's slot, J = the neighbour's) and those of its -axis
// neighbour toward it, 8 words each, as two uint4 per axis. Indices are compile-time (every loop that reads them is unrolled), so they
// stay in registers.
struct CoarsestCouplings
{
    uint4 next[6];
    uint4 previous[6];
    bool hasNext[3];
    bool hasPrevious[3];
};

CoarsestCouplings LoadCoarsestCouplings(int3 local, uint cellIndex, bool inside)
{
    CoarsestCouplings couplings;

    [unroll]
    for(uint axis = 0; axis < 3; ++axis)
    {
        int3 next = local;
        next[axis] += 1;
        int3 previous = local;
        previous[axis] -= 1;
        couplings.hasNext[axis] = inside && CoarseContains(level, next);
        couplings.hasPrevious[axis] = inside && CoarseContains(level, previous);
        const uint own = level.weights + cellIndex * PressureWeightBytes + axis * 32;
        const uint other = couplings.hasPrevious[axis] ? level.weights + CoarseCellIndex(level, previous) * PressureWeightBytes + axis * 32 : own;
        couplings.next[axis * 2] = couplings.hasNext[axis] ? Raw(hierarchyIndex).Load4(own) : uint4(0, 0, 0, 0);
        couplings.next[axis * 2 + 1] = couplings.hasNext[axis] ? Raw(hierarchyIndex).Load4(own + 16) : uint4(0, 0, 0, 0);
        couplings.previous[axis * 2] = couplings.hasPrevious[axis] ? Raw(hierarchyIndex).Load4(other) : uint4(0, 0, 0, 0);
        couplings.previous[axis * 2 + 1] = couplings.hasPrevious[axis] ? Raw(hierarchyIndex).Load4(other + 16) : uint4(0, 0, 0, 0);
    }

    return couplings;
}

// Slot `slot` of a coarsest cell: the sum of its couplings times the neighbours' phi, in the order of CoarseNeighbours.
float SharedNeighbours(CoarsestCouplings couplings, int3 local, uint slot)
{
    float sum = 0.0;

    [unroll]
    for(uint axis = 0; axis < 3; ++axis)
    {
        int3 next = local;
        next[axis] += 1;

        if(couplings.hasNext[axis])
        {
            const uint nextIndex = CoarseCellIndex(level, next);

            [unroll]
            for(uint other = 0; other < PressureSlots; ++other)
            {
                sum += CoarseRowCoupling(couplings.next, axis, slot * PressureSlots + other) * coarsestPhi[nextIndex * PressureSlots + other];
            }
        }

        int3 previous = local;
        previous[axis] -= 1;

        if(couplings.hasPrevious[axis])
        {
            const uint previousIndex = CoarseCellIndex(level, previous);

            [unroll]
            for(uint other = 0; other < PressureSlots; ++other)
            {
                sum += CoarseRowCoupling(couplings.previous, axis, other * PressureSlots + slot) * coarsestPhi[previousIndex * PressureSlots + other];
            }
        }
    }

    return sum;
}

[numthreads(CoarsestCells, 1, 1)]
void CSCoarsest(uint3 thread : SV_GroupThreadID)
{
    const uint cellIndex = thread.x;
    const bool inside = cellIndex < level.cells;
    const int3 local = inside ? LocalOfCoarse(level, cellIndex) : int3(0, 0, 0);
    const uint present = inside ? CoarsePresent(level, cellIndex) : 0;
    const uint parity = uint(local.x + local.y + local.z) & 1;
    const CoarsestCouplings couplings = LoadCoarsestCouplings(local, cellIndex, inside);
    float4 diagonal = float4(0.0, 0.0, 0.0, 0.0);
    float4 rhs = float4(0.0, 0.0, 0.0, 0.0);

    for(uint slot = 0; slot < PressureSlots; ++slot)
    {
        if(inside)
        {
            diagonal[slot] = CoarseValue(level.diagonal, cellIndex, slot);
            rhs[slot] = CoarseValue(level.rhs, cellIndex, slot);
            coarsestPhi[cellIndex * PressureSlots + slot] = 0.0;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    for(uint sweep = 0; sweep < sweeps * 2; ++sweep)
    {
        if(inside && parity == (sweep & 1))
        {
            [unroll]
            for(uint slot = 0; slot < PressureSlots; ++slot)
            {
                if((present & (1u << slot)) != 0 && diagonal[slot] > 0.0)
                {
                    coarsestPhi[cellIndex * PressureSlots + slot] = (rhs[slot] + SharedNeighbours(couplings, local, slot)) / diagonal[slot];
                }
            }
        }

        GroupMemoryBarrierWithGroupSync();
    }

    if(inside)
    {
        for(uint slot = 0; slot < PressureSlots; ++slot)
        {
            Raw(hierarchyIndex).Store(level.phi + (cellIndex * PressureSlots + slot) * 4, asuint(coarsestPhi[cellIndex * PressureSlots + slot]));
        }
    }
}

// `level` += correctionScale x its aggregates' values on `coarse`.
[numthreads(4, 4, 4)]
void CSProlongCoarse(uint3 thread : SV_DispatchThreadID)
{
    if(any(thread >= level.dims))
    {
        return;
    }

    const uint cellIndex = CoarseCellIndex(level, int3(thread));
    const uint present = CoarsePresent(level, cellIndex);
    const uint parentIndex = CoarseCellIndex(coarse, int3(thread) / 2);

    for(uint slot = 0; slot < PressureSlots; ++slot)
    {
        const uint target = CoarseMap(level, cellIndex, slot);

        if((present & (1u << slot)) != 0 && target != PressureNoSlot)
        {
            const uint address = level.phi + (cellIndex * PressureSlots + slot) * 4;
            const float value = asfloat(Raw(hierarchyIndex).Load(address)) + correctionScale * CoarseValue(coarse.phi, parentIndex, target);
            Raw(hierarchyIndex).Store(address, asuint(value));
        }
    }
}

// The fine phi += correctionScale x level 1's values.
[numthreads(8, 8, 8)]
void CSProlongFine(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const int3 cell = AirCellOfSlot(field, slot);
    const uint slotIndex = FineSlotIndex(cell);
    const uint parentIndex = CoarseCellIndex(coarse, (cell - field.minimum) / 2);
    float2 phi = asfloat(Raw(phiIndex).Load2(slotIndex * 8));

    for(uint piece = 0; piece < 2; ++piece)
    {
        const uint target = FineMapSlot(slotIndex, piece);

        if(target != PressureNoSlot)
        {
            phi[piece] += correctionScale * CoarseValue(coarse.phi, parentIndex, target);
        }
    }

    Raw(phiIndex).Store2(slotIndex * 8, asuint(phi));
}

// The gauge of closed components: phi's volume-weighted mean, accumulated (CSGaugeScale, CSGaugeSum) then removed (CSGaugeApply). Open
// components are fixed by their window faces (pressure 0 outside).
//
// AIR P1/G1 (AirRelativeScaleGaugeG1.md): each closed component's sum of V phi is taken at a power-of-two scale relative to that
// component's own largest finite |V phi| (CSGaugeScale; ClosedSumScale, A1's scale), rounded to nearest. A fixed scale (AIR.4E.3's 2^20,
// truncated) dropped every term once V phi fell below one quantum: a settled closed room then kept a constant phi offset forever (V-
// cycles never change a closed component's constant), and as the air came to rest its fp32 rounding set a residual floor above the
// convergence criterion (AirStationaryBuoyancyConvergenceP1.md). Relative to the component's own magnitude, a term's error is at most
// half a quantum (2^-31 of the largest term), whatever the pressure's size or sign, and a component of up to 2^21 pieces sums inside 64
// bits; one component's scale never depends on another's pressure. Non-finite pieces are left out of the mean (they keep their value,
// and the residual's non-finite check reports them).

// The largest finite |V phi| of each closed component, as uint bits in its entry (non-negative floats order as their bits).
[numthreads(8, 8, 8)]
void CSGaugeScale(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const int3 cell = AirCellOfSlot(field, slot);
    const uint slotIndex = FineSlotIndex(cell);
    uint2 codes;
    const uint closed = FineClosedPieces(slotIndex, codes);

    if(closed == 0)
    {
        return;
    }

    const AirPieces pieces = AirPiecesOf(field, cell);
    const float2 phi = asfloat(Raw(phiIndex).Load2(slotIndex * 8));

    for(uint piece = 0; piece < 2; ++piece)
    {
        const float weighted = abs(float(AirSubcellCount(PieceMask(pieces, piece))) * phi[piece]);

        if((closed & (1u << piece)) != 0 && isfinite(weighted))
        {
            Raw(tableIndex).InterlockedMax(EntryAddress(codes[piece]) + PressureGaugeMagnitude, asuint(weighted));
        }
    }
}

// The scale of a closed component's gauge sum (CSGaugeScale's magnitude, complete after its barrier).
float GaugeScale(uint code)
{
    return ClosedSumScale(asfloat(Raw(tableIndex).Load(EntryAddress(code) + PressureGaugeMagnitude)));
}

[numthreads(8, 8, 8)]
void CSGaugeSum(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const int3 cell = AirCellOfSlot(field, slot);
    const uint slotIndex = FineSlotIndex(cell);
    uint2 codes;
    const uint closed = FineClosedPieces(slotIndex, codes);

    if(closed == 0)
    {
        return;
    }

    const AirPieces pieces = AirPiecesOf(field, cell);
    const float2 phi = asfloat(Raw(phiIndex).Load2(slotIndex * 8));

    for(uint piece = 0; piece < 2; ++piece)
    {
        const uint code = codes[piece];
        const float weighted = float(AirSubcellCount(PieceMask(pieces, piece))) * phi[piece];

        if((closed & (1u << piece)) != 0 && isfinite(weighted))
        {
            // Multiplying by a power of two is exact: the rounding is the term's only error.
            Raw(tableIndex).InterlockedAdd64(EntryAddress(code) + 16, int64_t(round(weighted * GaugeScale(code))));
        }
    }
}

[numthreads(8, 8, 8)]
void CSGaugeApply(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const int3 cell = AirCellOfSlot(field, slot);
    const uint slotIndex = FineSlotIndex(cell);
    uint2 codes;
    const uint closed = FineClosedPieces(slotIndex, codes);

    // AIR.4E.6: other cells keep their phi (no load, no store).
    if(closed == 0)
    {
        return;
    }

    float2 phi = asfloat(Raw(phiIndex).Load2(slotIndex * 8));

    for(uint piece = 0; piece < 2; ++piece)
    {
        const uint code = codes[piece];

        if((closed & (1u << piece)) != 0)
        {
            const double volume = double(Raw(tableIndex).Load(EntryAddress(code)));
            const double sum = double(LoadInt64(Raw(tableIndex), EntryAddress(code) + 16));
            phi[piece] -= float(sum / (double(GaugeScale(code)) * volume));
        }
    }

    Raw(phiIndex).Store2(slotIndex * 8, asuint(phi));
}

// The fine residual r = b - K phi into scratch (0 for pieces not solved), and its largest magnitude into the header. AIR P1/C1: also the
// largest excess of |r| over each piece's right-hand-side rounding allowance (PressureRhsRoundingUnits x 2^-24 x G), the recovery gate's
// measure; a non-finite residual is its own excess, so the gate's finiteness check still sees it.
[numthreads(8, 8, 8)]
void CSResidual(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const int3 cell = AirCellOfSlot(field, slot);
    const uint slotIndex = FineSlotIndex(cell);
    const AirPieces pieces = AirPiecesOf(field, cell);
    const uint2 record = FineRecord(slotIndex);
    const float2 phi = asfloat(Raw(phiIndex).Load2(slotIndex * 8));
    const float2 rhs = asfloat(Raw(rhsIndex).Load2(slotIndex * 8));
    const uint grossWord = Raw(rhsGrossIndex).Load(slotIndex * 4);
    const float2 gross = float2(asfloat(grossWord << 16), asfloat(grossWord & 0xFFFF0000u));
    float2 residual = float2(0.0, 0.0);

    for(uint piece = 0; piece < 2; ++piece)
    {
        const uint64_t mask = PieceMask(pieces, piece);

        if(!FineSolvable(mask, FineComponent(slotIndex, piece)))
        {
            continue;
        }

        const FineRow row = FineRowOf(cell, piece, mask, record, phiIndex);
        residual[piece] = rhs[piece] - (row.diagonal * phi[piece] - row.neighbours);
        const float magnitude = abs(residual[piece]);
        const float allowance = PressureRhsRoundingUnits * PressureUnitRoundoff * gross[piece];
        const float excess = isfinite(magnitude) ? max(magnitude - allowance, 0.0) : magnitude;
        Raw(tableIndex).InterlockedMax(PressureMaximumResidual, asuint(magnitude));
        Raw(tableIndex).InterlockedMax(PressureMaximumExcess, asuint(excess));
    }

    Raw(scratchIndex).Store2(slotIndex * 8, asuint(residual));
}

// A cold start: phi = 0 everywhere.
[numthreads(8, 8, 8)]
void CSZeroPhi(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    Raw(phiIndex).Store2(FineSlotIndex(AirCellOfSlot(field, slot)) * 8, uint2(0, 0));
}

// ================================================================================================ correction (AIR.4E.4)

// The pressure correction of one piece's stored velocity, AIR.4E.0's (collocated, approximate) projection: along each axis, the
// velocity changes by the area-weighted mean of its faces' corrections, where a face's correction is phi(+axis side) - phi(-axis side)
// (phi = 0 outside the window). Then the step's wall constraint again (AirSolver.hlsl): a component pointing at a face the piece shares
// no open sub-face across is removed. A face is open when it has an aperture toward some piece of the neighbour, or lies on the
// window's edge: exactly AirFaceOpen's test, read from the face connections. Only xyz is written: w (the temperature) is untouched.
//
// AIR.4F.1b: with buoyancy, a vertical face's term is phi(+y side) - phi(-y side) minus the face's AirFaceBuoyancy, the same force
// CSPrepare added to that face's velocity, from the same two pieces' ΔT. ΔT is read through the writable views this kernel stores xyz
// through; no thread writes w, so every thread reads the step's values.

// A piece's ΔT, through the velocity buffers' writable views.
float CorrectTemperature(uint slotIndex, uint piece)
{
    return asfloat(Raw(piece == 0 ? velocityWriteIndex : piece1WriteIndex).Load(slotIndex * 16 + 12));
}

// A face's correction (phi(+axis side) - phi(-axis side)) less its buoyancy on vertical faces. A zero force leaves the correction's bits.
float CorrectFace(bool buoyant, float correction, float lowerTemperature, float upperTemperature)
{
    if(!buoyant)
    {
        return correction;
    }

    const float force = AirFaceBuoyancy(Buoyancy(), lowerTemperature, upperTemperature);
    return force != 0.0 ? correction - force : correction;
}

[numthreads(8, 8, 8)]
void CSCorrect(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const int3 cell = AirCellOfSlot(field, slot);
    const uint slotIndex = FineSlotIndex(cell);
    const AirPieces pieces = AirPiecesOf(field, cell);
    const uint2 record = FineRecord(slotIndex);
    const float2 phi = asfloat(Raw(phiIndex).Load2(slotIndex * 8));

    for(uint piece = 0; piece < 2; ++piece)
    {
        const uint64_t mask = PieceMask(pieces, piece);

        if(!FineSolvable(mask, FineComponent(slotIndex, piece)))
        {
            continue;
        }

        const uint velocityIndex = piece == 0 ? velocityWriteIndex : piece1WriteIndex;
        float3 velocity = asfloat(Raw(velocityIndex).Load3(slotIndex * 16));
        const bool buoyancy = buoyancyImpulse != 0.0;
        const float temperature = buoyancy ? CorrectTemperature(slotIndex, piece) : 0.0;

        [unroll]
        for(uint axis = 0; axis < 3; ++axis)
        {
            const bool buoyant = buoyancy && axis == 1;
            float weighted = 0.0;
            uint area = 0;
            bool highOpen = false;
            bool lowOpen = false;

            // +axis faces: this piece is the lower side.
            if(AirHighBoundary(record, axis))
            {
                const uint aperture = AirApertureOf(record, axis, piece, 0);
                weighted += float(aperture) * CorrectFace(buoyant, -phi[piece], temperature, 0.0);
                area += aperture;
                highOpen = true;
            }
            else
            {
                int3 next = cell;
                next[axis] += 1;
                const uint nextSlot = FineSlotIndex(next);

                [unroll]
                for(uint other = 0; other < 2; ++other)
                {
                    const uint aperture = AirApertureOf(record, axis, piece, other);

                    if(aperture != 0)
                    {
                        const float upperTemperature = buoyant ? CorrectTemperature(nextSlot, other) : 0.0;
                        weighted += float(aperture) *
                            CorrectFace(buoyant, FineValue(phiIndex, nextSlot, other) - phi[piece], temperature, upperTemperature);
                        area += aperture;
                        highOpen = true;
                    }
                }
            }

            // -axis faces: this piece is the upper side.
            int3 previous = cell;
            previous[axis] -= 1;

            if(!AirContains(field, previous))
            {
                const uint aperture = AirLowBoundaryAperture(mask, axis);
                weighted += float(aperture) * CorrectFace(buoyant, phi[piece], 0.0, temperature);
                area += aperture;
                lowOpen = true;
            }
            else
            {
                const uint previousSlot = FineSlotIndex(previous);
                const uint2 previousRecord = FineRecord(previousSlot);

                [unroll]
                for(uint other = 0; other < 2; ++other)
                {
                    const uint aperture = AirApertureOf(previousRecord, axis, other, piece);

                    if(aperture != 0)
                    {
                        const float lowerTemperature = buoyant ? CorrectTemperature(previousSlot, other) : 0.0;
                        weighted += float(aperture) *
                            CorrectFace(buoyant, phi[piece] - FineValue(phiIndex, previousSlot, other), lowerTemperature, temperature);
                        area += aperture;
                        lowOpen = true;
                    }
                }
            }

            if(area != 0)
            {
                velocity[axis] -= weighted / float(area);
            }

            // The wall constraint.
            const bool open = velocity[axis] > 0.0 ? highOpen : lowOpen;

            if(velocity[axis] != 0.0 && !open)
            {
                velocity[axis] = 0.0;
            }
        }

        Raw(velocityIndex).Store3(slotIndex * 16, asuint(velocity));
    }
}

// ================================================================================================ AIR.4F.3 F1a.3: local topology

groupshared uint g_topologyLabels[TopologyNodeCapacity];
groupshared uint g_topologyChanged;
groupshared uint g_topologyCodeLow;
groupshared uint g_topologyCodeHigh;
groupshared uint g_topologyLabelLow;
groupshared uint g_topologyLabelHigh;
groupshared uint g_topologyShellPieces;
groupshared uint g_topologyPresent;
groupshared uint g_topologyWindowPieces;
// AIR A2/T2a: per code of the box (the lowest and the highest present), the lowest and highest label (root) of its pieces and its outer-
// layer pieces; whether a label holds pieces of different codes, and whether a third code is present.
groupshared uint g_topologyRootLow[2];
groupshared uint g_topologyRootHigh[2];
groupshared uint g_topologyCodeShell[2];
groupshared uint g_topologyMixed;
groupshared uint g_topologyThirdCode;
groupshared uint g_topologySettled;

uint3 TopologyLocal(uint cell)
{
    const uint3 size = uint3(boxSize);
    return uint3(cell % size.x, (cell / size.x) % size.y, cell / (size.x * size.y));
}

// Whether the moving-solid change inside the box (window-local fine cells: the overlay's dirty cells grown by one, clipped to the window)
// can leave the components as they were. True only when
//   - every piece of the box now, and every piece the overlay replaced, holds one code c (one component, not isolated or overflowed);
//   - the box's pieces are all connected to one another through links inside the box;
//   - its outer layer holds a piece;
//   - and c's grounding (open or closed) cannot change (below).
// Then no component split, merged or appeared: every piece outside the box reaches the box (where its old component did) through the
// same unchanged links, the box is one connected set, and no other component touches it. Anything else is reported as a possible
// change and relabelled (false positives cost a relabel; there are no false negatives).
//
// AIR A2/T2a (AirConnectivityRelabelT2a.md): the box may also hold a second code d, the case of a solid moving beside a thin wall or
// floor whose other side is another component (the box's one-cell margin reaches its pieces). Then, in addition, d's pieces in the box
// are connected inside the box and reach its outer layer, no label holds pieces of both codes (no link inside the box joins them), every
// replaced piece and every new piece of a rewritten cell holds one code r (so the volume change is r's), no window face is involved,
// and the labelling settled. The overlay's rewritten cells are inside the box's margin, so links into the box from outside are
// unchanged; the other code's pieces are all in cells the overlay did not rewrite or pieces it kept exactly (AIR A2/T2b), whose links
// to one another are unchanged; so that component is unchanged, and r's is as in the one-code case. A box of one code is tested by the
// one-code conditions alone.
[numthreads(256, 1, 1)]
void CSTopologyCheck(uint3 thread : SV_GroupThreadID)
{
    const uint cells = uint(boxSize.x * boxSize.y * boxSize.z);
    const uint nodes = cells * 2;

    if(thread.x == 0)
    {
        g_topologyChanged = 0;
        g_topologyCodeLow = 0xFFFFFFFFu;
        g_topologyCodeHigh = 0;
        g_topologyLabelLow = 0xFFFFFFFFu;
        g_topologyLabelHigh = 0;
        g_topologyShellPieces = 0;
        g_topologyPresent = 0;
        g_topologyWindowPieces = 0;
        g_topologyRootLow[0] = 0xFFFFFFFFu;
        g_topologyRootLow[1] = 0xFFFFFFFFu;
        g_topologyRootHigh[0] = 0;
        g_topologyRootHigh[1] = 0;
        g_topologyCodeShell[0] = 0;
        g_topologyCodeShell[1] = 0;
        g_topologyMixed = 0;
        g_topologyThirdCode = 0;
        g_topologySettled = 0;
    }

    GroupMemoryBarrierWithGroupSync();

    const bool fits = nodes <= TopologyNodeCapacity;

    // Every present piece starts as its own label; codes and the outer layer are gathered.
    for(uint node = thread.x; fits && node < nodes; node += 256)
    {
        const uint3 local = TopologyLocal(node / 2);
        const int3 cell = FineCellOfLocal(boxMinimum + int3(local));
        const AirPieces pieces = AirPiecesOf(field, cell);
        const uint piece = node & 1u;

        if(PieceMask(pieces, piece) == 0)
        {
            g_topologyLabels[node] = PressureAbsent;
            continue;
        }

        g_topologyLabels[node] = node;
        const uint code = FineComponent(FineSlotIndex(cell), piece);
        InterlockedMin(g_topologyCodeLow, code);
        InterlockedMax(g_topologyCodeHigh, code);
        InterlockedAdd(g_topologyPresent, 1);
        const bool outer = any(local == 0) || any(local == uint3(boxSize) - 1);

        if(outer)
        {
            InterlockedAdd(g_topologyShellPieces, 1);
        }

        // A piece with a window face (open to the outside, pressure 0) grounds its component.
        const int3 windowLocal = boxMinimum + int3(local);

        if(any(windowLocal == 0) || any(windowLocal == int3(field.dimensions) - 1))
        {
            const uint slotIndex = FineSlotIndex(cell);
            const FineRow row = FineRowOf(cell, piece, PieceMask(pieces, piece), FineRecord(slotIndex), phiIndex);

            if(row.boundary > 0.0)
            {
                InterlockedAdd(g_topologyWindowPieces, 1);
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    // Labels: the smallest node of each component inside the box, by propagation along links inside the box, then pointer jumping.
    [loop]
    for(uint round = 0; fits && round < 512; ++round)
    {
        for(uint node = thread.x; node < nodes; node += 256)
        {
            if(g_topologyLabels[node] == PressureAbsent)
            {
                continue;
            }

            const uint3 local = TopologyLocal(node / 2);
            const int3 cell = FineCellOfLocal(boxMinimum + int3(local));
            const uint2 record = FineRecord(FineSlotIndex(cell));
            const uint piece = node & 1u;

            for(uint axis = 0; axis < 3; ++axis)
            {
                uint3 next = local;
                next[axis] += 1;

                if(next[axis] >= uint(boxSize[axis]))
                {
                    continue;
                }

                const uint nextCell = next.x + uint(boxSize.x) * (next.y + uint(boxSize.y) * next.z);

                for(uint other = 0; other < 2; ++other)
                {
                    const uint neighbour = nextCell * 2 + other;

                    if(AirApertureOf(record, axis, piece, other) == 0 || g_topologyLabels[neighbour] == PressureAbsent)
                    {
                        continue;
                    }

                    const uint own = g_topologyLabels[node];
                    const uint theirs = g_topologyLabels[neighbour];

                    if(own != theirs)
                    {
                        const uint lowest = min(own, theirs);
                        uint ignored;
                        InterlockedMin(g_topologyLabels[node], lowest, ignored);
                        InterlockedMin(g_topologyLabels[neighbour], lowest, ignored);
                        g_topologyChanged = 1;
                    }
                }
            }
        }

        GroupMemoryBarrierWithGroupSync();

        for(uint jumped = thread.x; jumped < nodes; jumped += 256)
        {
            const uint label = g_topologyLabels[jumped];

            if(label != PressureAbsent)
            {
                g_topologyLabels[jumped] = g_topologyLabels[label];
            }
        }

        GroupMemoryBarrierWithGroupSync();
        const bool settled = g_topologyChanged == 0;
        GroupMemoryBarrierWithGroupSync();

        if(settled)
        {
            if(thread.x == 0)
            {
                g_topologySettled = 1;
            }

            break;
        }

        if(thread.x == 0)
        {
            g_topologyChanged = 0;
        }

        GroupMemoryBarrierWithGroupSync();
    }

    for(uint node = thread.x; fits && node < nodes; node += 256)
    {
        const uint label = g_topologyLabels[node];

        if(label != PressureAbsent)
        {
            InterlockedMin(g_topologyLabelLow, label);
            InterlockedMax(g_topologyLabelHigh, label);

            // AIR A2/T2a: the piece's code against its label's (the root piece's) code, and its code's labels and outer layer.
            const uint3 local = TopologyLocal(node / 2);
            const int3 cell = FineCellOfLocal(boxMinimum + int3(local));
            const uint code = FineComponent(FineSlotIndex(cell), node & 1u);
            const int3 rootCell = FineCellOfLocal(boxMinimum + int3(TopologyLocal(label / 2)));
            const uint rootCode = FineComponent(FineSlotIndex(rootCell), label & 1u);

            if(code != rootCode)
            {
                g_topologyMixed = 1;
            }

            if(code != g_topologyCodeLow && code != g_topologyCodeHigh)
            {
                g_topologyThirdCode = 1;
            }

            const uint which = code == g_topologyCodeLow ? 0u : 1u;
            InterlockedMin(g_topologyRootLow[which], label);
            InterlockedMax(g_topologyRootHigh[which], label);

            if(any(local == 0) || any(local == uint3(boxSize) - 1))
            {
                InterlockedAdd(g_topologyCodeShell[which], 1);
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if(thread.x != 0)
    {
        return;
    }

    RWByteAddressBuffer topology = Raw(topologyIndex);
    const uint replacedLow = topology.Load(TopologyCodeLow);
    const uint replacedHigh = topology.Load(TopologyCodeHigh);
    const uint code = g_topologyCodeLow;
    const bool oneCode = g_topologyPresent > 0 && g_topologyCodeLow == g_topologyCodeHigh && code != PressureIsolated &&
        code != PressureOverflow;
    // The overlay saw no piece to replace (low above high), or only pieces of the same code.
    const bool replacedSame = replacedLow > replacedHigh || (replacedLow == code && replacedHigh == code);
    const bool connected = g_topologyLabelLow == g_topologyLabelHigh;
    // Grounding (whether c reaches a window face, pressure 0 outside): unchanged when no piece of the box has a window face now and none
    // the overlay replaced had one (window faces elsewhere are unchanged). Otherwise an open component stays open only if a piece of the
    // box still has a window face, and a closed one cannot gain one.
    const bool windowUntouched = g_topologyWindowPieces == 0 && topology.Load(TopologyReplacedWindow) == 0;
    const bool grounding = windowUntouched || (code == PressureOpen && g_topologyWindowPieces > 0);
    const bool oneCodeOk = fits && oneCode && replacedSame && connected && g_topologyShellPieces > 0 && grounding;
    // AIR A2/T2a: two codes (above). Neither isolated nor overflowed, no third, no label of both, each connected and on the outer layer,
    // exactly one replaced code among them, the labelling settled, and no window face involved.
    const uint second = g_topologyCodeHigh;
    const bool twoCodes = g_topologyPresent > 0 && g_topologyCodeLow != g_topologyCodeHigh && code != PressureIsolated &&
        code != PressureOverflow && second != PressureIsolated && second != PressureOverflow && g_topologyThirdCode == 0 &&
        g_topologyMixed == 0;
    const bool eachConnected = g_topologyRootLow[0] == g_topologyRootHigh[0] && g_topologyRootLow[1] == g_topologyRootHigh[1];
    const bool eachShell = g_topologyCodeShell[0] > 0 && g_topologyCodeShell[1] > 0;
    // Every rewritten cell's pieces, before and after, read one code r of the two, so the volume change (attributed to the replaced code
    // by CSTopologyArguments) is r's alone.
    const uint currentLow = topology.Load(TopologyCurrentLow);
    const uint currentHigh = topology.Load(TopologyCurrentHigh);
    const bool replacedOne = replacedLow == replacedHigh && (replacedLow == code || replacedLow == second);
    const bool currentOne = currentLow > currentHigh || (currentLow == replacedLow && currentHigh == replacedLow);
    const bool settled = g_topologySettled != 0;
    const bool twoCodeOk = fits && settled && twoCodes && eachConnected && eachShell && replacedOne && currentOne && windowUntouched;
    // AIR A2/T2b: no cell's pieces changed (none replaced, none new; kept pieces apart, see RecordTopology), so no aperture changed:
    // the components are as they were, whatever the box holds.
    const bool nothingRewritten = replacedLow > replacedHigh && currentLow > currentHigh;
    const bool ok = oneCodeOk || twoCodeOk || nothingRewritten;
    // The reasons: the one-code test's, or with two codes the two-code test's (replaced, connected, outer layer and grounding per code,
    // then 64 for new pieces of another code and 128 for unsettled labels).
    const uint oneCodeFailure = (fits ? 0u : 1u) | (oneCode ? 0u : 2u) | (replacedSame ? 0u : 4u) | (connected ? 0u : 8u) |
        (g_topologyShellPieces > 0 ? 0u : 16u) | (grounding ? 0u : 32u);
    const uint twoCodeFailure = (fits ? 0u : 1u) | (replacedOne ? 0u : 4u) | (eachConnected ? 0u : 8u) | (eachShell ? 0u : 16u) |
        (windowUntouched ? 0u : 32u) | (currentOne ? 0u : 64u) | (settled ? 0u : 128u);
    const uint failure = twoCodes ? twoCodeFailure : oneCodeFailure;
    // AIR A2/T2b: one code: c holds every piece of the box, the rewritten cells' included, even when none was replaced (cells that were
    // fully solid); two codes: r.
    const uint accepted = oneCodeOk ? code : (twoCodeOk ? replacedLow : PressureAbsent);
    topology.Store(TopologyAcceptedCode, accepted);
    topology.Store(TopologyLocalOk, ok ? 1u : 0u);
    topology.Store(TopologyFailure, failure);
    topology.Store(TopologySampleCodeLow, g_topologyCodeLow);
    topology.Store(TopologySampleCodeHigh, g_topologyCodeHigh);
    topology.Store(TopologySampleCells, cells);
    topology.Store(TopologySamplePieces, g_topologyPresent);
}

// After the check: whether to relabel (forced by the caller when `colour` is set, or the check failed), as the predicate the relabel is
// recorded under, and without a relabel the closed component's volume change.
[numthreads(1, 1, 1)]
void CSTopologyArguments(uint3 thread : SV_DispatchThreadID)
{
    RWByteAddressBuffer topology = Raw(topologyIndex);
    const bool relabel = colour != 0 || topology.Load(TopologyLocalOk) == 0;

    if(!relabel)
    {
        // A closed component whose fluid volume the solids changed: the table's volume follows (the delta wraps as two's complement).
        // AIR A2/T2b: the accepted component's, not the replaced pieces' (none when the solids only exposed fully solid cells).
        const uint code = topology.Load(TopologyAcceptedCode);

        if(code < componentCapacity)
        {
            uint ignored;
            Raw(tableIndex).InterlockedAdd(EntryAddress(code), topology.Load(TopologyVolumeDelta), ignored);
        }
    }

    topology.Store(TopologyRelabel, relabel ? 1u : 0u);
    // F1a.3 closeout: the predicate the relabel's passes are recorded under (skipped when zero).
    Raw(predicateIndex).Store2(0, uint2(relabel ? 1u : 0u, 0));
    topology.Store(TopologyChecks, topology.Load(TopologyChecks) + 1);
    topology.Store(TopologyRelabels, topology.Load(TopologyRelabels) + (relabel ? 1u : 0u));

    // AIR A2/T2a: why (a forced relabel's check result is not counted: the caller decided before it).
    if(colour != 0)
    {
        topology.Store(TopologyReasonForced, topology.Load(TopologyReasonForced) + 1);
    }
    else if(relabel)
    {
        const uint failure = topology.Load(TopologyFailure);
        topology.Store(TopologyReasonSize, topology.Load(TopologyReasonSize) + ((failure & 1u) != 0 ? 1u : 0u));
        topology.Store(TopologyReasonCodes, topology.Load(TopologyReasonCodes) + ((failure & 2u) != 0 ? 1u : 0u));
        topology.Store(TopologyReasonReplaced, topology.Load(TopologyReasonReplaced) + ((failure & 4u) != 0 ? 1u : 0u));
        topology.Store(TopologyReasonDisconnected, topology.Load(TopologyReasonDisconnected) + ((failure & 8u) != 0 ? 1u : 0u));
        topology.Store(TopologyReasonShell, topology.Load(TopologyReasonShell) + ((failure & 16u) != 0 ? 1u : 0u));
        topology.Store(TopologyReasonGrounding, topology.Load(TopologyReasonGrounding) + ((failure & 32u) != 0 ? 1u : 0u));
        topology.Store(TopologyReasonCurrent, topology.Load(TopologyReasonCurrent) + ((failure & 64u) != 0 ? 1u : 0u));
        topology.Store(TopologyReasonUnsettled, topology.Load(TopologyReasonUnsettled) + ((failure & 128u) != 0 ? 1u : 0u));

        // AIR A2/T2b (diagnostics): the class (above).
        const bool empty = topology.Load(TopologyCodeLow) > topology.Load(TopologyCodeHigh) &&
            topology.Load(TopologyCurrentLow) > topology.Load(TopologyCurrentHigh);
        const bool newSlots = topology.Load(TopologyUpdateNewSlots) > 0;
        const uint classWord = empty ? TopologyClassEmpty : (newSlots ? TopologyClassNewSlots : TopologyClassOther);
        topology.Store(classWord, topology.Load(classWord) + 1);
    }
}

// ================================================================================================ AIR.4F.3 F1a.3: bounded recovery

// Before recovery cycle `recoveryStep` (of `sweeps`): whether the latest residual (CSResidual's maximum) is above the criterion, and if
// so the cycle's indirect arguments (every shape a V-cycle dispatches: fine slots, fine colour, single, then each coarse level's cells);
// zero arguments otherwise, so the remaining recovery records nothing. The residual maximum is reset for the next measurement; the
// right-hand side's (CSPrepare's) is kept. The last gate (recoveryStep == sweeps) only counts a solve still above the criterion. AIR A1:
// from the second recovery cycle on, a cycle also needs the previous one to have reduced the residual (PressureStagnationRatio); a
// solve whose residual stopped falling ends there, counted as a stagnation and a failure.
[numthreads(1, 1, 1)]
void CSRecoveryGate(uint3 thread : SV_DispatchThreadID)
{
    RWByteAddressBuffer table = Raw(tableIndex);
    const float maximumRhs = asfloat(table.Load(PressureMaximumRhs));
    const float maximumResidual = asfloat(table.Load(PressureMaximumResidual));
    const float maximumExcess = asfloat(table.Load(PressureMaximumExcess));
    // AIR O2.1: converged only when the residual is finite and within the criterion; a non-finite residual or right-hand side never
    // passes (it runs the remaining budget, then counts as a failure). A zero right-hand side has nothing to solve. AIR P1/C1: the
    // criterion applies to each piece's residual beyond its right-hand-side rounding allowance (CSResidual), never larger than the
    // residual itself; the stagnation guard keeps the raw residual.
    const bool finite = !isnan(maximumRhs) && !isinf(maximumRhs) && !isnan(maximumResidual) && !isinf(maximumResidual) &&
        !isnan(maximumExcess) && !isinf(maximumExcess);
    const float criterion = PressureRecoveryCriterion * maximumRhs;
    const bool converged = maximumRhs == 0.0 || (finite && maximumExcess <= criterion);
    const bool roundingConverged = converged && maximumRhs != 0.0 && maximumResidual > criterion;
    const bool above = !converged;
    const float previousResidual = asfloat(table.Load(PressurePreviousResidual));
    const bool stagnated = above && finite && recoveryStep > 0 && previousResidual > 0.0 &&
        maximumResidual > PressureStagnationRatio * previousResidual;
    const bool active = above && !stagnated && recoveryStep < sweeps;
    RWByteAddressBuffer topology = Raw(topologyIndex);

    if(roundingConverged)
    {
        topology.Store(TopologyRhsRoundingConverged, topology.Load(TopologyRhsRoundingConverged) + 1);
    }

    if(active)
    {
        topology.Store(TopologyRecoveryCycles, topology.Load(TopologyRecoveryCycles) + 1);

        if(recoveryStep >= budgetedRecoveryCycles)
        {
            topology.Store(TopologyRecoveryBeyondBudget, topology.Load(TopologyRecoveryBeyondBudget) + 1);
        }
    }
    else if(above)
    {
        topology.Store(TopologyRecoveryFailures, topology.Load(TopologyRecoveryFailures) + 1);

        if(stagnated)
        {
            topology.Store(TopologyRecoveryStagnations, topology.Load(TopologyRecoveryStagnations) + 1);
        }
    }

    // A stopped solve's later gates see residual 0 (no cycle ran) and count nothing more: the previous residual is cleared with it.
    table.Store(PressurePreviousResidual, active ? asuint(maximumResidual) : 0u);
    table.Store(PressureMaximumResidual, 0);
    table.Store(PressureMaximumExcess, 0);

    RWByteAddressBuffer arguments = Raw(gateArgumentsIndex);
    const uint3 fineSlots = active ? (field.dimensions + 7) / 8 : uint3(0, 0, 0);
    const uint3 fineColour = active ? uint3((field.dimensions.x / 2 + 7) / 8, (field.dimensions.y + 7) / 8, (field.dimensions.z + 7) / 8) :
        uint3(0, 0, 0);
    arguments.Store3(0, fineSlots);
    arguments.Store3(12, fineColour);
    arguments.Store3(24, active ? uint3(1, 1, 1) : uint3(0, 0, 0));
    uint3 dims = field.dimensions;

    for(uint index = 0; index < levelCount; ++index)
    {
        dims = (dims + 1) / 2;
        arguments.Store3(36 + index * 12, active ? (dims + 3) / 4 : uint3(0, 0, 0));
    }
}
