#pragma once

#include "Renderer/AirPressure.h"
#include "Renderer/Buffer.h"
#include "Renderer/ComputePipeline.h"
#include "Renderer/VoxelResidencyResources.h"
#include "Math/Vector3.h"
#include "Scene/AirMotorComponent.h"
#include "Scene/MistEmitterComponent.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include <wrl/client.h>

class DescriptorHeap;
class ShaderLibrary;
class UploadBuffer;

// AIR.1: the camera-centred air field's GPU storage, camera following and obstacle mask (see Documentation/Air/AirArchitecture.md).
//
// Storage (raw buffers, SRV and UAV in the bindless heap), allocated on the first enabled frame:
//   - velocity: two float4-per-cell buffers (ping-pong), the velocity of each cell's fluid piece 0;
//   - piece-1 velocity (AIR.4D D2): two more, the velocity of each cell's piece 1, swapped together with the first pair;
//   - obstacles: 2 bits per cell, 16 cells per u32 along x (blocked, known), built from VX.5 residency;
//   - pieces (AIR.4D): two 64-bit sub-cell masks per cell, built with the obstacle mask.
//   - mist (AIR.4F): two more float4-per-cell pairs, each piece's premultiplied mist, allocated only while AirMistSettings::enabled;
//     a mist step follows every solver step (AirMist.hlsl). Entering cells start clear, and a rebuild spreads each old piece's mist
//     over the sub-cells the new pieces share with it (AirField.hlsl).
// Addressing is toroidal over a world-aligned lattice of cubes of edge 2^cellExponent metres, so recentring moves the window's
// minimum cell and touches only the cells that enter it.
//
// Each enabled frame (Record, at the start of the frame's command list, after the VX.5 residency uploads):
//   - the window recentres per axis in whole steps of RecentreStep cells once the camera is more than RecentreStep cells from its
//     centre; a move of a full window or more on any axis (a teleport), the first frame and ResetField re-initialize everything;
//   - entering cells get the ambient velocity in all four velocity buffers and their obstacle bits and pieces, before anything
//     else reads them;
//   - any change of the VX.5 binding (publication, eviction, rebake, reset: its generation or validity) rebuilds every obstacle
//     word and piece; unknown space (unbaked, not resident, failed, or no residency at all) is free air with its unknown state
//     kept. Retained cells keep their air: each new piece takes the velocity of the old piece it overlaps most (ambient if none).
//
// Lifetime: the buffers are replaced only when the dimensions or cell size change; the old ones retire with the last submission
// fence and are released once the GPU completes it (or at Shutdown, after the Renderer's drain).

// Mirrored in Air.hlsli. Descriptor indices are read-only views (SRVs) in the Renderer's bindless resource heap: velocityIndex
// is the latest velocity, velocityOtherIndex the previous one. After the frame's air work the buffers are in a shader-read state.
struct AirFieldBinding
{
    std::uint32_t valid = 0;
    std::uint32_t velocityIndex = 0;
    std::uint32_t obstacleIndex = 0;
    std::uint32_t generation = 0;

    std::int32_t minimum[3] = {0, 0, 0};
    float cellSize = 0.0f;

    std::uint32_t dimensions[3] = {0, 0, 0};
    float inverseCellSize = 0.0f;

    std::uint32_t velocityOtherIndex = 0;
    // AIR.4D: the cells' fluid-piece masks (Air.hlsli), read-only.
    std::uint32_t pieceIndex = 0;
    // AIR.4D D2: piece 1's velocity, latest and previous, laid out as the piece-0 velocity above.
    std::uint32_t velocityPiece1Index = 0;
    std::uint32_t velocityPiece1OtherIndex = 0;
};

static_assert(sizeof(AirFieldBinding) == 64);

struct AirFieldSettings
{
    // x a multiple of 16 (obstacle words), y and z multiples of 8.
    std::array<std::uint32_t, 3> dimensions{128, 64, 128};
    // Cell edge 2^cellExponent metres: 0.5 m.
    int cellExponent = -1;
    Vector3 ambientVelocity{0.0f, 0.0f, 0.0f};

    bool operator==(const AirFieldSettings&) const = default;
};

// AIR.2 approximate solver (AirSolver.hlsl): advection, damping toward the ambient velocity, wall constraints.
struct AirSolverSettings
{
    // Per second: the gap to the ambient velocity shrinks by exp(-dampingRate * t).
    float dampingRate = 0.5f;
    // AIR.3: the fastest a motor may push air, in m/s. Applied only to cells a motor touched, so the solver without motors is
    // unchanged. It is also kept below the backtrace limit (8 cells per step), so motor-driven advection is never truncated.
    float maximumMotorSpeed = 40.0f;
    // AIR.4F F4.5.1b: the scene's air environment (AirEnvironmentSettings). Each piece's temperature is velocity.w, ΔT in kelvin from
    // the ambient; the ambient itself is not used by the step yet (ΔT is relative, and nothing depends on absolute temperature). ΔT
    // decays by exp(-t / temperatureRelaxationTime) seconds; 0 turns the relaxation off.
    float ambientTemperature = 293.15f;
    float temperatureRelaxationTime = 30.0f;

    bool operator==(const AirSolverSettings&) const = default;
};

// AIR.3: one Air Motor of the frame, in world space. Built by the Engine from an AirMotorComponent and its object's presented pose
// (unit axes; the object's scale is ignored). See AirMotorComponent.h for each type's meaning.
struct AirMotorInstance
{
    AirMotorType type = AirMotorType::Directional;
    // AIR.4D D2: the emission origin in world space (a Moving motor's object position); the region is centred here.
    Vector3 position{0.0f, 0.0f, 0.0f};
    // Orthonormal: local X, Y and Z (the motor's axis or forward direction). For an Omni or Vortex (AIR.4D D2.1), the emission
    // shape's axes: the object's rotation, then the motor's emission rotation.
    Vector3 axisX{1.0f, 0.0f, 0.0f};
    Vector3 axisY{0.0f, 1.0f, 0.0f};
    Vector3 axisZ{0.0f, 0.0f, 1.0f};
    float strength = 0.0f;
    float falloff = 0.0f;
    Vector3 halfExtents{1.0f, 1.0f, 1.0f};
    float radius = 1.0f;
    float halfLength = 1.0f;
    float inwardPull = 0.0f;
    float coupling = 0.0f;
    // Moving: the object's velocity this frame (zero after a teleport).
    Vector3 velocity{0.0f, 0.0f, 0.0f};
    // AIR.4D D2.1, Omni and Vortex: the ellipsoid's or ring's semi-axes along axisX, axisY (and axisZ for an Omni); (0, 0, 0) is a
    // point Omni. `radius` is then the influence distance beyond the surface or ring.
    Vector3 emissionSemiAxes{0.0f, 0.0f, 0.0f};
    // Identifies the motor in the emission status read back to the CPU (AirField::GetEmissionStatus); not part of the simulation.
    std::uint64_t tag = 0;
};

