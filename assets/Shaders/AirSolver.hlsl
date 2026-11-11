// AIR.2 approximate velocity solver, piece-aware since AIR.4D D2: one fused step per dispatch, from the current velocity buffers
// into the other ones. No pressure projection: the field is not divergence-free (see Documentation/Air/AirArchitecture.md, AIR.2
// and AIR.4D D2).
//
// A cell holds up to two fluid pieces (Air.hlsli), each with its own velocity; piece 0 lives in the original velocity buffers and
// piece 1 in its own pair. Per piece, in order:
//   1. backtrace from the piece's anchor (AirPieceAnchor: the cell centre for a whole cell) along -v dt, at most 8 cells, by an
//      exact traversal through open fluid (AirTraverse); it stops before the first closed sub-cell, so a sample never comes from
//      beyond a wall, however thin;
//   2. trilinear sample there over the 8 cells whose centres surround the point. When all 8 are whole (or outside the window), this
//      is plain trilinear, as before D2. Otherwise each corner contributes one piece, never a blend of two: the piece a straight
//      path from the point enters it by, else the piece reachable through linked pieces of the 8 cells (fewest links, then nearest
//      sub-cell, then piece 0), else nothing; the weights left are renormalized. Outside the window reads the ambient velocity.
//      With no corner left, the piece keeps its own velocity;
//   3. damping toward the ambient velocity: v = a + (v - a) * dampingFactor;
//   4. AIR.3 motors at the anchor: each motor whose region holds it, and whose emission geometry sees the piece (its visibility
//      bit, CSMotorVisibility), adds its acceleration * dt (scaled by its falloff); Moving motors pull the air towards their own
//      velocity. Air a motor touched is capped at maximumMotorSpeed. A motor without computed visibility applies nothing;
//   5. wall constraint: a component pointing at a face the piece shares no open sub-face across is removed (binary, not scaled
//      by how much of the face is open).
// A cell without pieces is written as zero in both, and an absent piece 1 as zero. Unknown cells are free air (AIR.1).
//
// AIR.4D D2 motor visibility: a piece receives a motor's force only if a straight path through open fluid (AirTraverse, the
// backtrace's rules) joins one of the motor's emission points to the piece's anchor. The points are the one of the emission
// geometry nearest the anchor, then fixed points spread over it (EmissionPoint). A point in a closed sub-cell emits nothing: solid
// geometry is never skipped, the motor's own object included. The bits are cached per motor (AirField), computed by
// CSMotorVisibility only when the motor, the obstacles or (for a motor at the window's edge) the window change.
//
// AIR.4D D2.1 shaped emitters (an Omni ellipsoid, a Vortex ring; AirEmitter.hlsli): the nearest surface or ring point is tried
// first, then up to 7 emitting points of the shape's lattice near it, nearest first. Whether each lattice point emits is computed
// once per visibility recompute (CSEmissionSamples) and kept with the motor's visibility slot.

// The air buffers are in the UAV state during the air work, so the shared path and sample code reads writable views.
#define AIR_PATH_WRITABLE 1
#include "AirPath.hlsli"
#include "AirSample.hlsli"
#include "AirEmitter.hlsli"

cbuffer AirSolverConstants : register(b0)
{
    AirFieldBinding field;
    float3 ambientVelocity;
    float timeStep;
    uint velocityReadIndex;
    uint velocityWriteIndex;
    uint obstacleIndex;
    // exp(-dampingRate * timeStep)
    float dampingFactor;
    uint motorCount;
    float maximumMotorSpeed;
    // AIR.4D D2: piece 1's velocity pair, the piece masks and the motors' visibility bits (writable views).
    uint piece1ReadIndex;
    uint piece1WriteIndex;
    uint pieceIndex;
    uint visibilityIndex;
    uint2 visibilityPadding;
    // CSMotorVisibility: the motor (an index into `motors`), and the box of cells to compute: from visibilityBoxMinimum (x a
    // multiple of 16), visibilityBoxWords.x words of 16 cells along x by visibilityBoxWords.y by visibilityBoxWords.z cells.
    int3 visibilityBoxMinimum;
    uint visibilityMotor;
    uint3 visibilityBoxWords;
    // CSEmissionStatus: the writable status buffer, one uint4 per motor.
    uint statusIndex;
    // The two-pass step (CSStepOpen, CSPrepareSplit, CSStepSplit): the work list (word 0: the count CSStepSplit reads, then one
    // linear slot index per listed cell) and the counter and dispatch arguments (word 0: counter; words 1 to 3: the dispatch).
    uint splitListIndex;
    uint splitArgumentsIndex;
    // AIR.4D D2.1: the shaped emitters' lattice states, AirSampleWordsPerMotor words per visibility slot (writable view).
    uint emissionSamplesIndex;
    uint constantsPadding;
};

