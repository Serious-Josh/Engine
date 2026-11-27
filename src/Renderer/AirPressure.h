#pragma once

#include "Renderer/Buffer.h"
#include "Renderer/ComputePipeline.h"
#include "Math/Vector3.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

struct AirFieldBinding;
class DescriptorHeap;
class ShaderLibrary;
class UploadBuffer;

// AIR.4E.3: the solver's diagnostic settings (no user-facing controls yet; AIR.4E.4 decides production values).
// AIR A2/T1 (diagnostics): what the recorded solves budgeted, cumulative since creation (the CPU side of the GPU recovery counters).
struct AirPressureSolveCounts
{
    std::uint64_t solves = 0;
    // The V-cycles every solve runs, and the recovery cycles it may run (each only while the residual is above the criterion).
    std::uint64_t fixedCycles = 0;
    std::uint64_t recoveryBudget = 0;
};

struct AirPressureSolveSettings
{
    // V-cycles per solve.
    std::uint32_t cycles = 2;
    // Red-black Gauss-Seidel sweeps before and after each coarse correction, on every level above the coarsest.
    std::uint32_t smoothingSweeps = 2;
    // Red-black sweeps on the coarsest level (solved in one thread group). AIR.4E.5b: 16 converge as 64 did (the same face divergence to
    // three digits, warm and cold) at a quarter of the coarsest solve's cost.
    std::uint32_t coarsestSweeps = 16;
    // Each coarse correction is scaled by this before it is added: piecewise-constant aggregation under-corrects smooth error. Measured
    // (AIR.4E.3): 1.8 with two sweeps converges about 0.41 per V-cycle on a developed motor field, against 0.65 at 1.0 with one sweep.
    float correctionScale = 1.8f;
    // Start from the previous solve's phi (invalidated by every rebuild); otherwise from phi = 0.
    bool warmStart = true;
    // AIR.4F.3 F1a.3: after `cycles`, at most this many more V-cycles, each run only while the residual is above the convergence
    // criterion (AirPressure.hlsl's CSRecoveryGate, on the GPU); 0 records none.
    std::uint32_t recoveryCycles = 0;
    // AIR solver restructure: how many of the first recovery cycles belong to the step's own cycle budget (AIR O2.1's adaptive warm cycles).
    // The gate counts the cycles run beyond them separately (AirPressure.hlsl's TopologyRecoveryBeyondBudget), so the adaptive recovery
    // budget tells true recovery from the warm budget's gated cycles.
    std::uint32_t budgetedRecoveryCycles = 0;
};

// AIR.4F.3 F1a.3: a local hierarchy update after moving-solid geometry changes: the fine cells whose pieces or face connections changed,
// grown by one (window-local, inclusive), and whether the components must be relabelled whatever the topology check finds.
struct AirPressureLocalUpdate
{
    std::array<std::int32_t, 3> minimum{0, 0, 0};
    std::array<std::int32_t, 3> maximum{0, 0, 0};
    bool forceRelabel = false;
};

// AIR.4F.1b: temperature-driven buoyancy, applied by the projection as a force on vertical faces (AirDivergence.hlsli's AirBuoyancy;
// Documentation/Air/AirBuoyancyImplementation4F1b.md). Bound as two root constants of CSPrepare and CSCorrect. The default (impulse 0)
// is no buoyancy: the solve and the correction are then exactly those before AIR.4F.1b.
struct AirPressureBuoyancy
{
    // gravity * step in m/s: the most velocity a vertical face gains from buoyancy in one step (|a| <= g).
    float impulse = 0.0f;
    // The ambient temperature the field's ΔT is measured from, in kelvin.
    float referenceTemperature = 293.15f;
};

// AIR.4F.3 F2b.2: the moving-wall source of a projected step (AirMovingSolids.hlsl's D: float2 per slot, sub-cell volumes per step) and its
// scale to the right-hand side's units (cell / (64 dt)); and the per-closed-component diagnostic sums (net and gross, 16 B per closed code).
// Read by CSPrepare only; UINT32_MAX: none, the solve unchanged.
struct AirPressureWallSource
{
    std::uint32_t sourceIndex = UINT32_MAX;
    float scale = 0.0f;
    std::uint32_t closedSourceIndex = UINT32_MAX;
    std::uint32_t padding = 0;
};