// AIR.3: a motor as the solver reads it. Must match AirMotor in AirSolver.hlsl.
struct GpuAirMotor
{
    // AIR.4D D2: the emission origin (a Moving motor's object position).
    float position[3] = {0.0f, 0.0f, 0.0f};
    std::uint32_t type = 0;
    float axisX[3] = {1.0f, 0.0f, 0.0f};
    float strength = 0.0f;
    float axisY[3] = {0.0f, 1.0f, 0.0f};
    float falloff = 0.0f;
    float axisZ[3] = {0.0f, 0.0f, 1.0f};
    float boundingRadius = 0.0f;
    // Directional: half extents; the others: radius in x (Omni and Vortex: the influence distance) and half length in y.
    float size[3] = {0.0f, 0.0f, 0.0f};
    float inwardPull = 0.0f;
    float velocity[3] = {0.0f, 0.0f, 0.0f};
    float coupling = 0.0f;
    // AIR.4D D2.1: the Omni ellipsoid's or Vortex ring's semi-axes ((0, 0, 0): a point Omni); then the motor's visibility slot
    // (0xFFFFFFFF: none, no force) and the cell crossings a path inside its bounding sphere can need.
    float semiAxes[3] = {0.0f, 0.0f, 0.0f};
    std::uint32_t visibilitySlot = 0;
    std::uint32_t maximumCrossings = 0;
    std::uint32_t padding[3] = {0, 0, 0};
};

// AIR.4D D2.1: a visibility cache key's words: position, axes, bounding radius, size and emission semi-axes as bits, then the type
// (AirField.cpp, VisibilityKey).
inline constexpr std::size_t AirVisibilityKeyWords = 20;

// AIR.4F: the passive mist field (AirMist.hlsl), carried by the air without acting on it. Its storage exists only while enabled.
struct AirMistSettings
{
    bool enabled = false;
    // No mist transport (and no dissipation); the air, recentring, rebuild remapping and clears continue.
    bool paused = false;
    // Per second: all mist (density and colour together) scales by exp(-dissipationRate * t).
    float dissipationRate = 0.0f;
    // AIR.4F.2d.4 Stage 2c.4: conservative finite-volume transport (AirMistFlux.hlsl) in projected steps, in place of the advective step;
    // on by default. Steps without the projection keep the advective transport whatever this says. The Engine clears it only for an
    // explicit ENGINE_AIR_CONSERVATIVE_MIST=0 (diagnostic A/B comparisons).
    bool conservativeTransport = true;
    // AIR.4F.2d.4 Stage 2b.1 (diagnostic): 0 chooses the transport substeps adaptively (1 to 4, from the largest Courant number); 1 to 4
    // forces that many.
    std::uint32_t forcedSubsteps = 0;

    bool operator==(const AirMistSettings&) const = default;
};

// AIR.4F: the mist's read-only views, for its consumers. Each piece's buffer holds one float4 per cell in the velocity layout:
// premultiplied (density-weighted colour, density). Valid only with the field's binding, and only while mist is enabled.
struct AirMistBinding
{
    std::uint32_t valid = 0;
    std::uint32_t piece0Index = 0;
    std::uint32_t piece1Index = 0;
    std::uint32_t generation = 0;
};

// AIR.4F F2: one Mist Emitter of the frame, in world space. Built by the Engine from a MistEmitterComponent and its object's presented
// pose (unit axes: the object's rotation, then the emitter's emission rotation; scale ignored).
struct MistEmitterInstance
{
    // AIR.4F.2b: Legacy fills its shape at densityRate; Point adds massRate within a footprint of `radius` (MistEmitterComponent.h).
    MistEmitterMode mode = MistEmitterMode::Legacy;
    MistEmitterShape shape = MistEmitterShape::Sphere;
    // The emission origin, where the shape is centred.
    Vector3 position{0.0f, 0.0f, 0.0f};
    Vector3 axisX{1.0f, 0.0f, 0.0f};
    Vector3 axisY{0.0f, 1.0f, 0.0f};
    Vector3 axisZ{0.0f, 0.0f, 1.0f};
    // Legacy: the sphere's radius. Point: the footprint radius (MistEmitterPointFootprint, at most MaximumPointRadius).
    float radius = 0.5f;
    Vector3 halfExtents{0.5f, 0.5f, 0.5f};
    // Legacy: density per second within the shape: the component's rate times its density multiplier.
    float densityRate = 0.0f;
    // AIR.4F.2b, Point: density x m^3 per second (the component's emission rate).
    float massRate = 0.0f;
    // AIR.4F.2b.1, Point: the temperature response rate, per second. AIR.4F.2c, Volume: its temperature response, per second.
    float temperatureRate = MistEmitterComponent::DefaultTemperatureResponseRate;
    // AIR.4F.2c, Volume: the box is halfExtents along the axes; target density, replenishment rate (1/s), fill height and edge softness.
    float targetDensity = 0.0f;
    float replenishmentRate = 0.0f;
    float fillHeight = 1.0f;
    float edgeSoftness = 0.0f;
    Vector3 color{1.0f, 1.0f, 1.0f};
    // AIR.4F F4.5.1b: ΔT in kelvin that the air inside the shape approaches (0: no heating).
    float temperature = 0.0f;
    // Identifies the emitter in the status read back to the CPU (AirField::GetMistEmitterStatus); not part of the simulation.
    std::uint64_t tag = 0;
};

// AIR.4F F2: an emitter as the mist step reads it. Must match MistEmitter in AirMist.hlsl.
struct GpuMistEmitter
{
    float position[3] = {0.0f, 0.0f, 0.0f};
    std::uint32_t shape = 0;
    float axisX[3] = {1.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    float axisY[3] = {0.0f, 1.0f, 0.0f};
    float densityPerStep = 0.0f;
    float axisZ[3] = {0.0f, 0.0f, 1.0f};
    std::uint32_t maximumCrossings = 0;
    float halfExtents[3] = {0.0f, 0.0f, 0.0f};
    // AIR.4F F4.5.1b: ΔT the air approaches, bounded to the field's range (0: no heating).
    float temperature = 0.0f;
    float color[3] = {0.0f, 0.0f, 0.0f};
    // AIR.4F.2b: MistEmitterMode (Legacy: radius is the sphere's, densityPerStep a density; Point: radius is the footprint's,
    // densityPerStep the quantity per step, density x m^3).
    std::uint32_t mode = 0;
    // AIR.4F.2b.1, Point: the temperature response rate, per second (Legacy heats at its fixed 1/s and ignores it). AIR.4F.2c, Volume:
    // its temperature response; densityPerStep is then the target density.
    float temperatureRate = 1.0f;
    // AIR.4F.2c, Volume: the replenishment factor per step 1 - exp(-rate dt), the edge softness and the fill height (m).
    float replenishment = 0.0f;
    float edgeSoftness = 0.0f;
    float fillHeight = 0.0f;
};

// AIR.4E.4 (experimental; an Editor session setting, off by default, never saved): the pressure projection. After each solver step and
// before its mist step, the step's velocity is projected (AirPressure: right-hand side, warm-started V-cycles, gauge) and corrected (xyz
// only, then the wall constraint). Off, the field records no pressure work and holds no pressure storage.
struct AirPressureRequest
{
    // V-cycles per projected step (warm-started). AIR.4E.5b measured 3 to keep the predicted face divergence within 1e-3 of the outflow
    // before with about 8x margin in open, dense and pocket scenes (2 left only 2.5x in the dense scene); Documentation/Air/AirPressure4E5b.md.
    static constexpr std::uint32_t DefaultCycles = 3;