// Matches GpuAirMotor (AirField.cpp) and AirMotorType (AirMotorComponent.h).
struct AirMotor
{
    // The emission origin (the region's centre); a Moving motor's object position.
    float3 position;
    uint type;
    float3 axisX;
    float strength;
    float3 axisY;
    float falloff;
    float3 axisZ;
    float boundingRadius;
    // Directional: half extents; the others: radius in x (Omni and Vortex: the influence distance) and half length in y.
    float3 size;
    float inwardPull;
    float3 velocity;
    float coupling;
    // AIR.4D D2.1: the Omni ellipsoid's or Vortex ring's semi-axes along axisX, axisY (and axisZ); (0, 0, 0) is a point Omni.
    float3 semiAxes;
    // The motor's slot in the visibility buffer, or NoVisibility (no force).
    uint visibilitySlot;
    // Cell crossings a path within the motor's bounding sphere can need, so visibility paths are never cut short.
    uint maximumCrossings;
    uint3 padding;
};

StructuredBuffer<AirMotor> motors : register(t0);

static const uint MotorDirectional = 0;
static const uint MotorOmni = 1;
static const uint MotorVortex = 2;
static const uint MotorMoving = 3;
static const uint MotorCylinder = 4;

static const uint NoVisibility = 0xFFFFFFFFu;
// Emission points: the nearest one, and at most this many fixed ones.
static const uint MaximumFixedEmissionPoints = 7;

AirPathField Path()
{
    AirPathField path;
    path.field = field;
    path.obstacleIndex = obstacleIndex;
    path.pieceIndex = pieceIndex;
    path.velocityIndex = velocityReadIndex;
    path.velocityPiece1Index = piece1ReadIndex;
    path.ambient = ambientVelocity;
    return path;
}

// t is the normalized distance from the centre (or axis) to the region's edge, 0 to 1. Full effect inside, fading to zero at the
// edge over the outer `falloff` fraction; a falloff of 0 is a hard edge.
float Falloff(float t, float falloff)
{
    return falloff > 0.0 ? saturate((1.0 - t) / falloff) : 1.0;
}

// AIR.4D D2.1: whether the motor emits from a shape (an Omni ellipsoid or a Vortex ring) rather than from fixed points.
bool IsShapedEmitter(AirMotor motor)
{
    return motor.type == MotorVortex || (motor.type == MotorOmni && any(motor.semiAxes > 0.0));
}

float3 ToLocal(AirMotor motor, float3 offset)
{
    return float3(dot(offset, motor.axisX), dot(offset, motor.axisY), dot(offset, motor.axisZ));
}

// A local direction in world space.
float3 RotateToWorld(AirMotor motor, float3 local)
{
    return motor.axisX * local.x + motor.axisY * local.y + motor.axisZ * local.z;
}

// A local point in world space.
float3 ToWorld(AirMotor motor, float3 local)
{
    return motor.position + RotateToWorld(motor, local);
}

// A shaped emitter's nearest point to `offset` from its centre (local frame), and the region coordinate there: the distance from
// the surface or ring over the influence distance; above 1 (2) for an Omni's inside, which the region never holds.
AirNearestPoint ShapeNearest(AirMotor motor, float3 offset, out float t)
{
    const float3 local = ToLocal(motor, offset);
    AirNearestPoint nearest;

    if(motor.type == MotorOmni)
    {
        nearest = AirEllipsoidNearest(local, motor.semiAxes);
    }
    else
    {
        nearest = AirRingNearest(local, motor.semiAxes.xy);
    }

    t = nearest.outside ? nearest.distance / motor.size.x : 2.0;
    return nearest;
}

