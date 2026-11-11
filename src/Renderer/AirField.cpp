#include "Renderer/AirField.h"

#include "Renderer/DescriptorHeap.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/UploadBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
// Must match the AirFieldConstants cbuffer of AirField.hlsl.
struct AirFieldConstants
{
    VoxelWorldBinding voxels;
    AirFieldBinding field;
    std::int32_t previousMinimum[3] = {0, 0, 0};
    std::uint32_t fullReset = 0;
    float ambientVelocity[3] = {0.0f, 0.0f, 0.0f};
    std::uint32_t rebuildObstacles = 0;
    std::int32_t voxelLevel = 0;
    std::uint32_t velocityUnordered[2] = {0, 0};
    std::uint32_t obstacleUnordered = 0;
    std::uint32_t pieceUnordered = 0;
    std::uint32_t piece1Unordered[2] = {0, 0};
    std::uint32_t currentVelocity = 0;
    // AIR.4F: the mist pairs, the current buffer of each, and whether mist is allocated.
    std::uint32_t mistUnordered[2] = {0, 0};
    std::uint32_t mistPiece1Unordered[2] = {0, 0};
    std::uint32_t currentMist = 0;
    std::uint32_t mistEnabled = 0;
    std::uint32_t mistPadding[2] = {0, 0};
};

static_assert(sizeof(AirFieldConstants) == 288);

// Must match the AirMistConstants cbuffer of AirMist.hlsl.
struct AirMistConstants
{
    AirFieldBinding field;
    std::uint32_t obstacleIndex = 0;
    std::uint32_t pieceIndex = 0;
    std::uint32_t velocityIndex = 0;
    std::uint32_t velocityPiece1Index = 0;
    std::uint32_t mistRead[2] = {0, 0};
    std::uint32_t mistWrite[2] = {0, 0};
    float timeStep = 0.0f;
    float dissipationFactor = 1.0f;
    std::uint32_t splitListIndex = 0;
    std::uint32_t splitArgumentsIndex = 0;
    std::uint32_t clear[4] = {0, 0, 0, 0};
    // AIR.4F F2 emission.
    std::uint32_t emitterIndex = 0;
    std::uint32_t emitterCount = 0;
    std::uint32_t countPieces = 0;
    std::uint32_t statusIndex = 0;
    std::int32_t emitBoxMinimum[3] = {0, 0, 0};
    std::uint32_t emitPadding0 = 0;
    std::uint32_t emitBoxSize[3] = {0, 0, 0};
    std::uint32_t emitPadding1 = 0;
};

static_assert(sizeof(AirMistConstants) == 176);
static_assert(sizeof(GpuMistEmitter) == 96);
// AIR.4F F2: one uint4 per emitter (origin state, receiving pieces, unused, unused).
constexpr std::size_t MistStatusBytesPerEmitter = 16;

// Must match the AirSolverConstants cbuffer of AirSolver.hlsl.
struct AirSolverConstants
{
    AirFieldBinding field;
    float ambientVelocity[3] = {0.0f, 0.0f, 0.0f};
    float timeStep = 0.0f;
    std::uint32_t velocityReadIndex = 0;
    std::uint32_t velocityWriteIndex = 0;
    std::uint32_t obstacleIndex = 0;
    float dampingFactor = 1.0f;
    std::uint32_t motorCount = 0;
    float maximumMotorSpeed = 0.0f;
    std::uint32_t piece1ReadIndex = 0;
    std::uint32_t piece1WriteIndex = 0;
    std::uint32_t pieceIndex = 0;
    std::uint32_t visibilityIndex = 0;
    // Keeps the int3 below from straddling a 16-byte register, as HLSL packing requires.
    std::uint32_t visibilityPadding[2] = {0, 0};
    // CSMotorVisibility and CSEmissionStatus only.
    std::int32_t visibilityBoxMinimum[3] = {0, 0, 0};
    std::uint32_t visibilityMotor = 0;
    std::uint32_t visibilityBoxWords[3] = {0, 0, 0};
    std::uint32_t statusIndex = 0;
    // The two-pass step's work list and its counter and dispatch arguments.
    std::uint32_t splitListIndex = 0;
    std::uint32_t splitArgumentsIndex = 0;
    // AIR.4D D2.1: the shaped emitters' lattice states (CSEmissionSamples).
    std::uint32_t emissionSamplesIndex = 0;
    std::uint32_t constantsPadding = 0;
};

static_assert(sizeof(AirSolverConstants) == 176);

// The two-pass step's argument buffer: the counter, then the split pass's dispatch (D3D12_DISPATCH_ARGUMENTS).
constexpr std::size_t SplitArgumentBytes = 16;
constexpr UINT64 SplitDispatchOffset = 4;

static_assert(sizeof(GpuAirMotor) == 128);

constexpr std::uint32_t NoVisibility = 0xFFFFFFFFu;
// One uint4 per motor: origin state, points, blocked, unknown (CSEmissionStatus).
constexpr std::size_t StatusBytesPerMotor = 16;
// AIR.4D D2.1: 2 bits per lattice sample of a shaped emitter, at most 128 samples: 8 words per visibility slot (AirEmitter.hlsli).
constexpr std::size_t SampleBytesPerMotor = 8 * sizeof(std::uint32_t);

// AIR.4D D2.1: whether a motor emits from a shape (an Omni ellipsoid or a Vortex ring), whose lattice CSEmissionSamples computes.
bool IsShapedEmitter(const GpuAirMotor& motor)
{
    const bool omniSurface = motor.type == static_cast<std::uint32_t>(AirMotorType::Omni) &&
        (motor.semiAxes[0] > 0.0f || motor.semiAxes[1] > 0.0f || motor.semiAxes[2] > 0.0f);
    return omniSurface || motor.type == static_cast<std::uint32_t>(AirMotorType::Vortex);
}

// What decides a motor's visibility, bit for bit: its emission geometry (position, type, axes, bounding radius, size and emission
// semi-axes), not its strength, falloff or the air it pushes.
std::array<std::uint32_t, AirVisibilityKeyWords> VisibilityKey(const GpuAirMotor& motor)
{
    const float values[AirVisibilityKeyWords - 1] = {motor.position[0], motor.position[1], motor.position[2], motor.axisX[0], motor.axisX[1],
        motor.axisX[2], motor.axisY[0], motor.axisY[1], motor.axisY[2], motor.axisZ[0], motor.axisZ[1], motor.axisZ[2],
        motor.boundingRadius, motor.size[0], motor.size[1], motor.size[2], motor.semiAxes[0], motor.semiAxes[1], motor.semiAxes[2]};
    std::array<std::uint32_t, AirVisibilityKeyWords> key{};
    std::memcpy(key.data(), values, sizeof(values));
    key[AirVisibilityKeyWords - 1] = motor.type;
    return key;
}

// The cells holding the motor's bounding sphere, inclusive, unclamped.
struct CellBox
{
    std::array<std::int64_t, 3> low{};
    std::array<std::int64_t, 3> high{};
};

CellBox BoundingCells(const GpuAirMotor& motor, double cellSize)
{
    CellBox box;

    for(std::size_t axis = 0; axis < 3; ++axis)
    {
        box.low[axis] = static_cast<std::int64_t>(std::floor((static_cast<double>(motor.position[axis]) - motor.boundingRadius) / cellSize));
        box.high[axis] = static_cast<std::int64_t>(std::floor((static_cast<double>(motor.position[axis]) + motor.boundingRadius) / cellSize));
    }

    return box;
}

