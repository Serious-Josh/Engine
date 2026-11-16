// AIR.4F.3 F1a: the local moving-solid geometry overlay. For the cells of a list of dirty boxes, each cell's pieces are rebuilt from
// its static free sub-cells (VX.5 residency, exactly as maintenance builds them) minus the sub-cells any moving solid overlaps; then
// the face connections of every slot whose record can depend on those cells. Velocity, temperature and mist are left as they are
// (F1a is geometry only).
//
// AIR.4F.3 F1a.2: the same pass also
//   - sets the cell's obstacle bits: its static bits, plus blocked when a solid removes any free sub-cell. A clear blocked bit means
//     "one whole free piece" to every path consumer (AirPath.hlsli, AirLoadCell, AirCellIsOpen), so a carved cell must carry it;
//   - carries phi onto the rebuilt pieces when the pressure solver keeps it (each new piece takes the phi of the old piece it shares
//     most sub-cells with; a piece sharing none starts at 0), so a warm start never reads a phi that belonged to another piece;
//   - counts the free sub-cells the two-piece representation drops (diagnostics).
//
// AIR.4F.3 F1a.3: when the pressure solver will update its hierarchy locally, the pieces pass also records, for its topology check
// (AirPressure.hlsl, CSTopologyCheck), the lowest and highest component code of the pieces it replaces and the net change of fluid
// sub-cells; CSMovingSolidTopologyReset clears them first.
//
// A sub-cell is solid when a moving solid overlaps its interior (an open overlap: touching is not overlap). The union of all solids
// is evaluated per sub-cell, so overlapping solids never count a sub-cell twice, and static solid sub-cells stay solid.
//
// AIR.4F.3 F1b: the same update remaps the cell state onto the new pieces (Documentation/Air/AirFogDisplacement4F3Investigation.md, F1b).
// Old pieces exist only in the pieces pass (it reads them before overwriting), so that pass does everything that needs only its own
// cell and leaves the rest in scratch; the passes after it read neighbours' new pieces, which are final by then.
//   - Velocity (and ΔT, velocity.w): each new piece takes the mean of the old pieces it overlaps, weighted by the shared sub-cells. A
//     piece sharing none is newly exposed: CSMovingSolidRemapDistribute fills it with the mean of its open face neighbours' (never
//     larger than the largest of them), or zero. Displaced air's velocity is dropped; nothing is invented.
//   - Mist (premultiplied, an amount per volume): each new piece keeps every old piece's mist over the sub-cells they share (the
//     maintenance rule, RemappedMist). The rest of an old piece, its orphan sub-cells (now solid, or dropped by the two-piece limit),
//     holds an exact amount, which is pushed rather than deleted:
//       - an orphan region touching new fluid (in its cell, across a face, or the window's open boundary) is terminal: its amount goes
//         to the pieces it touches, in proportion to the touching sub-faces (CSMovingSolidRemapDistribute);
//       - one touching none (deep inside a solid that moved far, appeared or teleported) is relayed through face-adjacent orphan
//         regions, which were fluid before the update (so never through a static wall), toward the nearest terminal, at most
//         AirRemapRelayLevels cells (CSMovingSolidRelayDistance, CSMovingSolidRelayFlow); what remains is unresolved, in the ledger.
//     Newly exposed sub-cells receive no mist of their own: transport fills them.
// The scratch is the non-current velocity and mist buffers, which the next step overwrites whole before reading (see AirField.cpp,
// RecordMovingSolidUpdate). Every remap pass reads neighbours only from buffers it does not write, or from words or nodes no thread of
// the pass writes, so no pass depends on thread order.

#include "Voxel.hlsli"
#include "Air.hlsli"

static const uint AirMovingSolidBoxCapacity = 64;
static const uint AirMovingSolidBox = 0;
static const uint AirMovingSolidSphere = 1;
static const uint AirMovingSolidCapsule = 2;
// A view index meaning "no such buffer".
static const uint AirMovingSolidNone = 0xFFFFFFFFu;
// Truncation counter words: cells, dropped components, dropped sub-cells, cells whose static geometry alone already drops a component,
// cells where the drop needs the moving solid.
static const uint AirTruncationCells = 0;
static const uint AirTruncationComponents = 4;
static const uint AirTruncationSubcells = 8;
static const uint AirTruncationStaticCells = 12;
static const uint AirTruncationMovingCells = 16;

cbuffer AirMovingSolidConstants : register(b0)
{
    VoxelWorldBinding voxels;
    AirFieldBinding field;
    int voxelLevel;
    uint pieceUnordered;
    uint apertureUnordered;
    uint solidCount;
    uint boxCount;
    uint totalCells;
    // F1a.2: the obstacle mask (writable), phi (writable, or AirMovingSolidNone when the solver keeps none) and the truncation counters
    // (AirMovingSolidNone: not counted).
    uint obstacleUnordered;
    uint phiUnordered;
    uint truncationUnordered;
    // F1a.3: the solver's component codes and its topology buffer (AirMovingSolidNone: not recorded).
    uint componentUnordered;
    uint topologyUnordered;
    // F2b.4: the AIR step in seconds (CSWallTangential's wall velocity); 0 outside that pass.
    float stepSeconds;
    // F1b: the current velocity of pieces 0 and 1 and the current mist of pieces 0 and 1 (AirMovingSolidNone: no mist), written in place;
    // the scratch (orphan amounts per old piece, orphan masks, node state); the remap ledger and the relay predicate.
    uint velocityUnordered0;
    uint velocityUnordered1;
    uint mistUnordered0;
    uint mistUnordered1;
    uint scratchAmount0;
    uint scratchAmount1;
    uint scratchMasks;
    uint scratchState;
    uint ledgerUnordered;
    uint predicateUnordered;
    // F1b relay: the distance pass's round (reads parity relayLevel % 2) or the flow pass's level; the parity holding final distances.
    uint relayLevel;
    uint relayParity;
    // F1b: the dirty boxes of the pieces pass (the cells holding scratch), which a pass over other boxes looks donors up in.
    uint donorBoxCount;
    // F2b.1: the anchor pose list inside the solids buffer (offset, count; the new poses are entries 0 to solidCount - 1), and whether the
    // boundary passes are recorded at all.
    uint anchorOffset;
    uint anchorCount;
    uint boundaryEnabled;
    // F2b.1: the effective-volume offset S and wall source D (float2 per slot), the per-solid correction sums and the boundary diagnostics.
    uint effectiveOffsetUnordered;
    uint wallSourceUnordered;
    uint correctionUnordered;
    uint boundaryDiagnosticsUnordered;
    // F2b.2: the per-boundary-cell records (AirBoundaryRecordStride each, indexed by the cell's position in the pass's box list), and
    // whether this update is continuous (the control-volume remap; 0: F1b's geometric remap).
    uint recordUnordered;
    uint continuousRemap;
    // Diagnostics only (F2a's naive comparison): the gather leaves S at zero and the densities geometric.
    uint naiveCoupling;
    // F2b.4: the tangential wall coupling's rate k (1/s) times the step (s); 0 records no coupling (CSWallTangential).
    float tangentialRateStep;
    // Dirty boxes in world cells: xyz the minimum and w the first linear thread of the box; xyz the size and w its cell count.
    int4 boxMinimum[AirMovingSolidBoxCapacity];
    int4 boxSize[AirMovingSolidBoxCapacity];
    // F1b: the pieces pass's boxes (xyz minimum; xyz size).
    int4 donorMinimum[AirMovingSolidBoxCapacity];
    int4 donorSize[AirMovingSolidBoxCapacity];
};

// F1b ledger (cumulative): fixed-point amounts of the density channel (mist.w times sub-cells, AirRemapFixedScale per unit), then counts.
//   displaced: orphan mist on sub-cells that became solid; truncated: orphan mist on free sub-cells the two-piece limit dropped;
//   delivered: mist added to pieces; outflow: mist pushed across the window's open boundary; unresolved: orphan mist with no destination
//   within reach (removed, never silently); relayed: mist moved by relay hops (counted per hop);
//   orphan regions, relayed regions, unresolved regions, newly exposed pieces, exposed pieces with no open neighbour to take state from.
// Closure: displaced + truncated = delivered + outflow + unresolved, up to rounding.
static const uint AirRemapLedgerDisplaced = 0;
static const uint AirRemapLedgerTruncated = 8;
static const uint AirRemapLedgerDelivered = 16;
static const uint AirRemapLedgerOutflow = 24;
static const uint AirRemapLedgerUnresolved = 32;
static const uint AirRemapLedgerRelayed = 40;
static const uint AirRemapLedgerOrphanRegions = 48;
static const uint AirRemapLedgerRelayedRegions = 52;
static const uint AirRemapLedgerUnresolvedRegions = 56;
static const uint AirRemapLedgerExposedPieces = 60;
static const uint AirRemapLedgerExposedIsolated = 64;
// F2b.2: mist of fluid no new control region owns (the unowned fallback of a continuous update), pushed F1b's way (int64, fixed point).
static const uint AirRemapLedgerUnowned = 72;
static const float AirRemapFixedScale = 1048576.0;

// F1b relay: the most cells an orphan amount travels to reach new fluid, and the distance meaning "none within reach".
static const uint AirRemapRelayLevels = 8;
static const uint AirRemapUnreached = 0xFFFFu;

// F1b node state (scratchState, a uint4 per slot): x and y the two parities of each orphan region's distance (16 bits per old piece),
// z the flags below, w each region's terminal weight (16 bits per old piece: touching sub-faces with new fluid and the open boundary).
static const uint AirRemapExposed0 = 1u;
static const uint AirRemapExposed1 = 2u;
// z bits 8 to 15 and 16 to 23: each region's sub-faces on the window's open boundary (part of its weight).
static const uint AirRemapOutsideShift0 = 8;
static const uint AirRemapOutsideShift1 = 16;

#include "AirGeometryBuild.hlsli"

// World space. Box: center, unit axes and half extents. Sphere: center and radius. Capsule: the segment and radius. Every shape also
// carries its world bounds, which reject most sub-cells with one test.
struct AirMovingSolid
{
    float3 center;
    uint type;
    float3 axisX;
    float radius;
    float3 axisY;
    // F2b.1: the solid's exact volume in sub-cell volumes ((cell / 4)^3).
    float exactVolume;
    float3 axisZ;
    // F2b.1: bit 0, isolated (no other solid's bounds within a sub-cell of its own, so its coverage may be corrected to its exact volume).
    uint boundaryFlags;
    float3 halfExtents;
    float padding2;
    float3 segmentA;
    float padding3;
    float3 segmentB;
    float padding4;
    float3 boundsMinimum;
    float padding5;
    float3 boundsMaximum;
    float padding6;
};

StructuredBuffer<AirMovingSolid> movingSolids : register(t0);

float PointBoxDistanceSquared(float3 position, float3 low, float3 high)
{
    const float3 offset = clamp(position, low, high) - position;
    return dot(offset, offset);
}

// Separating axes between the oriented box and the axis-aligned sub-cell: 3 world axes, 3 box axes, 9 cross products (skipped when
// degenerate, which happens only for parallel axes already tested). Touching counts as separated.
bool BoxOverlaps(AirMovingSolid solid, float3 low, float3 high)
{
    const float3 extent = 0.5 * (high - low);
    const float3 offset = solid.center - 0.5 * (high + low);
    const float3 axes[3] = {solid.axisX, solid.axisY, solid.axisZ};
    const float halfExtents[3] = {solid.halfExtents.x, solid.halfExtents.y, solid.halfExtents.z};

    for(uint world = 0; world < 3; ++world)
    {
        const float boxRadius = halfExtents[0] * abs(axes[0][world]) + halfExtents[1] * abs(axes[1][world]) +
            halfExtents[2] * abs(axes[2][world]);

        if(abs(offset[world]) >= extent[world] + boxRadius)
        {
            return false;
        }
    }

    for(uint own = 0; own < 3; ++own)
    {
        const float cellRadius = dot(extent, abs(axes[own]));

        if(abs(dot(offset, axes[own])) >= halfExtents[own] + cellRadius)
        {
            return false;
        }
    }

    for(uint worldAxis = 0; worldAxis < 3; ++worldAxis)
    {
        float3 unit = float3(0.0, 0.0, 0.0);
        unit[worldAxis] = 1.0;

        for(uint boxAxis = 0; boxAxis < 3; ++boxAxis)
        {
            const float3 axis = cross(unit, axes[boxAxis]);

            if(dot(axis, axis) < 1.0e-10)
            {
                continue;
            }

            const float cellRadius = dot(extent, abs(axis));
            const float boxRadius = halfExtents[0] * abs(dot(axes[0], axis)) + halfExtents[1] * abs(dot(axes[1], axis)) +
                halfExtents[2] * abs(dot(axes[2], axis));

            if(abs(dot(offset, axis)) >= cellRadius + boxRadius)
            {
                return false;
            }
        }
    }

    return true;
}