// The region coordinate of a point `offset` from the motor's centre: 1 at the region's edge, more outside it. Shaped emitters use
// ShapeNearest.
float RegionT(AirMotor motor, float3 offset)
{
    const float3 local = ToLocal(motor, offset);

    if(motor.type == MotorDirectional)
    {
        return max(max(abs(local.x) / motor.size.x, abs(local.y) / motor.size.y), abs(local.z) / motor.size.z);
    }

    if(motor.type == MotorCylinder)
    {
        return max(length(local.xy) / motor.size.x, abs(local.z) / motor.size.y);
    }

    if(IsShapedEmitter(motor))
    {
        float t;
        ShapeNearest(motor, offset, t);
        return t;
    }

    return length(offset) / motor.size.x;
}

// The cell and sub-cell (window-local) holding a slot's visibility bits for `slot` of the window: the obstacle mask's layout,
// repeated per motor slot.
uint VisibilityWordAddress(uint motorSlot, uint3 slot)
{
    const uint wordsPerMotor = (field.dimensions.x / AirSlotsPerObstacleWord) * field.dimensions.y * field.dimensions.z;
    return (motorSlot * wordsPerMotor + AirObstacleWord(field, slot)) * 4;
}

bool Visible(AirMotor motor, int3 cell, uint piece)
{
    if(motor.visibilitySlot == NoVisibility)
    {
        return false;
    }

    RWByteAddressBuffer visibility = ResourceDescriptorHeap[NonUniformResourceIndex(visibilityIndex)];
    const uint3 slot = AirSlotOf(field, cell);
    const uint word = visibility.Load(VisibilityWordAddress(motor.visibilitySlot, slot));
    return ((word >> ((slot.x % AirSlotsPerObstacleWord) * 2 + piece)) & 1u) != 0;
}

// Adds the motors' effect on piece `piece` of `cell`, whose anchor is at `world`; returns whether any motor reached it.
bool ApplyMotors(float3 world, int3 cell, uint piece, inout float3 velocity)
{
    bool driven = false;

    for(uint index = 0; index < motorCount; ++index)
    {
        const AirMotor motor = motors[index];
        const float3 offset = world - motor.position;

        if(dot(offset, offset) > motor.boundingRadius * motor.boundingRadius)
        {
            continue;
        }

        // A shaped emitter's nearest point gives both its region coordinate and its force direction.
        const bool shaped = IsShapedEmitter(motor);
        AirNearestPoint nearest = (AirNearestPoint)0;
        float t;

        if(shaped)
        {
            nearest = ShapeNearest(motor, offset, t);
        }
        else
        {
            t = RegionT(motor, offset);
        }

        if(t > 1.0 || !Visible(motor, cell, piece))
        {
            continue;
        }

        const float effect = Falloff(t, motor.falloff);

        if(motor.type == MotorDirectional || motor.type == MotorCylinder)
        {
            velocity += motor.axisZ * (motor.strength * effect * timeStep);
        }
        else if(motor.type == MotorOmni && shaped)
        {
            // Along the ellipsoid's outward normal at the nearest surface point.
            velocity += RotateToWorld(motor, nearest.direction) * (motor.strength * effect * timeStep);
        }
        else if(motor.type == MotorOmni)
        {
            // A point Omni, exactly as AIR.3: radially from its centre.
            const float distance = length(offset);
            const float3 direction = distance > 1e-6 ? offset / distance : float3(0.0, 0.0, 0.0);
            velocity += direction * (motor.strength * effect * timeStep);
        }
        else if(motor.type == MotorMoving)
        {
            // Never overshoots the object's velocity, whatever the coupling.
            velocity += (motor.velocity - velocity) * saturate(motor.coupling * effect * timeStep);
        }
        else if(motor.type == MotorVortex)
        {
            // Along the ring's tangent at the nearest ring point, pulled along its in-plane inward normal there.
            const float3 tangent = RotateToWorld(motor, nearest.direction);
            const float3 inward = RotateToWorld(motor, nearest.inward);
            velocity += (tangent * motor.strength + inward * motor.inwardPull) * (effect * timeStep);
        }

        driven = true;
    }

    return driven;
}

// How many fixed emission points a motor without a shape has (see EmissionPoint).
uint FixedEmissionPoints(AirMotor motor)
{
    if(motor.type == MotorMoving || motor.type == MotorOmni)
    {
        return 0;
    }

    return MaximumFixedEmissionPoints;
}