    // AIR.4E.5b: V-cycles for a step whose solver was just rebuilt from phi = 0 (a reset or a geometry change; recentres keep phi), so
    // a cold start still meets the convergence criterion: measured, 12 do (5.4e-4 at worst), 8 do not (2.8e-3). The step's other settings
    // are `solve`'s.
    static constexpr std::uint32_t DefaultColdCycles = 12;

    // AIR.4F.1b: the budgets while buoyancy acts (buoyancyGravity > 0); without it the budgets above are unchanged. Measured in
    // Documentation/Air/AirBuoyancyCorrectness4F1b.md: with buoyancy, phi holds a large hydrostatic part, and 3 warm cycles leave up to 3e-3 of
    // the outflow after an abrupt change of forcing, and up to 1.7e-2 for a few steps after a recentre (entering cells start from phi = 0).
    // Warm steps take buoyantCycles; a recentre's step and the buoyantRecentreSteps - 1 steps after it take buoyantRecentreCycles.
    static constexpr std::uint32_t DefaultBuoyantCycles = 4;
    static constexpr std::uint32_t DefaultBuoyantRecentreCycles = DefaultColdCycles;
    static constexpr std::uint32_t DefaultBuoyantRecentreSteps = 3;
    // AIR O2.1: adaptive warm cycles. A warm step runs this many V-cycles unconditionally; each further cycle of its budget (cycles; buoyant
    // solves keep fixed budgets) runs only while the residual is above the convergence criterion, decided on the GPU (the recovery gate). The budget,
    // the criterion and cold starts (coldCycles, always run) are unchanged. 0 runs every cycle of the budget (the fixed solver).
    // AIR O2.2: the red-black sweeps per side of those adaptive solves (cold and buoyant solves keep solve.smoothingSweeps). Measured, 3
    // sweeps with a minimum of 1 cycle meet the criterion and save about 0.28 ms per warm step, but a gated second cycle then runs in one
    // run of the projection suite's isolation check and not the other, so a sealed room's velocity differs by about 4e-5 relative (not bit-
    // identical). Not the default pending review (AirOptimizationInvestigation.md, O2.2): 2 and 2 are O2.1.
    static constexpr std::uint32_t DefaultAdaptiveMinimumCycles = 2;
    static constexpr std::uint32_t DefaultAdaptiveSmoothingSweeps = 2;

