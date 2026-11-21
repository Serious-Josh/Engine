#include "Renderer/AirPressure.h"
#include "Debug/MemoryRegistry.h"

#include "Renderer/AirField.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/UploadBuffer.h"

#include <algorithm>
#include <cstring>
#include <string_view>

struct AirPressure::Constants
{
    AirFieldBinding field;
    std::uint32_t apertureIndex = 0;
    std::uint32_t phiIndex = 0;
    std::uint32_t rhsIndex = 0;
    std::uint32_t scratchIndex = 0;
    std::uint32_t componentIndex = 0;
    std::uint32_t fineMapIndex = 0;
    std::uint32_t tableIndex = 0;
    std::uint32_t hierarchyIndex = 0;
    float ambientVelocity[3] = {0.0f, 0.0f, 0.0f};
    std::uint32_t colour = 0;
    Level level;
    Level coarse;
    std::uint32_t labelRound = 0;
    std::uint32_t componentCapacity = ClosedComponentCapacity;
    std::uint32_t sweeps = 0;
    float correctionScale = 1.0f;
    std::uint32_t velocityWriteIndex = 0;
    std::uint32_t piece1WriteIndex = 0;
    std::uint32_t labelArgumentsIndex = 0;
    std::uint32_t firstSweepFused = 0;
    std::int32_t preservedMinimum[3] = {0, 0, 0};
    std::uint32_t preservePhi = 0;
    std::int32_t boxMinimum[3] = {0, 0, 0};
    std::uint32_t levelCount = 0;
    std::int32_t boxSize[3] = {0, 0, 0};
    std::uint32_t recoveryStep = 0;
    std::uint32_t topologyIndex = 0;
    std::uint32_t gateArgumentsIndex = 0;
    std::uint32_t localHierarchy = 0;
    std::uint32_t predicateIndex = 0;
    // AIR P1/C1: each piece's gross right-hand-side magnitude (CSPrepare writes it, CSResidual reads it).
    std::uint32_t rhsGrossIndex = 0;
    std::uint32_t constantsPadding[3] = {0, 0, 0};
};

namespace
{
// AIR.4F.1b: AirBuoyancyConstants (AirPressure.hlsl): impulse and reference temperature, mirroring AirPressureBuoyancy.
// AIR.4F.3 F2b.2: followed by AirPressureWallSource (the moving-wall source, read by CSPrepare).
constexpr UINT BuoyancyConstantCount = 6;
static_assert(sizeof(AirPressureBuoyancy) + sizeof(AirPressureWallSource) == BuoyancyConstantCount * sizeof(std::uint32_t));

// The kernels that read the buoyancy constants, and so take them as a second root parameter.
bool UsesBuoyancy(const char* kernel)
{
    return std::string_view(kernel) == "CSPrepare" || std::string_view(kernel) == "CSCorrect";
}

// Every kernel of AirPressure.hlsl.
constexpr const char* KernelNames[] = {"CSLabelInit", "CSLabelHook", "CSLabelJump", "CSLabelGround", "CSLabelCompact", "CSLabelFinalize",
    "CSLabelVolume", "CSClearEntries", "CSAggregateFine", "CSAggregateCoarse", "CSCouplingsFine", "CSCouplingsCoarse", "CSDiagonal",
    "CSClearSums", "CSPrepare", "CSClosedSum", "CSCompatible", "CSSmoothFine", "CSRestrictFine", "CSSmoothCoarse", "CSRestrictCoarse", "CSCoarsest",
    "CSProlongCoarse", "CSProlongFine", "CSGaugeScale", "CSGaugeSum", "CSGaugeApply", "CSResidual", "CSZeroPhi", "CSCorrect", "CSLabelGate",
    "CSTopologyCheck", "CSTopologyArguments", "CSRecoveryGate"};

// AIR.4F.3 F1a.3: the topology buffer's size, and the gate arguments' offsets (AirPressure.hlsl).
// AIR A2/T2a: 128 with the relabel-reason counters (words 12 to 18).
constexpr std::uint32_t TopologyBytes = 256;
constexpr std::uint32_t GateFineSlots = 0;
constexpr std::uint32_t GateFineColour = 12;
constexpr std::uint32_t GateSingle = 24;
constexpr std::uint32_t GateLevels = 36;

// Per coarse cell: fp16 couplings (3 axes x 4 x 4), then 4 floats each of diagonal, phi and rhs, a word of present slots and one of map.
constexpr std::uint32_t WeightBytesPerCell = 96;
constexpr std::uint32_t SlotFloatsBytesPerCell = AirPressure::SlotsPerCoarseCell * 4;

std::uint32_t AlignUp(std::uint32_t value)
{
    return (value + 255u) & ~255u;
}

std::uint32_t GroupCount(std::uint32_t threads, std::uint32_t perGroup)
{
    return (threads + perGroup - 1) / perGroup;
}

// AIR.4E.6: the coarse levels' diagnostic stage names, per level (level 1 is m_levels[0]) and operation; deeper levels share the last.
enum class CoarseStage
{
    SmoothingDown,
    Restriction,
    Prolongation,
    SmoothingUp
};

const char* CoarseStageName(std::size_t levelNumber, CoarseStage stage)
{
    static constexpr const char* Names[][4] = {
        {"level 1: smoothing down", "level 1: restriction", "level 1: prolongation", "level 1: smoothing up"},
        {"level 2: smoothing down", "level 2: restriction", "level 2: prolongation", "level 2: smoothing up"},
        {"level 3: smoothing down", "level 3: restriction", "level 3: prolongation", "level 3: smoothing up"},
        {"level 4: smoothing down", "level 4: restriction", "level 4: prolongation", "level 4: smoothing up"},
        {"level 5+: smoothing down", "level 5+: restriction", "level 5+: prolongation", "level 5+: smoothing up"}};
    constexpr std::size_t Rows = sizeof(Names) / sizeof(Names[0]);
    const std::size_t row = (std::min)(levelNumber, Rows) - 1;
    return Names[row][static_cast<std::size_t>(stage)];
}
}