// Emission point `index` of a motor without a shape, in world space. Index 0 is the point of the emission geometry nearest `target`;
// then the fixed points (FixedEmissionPoints of them):
//   Directional: the ray from the origin along +Z to the box's +Z face; fixed points evenly along it, both ends included;
//   Cylinder: the disc of the motor's radius at the origin, facing +Z; fixed points at its centre and six at half its radius;
//   point Omni and Moving: the centre (the object's position) only.
float3 EmissionPoint(AirMotor motor, uint index, float3 target)
{
    const float3 offset = target - motor.position;
    const float along = dot(offset, motor.axisZ);
    const float3 radial = offset - along * motor.axisZ;
    const float radialLength = length(radial);

    if(motor.type == MotorDirectional)
    {
        const float distance = index == 0 ? clamp(along, 0.0, motor.size.z) : motor.size.z * float(index - 1) / 6.0;
        return motor.position + motor.axisZ * distance;
    }

    if(motor.type == MotorCylinder)
    {
        if(index == 0)
        {
            const float3 onDisc = radialLength > motor.size.x ? radial * (motor.size.x / radialLength) : radial;
            return motor.position + onDisc;
        }

        if(index == 1)
        {
            return motor.position;
        }

        const float angle = float(index - 2) * 1.04719755;
        return motor.position + (motor.axisX * cos(angle) + motor.axisY * sin(angle)) * (motor.size.x * 0.5);
    }

    return motor.position;
}

// Where an emission point lies: an AirMotorEmissionStatus::Point value. The solver's view: a sub-cell no piece holds is blocked;
// a cell without voxel data, and anything outside the window, is free.
uint EmissionPointState(float3 world)
{
    const float3 local = world * field.inverseCellSize - float3(field.minimum);
    const int3 cell = field.minimum + int3(floor(local));

    if(!AirContains(field, cell))
    {
        return 3;
    }

    // The writable view: during the air work the mask is in the UAV state.
    RWByteAddressBuffer obstacles = ResourceDescriptorHeap[NonUniformResourceIndex(obstacleIndex)];
    const uint3 slot = AirSlotOf(field, cell);
    const uint bits = (obstacles.Load(AirObstacleWord(field, slot) * 4) >> ((slot.x % AirSlotsPerObstacleWord) * 2)) & 3u;

    if((bits & AirObstacleBlocked) == 0)
    {
        return (bits & AirObstacleKnown) != 0 ? 0u : 2u;
    }

    const AirCell holder = AirLoadCell(Path(), cell);
    const int3 subcell = clamp(int3(floor(local * 4.0)) - (cell - field.minimum) * 4, 0, 3);
    return AirPieceOfSubcell(holder, subcell) == AirNoPiece ? 1u : 0u;
}

// Whether a straight path through open fluid joins `from` (world space) to piece `piece` of `cell`, whose anchor is `to` (cell units
// from the window minimum). A start in a closed sub-cell sees nothing.
bool SeesPiece(float3 from, float3 to, int3 cell, uint piece, uint maximumCrossings)
{
    const float3 start = from * field.inverseCellSize - float3(field.minimum);
    const int3 startCell = field.minimum + int3(floor(start));
    const AirCell holder = AirLoadCell(Path(), startCell);
    uint startPiece = 0;

    if(holder.inside)
    {
        startPiece = AirPieceOfSubcell(holder, clamp(int3(floor(start * 4.0)) - (startCell - field.minimum) * 4, 0, 3));

        if(startPiece == AirNoPiece)
        {
            return false;
        }
    }

    const AirPathEnd end = AirTraverse(Path(), start, startCell, startPiece, to - start, false, cell, maximumCrossings);
    return !end.stopped && end.inside && all(end.cell == cell) && end.piece == piece;
}

// Whether the straight path `from` -> `to` (world space, short) stays in open fluid: it starts in an open sub-cell and AirTraverse
// does not stop. Space outside the window and cells without pieces are open.
bool OpenStep(float3 from, float3 to)
{
    const float3 start = from * field.inverseCellSize - float3(field.minimum);
    const int3 startCell = field.minimum + int3(floor(start));
    const AirCell holder = AirLoadCell(Path(), startCell);
    uint startPiece = 0;

    if(holder.inside)
    {
        startPiece = AirPieceOfSubcell(holder, clamp(int3(floor(start * 4.0)) - (startCell - field.minimum) * 4, 0, 3));

        if(startPiece == AirNoPiece)
        {
            return false;
        }
    }

    const float3 displacement = (to - from) * field.inverseCellSize;
    const AirPathEnd end = AirTraverse(Path(), start, startCell, startPiece, displacement, false, startCell, AirMaximumCornerCrossings);
    return !end.stopped;
}