bool BoxInsideWindow(const CellBox& box, const std::array<std::int32_t, 3>& minimum, const std::array<std::uint32_t, 3>& dimensions)
{
    for(std::size_t axis = 0; axis < 3; ++axis)
    {
        if(box.low[axis] < minimum[axis] || box.high[axis] >= std::int64_t{minimum[axis]} + dimensions[axis])
        {
            return false;
        }
    }

    return true;
}

// The radius of a sphere around the motor's position that holds its whole region. A shaped emitter's region reaches the influence
// distance beyond its largest semi-axis.
float BoundingRadius(const AirMotorInstance& motor)
{
    const Vector3& semiAxes = motor.emissionSemiAxes;

    switch(motor.type)
    {
    case AirMotorType::Directional:
        return std::sqrt(motor.halfExtents.x * motor.halfExtents.x + motor.halfExtents.y * motor.halfExtents.y +
            motor.halfExtents.z * motor.halfExtents.z);
    case AirMotorType::Cylinder:
        return std::sqrt(motor.radius * motor.radius + motor.halfLength * motor.halfLength);
    case AirMotorType::Vortex:
        return (std::max)(semiAxes.x, semiAxes.y) + motor.radius;
    case AirMotorType::Omni:
        return (std::max)({semiAxes.x, semiAxes.y, semiAxes.z}) + motor.radius;
    case AirMotorType::Moving:
        break;
    }

    return motor.radius;
}

void CopyVector(const Vector3& source, float (&destination)[3])
{
    destination[0] = source.x;
    destination[1] = source.y;
    destination[2] = source.z;
}

constexpr std::uint32_t SlotsPerObstacleWord = 16;
constexpr std::size_t VelocityBytesPerCell = 16;
// AIR.4D: piece 0 and piece 1, 64 bits each.
constexpr std::size_t PieceBytesPerCell = 16;
// AIR.4F: one premultiplied float4 per piece and cell.
constexpr std::size_t MistBytesPerCell = 16;
// Where consumers read the field after the frame's air work.
constexpr D3D12_RESOURCE_STATES ReadState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

std::int32_t FloorDivide(std::int64_t value, std::int64_t divisor)
{
    const std::int64_t quotient = value / divisor;
    return static_cast<std::int32_t>((value % divisor != 0 && value < 0) ? quotient - 1 : quotient);
}

std::size_t CellCount(const AirFieldSettings& settings)
{
    return std::size_t{settings.dimensions[0]} * settings.dimensions[1] * settings.dimensions[2];
}

void UnorderedAccessBarrier(ID3D12GraphicsCommandList* commandList)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = nullptr;
    commandList->ResourceBarrier(1, &barrier);
}

bool MakePipeline(ID3D12Device* device, ShaderLibrary& shaderLibrary, const char* file, const char* entryPoint, ComputePipeline& pipeline,
    bool motorBuffer = false)
{
    const Shader* shader = shaderLibrary.Load({file, entryPoint, ShaderStage::Compute});

    if(!shader)
    {
        return false;
    }

    ComputePipelineDescription description{};
    description.computeShader = shader;
    RootParameterDescription constants{};
    constants.type = RootParameterType::ConstantBuffer;
    constants.shaderRegister = 0;
    constants.visibility = D3D12_SHADER_VISIBILITY_ALL;
    description.rootParameters.push_back(constants);

    // The solver's motors: a root SRV straight into the frame's upload ring.
    if(motorBuffer)
    {
        RootParameterDescription motors{};
        motors.type = RootParameterType::ShaderResource;
        motors.shaderRegister = 0;
        motors.visibility = D3D12_SHADER_VISIBILITY_ALL;
        description.rootParameters.push_back(motors);
    }

    return pipeline.Initialize(device, description);
}

void DispatchCells(ID3D12GraphicsCommandList* commandList, const AirFieldSettings& settings)
{
    commandList->Dispatch((settings.dimensions[0] + 7) / 8, (settings.dimensions[1] + 7) / 8, (settings.dimensions[2] + 7) / 8);
}
}

bool AirField::ValidateSettings(const AirFieldSettings& settings, std::string* error)
{
    const auto& size = settings.dimensions;
    const bool shape = size[0] >= 16 && size[1] >= 8 && size[2] >= 8 && size[0] % 16 == 0 && size[1] % 8 == 0 && size[2] % 8 == 0 &&
        size[0] <= 1024 && size[1] <= 1024 && size[2] <= 1024;
    const bool cell = settings.cellExponent >= -7 && settings.cellExponent <= 4;
    const bool ambient = std::isfinite(settings.ambientVelocity.x) && std::isfinite(settings.ambientVelocity.y) &&
        std::isfinite(settings.ambientVelocity.z);

    if(!shape || !cell || !ambient)
    {
        if(error)
        {
            *error = "Air field dimensions must be multiples of 16 (x) and 8 (y, z), at most 1024, with a cell of 2^-7 to 2^4 m and a "
                "finite ambient velocity.";
        }

        return false;
    }

    return true;
}

bool AirField::Initialize(ID3D12Device* device, DescriptorHeap& descriptorHeap, ShaderLibrary& shaderLibrary)
{
    m_device = device;
    m_descriptorHeap = &descriptorHeap;
    const bool pipelines = MakePipeline(device, shaderLibrary, "AirField.hlsl", "CSInitializeVelocity", m_initializePipeline) &&
        MakePipeline(device, shaderLibrary, "AirField.hlsl", "CSBuildObstacles", m_obstaclePipeline) &&
        MakePipeline(device, shaderLibrary, "AirField.hlsl", "CSBuildPieces", m_piecePipeline) &&
        MakePipeline(device, shaderLibrary, "AirSolver.hlsl", "CSStepOpen", m_stepPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirSolver.hlsl", "CSStepSplit", m_splitStepPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirSolver.hlsl", "CSPrepareSplit", m_prepareSplitPipeline) &&
        MakePipeline(device, shaderLibrary, "AirSolver.hlsl", "CSResetSplit", m_resetSplitPipeline) &&
        MakePipeline(device, shaderLibrary, "AirSolver.hlsl", "CSMotorVisibility", m_visibilityPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirSolver.hlsl", "CSEmissionStatus", m_statusPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirSolver.hlsl", "CSEmissionSamples", m_samplesPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMist.hlsl", "CSMistOpen", m_mistOpenPipeline) &&
        MakePipeline(device, shaderLibrary, "AirMist.hlsl", "CSMistSplit", m_mistSplitPipeline) &&
        MakePipeline(device, shaderLibrary, "AirMist.hlsl", "CSMistPrepare", m_mistPreparePipeline) &&
        MakePipeline(device, shaderLibrary, "AirMist.hlsl", "CSMistClear", m_mistClearPipeline) &&
        MakePipeline(device, shaderLibrary, "AirMist.hlsl", "CSMistEmit", m_mistEmitPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMist.hlsl", "CSMistEmitterStatus", m_mistEmitterStatusPipeline, true);

    if(!pipelines)
    {
        return false;
    }

    // The split pass is dispatched from the GPU's own count of listed cells.
    D3D12_INDIRECT_ARGUMENT_DESC dispatch{};
    dispatch.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature{};
    signature.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    signature.NumArgumentDescs = 1;
    signature.pArgumentDescs = &dispatch;

    if(FAILED(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&m_splitSignature))))
    {
        return false;
    }

    // AIR.4D D2: the emission status copies, one per frame slot, read once their frame completes.
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = MaximumMotors * StatusBytesPerMotor;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for(StatusSlot& slot : m_statusSlots)
    {
        if(FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&slot.readback))))
        {
            return false;
        }
    }

    // AIR.4F F2: the Mist Emitters' status copies, the same way.
    description.Width = MaximumMistEmitters * MistStatusBytesPerEmitter;

    for(StatusSlot& slot : m_mistStatusSlots)
    {
        if(FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&slot.readback))))
        {
            return false;
        }
    }

    return true;
}