void AirPressure::Initialize(ID3D12Device* device, DescriptorHeap& descriptorHeap, ShaderLibrary& shaderLibrary) noexcept
{
    m_device = device;
    m_descriptorHeap = &descriptorHeap;
    m_shaderLibrary = &shaderLibrary;
}

void AirPressure::Shutdown()
{
    for(Buffer* buffer : StorageBuffers())
    {
        buffer->Shutdown();
    }

    ForgetStorage();

    for(auto& [name, pipeline] : m_pipelines)
    {
        pipeline.Shutdown();
    }

    m_pipelines.clear();
    m_dispatchSignature.Reset();
    m_pipelinesReady = false;
}

void AirPressure::ForgetStorage() noexcept
{
    m_allocated = false;
    m_phiInitialized = false;
    m_built = false;
    m_dimensions = {0, 0, 0};
    m_levels.clear();
}

bool AirPressure::EnsurePipelines()
{
    if(m_pipelinesReady)
    {
        return true;
    }

    if(m_device == nullptr || m_shaderLibrary == nullptr)
    {
        return false;
    }

    const MemoryCategoryScope memoryScope(MemoryCategory::AirVolumetrics);

    for(const char* name : KernelNames)
    {
        ComputePipelineDescription description{};
        description.computeShader = m_shaderLibrary->Load({"AirPressure.hlsl", name, ShaderStage::Compute});
        RootParameterDescription constants{};
        constants.type = RootParameterType::ConstantBuffer;
        constants.shaderRegister = 0;
        constants.visibility = D3D12_SHADER_VISIBILITY_ALL;
        description.rootParameters.push_back(constants);

        // AIR.4F.1b: the buoyancy root constants (AirBuoyancyConstants, b1), for the two kernels that read them.
        if(UsesBuoyancy(name))
        {
            RootParameterDescription buoyancy{};
            buoyancy.type = RootParameterType::Constants;
            buoyancy.shaderRegister = 1;
            buoyancy.constantCount = BuoyancyConstantCount;
            buoyancy.visibility = D3D12_SHADER_VISIBILITY_ALL;
            description.rootParameters.push_back(buoyancy);
        }

        if(description.computeShader == nullptr || !m_pipelines[name].Initialize(m_device, description))
        {
            return false;
        }
    }

    // AIR.4E.5b: indirect dispatches (the labelling rounds) read three group counts.
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature{};
    signature.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    signature.NumArgumentDescs = 1;
    signature.pArgumentDescs = &argument;

    if(FAILED(m_device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&m_dispatchSignature))))
    {
        return false;
    }

    m_pipelinesReady = true;
    return true;
}