// AIR.4E.4: the latest projected step's solver buffers, for the divergence view (read-only views, valid for the frame that published
// them). rhs is b = -(outflow before the projection), made compatible in closed pockets; the residual b - K phi is minus the predicted
// face-flux divergence after it (AirPressure.hlsl).
struct AirPressureDiagnostics
{
    std::uint32_t valid = 0;
    std::uint32_t rhsIndex = 0;
    std::uint32_t residualIndex = 0;
    std::uint32_t generation = 0;
};

// GPU dispatches and barriers recorded, by stage (diagnostics).
struct AirPressureCounts
{
    std::uint32_t dispatches = 0;
    std::uint32_t barriers = 0;
};

// AIR.4E.3: the geometry-aware pressure solver for the air field's fluid pieces (AirPressure.hlsl; Documentation/Air/AirArchitecture.md,
// AIR.4E.3). It solves K phi = b, with one unknown per fluid piece, over the AIR.4E.1 face connections; it never changes the air's
// velocity, temperature or mist (AIR.4E.4 applies the correction).
//
// Owned by AirField and part of its window storage: allocated on first use (EnsureStorage), retired with the field's storage
// (StorageBuffers) and released once the GPU completes the fence. In AIR.4E.3 only diagnostics record it: production frames do no
// pressure work and hold no pressure memory.
//
// Rebuild (RecordRebuild): connected components (labels, open or closed, closed volumes) and the multigrid hierarchy, from the face
// connections; required again whenever they are rebuilt (AirField's aperture builds: resets, recentring, geometry). A rebuild also
// zeroes phi, so warm starts never cross a topology change.
//
// Solve (RecordSolve): the right-hand side (fused with the divergence), closed components' compatibility, V-cycles, and the closed
// components' gauge. The field's buffers must be in their shader-read state; the solver's own buffers stay writable.
class AirPressure
{
public:
    // Coarse nodes per coarse cell.
    static constexpr std::uint32_t SlotsPerCoarseCell = 4;
    static constexpr std::uint32_t ClosedComponentCapacity = 65'536;
    // The coarsest level is solved in one thread group: at most this many cells.
    static constexpr std::uint32_t CoarsestCellLimit = 512;
    // Hooking rounds with their own changed flags (AirPressure.hlsl's header).
    static constexpr std::uint32_t FlaggedRounds = 48;
    // AIR.4E.5b: rounds stop on the GPU as soon as one changes nothing (CSLabelGate), so the bound only caps pathological components:
    // every flagged round may be recorded at little cost.
    static constexpr std::uint32_t DefaultLabelRounds = FlaggedRounds;
    // Pointer-jumping passes after each hooking pass.
    static constexpr std::uint32_t JumpsPerRound = 3;

    // Header byte offsets of the closed-component table (AirPressure.hlsl).
    static constexpr std::uint32_t ClosedCountOffset = 0;
    static constexpr std::uint32_t OverflowCountOffset = 4;
    static constexpr std::uint32_t IsolatedCountOffset = 8;
    static constexpr std::uint32_t MaximumRhsOffset = 12;
    static constexpr std::uint32_t MaximumResidualOffset = 16;
    static constexpr std::uint32_t AggregateOverflowOffset = 20;
    static constexpr std::uint32_t ChangedFlagsOffset = 64;
    static constexpr std::uint32_t TableHeaderBytes = 256;
    static constexpr std::uint32_t TableEntryBytes = 32;
    // Component codes per piece.
    static constexpr std::uint32_t Open = 0xFFFF'FFFFu;
    static constexpr std::uint32_t Overflow = 0xFFFF'FFFEu;
    static constexpr std::uint32_t Isolated = 0xFFFF'FFFDu;
    static constexpr double FixedScale = 1'048'576.0;

    // One coarse level's shape and its regions in the hierarchy buffer (byte offsets). Mirrors AirPressureLevel in AirPressure.hlsl.
    struct Level
    {
        std::uint32_t dims[3] = {0, 0, 0};
        std::uint32_t cells = 0;
        std::uint32_t weights = 0;
        std::uint32_t diagonal = 0;
        std::uint32_t info = 0;
        std::uint32_t map = 0;
        std::uint32_t phi = 0;
        std::uint32_t rhs = 0;
        // AIR.4F.3 F1a.3: the aggregates' window-face weights (4 floats per cell), kept for the diagonal (AirPressure.hlsl).
        std::uint32_t boundary = 0;
        std::uint32_t padding = 0;
    };