// Squared distance between segments p0-p1 and q0-q1 (Ericson, Real-Time Collision Detection 5.1.9).
float SegmentSegmentDistanceSquared(float3 p0, float3 p1, float3 q0, float3 q1)
{
    const float3 d1 = p1 - p0;
    const float3 d2 = q1 - q0;
    const float3 r = p0 - q0;
    const float a = dot(d1, d1);
    const float e = dot(d2, d2);
    const float f = dot(d2, r);
    float s = 0.0;
    float t = 0.0;

    if(a <= 1.0e-12 && e <= 1.0e-12)
    {
        return dot(r, r);
    }

    if(a <= 1.0e-12)
    {
        t = saturate(f / e);
    }
    else
    {
        const float c = dot(d1, r);

        if(e <= 1.0e-12)
        {
            s = saturate(-c / a);
        }
        else
        {
            const float b = dot(d1, d2);
            const float denominator = a * e - b * b;
            s = denominator > 0.0 ? saturate((b * f - c * e) / denominator) : 0.0;
            t = (b * s + f) / e;

            if(t < 0.0)
            {
                t = 0.0;
                s = saturate(-c / a);
            }
            else if(t > 1.0)
            {
                t = 1.0;
                s = saturate((b - c) / a);
            }
        }
    }

    const float3 difference = (p0 + d1 * s) - (q0 + d2 * t);
    return dot(difference, difference);
}

// Whether the segment passes through the box's interior (slab clipping over t in [0, 1]).
bool SegmentEntersBox(float3 start, float3 end, float3 low, float3 high)
{
    const float3 direction = end - start;
    float entry = 0.0;
    float exit = 1.0;

    for(uint axis = 0; axis < 3; ++axis)
    {
        if(abs(direction[axis]) < 1.0e-12)
        {
            if(start[axis] <= low[axis] || start[axis] >= high[axis])
            {
                return false;
            }

            continue;
        }

        const float first = (low[axis] - start[axis]) / direction[axis];
        const float second = (high[axis] - start[axis]) / direction[axis];
        entry = max(entry, min(first, second));
        exit = min(exit, max(first, second));
    }

    return entry < exit;
}

// The segment-to-box distance is reached at an endpoint or against one of the box's 12 edges unless the segment enters the box.
bool CapsuleOverlaps(AirMovingSolid solid, float3 low, float3 high)
{
    if(SegmentEntersBox(solid.segmentA, solid.segmentB, low, high))
    {
        return true;
    }

    const float radiusSquared = solid.radius * solid.radius;
    float nearest = min(PointBoxDistanceSquared(solid.segmentA, low, high), PointBoxDistanceSquared(solid.segmentB, low, high));

    for(uint axis = 0; axis < 3; ++axis)
    {
        const uint first = (axis + 1) % 3;
        const uint second = (axis + 2) % 3;

        for(uint corner = 0; corner < 4; ++corner)
        {
            float3 start = low;
            start[first] = (corner & 1u) != 0 ? high[first] : low[first];
            start[second] = (corner & 2u) != 0 ? high[second] : low[second];
            float3 end = start;
            end[axis] = high[axis];
            nearest = min(nearest, SegmentSegmentDistanceSquared(solid.segmentA, solid.segmentB, start, end));
        }
    }

    return nearest < radiusSquared;
}

bool SolidOverlaps(AirMovingSolid solid, float3 low, float3 high)
{
    if(any(high <= solid.boundsMinimum) || any(low >= solid.boundsMaximum))
    {
        return false;
    }

    if(solid.type == AirMovingSolidSphere)
    {
        return PointBoxDistanceSquared(solid.center, low, high) < solid.radius * solid.radius;
    }

    if(solid.type == AirMovingSolidCapsule)
    {
        return CapsuleOverlaps(solid, low, high);
    }

    return BoxOverlaps(solid, low, high);
}

// The cell's sub-cells some moving solid overlaps.
uint64_t MovingSolidSubcells(int3 cell)
{
    const float subcellSize = field.cellSize / float(AirSubcellsPerAxis);
    const float3 cellLow = float3(cell) * field.cellSize;
    const float3 cellHigh = cellLow + field.cellSize;
    uint64_t solid = 0;

    for(uint index = 0; index < solidCount; ++index)
    {
        const AirMovingSolid shape = movingSolids[index];

        if(any(cellHigh <= shape.boundsMinimum) || any(cellLow >= shape.boundsMaximum))
        {
            continue;
        }

        for(uint bit = 0; bit < 64; ++bit)
        {
            const uint64_t subcell = uint64_t(1) << bit;

            if((solid & subcell) != 0)
            {
                continue;
            }

            const float3 low = cellLow + float3(bit & 3u, (bit >> 2) & 3u, bit >> 4) * subcellSize;

            if(SolidOverlaps(shape, low, low + subcellSize))
            {
                solid |= subcell;
            }
        }
    }

    return solid;
}

// The world cell of a linear thread over the dirty boxes, and its box; false past the last box.
bool DirtyCell(uint thread, out int3 cell, out uint owner)
{
    cell = int3(0, 0, 0);
    owner = 0;

    for(uint box = 0; box < boxCount; ++box)
    {
        const uint first = uint(boxMinimum[box].w);
        const uint count = uint(boxSize[box].w);

        if(thread >= first && thread < first + count)
        {
            const uint local = thread - first;
            const uint3 size = uint3(boxSize[box].xyz);
            cell = boxMinimum[box].xyz + int3(local % size.x, (local / size.x) % size.y, local / (size.x * size.y));
            owner = box;
            return true;
        }
    }

    return false;
}

// Whether `box` is the first dirty box holding the cell, so each cell is written (and counted) once however the boxes overlap.
bool FirstBoxOf(int3 cell, uint box)
{
    for(uint earlier = 0; earlier < box; ++earlier)
    {
        if(all(cell >= boxMinimum[earlier].xyz) && all(cell < boxMinimum[earlier].xyz + boxSize[earlier].xyz))
        {
            return false;
        }
    }

    return true;
}

// The phi a rebuilt piece starts with: the old piece it shares most sub-cells with (piece 0 on a tie), or 0 when it shares none.
float CarriedPhi(uint64_t piece, AirPieces previous, float2 previousPhi)
{
    if(piece == 0)
    {
        return 0.0;
    }

    const uint overlap0 = AirSubcellCount(piece & previous.piece0);
    const uint overlap1 = AirSubcellCount(piece & previous.piece1);

    if(overlap0 == 0 && overlap1 == 0)
    {
        return 0.0;
    }

    return overlap1 > overlap0 ? previousPhi.y : previousPhi.x;
}

// The number of 6-connected components of a sub-cell mask.
uint ComponentCount(uint64_t mask)
{
    uint count = 0;

    [loop]
    while(mask != 0 && count < 64)
    {
        mask &= ~AirFloodSubcells(mask, AirLowestSubcell(mask));
        ++count;
    }

    return count;
}

void CountTruncation(uint64_t staticFree, uint64_t free, AirPieces pieces)
{
    const uint64_t dropped = free & ~(pieces.piece0 | pieces.piece1);

    if(dropped == 0)
    {
        return;
    }

    const AirPieces staticPieces = AirLabelPieces(staticFree);
    const bool staticDrops = (staticFree & ~(staticPieces.piece0 | staticPieces.piece1)) != 0;
    RWByteAddressBuffer counters = ResourceDescriptorHeap[NonUniformResourceIndex(truncationUnordered)];
    uint ignored;
    counters.InterlockedAdd(AirTruncationCells, 1, ignored);
    counters.InterlockedAdd(AirTruncationComponents, ComponentCount(dropped), ignored);
    counters.InterlockedAdd(AirTruncationSubcells, AirSubcellCount(dropped), ignored);
    counters.InterlockedAdd(staticDrops ? AirTruncationStaticCells : AirTruncationMovingCells, 1, ignored);
}

// F1a.3: the topology buffer's words this pass writes (AirPressure.hlsl's TopologyCodeLow, TopologyCodeHigh, TopologyVolumeDelta).
static const uint AirTopologyCodeLow = 0;
static const uint AirTopologyCodeHigh = 4;
static const uint AirTopologyVolumeDelta = 8;
static const uint AirTopologyReplacedWindow = 40;
// AIR A2/T2a: the codes the rewritten cells' new pieces read (AirPressure.hlsl's TopologyCurrentLow, TopologyCurrentHigh).
static const uint AirTopologyCurrentLow = 92;
static const uint AirTopologyCurrentHigh = 96;
// AIR A2/T2b (diagnostics): per update, the code range of the pieces cells kept exactly; running counts of rewritten cells, of those that
// held two pieces, of kept pieces, and of new pieces at an index that held none.
static const uint AirTopologyKeptLow = 120;
static const uint AirTopologyRewrittenCells = 136;
static const uint AirTopologyTwoPieceCells = 140;
static const uint AirTopologyKeptPieces = 144;
static const uint AirTopologyNewSlotPieces = 148;
// The latest update's new pieces at an index that held none (reset per update).
static const uint AirTopologyUpdateNewSlots = 156;

[numthreads(1, 1, 1)]
void CSMovingSolidTopologyReset(uint3 thread : SV_DispatchThreadID)
{
    RWByteAddressBuffer topology = ResourceDescriptorHeap[NonUniformResourceIndex(topologyUnordered)];
    topology.Store3(AirTopologyCodeLow, uint3(0xFFFFFFFFu, 0, 0));
    topology.Store(AirTopologyReplacedWindow, 0);
    topology.Store2(AirTopologyCurrentLow, uint2(0xFFFFFFFFu, 0));
    topology.Store2(AirTopologyKeptLow, uint2(0xFFFFFFFFu, 0));
    topology.Store(AirTopologyUpdateNewSlots, 0);
}

// A code into the range whose low word is at `low` (the high word follows).
void RecordCodeRange(RWByteAddressBuffer topology, uint low, uint code)
{
    uint ignored;
    topology.InterlockedMin(low, code, ignored);
    topology.InterlockedMax(low + 4, code, ignored);
}

// Whether a piece of `cell` has a face on the window's boundary (open to the outside).
bool HasWindowFace(int3 cell, uint64_t piece)
{
    const int3 local = cell - field.minimum;
    const int3 last = int3(field.dimensions) - 1;
    const uint64_t low[3] = {AirSubcellsLowX, AirSubcellsLowY, AirSubcellsLowZ};
    const uint64_t high[3] = {AirSubcellsHighX, AirSubcellsHighY, AirSubcellsHighZ};
    bool face = false;

    for(uint axis = 0; axis < 3; ++axis)
    {
        face = face || (local[axis] == 0 && (piece & low[axis]) != 0) || (local[axis] == last[axis] && (piece & high[axis]) != 0);
    }

    return face;
}

// The replaced pieces' codes (AIR A2/T2b: and the new ones', kept pieces apart), whether one had a window face, and the change of the
// cell's fluid sub-cells.
void RecordTopology(int3 cell, uint3 slot, AirPieces previous, AirPieces pieces)
{
    RWByteAddressBuffer topology = ResourceDescriptorHeap[NonUniformResourceIndex(topologyUnordered)];
    RWByteAddressBuffer components = ResourceDescriptorHeap[NonUniformResourceIndex(componentUnordered)];
    const uint slotIndex = slot.x + field.dimensions.x * (slot.y + field.dimensions.y * slot.z);
    const uint2 codes = components.Load2(slotIndex * 8);
    uint ignored;

    // AIR A2/T2b: a piece the cell keeps exactly (the same mask at the same index) is neither replaced nor new. Its code stays valid, and
    // so do its links to unchanged pieces (an aperture is a function of the two pieces' masks); the check's box labelling sees its links
    // to changed ones.
    const bool kept0 = previous.piece0 != 0 && previous.piece0 == pieces.piece0;
    const bool kept1 = previous.piece1 != 0 && previous.piece1 == pieces.piece1;

    if(previous.piece0 != 0)
    {
        RecordCodeRange(topology, kept0 ? AirTopologyKeptLow : AirTopologyCodeLow, codes.x);
    }

    if(previous.piece1 != 0)
    {
        RecordCodeRange(topology, kept1 ? AirTopologyKeptLow : AirTopologyCodeLow, codes.y);
    }

    if(pieces.piece0 != 0 && !kept0)
    {
        RecordCodeRange(topology, AirTopologyCurrentLow, codes.x);
    }

    if(pieces.piece1 != 0 && !kept1)
    {
        RecordCodeRange(topology, AirTopologyCurrentLow, codes.y);
    }

    // AIR A2/T2b (diagnostics): the counts.
    const uint newSlots = (previous.piece0 == 0 && pieces.piece0 != 0 ? 1u : 0u) + (previous.piece1 == 0 && pieces.piece1 != 0 ? 1u : 0u);
    topology.InterlockedAdd(AirTopologyRewrittenCells, 1, ignored);
    topology.InterlockedAdd(AirTopologyTwoPieceCells, previous.piece0 != 0 && previous.piece1 != 0 ? 1u : 0u, ignored);
    topology.InterlockedAdd(AirTopologyKeptPieces, (kept0 ? 1u : 0u) + (kept1 ? 1u : 0u), ignored);
    topology.InterlockedAdd(AirTopologyNewSlotPieces, newSlots, ignored);
    topology.InterlockedAdd(AirTopologyUpdateNewSlots, newSlots, ignored);

    if(HasWindowFace(cell, previous.piece0) || HasWindowFace(cell, previous.piece1))
    {
        topology.Store(AirTopologyReplacedWindow, 1);
    }

    const int before = int(AirSubcellCount(previous.piece0) + AirSubcellCount(previous.piece1));
    const int after = int(AirSubcellCount(pieces.piece0) + AirSubcellCount(pieces.piece1));

    if(after != before)
    {
        topology.InterlockedAdd(AirTopologyVolumeDelta, uint(after - before), ignored);
    }
}