// A shaped emitter's lattice sample `index`: where it emits from (world space; an Omni's point is the surface point stepped
// AirSurfaceStep outward) and its outward normal (world space; zero for a ring, which has no outside).
struct ShapeSample
{
    float3 surface;
    float3 emitter;
    float3 normal;
};

ShapeSample ShapeSampleOf(AirMotor motor, uint index)
{
    ShapeSample sample;

    if(motor.type == MotorOmni)
    {
        const float3 direction = AirOmniSampleDirection(index);
        sample.surface = ToWorld(motor, motor.semiAxes * direction);
        sample.normal = RotateToWorld(motor, normalize(direction / motor.semiAxes));
        sample.emitter = sample.surface + sample.normal * AirSurfaceStep;
        return sample;
    }

    const float angle = AirRingSampleAngle(index);
    sample.surface = ToWorld(motor, float3(motor.semiAxes.x * cos(angle), motor.semiAxes.y * sin(angle), 0.0));
    sample.emitter = sample.surface;
    sample.normal = float3(0.0, 0.0, 0.0);
    return sample;
}

uint ShapeSampleCount(AirMotor motor)
{
    return motor.type == MotorOmni ? AirOmniSamples : AirVortexSamples;
}

// A lattice sample's state, an AirMotorEmissionStatus::Point value: an Omni sample is blocked unless its surface point is in open
// fluid and the step outward along the normal stays in it (solid is never stepped through); otherwise it takes the state of the
// point it emits from. A ring sample takes its point's state.
uint ShapeSampleState(AirMotor motor, uint index)
{
    const ShapeSample sample = ShapeSampleOf(motor, index);

    if(motor.type == MotorOmni && !OpenStep(sample.surface, sample.emitter))
    {
        return 1;
    }

    return EmissionPointState(sample.emitter);
}

uint SampleWordAddress(uint motorSlot, uint word)
{
    return (motorSlot * AirSampleWordsPerMotor + word) * 4;
}

// The stored state of lattice sample `index` of the motor's slot (CSEmissionSamples).
uint StoredSampleState(AirMotor motor, uint index)
{
    RWByteAddressBuffer samples = ResourceDescriptorHeap[NonUniformResourceIndex(emissionSamplesIndex)];
    const uint word = samples.Load(SampleWordAddress(motor.visibilitySlot, index / 16));
    return (word >> ((index % 16) * 2)) & 3u;
}

// Whether a shaped emitter sees piece `piece` of `cell`, whose anchor is at `world` (`to`: cell units from the window minimum): from
// the nearest surface or ring point (for an Omni, the anchor lies along the outward normal from it, so the path leaves the surface
// outward), else from at most AirMaximumFallbackTraces emitting lattice samples near it, nearest first. An Omni sample is used only if
// the anchor lies in front of it (outside its tangent plane), so its path never runs back into the surface. `nearest` is the
// shape's nearest point to the anchor (ShapeNearest).
bool ShapeSees(AirMotor motor, AirNearestPoint nearest, float3 world, float3 to, int3 cell, uint piece)
{
    if(SeesPiece(ToWorld(motor, nearest.position), to, cell, piece, motor.maximumCrossings))
    {
        return true;
    }

    const bool omni = motor.type == MotorOmni;
    const int2 omniCell = omni ? AirOmniLatticeCell(nearest.position, motor.semiAxes) : int2(0, 0);
    const int ringIndex = omni ? 0 : AirRingLatticeIndex(nearest.position, motor.semiAxes.xy);
    uint traced = 0;

    for(uint candidate = 0; candidate < AirFallbackCandidates && traced < AirMaximumFallbackTraces; ++candidate)
    {
        const int index = omni ? AirOmniFallbackSample(omniCell, candidate) : AirRingFallbackSample(ringIndex, candidate);

        if(index < 0 || StoredSampleState(motor, uint(index)) == 1)
        {
            continue;
        }

        const ShapeSample sample = ShapeSampleOf(motor, uint(index));

        if(omni && dot(sample.normal, world - sample.surface) <= 0.0)
        {
            continue;
        }

        ++traced;

        if(SeesPiece(sample.emitter, to, cell, piece, motor.maximumCrossings))
        {
            return true;
        }
    }

    return false;
}