void AirField::Shutdown()
{
    // Buffer has no destructor: each is shut down explicitly to release its resource and descriptors.
    for(RetiredBuffer& retired : m_retired)
    {
        retired.buffer.Shutdown();
    }

    m_retired.clear();

    for(Buffer* buffer : StorageBuffers())
    {
        buffer->Shutdown();
    }

    for(Buffer* buffer : MistBuffers())
    {
        buffer->Shutdown();
    }

    m_mistAllocated = false;
    m_mistBinding = {};

    for(StatusSlot& slot : m_statusSlots)
    {
        slot = {};
    }

    for(StatusSlot& slot : m_mistStatusSlots)
    {
        slot = {};
    }

    m_mistEmitterStatus.clear();
    m_mistEmitPipeline.Shutdown();
    m_mistEmitterStatusPipeline.Shutdown();
    m_allocated = false;
    m_binding = {};
    m_emission.clear();
    m_initializePipeline.Shutdown();
    m_obstaclePipeline.Shutdown();
    m_piecePipeline.Shutdown();
    m_stepPipeline.Shutdown();
    m_splitStepPipeline.Shutdown();
    m_prepareSplitPipeline.Shutdown();
    m_resetSplitPipeline.Shutdown();
    m_splitSignature.Reset();
    m_visibilityPipeline.Shutdown();
    m_statusPipeline.Shutdown();
    m_samplesPipeline.Shutdown();
    m_mistOpenPipeline.Shutdown();
    m_mistSplitPipeline.Shutdown();
    m_mistPreparePipeline.Shutdown();
    m_mistClearPipeline.Shutdown();
}

bool AirField::Allocate(const AirFieldSettings& settings)
{
    const std::size_t cells = CellCount(settings);
    const auto create = [this](Buffer& buffer, std::size_t bytes)
    {
        return buffer.InitializeGPUWritable(m_device, bytes) && buffer.CreateRawSRV(m_device, *m_descriptorHeap) &&
            buffer.CreateRawUAV(m_device, *m_descriptorHeap);
    };

    const bool created = create(m_velocity[0], cells * VelocityBytesPerCell) && create(m_velocity[1], cells * VelocityBytesPerCell) &&
        create(m_piece1Velocity[0], cells * VelocityBytesPerCell) && create(m_piece1Velocity[1], cells * VelocityBytesPerCell) &&
        create(m_obstacles, cells / SlotsPerObstacleWord * sizeof(std::uint32_t)) && create(m_pieces, cells * PieceBytesPerCell) &&
        create(m_visibility, MaximumMotors * (cells / SlotsPerObstacleWord) * sizeof(std::uint32_t)) &&
        create(m_emissionStatus, MaximumMotors * StatusBytesPerMotor) && create(m_emissionSamples, MaximumMotors * SampleBytesPerMotor) &&
        create(m_splitList, (cells + 1) * sizeof(std::uint32_t)) && create(m_splitArguments, SplitArgumentBytes) &&
        create(m_mistStatus, MaximumMistEmitters * MistStatusBytesPerEmitter);

    if(!created)
    {
        // Never recorded into: released at once.
        for(Buffer* buffer : StorageBuffers())
        {
            buffer->Shutdown();
        }

        return false;
    }

    m_allocated = true;
    m_current = 0;
    // New storage holds no visibility, and its split counter must be zeroed before the first step.
    m_visibilityEntries = {};
    m_splitResetPending = true;
    return true;
}

bool AirField::AllocateMist(const AirFieldSettings& settings)
{
    const std::size_t bytes = CellCount(settings) * MistBytesPerCell;
    bool created = true;

    for(Buffer* buffer : MistBuffers())
    {
        created = created && buffer->InitializeGPUWritable(m_device, bytes) && buffer->CreateRawSRV(m_device, *m_descriptorHeap) &&
            buffer->CreateRawUAV(m_device, *m_descriptorHeap);
    }

    if(!created)
    {
        // Never recorded into: released at once.
        for(Buffer* buffer : MistBuffers())
        {
            buffer->Shutdown();
        }

        return false;
    }

    // New storage holds whatever the allocation held: it is zeroed before anything reads it.
    m_mistAllocated = true;
    m_mistCurrent = 0;
    m_mistClearPending = true;
    return true;
}

void AirField::RetireMist(std::uint64_t fence)
{
    for(Buffer* buffer : MistBuffers())
    {
        if(buffer->GetResource() != nullptr)
        {
            RetiredBuffer retired;
            retired.buffer = std::move(*buffer);
            retired.fence = fence;
            m_retired.push_back(std::move(retired));
        }
    }

    m_mistAllocated = false;
    m_mistBinding.valid = 0;
    m_mistEmitterStatus.clear();
}

void AirField::TransitionStorage(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES state)
{
    for(Buffer* buffer : StorageBuffers())
    {
        buffer->Transition(commandList, state);
    }

    if(m_mistAllocated)
    {
        for(Buffer* buffer : MistBuffers())
        {
            buffer->Transition(commandList, state);
        }
    }
}

void AirField::RetireStorage(std::uint64_t fence)
{
    // The mist lives in the field's window: it goes with it.
    RetireMist(fence);

    for(Buffer* buffer : StorageBuffers())
    {
        if(buffer->GetResource() != nullptr)
        {
            RetiredBuffer retired;
            retired.buffer = std::move(*buffer);
            retired.fence = fence;
            m_retired.push_back(std::move(retired));
        }
    }

    m_allocated = false;
    m_binding.valid = 0;
}

void AirField::Release(std::uint64_t lastSubmissionFence)
{
    RetireStorage(lastSubmissionFence);
    m_resetPending = true;
}

void AirField::CollectRetired(std::uint64_t completedFence)
{
    while(!m_retired.empty() && m_retired.front().fence <= completedFence)
    {
        m_retired.front().buffer.Shutdown();
        m_retired.pop_front();
    }
}

std::uint32_t AirField::TakeSteps(const AirFrameRequest& request)
{
    if(request.paused || !(request.deltaSeconds > 0.0) || !std::isfinite(request.deltaSeconds))
    {
        return 0;
    }

    m_accumulator += request.deltaSeconds;
    const double due = std::floor(m_accumulator / StepSeconds);
    const std::uint32_t steps = due >= MaximumStepsPerFrame ? MaximumStepsPerFrame : static_cast<std::uint32_t>(due);
    m_accumulator -= steps * StepSeconds;

    // A long frame: run the maximum and drop the rest, so the simulation never falls further and further behind.
    if(due > MaximumStepsPerFrame)
    {
        m_accumulator = 0.0;
    }

    return steps;
}