bool AirPressure::EnsureStorage(const std::array<std::uint32_t, 3>& dimensions)
{
    if(!EnsurePipelines())
    {
        return false;
    }

    if(m_allocated && dimensions == m_dimensions)
    {
        return true;
    }

    const MemoryCategoryScope memoryScope(MemoryCategory::AirVolumetrics);

    // The owner retires storage before the window's shape changes; a mismatch here is never in flight.
    for(Buffer* buffer : StorageBuffers())
    {
        buffer->Shutdown();
    }

    ForgetStorage();

    // Coarse levels: halved (rounded up) until one fits the coarsest group.
    std::array<std::uint32_t, 3> shape = dimensions;
    std::uint32_t offset = 0;

    do
    {
        Level level;

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            shape[axis] = (shape[axis] + 1) / 2;
            level.dims[axis] = shape[axis];
        }

        level.cells = shape[0] * shape[1] * shape[2];
        level.weights = offset;
        offset = AlignUp(offset + level.cells * WeightBytesPerCell);
        level.diagonal = offset;
        offset = AlignUp(offset + level.cells * SlotFloatsBytesPerCell);
        level.phi = offset;
        offset = AlignUp(offset + level.cells * SlotFloatsBytesPerCell);
        level.rhs = offset;
        offset = AlignUp(offset + level.cells * SlotFloatsBytesPerCell);
        level.info = offset;
        offset = AlignUp(offset + level.cells * 4);
        level.map = offset;
        offset = AlignUp(offset + level.cells * 4);
        level.boundary = offset;
        offset = AlignUp(offset + level.cells * SlotFloatsBytesPerCell);
        m_levels.push_back(level);
    }
    while(m_levels.back().cells > CoarsestCellLimit);

    const std::size_t slots = std::size_t{dimensions[0]} * dimensions[1] * dimensions[2];
    const auto create = [this](Buffer& buffer, std::size_t bytes, bool readable)
    {
        return buffer.InitializeGPUWritable(m_device, bytes) && buffer.CreateRawUAV(m_device, *m_descriptorHeap) &&
            (!readable || buffer.CreateRawSRV(m_device, *m_descriptorHeap));
    };
    // phi, the right-hand side and the residual (scratch) have read-only views for the divergence diagnostics.
    const bool created = create(m_phi, slots * 8, true) && create(m_rhs, slots * 8, true) && create(m_scratch, slots * 8, true) &&
        create(m_component, slots * 8, false) && create(m_fineMap, slots * 4, false) &&
        create(m_table, TableHeaderBytes + std::size_t{ClosedComponentCapacity} * TableEntryBytes, false) && create(m_hierarchy, offset, false) &&
        create(m_labelArguments, sizeof(D3D12_DISPATCH_ARGUMENTS), false) && create(m_topology, TopologyBytes, false) &&
        create(m_gateArguments, GateLevels + m_levels.size() * sizeof(D3D12_DISPATCH_ARGUMENTS), false) && create(m_relabelPredicate, 8, false) &&
        create(m_rhsGross, slots * 4, false);

    if(!created)
    {
        // Never recorded into: released at once.
        for(Buffer* buffer : StorageBuffers())
        {
            buffer->Shutdown();
        }

        ForgetStorage();
        return false;
    }

    m_allocated = true;
    m_dimensions = dimensions;
    return true;
}

std::uint64_t AirPressure::GetStorageBytes() const noexcept
{
    std::uint64_t bytes = 0;

    for(const Buffer* buffer : {&m_phi, &m_rhs, &m_scratch, &m_component, &m_fineMap, &m_table, &m_hierarchy, &m_labelArguments, &m_rhsGross})
    {
        bytes += buffer->GetResource() != nullptr ? buffer->GetResource()->GetDesc().Width : 0;
    }

    return bytes;
}

AirPressure::Constants AirPressure::BaseConstants(const AirFieldBinding& field, std::uint32_t apertureIndex) const
{
    Constants constants;
    constants.field = field;
    constants.apertureIndex = apertureIndex;
    constants.phiIndex = m_phi.GetUAVDescriptorIndex();
    constants.rhsIndex = m_rhs.GetUAVDescriptorIndex();
    constants.scratchIndex = m_scratch.GetUAVDescriptorIndex();
    constants.componentIndex = m_component.GetUAVDescriptorIndex();
    constants.fineMapIndex = m_fineMap.GetUAVDescriptorIndex();
    constants.tableIndex = m_table.GetUAVDescriptorIndex();
    constants.hierarchyIndex = m_hierarchy.GetUAVDescriptorIndex();
    constants.labelArgumentsIndex = m_labelArguments.GetUAVDescriptorIndex();
    constants.topologyIndex = m_topology.GetUAVDescriptorIndex();
    constants.gateArgumentsIndex = m_gateArguments.GetUAVDescriptorIndex();
    constants.predicateIndex = m_relabelPredicate.GetUAVDescriptorIndex();
    constants.rhsGrossIndex = m_rhsGross.GetUAVDescriptorIndex();
    constants.levelCount = static_cast<std::uint32_t>(m_levels.size());
    return constants;
}