// One thread per state word of a shaped emitter's lattice (16 samples each): the states of the motor's samples, kept with its
// visibility slot for CSMotorVisibility and CSEmissionStatus. Runs before the motor's visibility, behind a UAV barrier.
[numthreads(8, 1, 1)]
void CSEmissionSamples(uint3 thread : SV_DispatchThreadID)
{
    const AirMotor motor = motors[visibilityMotor];
    const uint first = thread.x * 16;
    const uint count = ShapeSampleCount(motor);
    uint word = 0;

    for(uint offset = 0; offset < 16; ++offset)
    {
        if(first + offset < count)
        {
            word |= ShapeSampleState(motor, first + offset) << (offset * 2);
        }
    }

    RWByteAddressBuffer samples = ResourceDescriptorHeap[NonUniformResourceIndex(emissionSamplesIndex)];
    samples.Store(SampleWordAddress(motor.visibilitySlot, thread.x), word);
}

// One thread per 16-cell word of the motor's box: the visibility bits of the box's cells (2 per cell: piece 0 and piece 1). A piece
// beyond the region (with a margin, so the step's own region test never meets an uncomputed bit) or out of sight is 0.
[numthreads(4, 4, 4)]
void CSMotorVisibility(uint3 thread : SV_DispatchThreadID)
{
    if(any(thread >= visibilityBoxWords))
    {
        return;
    }

    const AirMotor motor = motors[visibilityMotor];
    const int3 first = visibilityBoxMinimum + int3(int(thread.x) * int(AirSlotsPerObstacleWord), int(thread.y), int(thread.z));
    uint bits = 0;
    // The bits of this word's cells inside the window; the others are left alone.
    uint owned = 0;

    for(uint index = 0; index < AirSlotsPerObstacleWord; ++index)
    {
        const int3 cell = first + int3(index, 0, 0);

        if(!AirContains(field, cell))
        {
            continue;
        }

        owned |= 3u << (index * 2);

        const AirCell own = AirLoadCell(Path(), cell);

        for(uint piece = 0; piece < 2; ++piece)
        {
            const uint64_t mask = AirPieceMask(own, piece);

            if(mask == 0)
            {
                continue;
            }

            const float3 anchor = AirPieceAnchor(mask);
            const float3 world = (float3(cell) + anchor) * field.cellSize;
            const bool shaped = IsShapedEmitter(motor);
            AirNearestPoint nearest = (AirNearestPoint)0;
            float t;

            if(shaped)
            {
                nearest = ShapeNearest(motor, world - motor.position, t);
            }
            else
            {
                t = RegionT(motor, world - motor.position);
            }

            if(t > 1.001)
            {
                continue;
            }

            const float3 to = float3(cell - field.minimum) + anchor;
            bool seen = false;

            if(shaped)
            {
                seen = ShapeSees(motor, nearest, world, to, cell, piece);
            }
            else
            {
                const uint points = 1 + FixedEmissionPoints(motor);

                for(uint emitter = 0; emitter < points && !seen; ++emitter)
                {
                    seen = SeesPiece(EmissionPoint(motor, emitter, world), to, cell, piece, motor.maximumCrossings);
                }
            }

            bits |= seen ? 1u << (index * 2 + piece) : 0u;
        }
    }

    // The word's 16 slots are these 16 cells' (the box starts on a multiple of 16 along x, and the window's width is one). Where the
    // window's edge falls inside the word, its other slots hold cells at the window's far side, which another thread may be writing
    // into the same word, so only the owned bits change, atomically.
    RWByteAddressBuffer visibility = ResourceDescriptorHeap[NonUniformResourceIndex(visibilityIndex)];
    const uint address = VisibilityWordAddress(motor.visibilitySlot, AirSlotOf(field, first));
    visibility.InterlockedAnd(address, ~owned);
    visibility.InterlockedOr(address, bits);
}