bool AirField::RecordMaintenance(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next, bool full,
    bool shifted, bool voxelsChanged, const VoxelWorldBinding& voxels)
{
    AirFieldConstants constants;
    constants.voxels = voxels;
    constants.field = next;
    constants.fullReset = full ? 1u : 0u;
    constants.rebuildObstacles = (full || voxelsChanged) ? 1u : 0u;
    constants.voxelLevel = m_settings.cellExponent - voxels.baseExponent;
    constants.ambientVelocity[0] = m_settings.ambientVelocity.x;
    constants.ambientVelocity[1] = m_settings.ambientVelocity.y;
    constants.ambientVelocity[2] = m_settings.ambientVelocity.z;
    constants.velocityUnordered[0] = m_velocity[0].GetUAVDescriptorIndex();
    constants.velocityUnordered[1] = m_velocity[1].GetUAVDescriptorIndex();
    constants.obstacleUnordered = m_obstacles.GetUAVDescriptorIndex();
    constants.pieceUnordered = m_pieces.GetUAVDescriptorIndex();
    constants.piece1Unordered[0] = m_piece1Velocity[0].GetUAVDescriptorIndex();
    constants.piece1Unordered[1] = m_piece1Velocity[1].GetUAVDescriptorIndex();
    constants.currentVelocity = m_current;

    if(m_mistAllocated)
    {
        constants.mistUnordered[0] = m_mist[0].GetUAVDescriptorIndex();
        constants.mistUnordered[1] = m_mist[1].GetUAVDescriptorIndex();
        constants.mistPiece1Unordered[0] = m_mistPiece1[0].GetUAVDescriptorIndex();
        constants.mistPiece1Unordered[1] = m_mistPiece1[1].GetUAVDescriptorIndex();
        constants.currentMist = m_mistCurrent;
        constants.mistEnabled = 1;
    }

    for(std::size_t axis = 0; axis < 3; ++axis)
    {
        constants.previousMinimum[axis] = m_minimum[axis];
    }

    const D3D12_GPU_VIRTUAL_ADDRESS address = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(address == 0)
    {
        return false;
    }

    // Entering cells start at the ambient velocity in all four velocity buffers; then their obstacle bits and pieces (every cell's
    // after a VX.5 change, when retained cells' velocity is remapped onto their new pieces). No barrier between these dispatches:
    // none reads what another writes (AirField.hlsl). The barrier after them orders all of it before the steps.
    if(full || shifted)
    {
        m_initializePipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, address);
        DispatchCells(commandList, m_settings);
    }

    const std::uint32_t words = static_cast<std::uint32_t>(CellCount(m_settings) / SlotsPerObstacleWord);
    m_obstaclePipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    commandList->Dispatch((words + 63) / 64, 1, 1);
    // AIR.4D: the fluid pieces of the same cells.
    m_piecePipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    DispatchCells(commandList, m_settings);
    UnorderedAccessBarrier(commandList);

    m_fullInitializations += full ? 1 : 0;
    m_incrementalUpdates += !full && shifted ? 1 : 0;
    m_obstacleRebuilds += constants.rebuildObstacles;

    // New obstacles or pieces: every motor's visibility must be recomputed before it is used again.
    if(constants.rebuildObstacles != 0)
    {
        ++m_obstacleEpoch;
    }

    return true;
}

std::uint32_t AirField::UploadMotors(UploadBuffer& upload, const AirFieldBinding& next, const AirFrameRequest& request,
    D3D12_GPU_VIRTUAL_ADDRESS& address, std::vector<std::uint32_t>& recompute)
{
    recompute.clear();
    m_frameTags.clear();
    // The window in world space; motors whose bounding sphere misses it do no per-cell work at all.
    float low[3];
    float high[3];

    for(std::size_t axis = 0; axis < 3; ++axis)
    {
        low[axis] = static_cast<float>(next.minimum[axis]) * next.cellSize;
        high[axis] = static_cast<float>(next.minimum[axis] + static_cast<std::int32_t>(next.dimensions[axis])) * next.cellSize;
    }

    std::vector<GpuAirMotor> motors;
    motors.reserve((std::min)(request.motors.size(), std::size_t{MaximumMotors}));

    for(const AirMotorInstance& motor : request.motors)
    {
        if(motors.size() >= MaximumMotors)
        {
            break;
        }

        const float position[3] = {motor.position.x, motor.position.y, motor.position.z};
        const float radius = BoundingRadius(motor);
        float distanceSquared = 0.0f;

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            const float outside = (std::max)({low[axis] - position[axis], 0.0f, position[axis] - high[axis]});
            distanceSquared += outside * outside;
        }

        if(!std::isfinite(radius) || distanceSquared > radius * radius)
        {
            continue;
        }

        GpuAirMotor gpu;
        CopyVector(motor.position, gpu.position);
        gpu.type = static_cast<std::uint32_t>(motor.type);
        CopyVector(motor.axisX, gpu.axisX);
        CopyVector(motor.axisY, gpu.axisY);
        CopyVector(motor.axisZ, gpu.axisZ);
        gpu.strength = motor.strength;
        gpu.falloff = motor.falloff;
        gpu.boundingRadius = radius;
        gpu.inwardPull = motor.inwardPull;
        gpu.coupling = motor.coupling;
        CopyVector(motor.velocity, gpu.velocity);
        CopyVector(motor.emissionSemiAxes, gpu.semiAxes);
        // Enough cell crossings for any straight path inside the bounding sphere (sqrt(3) per cell of its diameter), so a
        // visibility path is never cut short.
        const double crossings = std::ceil(2.0 * static_cast<double>(radius) * 1.7320508075688772 * next.inverseCellSize) + 4.0;
        gpu.maximumCrossings = static_cast<std::uint32_t>((std::min)(crossings, 1048576.0));

        if(motor.type == AirMotorType::Directional)
        {
            CopyVector(motor.halfExtents, gpu.size);
        }
        else
        {
            gpu.size[0] = motor.radius;
            gpu.size[1] = motor.halfLength;
        }

        motors.push_back(gpu);
        m_frameTags.push_back(motor.tag);
    }

    if(motors.empty())
    {
        return 0;
    }

    AssignVisibility(motors, next, recompute);
    m_frameMotors = motors;
    address = upload.Allocate(motors.data(), motors.size() * sizeof(GpuAirMotor), 16);

    if(address == 0)
    {
        recompute.clear();
        return 0;
    }

    return static_cast<std::uint32_t>(motors.size());
}