// The cell's two obstacle bits, in place: the word is shared with 15 other cells, which other threads may be updating.
void StoreObstacleBits(uint3 slot, uint bits)
{
    RWByteAddressBuffer obstacles = ResourceDescriptorHeap[NonUniformResourceIndex(obstacleUnordered)];
    const uint address = AirObstacleWord(field, slot) * 4;
    const uint shift = (slot.x % AirSlotsPerObstacleWord) * 2;
    uint ignored;
    obstacles.InterlockedAnd(address, ~(3u << shift), ignored);
    obstacles.InterlockedOr(address, bits << shift, ignored);
}

RWByteAddressBuffer RawBuffer(uint index)
{
    return ResourceDescriptorHeap[NonUniformResourceIndex(index)];
}

void LedgerAdd(uint offset, float amount)
{
    if(amount != 0.0)
    {
        RawBuffer(ledgerUnordered).InterlockedAdd64(offset, int64_t(amount * AirRemapFixedScale));
    }
}

void LedgerCount(uint offset, uint count)
{
    uint ignored;
    RawBuffer(ledgerUnordered).InterlockedAdd(offset, count, ignored);
}

uint64_t MaskOf(uint low, uint high)
{
    return uint64_t(low) | (uint64_t(high) << 32);
}

// F1b: a new piece's velocity and ΔT: the mean of the old pieces it overlaps, weighted by the sub-cells shared. Only overlapping old
// pieces are read. A piece sharing none is newly exposed (zero here; CSMovingSolidRemapDistribute fills it).
float4 RemappedVelocity(uint64_t piece, AirPieces previous, float4 previous0, float4 previous1, out bool exposed)
{
    exposed = false;

    if(piece == 0)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const uint overlap0 = AirSubcellCount(piece & previous.piece0);
    const uint overlap1 = AirSubcellCount(piece & previous.piece1);
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);

    if(overlap0 > 0)
    {
        sum += previous0 * float(overlap0);
    }

    if(overlap1 > 0)
    {
        sum += previous1 * float(overlap1);
    }

    if(overlap0 + overlap1 == 0)
    {
        exposed = true;
        return sum;
    }

    return sum / float(overlap0 + overlap1);
}

// F1b: the mist a new piece keeps: each old piece's mist over the sub-cells the two share, spread over the new piece's volume (the
// maintenance rule, AirField.hlsl's RemappedMist, reading only overlapping old pieces). Newly exposed sub-cells bring nothing.
float4 RetainedMist(uint64_t piece, AirPieces previous, float4 previous0, float4 previous1)
{
    const uint volume = AirSubcellCount(piece);
    const uint overlap0 = AirSubcellCount(piece & previous.piece0);
    const uint overlap1 = AirSubcellCount(piece & previous.piece1);
    float4 amount = float4(0.0, 0.0, 0.0, 0.0);

    if(overlap0 > 0)
    {
        amount += previous0 * float(overlap0);
    }

    if(overlap1 > 0)
    {
        amount += previous1 * float(overlap1);
    }

    return volume > 0 ? amount / float(volume) : float4(0.0, 0.0, 0.0, 0.0);
}

// F1b, in the pieces pass: the cell's velocity and mist onto its new pieces, in place, and its orphan regions into scratch. Every dirty
// cell writes its scratch (an unchanged cell: no orphans, nothing exposed), because later passes read it for any dirty cell.
// F2b.2: in a continuous update only the velocity (and its exposed flags) is remapped here: the mist follows the control volumes in
// CSBoundaryVolumes and CSBoundaryGather, and the orphans are the fluid no new region owns (written there).
void RemapCell(uint address, uint64_t free, AirPieces previous, AirPieces pieces, bool changed)
{
    const uint64_t represented = pieces.piece0 | pieces.piece1;
    const bool geometric = continuousRemap == 0;
    const uint64_t orphan0 = changed && geometric ? previous.piece0 & ~represented : 0;
    const uint64_t orphan1 = changed && geometric ? previous.piece1 & ~represented : 0;
    uint flags = 0;

    if(changed)
    {
        RWByteAddressBuffer velocity0 = RawBuffer(velocityUnordered0);
        RWByteAddressBuffer velocity1 = RawBuffer(velocityUnordered1);
        const float4 previousVelocity0 = asfloat(velocity0.Load4(address));
        const float4 previousVelocity1 = asfloat(velocity1.Load4(address));
        bool exposed0;
        bool exposed1;
        velocity0.Store4(address, asuint(RemappedVelocity(pieces.piece0, previous, previousVelocity0, previousVelocity1, exposed0)));
        velocity1.Store4(address, asuint(RemappedVelocity(pieces.piece1, previous, previousVelocity0, previousVelocity1, exposed1)));
        flags = (exposed0 ? AirRemapExposed0 : 0u) | (exposed1 ? AirRemapExposed1 : 0u);

        if(flags != 0)
        {
            LedgerCount(AirRemapLedgerExposedPieces, (exposed0 ? 1u : 0u) + (exposed1 ? 1u : 0u));
        }
    }

    if(changed && geometric && mistUnordered0 != AirMovingSolidNone)
    {
        RWByteAddressBuffer mist0 = RawBuffer(mistUnordered0);
        RWByteAddressBuffer mist1 = RawBuffer(mistUnordered1);
        const float4 previousMist0 = asfloat(mist0.Load4(address));
        const float4 previousMist1 = asfloat(mist1.Load4(address));
        mist0.Store4(address, asuint(RetainedMist(pieces.piece0, previous, previousMist0, previousMist1)));
        mist1.Store4(address, asuint(RetainedMist(pieces.piece1, previous, previousMist0, previousMist1)));

        // The orphan amounts: exactly each old piece's mist over the sub-cells no new piece kept.
        const float4 amount0 = orphan0 != 0 ? previousMist0 * float(AirSubcellCount(orphan0)) : float4(0.0, 0.0, 0.0, 0.0);
        const float4 amount1 = orphan1 != 0 ? previousMist1 * float(AirSubcellCount(orphan1)) : float4(0.0, 0.0, 0.0, 0.0);
        RawBuffer(scratchAmount0).Store4(address, asuint(amount0));
        RawBuffer(scratchAmount1).Store4(address, asuint(amount1));

        // Solid now, or free but dropped by the two-piece limit (truncation).
        LedgerAdd(AirRemapLedgerDisplaced, previousMist0.w * float(AirSubcellCount(orphan0 & ~free)) +
            previousMist1.w * float(AirSubcellCount(orphan1 & ~free)));
        LedgerAdd(AirRemapLedgerTruncated, previousMist0.w * float(AirSubcellCount(orphan0 & free)) +
            previousMist1.w * float(AirSubcellCount(orphan1 & free)));
    }

    const uint unreached = AirRemapUnreached | (AirRemapUnreached << 16);
    RawBuffer(scratchMasks).Store4(address, uint4(uint(orphan0), uint(orphan0 >> 32), uint(orphan1), uint(orphan1 >> 32)));
    RawBuffer(scratchState).Store4(address, uint4(unreached, unreached, flags, 0));
}

// One thread per dirty cell (boxes may overlap; the first box holding a cell owns it).
[numthreads(64, 1, 1)]
void CSMovingSolidPieces(uint3 thread : SV_DispatchThreadID)
{
    int3 cell;
    uint box;

    // Only the first box holding a cell writes it: phi is read and rewritten here, which a second thread must not repeat.
    if(thread.x >= totalCells || !DirtyCell(thread.x, cell, box) || !AirContains(field, cell) || !FirstBoxOf(cell, box))
    {
        return;
    }

    const uint64_t staticFree = FreeSubcells(cell);
    const uint64_t solid = MovingSolidSubcells(cell);
    const uint64_t free = staticFree & ~solid;
    const AirPieces pieces = AirLabelPieces(free);
    const uint3 slot = AirSlotOf(field, cell);
    const uint address = AirVelocityAddress(field, slot);
    RWByteAddressBuffer output = ResourceDescriptorHeap[NonUniformResourceIndex(pieceUnordered)];
    const uint4 previousWords = output.Load4(address);
    const uint4 words = uint4(uint(pieces.piece0), uint(pieces.piece0 >> 32), uint(pieces.piece1), uint(pieces.piece1 >> 32));

    AirPieces previous;
    previous.piece0 = uint64_t(previousWords.x) | (uint64_t(previousWords.y) << 32);
    previous.piece1 = uint64_t(previousWords.z) | (uint64_t(previousWords.w) << 32);

    if(topologyUnordered != AirMovingSolidNone && any(words != previousWords))
    {
        RecordTopology(cell, slot, previous, pieces);
    }

    if(phiUnordered != AirMovingSolidNone && any(words != previousWords))
    {
        RWByteAddressBuffer phi = ResourceDescriptorHeap[NonUniformResourceIndex(phiUnordered)];
        const uint phiAddress = (slot.x + field.dimensions.x * (slot.y + field.dimensions.y * slot.z)) * 8;
        const float2 previousPhi = asfloat(phi.Load2(phiAddress));
        const float2 carried = float2(CarriedPhi(pieces.piece0, previous, previousPhi), CarriedPhi(pieces.piece1, previous, previousPhi));
        phi.Store2(phiAddress, asuint(carried));
    }

    // F1b: velocity and mist follow the same transition, here and only here (the pieces are replaced below, once).
    if(velocityUnordered0 != AirMovingSolidNone)
    {
        RemapCell(address, free, previous, pieces, any(words != previousWords));
    }

    output.Store4(address, words);
    StoreObstacleBits(slot, ObstacleBits(cell) | ((staticFree & solid) != 0 ? AirObstacleBlocked : 0u));

    if(truncationUnordered != AirMovingSolidNone)
    {
        CountTruncation(staticFree, free, pieces);
    }
}

// One thread per slot whose record reads a dirty cell (the boxes, grown by one cell toward -x, -y and -z by the caller).
[numthreads(64, 1, 1)]
void CSMovingSolidApertures(uint3 thread : SV_DispatchThreadID)
{
    int3 cell;
    uint box;

    if(thread.x >= totalCells || !DirtyCell(thread.x, cell, box) || !AirContains(field, cell))
    {
        return;
    }

    const uint3 slot = AirSlotOf(field, cell);
    const uint64_t record = AirBuildApertureRecord(slot);
    RWByteAddressBuffer apertures = ResourceDescriptorHeap[NonUniformResourceIndex(apertureUnordered)];
    const uint index = slot.x + field.dimensions.x * (slot.y + field.dimensions.y * slot.z);
    apertures.Store2(index * AirApertureBytesPerSlot, uint2(uint(record), uint(record >> 32)));
}

// ---------------------------------------------------------------------------------------------------------------------------- F1b remap

static const int3 AirRemapDirections[6] = {int3(-1, 0, 0), int3(1, 0, 0), int3(0, -1, 0), int3(0, 1, 0), int3(0, 0, -1), int3(0, 0, 1)};

// Sub-face pairs between `from` (in a cell) and `to` (in its neighbour along `axis`, on the positive side when `positive`).
uint FaceContacts(uint64_t from, uint64_t to, uint axis, bool positive)
{
    return positive ? AirPieceLink(from, to, axis) : AirPieceLink(to, from, axis);
}

// Sub-face pairs between two sub-cell sets of one cell (each pair of face-adjacent sub-cells, one in each).
uint InteriorContacts(uint64_t from, uint64_t to)
{
    return AirSubcellCount(((from & ~AirSubcellsHighX) << 1) & to) + AirSubcellCount(((from & ~AirSubcellsLowX) >> 1) & to) +
        AirSubcellCount(((from & ~AirSubcellsHighY) << 4) & to) + AirSubcellCount(((from & ~AirSubcellsLowY) >> 4) & to) +
        AirSubcellCount((from << 16) & to) + AirSubcellCount((from >> 16) & to);
}

AirPieces LoadPieces(int3 cell)
{
    const uint4 words = RawBuffer(pieceUnordered).Load4(AirVelocityAddress(field, AirSlotOf(field, cell)));
    AirPieces pieces;
    pieces.piece0 = MaskOf(words.x, words.y);
    pieces.piece1 = MaskOf(words.z, words.w);
    return pieces;
}