// One thread per motor: where its fixed emission points lie (its origin, how many, how many blocked and how many in unknown space).
// A shaped emitter reports its lattice, from the states CSEmissionSamples kept with its visibility slot.
[numthreads(64, 1, 1)]
void CSEmissionStatus(uint3 thread : SV_DispatchThreadID)
{
    if(thread.x >= motorCount)
    {
        return;
    }

    const AirMotor motor = motors[thread.x];
    const bool shaped = IsShapedEmitter(motor) && motor.visibilitySlot != NoVisibility;
    const uint fixedPoints = FixedEmissionPoints(motor);
    // The fixed points; a motor without any (Moving, or a point Omni) emits from its origin alone.
    const uint points = shaped ? ShapeSampleCount(motor) : max(fixedPoints, 1u);
    uint blocked = 0;
    uint unknown = 0;

    for(uint emitter = 0; emitter < points; ++emitter)
    {
        uint state;

        if(shaped)
        {
            state = StoredSampleState(motor, emitter);
        }
        else
        {
            state = EmissionPointState(fixedPoints > 0 ? EmissionPoint(motor, emitter + 1, motor.position) : motor.position);
        }

        blocked += state == 1 ? 1u : 0u;
        unknown += state == 2 ? 1u : 0u;
    }

    RWByteAddressBuffer status = ResourceDescriptorHeap[NonUniformResourceIndex(statusIndex)];
    status.Store4(thread.x * 16, uint4(EmissionPointState(motor.position), points, blocked, unknown));
}

// Step 4: the motors, and the cap on air they touched.
float3 DriveAndCap(float3 world, int3 cell, uint piece, float3 velocity)
{
    if(motorCount > 0 && ApplyMotors(world, cell, piece, velocity))
    {
        const float speed = length(velocity);

        if(speed > maximumMotorSpeed)
        {
            velocity *= maximumMotorSpeed / speed;
        }
    }

    return velocity;
}

// One piece's new velocity (steps 1 to 5 above).
float3 StepPiece(int3 cell, uint64_t mask, uint piece)
{
    const float3 anchor = AirPieceAnchor(mask);
    const float3 current = AirPieceVelocity(Path(), cell, piece);
    const float3 start = float3(cell - field.minimum) + anchor;
    const float3 displacement = AirBacktraceDisplacement(current, timeStep, field.inverseCellSize);

    const AirPathEnd from = AirBacktrace(Path(), cell, mask, piece, start, displacement);
    float3 velocity = AirSampleVelocity(Path(), from, current);

    velocity = ambientVelocity + (velocity - ambientVelocity) * dampingFactor;
    velocity = DriveAndCap((float3(cell) + anchor) * field.cellSize, cell, piece, velocity);

    // Nothing flows into a wall: remove components pointing at a face this piece has no open sub-face across.
    for(uint axis = 0; axis < 3; ++axis)
    {
        const int direction = velocity[axis] > 0.0 ? 1 : -1;

        if(velocity[axis] != 0.0 && !AirFaceOpen(Path(), cell, mask, axis, direction))
        {
            velocity[axis] = 0.0;
        }
    }

    return velocity;
}

// Both pieces of a cell, the full path, written to the output buffers.
void StepCell(uint3 slot)
{
    RWByteAddressBuffer output0 = ResourceDescriptorHeap[NonUniformResourceIndex(velocityWriteIndex)];
    RWByteAddressBuffer output1 = ResourceDescriptorHeap[NonUniformResourceIndex(piece1WriteIndex)];
    const int3 cell = AirCellOfSlot(field, slot);
    const uint address = AirVelocityAddress(field, slot);
    const AirCell own = AirLoadCell(Path(), cell);

    const float3 velocity0 = own.piece0 != 0 ? StepPiece(cell, own.piece0, 0) : float3(0.0, 0.0, 0.0);
    const float3 velocity1 = own.piece1 != 0 ? StepPiece(cell, own.piece1, 1) : float3(0.0, 0.0, 0.0);
    output0.Store4(address, asuint(float4(velocity0, 0.0)));
    output1.Store4(address, asuint(float4(velocity1, 0.0)));
}