void AirField::AssignVisibility(std::vector<GpuAirMotor>& motors, const AirFieldBinding& next, std::vector<std::uint32_t>& recompute)
{
    const std::array<std::int32_t, 3> minimum{next.minimum[0], next.minimum[1], next.minimum[2]};
    const std::array<std::uint32_t, 3> dimensions = m_settings.dimensions;
    const double cellSize = next.cellSize;
    std::array<bool, MaximumMotors> taken{};
    ++m_visibilityFrame;

    // First, motors whose geometry an entry already holds keep it: additions, removals, reordering and replacements never reuse
    // another motor's bits, because the key is the geometry itself.
    std::vector<std::uint32_t> unassigned;

    for(std::uint32_t index = 0; index < motors.size(); ++index)
    {
        const auto key = VisibilityKey(motors[index]);
        motors[index].visibilitySlot = NoVisibility;

        for(std::uint32_t slot = 0; slot < MaximumMotors; ++slot)
        {
            if(!taken[slot] && m_visibilityEntries[slot].epoch != 0 && m_visibilityEntries[slot].key == key)
            {
                taken[slot] = true;
                motors[index].visibilitySlot = slot;
                break;
            }
        }

        if(motors[index].visibilitySlot == NoVisibility)
        {
            unassigned.push_back(index);
        }
    }

    // Then new geometry takes the entry least recently used (never-used entries first); its bits are computed this frame.
    for(const std::uint32_t index : unassigned)
    {
        std::uint32_t chosen = NoVisibility;

        for(std::uint32_t slot = 0; slot < MaximumMotors; ++slot)
        {
            if(!taken[slot] && (chosen == NoVisibility || m_visibilityEntries[slot].lastUsed < m_visibilityEntries[chosen].lastUsed))
            {
                chosen = slot;
            }
        }

        taken[chosen] = true;
        m_visibilityEntries[chosen] = {};
        m_visibilityEntries[chosen].key = VisibilityKey(motors[index]);
        motors[index].visibilitySlot = chosen;
    }

    // Kept bits stay valid only for the obstacles they were computed with, and for a window that held the motor's whole bounding
    // box before and after a move: then every path inside it stayed inside the window, through cells whose pieces did not change.
    // A motor at the window's edge, whose paths may cross space that entered the window, is recomputed.
    for(std::uint32_t index = 0; index < motors.size(); ++index)
    {
        VisibilityEntry& entry = m_visibilityEntries[motors[index].visibilitySlot];
        const CellBox box = BoundingCells(motors[index], cellSize);
        const bool sameWindow = entry.minimum == minimum;
        const bool contained = BoxInsideWindow(box, entry.minimum, dimensions) && BoxInsideWindow(box, minimum, dimensions);
        const bool valid = entry.epoch == m_obstacleEpoch && (sameWindow || contained);

        if(!valid)
        {
            recompute.push_back(index);
            ++m_visibilityComputes;
        }
        else
        {
            ++m_visibilityKept;
        }

        entry.epoch = m_obstacleEpoch;
        entry.minimum = minimum;
        entry.lastUsed = m_visibilityFrame;
    }
}

bool AirField::RecordMotorWork(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next,
    D3D12_GPU_VIRTUAL_ADDRESS motors, std::uint32_t motorCount, const std::vector<std::uint32_t>& recompute, unsigned int frameIndex)
{
    AirSolverConstants constants;
    constants.field = next;
    constants.obstacleIndex = m_obstacles.GetUAVDescriptorIndex();
    constants.pieceIndex = m_pieces.GetUAVDescriptorIndex();
    constants.visibilityIndex = m_visibility.GetUAVDescriptorIndex();
    constants.statusIndex = m_emissionStatus.GetUAVDescriptorIndex();
    constants.emissionSamplesIndex = m_emissionSamples.GetUAVDescriptorIndex();
    constants.motorCount = motorCount;
    const std::array<std::int32_t, 3> minimum{next.minimum[0], next.minimum[1], next.minimum[2]};

    // AIR.4D D2.1: the lattice states of each shaped emitter being recomputed, which its visibility (and every later status) reads:
    // one group of 8 threads, one per state word. A barrier then makes them visible to the visibility dispatches.
    bool sampled = false;

    for(const std::uint32_t index : recompute)
    {
        if(!IsShapedEmitter(m_frameMotors[index]))
        {
            continue;
        }

        constants.visibilityMotor = index;
        const D3D12_GPU_VIRTUAL_ADDRESS address = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(address == 0)
        {
            return false;
        }

        m_samplesPipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, address);
        commandList->SetComputeRootShaderResourceView(1, motors);
        commandList->Dispatch(1, 1, 1);
        sampled = true;
    }

    if(sampled)
    {
        UnorderedAccessBarrier(commandList);
    }

    // Each motor needing visibility: its bounding box within the window, x widened to whole 16-cell words.
    for(const std::uint32_t index : recompute)
    {
        const CellBox box = BoundingCells(m_frameMotors[index], next.cellSize);
        std::int64_t low[3];
        std::int64_t count[3];

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            low[axis] = (std::max)(box.low[axis], std::int64_t{minimum[axis]});
            const std::int64_t high = (std::min)(box.high[axis], std::int64_t{minimum[axis]} + next.dimensions[axis] - 1);
            count[axis] = high - low[axis] + 1;
        }

        if(count[0] <= 0 || count[1] <= 0 || count[2] <= 0)
        {
            continue;
        }

        // Signed 64-bit throughout: the box often starts at negative cells, and an unsigned word size here would wrap.
        constexpr std::int64_t WordCells = SlotsPerObstacleWord;
        const std::int64_t first = FloorDivide(low[0], WordCells) * WordCells;
        const std::int64_t words = (low[0] + count[0] - 1 - first) / WordCells + 1;
        constants.visibilityMotor = index;
        constants.visibilityBoxMinimum[0] = static_cast<std::int32_t>(first);
        constants.visibilityBoxMinimum[1] = static_cast<std::int32_t>(low[1]);
        constants.visibilityBoxMinimum[2] = static_cast<std::int32_t>(low[2]);
        constants.visibilityBoxWords[0] = static_cast<std::uint32_t>(words);
        constants.visibilityBoxWords[1] = static_cast<std::uint32_t>(count[1]);
        constants.visibilityBoxWords[2] = static_cast<std::uint32_t>(count[2]);
        const D3D12_GPU_VIRTUAL_ADDRESS address = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(address == 0)
        {
            return false;
        }

        m_visibilityPipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, address);
        commandList->SetComputeRootShaderResourceView(1, motors);
        commandList->Dispatch(static_cast<UINT>((words + 3) / 4), static_cast<UINT>((count[1] + 3) / 4), static_cast<UINT>((count[2] + 3) / 4));
    }

    // Every uploaded motor's emission status, copied out for the CPU when this frame slot's previous copy has been read.
    const D3D12_GPU_VIRTUAL_ADDRESS address = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(address == 0)
    {
        return false;
    }

    m_statusPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    commandList->SetComputeRootShaderResourceView(1, motors);
    commandList->Dispatch(1, 1, 1);
    StatusSlot& slot = m_statusSlots[frameIndex % m_statusSlots.size()];

    if(slot.fence == 0)
    {
        m_emissionStatus.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList->CopyBufferRegion(slot.readback.Get(), 0, m_emissionStatus.GetResource(), 0, motorCount * StatusBytesPerMotor);
        m_emissionStatus.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        slot.tags = m_frameTags;
        slot.recorded = true;
    }

    // The steps read the visibility just written.
    UnorderedAccessBarrier(commandList);
    return true;
}

void AirField::MarkSubmitted(unsigned int frameIndex, std::uint64_t fence) noexcept
{
    StatusSlot& slot = m_statusSlots[frameIndex % m_statusSlots.size()];

    if(slot.recorded)
    {
        slot.fence = fence;
        slot.recorded = false;
    }

    StatusSlot& mistSlot = m_mistStatusSlots[frameIndex % m_mistStatusSlots.size()];

    if(mistSlot.recorded)
    {
        mistSlot.fence = fence;
        mistSlot.recorded = false;
    }
}

void AirField::PollEmissionStatus(std::uint64_t completedFence)
{
    for(StatusSlot& slot : m_statusSlots)
    {
        if(slot.fence == 0 || slot.fence > completedFence || !slot.readback)
        {
            continue;
        }

        void* data = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(slot.tags.size() * StatusBytesPerMotor)};

        if(slot.fence >= m_emissionFence && SUCCEEDED(slot.readback->Map(0, &range, &data)))
        {
            std::vector<std::uint32_t> words(slot.tags.size() * 4);
            std::memcpy(words.data(), data, words.size() * sizeof(std::uint32_t));
            const D3D12_RANGE written{0, 0};
            slot.readback->Unmap(0, &written);
            m_emission.clear();

            for(std::size_t index = 0; index < slot.tags.size(); ++index)
            {
                AirMotorEmissionStatus status;
                status.origin = static_cast<AirMotorEmissionStatus::Point>(words[index * 4] & 3u);
                status.samples = words[index * 4 + 1];
                status.blocked = words[index * 4 + 2];
                status.unknown = words[index * 4 + 3];
                m_emission[slot.tags[index]] = status;
            }

            m_emissionFence = slot.fence;
        }

        slot.fence = 0;
    }
}