    bool enabled = false;
    AirPressureSolveSettings solve{DefaultCycles};
    std::uint32_t coldCycles = DefaultColdCycles;
    std::uint32_t buoyantCycles = DefaultBuoyantCycles;
    std::uint32_t buoyantRecentreCycles = DefaultBuoyantRecentreCycles;
    std::uint32_t buoyantRecentreSteps = DefaultBuoyantRecentreSteps;
    std::uint32_t adaptiveMinimumCycles = DefaultAdaptiveMinimumCycles;
    std::uint32_t adaptiveSmoothingSweeps = DefaultAdaptiveSmoothingSweeps;
    // The divergence view's diagnostics: the last step's residual is recorded and the solver's buffers published (GetPressureDiagnostics).
    bool diagnostics = false;
    // AIR.4F.1b (an Editor session setting): temperature-driven buoyancy, the magnitude of gravity in m/s^2, acting along -y; 0 (the
    // default) is no buoyancy. It acts only through the projection, so it does nothing while `enabled` is false. The reference temperature
    // is the solver's ambientTemperature. Negative or non-finite values are no buoyancy.
    float buoyancyGravity = 0.0f;
};

// AIR.4F.3 F1a: one participating moving solid as AIR receives it. Engine converts Physics::MovingSolidSnapshot entries, so AIR never
// sees Physics. The shape is in the object's frame and the pose places it:
//   Box: center and half extents (no local rotation); Sphere: center and radius; Capsule: lowerCenter..upperCenter and radius.
enum class AirMovingSolidShape : std::uint32_t
{
    Box = 0,
    Sphere = 1,
    Capsule = 2
};

struct AirMovingSolid
{
    std::string objectID;
    // Physics' motion epoch: a different epoch is a discontinuity (teleport, spawn, rejoin), never swept or interpolated.
    std::uint64_t motionEpoch = 0;
    AirMovingSolidShape shape = AirMovingSolidShape::Box;
    Vector3 position{0.0f, 0.0f, 0.0f};
    // Unit quaternion x, y, z, w.
    std::array<float, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f};
    Vector3 center{0.0f, 0.0f, 0.0f};
    Vector3 halfExtents{0.0f, 0.0f, 0.0f};
    Vector3 lowerCenter{0.0f, 0.0f, 0.0f};
    Vector3 upperCenter{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    // AIR.4F.3 F2b.2a: the pose at the start of physics' most recent step (same epoch; absent after a discontinuity). With several AIR
    // steps in one frame, the steps before the last move toward it, so two steps reproduce the two physics steps instead of a straight line.
    bool hasPrevious = false;
    Vector3 previousPosition{0.0f, 0.0f, 0.0f};
    std::array<float, 4> previousOrientation{0.0f, 0.0f, 0.0f, 1.0f};
};

// AIR.4F.3 F1a (diagnostic, off by default): the frame's published moving solids. While enabled, AIR carves them out of its fluid pieces
// locally. Each frame the field moves every solid from the pose it last applied to the pose given here, over the frame's steps.
// AIR.4F.3 F1b: each geometry update also remaps velocity, ΔT and mist onto the new pieces in the same update: mist displaced by a solid
// is pushed into the fluid it touches (conservatively, with a ledger), newly exposed fluid starts clear with its neighbours' velocity.
// There is no moving-wall velocity yet (F2): displaced air's momentum is dropped, not pushed.
struct AirMovingSolidRequest
{
    bool enabled = false;
    std::vector<AirMovingSolid> solids;
    // AIR.4F.3 F1a.3 closeout: with the overlay on, the pressure solver keeps phi across its geometry changes, updates its components and
    // hierarchy locally (AirPressure::RecordIncrementalRebuild: a GPU topology check decides on a relabel), and solves with the warm budget
    // plus bounded recovery cycles while the residual is above the criterion. Diagnostics only (no production caller sets it): every
    // change is instead a full cold rebuild, F1a's behaviour, for comparisons.
    bool coldRebuildForDiagnostics = false;
    // AIR.4F.3 F2b.1, diagnostics only: classify updates on frames without AIR steps as continuous (as if a projected step followed), so a
    // diagnostic can read a continuous update's boundary state before any step consumes the scratch.
    bool continuousWithoutStepsForDiagnostics = false;
    // AIR.4F.3 F2b.5: the physics session and step the solids' poses belong to (pose: that step; previous: the one before). When
    // set (physicsStep > 0), each AIR step applies exactly one physics step of motion, tracked by a cursor, instead of the latest
    // pose: AIR and physics keep separate 60 Hz clocks, so a frame can run an AIR step without a physics step or the reverse, and the
    // latest pose then delivers 0 or 2 steps of motion to one AIR step (pulsing wall sources). 0 keeps the latest-pose behaviour.
    std::uint64_t physicsSession = 0;
    std::uint64_t physicsStep = 0;
    // AIR.4F.3 F2b.2a: geometry is step-aligned: a frame without an AIR step keeps the applied poses (maintenance re-stamps them), so every
    // transition has a projected step. Diagnostics only: apply the frame's poses on frames without steps too (the F1a, F1b and F2b.1
    // studies' exact frames).
    bool applyWithoutStepsForDiagnostics = false;
    // AIR.4F.3 F2b.2, diagnostics only (F2a's comparison): Coupled is production. NoWallSource treats every update as discontinuous (F1b
    // alone, no moving-wall airflow); Naive keeps the wall source but remaps mist with F1b and transports it in geometric volumes.
    enum class Coupling
    {
        Coupled,
        NoWallSource,
        Naive
    };
    Coupling couplingForDiagnostics = Coupling::Coupled;
    // AIR.4F.3 F2b.4: tangential wall coupling rate k in 1/s (0 to MaximumTangentialCouplingRate; 0 is F2b.2's normal-only coupling). Each
    // AIR step after a continuous update, fluid within one cell of a moving solid's surface moves its tangential velocity toward the wall's
    // by 1 - exp(-k w dt) (w: contact weight). Default chosen from AirFogDisplacement4F3Investigation.md, F2b.4.
    static constexpr float MaximumTangentialCouplingRate = 600.0f;
    static constexpr float DefaultTangentialCouplingRate = 30.0f;
    float tangentialCouplingRate = DefaultTangentialCouplingRate;
};

// AIR.4F.3 F1a diagnostics of the overlay: updates recorded, and the latest update's dirty boxes and cells.
struct AirMovingSolidStatistics
{
    std::uint64_t updates = 0;
    std::uint64_t fallbacks = 0;
    std::uint64_t truncatedFrames = 0;
    std::uint32_t lastBoxes = 0;
    std::uint64_t lastPieceCells = 0;
    std::uint64_t lastApertureCells = 0;
    std::uint32_t lastSolids = 0;
    // AIR.4F.3 F1a.2 / F1a.3: updates that kept phi; those the CPU classed as a connectivity risk (a solid appearing, leaving or
    // jumping, solids meeting, the fallback, restamping: diagnostics only since F1a.3); local and full pressure rebuilds after updates.
    std::uint64_t warmUpdates = 0;
    std::uint64_t coldBudgetUpdates = 0;
    std::uint64_t localPressureRebuilds = 0;
    std::uint64_t fullPressureRebuilds = 0;
    // AIR A2/T2a: local rebuilds whose relabel the CPU forced, by reason: several updates pending since the last solve, and an update box
    // past the GPU check's cell limit (both may hold).
    std::uint64_t forcedRelabelsPending = 0;
    std::uint64_t forcedRelabelsSize = 0;
    // AIR A2/T2a: updates whose face connections changed in more than one dirty box (all joined into one pending box), and the latest
    // local pressure update's box in cells.
    std::uint64_t multiBoxUpdates = 0;
    std::uint64_t lastLocalBoxCells = 0;
    // AIR A2/T2a: the latest update's first two face-connection boxes, window-local cells (minimum xyz, maximum xyz).
    std::array<std::array<std::int32_t, 6>, 2> lastApertureBoxes{};
    // AIR.4F.3 F1b: updates that remapped mist (mist allocated), and those that recorded the relay (a large or discontinuous change).
    std::uint64_t mistRemapUpdates = 0;
    std::uint64_t relayUpdates = 0;
    // AIR.4F.3 F2b.1: solid updates classified continuous (a wall source from their two applied poses), and discontinuous by reason: no
    // previous pose or a new motion epoch (appearance, teleport, rejoin), no projected step following, or a surface motion beyond one cell.
    std::uint64_t continuousSolidUpdates = 0;
    std::uint64_t jumpedNewOrEpoch = 0;
    std::uint64_t jumpedNoStep = 0;
    std::uint64_t jumpedReach = 0;
    // Updates whose boundary passes ran, and the solids corrected to their exact volume (isolated) or left uncorrected (bounds meeting
    // another solid's), over both pose lists.
    std::uint64_t boundaryUpdates = 0;
    std::uint64_t isolatedSolids = 0;
    std::uint64_t uncorrectedSolids = 0;
    // AIR.4F.3 F2b.2: updates remapped by control volumes (continuous) or F1b's geometric remap (any discontinuity, including a mixed
    // update), and updates whose boundary cells exceeded the records (effective volumes reverted to geometric).
    std::uint64_t continuousRemapUpdates = 0;
    std::uint64_t discontinuousRemapUpdates = 0;
    std::uint64_t boundaryOverflows = 0;
    // AIR.4F.3 F2b.2a: frames without an AIR step whose snapshot waited for the next stepped frame (step-aligned geometry).
    std::uint64_t deferredFrames = 0;
};

// One frame's air request; absent or disabled means no air work and an invalid binding.
struct AirFrameRequest
{
    bool enabled = false;
    // No simulation steps (and no time accumulates); recentring and obstacle updates continue.
    bool paused = false;
    // AIR.4E.5b.1 (an Editor session setting): the most fixed steps one frame may run, 1 or 2 (AirField::MaximumStepsPerFrame, the
    // default). The step stays StepSeconds; with 1, frames longer than a step run one step and the simulation runs slower than real time.
    std::uint32_t maximumStepsPerFrame = 2;
    // Wall-clock time since the previous frame; the solver steps in fixed StepSeconds increments.
    double deltaSeconds = 0.0;
    Vector3 cameraPosition{0.0f, 0.0f, 0.0f};
    AirFieldSettings settings;
    AirSolverSettings solver;
    // AIR.3: at most MaximumMotors are applied; motors whose region misses the window are skipped.
    std::vector<AirMotorInstance> motors;
    AirMistSettings mist;
    // AIR.4F F2: applied only while mist is enabled; the first MaximumMistEmitters whose shape reaches the window emit, in order.
    // AIR.4F.2b: a Point emitter's jet is a Moving motor in `motors`, after every Air Motor component's.
    std::vector<MistEmitterInstance> mistEmitters;
    // AIR.4E.4: the experimental pressure projection.
    AirPressureRequest pressure;
    // AIR.4F.3 F1a: the moving-solid geometry overlay (diagnostic, off by default).
    AirMovingSolidRequest movingSolids;
};

class AirField
{
public:
    // Cells the camera may move from the window's centre before it recentres, and the recentring step.
    static constexpr std::int32_t RecentreStep = 8;
    // Fixed solver step, and the most steps one frame may run; time beyond that is dropped rather than carried.
    static constexpr double StepSeconds = 1.0 / 60.0;
    // The part of a frame's upload ring a step after the first must leave for the rest of the renderer (it allocates after AIR). Measured
    // with moving solids and buoyancy: a step records up to about 420 KiB of constants, the rest of a Demo Scene frame about 30 KiB.
    static constexpr std::size_t UploadReserveBytes = 256 * 1024;
    static constexpr std::uint32_t MaximumStepsPerFrame = 2;
    static constexpr std::uint32_t MaximumMotors = 64;
    static constexpr std::uint32_t MaximumMistEmitters = 32;
    // AIR.4F.3 F1a: the overlay's solids per frame (the first by object ID; the rest are not carved, counted in truncatedFrames), the
    // dirty boxes one update dispatches (AirMovingSolids.hlsl), and the dirty share of the window above which one update covers the
    // whole window instead (still only pieces and face connections).
    static constexpr std::uint32_t MaximumMovingSolids = 32;
    static constexpr std::uint32_t MovingSolidBoxCapacity = 64;
    static constexpr std::uint32_t MovingSolidFallbackDivisor = 8;
    // AIR.4F.3 F1b: the most cells displaced mist is relayed through a solid to reach fluid (AirMovingSolids.hlsl, AirRemapRelayLevels).
    static constexpr std::uint32_t MovingSolidRelayLevels = 8;