void AirPressure::Begin(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap)
{
    ID3D12DescriptorHeap* heaps[]{descriptorHeap.GetHeap()};
    commandList->SetDescriptorHeaps(1, heaps);

    for(Buffer* buffer : StorageBuffers())
    {
        buffer->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    m_recording = {};
    m_uploadedConstantBytes.clear();
    m_uploadedConstantAddresses.clear();
}

void AirPressure::Mark(ID3D12GraphicsCommandList* commandList, const char* stage) const
{
    if(m_stageMarker)
    {
        m_stageMarker(commandList, stage);
    }
}

void AirPressure::Barrier(ID3D12GraphicsCommandList* commandList, Buffer* transition, D3D12_RESOURCE_STATES state)
{
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    const bool moved = transition != nullptr && transition->PrepareTransition(commandList, state, barriers[1]);
    commandList->ResourceBarrier(moved ? 2 : 1, barriers);
    ++m_recording.barriers;
}

bool AirPressure::Dispatch(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const ComputePipeline& pipeline, const Constants& constants,
    GroupShape shape, const AirPressureBuoyancy* buoyancy, Buffer* transition, D3D12_RESOURCE_STATES state, bool barrier)
{
    // Mirrors AirPressureConstants in AirPressure.hlsl.
    static_assert(sizeof(Constants) == 320);
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
    const auto* constantBytes = reinterpret_cast<const std::uint8_t*>(&constants);

    for(std::size_t index = 0; index < m_uploadedConstantAddresses.size() && address == 0; ++index)
    {
        if(std::memcmp(m_uploadedConstantBytes.data() + index * sizeof(Constants), constantBytes, sizeof(Constants)) == 0)
        {
            address = m_uploadedConstantAddresses[index];
        }
    }

    if(address == 0)
    {
        address = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(address == 0)
        {
            return false;
        }

        m_uploadedConstantBytes.insert(m_uploadedConstantBytes.end(), constantBytes, constantBytes + sizeof(Constants));
        m_uploadedConstantAddresses.push_back(address);
    }

    pipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);

    if(buoyancy != nullptr)
    {
        struct
        {
            AirPressureBuoyancy buoyancy;
            AirPressureWallSource wallSource;
        } rootConstants{*buoyancy, m_wallSource};

        commandList->SetComputeRoot32BitConstants(1, BuoyancyConstantCount, &rootConstants, 0);
    }

    // AIR.4F.3 F1a.3: a recovery cycle's dispatches read their groups from the gate's arguments (zero once the solve has converged).
    if(m_recoveryGated)
    {
        std::uint32_t offset = GateSingle;

        if(shape == GroupShape::FineSlots)
        {
            offset = GateFineSlots;
        }
        else if(shape == GroupShape::FineColour)
        {
            offset = GateFineColour;
        }
        else if(shape == GroupShape::LevelCells)
        {
            for(std::size_t index = 0; index < m_levels.size(); ++index)
            {
                if(m_levels[index].weights == constants.level.weights)
                {
                    offset = GateLevels + static_cast<std::uint32_t>(index * sizeof(D3D12_DISPATCH_ARGUMENTS));
                }
            }
        }

        commandList->ExecuteIndirect(m_dispatchSignature.Get(), 1, m_gateArguments.GetResource(), offset, nullptr, 0);
        Barrier(commandList, transition, state);
        ++m_recording.dispatches;
        return true;
    }

    const std::uint32_t* fine = constants.field.dimensions;

    switch(shape)
    {
        case GroupShape::FineSlots:
            commandList->Dispatch(GroupCount(fine[0], 8), GroupCount(fine[1], 8), GroupCount(fine[2], 8));
            break;
        case GroupShape::FineColour:
            commandList->Dispatch(GroupCount(fine[0] / 2, 8), GroupCount(fine[1], 8), GroupCount(fine[2], 8));
            break;
        case GroupShape::CoarseCells:
            commandList->Dispatch(GroupCount(constants.coarse.dims[0], 4), GroupCount(constants.coarse.dims[1], 4),
                GroupCount(constants.coarse.dims[2], 4));
            break;
        case GroupShape::LevelCells:
            commandList->Dispatch(GroupCount(constants.level.dims[0], 4), GroupCount(constants.level.dims[1], 4),
                GroupCount(constants.level.dims[2], 4));
            break;
        case GroupShape::Entries:
            commandList->Dispatch(GroupCount((std::max)(constants.componentCapacity, 1u), 64), 1, 1);
            break;
        case GroupShape::Single:
            commandList->Dispatch(1, 1, 1);
            break;
        case GroupShape::BoxCells:
            commandList->Dispatch(GroupCount(static_cast<std::uint32_t>(constants.boxSize[0]), 4),
                GroupCount(static_cast<std::uint32_t>(constants.boxSize[1]), 4), GroupCount(static_cast<std::uint32_t>(constants.boxSize[2]), 4));
            break;
    }

    if(barrier)
    {
        Barrier(commandList, transition, state);
    }

    ++m_recording.dispatches;
    return true;
}