bool AirField::RecordStep(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next,
    const AirFrameRequest& request, D3D12_GPU_VIRTUAL_ADDRESS motors, std::uint32_t motorCount)
{
    AirSolverConstants constants;
    constants.field = next;
    constants.ambientVelocity[0] = m_settings.ambientVelocity.x;
    constants.ambientVelocity[1] = m_settings.ambientVelocity.y;
    constants.ambientVelocity[2] = m_settings.ambientVelocity.z;
    constants.timeStep = static_cast<float>(StepSeconds);
    constants.velocityReadIndex = m_velocity[m_current].GetUAVDescriptorIndex();
    constants.velocityWriteIndex = m_velocity[1 - m_current].GetUAVDescriptorIndex();
    constants.obstacleIndex = m_obstacles.GetUAVDescriptorIndex();
    constants.piece1ReadIndex = m_piece1Velocity[m_current].GetUAVDescriptorIndex();
    constants.piece1WriteIndex = m_piece1Velocity[1 - m_current].GetUAVDescriptorIndex();
    constants.pieceIndex = m_pieces.GetUAVDescriptorIndex();
    constants.visibilityIndex = m_visibility.GetUAVDescriptorIndex();
    constants.splitListIndex = m_splitList.GetUAVDescriptorIndex();
    constants.splitArgumentsIndex = m_splitArguments.GetUAVDescriptorIndex();
    const double dampingRate = (std::max)(0.0, static_cast<double>(request.solver.dampingRate));
    constants.dampingFactor = static_cast<float>(std::exp(-dampingRate * StepSeconds));
    constants.motorCount = motorCount;
    // Below the backtrace's reach (8 cells per step), with a margin, so motor-driven air is never truncated.
    const double reachSpeed = 0.9 * 8.0 * static_cast<double>(next.cellSize) / StepSeconds;
    constants.maximumMotorSpeed = static_cast<float>((std::min)(reachSpeed, (std::max)(0.0, static_cast<double>(request.solver.maximumMotorSpeed))));

    const D3D12_GPU_VIRTUAL_ADDRESS address = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(address == 0)
    {
        return false;
    }

    // With no motors the SRV is never read; the constants stand in so the root parameter is a valid address.
    const D3D12_GPU_VIRTUAL_ADDRESS motorView = motorCount > 0 ? motors : address;

    if(m_splitResetPending)
    {
        m_resetSplitPipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, address);
        commandList->Dispatch(1, 1, 1);
        UnorderedAccessBarrier(commandList);
        m_splitResetPending = false;
    }

    // Pass 1: every cell whose step meets only open cells; the others are listed.
    m_stepPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    commandList->SetComputeRootShaderResourceView(1, motorView);
    DispatchCells(commandList, m_settings);
    // The list and its counter are complete before they are read.
    UnorderedAccessBarrier(commandList);

    // The count becomes the split pass's dispatch (and the counter is zeroed for the next step).
    m_prepareSplitPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    commandList->Dispatch(1, 1, 1);
    UnorderedAccessBarrier(commandList);
    m_splitArguments.Transition(commandList, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    // Pass 2: the listed cells, by the full piece-aware path. It reads the same current buffers as pass 1 and writes other cells.
    m_splitStepPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    commandList->SetComputeRootShaderResourceView(1, motorView);
    commandList->ExecuteIndirect(m_splitSignature.Get(), 1, m_splitArguments.GetResource(), SplitDispatchOffset, nullptr, 0);
    m_splitArguments.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // The next step, or a reader, sees this step's writes; the buffers of both pairs swap roles together.
    UnorderedAccessBarrier(commandList);
    m_current = 1 - m_current;
    ++m_steps;
    return true;
}

bool AirField::RecordMistClear(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next)
{
    AirMistConstants constants;
    constants.field = next;
    const auto buffers = MistBuffers();

    for(std::size_t index = 0; index < buffers.size(); ++index)
    {
        constants.clear[index] = buffers[index]->GetUAVDescriptorIndex();
    }

    const D3D12_GPU_VIRTUAL_ADDRESS address = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(address == 0)
    {
        return false;
    }

    m_mistClearPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    DispatchCells(commandList, m_settings);
    // Maintenance (entering cells, rebuild remapping) writes the same buffers next.
    UnorderedAccessBarrier(commandList);
    ++m_mistClears;
    return true;
}

std::uint32_t AirField::UploadMistEmitters(UploadBuffer& upload, const AirFieldBinding& next, const AirFrameRequest& request,
    D3D12_GPU_VIRTUAL_ADDRESS& address)
{
    m_frameEmitterBoxes.clear();
    m_frameEmitterTags.clear();
    std::vector<GpuMistEmitter> emitters;
    const double cellSize = next.cellSize;

    for(const MistEmitterInstance& emitter : request.mistEmitters)
    {
        if(emitters.size() >= MaximumMistEmitters)
        {
            break;
        }

        const Vector3& half = emitter.halfExtents;
        const float boundingRadius = emitter.shape == MistEmitterShape::Sphere ? emitter.radius :
            std::sqrt(half.x * half.x + half.y * half.y + half.z * half.z);
        const double position[3] = {emitter.position.x, emitter.position.y, emitter.position.z};

        if(!std::isfinite(boundingRadius) || !std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2]) ||
            !(emitter.densityRate >= 0.0f) || !std::isfinite(emitter.densityRate))
        {
            continue;
        }

        // The cells holding the bounding sphere, within the window; an emitter missing the window does no work at all.
        EmitterBox box;
        bool reaches = true;

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            const std::int64_t windowLow = next.minimum[axis];
            const std::int64_t windowHigh = windowLow + next.dimensions[axis] - 1;
            const std::int64_t low = (std::max)(static_cast<std::int64_t>(std::floor((position[axis] - boundingRadius) / cellSize)), windowLow);
            const std::int64_t high = (std::min)(static_cast<std::int64_t>(std::floor((position[axis] + boundingRadius) / cellSize)), windowHigh);
            reaches = reaches && high >= low;
            box.minimum[axis] = static_cast<std::int32_t>(low);
            box.size[axis] = reaches ? static_cast<std::uint32_t>(high - low + 1) : 0;
        }

        if(!reaches)
        {
            continue;
        }

        GpuMistEmitter gpu;
        CopyVector(emitter.position, gpu.position);
        gpu.shape = static_cast<std::uint32_t>(emitter.shape);
        CopyVector(emitter.axisX, gpu.axisX);
        CopyVector(emitter.axisY, gpu.axisY);
        CopyVector(emitter.axisZ, gpu.axisZ);
        gpu.radius = emitter.radius;
        CopyVector(emitter.halfExtents, gpu.halfExtents);
        CopyVector(emitter.color, gpu.color);
        gpu.densityPerStep = static_cast<float>(static_cast<double>(emitter.densityRate) * StepSeconds);
        // Enough cell crossings for any straight path inside the bounding sphere, as for motors.
        const double crossings = std::ceil(2.0 * static_cast<double>(boundingRadius) * 1.7320508075688772 * next.inverseCellSize) + 4.0;
        gpu.maximumCrossings = static_cast<std::uint32_t>((std::min)(crossings, 1048576.0));
        emitters.push_back(gpu);
        m_frameEmitterBoxes.push_back(box);
        m_frameEmitterTags.push_back(emitter.tag);
    }

    if(emitters.empty())
    {
        return 0;
    }

    address = upload.Allocate(emitters.data(), emitters.size() * sizeof(GpuMistEmitter), 16);

    if(address == 0)
    {
        m_frameEmitterBoxes.clear();
        m_frameEmitterTags.clear();
        return 0;
    }

    return static_cast<std::uint32_t>(emitters.size());
}