    // AIR.4F.1b: the projection's buoyancy for a request: impulse gravity * StepSeconds (0 for no, negative or non-finite gravity) and
    // the solver's ambient, bounded to AirEnvironmentSettings' range, as the reference temperature.
    static AirPressureBuoyancy PressureBuoyancy(const AirFrameRequest& request);

    // R1.2a: never called in a packaged runtime (its shaders are not packaged; PackagedShaderInventory.h). An uninitialized field is
    // off: Record treats every request as disabled, so its binding stays invalid and no storage or GPU work exists.
    bool Initialize(ID3D12Device* device, DescriptorHeap& descriptorHeap, ShaderLibrary& shaderLibrary);
    // After the Renderer's GPU drain.
    void Shutdown();

    // The next enabled frame re-initializes the whole field (scene change, physics session change, the Air window's Reset). R1.6: the
    // mist's transport clock (the detail noise phase) restarts with it, so a reset field renders as a freshly started one.
    void ResetField() noexcept
    {
        m_resetPending = true;
        m_mistSeconds = 0.0;
    }

    // AIR.4F: the next frame with mist enabled zeroes all of it.
    void ClearMist() noexcept
    {
        m_mistClearPending = true;
    }

    // Releases the storage (retired with `lastSubmissionFence`); the next enabled frame allocates again.
    void Release(std::uint64_t lastSubmissionFence);

    // Inside Render, after the VX.5 residency uploads, before any pass; `upload` is the frame's constant ring.
    void Record(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, UploadBuffer& upload, const AirFrameRequest& request,
        const VoxelWorldBinding& voxels, unsigned int frameIndex, std::uint64_t lastSubmissionFence, std::uint64_t completedFence);

    // AIR.4D D2: after the frame's submission, the fence that marks its emission status copy complete.
    void MarkSubmitted(unsigned int frameIndex, std::uint64_t fence) noexcept;

    // AIR.4D D2: the latest emission status read back for each motor the field ran (by AirMotorInstance::tag), a few frames old;
    // empty while air is off.
    const std::map<std::uint64_t, AirMotorEmissionStatus>& GetEmissionStatus() const noexcept
    {
        return m_emission;
    }

    const AirFieldBinding& GetBinding() const noexcept
    {
        return m_binding;
    }

    // AIR.4E.1: the face connections' read-only view (Air.hlsli, AirApertureOf), valid while the binding is; UINT32_MAX without storage.
    std::uint32_t GetApertureIndex() const noexcept
    {
        return m_allocated ? m_apertures.GetDescriptorIndex() : UINT32_MAX;
    }

    // AIR.4E.3: how many times the face connections were rebuilt; the pressure solver's topology key (a rebuild is due whenever it changes).
    std::uint64_t GetApertureBuilds() const noexcept
    {
        return m_apertureBuilds;
    }

    // AIR.4E.3: the pressure solver, part of the window's storage (allocated on first use, retired with the field's buffers). Only
    // diagnostics record it in AIR.4E.3: no frame does pressure work, and the air's velocity never reads it.
    // R1.5b: whether Initialize created every pipeline (false in a packaged runtime until its scene requests AIR).
    bool IsInitialized() const noexcept
    {
        return m_initialized;
    }

    AirPressure& GetPressure() noexcept
    {
        return m_pressure;
    }

    // AIR.4E.5a diagnostics: called between the frame's air stages (named after the stage just recorded), so a profiler can insert
    // timestamps inside real frames; empty in production.
    void SetStageMarker(AirPressure::StageMarker marker)
    {
        m_stageMarker = std::move(marker);
    }

    // AIR.4F.3 F1a: the overlay's counters, and the solids as the field last applied them (by object ID).
    // Steps deferred to the next frame because the frame's upload ring could not hold them (UploadReserveBytes).
    std::uint64_t GetUploadDeferredSteps() const noexcept
    {
        return m_uploadDeferredSteps;
    }

    const AirMovingSolidStatistics& GetMovingSolidStatistics() const noexcept
    {
        return m_movingSolidStatistics;
    }

    const std::map<std::string, AirMovingSolid, std::less<>>& GetAppliedMovingSolids() const noexcept
    {
        return m_appliedMovingSolids;
    }

    // AIR.4E.5b.1: the fixed steps the latest Record actually recorded (0 when air is off or paused, no time was due, or recording failed).
    std::uint32_t GetLastStepCount() const noexcept
    {
        return m_lastStepCount;
    }

    // AIR.4E.4: the solver's right-hand side and residual of this frame's last projected step (AirPressureRequest::diagnostics), for the
    // divergence view; invalid on frames without one. They describe the same step as the published velocity.
    const AirPressureDiagnostics& GetPressureDiagnostics() const noexcept
    {
        return m_pressureDiagnostics;
    }

    const AirMistBinding& GetMistBinding() const noexcept
    {
        return m_mistBinding;
    }

    // AIR.4F F4.1: seconds of mist transport so far (StepSeconds per mist step; paused mist does not advance it), which drives the
    // rendered detail's flow phases.
    double GetMistSeconds() const noexcept
    {
        return m_mistSeconds;
    }

    // AIR.4F F2: the latest status read back for each emitter that emitted (by MistEmitterInstance::tag), a few frames old; empty
    // while mist is off.
    const std::map<std::uint64_t, MistEmitterStatus>& GetMistEmitterStatus() const noexcept
    {
        return m_mistEmitterStatus;
    }

    // AIR.4F.2b: the tags of the latest frame's motors whose region reached the window but found every motor slot taken (they
    // moved no air), in request order.
    const std::vector<std::uint64_t>& GetMotorsWithoutCapacity() const noexcept
    {
        return m_motorsWithoutCapacity;
    }

    static bool ValidateSettings(const AirFieldSettings& settings, std::string* error);

private:
    friend class AirFieldVerification;
    friend class AirMotorEditorVerification;
    friend class GamePlayGPUVerification;
    friend class AirPackagedParityVerification;
    friend class MemoryRegistryVerification;

    struct RetiredBuffer
    {
        Buffer buffer;
        std::uint64_t fence = 0;
    };

    bool Allocate(const AirFieldSettings& settings);
    // AIR.4E.5d: grows the visibility and emission-sample storage to hold `motors` slots (powers of two, at most MaximumMotors). The
    // old buffers retire on `fence`; every slot is recomputed after a growth.
    bool EnsureVisibilityCapacity(std::size_t motors, std::uint64_t fence);