bool AirPressure::RecordLabels(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, Constants& constants, std::uint32_t rounds)
{
    const auto run = [&](const char* name, GroupShape shape)
    {
        return Dispatch(commandList, upload, m_pipelines.at(name), constants, shape);
    };

    // Connected components: hook and jump until no link joins two labels (a round's flag stays 0), then open or closed per root.
    // AIR.4E.5b: each round's passes read their group counts from m_labelArguments, which CSLabelGate zeroes after the first round that
    // changes nothing; the converged rounds then cost only their (empty) indirect dispatches.
    // AIR A2/T2c: m_labelArguments stays in its indirect-argument state between rounds (it is written only by CSLabelGate), and each
    // of its state changes rides in the UAV barrier after the dispatch before it: one barrier call per dispatch. Under the local update's
    // predication the dispatches are skipped but barriers still execute, so their count is the skipped relabel's cost.
    const std::uint32_t recordedRounds = (std::min)(rounds, FlaggedRounds);
    Buffer* const roundArguments = recordedRounds > 0 ? &m_labelArguments : nullptr;
    const auto labels = [&](const char* name, Buffer* transition, D3D12_RESOURCE_STATES state)
    {
        return DispatchLabels(commandList, upload, m_pipelines.at(name), constants, transition, state);
    };
    bool recorded = run("CSLabelInit", GroupShape::FineSlots) && Dispatch(commandList, upload, m_pipelines.at("CSClearEntries"), constants,
        GroupShape::Entries, nullptr, roundArguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    for(std::uint32_t round = 0; recorded && round < recordedRounds; ++round)
    {
        constants.labelRound = round;
        recorded = labels("CSLabelHook", &m_labelArguments, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
            Dispatch(commandList, upload, m_pipelines.at("CSLabelGate"), constants, GroupShape::Single, nullptr, &m_labelArguments,
                D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

        for(std::uint32_t jump = 0; recorded && jump < JumpsPerRound; ++jump)
        {
            // After the last round's last jump the arguments return to their unordered-access state.
            const bool last = round + 1 == recordedRounds && jump + 1 == JumpsPerRound;
            recorded = labels("CSLabelJump", last ? &m_labelArguments : nullptr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }

    // A recording that stopped early (the upload ring full) still leaves the arguments as the solve expects them.
    m_labelArguments.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    Mark(commandList, "labels: hook and jump");
    recorded = recorded && run("CSLabelGround", GroupShape::FineSlots) && run("CSLabelCompact", GroupShape::FineSlots) &&
        run("CSLabelFinalize", GroupShape::FineSlots) && run("CSLabelVolume", GroupShape::FineSlots);
    Mark(commandList, "labels: open, closed and volumes");
    return recorded;
}

bool AirPressure::RecordHierarchy(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, Constants constants,
    std::array<std::int32_t, 3> low, std::array<std::int32_t, 3> high)
{
    // AIR A2/T2c: whether the latest recorded dispatch still lacks its barrier (a diagonal; below).
    bool overlapping = false;
    const auto run = [&](const char* name, std::array<std::int32_t, 3> minimum, std::array<std::int32_t, 3> maximum, bool barrier)
    {
        // The box, clipped to the level being written (`constants.coarse`).
        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            minimum[axis] = (std::max)(minimum[axis], 0);
            maximum[axis] = (std::min)(maximum[axis], static_cast<std::int32_t>(constants.coarse.dims[axis]) - 1);
            constants.boxMinimum[axis] = minimum[axis];
            constants.boxSize[axis] = (std::max)(maximum[axis] - minimum[axis] + 1, 0);
        }

        if(constants.boxSize[0] == 0 || constants.boxSize[1] == 0 || constants.boxSize[2] == 0)
        {
            return true;
        }

        overlapping = !barrier;
        return Dispatch(commandList, upload, m_pipelines.at(name), constants, GroupShape::BoxCells, nullptr, nullptr,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, barrier);
    };

    // Per level, from the changed cells of the level below (`low`..`high`, inclusive):
    //   aggregates: the parents of those cells (an aggregate reads its block's children and the links inside the block);
    //   couplings: those parents and their -x, -y, -z neighbours (a cell's couplings read its block and the next block up each axis);
    //   diagonals: the parents grown by one each way (couplings from both sides, and couplings dropped across neighbouring blocks).
    // The next level's changed cells are this level's couplings.
    // AIR A2/T2c: a level's diagonals are read only by the solve: not by the next level's aggregation, couplings or diagonals (which read
    // this level's present slots, couplings and window-face weights, and the dropped couplings through the level below's map), and they
    // write only the level's diagonal region. So a diagonal pass records no barrier of its own and overlaps the next level's
    // aggregation; the aggregation's barrier (or the one after the last level) orders it before everything later.
    bool recorded = true;

    for(std::size_t index = 0; recorded && index < m_levels.size(); ++index)
    {
        const bool fine = index == 0;
        constants.coarse = m_levels[index];
        constants.colour = fine ? 0u : 1u;

        if(!fine)
        {
            constants.level = m_levels[index - 1];
        }

        std::array<std::int32_t, 3> aggregateLow{};
        std::array<std::int32_t, 3> aggregateHigh{};
        std::array<std::int32_t, 3> couplingLow{};
        std::array<std::int32_t, 3> diagonalLow{};
        std::array<std::int32_t, 3> diagonalHigh{};

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            aggregateLow[axis] = low[axis] >> 1;
            aggregateHigh[axis] = high[axis] >> 1;
            couplingLow[axis] = aggregateLow[axis] - 1;
            diagonalLow[axis] = aggregateLow[axis] - 1;
            diagonalHigh[axis] = aggregateHigh[axis] + 1;
        }

        recorded = run(fine ? "CSAggregateFine" : "CSAggregateCoarse", aggregateLow, aggregateHigh, true) &&
            run(fine ? "CSCouplingsFine" : "CSCouplingsCoarse", couplingLow, aggregateHigh, true) &&
            run("CSDiagonal", diagonalLow, diagonalHigh, false);
        Mark(commandList, fine ? "hierarchy: level 1" : "hierarchy: coarser levels");

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            low[axis] = (std::max)(couplingLow[axis], 0);
            high[axis] = (std::min)(aggregateHigh[axis], static_cast<std::int32_t>(m_levels[index].dims[axis]) - 1);
        }
    }

    if(overlapping)
    {
        Barrier(commandList, nullptr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    return recorded;
}

bool AirPressure::DispatchLabels(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const ComputePipeline& pipeline,
    const Constants& constants, Buffer* transition, D3D12_RESOURCE_STATES state)
{
    const D3D12_GPU_VIRTUAL_ADDRESS address = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(address == 0)
    {
        return false;
    }

    pipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    commandList->ExecuteIndirect(m_dispatchSignature.Get(), 1, m_labelArguments.GetResource(), 0, nullptr, 0);
    Barrier(commandList, transition, state);
    ++m_recording.dispatches;
    return true;
}

bool AirPressure::RecordRebuild(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, UploadBuffer& upload,
    const AirFieldBinding& field, std::uint32_t apertureIndex, std::uint64_t topologyKey, std::uint32_t rounds,
    const std::array<std::int32_t, 3>* preservedMinimum)
{
    const std::array<std::uint32_t, 3> dimensions{field.dimensions[0], field.dimensions[1], field.dimensions[2]};

    if(field.valid == 0 || apertureIndex == UINT32_MAX || !EnsureStorage(dimensions))
    {
        return false;
    }

    // Storage just allocated holds no phi to keep.
    const bool preserve = preservedMinimum != nullptr && m_allocated && m_phiInitialized;
    m_built = false;
    Begin(commandList, descriptorHeap);
    Constants constants = BaseConstants(field, apertureIndex);
    constants.preservePhi = preserve ? 1u : 0u;

    if(preserve)
    {
        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            constants.preservedMinimum[axis] = (*preservedMinimum)[axis];
        }
    }

    m_lastRebuildPreserved = preserve;
    m_phiInitialized = true;
    const auto run = [&](const char* name, GroupShape shape)
    {
        return Dispatch(commandList, upload, m_pipelines.at(name), constants, shape);
    };

    bool recorded = RecordLabels(commandList, upload, constants, rounds);

    // The hierarchy, level by level: aggregates, Galerkin couplings, then diagonals (which need the neighbours' couplings), over every
    // cell of every level (the window's footprint covers each level whole).
    const std::array<std::int32_t, 3> windowHigh{static_cast<std::int32_t>(dimensions[0]) - 1, static_cast<std::int32_t>(dimensions[1]) - 1,
        static_cast<std::int32_t>(dimensions[2]) - 1};
    recorded = recorded && RecordHierarchy(commandList, upload, constants, {0, 0, 0}, windowHigh);

    m_lastRebuild = m_recording;

    if(!recorded)
    {
        return false;
    }

    m_built = true;
    m_topologyKey = topologyKey;
    return true;
}

bool AirPressure::RecordIncrementalRebuild(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, UploadBuffer& upload,
    const AirFieldBinding& field, std::uint32_t apertureIndex, std::uint64_t topologyKey, const AirPressureLocalUpdate& update)
{
    const std::array<std::uint32_t, 3> dimensions{field.dimensions[0], field.dimensions[1], field.dimensions[2]};

    if(field.valid == 0 || apertureIndex == UINT32_MAX || !m_allocated || !m_built || dimensions != m_dimensions)
    {
        return false;
    }

    m_built = false;
    Begin(commandList, descriptorHeap);
    Constants constants = BaseConstants(field, apertureIndex);
    // Phi is kept in every cell (the window has not moved); a relabel's CSLabelInit then leaves it as it is, and keeps the overflow total
    // the local aggregations maintain.
    constants.preservePhi = 1;
    constants.localHierarchy = 1;

    for(std::size_t axis = 0; axis < 3; ++axis)
    {
        constants.preservedMinimum[axis] = field.minimum[axis];
    }

    const auto run = [&](const char* name, GroupShape shape)
    {
        return Dispatch(commandList, upload, m_pipelines.at(name), constants, shape);
    };
    // The topology check over the update's box, then the relabel's arguments (zero when the components cannot have changed).
    for(std::size_t axis = 0; axis < 3; ++axis)
    {
        constants.boxMinimum[axis] = update.minimum[axis];
        constants.boxSize[axis] = update.maximum[axis] - update.minimum[axis] + 1;
    }

    constants.colour = update.forceRelabel ? 1u : 0u;
    bool recorded = run("CSTopologyCheck", GroupShape::Single) && run("CSTopologyArguments", GroupShape::Single);
    Mark(commandList, "topology check");
    constants.colour = 0;

    // F1a.3 closeout: the relabel, exactly as RecordRebuild records it, predicated on the check's result: when no relabel is needed the
    // GPU skips every one of its dispatches (D3D12 predication; the CPU never learns the outcome). The predicate is written above, then
    // made readable for predication for the relabel's span only.
    m_relabelPredicate.Transition(commandList, D3D12_RESOURCE_STATE_PREDICATION);
    commandList->SetPredication(m_relabelPredicate.GetResource(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    recorded = recorded && RecordLabels(commandList, upload, constants, FlaggedRounds);
    commandList->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    m_relabelPredicate.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // The hierarchy over each level's footprint of the box (codes change only inside it: a piece becomes isolated or solvable only where
    // its own links changed).
    recorded = recorded && RecordHierarchy(commandList, upload, constants, update.minimum, update.maximum);
    m_lastRebuild = m_recording;

    if(!recorded)
    {
        return false;
    }

    m_lastRebuildPreserved = true;
    m_built = true;
    m_topologyKey = topologyKey;
    return true;
}

bool AirPressure::RecordCycle(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const Constants& base,
    const AirPressureSolveSettings& settings)
{
    Constants constants = base;
    const auto run = [&](const char* name, GroupShape shape)
    {
        return Dispatch(commandList, upload, m_pipelines.at(name), constants, shape);
    };
    // Symmetric smoothing: red then black before the coarse correction, black then red after. AIR.4E.6: `firstFused`: the restriction
    // into this level already did its first (red) half-sweep (firstSweepFused).
    const auto smooth = [&](const char* name, GroupShape shape, bool before, bool firstFused)
    {
        bool smoothed = true;

        for(std::uint32_t sweep = 0; smoothed && sweep < settings.smoothingSweeps; ++sweep)
        {
            if(sweep > 0 || !firstFused)
            {
                constants.colour = before ? 0 : 1;
                smoothed = run(name, shape);
            }

            constants.colour = before ? 1 : 0;
            smoothed = smoothed && run(name, shape);
        }

        return smoothed;
    };
    const std::size_t coarsest = m_levels.size() - 1;
    // AIR.4E.6: a restriction into a smoothed level (above the coarsest, which CSCoarsest solves from phi = 0 itself) also performs
    // that level's first pre-smoothing half-sweep.
    const bool fuseFirstSweep = settings.smoothingSweeps > 0;
    const auto fusedInto = [&](std::size_t coarseIndex)
    {
        return fuseFirstSweep && coarseIndex < coarsest;
    };

    // Down: the fine level, then every coarse level above the coarsest.
    constants.coarse = m_levels.front();
    bool recorded = smooth("CSSmoothFine", GroupShape::FineColour, true, false);
    Mark(commandList, "fine smoothing");
    // AIR.4E.6: the restrictions run one thread per child (AirPressure.hlsl).
    constants.firstSweepFused = fusedInto(0) ? 1u : 0u;
    recorded = recorded && run("CSRestrictFine", GroupShape::FineSlots);
    Mark(commandList, "fine residual and restriction");

    for(std::size_t index = 0; recorded && index < coarsest; ++index)
    {
        constants.level = m_levels[index];
        constants.coarse = m_levels[index + 1];
        recorded = smooth("CSSmoothCoarse", GroupShape::LevelCells, true, fusedInto(index));
        Mark(commandList, CoarseStageName(index + 1, CoarseStage::SmoothingDown));
        constants.firstSweepFused = fusedInto(index + 1) ? 1u : 0u;
        recorded = recorded && run("CSRestrictCoarse", GroupShape::LevelCells);
        Mark(commandList, CoarseStageName(index + 1, CoarseStage::Restriction));
    }

    constants.firstSweepFused = 0;
    constants.level = m_levels[coarsest];
    constants.sweeps = settings.coarsestSweeps;
    recorded = recorded && run("CSCoarsest", GroupShape::Single);
    Mark(commandList, "coarsest solve");

    // Up: prolong each correction (scaled) and smooth.
    constants.correctionScale = settings.correctionScale;

    for(std::size_t index = coarsest; recorded && index > 0; --index)
    {
        constants.level = m_levels[index - 1];
        constants.coarse = m_levels[index];
        recorded = run("CSProlongCoarse", GroupShape::LevelCells);
        Mark(commandList, CoarseStageName(index, CoarseStage::Prolongation));
        recorded = recorded && smooth("CSSmoothCoarse", GroupShape::LevelCells, false, false);
        Mark(commandList, CoarseStageName(index, CoarseStage::SmoothingUp));
    }

    constants.coarse = m_levels.front();
    recorded = recorded && run("CSProlongFine", GroupShape::FineSlots);
    Mark(commandList, "fine prolongation");
    recorded = recorded && smooth("CSSmoothFine", GroupShape::FineColour, false, false);
    Mark(commandList, "fine smoothing");
    return recorded;
}

bool AirPressure::RecordSolve(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, UploadBuffer& upload,
    const AirFieldBinding& field, std::uint32_t apertureIndex, const Vector3& ambientVelocity, const AirPressureSolveSettings& settings,
    const AirPressureBuoyancy& buoyancy, const AirPressureWallSource& wallSource)
{
    if(!m_built || field.valid == 0 || apertureIndex == UINT32_MAX || m_levels.empty())
    {
        return false;
    }

    ++m_solveCounts.solves;
    m_solveCounts.fixedCycles += settings.cycles;
    m_solveCounts.recoveryBudget += settings.recoveryCycles;
    Begin(commandList, descriptorHeap);
    Constants constants = BaseConstants(field, apertureIndex);
    constants.ambientVelocity[0] = ambientVelocity.x;
    constants.ambientVelocity[1] = ambientVelocity.y;
    constants.ambientVelocity[2] = ambientVelocity.z;
    const auto run = [&](const char* name, GroupShape shape)
    {
        return Dispatch(commandList, upload, m_pipelines.at(name), constants, shape);
    };

    // The right-hand side (divergence fused in), then closed components made compatible. F2b.2: the wall source is bound for CSPrepare's
    // root constants only, then cleared, so CSCorrect (the other reader of those constants) never sees it.
    bool recorded = run("CSClearSums", GroupShape::Entries);
    m_wallSource = wallSource;
    recorded = recorded && Dispatch(commandList, upload, m_pipelines.at("CSPrepare"), constants, GroupShape::FineSlots, &buoyancy);
    m_wallSource = {};
    Mark(commandList, "right-hand side (fused divergence)");
    // AIR A1: the closed components' sums at a scale relative to the largest |b|, which CSPrepare has finished.
    recorded = recorded && run("CSClosedSum", GroupShape::FineSlots) && run("CSCompatible", GroupShape::FineSlots);
    Mark(commandList, "closed compatibility");

    if(recorded && !settings.warmStart)
    {
        recorded = run("CSZeroPhi", GroupShape::FineSlots);
    }

    for(std::uint32_t cycle = 0; recorded && cycle < settings.cycles; ++cycle)
    {
        recorded = RecordCycle(commandList, upload, constants, settings);
    }

    // AIR.4F.3 F1a.3: bounded recovery. The residual after the budgeted cycles; then each gate enables one more cycle (and its residual)
    // only while the residual is above the criterion; the last gate only counts a solve that ran out of budget. All on the GPU: once
    // converged, the remaining dispatches launch no groups.
    if(recorded && settings.recoveryCycles > 0)
    {
        recorded = run("CSResidual", GroupShape::FineSlots);
        Constants gate = constants;
        gate.sweeps = settings.recoveryCycles;

        for(std::uint32_t step = 0; recorded && step <= settings.recoveryCycles; ++step)
        {
            gate.recoveryStep = step;
            recorded = Dispatch(commandList, upload, m_pipelines.at("CSRecoveryGate"), gate, GroupShape::Single);

            if(!recorded || step == settings.recoveryCycles)
            {
                break;
            }

            m_gateArguments.Transition(commandList, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            m_recoveryGated = true;
            recorded = RecordCycle(commandList, upload, constants, settings) && run("CSResidual", GroupShape::FineSlots);
            m_recoveryGated = false;
            m_gateArguments.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        Mark(commandList, "recovery");
    }

    // The closed components' gauge: zero volume-weighted mean. AIR P1/G1: each component's sum at a scale relative to its own largest |V phi|
    // (CSGaugeScale), so a constant offset is removed at any magnitude (AirRelativeScaleGaugeG1.md).
    recorded = recorded && run("CSGaugeScale", GroupShape::FineSlots) && run("CSGaugeSum", GroupShape::FineSlots) &&
        run("CSGaugeApply", GroupShape::FineSlots);
    Mark(commandList, "closed gauge");
    m_lastSolve = m_recording;
    return recorded;
}

bool AirPressure::RecordResidual(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, UploadBuffer& upload,
    const AirFieldBinding& field, std::uint32_t apertureIndex)
{
    if(!m_built || field.valid == 0 || apertureIndex == UINT32_MAX)
    {
        return false;
    }

    Begin(commandList, descriptorHeap);
    Constants constants = BaseConstants(field, apertureIndex);
    // No entries: CSClearSums only resets the header's maxima.
    constants.componentCapacity = 0;
    bool recorded = Dispatch(commandList, upload, m_pipelines.at("CSClearSums"), constants, GroupShape::Single);
    constants.componentCapacity = ClosedComponentCapacity;
    return recorded && Dispatch(commandList, upload, m_pipelines.at("CSResidual"), constants, GroupShape::FineSlots);
}

bool AirPressure::RecordCorrect(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, UploadBuffer& upload,
    const AirFieldBinding& field, std::uint32_t apertureIndex, std::uint32_t velocityWriteIndex, std::uint32_t piece1WriteIndex,
    const AirPressureBuoyancy& buoyancy)
{
    if(!m_built || field.valid == 0 || apertureIndex == UINT32_MAX)
    {
        return false;
    }

    Begin(commandList, descriptorHeap);
    Constants constants = BaseConstants(field, apertureIndex);
    constants.velocityWriteIndex = velocityWriteIndex;
    constants.piece1WriteIndex = piece1WriteIndex;
    const bool recorded = Dispatch(commandList, upload, m_pipelines.at("CSCorrect"), constants, GroupShape::FineSlots, &buoyancy);
    Mark(commandList, "velocity correction and wall constraint");
    m_lastCorrect = m_recording;
    return recorded;
}

AirPressureDiagnostics AirPressure::PublishDiagnostics(ID3D12GraphicsCommandList* commandList)
{
    AirPressureDiagnostics diagnostics;

    if(!m_built)
    {
        return diagnostics;
    }

    // Read by the divergence view this frame; the next recording makes them writable again (Begin).
    m_rhs.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_scratch.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    diagnostics.valid = 1;
    diagnostics.rhsIndex = m_rhs.GetDescriptorIndex();
    diagnostics.residualIndex = m_scratch.GetDescriptorIndex();
    return diagnostics;
}