    static_assert(sizeof(Level) == 48);

    // Keeps the device, heap and shader library; pipelines are created on first use.
    void Initialize(ID3D12Device* device, DescriptorHeap& descriptorHeap, ShaderLibrary& shaderLibrary) noexcept;
    // After the Renderer's drain (storage already retired or released by the owner).
    void Shutdown();

    // Storage for a window of `dimensions`; false when a pipeline or a buffer cannot be created.
    bool EnsureStorage(const std::array<std::uint32_t, 3>& dimensions);
    bool HasStorage() const noexcept
    {
        return m_allocated;
    }

    // Every buffer of the solver's storage, for the owner's retirement. Afterwards the solver holds none (ForgetStorage).
    std::array<Buffer*, 12> StorageBuffers() noexcept
    {
        return {&m_phi, &m_rhs, &m_scratch, &m_component, &m_fineMap, &m_table, &m_hierarchy, &m_labelArguments, &m_topology,
            &m_gateArguments, &m_relabelPredicate, &m_rhsGross};
    }

    void ForgetStorage() noexcept;

    // Components and hierarchy for the field's current pieces and face connections (`apertureIndex`), with `rounds` hooking rounds;
    // `topologyKey` names the connections it was built from (AirField's aperture build count).
    // AIR.4E.5b: `preservedMinimum` (the window minimum of the previous build, after nothing but recentres) keeps phi in the cells both
    // windows hold, as the warm start; null zeroes phi everywhere (a reset or a geometry change can change any piece).
    bool RecordRebuild(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, UploadBuffer& upload, const AirFieldBinding& field,
        std::uint32_t apertureIndex, std::uint64_t topologyKey, std::uint32_t rounds = DefaultLabelRounds,
        const std::array<std::int32_t, 3>* preservedMinimum = nullptr);
    // AIR.4F.3 F1a.3: the same after moving-solid geometry changes, from the hierarchy built before them: a topology check over the
    // update's box decides on the GPU whether the components are relabelled (CSTopologyCheck); the hierarchy is rebuilt only over each
    // level's dependency footprint of the box; phi is kept. Requires a built hierarchy of the same window.
    bool RecordIncrementalRebuild(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, UploadBuffer& upload,
        const AirFieldBinding& field, std::uint32_t apertureIndex, std::uint64_t topologyKey, const AirPressureLocalUpdate& update);
    // AIR.4F.3 F1a.3: the component codes' and topology buffer's writable views, for the moving-solid overlay (AirMovingSolids.hlsl).
    std::uint32_t GetComponentUnorderedIndex() const noexcept
    {
        return m_component.GetUAVDescriptorIndex();
    }

    // AIR A2/T1 (diagnostics): the recorded solves' budgets, cumulative.
    const AirPressureSolveCounts& GetSolveCounts() const noexcept
    {
        return m_solveCounts;
    }

    std::uint32_t GetTopologyUnorderedIndex() const noexcept
    {
        return m_topology.GetUAVDescriptorIndex();
    }

    // Whether the latest rebuild kept phi (otherwise it started from 0: a cold start).
    bool LastRebuildPreserved() const noexcept
    {
        return m_lastRebuildPreserved;
    }
    bool IsBuiltFor(std::uint64_t topologyKey) const noexcept
    {
        return m_allocated && m_built && m_topologyKey == topologyKey;
    }

    // One solve of the field's current velocity (published binding). Requires a rebuild for the current topology.
    bool RecordSolve(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, UploadBuffer& upload, const AirFieldBinding& field,
        std::uint32_t apertureIndex, const Vector3& ambientVelocity, const AirPressureSolveSettings& settings,
        const AirPressureBuoyancy& buoyancy = {}, const AirPressureWallSource& wallSource = {});
    // The residual b - K phi into the scratch buffer (and its largest magnitude into the table's header).
    // AIR optimization report item 4: copies the cumulative recovery counters (cycles run, then solves that ran out of budget; AIR solver
    // restructure: then cycles run beyond the steps' budgeted recovery cycles: three uints) into `destination` at `offset`; false (nothing
    // recorded) without storage.
    bool RecordRecoveryCounterCopy(ID3D12GraphicsCommandList* commandList, ID3D12Resource* destination, std::uint64_t offset);
    bool RecordResidual(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, UploadBuffer& upload, const AirFieldBinding& field,
        std::uint32_t apertureIndex);