    // Every buffer of the field's storage, for allocation, transitions and retirement alike.
    std::array<Buffer*, 13> StorageBuffers() noexcept
    {
        return {&m_velocity[0], &m_velocity[1], &m_piece1Velocity[0], &m_piece1Velocity[1], &m_obstacles, &m_pieces, &m_visibility,
            &m_emissionStatus, &m_emissionSamples, &m_splitList, &m_splitArguments, &m_mistStatus, &m_apertures};
    }

    // AIR.4F: the four mist buffers (piece 0's pair, then piece 1's), allocated only while mist is enabled.
    std::array<Buffer*, 4> MistBuffers() noexcept
    {
        return {&m_mist[0], &m_mist[1], &m_mistPiece1[0], &m_mistPiece1[1]};
    }

    bool AllocateMist(const AirFieldSettings& settings);
    void RetireMist(std::uint64_t fence);
    // Every buffer the frame's air work touches, in `state`.
    void TransitionStorage(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES state);
    bool RecordMistClear(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next);
    // AIR.4F F2: the frame's emitters that reach the window, in the GPU layout, uploaded once for every step, with each one's box of
    // cells within the window; returns how many.
    std::uint32_t UploadMistEmitters(UploadBuffer& upload, const AirFieldBinding& next, const AirFrameRequest& request,
        D3D12_GPU_VIRTUAL_ADDRESS& address);
    // The emitters' origin states into the status buffer, before the frame's mist steps; then (after them) its copy for the CPU.
    bool RecordMistEmitterStatus(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next,
        D3D12_GPU_VIRTUAL_ADDRESS emitters, std::uint32_t emitterCount);
    void CopyMistEmitterStatus(ID3D12GraphicsCommandList* commandList, unsigned int frameIndex);
    void PollMistEmitterStatus(std::uint64_t completedFence);
    // `emitters` (emitterCount of them) emit after the step's transport; `step` is the step of the frame (0 counts receiving pieces
    // into the status; AIR.4F.2b, each step has its own Point weight words).
    bool RecordMistStep(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next, const AirMistSettings& mist,
        D3D12_GPU_VIRTUAL_ADDRESS emitters = 0, std::uint32_t emitterCount = 0, std::uint32_t step = 0);
    void RetireStorage(std::uint64_t fence);
    void CollectRetired(std::uint64_t completedFence);
    // Steps due this frame from the accumulator (none while paused).
    std::uint32_t TakeSteps(const AirFrameRequest& request);
    bool RecordMaintenance(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next, bool full, bool shifted,
        bool voxelsChanged, const VoxelWorldBinding& voxels);
    bool RecordStep(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next, const AirFrameRequest& request,
        D3D12_GPU_VIRTUAL_ADDRESS motors, std::uint32_t motorCount);
    // AIR.4E.4: projects the step just recorded (the current buffers); the storage is writable before and after. `lastStep` records the
    // divergence view's diagnostics when requested.
    bool RecordProjection(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next, const AirFrameRequest& request,
        bool lastStep, bool conservativeMist);
    // AIR.4F.2d.4 Stage 2a: the conservative mist transport of a projected step (AirMistFlux.hlsl), between its solve and its correction;
    // `current` is the projection's binding (the step's provisional velocity). The mist is left in the current buffers.
    bool RecordMistFlux(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& current, std::uint32_t apertures,
        const AirFrameRequest& request, const AirPressureBuoyancy& buoyancy);
    // AIR.4E.4: the pressure solver's storage, retired with `fence` (projection disabled, or the field's storage going).
    void RetirePressure(std::uint64_t fence);
    void Mark(ID3D12GraphicsCommandList* commandList, const char* stage) const;
    // The frame's motors that reach the window, in the GPU layout, uploaded once for every step; returns how many. `recompute`
    // receives the motors whose visibility must be computed this frame.
    std::uint32_t UploadMotors(UploadBuffer& upload, const AirFieldBinding& next, const AirFrameRequest& request, D3D12_GPU_VIRTUAL_ADDRESS& address,
        std::vector<std::uint32_t>& recompute);
    // AIR.4D D2: gives each motor a visibility slot (its own, if its geometry already has one) and lists those to recompute.
    void AssignVisibility(std::vector<GpuAirMotor>& motors, const AirFieldBinding& next, std::vector<std::uint32_t>& recompute);
    // AIR.4D D2: the visibility dispatches, the emission status and its copy, then a barrier before the steps.
    bool RecordMotorWork(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next, D3D12_GPU_VIRTUAL_ADDRESS motors,
        std::uint32_t motorCount, const std::vector<std::uint32_t>& recompute, unsigned int frameIndex);
    void PollEmissionStatus(std::uint64_t completedFence);
    // AIR.4F.3 F1a: re-carves the cells `previous` and `target` disagree on (all of both when `restampAll`: after maintenance rebuilt pieces
    // from residency alone), then the face connections reading them. F1b: remaps velocity, ΔT and mist onto the new pieces in the same
    // update. False only when recording failed (no room for constants): the GPU geometry and state are then unchanged.
    bool RecordMovingSolidUpdate(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next,
        const VoxelWorldBinding& voxels, const std::map<std::string, AirMovingSolid, std::less<>>& previous,
        const std::map<std::string, AirMovingSolid, std::less<>>& target, bool restampAll, bool warmPressure, bool continuousStep);