// Whether the pieces pass wrote the cell's scratch this update.
bool IsDonorCell(int3 cell)
{
    for(uint box = 0; box < donorBoxCount; ++box)
    {
        if(all(cell >= donorMinimum[box].xyz) && all(cell < donorMinimum[box].xyz + donorSize[box].xyz))
        {
            return true;
        }
    }

    return false;
}

// A donor cell's two orphan regions (old pieces 0 and 1).
void LoadOrphans(uint address, out uint64_t orphans[2])
{
    const uint4 words = RawBuffer(scratchMasks).Load4(address);
    orphans[0] = MaskOf(words.x, words.y);
    orphans[1] = MaskOf(words.z, words.w);
}

uint DistanceOf(uint packed, uint region)
{
    return (packed >> (16 * region)) & 0xFFFFu;
}

float4 LoadOrphanAmount(uint address, uint region)
{
    return asfloat(RawBuffer(region == 0 ? scratchAmount0 : scratchAmount1).Load4(address));
}

// A region's terminal weight: the sub-faces it shares with new fluid in its cell and in the six neighbours, plus its sub-faces on the
// window's open boundary (`outside`). Zero: nothing to push it into from here.
uint TerminalWeight(int3 cell, uint64_t orphan, AirPieces own, out uint outside)
{
    uint weight = InteriorContacts(orphan, own.piece0) + InteriorContacts(orphan, own.piece1);
    outside = 0;

    for(uint direction = 0; direction < 6; ++direction)
    {
        const uint axis = direction / 2;
        const bool positive = (direction & 1u) != 0;
        const int3 neighbour = cell + AirRemapDirections[direction];

        if(!AirContains(field, neighbour))
        {
            outside += positive ? AirHighBoundaryAperture(orphan, axis) : AirLowBoundaryAperture(orphan, axis);
            continue;
        }

        const AirPieces other = LoadPieces(neighbour);
        weight += FaceContacts(orphan, other.piece0, axis, positive) + FaceContacts(orphan, other.piece1, axis, positive);
    }

    return weight + outside;
}

// The owned cell of a remap pass's thread, over the constants' box list (each cell once however the boxes overlap).
bool OwnedCell(uint thread, out int3 cell)
{
    uint box;
    return thread < totalCells && DirtyCell(thread, cell, box) && AirContains(field, cell) && FirstBoxOf(cell, box);
}

// One thread per dirty cell, after the pieces and before any relay: each orphan region's terminal weight, and its distance (0 when
// terminal). Regions with no weight count into the relay predicate.
[numthreads(64, 1, 1)]
void CSMovingSolidRemapClassify(uint3 thread : SV_DispatchThreadID)
{
    int3 cell;

    if(!OwnedCell(thread.x, cell))
    {
        return;
    }

    const uint address = AirVelocityAddress(field, AirSlotOf(field, cell));
    uint64_t orphans[2];
    LoadOrphans(address, orphans);

    if(orphans[0] == 0 && orphans[1] == 0)
    {
        return;
    }

    const AirPieces own = LoadPieces(cell);
    uint4 state = RawBuffer(scratchState).Load4(address);
    uint distances = 0;
    uint weights = 0;

    for(uint region = 0; region < 2; ++region)
    {
        uint distance = AirRemapUnreached;
        uint weight = 0;
        uint outside = 0;

        if(orphans[region] != 0)
        {
            weight = TerminalWeight(cell, orphans[region], own, outside);
            distance = weight > 0 ? 0 : AirRemapUnreached;
            LedgerCount(AirRemapLedgerOrphanRegions, 1);

            if(weight == 0)
            {
                uint ignored;
                RawBuffer(predicateUnordered).InterlockedAdd(0, 1, ignored);
            }
        }

        distances |= distance << (16 * region);
        weights |= weight << (16 * region);
        state.z |= outside << (region == 0 ? AirRemapOutsideShift0 : AirRemapOutsideShift1);
    }

    state.x = distances;
    state.y = distances;
    state.w = weights;
    RawBuffer(scratchState).Store4(address, state);
}

// One relay distance round (relayLevel): each orphan region's distance becomes one more than its nearest face-adjacent orphan region's,
// if smaller. Reads parity relayLevel % 2 and writes the other parity's word only, so no thread reads a word another writes.
[numthreads(64, 1, 1)]
void CSMovingSolidRelayDistance(uint3 thread : SV_DispatchThreadID)
{
    int3 cell;

    if(!OwnedCell(thread.x, cell))
    {
        return;
    }

    const uint address = AirVelocityAddress(field, AirSlotOf(field, cell));
    uint64_t orphans[2];
    LoadOrphans(address, orphans);

    if(orphans[0] == 0 && orphans[1] == 0)
    {
        return;
    }

    const uint readOffset = (relayLevel % 2) * 4;
    const uint writeOffset = (1 - relayLevel % 2) * 4;
    const uint current = RawBuffer(scratchState).Load(address + readOffset);
    uint updated = 0;

    for(uint region = 0; region < 2; ++region)
    {
        uint distance = DistanceOf(current, region);

        for(uint direction = 0; direction < 6 && orphans[region] != 0 && distance != 0; ++direction)
        {
            const int3 neighbour = cell + AirRemapDirections[direction];

            if(!AirContains(field, neighbour) || !IsDonorCell(neighbour))
            {
                continue;
            }

            const uint neighbourAddress = AirVelocityAddress(field, AirSlotOf(field, neighbour));
            uint64_t neighbourOrphans[2];
            LoadOrphans(neighbourAddress, neighbourOrphans);
            const uint neighbourDistances = RawBuffer(scratchState).Load(neighbourAddress + readOffset);

            for(uint other = 0; other < 2; ++other)
            {
                const uint otherDistance = DistanceOf(neighbourDistances, other);
                const bool touches = FaceContacts(orphans[region], neighbourOrphans[other], direction / 2, (direction & 1u) != 0) > 0;

                if(touches && otherDistance != AirRemapUnreached)
                {
                    distance = min(distance, otherDistance + 1);
                }
            }
        }

        updated |= distance << (16 * region);
    }

    RawBuffer(scratchState).Store(address + writeOffset, updated);
}

// A relay donor's weight at level `level`: its sub-faces with the face-adjacent orphan regions one level nearer.
uint RelayWeight(int3 cell, uint64_t orphan, uint level)
{
    uint weight = 0;

    for(uint direction = 0; direction < 6; ++direction)
    {
        const int3 neighbour = cell + AirRemapDirections[direction];

        if(!AirContains(field, neighbour) || !IsDonorCell(neighbour))
        {
            continue;
        }

        const uint neighbourAddress = AirVelocityAddress(field, AirSlotOf(field, neighbour));
        uint64_t neighbourOrphans[2];
        LoadOrphans(neighbourAddress, neighbourOrphans);
        const uint neighbourDistances = RawBuffer(scratchState).Load(neighbourAddress + relayParity * 4);

        for(uint other = 0; other < 2; ++other)
        {
            if(DistanceOf(neighbourDistances, other) == level - 1)
            {
                weight += FaceContacts(orphan, neighbourOrphans[other], direction / 2, (direction & 1u) != 0);
            }
        }
    }

    return weight;
}

// One relay level (relayLevel, from AirRemapRelayLevels down to 1): every region at distance level - 1 gathers, from each face-adjacent
// region at distance `level`, its amount times their shared sub-faces over that donor's relay weight. Donors and receivers are at
// different distances, so no amount is read and written in the same pass; donors keep their (now passed on) amounts, never read again.
[numthreads(64, 1, 1)]
void CSMovingSolidRelayFlow(uint3 thread : SV_DispatchThreadID)
{
    int3 cell;

    if(!OwnedCell(thread.x, cell))
    {
        return;
    }

    const uint address = AirVelocityAddress(field, AirSlotOf(field, cell));
    uint64_t orphans[2];
    LoadOrphans(address, orphans);
    const uint distances = RawBuffer(scratchState).Load(address + relayParity * 4);

    for(uint region = 0; region < 2; ++region)
    {
        if(orphans[region] == 0 || DistanceOf(distances, region) != relayLevel - 1)
        {
            continue;
        }

        float4 gathered = float4(0.0, 0.0, 0.0, 0.0);

        for(uint direction = 0; direction < 6; ++direction)
        {
            const int3 neighbour = cell + AirRemapDirections[direction];

            if(!AirContains(field, neighbour) || !IsDonorCell(neighbour))
            {
                continue;
            }

            const uint neighbourAddress = AirVelocityAddress(field, AirSlotOf(field, neighbour));
            uint64_t neighbourOrphans[2];
            LoadOrphans(neighbourAddress, neighbourOrphans);
            const uint neighbourDistances = RawBuffer(scratchState).Load(neighbourAddress + relayParity * 4);

            for(uint donor = 0; donor < 2; ++donor)
            {
                if(DistanceOf(neighbourDistances, donor) != relayLevel)
                {
                    continue;
                }

                const uint contacts = FaceContacts(orphans[region], neighbourOrphans[donor], direction / 2, (direction & 1u) != 0);

                if(contacts > 0)
                {
                    const uint weight = RelayWeight(neighbour, neighbourOrphans[donor], relayLevel);
                    gathered += LoadOrphanAmount(neighbourAddress, donor) * (float(contacts) / float(weight));
                }
            }
        }

        if(any(gathered != 0.0))
        {
            RWByteAddressBuffer amounts = RawBuffer(region == 0 ? scratchAmount0 : scratchAmount1);
            amounts.Store4(address, asuint(asfloat(amounts.Load4(address)) + gathered));
            LedgerAdd(AirRemapLedgerRelayed, gathered.w);
        }
    }
}

// A newly exposed piece's velocity and ΔT: the mean of its open face neighbours' pieces that are not newly exposed themselves, weighted by
// the open sub-faces (zero with none). Exposed pieces are the only ones this pass writes, and it never reads them.
void FillExposed(int3 cell, uint address, uint piece, uint64_t mask)
{
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    uint total = 0;

    for(uint direction = 0; direction < 6; ++direction)
    {
        const int3 neighbour = cell + AirRemapDirections[direction];

        if(!AirContains(field, neighbour))
        {
            continue;
        }

        const uint neighbourAddress = AirVelocityAddress(field, AirSlotOf(field, neighbour));
        const AirPieces other = LoadPieces(neighbour);
        const uint exposed = IsDonorCell(neighbour) ? RawBuffer(scratchState).Load(neighbourAddress + 8) : 0u;
        const uint64_t masks[2] = {other.piece0, other.piece1};

        for(uint otherPiece = 0; otherPiece < 2; ++otherPiece)
        {
            const uint flag = otherPiece == 0 ? AirRemapExposed0 : AirRemapExposed1;

            if(masks[otherPiece] == 0 || (exposed & flag) != 0)
            {
                continue;
            }

            const uint link = FaceContacts(mask, masks[otherPiece], direction / 2, (direction & 1u) != 0);

            if(link > 0)
            {
                sum += asfloat(RawBuffer(otherPiece == 0 ? velocityUnordered0 : velocityUnordered1).Load4(neighbourAddress)) * float(link);
                total += link;
            }
        }
    }

    if(total == 0)
    {
        LedgerCount(AirRemapLedgerExposedIsolated, 1);
    }

    const float4 value = total > 0 ? sum / float(total) : float4(0.0, 0.0, 0.0, 0.0);
    RawBuffer(piece == 0 ? velocityUnordered0 : velocityUnordered1).Store4(address, asuint(value));
}