bool AirField::RecordMistEmitterStatus(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next,
    D3D12_GPU_VIRTUAL_ADDRESS emitters, std::uint32_t emitterCount)
{
    AirMistConstants constants;
    constants.field = next;
    constants.obstacleIndex = m_obstacles.GetUAVDescriptorIndex();
    constants.pieceIndex = m_pieces.GetUAVDescriptorIndex();
    constants.emitterCount = emitterCount;
    constants.statusIndex = m_mistStatus.GetUAVDescriptorIndex();
    const D3D12_GPU_VIRTUAL_ADDRESS address = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(address == 0)
    {
        return false;
    }

    m_mistEmitterStatusPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    commandList->SetComputeRootShaderResourceView(1, emitters);
    commandList->Dispatch(1, 1, 1);
    // The steps' emission adds receiving pieces to these words.
    UnorderedAccessBarrier(commandList);
    return true;
}

void AirField::CopyMistEmitterStatus(ID3D12GraphicsCommandList* commandList, unsigned int frameIndex)
{
    StatusSlot& slot = m_mistStatusSlots[frameIndex % m_mistStatusSlots.size()];

    // The slot's previous copy must have been read first.
    if(slot.fence != 0 || m_frameEmitterTags.empty())
    {
        return;
    }

    m_mistStatus.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyBufferRegion(slot.readback.Get(), 0, m_mistStatus.GetResource(), 0, m_frameEmitterTags.size() * MistStatusBytesPerEmitter);
    m_mistStatus.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    slot.tags = m_frameEmitterTags;
    slot.recorded = true;
}

void AirField::PollMistEmitterStatus(std::uint64_t completedFence)
{
    for(StatusSlot& slot : m_mistStatusSlots)
    {
        if(slot.fence == 0 || slot.fence > completedFence || !slot.readback)
        {
            continue;
        }

        void* data = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(slot.tags.size() * MistStatusBytesPerEmitter)};

        if(slot.fence >= m_mistStatusFence && SUCCEEDED(slot.readback->Map(0, &range, &data)))
        {
            std::vector<std::uint32_t> words(slot.tags.size() * 4);
            std::memcpy(words.data(), data, words.size() * sizeof(std::uint32_t));
            const D3D12_RANGE written{0, 0};
            slot.readback->Unmap(0, &written);
            m_mistEmitterStatus.clear();

            for(std::size_t index = 0; index < slot.tags.size(); ++index)
            {
                MistEmitterStatus status;
                status.origin = static_cast<MistEmitterStatus::Origin>(words[index * 4] & 3u);
                status.pieces = words[index * 4 + 1];
                m_mistEmitterStatus[slot.tags[index]] = status;
            }

            m_mistStatusFence = slot.fence;
        }

        slot.fence = 0;
    }
}

bool AirField::RecordMistStep(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next,
    const AirMistSettings& mist, D3D12_GPU_VIRTUAL_ADDRESS emitters, std::uint32_t emitterCount, bool countPieces)
{
    AirMistConstants constants;
    constants.field = next;
    constants.obstacleIndex = m_obstacles.GetUAVDescriptorIndex();
    constants.pieceIndex = m_pieces.GetUAVDescriptorIndex();
    // The velocity the solver's step just wrote (RecordStep has swapped m_current to it).
    constants.velocityIndex = m_velocity[m_current].GetUAVDescriptorIndex();
    constants.velocityPiece1Index = m_piece1Velocity[m_current].GetUAVDescriptorIndex();
    constants.mistRead[0] = m_mist[m_mistCurrent].GetUAVDescriptorIndex();
    constants.mistRead[1] = m_mistPiece1[m_mistCurrent].GetUAVDescriptorIndex();
    constants.mistWrite[0] = m_mist[1 - m_mistCurrent].GetUAVDescriptorIndex();
    constants.mistWrite[1] = m_mistPiece1[1 - m_mistCurrent].GetUAVDescriptorIndex();
    constants.timeStep = static_cast<float>(StepSeconds);
    const double dissipationRate = std::isfinite(mist.dissipationRate) ? (std::max)(0.0, static_cast<double>(mist.dissipationRate)) : 0.0;
    constants.dissipationFactor = static_cast<float>(std::exp(-dissipationRate * StepSeconds));
    constants.splitListIndex = m_splitList.GetUAVDescriptorIndex();
    constants.splitArgumentsIndex = m_splitArguments.GetUAVDescriptorIndex();
    const D3D12_GPU_VIRTUAL_ADDRESS address = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(address == 0)
    {
        return false;
    }

    // Pass 1: every cell whose mist step meets only open cells; the others are listed. The solver's step left the counter zeroed
    // and its list unread from here on (RecordStep ends with a UAV barrier).
    m_mistOpenPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    DispatchCells(commandList, m_settings);
    UnorderedAccessBarrier(commandList);

    m_mistPreparePipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    commandList->Dispatch(1, 1, 1);
    UnorderedAccessBarrier(commandList);
    m_splitArguments.Transition(commandList, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    // Pass 2: the listed cells, by the full piece-aware path.
    m_mistSplitPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    commandList->ExecuteIndirect(m_splitSignature.Get(), 1, m_splitArguments.GetResource(), SplitDispatchOffset, nullptr, 0);
    m_splitArguments.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    UnorderedAccessBarrier(commandList);

    // AIR.4F F2: emission into the step's new mist, one emitter at a time in upload order, a barrier after each so overlapping
    // emitters add deterministically.
    for(std::uint32_t index = 0; index < emitterCount; ++index)
    {
        const EmitterBox& box = m_frameEmitterBoxes[index];
        constants.emitterIndex = index;
        constants.emitterCount = emitterCount;
        constants.countPieces = countPieces ? 1u : 0u;
        constants.statusIndex = m_mistStatus.GetUAVDescriptorIndex();

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            constants.emitBoxMinimum[axis] = box.minimum[axis];
            constants.emitBoxSize[axis] = box.size[axis];
        }

        const D3D12_GPU_VIRTUAL_ADDRESS emitAddress = upload.Allocate(&constants, sizeof(constants),
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(emitAddress == 0)
        {
            break;
        }

        m_mistEmitPipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, emitAddress);
        commandList->SetComputeRootShaderResourceView(1, emitters);
        commandList->Dispatch((box.size[0] + 3) / 4, (box.size[1] + 3) / 4, (box.size[2] + 3) / 4);
        UnorderedAccessBarrier(commandList);
        ++m_emitDispatches;
    }

    // The next step (solver or mist), or a reader, sees this step's mist.
    m_mistCurrent = 1 - m_mistCurrent;
    ++m_mistSteps;
    m_mistSeconds += StepSeconds;
    return true;
}