    ID3D12Device* m_device = nullptr;
    DescriptorHeap* m_descriptorHeap = nullptr;
    // R1.2a: set once Initialize has created every pipeline and readback; false in a packaged runtime.
    bool m_initialized = false;
    ComputePipeline m_initializePipeline;
    ComputePipeline m_obstaclePipeline;
    ComputePipeline m_piecePipeline;
    ComputePipeline m_aperturePipeline;
    // AIR.4E.1: how many times the face connections were rebuilt (a diagnostic).
    std::uint64_t m_apertureBuilds = 0;
    // AIR.4F.3 F1a: the overlay's passes, the solids as last applied, and its counters.
    ComputePipeline m_movingSolidPiecePipeline;
    ComputePipeline m_movingSolidAperturePipeline;
    std::map<std::string, AirMovingSolid, std::less<>> m_appliedMovingSolids;
    AirMovingSolidStatistics m_movingSolidStatistics;
    // AIR.4F.3 F1a.2: the overlay's truncation counters (five words, AirMovingSolids.hlsl; cumulative, zero at creation: committed buffers
    // are created zeroed), allocated on the overlay's first update and retired with the field. Always in the UAV state.
    Buffer m_truncationCounters;
    // AIR.4F.3 F1b: the remap passes; the remap ledger (AirMovingSolids.hlsl, cumulative, zero at creation) and the relay predicate (the
    // count of orphan regions touching no fluid, zeroed by the distribute pass), created on first use and retired with the field.
    ComputePipeline m_movingSolidClassifyPipeline;
    ComputePipeline m_movingSolidRelayDistancePipeline;
    ComputePipeline m_movingSolidRelayFlowPipeline;
    ComputePipeline m_movingSolidDistributePipeline;
    Buffer m_remapLedger;
    Buffer m_remapPredicate;
    static constexpr std::size_t RemapLedgerBytes = 80;
    // AIR.4F.3 F2b.1: the moving-boundary passes; per slot the effective-volume offset S = G - C(new pose) and the wall source D =
    // C(anchor) - C(new pose) (float2 each, sub-cell volumes; zero away from moving solids); per solid entry the coverage-correction sums;
    // the boundary diagnostics. Created on the overlay's first update, retired with the field. The receiver boxes of the latest update
    // (window cells, minimum and maximum), whose wall source a step without an update clears.
    ComputePipeline m_boundaryCorrectionClearPipeline;
    ComputePipeline m_boundaryCorrectionSumsPipeline;
    ComputePipeline m_boundaryVolumesPipeline;
    ComputePipeline m_boundaryGatherPipeline;
    ComputePipeline m_boundaryClearPipeline;
    Buffer m_effectiveOffset;
    Buffer m_wallSource;
    Buffer m_boundaryCorrection;
    Buffer m_boundaryDiagnostics;
    static constexpr std::size_t BoundaryCorrectionStride = 32;
    static constexpr std::size_t BoundaryDiagnosticsBytes = 64;
    std::vector<std::array<std::int32_t, 6>> m_boundaryReceivers;
    // AIR.4F.3 F2b.2: the coupled remap's passes; the boundary cells' records (AirMovingSolids.hlsl, AirBoundaryRecordStride each) and
    // their size; the closed components' wall-source sums (16 B per closed code, zeroed before each projection). The records grow only
    // before their first use in a frame (`m_boundaryRecordsUsed`), retired with the frame's last submission fence.
    ComputePipeline m_boundarySnapshotPipeline;
    ComputePipeline m_boundaryToGeometricPipeline;
    ComputePipeline m_boundaryZeroPipeline;
    Buffer m_boundaryRecords;
    std::uint64_t m_boundaryRecordBytes = 0;
    Buffer m_closedSources;
    static constexpr std::uint64_t BoundaryRecordStride = 112;
    static constexpr std::uint64_t BoundaryRecordCapacity = 1u << 18;
    std::uint64_t m_frameSubmissionFence = 0;
    bool m_boundaryRecordsUsed = false;
    // A wall source may be nonzero somewhere (an update wrote one since the last clear).
    bool m_wallSourceWritten = false;
    std::uint64_t m_uploadDeferredSteps = 0;
    // F2b.5: the physics session and step whose poses the latest geometry update applied (AirMovingSolidRequest::physicsStep).
    std::uint64_t m_physicsCursorSession = 0;
    std::uint64_t m_physicsCursor = 0;
    // F2b.2 diagnostics: the frame's coupling mode (AirMovingSolidRequest::couplingForDiagnostics).
    AirMovingSolidRequest::Coupling m_couplingForDiagnostics = AirMovingSolidRequest::Coupling::Coupled;
    // F2b.4: the latest continuous update's boundary constants (raw AirMovingSolidConstants) and solids, for the tangential wall coupling of
    // the step that follows it (CSWallTangential, before that step's projection); consumed by that step.
    std::vector<std::uint8_t> m_tangentialConstants;
    D3D12_GPU_VIRTUAL_ADDRESS m_tangentialSolids = 0;
    bool m_tangentialPending = false;
    ComputePipeline m_wallTangentialPipeline;
    // AIR.4F.3 F2b.1: clears the wall source over the latest receiver boxes (a step without a geometry update), or, with `all`, both
    // boundary buffers over the window (after maintenance rebuilt pieces from residency alone).
    bool RecordBoundaryClear(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next, bool all);
    // AIR.4F.3 F2b.4: the tangential wall coupling (CSWallTangential) of the latest continuous update, at `rate` 1/s.
    void RecordWallTangential(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, float rate);
    // AIR.4F.3 F2b.2: zeroes the closed components' wall-source sums before a projection reads the wall source.
    bool RecordClosedSourceClear(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next);
    // AIR.4F.3 F1a.3: the overlay updates since the pressure solver was last rebuilt, and the window-local fine cells they changed (pieces
    // and face connections, grown by one): the next projected step's local rebuild covers them.
    std::uint32_t m_pressurePendingUpdates = 0;
    std::array<std::int32_t, 3> m_pressurePendingLow{0, 0, 0};
    std::array<std::int32_t, 3> m_pressurePendingHigh{0, 0, 0};
    // The topology check's limit on its box's cells (AirPressure.hlsl, TopologyNodeCapacity / 2); larger boxes relabel.
    static constexpr std::uint64_t PressureTopologyCellLimit = 2048;
    ComputePipeline m_movingSolidTopologyResetPipeline;
    // AIR.4D D2 two-pass step: CSStepOpen (every cell), then CSStepSplit dispatched indirectly over the cells it listed.
    ComputePipeline m_stepPipeline;
    ComputePipeline m_splitStepPipeline;
    ComputePipeline m_prepareSplitPipeline;
    ComputePipeline m_resetSplitPipeline;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_splitSignature;
    // The work list (word 0: count, then one slot index per listed cell; capacity: every cell) and the counter with the split pass's
    // dispatch arguments.
    Buffer m_splitList;
    Buffer m_splitArguments;
    bool m_splitResetPending = true;
    ComputePipeline m_visibilityPipeline;
    ComputePipeline m_statusPipeline;
    // AIR.4D D2.1: CSEmissionSamples, the shaped emitters' lattice states.
    ComputePipeline m_samplesPipeline;

    // AIR.4F mist: CSMistOpen (every cell), then CSMistSplit over the cells it listed, through the solver's work list buffers.
    ComputePipeline m_mistOpenPipeline;
    ComputePipeline m_mistSplitPipeline;
    ComputePipeline m_mistPreparePipeline;
    ComputePipeline m_mistClearPipeline;
    // AIR.4F F2 emission: CSMistEmit per emitter, CSMistEmitterStatus once a frame.
    ComputePipeline m_mistEmitPipeline;
    ComputePipeline m_mistEmitterStatusPipeline;
    // AIR.4F.2b: the Point passes, two per Point emitter and step.
    ComputePipeline m_mistPointSumPipeline;
    ComputePipeline m_mistPointDepositPipeline;
    // AIR.4F.2c: the shared Volume pass, one per step for all Volume emitters.
    ComputePipeline m_mistVolumePipeline;
    // AIR.4F.2d.4 Stage 2a: the conservative transport's two passes (AirMistFlux.hlsl).
    ComputePipeline m_mistFluxOutPipeline;
    ComputePipeline m_mistFluxUpdatePipeline;
    // AIR.4F.2d.4 Stage 2b.1: the Courant measurement and the substep decision (AirMistFlux.hlsl), with their small buffers, allocated
    // and retired with the mist: the substep state (N, dt / N, the largest Courant number, then one float per 8^3 thread group) and the
    // indirect dispatch records of substeps 2 to 4.
    ComputePipeline m_mistCourantPipeline;
    ComputePipeline m_mistSubstepsPipeline;
    Buffer m_mistSubstepState;
    Buffer m_mistSubstepArguments;
    // Set when the step's projection transported the mist conservatively, so its mist step only emits (into the current buffers).
    bool m_mistTransportedByFlux = false;
    // The frame's uploaded emitters' cell boxes (Legacy: within the window; Point: the whole footprint, which may extend beyond it)
    // and tags, in upload order; with each emitter's mode and whether it has a quantity to deposit (Point only).
    struct EmitterBox
    {
        std::array<std::int32_t, 3> minimum{};
        std::array<std::uint32_t, 3> size{};
        MistEmitterMode mode = MistEmitterMode::Legacy;
        bool deposits = false;
    };