// One thread per cell of the dirty boxes grown by one (the receivers: every piece an orphan region can touch). As a receiver, each new
// piece gathers, from every terminal region of its own cell and its six neighbours, that region's amount times their shared sub-faces
// over the region's terminal weight, which is how CSMovingSolidRemapClassify counted it, so a region's shares sum to its amount. As a
// donor (a dirty cell), its regions' outflow and unresolved amounts enter the ledger, and its newly exposed pieces are filled.
[numthreads(64, 1, 1)]
void CSMovingSolidRemapDistribute(uint3 thread : SV_DispatchThreadID)
{
    int3 cell;

    // The relay predicate is read only between the classification and this pass: ready for the next update.
    if(thread.x == 0)
    {
        RawBuffer(predicateUnordered).Store2(0, uint2(0, 0));
    }

    if(!OwnedCell(thread.x, cell))
    {
        return;
    }

    const uint address = AirVelocityAddress(field, AirSlotOf(field, cell));
    const AirPieces own = LoadPieces(cell);
    const bool donorCell = IsDonorCell(cell);

    if(donorCell)
    {
        const uint flags = RawBuffer(scratchState).Load(address + 8);

        if((flags & AirRemapExposed0) != 0)
        {
            FillExposed(cell, address, 0, own.piece0);
        }

        if((flags & AirRemapExposed1) != 0)
        {
            FillExposed(cell, address, 1, own.piece1);
        }
    }

    if(mistUnordered0 == AirMovingSolidNone)
    {
        return;
    }

    // The receiver: the cell itself (source 6) and its six neighbours.
    const uint64_t pieces[2] = {own.piece0, own.piece1};
    float4 gathered[2] = {float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0)};

    for(uint source = 0; source < 7; ++source)
    {
        const bool self = source == 6;
        const int3 donor = self ? cell : cell + AirRemapDirections[source];

        if(!AirContains(field, donor) || !IsDonorCell(donor))
        {
            continue;
        }

        const uint donorAddress = AirVelocityAddress(field, AirSlotOf(field, donor));
        uint64_t orphans[2];
        LoadOrphans(donorAddress, orphans);

        if(orphans[0] == 0 && orphans[1] == 0)
        {
            continue;
        }

        const uint4 state = RawBuffer(scratchState).Load4(donorAddress);
        const uint distances = relayParity == 0 ? state.x : state.y;

        for(uint region = 0; region < 2; ++region)
        {
            if(orphans[region] == 0 || DistanceOf(distances, region) != 0)
            {
                continue;
            }

            const float weight = float(DistanceOf(state.w, region));
            const float4 amount = LoadOrphanAmount(donorAddress, region);

            for(uint piece = 0; piece < 2; ++piece)
            {
                if(pieces[piece] == 0)
                {
                    continue;
                }

                // The donor lies in direction `source` from this cell, so this cell lies in the opposite direction from the donor.
                const uint axis = source / 2;
                const bool towardReceiver = (source & 1u) == 0;
                const uint contacts = self ? InteriorContacts(orphans[region], pieces[piece]) :
                    FaceContacts(orphans[region], pieces[piece], axis, towardReceiver);

                if(contacts > 0)
                {
                    gathered[piece] += amount * (float(contacts) / weight);
                }
            }
        }
    }

    for(uint piece = 0; piece < 2; ++piece)
    {
        if(any(gathered[piece] != 0.0))
        {
            RWByteAddressBuffer mist = RawBuffer(piece == 0 ? mistUnordered0 : mistUnordered1);
            float volume = float(AirSubcellCount(pieces[piece]));

            // F2b.2: a continuous update's densities are per effective volume at the step's start, G - S + D (CSBoundaryGather wrote both).
            if(continuousRemap != 0)
            {
                const uint slotIndex = address / 16;
                volume += asfloat(RawBuffer(wallSourceUnordered).Load(slotIndex * 8 + piece * 4)) -
                    asfloat(RawBuffer(effectiveOffsetUnordered).Load(slotIndex * 8 + piece * 4));
            }

            mist.Store4(address, asuint(asfloat(mist.Load4(address)) + (volume > 0.0 ? gathered[piece] / volume : float4(0.0, 0.0, 0.0, 0.0))));
            LedgerAdd(AirRemapLedgerDelivered, gathered[piece].w);
        }
    }

    // The donor's own accounting: what its terminal regions sent across the window's boundary, and what reached nothing.
    if(!donorCell)
    {
        return;
    }

    uint64_t orphans[2];
    LoadOrphans(address, orphans);
    const uint4 state = RawBuffer(scratchState).Load4(address);
    const uint distances = relayParity == 0 ? state.x : state.y;

    for(uint region = 0; region < 2; ++region)
    {
        if(orphans[region] == 0)
        {
            continue;
        }

        const uint distance = DistanceOf(distances, region);
        const float4 amount = LoadOrphanAmount(address, region);

        if(distance == 0)
        {
            const uint outside = (state.z >> (region == 0 ? AirRemapOutsideShift0 : AirRemapOutsideShift1)) & 0xFFu;

            if(outside > 0)
            {
                LedgerAdd(AirRemapLedgerOutflow, amount.w * (float(outside) / float(DistanceOf(state.w, region))));
            }
        }
        else if(distance == AirRemapUnreached)
        {
            LedgerAdd(AirRemapLedgerUnresolved, amount.w);
            LedgerCount(AirRemapLedgerUnresolvedRegions, 1);
        }
        else
        {
            LedgerCount(AirRemapLedgerRelayedRegions, 1);
        }
    }
}

// ------------------------------------------------------------------------------------------------------- F2b.1 moving-boundary volumes
//
// Documentation/Air/AirFogDisplacement4F3Investigation.md, F2b.0 and F2b.1. Each piece's control volume C(X) at pose set X is its own fluid
// plus the fluid of the loose sub-cells it owns: carved (a moving solid overlaps them) or dropped by the two-piece limit, but statically
// representable. In-cell ownership grows piece 0 and piece 1 through loose sub-cells one face step at a time (piece 0 first on a tie); what
// neither reaches (U) belongs to the face-neighbour cells' pieces by touching sub-faces, one cell of reach, or is unowned (counted).
// Fluid is 1 - coverage, the coverage measured on AirBoundaryLines^2 lines along x per sub-cell, each intersected exactly with every solid of
// the pose list; an isolated solid's partial coverage is corrected so its total equals its exact volume.
// Outputs, per piece: S = G - C(X_new) (the effective-volume offset at the new pose) and D = C(X_anchor) - C(X_new) (the volume the piece
// loses through its open faces over the step: 0 for stationary or jumped solids, whose anchor is their new pose). F2b.1 only prepares them:
// no transport, remap or pressure reads them yet.

static const uint AirBoundaryLines = 8;
static const uint AirBoundaryIsolated = 1u;
static const uint AirBoundaryMaximumCandidates = 8;
static const uint AirBoundaryGroupRow = 32768;
static const float AirBoundaryEmpty = 1.0e30;
// Correction sums per solid (stride 32 B): the coverage sum S1 = sum c at 8 and the weight sum S2 = sum c (1 - c) at 16 (int64, 2^32 per
// sub-cell volume); 0 and 24 unused.
static const uint AirCorrectionStride = 32;
static const float AirCorrectionScale = 4294967296.0;
// Diagnostics (cumulative): corrected sub-cells (uint) at 0, the largest |correction| (float bits) at 4, the summed |correction| at 8,
// unowned fluid at the anchor and new poses at 16 and 24, the summed source and summed |source| at 32 and 40 (int64, AirRemapFixedScale),
// candidate overflows (uint) at 48.
static const uint AirBoundaryCorrectedSubcells = 0;
static const uint AirBoundaryLargestCorrection = 4;
static const uint AirBoundaryTotalCorrection = 8;
static const uint AirBoundaryUnownedAnchor = 16;
static const uint AirBoundaryUnownedNew = 24;
static const uint AirBoundarySourceNet = 32;
static const uint AirBoundarySourceGross = 40;
static const uint AirBoundaryCandidateOverflow = 48;

void BoundaryAdd(uint offset, float amount)
{
    if(amount != 0.0)
    {
        RawBuffer(boundaryDiagnosticsUnordered).InterlockedAdd64(offset, int64_t(amount * AirRemapFixedScale));
    }
}

// The x-interval of the line (t, y, z) (sub-cell-local coordinates) inside a sphere; empty: start >= end.
float2 SphereInterval(float3 centre, float radius, float y, float z)
{
    const float perpendicular = (y - centre.y) * (y - centre.y) + (z - centre.z) * (z - centre.z);

    if(perpendicular >= radius * radius)
    {
        return float2(AirBoundaryEmpty, -AirBoundaryEmpty);
    }

    const float halfChord = sqrt(radius * radius - perpendicular);
    return float2(centre.x - halfChord, centre.x + halfChord);
}

// Inside the infinite cylinder about a-b, then clipped to the slab between its end planes.
float2 CylinderInterval(float3 a, float3 b, float radius, float y, float z)
{
    const float3 axis = b - a;
    const float length = sqrt(dot(axis, axis));

    if(length < 1.0e-9)
    {
        return float2(AirBoundaryEmpty, -AirBoundaryEmpty);
    }

    const float3 unit = axis / length;
    const float3 origin = float3(0.0, y, z) - a;
    const float along = dot(origin, unit);
    const float3 originPerpendicular = origin - along * unit;
    const float3 directionPerpendicular = float3(1.0, 0.0, 0.0) - unit * unit.x;
    const float coefficientA = dot(directionPerpendicular, directionPerpendicular);
    const float coefficientB = 2.0 * dot(originPerpendicular, directionPerpendicular);
    const float coefficientC = dot(originPerpendicular, originPerpendicular) - radius * radius;
    float2 interval;

    if(coefficientA < 1.0e-12)
    {
        interval = coefficientC < 0.0 ? float2(-AirBoundaryEmpty, AirBoundaryEmpty) : float2(AirBoundaryEmpty, -AirBoundaryEmpty);
    }
    else
    {
        const float discriminant = coefficientB * coefficientB - 4.0 * coefficientA * coefficientC;

        if(discriminant <= 0.0)
        {
            return float2(AirBoundaryEmpty, -AirBoundaryEmpty);
        }

        // The cancellation-free roots: q = -(B + sign(B) sqrt(disc)) / 2, roots q / A and C / q (no difference of nearly equal values when
        // a line grazes the cylinder).
        const float root = sqrt(discriminant);
        const float q = -0.5 * (coefficientB + (coefficientB >= 0.0 ? root : -root));
        const float first = q / coefficientA;
        const float second = abs(q) > 1.0e-20 ? coefficientC / q : first;
        interval = float2(min(first, second), max(first, second));
    }

    if(abs(unit.x) < 1.0e-12)
    {
        return along > 0.0 && along < length ? interval : float2(AirBoundaryEmpty, -AirBoundaryEmpty);
    }

    const float first = -along / unit.x;
    const float second = (length - along) / unit.x;
    return float2(max(interval.x, min(first, second)), min(interval.y, max(first, second)));
}

// The x-interval of the line (t, y, z) inside `solid`, in coordinates relative to `origin` (the sub-cell's low corner).
float2 LineInterval(AirMovingSolid solid, float3 origin, float y, float z)
{
    if(solid.type == AirMovingSolidSphere)
    {
        return SphereInterval(solid.center - origin, solid.radius, y, z);
    }

    if(solid.type == AirMovingSolidCapsule)
    {
        const float3 a = solid.segmentA - origin;
        const float3 b = solid.segmentB - origin;
        float2 result = SphereInterval(a, solid.radius, y, z);
        const float2 parts[2] = {SphereInterval(b, solid.radius, y, z), CylinderInterval(a, b, solid.radius, y, z)};

        // The capsule is convex: its line intersection is one interval, the union's extent.
        for(uint part = 0; part < 2; ++part)
        {
            if(parts[part].x < parts[part].y)
            {
                result = result.x < result.y ? float2(min(result.x, parts[part].x), max(result.y, parts[part].y)) : parts[part];
            }
        }

        return result;
    }

    const float3 offset = float3(0.0, y, z) - (solid.center - origin);
    const float3 axes[3] = {solid.axisX, solid.axisY, solid.axisZ};
    float2 result = float2(-AirBoundaryEmpty, AirBoundaryEmpty);

    for(uint axis = 0; axis < 3; ++axis)
    {
        const float slope = axes[axis].x;
        const float position = dot(offset, axes[axis]);
        const float halfExtent = solid.halfExtents[axis];

        if(abs(slope) < 1.0e-12)
        {
            if(abs(position) >= halfExtent)
            {
                return float2(AirBoundaryEmpty, -AirBoundaryEmpty);
            }

            continue;
        }

        const float first = (-halfExtent - position) / slope;
        const float second = (halfExtent - position) / slope;
        result = float2(max(result.x, min(first, second)), min(result.y, max(first, second)));
    }

    return result;
}

// A sub-cell's raw coverage by a pose list (the union of its solids), the number of solids whose bounds reach it, and the last of them.
struct AirBoundaryCoverage
{
    float covered;
    uint solids;
    uint lastSolid;
};