void AirField::Record(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, UploadBuffer& upload,
    const AirFrameRequest& request, const VoxelWorldBinding& voxels, unsigned int frameIndex, std::uint64_t lastSubmissionFence,
    std::uint64_t completedFence)
{
    CollectRetired(completedFence);
    PollEmissionStatus(completedFence);
    PollMistEmitterStatus(completedFence);

    if(!request.enabled || !ValidateSettings(request.settings, nullptr))
    {
        m_binding.valid = 0;
        m_mistBinding.valid = 0;
        m_emission.clear();
        m_mistEmitterStatus.clear();
        return;
    }

    // New dimensions or cell size: fresh storage; the old buffers wait for frames still reading them.
    const bool reshaped = request.settings.dimensions != m_settings.dimensions || request.settings.cellExponent != m_settings.cellExponent;

    if(!m_allocated || reshaped)
    {
        RetireStorage(lastSubmissionFence);

        if(!Allocate(request.settings))
        {
            return;
        }

        m_resetPending = true;
    }

    m_settings = request.settings;

    // AIR.4F: the mist's storage follows its setting: allocated (and zeroed before anything reads it) when enabled, retired when
    // disabled. A failed allocation leaves the air running without mist; it is retried next frame.
    if(request.mist.enabled && !m_mistAllocated)
    {
        AllocateMist(m_settings);
    }
    else if(!request.mist.enabled && m_mistAllocated)
    {
        RetireMist(lastSubmissionFence);
    }

    const double cellSize = std::ldexp(1.0, m_settings.cellExponent);
    const std::array<double, 3> camera{request.cameraPosition.x, request.cameraPosition.y, request.cameraPosition.z};

    // The window's minimum: centred on the camera's cell, in whole recentring steps; it moves per axis only once the camera is
    // more than one step from the centre.
    std::array<std::int32_t, 3> minimum = m_minimum;
    bool teleport = false;

    for(std::size_t axis = 0; axis < 3; ++axis)
    {
        const std::int32_t size = static_cast<std::int32_t>(m_settings.dimensions[axis]);
        const auto cameraCell = static_cast<std::int64_t>(std::floor(camera[axis] / cellSize));
        const std::int64_t centre = std::int64_t{m_minimum[axis]} + size / 2;
        const std::int32_t target = FloorDivide(cameraCell - size / 2, RecentreStep) * RecentreStep;

        if(m_resetPending || std::llabs(cameraCell - centre) > RecentreStep)
        {
            minimum[axis] = target;
        }

        teleport = teleport || std::llabs(std::int64_t{minimum[axis]} - m_minimum[axis]) >= size;
    }

    const bool full = m_resetPending || teleport;
    const bool shifted = minimum != m_minimum;
    const bool voxelsChanged = voxels.generation != m_voxelGeneration || voxels.valid != m_voxelValid;
    const bool maintenance = full || shifted || voxelsChanged;

    // A re-initialized field starts its clock afresh.
    if(full)
    {
        m_accumulator = 0.0;
    }

    const std::uint32_t steps = TakeSteps(request);
    const bool mistClear = m_mistAllocated && m_mistClearPending;

    // The binding after this frame's work; published only once that work is recorded.
    AirFieldBinding next = m_binding;
    next.valid = 1;
    next.obstacleIndex = m_obstacles.GetDescriptorIndex();
    next.pieceIndex = m_pieces.GetDescriptorIndex();
    next.cellSize = static_cast<float>(cellSize);
    next.inverseCellSize = static_cast<float>(1.0 / cellSize);

    for(std::size_t axis = 0; axis < 3; ++axis)
    {
        next.minimum[axis] = minimum[axis];
        next.dimensions[axis] = m_settings.dimensions[axis];
    }

    if(maintenance || steps > 0 || mistClear)
    {
        ID3D12DescriptorHeap* heaps[]{descriptorHeap.GetHeap()};
        commandList->SetDescriptorHeaps(1, heaps);
        TransitionStorage(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // A mist clear first (new mist storage, or ClearMist), so maintenance and steps work on cleared mist; then maintenance, so
        // entering cells are initialized before a step reads them.
        const bool cleared = !mistClear || RecordMistClear(commandList, upload, next);

        if(!cleared || (maintenance && !RecordMaintenance(commandList, upload, next, full, shifted, voxelsChanged, voxels)))
        {
            // No room for constants this frame: the previous binding stays (invalid if never initialized); retried next frame.
            TransitionStorage(commandList, ReadState);
            m_binding.valid = m_resetPending ? 0u : m_binding.valid;
            m_mistBinding.valid = 0;
            return;
        }

        m_mistClearPending = m_mistClearPending && !mistClear;

        m_minimum = minimum;
        m_voxelGeneration = voxels.generation;
        m_voxelValid = voxels.valid;
        m_resetPending = false;

        // The frame's motors are uploaded once and read by every step, after their visibility (where it must be recomputed) and
        // emission status. Without visibility a motor applies nothing, so a failed recording drops the motors for the frame.
        D3D12_GPU_VIRTUAL_ADDRESS motors = 0;
        std::vector<std::uint32_t> recompute;
        std::uint32_t motorCount = steps > 0 ? UploadMotors(upload, next, request, motors, recompute) : 0;

        if(motorCount > 0 && !RecordMotorWork(commandList, upload, next, motors, motorCount, recompute, frameIndex))
        {
            m_visibilityEntries = {};
            motorCount = 0;
        }

        m_lastMotorCount = motorCount;

        // AIR.4F: each solver step is followed by a mist step carried by the velocity it just wrote. The mist never feeds back:
        // no solver kernel reads it.
        bool mistStepping = m_mistAllocated && !request.mist.paused;

        // AIR.4F F2: the emitters, uploaded once for the frame's mist steps, and their origin states for the Inspector.
        D3D12_GPU_VIRTUAL_ADDRESS emitters = 0;
        std::uint32_t emitterCount = mistStepping && steps > 0 ? UploadMistEmitters(upload, next, request, emitters) : 0;

        if(emitterCount > 0 && !RecordMistEmitterStatus(commandList, upload, next, emitters, emitterCount))
        {
            emitterCount = 0;
        }

        m_lastMistEmitterCount = emitterCount;

        for(std::uint32_t step = 0; step < steps; ++step)
        {
            if(!RecordStep(commandList, upload, next, request, motors, motorCount))
            {
                break;
            }

            if(mistStepping && !RecordMistStep(commandList, upload, next, request.mist, emitters, emitterCount, step == 0))
            {
                mistStepping = false;
            }
        }

        if(emitterCount > 0)
        {
            CopyMistEmitterStatus(commandList, frameIndex);
        }

        // Readers of this frame, and of later frames, see the latest velocity, mist and mask in a shader-read state.
        TransitionStorage(commandList, ReadState);

        ++next.generation;
    }

    next.velocityIndex = m_velocity[m_current].GetDescriptorIndex();
    next.velocityOtherIndex = m_velocity[1 - m_current].GetDescriptorIndex();
    next.velocityPiece1Index = m_piece1Velocity[m_current].GetDescriptorIndex();
    next.velocityPiece1OtherIndex = m_piece1Velocity[1 - m_current].GetDescriptorIndex();
    m_binding = next;

    m_mistBinding = {};

    if(m_mistAllocated && !m_mistClearPending)
    {
        m_mistBinding.valid = 1;
        m_mistBinding.piece0Index = m_mist[m_mistCurrent].GetDescriptorIndex();
        m_mistBinding.piece1Index = m_mistPiece1[m_mistCurrent].GetDescriptorIndex();
        m_mistBinding.generation = next.generation;
    }
}
