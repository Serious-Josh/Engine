#pragma once

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
    MistEmitterShape shape = MistEmitterShape::Sphere;
    // The emission origin, where the shape is centred.
    Vector3 position{0.0f, 0.0f, 0.0f};
    Vector3 axisX{1.0f, 0.0f, 0.0f};
    Vector3 axisY{0.0f, 1.0f, 0.0f};
    Vector3 axisZ{0.0f, 0.0f, 1.0f};
    float radius = 0.5f;
    Vector3 halfExtents{0.5f, 0.5f, 0.5f};
    // Density per second within the shape: the component's rate times its density multiplier.
    float densityRate = 0.0f;
    Vector3 color{1.0f, 1.0f, 1.0f};
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
    std::uint32_t padding0 = 0;
    float color[3] = {0.0f, 0.0f, 0.0f};
    float padding1 = 0.0f;
};

// One frame's air request; absent or disabled means no air work and an invalid binding.
struct AirFrameRequest
{
    bool enabled = false;
    // No simulation steps (and no time accumulates); recentring and obstacle updates continue.
    bool paused = false;
    // Wall-clock time since the previous frame; the solver steps in fixed StepSeconds increments.
    double deltaSeconds = 0.0;
    Vector3 cameraPosition{0.0f, 0.0f, 0.0f};
    AirFieldSettings settings;
    AirSolverSettings solver;
    // AIR.3: at most MaximumMotors are applied; motors whose region misses the window are skipped.
    std::vector<AirMotorInstance> motors;
    AirMistSettings mist;
    // AIR.4F F2: applied only while mist is enabled; the first MaximumMistEmitters whose shape reaches the window emit, in order.
    std::vector<MistEmitterInstance> mistEmitters;
};

class AirField
{
public:
    // Cells the camera may move from the window's centre before it recentres, and the recentring step.
    static constexpr std::int32_t RecentreStep = 8;
    // Fixed solver step, and the most steps one frame may run; time beyond that is dropped rather than carried.
    static constexpr double StepSeconds = 1.0 / 60.0;
    static constexpr std::uint32_t MaximumStepsPerFrame = 2;
    static constexpr std::uint32_t MaximumMotors = 64;
    static constexpr std::uint32_t MaximumMistEmitters = 32;

    bool Initialize(ID3D12Device* device, DescriptorHeap& descriptorHeap, ShaderLibrary& shaderLibrary);
    // After the Renderer's GPU drain.
    void Shutdown();

    // The next enabled frame re-initializes the whole field (scene change).
    void ResetField() noexcept
    {
        m_resetPending = true;
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

    static bool ValidateSettings(const AirFieldSettings& settings, std::string* error);

private:
    friend class AirFieldVerification;
    friend class AirMotorEditorVerification;

    struct RetiredBuffer
    {
        Buffer buffer;
        std::uint64_t fence = 0;
    };

    bool Allocate(const AirFieldSettings& settings);

    // Every buffer of the field's storage, for allocation, transitions and retirement alike.
    std::array<Buffer*, 12> StorageBuffers() noexcept
    {
        return {&m_velocity[0], &m_velocity[1], &m_piece1Velocity[0], &m_piece1Velocity[1], &m_obstacles, &m_pieces, &m_visibility,
            &m_emissionStatus, &m_emissionSamples, &m_splitList, &m_splitArguments, &m_mistStatus};
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
    // `emitters` (emitterCount of them) emit after the step's transport; `countPieces` counts receiving pieces into the status.
    bool RecordMistStep(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next, const AirMistSettings& mist,
        D3D12_GPU_VIRTUAL_ADDRESS emitters = 0, std::uint32_t emitterCount = 0, bool countPieces = false);
    void RetireStorage(std::uint64_t fence);
    void CollectRetired(std::uint64_t completedFence);
    // Steps due this frame from the accumulator (none while paused).
    std::uint32_t TakeSteps(const AirFrameRequest& request);
    bool RecordMaintenance(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next, bool full, bool shifted,
        bool voxelsChanged, const VoxelWorldBinding& voxels);
    bool RecordStep(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next, const AirFrameRequest& request,
        D3D12_GPU_VIRTUAL_ADDRESS motors, std::uint32_t motorCount);
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

    ID3D12Device* m_device = nullptr;
    DescriptorHeap* m_descriptorHeap = nullptr;
    ComputePipeline m_initializePipeline;
    ComputePipeline m_obstaclePipeline;
    ComputePipeline m_piecePipeline;
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
    // The frame's uploaded emitters' cell boxes (within the window) and tags, in upload order.
    struct EmitterBox
    {
        std::array<std::int32_t, 3> minimum{};
        std::array<std::uint32_t, 3> size{};
    };

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
    // AIR.4D D2.1: per visibility slot, the lattice states of a shaped emitter (2 bits per sample), valid with the slot's bits.
    Buffer m_emissionSamples;
    std::array<VisibilityEntry, MaximumMotors> m_visibilityEntries{};
    // Grows with every obstacle and piece rebuild.
    std::uint64_t m_obstacleEpoch = 1;
    std::uint64_t m_visibilityFrame = 0;
    // The frame's uploaded motors and their tags, in upload order.
    std::vector<GpuAirMotor> m_frameMotors;
    std::vector<std::uint64_t> m_frameTags;

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
    // AIR.4F F2: one uint4 per Mist Emitter (origin state, receiving pieces), allocated with the field and copied to one readback per
    // frame slot like the motors' status.
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
};