AirBoundaryCoverage RawCoverage(float3 low, uint offset, uint count)
{
    const float size = field.cellSize / float(AirSubcellsPerAxis);
    const float3 high = low + size;
    AirBoundaryCoverage coverage;
    coverage.covered = 0.0;
    coverage.solids = 0;
    coverage.lastSolid = 0;
    uint candidates[AirBoundaryMaximumCandidates];

    for(uint index = offset; index < offset + count; ++index)
    {
        const AirMovingSolid solid = movingSolids[index];

        if(any(high <= solid.boundsMinimum) || any(low >= solid.boundsMaximum))
        {
            continue;
        }

        if(coverage.solids == AirBoundaryMaximumCandidates)
        {
            uint ignored;
            RawBuffer(boundaryDiagnosticsUnordered).InterlockedAdd(AirBoundaryCandidateOverflow, 1, ignored);
            break;
        }

        candidates[coverage.solids] = index;
        coverage.solids += 1;
        coverage.lastSolid = index;
    }

    if(coverage.solids == 0)
    {
        return coverage;
    }

    float total = 0.0;

    for(uint sampleLine = 0; sampleLine < AirBoundaryLines * AirBoundaryLines; ++sampleLine)
    {
        const float y = (float(sampleLine % AirBoundaryLines) + 0.5) / float(AirBoundaryLines) * size;
        const float z = (float(sampleLine / AirBoundaryLines) + 0.5) / float(AirBoundaryLines) * size;
        float2 intervals[AirBoundaryMaximumCandidates];

        // Each solid's interval clipped to the sub-cell, kept sorted by start (insertion), then swept for the union's length.
        for(uint candidate = 0; candidate < coverage.solids; ++candidate)
        {
            const float2 raw = LineInterval(movingSolids[candidates[candidate]], low, y, z);
            const float start = clamp(raw.x, 0.0, size);
            const float2 clipped = float2(start, max(clamp(raw.y, 0.0, size), start));
            uint slot = candidate;

            while(slot > 0 && intervals[slot - 1].x > clipped.x)
            {
                intervals[slot] = intervals[slot - 1];
                --slot;
            }

            intervals[slot] = clipped;
        }

        float reach = 0.0;

        for(uint candidate = 0; candidate < coverage.solids; ++candidate)
        {
            total += max(intervals[candidate].y - max(intervals[candidate].x, reach), 0.0);
            reach = max(reach, intervals[candidate].y);
        }
    }

    coverage.covered = total / (float(AirBoundaryLines * AirBoundaryLines) * size);
    return coverage;
}

// The correction strength mu of an isolated solid: c' = c + mu c (1 - c) brings its summed coverage to its exact volume. The weight c (1 - c)
// vanishes at empty and full sub-cells, so the correction is continuous in c (no classification threshold), adds nothing to a sub-cell a
// solid merely touches, and lives where the line quadrature errs: the partially covered sub-cells. |mu| <= 1 keeps c' in [0, 1]; a larger
// one (never expected: the quadrature's error is a few percent at most) is clamped and the total is then not exact.
float SolidCorrection(uint index)
{
    RWByteAddressBuffer sums = RawBuffer(correctionUnordered);
    const uint address = index * AirCorrectionStride;
    const double coverage = double(sums.Load<int64_t>(address + 8)) / double(AirCorrectionScale);
    const double weight = double(sums.Load<int64_t>(address + 16)) / double(AirCorrectionScale);

    if(weight <= 0.0)
    {
        return 0.0;
    }

    return clamp(float((double(movingSolids[index].exactVolume) - coverage) / weight), -1.0, 1.0);
}

// A sub-cell's fluid fraction for a pose list: 1 - coverage, the coverage corrected when exactly one isolated solid reaches the sub-cell.
float FluidFraction(float3 low, uint offset, uint count, bool record)
{
    const AirBoundaryCoverage coverage = RawCoverage(low, offset, count);
    float covered = coverage.covered;

    if(covered >= 1.0)
    {
        return 0.0;
    }

    if(covered <= 0.0)
    {
        return 1.0;
    }

    const bool isolated = coverage.solids == 1 && (movingSolids[coverage.lastSolid].boundaryFlags & AirBoundaryIsolated) != 0;

    if(isolated)
    {
        const float corrected = saturate(covered + SolidCorrection(coverage.lastSolid) * covered * (1.0 - covered));

        if(record && corrected != covered)
        {
            uint ignored;
            RWByteAddressBuffer diagnostics = RawBuffer(boundaryDiagnosticsUnordered);
            diagnostics.InterlockedAdd(AirBoundaryCorrectedSubcells, 1, ignored);
            diagnostics.InterlockedMax(AirBoundaryLargestCorrection, asuint(abs(corrected - covered)), ignored);
            BoundaryAdd(AirBoundaryTotalCorrection, abs(corrected - covered));
        }

        covered = corrected;
    }

    return 1.0 - covered;
}

// One thread per solid entry: its correction sums cleared before they are accumulated.
[numthreads(64, 1, 1)]
void CSBoundaryCorrectionClear(uint3 thread : SV_DispatchThreadID)
{
    if(thread.x < totalCells)
    {
        RawBuffer(correctionUnordered).Store4(thread.x * AirCorrectionStride, uint4(0, 0, 0, 0));
        RawBuffer(correctionUnordered).Store4(thread.x * AirCorrectionStride + 16, uint4(0, 0, 0, 0));
    }
}

// One thread per sub-cell of each isolated solid's bounds (the boxes are in sub-cells; donorMinimum[box].x names the solid's entry): the
// solid's own coverage, alone, summed with its correction weight (integer atomics: the sums do not depend on thread order).
[numthreads(64, 1, 1)]
void CSBoundaryCorrectionSums(uint3 thread : SV_DispatchThreadID)
{
    int3 subcell;
    uint box;

    if(thread.x >= totalCells || !DirtyCell(thread.x, subcell, box))
    {
        return;
    }

    const uint index = uint(donorMinimum[box].x);
    const float size = field.cellSize / float(AirSubcellsPerAxis);
    const float covered = RawCoverage(float3(subcell) * size, index, 1).covered;
    RWByteAddressBuffer sums = RawBuffer(correctionUnordered);
    const uint address = index * AirCorrectionStride;

    if(covered > 0.0)
    {
        sums.InterlockedAdd64(address + 8, int64_t(double(covered) * double(AirCorrectionScale)));
        sums.InterlockedAdd64(address + 16, int64_t(double(covered) * double(1.0 - covered) * double(AirCorrectionScale)));
    }
}

// The loose sub-cells a cell's control regions may own: free statically and statically representable (a sub-cell static truncation already
// drops is never fluid of any piece), but in no current piece.
uint64_t LooseSubcells(int3 cell, AirPieces pieces)
{
    const uint64_t staticFree = FreeSubcells(cell);
    const AirPieces staticPieces = AirLabelPieces(staticFree);
    return staticFree & (staticPieces.piece0 | staticPieces.piece1) & ~(pieces.piece0 | pieces.piece1);
}

// In-cell ownership: regions 0 and 1 grown through the loose sub-cells, one face step a round, piece 0 first on a tie; the rest unreached.
void BoundaryRegions(AirPieces pieces, uint64_t loose, out uint64_t region0, out uint64_t region1, out uint64_t unreached)
{
    region0 = pieces.piece0;
    region1 = pieces.piece1;

    // A path through 64 sub-cells takes at most 63 rounds.
    for(uint growth = 0; growth < 64; ++growth)
    {
        const uint64_t claimed = region0 | region1;
        const uint64_t grown0 = region0 != 0 ? AirDilateSubcells(region0) & loose & ~claimed : 0;
        const uint64_t grown1 = region1 != 0 ? AirDilateSubcells(region1) & loose & ~claimed & ~grown0 : 0;

        if((grown0 | grown1) == 0)
        {
            break;
        }

        region0 |= grown0;
        region1 |= grown1;
    }

    unreached = loose & ~(region0 | region1);
}

// F2b.2: per boundary cell, a record (stride AirBoundaryRecordStride), indexed by the cell's position in the pass's box list:
//   0   old piece masks (piece 0 low, high; piece 1 low, high)                  - CSBoundarySnapshot, before the pieces change
//   16  the old unreached group's density (its owners' densities by contact)   - CSBoundarySnapshot
//   32  F(U, anchor), F(U, new), U's mask (low, high)                          - CSBoundaryVolumes
//   48  F(R0, anchor), F(R1, anchor), F(R0, new), F(R1, new)                   - CSBoundaryVolumes
//   64  mass to region 0, to region 1, to U (float4 each, mist units)          - CSBoundaryVolumes (continuous)
static const uint AirBoundaryRecordStride = 112;

// A cell's index in the pass's box list (its first box), which is its record; false when no box holds it.
bool BoundaryRecordIndex(int3 cell, out uint index)
{
    index = 0;

    for(uint box = 0; box < boxCount; ++box)
    {
        const int3 local = cell - boxMinimum[box].xyz;
        const int3 size = boxSize[box].xyz;

        if(all(local >= 0) && all(local < size))
        {
            index = uint(boxMinimum[box].w) + uint(local.x + size.x * (local.y + size.y * local.z));
            return true;
        }
    }

    return false;
}

uint RecordAddress(uint index)
{
    return index * AirBoundaryRecordStride;
}

// F2b.2a: what an unreached group of a neighbouring cell may belong to in `cell`: its pieces, or (byRegions) its regions, each piece with
// the loose sub-cells it reaches in the cell. An unreached group touching no neighbouring piece (fluid the solid carved beyond a piece's
// direct reach) belongs to the neighbours' regions instead: connected fluid through statically free sub-cells, still within one cell.
AirPieces OwnerMasks(int3 cell, bool byRegions)
{
    AirPieces masks = LoadPieces(cell);

    if(byRegions)
    {
        uint64_t region0;
        uint64_t region1;
        uint64_t unreached;
        BoundaryRegions(masks, LooseSubcells(cell, masks), region0, region1, unreached);
        masks.piece0 = region0;
        masks.piece1 = region1;
    }

    return masks;
}

// The density of an unreached group's owners at one tier (pieces, or regions); 0 when none touches it.
float4 UnreachedDensityOf(int3 cell, uint64_t unreached, bool byRegions)
{
    float4 weighted = float4(0.0, 0.0, 0.0, 0.0);
    uint total = 0;

    for(uint direction = 0; direction < 6; ++direction)
    {
        const int3 neighbour = cell + AirRemapDirections[direction];

        if(!AirContains(field, neighbour))
        {
            continue;
        }

        const AirPieces other = OwnerMasks(neighbour, byRegions);
        const uint address = AirVelocityAddress(field, AirSlotOf(field, neighbour));
        const uint64_t masks[2] = {other.piece0, other.piece1};

        for(uint piece = 0; piece < 2; ++piece)
        {
            const uint contacts = masks[piece] != 0 ? FaceContacts(unreached, masks[piece], direction / 2, (direction & 1u) != 0) : 0;

            if(contacts > 0)
            {
                weighted += asfloat(RawBuffer(piece == 0 ? mistUnordered0 : mistUnordered1).Load4(address)) * float(contacts);
                total += contacts;
            }
        }
    }

    return total > 0 ? weighted / float(total) : float4(0.0, 0.0, 0.0, 0.0);
}

// The density the mist of an unreached group belongs to: its owners' (the neighbours' pieces, else their regions) by touching sub-faces;
// 0 when unowned.
float4 UnreachedDensity(int3 cell, uint64_t unreached)
{
    float4 density = UnreachedDensityOf(cell, unreached, false);
    return any(density != 0.0) ? density : UnreachedDensityOf(cell, unreached, true);
}

// One thread per boundary cell, before the pieces pass: the old piece masks and the old unreached group's density, read while every
// piece and every density is still the old one (the pieces pass rewrites both).
[numthreads(64, 1, 1)]
void CSBoundarySnapshot(uint3 thread : SV_DispatchThreadID)
{
    int3 cell;

    if(!OwnedCell(thread.x, cell))
    {
        return;
    }

    uint index;
    BoundaryRecordIndex(cell, index);
    const AirPieces pieces = LoadPieces(cell);
    float4 density = float4(0.0, 0.0, 0.0, 0.0);

    if(mistUnordered0 != AirMovingSolidNone)
    {
        uint64_t region0;
        uint64_t region1;
        uint64_t unreached;
        BoundaryRegions(pieces, LooseSubcells(cell, pieces), region0, region1, unreached);
        density = unreached != 0 ? UnreachedDensity(cell, unreached) : density;
    }

    const uint address = RecordAddress(index);
    RawBuffer(recordUnordered).Store4(address, uint4(uint(pieces.piece0), uint(pieces.piece0 >> 32), uint(pieces.piece1), uint(pieces.piece1 >> 32)));
    RawBuffer(recordUnordered).Store4(address + 16, asuint(density));
}

// One thread per boundary cell, before a discontinuous update's pieces pass: each piece's stored density (mass per effective volume) into
// F1b's units (mass per sub-cell), rho G_eff / G with G_eff = G - S, so F1b's geometric remap moves the true masses. CSBoundaryGather
// converts back with the new effective volumes.
// A piece's density per effective volume G - S converted to per geometric volume G (mass unchanged).
float4 GeometricDensity(float4 stored, uint64_t mask, float offset)
{
    const float geometric = float(AirSubcellCount(mask));
    return stored * ((geometric - offset) / geometric);
}