// The two-pass step. CSStepOpen settles every cell whose whole step meets only open cells: it is one whole piece, its backtrace
// passes AirOpenBacktrace, its 8 sample corners are open, and every neighbour its new velocity points at is open (so no face check
// needs masks). For such a cell the full path computes exactly this, through the same functions. Every other cell is appended to
// the work list, and CSStepSplit runs the full path for it alone. The lean kernel holds none of the split-cell code, so its
// registers, and so its occupancy, are those of the open case.
//
// The list holds one entry per cell at most (a cell is appended once per step), so its capacity of every cell plus the count
// cannot overflow, whatever the geometry.

[numthreads(8, 8, 8)]
void CSStepOpen(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const int3 cell = AirCellOfSlot(field, slot);
    bool settled = false;
    float3 velocity = float3(0.0, 0.0, 0.0);

    if(AirCellIsOpen(Path(), cell))
    {
        const float3 current = AirPieceVelocity(Path(), cell, 0);
        const float3 start = float3(cell - field.minimum) + 0.5;
        const float3 end = start + AirBacktraceDisplacement(current, timeStep, field.inverseCellSize);
        const float3 shifted = end - 0.5;
        const int3 base = int3(floor(shifted));

        if(AirOpenBacktrace(Path(), start, end) && AirCornersOpen(Path(), base))
        {
            velocity = AirSampleOpen(Path(), base, shifted - float3(base), current);
            velocity = ambientVelocity + (velocity - ambientVelocity) * dampingFactor;
            velocity = DriveAndCap((float3(cell) + 0.5) * field.cellSize, cell, 0, velocity);
            settled = true;

            // The wall constraint against open neighbours removes nothing; any other neighbour needs the full path's face check.
            for(uint axis = 0; axis < 3; ++axis)
            {
                int3 neighbour = cell;
                neighbour[axis] += velocity[axis] > 0.0 ? 1 : -1;
                settled = settled && (velocity[axis] == 0.0 || AirCellIsOpen(Path(), neighbour));
            }
        }
    }

    if(settled)
    {
        RWByteAddressBuffer output0 = ResourceDescriptorHeap[NonUniformResourceIndex(velocityWriteIndex)];
        RWByteAddressBuffer output1 = ResourceDescriptorHeap[NonUniformResourceIndex(piece1WriteIndex)];
        const uint address = AirVelocityAddress(field, slot);
        output0.Store4(address, asuint(float4(velocity, 0.0)));
        output1.Store4(address, asuint(float4(0.0, 0.0, 0.0, 0.0)));
        return;
    }

    RWByteAddressBuffer arguments = ResourceDescriptorHeap[NonUniformResourceIndex(splitArgumentsIndex)];
    RWByteAddressBuffer list = ResourceDescriptorHeap[NonUniformResourceIndex(splitListIndex)];
    uint entry = 0;
    arguments.InterlockedAdd(0, 1, entry);
    list.Store((entry + 1) * 4, slot.x + field.dimensions.x * (slot.y + field.dimensions.y * slot.z));
}

// Groups of CSStepSplit along x per row of the dispatch (within D3D12's 65535).
static const uint SplitGroupsPerRow = 32768;
static const uint SplitThreads = 64;

// One thread: the list's count for CSStepSplit, its dispatch (rows of SplitGroupsPerRow groups), and the counter reset for the next
// step.
[numthreads(1, 1, 1)]
void CSPrepareSplit()
{
    RWByteAddressBuffer arguments = ResourceDescriptorHeap[NonUniformResourceIndex(splitArgumentsIndex)];
    RWByteAddressBuffer list = ResourceDescriptorHeap[NonUniformResourceIndex(splitListIndex)];
    const uint count = arguments.Load(0);
    const uint groups = (count + SplitThreads - 1) / SplitThreads;
    list.Store(0, count);
    arguments.Store3(4, uint3(min(groups, SplitGroupsPerRow), (groups + SplitGroupsPerRow - 1) / SplitGroupsPerRow, 1));
    arguments.Store(0, 0);
}

// One thread: zeroes the counter (new storage).
[numthreads(1, 1, 1)]
void CSResetSplit()
{
    RWByteAddressBuffer arguments = ResourceDescriptorHeap[NonUniformResourceIndex(splitArgumentsIndex)];
    arguments.Store(0, 0);
}

[numthreads(64, 1, 1)]
void CSStepSplit(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
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
    StepCell(slot);
}