    // AIR.4E.4: the correction of the current velocity buffers (writable views `velocityWriteIndex`, `piece1WriteIndex`) by the latest
    // solve's phi, then the wall constraint; xyz only. The two velocity buffers must be writable and the field's other buffers in their
    // shader-read state. AIR.4F.1b: `buoyancy` must be the one the solve's right-hand side used.
    bool RecordCorrect(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, UploadBuffer& upload, const AirFieldBinding& field,
        std::uint32_t apertureIndex, std::uint32_t velocityWriteIndex, std::uint32_t piece1WriteIndex, const AirPressureBuoyancy& buoyancy = {});
    // AIR.4E.4: makes the right-hand side and the residual (after RecordResidual) readable for this frame's divergence view.
    AirPressureDiagnostics PublishDiagnostics(ID3D12GraphicsCommandList* commandList);

    // phi's read-only view: a float2 per slot (AirDivergence.hlsli's pressure layout).
    std::uint32_t GetPhiIndex() const noexcept
    {
        return m_phi.GetDescriptorIndex();
    }

    // AIR.4F.2d.4 Stage 2a: phi's writable view, for readers between the solve and the correction (the storage stays writable then).
    std::uint32_t GetPhiUnorderedIndex() const noexcept
    {
        return m_phi.GetUAVDescriptorIndex();
    }