[numthreads(64, 1, 1)]
void CSBoundaryToGeometric(uint3 thread : SV_DispatchThreadID)
{
    int3 cell;

    if(!OwnedCell(thread.x, cell) || mistUnordered0 == AirMovingSolidNone)
    {
        return;
    }

    const uint address = AirVelocityAddress(field, AirSlotOf(field, cell));
    const AirPieces pieces = LoadPieces(cell);
    const float2 offsets = asfloat(RawBuffer(effectiveOffsetUnordered).Load2((address / 16) * 8));
    const uint64_t masks[2] = {pieces.piece0, pieces.piece1};
    float4 stored[2] = {float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0)};

    for(uint piece = 0; piece < 2; ++piece)
    {
        if(masks[piece] == 0 || offsets[piece] == 0.0)
        {
            continue;
        }

        RWByteAddressBuffer mist = RawBuffer(piece == 0 ? mistUnordered0 : mistUnordered1);
        stored[piece] = asfloat(mist.Load4(address));
        mist.Store4(address, asuint(GeometricDensity(stored[piece], masks[piece], offsets[piece])));
    }

    // Within the boundary boxes, the record keeps what the conversion started from, so the gather can restore a piece's exact stored
    // value when neither its sub-cells, its offset nor (through F1b) its mist changed: the round trip would otherwise round it.
    uint index;

    if(recordUnordered != AirMovingSolidNone && BoundaryRecordIndex(cell, index))
    {
        const uint record = RecordAddress(index);
        RawBuffer(recordUnordered).Store4(record, uint4(uint(pieces.piece0), uint(pieces.piece0 >> 32), uint(pieces.piece1), uint(pieces.piece1 >> 32)));
        RawBuffer(recordUnordered).Store4(record + 64, asuint(stored[0]));
        RawBuffer(recordUnordered).Store4(record + 80, asuint(stored[1]));
        RawBuffer(recordUnordered).Store4(record + 96, uint4(asuint(offsets), 0, 0));
    }
}

// The sub-faces an unreached group `unreached` of `cell` shares with its six neighbours' owner masks at one tier.
uint UnreachedContacts(int3 cell, uint64_t unreached, bool byRegions)
{
    uint weight = 0;

    for(uint direction = 0; direction < 6; ++direction)
    {
        const int3 neighbour = cell + AirRemapDirections[direction];

        if(AirContains(field, neighbour))
        {
            const AirPieces other = OwnerMasks(neighbour, byRegions);
            weight += FaceContacts(unreached, other.piece0, direction / 2, (direction & 1u) != 0) +
                FaceContacts(unreached, other.piece1, direction / 2, (direction & 1u) != 0);
        }
    }

    return weight;
}

// An unreached group's ownership weight: its contacts with the neighbours' pieces; F2b.2a: when there are none, with their regions
// (byRegions). 0 only when it touches neither: unowned.
uint UnreachedWeight(int3 cell, uint64_t unreached, out bool byRegions)
{
    byRegions = false;
    const uint weight = UnreachedContacts(cell, unreached, false);

    if(weight > 0)
    {
        return weight;
    }

    byRegions = true;
    return UnreachedContacts(cell, unreached, true);
}

groupshared float3 g_boundaryAnchor[64];
groupshared float3 g_boundaryNew[64];
groupshared float4 g_boundaryMass[3][64];
groupshared uint g_boundaryUnreachedOwned;

// One group per boundary cell, one thread per sub-cell: each new group's fluid at the anchor and new poses (regions 0 and 1, the unreached
// group U), summed by a fixed tree, into the cell's record. In a continuous update also the mass each new group receives: every sub-cell
// keeps the mist its old owner held there, the owner's density times its fluid at the anchor (the old pose of every changed solid), so a
// uniform density stays uniform; the mist of an unowned U is an orphan for F1b's push (the unowned fallback, ledgered).
[numthreads(64, 1, 1)]
void CSBoundaryVolumes(uint3 group : SV_GroupID, uint threadIndex : SV_GroupIndex)
{
    int3 cell;
    uint box;
    // The groups come in rows of AirBoundaryGroupRow (AirField.cpp, BoundaryGroupRow).
    const uint cellIndex = group.x + group.y * AirBoundaryGroupRow;
    const bool owned = cellIndex < totalCells && DirtyCell(cellIndex, cell, box) && AirContains(field, cell) && FirstBoxOf(cell, box);
    const bool remapMass = owned && continuousRemap != 0 && mistUnordered0 != AirMovingSolidNone;
    float3 anchorFluid = float3(0.0, 0.0, 0.0);
    float3 newFluid = float3(0.0, 0.0, 0.0);
    float4 mass[3] = {float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0)};
    uint64_t unreached = 0;
    uint index = 0;

    if(owned)
    {
        BoundaryRecordIndex(cell, index);
        const AirPieces pieces = LoadPieces(cell);
        uint64_t region0;
        uint64_t region1;
        BoundaryRegions(pieces, LooseSubcells(cell, pieces), region0, region1, unreached);
        const uint64_t bit = uint64_t(1) << threadIndex;
        const uint member = (region0 & bit) != 0 ? 0u : ((region1 & bit) != 0 ? 1u : ((unreached & bit) != 0 ? 2u : 3u));

        if(member < 3)
        {
            const float size = field.cellSize / float(AirSubcellsPerAxis);
            const float3 low = float3(cell) * field.cellSize + float3(threadIndex & 3u, (threadIndex >> 2) & 3u, threadIndex >> 4) * size;
            anchorFluid[member] = FluidFraction(low, anchorOffset, anchorCount, false);
            newFluid[member] = FluidFraction(low, 0, solidCount, true);

            // The old owner of this sub-cell (the old regions, from the snapshot's pieces) and its density: the old pieces' densities are
            // still in place (nothing writes the mist before CSBoundaryGather in a continuous update).
            if(remapMass && anchorFluid[member] > 0.0)
            {
                const uint4 oldWords = RawBuffer(recordUnordered).Load4(RecordAddress(index));
                AirPieces old;
                old.piece0 = MaskOf(oldWords.x, oldWords.y);
                old.piece1 = MaskOf(oldWords.z, oldWords.w);
                uint64_t oldRegion0;
                uint64_t oldRegion1;
                uint64_t oldUnreached;
                BoundaryRegions(old, LooseSubcells(cell, old), oldRegion0, oldRegion1, oldUnreached);
                const uint address = AirVelocityAddress(field, AirSlotOf(field, cell));
                float4 density = float4(0.0, 0.0, 0.0, 0.0);

                if((oldRegion0 & bit) != 0)
                {
                    density = asfloat(RawBuffer(mistUnordered0).Load4(address));
                }
                else if((oldRegion1 & bit) != 0)
                {
                    density = asfloat(RawBuffer(mistUnordered1).Load4(address));
                }
                else if((oldUnreached & bit) != 0)
                {
                    density = asfloat(RawBuffer(recordUnordered).Load4(RecordAddress(index) + 16));
                }

                mass[member] = density * anchorFluid[member];
            }
        }
    }

    g_boundaryAnchor[threadIndex] = anchorFluid;
    g_boundaryNew[threadIndex] = newFluid;

    for(uint member = 0; member < 3; ++member)
    {
        g_boundaryMass[member][threadIndex] = mass[member];
    }

    GroupMemoryBarrierWithGroupSync();

    for(uint stride = 32; stride > 0; stride >>= 1)
    {
        if(threadIndex < stride)
        {
            g_boundaryAnchor[threadIndex] += g_boundaryAnchor[threadIndex + stride];
            g_boundaryNew[threadIndex] += g_boundaryNew[threadIndex + stride];

            for(uint member = 0; member < 3; ++member)
            {
                g_boundaryMass[member][threadIndex] += g_boundaryMass[member][threadIndex + stride];
            }
        }

        GroupMemoryBarrierWithGroupSync();
    }

    if(!owned || threadIndex != 0)
    {
        return;
    }

    const uint record = RecordAddress(index);
    const float3 anchorSums = g_boundaryAnchor[0];
    const float3 newSums = g_boundaryNew[0];
    RawBuffer(recordUnordered).Store4(record + 32, uint4(asuint(anchorSums.z), asuint(newSums.z), uint(unreached), uint(unreached >> 32)));
    RawBuffer(recordUnordered).Store4(record + 48, asuint(float4(anchorSums.x, anchorSums.y, newSums.x, newSums.y)));

    if(continuousRemap == 0)
    {
        return;
    }

    // Continuous: the masses, and F1b's orphan scratch for this cell (rewritten for every boundary cell: the push reads it for any of them).
    // An unowned U's mass is the orphan; the exposed flags the pieces pass wrote for a dirty cell are kept.
    float4 unreachedMass = g_boundaryMass[2][0];
    uint64_t orphan = 0;
    float4 orphanAmount = float4(0.0, 0.0, 0.0, 0.0);

    bool unreachedByRegions;

    if(unreached != 0 && UnreachedWeight(cell, unreached, unreachedByRegions) == 0)
    {
        orphan = unreached;
        orphanAmount = unreachedMass;
        unreachedMass = float4(0.0, 0.0, 0.0, 0.0);
        LedgerAdd(AirRemapLedgerUnowned, orphanAmount.w);
    }

    RawBuffer(recordUnordered).Store4(record + 64, asuint(g_boundaryMass[0][0]));
    RawBuffer(recordUnordered).Store4(record + 80, asuint(g_boundaryMass[1][0]));
    RawBuffer(recordUnordered).Store4(record + 96, asuint(unreachedMass));

    const uint address = AirVelocityAddress(field, AirSlotOf(field, cell));
    const uint flags = IsDonorCell(cell) ? RawBuffer(scratchState).Load(address + 8) & (AirRemapExposed0 | AirRemapExposed1) : 0u;
    const uint unreachedDistances = AirRemapUnreached | (AirRemapUnreached << 16);
    RawBuffer(scratchMasks).Store4(address, uint4(uint(orphan), uint(orphan >> 32), 0, 0));
    RawBuffer(scratchState).Store4(address, uint4(unreachedDistances, unreachedDistances, flags, 0));

    if(mistUnordered0 != AirMovingSolidNone)
    {
        RawBuffer(scratchAmount0).Store4(address, asuint(orphanAmount));
    }
}

// Whether `cell` is in the pass's box list (the boundary boxes): only those hold this update's records.
bool InPassBoxes(int3 cell)
{
    uint index;
    return BoundaryRecordIndex(cell, index);
}

// A cell outside the pass's boxes (at least three cells from any changed solid, so its coverage is the same at both poses): its unreached
// group's fluid and mask, in the record's layout (fluid twice: anchor and new). It holds one only beside a solid that did not change.
uint4 UnreachedOutsideBoxes(int3 cell)
{
    const AirPieces pieces = LoadPieces(cell);
    const uint64_t loose = LooseSubcells(cell, pieces);

    if(loose == 0)
    {
        return uint4(0, 0, 0, 0);
    }

    uint64_t region0;
    uint64_t region1;
    uint64_t unreached;
    BoundaryRegions(pieces, loose, region0, region1, unreached);
    float fluid = 0.0;
    const float size = field.cellSize / float(AirSubcellsPerAxis);

    for(uint bit = 0; bit < 64; ++bit)
    {
        if(((unreached >> bit) & 1u) != 0)
        {
            const float3 low = float3(cell) * field.cellSize + float3(bit & 3u, (bit >> 2) & 3u, bit >> 4) * size;
            fluid += FluidFraction(low, 0, solidCount, false);
        }
    }

    return uint4(asuint(fluid), asuint(fluid), uint(unreached), uint(unreached >> 32));
}