    // AIR.4F.2c: the cells covering every uploaded Volume emitter's box within the window (one shared pass), when there is one.
    bool m_frameVolumes = false;
    EmitterBox m_frameVolumeBox;

    std::vector<EmitterBox> m_frameEmitterBoxes;
    std::vector<std::uint64_t> m_frameEmitterTags;
    Buffer m_mist[2];
    Buffer m_mistPiece1[2];
    // The buffer of each mist pair holding the latest mist; a mist step writes the other one, then they swap.
    std::uint32_t m_mistCurrent = 0;
    bool m_mistAllocated = false;
    bool m_mistClearPending = false;
    AirMistBinding m_mistBinding;

    Buffer m_velocity[2];
    // AIR.4D D2: piece 1's velocity, ping-ponged with m_velocity (piece 0) under the same m_current.
    Buffer m_piece1Velocity[2];
    // The velocity buffer of each pair holding the latest state; a step writes the other one, then they swap.
    std::uint32_t m_current = 0;
    double m_accumulator = 0.0;
    Buffer m_obstacles;
    // AIR.4D: two 64-bit fluid-piece masks per cell, rebuilt with the obstacle mask.
    Buffer m_pieces;
    // AIR.4E.1: each slot's face connections toward its +axis neighbours (Air.hlsli, AirApertureOf), 8 bytes per slot; rebuilt with the
    // pieces (CSBuildApertures). Not read by the simulation yet: the pressure projection (AIR.4E.3) will.
    Buffer m_apertures;
    AirPressure m_pressure;
    AirPressureDiagnostics m_pressureDiagnostics;
    // The solver's storage is in use by the projection (allocated or used by RecordProjection), so a frame with projection off retires
    // it. Storage a diagnostic drives directly (AIR.4E.3's verification) goes only with the field's storage.
    bool m_projectionStorage = false;
    // AIR.4E.5b: the window minimum of the solver's latest build, and whether its phi still fits the field's cells (only recentres since:
    // a reset or a VX.5 change can change any piece).
    std::array<std::int32_t, 3> m_pressureBuildMinimum{0, 0, 0};
    bool m_pressurePhiCompatible = false;
    // AIR.4F.1b: projected steps left that take the buoyant recentre budget (AirPressureRequest::buoyantRecentreSteps after a recentre).
    std::uint32_t m_pressureRecentreSteps = 0;
    AirPressure::StageMarker m_stageMarker;
    // Diagnostics: projected steps recorded.
    std::uint64_t m_projections = 0;
    // AIR.4E.5b.1: steps the latest Record recorded.
    std::uint32_t m_lastStepCount = 0;

    // AIR.4D D2 motor visibility: 2 bits per cell (piece 0, piece 1) in the obstacle mask's layout, for each of MaximumMotors slots.
    // An entry names the motor geometry its slot was computed for, and the obstacles (m_obstacleEpoch) and window it is valid for.
    struct VisibilityEntry
    {
        std::array<std::uint32_t, AirVisibilityKeyWords> key{};
        // 0: never computed.
        std::uint64_t epoch = 0;
        std::array<std::int32_t, 3> minimum{};
        std::uint64_t lastUsed = 0;
    };

    Buffer m_visibility;
    // AIR.4E.5d: the slots m_visibility and m_emissionSamples hold (1 to MaximumMotors); slots at or past it are never assigned.
    std::uint32_t m_visibilityCapacity = 1;
    // AIR.4D D2.1: per visibility slot, the lattice states of a shaped emitter (2 bits per sample), valid with the slot's bits.
    Buffer m_emissionSamples;
    std::array<VisibilityEntry, MaximumMotors> m_visibilityEntries{};
    // Grows with every obstacle and piece rebuild.
    std::uint64_t m_obstacleEpoch = 1;
    std::uint64_t m_visibilityFrame = 0;
    // The frame's uploaded motors and their tags, in upload order.
    std::vector<GpuAirMotor> m_frameMotors;
    std::vector<std::uint64_t> m_frameTags;
    // AIR.4F.2b: see GetMotorsWithoutCapacity.
    std::vector<std::uint64_t> m_motorsWithoutCapacity;

    // AIR.4D D2 emission status: written on the GPU each frame motors run, copied to one readback per frame slot, read once that
    // frame completes (the AutoExposurePass pattern).
    struct StatusSlot
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        std::vector<std::uint64_t> tags;
        bool recorded = false;
        std::uint64_t fence = 0;
    };

    Buffer m_emissionStatus;
    std::array<StatusSlot, 3> m_statusSlots;
    std::map<std::uint64_t, AirMotorEmissionStatus> m_emission;
    std::uint64_t m_emissionFence = 0;
    // AIR.4F F2: 32 bytes per Mist Emitter (origin state, receiving pieces; AIR.4F.2b, each step's Point weight sums), allocated with
    // the field and copied to one readback per frame slot like the motors' status.
    Buffer m_mistStatus;
    std::array<StatusSlot, 3> m_mistStatusSlots;
    std::map<std::uint64_t, MistEmitterStatus> m_mistEmitterStatus;
    std::uint64_t m_mistStatusFence = 0;
    bool m_allocated = false;
    AirFieldSettings m_settings;
    bool m_resetPending = true;
    std::array<std::int32_t, 3> m_minimum{0, 0, 0};
    // What the obstacle mask was last built from.
    std::uint32_t m_voxelGeneration = 0;
    std::uint32_t m_voxelValid = 0;
    std::deque<RetiredBuffer> m_retired;
    AirFieldBinding m_binding;

    // Diagnostics: dispatches recorded, by kind.
    std::uint64_t m_fullInitializations = 0;
    std::uint64_t m_incrementalUpdates = 0;
    std::uint64_t m_obstacleRebuilds = 0;
    std::uint64_t m_steps = 0;
    std::uint32_t m_lastMotorCount = 0;
    // AIR.4D D2: motor visibility computed, and kept from earlier frames, per motor and frame.
    std::uint64_t m_visibilityComputes = 0;
    std::uint64_t m_visibilityKept = 0;
    // AIR.4F: mist steps and clears recorded.
    std::uint64_t m_mistSteps = 0;
    std::uint64_t m_mistClears = 0;
    double m_mistSeconds = 0.0;
    // AIR.4F F2: emitters uploaded on the latest frame with mist, and emission dispatches recorded.
    std::uint32_t m_lastMistEmitterCount = 0;
    std::uint64_t m_emitDispatches = 0;
    // AIR.4F.2b: Point passes recorded (sums and deposits). AIR.4F.2c: shared Volume passes recorded.
    std::uint64_t m_pointDispatches = 0;
    std::uint64_t m_volumeDispatches = 0;
    // AIR.4F.2d.4 Stage 2a: mist steps transported conservatively.
    std::uint64_t m_conservativeMistSteps = 0;
};