    // AIR.4F.3 F1a.2: phi in the UAV state, for the moving-solid overlay to carry it onto rebuilt pieces before a kept-phi rebuild.
    void PreparePhiWrite(ID3D12GraphicsCommandList* commandList)
    {
        m_phi.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    const std::vector<Level>& GetLevels() const noexcept
    {
        return m_levels;
    }

    std::uint64_t GetStorageBytes() const noexcept;

    // Diagnostics: called between the recording's stages (named), so a diagnostic can insert timestamps; empty in production.
    using StageMarker = std::function<void(ID3D12GraphicsCommandList*, const char*)>;
    void SetStageMarker(StageMarker marker)
    {
        m_stageMarker = std::move(marker);
    }

    bool HasStageMarker() const noexcept
    {
        return static_cast<bool>(m_stageMarker);
    }

    // R1.5b: creates every pipeline now rather than at the first projection, so a packaged game with AIR never creates them mid-game.
    bool PreparePipelines()
    {
        return EnsurePipelines();
    }

private:
    friend class AirFieldVerification;

    // Mirrors AirPressureConstants (AirPressure.hlsl); defined in AirPressure.cpp.
    struct Constants;

    enum class GroupShape
    {
        // One thread per fine slot (8 x 8 x 8).
        FineSlots,
        // One thread per fine cell of one colour (half the x extent).
        FineColour,
        // One thread per cell of `constants.coarse` (4 x 4 x 4).
        CoarseCells,
        // One thread per cell of `constants.level`.
        LevelCells,
        // One thread per closed-table entry (64 per group).
        Entries,
        // One group.
        Single,
        // AIR.4F.3 F1a.3: one thread per cell of the constants' box (4 x 4 x 4).
        BoxCells
    };

    bool EnsurePipelines();
    Constants BaseConstants(const AirFieldBinding& field, std::uint32_t apertureIndex) const;
    // Records `pipeline` with `constants`, then a UAV barrier; false when the constants do not fit the upload ring.
    // AIR.4F.1b: `buoyancy` is required by, and only by, the kernels with buoyancy constants (CSPrepare, CSCorrect).
    // AIR A2/T2c: `transition`, when given, moves to `state` in the same barrier call; without `barrier` the next pass may overlap this one
    // (the caller guarantees it neither reads nor writes what this one writes, and records a barrier before anything that does).
    bool Dispatch(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const ComputePipeline& pipeline, const Constants& constants,
        GroupShape shape, const AirPressureBuoyancy* buoyancy = nullptr, Buffer* transition = nullptr,
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS, bool barrier = true);
    // AIR.4E.5b: the same with the group counts read from m_labelArguments (which must be in its indirect-argument state).
    bool DispatchLabels(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const ComputePipeline& pipeline, const Constants& constants,
        Buffer* transition = nullptr, D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // The UAV barrier after a dispatch (every pass reads what the previous one wrote), with `transition` (when given) moved to `state` in
    // the same call.
    void Barrier(ID3D12GraphicsCommandList* commandList, Buffer* transition, D3D12_RESOURCE_STATES state);
    bool RecordCycle(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const Constants& base, const AirPressureSolveSettings& settings);
    // AIR.4F.3 F1a.3: the hierarchy of every level over `fine` (window-local fine cells, inclusive) and each level's dependency footprint
    // of it: the whole hierarchy when `fine` is the window.
    bool RecordHierarchy(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, Constants constants, std::array<std::int32_t, 3> low,
        std::array<std::int32_t, 3> high);
    // The labelling (components, open or closed, closed volumes), as every rebuild records it.
    bool RecordLabels(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, Constants& constants, std::uint32_t rounds);
    void Begin(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap);
    void Mark(ID3D12GraphicsCommandList* commandList, const char* stage) const;

    ID3D12Device* m_device = nullptr;
    DescriptorHeap* m_descriptorHeap = nullptr;
    ShaderLibrary* m_shaderLibrary = nullptr;
    bool m_pipelinesReady = false;
    std::map<std::string, ComputePipeline> m_pipelines;

    Buffer m_phi;
    Buffer m_rhs;
    Buffer m_scratch;
    Buffer m_component;
    Buffer m_fineMap;
    Buffer m_table;
    Buffer m_hierarchy;
    // AIR.4E.5b: the labelling passes' dispatch arguments (three group counts), and the dispatch signature that reads them.
    Buffer m_labelArguments;
    // AIR.4F.3 F1a.3: the topology check's words (AirPressure.hlsl's Topology* offsets), and the arguments its gates write: the relabel's
    // (fine slots, entries) and the recovery cycles' (fine slots, fine colour, single, then each coarse level).
    Buffer m_topology;
    AirPressureSolveCounts m_solveCounts;
    Buffer m_gateArguments;
    // F1a.3 closeout: a 64-bit predicate (nonzero: relabel) that a local update records its relabel under, so a relabel that is not needed
    // costs no dispatches at all (D3D12 predication skips them on the GPU; barriers and transitions still run).
    Buffer m_relabelPredicate;
    // AIR P1/C1 (AirRelativeScaleGaugeG1.md): each piece's gross right-hand-side magnitude G of the latest CSPrepare, one uint per slot (two
    // floats truncated to their upper 16 bits), the scale of the residual's rounding allowance.
    Buffer m_rhsGross;
    // AIR.4F.3 F1a.3: while set, Dispatch reads every V-cycle dispatch's groups from m_gateArguments (a recovery cycle).
    bool m_recoveryGated = false;
    // The constant blocks this recording pass (Begin to Begin) has uploaded, as raw bytes and their addresses: a dispatch whose constants
    // equal an earlier one's reuses its block. A solve's later and gated V-cycles repeat the first cycle's constants exactly, so without
    // this a solve with recovery cycles filled several hundred KiB of the frame's upload ring.
    std::vector<std::uint8_t> m_uploadedConstantBytes;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> m_uploadedConstantAddresses;
    // AIR.4F.3 F2b.2: the wall source the current solve's CSPrepare reads (none outside RecordSolve).
    AirPressureWallSource m_wallSource;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_dispatchSignature;
    bool m_allocated = false;
    std::array<std::uint32_t, 3> m_dimensions{0, 0, 0};
    std::vector<Level> m_levels;
    bool m_built = false;
    std::uint64_t m_topologyKey = 0;
    // AIR.4E.5b: phi has been written by a rebuild since allocation; the latest rebuild kept it.
    bool m_phiInitialized = false;
    bool m_lastRebuildPreserved = false;

    // Diagnostics: the latest recording's counts per kind.
    AirPressureCounts m_lastRebuild;
    AirPressureCounts m_lastSolve;
    AirPressureCounts m_lastCorrect;
    AirPressureCounts m_recording;
    StageMarker m_stageMarker;
};