// One thread per boundary cell: each piece's control volume at the anchor and new poses (its regions' fluid plus the neighbours' unreached
// groups' fluid in proportion to the sub-faces they share with it), then S and D. The mist:
//   - continuous: the mass its regions received plus its shares of the neighbours' U masses (a U outside the boxes, beside an unchanged
//     solid, keeps this piece's own old density: neither its fluid nor its ownership changed), over C(anchor): the step's starting density;
//   - discontinuous: F1b's result (mass per sub-cell, after CSBoundaryToGeometric) back to density over the new effective volume.
// A cell's own unreached group with no neighbour piece to belong to is counted as unowned.
[numthreads(64, 1, 1)]
void CSBoundaryGather(uint3 thread : SV_DispatchThreadID)
{
    int3 cell;

    if(!OwnedCell(thread.x, cell))
    {
        return;
    }

    uint index;
    BoundaryRecordIndex(cell, index);
    const uint record = RecordAddress(index);
    const uint address = AirVelocityAddress(field, AirSlotOf(field, cell));
    const AirPieces own = LoadPieces(cell);
    const float4 regions = asfloat(RawBuffer(recordUnordered).Load4(record + 48));
    float2 anchorVolume = regions.xy;
    float2 newVolume = regions.zw;
    const uint4 ownUnreached = RawBuffer(recordUnordered).Load4(record + 32);
    const uint64_t ownMask = MaskOf(ownUnreached.z, ownUnreached.w);
    const bool remapMass = continuousRemap != 0 && mistUnordered0 != AirMovingSolidNone;
    float4 mass[2] = {float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0)};
    float4 oldDensity[2] = {float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0)};

    if(remapMass)
    {
        mass[0] = asfloat(RawBuffer(recordUnordered).Load4(record + 64));
        mass[1] = asfloat(RawBuffer(recordUnordered).Load4(record + 80));
        oldDensity[0] = asfloat(RawBuffer(mistUnordered0).Load4(address));
        oldDensity[1] = asfloat(RawBuffer(mistUnordered1).Load4(address));
    }

    bool ownByRegions;

    if(ownMask != 0 && UnreachedWeight(cell, ownMask, ownByRegions) == 0)
    {
        BoundaryAdd(AirBoundaryUnownedAnchor, asfloat(ownUnreached.x));
        BoundaryAdd(AirBoundaryUnownedNew, asfloat(ownUnreached.y));
    }

    const uint64_t pieces[2] = {own.piece0, own.piece1};
    // F2b.2a: this cell's regions, the owner masks of a neighbouring unreached group that touches no piece.
    const AirPieces ownRegions = OwnerMasks(cell, true);
    const uint64_t regionMasks[2] = {ownRegions.piece0, ownRegions.piece1};

    for(uint direction = 0; direction < 6; ++direction)
    {
        const int3 neighbour = cell + AirRemapDirections[direction];

        if(!AirContains(field, neighbour))
        {
            continue;
        }

        uint neighbourIndex;
        const bool inBoxes = BoundaryRecordIndex(neighbour, neighbourIndex);
        const uint4 group = inBoxes ? RawBuffer(recordUnordered).Load4(RecordAddress(neighbourIndex) + 32) : UnreachedOutsideBoxes(neighbour);
        const uint64_t mask = MaskOf(group.z, group.w);

        if(mask == 0)
        {
            continue;
        }

        bool byRegions;
        const uint weight = UnreachedWeight(neighbour, mask, byRegions);

        if(weight == 0)
        {
            continue;
        }

        const float4 groupMass = inBoxes && remapMass ? asfloat(RawBuffer(recordUnordered).Load4(RecordAddress(neighbourIndex) + 96)) :
            float4(0.0, 0.0, 0.0, 0.0);

        // The neighbour lies in `direction` from this cell; seen from it, this cell is the opposite direction.
        const uint axis = direction / 2;
        const bool towardThis = (direction & 1u) == 0;

        for(uint piece = 0; piece < 2; ++piece)
        {
            const uint64_t owner = byRegions ? regionMasks[piece] : pieces[piece];
            const uint contacts = pieces[piece] != 0 ? FaceContacts(mask, owner, axis, towardThis) : 0;

            if(contacts > 0)
            {
                const float share = float(contacts) / float(weight);
                anchorVolume[piece] += asfloat(group.x) * share;
                newVolume[piece] += asfloat(group.y) * share;

                if(remapMass)
                {
                    mass[piece] += inBoxes ? groupMass * share : oldDensity[piece] * (asfloat(group.x) * share);
                }
            }
        }
    }

    float2 offset = float2(0.0, 0.0);
    float2 source = float2(0.0, 0.0);

    for(uint piece = 0; piece < 2; ++piece)
    {
        if(pieces[piece] != 0)
        {
            offset[piece] = naiveCoupling != 0 ? 0.0 : float(AirSubcellCount(pieces[piece])) - newVolume[piece];
            source[piece] = anchorVolume[piece] - newVolume[piece];
        }
    }

    const uint slotIndex = address / 16;
    RawBuffer(effectiveOffsetUnordered).Store2(slotIndex * 8, asuint(offset));
    RawBuffer(wallSourceUnordered).Store2(slotIndex * 8, asuint(source));
    BoundaryAdd(AirBoundarySourceNet, source.x + source.y);
    BoundaryAdd(AirBoundarySourceGross, abs(source.x) + abs(source.y));

    if(mistUnordered0 == AirMovingSolidNone)
    {
        return;
    }

    // Discontinuous: CSBoundaryToGeometric's record (old masks, stored values, offsets). A piece whose sub-cells and offset are unchanged
    // and whose mist F1b left as converted takes its stored value back bit for bit.
    bool restore[2] = {false, false};
    float4 original[2] = {float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0)};

    if(!remapMass && naiveCoupling == 0)
    {
        const uint4 oldWords = RawBuffer(recordUnordered).Load4(record);
        const uint64_t oldMasks[2] = {MaskOf(oldWords.x, oldWords.y), MaskOf(oldWords.z, oldWords.w)};
        const float2 oldOffsets = asfloat(RawBuffer(recordUnordered).Load2(record + 96));
        original[0] = asfloat(RawBuffer(recordUnordered).Load4(record + 64));
        original[1] = asfloat(RawBuffer(recordUnordered).Load4(record + 80));

        for(uint piece = 0; piece < 2; ++piece)
        {
            RWByteAddressBuffer mist = RawBuffer(piece == 0 ? mistUnordered0 : mistUnordered1);
            const bool converted = oldOffsets[piece] != 0.0 && oldMasks[piece] == pieces[piece] && oldOffsets[piece] == offset[piece];
            restore[piece] = converted &&
                all(asuint(asfloat(mist.Load4(address))) == asuint(GeometricDensity(original[piece], pieces[piece], oldOffsets[piece])));
        }
    }

    for(uint piece = 0; piece < 2; ++piece)
    {
        RWByteAddressBuffer mist = RawBuffer(piece == 0 ? mistUnordered0 : mistUnordered1);

        if(pieces[piece] == 0)
        {
            mist.Store4(address, uint4(0, 0, 0, 0));
            continue;
        }

        if(remapMass)
        {
            const float4 density = anchorVolume[piece] > 0.0 ? mass[piece] / anchorVolume[piece] : float4(0.0, 0.0, 0.0, 0.0);
            mist.Store4(address, asuint(density));
        }
        else if(restore[piece])
        {
            mist.Store4(address, asuint(original[piece]));
        }
        else if(offset[piece] != 0.0)
        {
            const float geometric = float(AirSubcellCount(pieces[piece]));
            const float effective = geometric - offset[piece];
            const float4 density = effective > 0.0 ? asfloat(mist.Load4(address)) * (geometric / effective) : float4(0.0, 0.0, 0.0, 0.0);
            mist.Store4(address, asuint(density));
        }
    }
}

// One thread per uint4 of a buffer (correctionUnordered names it): zeroed (the closed components' wall-source sums before a solve).
[numthreads(64, 1, 1)]
void CSBoundaryZero(uint3 thread : SV_DispatchThreadID)
{
    if(thread.x < totalCells)
    {
        RawBuffer(correctionUnordered).Store4(thread.x * 16, uint4(0, 0, 0, 0));
    }
}

// One thread per cell of the boxes: the wall source cleared (a step without a geometry update has none); with boundaryEnabled == 2 (after
// maintenance rebuilt pieces from residency) the effective-volume offset too.
[numthreads(64, 1, 1)]
void CSBoundaryClear(uint3 thread : SV_DispatchThreadID)
{
    int3 cell;

    if(!OwnedCell(thread.x, cell))
    {
        return;
    }

    const uint slotIndex = AirVelocityAddress(field, AirSlotOf(field, cell)) / 16;
    RawBuffer(wallSourceUnordered).Store2(slotIndex * 8, uint2(0, 0));

    if(boundaryEnabled == 2)
    {
        RawBuffer(effectiveOffsetUnordered).Store2(slotIndex * 8, uint2(0, 0));
    }
}


// ================================================================================================================================
// AIR.4F.3 F2b.4: tangential wall coupling. A moving wall drags the fluid touching it along its surface (friction), which the volume
// source D alone cannot do: D enters the pressure equation, whose correction is curl-free, so the flow it drives ends when the wall
// stops. Here, before the projection of the step that follows a continuous update, each fluid piece near a moving solid's surface moves
// the tangential part of its velocity toward the wall's surface velocity:
//   v_t += alpha (u_t - v_t),  alpha = 1 - exp(-k w dt),
// where u is the velocity of the solid's material point at the contact (its displacement between the update's anchor and new poses over
// one step: translation and rotation alike), n the solid's outward surface normal there, the tangential parts are taken across n, and
// w = 1 - d / cell the contact weight of the piece's anchor at distance d >= 0 outside the solid (0 beyond one cell). The normal part is
// left to D and the projection. alpha < 1, so a piece never passes the wall's velocity. The nearest solid alone acts on a piece (no
// contribution is applied twice where solids overlap).

// The signed distance from `position` to the solid (negative inside).
float WallSignedDistance(AirMovingSolid solid, float3 position)
{
    if(solid.type == AirMovingSolidSphere)
    {
        return length(position - solid.center) - solid.radius;
    }

    if(solid.type == AirMovingSolidCapsule)
    {
        const float3 axis = solid.segmentB - solid.segmentA;
        const float length2 = dot(axis, axis);
        const float along = length2 > 0.0 ? saturate(dot(position - solid.segmentA, axis) / length2) : 0.0;
        return length(position - (solid.segmentA + axis * along)) - solid.radius;
    }

    const float3 offset = position - solid.center;
    const float3 local = float3(dot(offset, solid.axisX), dot(offset, solid.axisY), dot(offset, solid.axisZ));
    const float3 outside = abs(local) - solid.halfExtents;
    return length(max(outside, 0.0)) + min(max(outside.x, max(outside.y, outside.z)), 0.0);
}

// A piece's anchor in cell units (the centroid of its sub-cells).
float3 WallPieceCentroid(uint64_t piece)
{
    if(piece == AirSubcellsAll)
    {
        return float3(0.5, 0.5, 0.5);
    }

    float3 sum = float3(0.0, 0.0, 0.0);

    for(uint bit = 0; bit < 64; ++bit)
    {
        if(((piece >> bit) & 1u) != 0)
        {
            sum += float3(bit & 3u, (bit >> 2) & 3u, bit >> 4) + 0.5;
        }
    }

    return sum / (float(AirSubcellCount(piece)) * float(AirSubcellsPerAxis));
}

// One thread per cell of the update's boundary boxes.
[numthreads(64, 1, 1)]
void CSWallTangential(uint3 thread : SV_DispatchThreadID)
{
    int3 cell;

    if(tangentialRateStep <= 0.0 || stepSeconds <= 0.0 || !OwnedCell(thread.x, cell))
    {
        return;
    }

    const AirPieces pieces = LoadPieces(cell);
    const uint64_t masks[2] = {pieces.piece0, pieces.piece1};
    const uint address = AirVelocityAddress(field, AirSlotOf(field, cell));

    for(uint piece = 0; piece < 2; ++piece)
    {
        if(masks[piece] == 0)
        {
            continue;
        }

        const float3 anchor = (float3(cell) + WallPieceCentroid(masks[piece])) * field.cellSize;
        float nearest = field.cellSize;
        uint chosen = solidCount;

        for(uint index = 0; index < solidCount; ++index)
        {
            // A solid whose bounds, grown by a cell, miss the anchor is farther than a cell (the reach): no distance needed.
            const AirMovingSolid candidate = movingSolids[index];

            if(any(anchor < candidate.boundsMinimum - field.cellSize) || any(anchor > candidate.boundsMaximum + field.cellSize))
            {
                continue;
            }

            const float distance = WallSignedDistance(candidate, anchor);

            if(distance >= 0.0 && distance < nearest)
            {
                nearest = distance;
                chosen = index;
            }
        }

        if(chosen == solidCount)
        {
            continue;
        }

        const AirMovingSolid solid = movingSolids[chosen];
        const AirMovingSolid previous = movingSolids[anchorOffset + chosen];
        // The outward normal from the distance's central differences, and the contact point on the surface.
        const float h = 0.125 * field.cellSize;
        const float3 gradient = float3(
            WallSignedDistance(solid, anchor + float3(h, 0.0, 0.0)) - WallSignedDistance(solid, anchor - float3(h, 0.0, 0.0)),
            WallSignedDistance(solid, anchor + float3(0.0, h, 0.0)) - WallSignedDistance(solid, anchor - float3(0.0, h, 0.0)),
            WallSignedDistance(solid, anchor + float3(0.0, 0.0, h)) - WallSignedDistance(solid, anchor - float3(0.0, 0.0, h)));
        const float gradientLength = length(gradient);

        if(gradientLength <= 0.0)
        {
            continue;
        }

        const float3 normal = gradient / gradientLength;
        const float3 contact = anchor - normal * nearest;
        // The material point at the contact, in the solid's frame, and where it was at the update's anchor pose.
        const float3 offset = contact - solid.center;
        const float3 local = float3(dot(offset, solid.axisX), dot(offset, solid.axisY), dot(offset, solid.axisZ));
        const float3 before = previous.center + previous.axisX * local.x + previous.axisY * local.y + previous.axisZ * local.z;
        const float3 wallVelocity = (contact - before) / stepSeconds;

        RWByteAddressBuffer velocityBuffer = RawBuffer(piece == 0 ? velocityUnordered0 : velocityUnordered1);
        const float4 stored = asfloat(velocityBuffer.Load4(address));
        const float3 relative = wallVelocity - stored.xyz;
        const float3 tangential = relative - dot(relative, normal) * normal;
        const float weight = 1.0 - nearest / field.cellSize;
        const float alpha = 1.0 - exp(-tangentialRateStep * weight);
        velocityBuffer.Store4(address, asuint(float4(stored.xyz + alpha * tangential, stored.w)));
    }
}
