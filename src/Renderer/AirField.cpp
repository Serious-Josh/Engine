#include "Renderer/AirField.h"
#include "Debug/MemoryRegistry.h"

#include "Renderer/DescriptorHeap.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/UploadBuffer.h"
#include "Scene/AirEnvironmentSettings.h"

#include <algorithm>
#include <cmath>
#include <numbers>
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
    // AIR.4E.1: the face connections, writable.
    std::uint32_t apertureUnordered = 0;
    std::uint32_t maintenancePadding = 0;
};

static_assert(sizeof(AirFieldConstants) == 288);

// AIR.4F.3 F2b.1: CSBoundaryVolumes' groups per dispatch row (AirMovingSolids.hlsl, AirBoundaryGroupRow).
constexpr std::uint32_t BoundaryGroupRow = 32768;

// Must match the AirMovingSolidConstants cbuffer of AirMovingSolids.hlsl.
struct AirMovingSolidConstants
{
    VoxelWorldBinding voxels;
    AirFieldBinding field;
    std::int32_t voxelLevel = 0;
    std::uint32_t pieceUnordered = 0;
    std::uint32_t apertureUnordered = 0;
    std::uint32_t solidCount = 0;
    std::uint32_t boxCount = 0;
    std::uint32_t totalCells = 0;
    std::uint32_t obstacleUnordered = 0;
    std::uint32_t phiUnordered = UINT32_MAX;
    std::uint32_t truncationUnordered = UINT32_MAX;
    std::uint32_t componentUnordered = UINT32_MAX;
    std::uint32_t topologyUnordered = UINT32_MAX;
    // AIR.4F.3 F2b.4: the AIR step in seconds (CSWallTangential only).
    float stepSeconds = 0.0f;
    // AIR.4F.3 F1b: current velocity and mist (written in place), the scratch views, the ledger and the relay predicate.
    std::uint32_t velocityUnordered[2] = {UINT32_MAX, UINT32_MAX};
    std::uint32_t mistUnordered[2] = {UINT32_MAX, UINT32_MAX};
    std::uint32_t scratchAmount[2] = {UINT32_MAX, UINT32_MAX};
    std::uint32_t scratchMasks = UINT32_MAX;
    std::uint32_t scratchState = UINT32_MAX;
    std::uint32_t ledgerUnordered = UINT32_MAX;
    std::uint32_t predicateUnordered = UINT32_MAX;
    std::uint32_t relayLevel = 0;
    std::uint32_t relayParity = 0;
    std::uint32_t donorBoxCount = 0;
    // AIR.4F.3 F2b.1: the anchor pose list in the solids buffer, whether the boundary passes run (2: also clear the effective-volume offset),
    // and the boundary buffers.
    std::uint32_t anchorOffset = 0;
    std::uint32_t anchorCount = 0;
    std::uint32_t boundaryEnabled = 0;
    std::uint32_t effectiveOffsetUnordered = UINT32_MAX;
    std::uint32_t wallSourceUnordered = UINT32_MAX;
    std::uint32_t correctionUnordered = UINT32_MAX;
    std::uint32_t boundaryDiagnosticsUnordered = UINT32_MAX;
    // AIR.4F.3 F2b.2: the per-boundary-cell records and whether this update is continuous (the control-volume remap).
    std::uint32_t recordUnordered = UINT32_MAX;
    std::uint32_t continuousRemap = 0;
    // Diagnostics only (F2a's naive comparison): the gather leaves S at zero and the densities geometric.
    std::uint32_t naiveCoupling = 0;
    // F2b.4: the tangential coupling rate times the step (CSWallTangential only).
    float tangentialRateStep = 0.0f;
    std::int32_t boxMinimum[AirField::MovingSolidBoxCapacity][4] = {};
    std::int32_t boxSize[AirField::MovingSolidBoxCapacity][4] = {};
    std::int32_t donorMinimum[AirField::MovingSolidBoxCapacity][4] = {};
    std::int32_t donorSize[AirField::MovingSolidBoxCapacity][4] = {};
};

static_assert(sizeof(AirMovingSolidConstants) == 128 + 64 + 48 + 64 + 16 + 16 + AirField::MovingSolidBoxCapacity * 64);

// Must match AirMovingSolid in AirMovingSolids.hlsl: world space.
struct GpuAirMovingSolid
{
    float center[3] = {0.0f, 0.0f, 0.0f};
    std::uint32_t type = 0;
    float axisX[3] = {1.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    float axisY[3] = {0.0f, 1.0f, 0.0f};
    // AIR.4F.3 F2b.1: the exact volume in sub-cell volumes, and bit 0 isolated (its coverage may be corrected to that volume).
    float exactVolume = 0.0f;
    float axisZ[3] = {0.0f, 0.0f, 1.0f};
    std::uint32_t boundaryFlags = 0;
    float halfExtents[3] = {0.0f, 0.0f, 0.0f};
    float padding2 = 0.0f;
    float segmentA[3] = {0.0f, 0.0f, 0.0f};
    float padding3 = 0.0f;
    float segmentB[3] = {0.0f, 0.0f, 0.0f};
    float padding4 = 0.0f;
    float boundsMinimum[3] = {0.0f, 0.0f, 0.0f};
    float padding5 = 0.0f;
    float boundsMaximum[3] = {0.0f, 0.0f, 0.0f};
    float padding6 = 0.0f;
};

static_assert(sizeof(GpuAirMovingSolid) == 144);

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
    // AIR.4F.2b, the Point passes: the step of the frame (0 or 1).
    std::uint32_t emitStep = 0;
    std::uint32_t emitBoxSize[3] = {0, 0, 0};
    std::uint32_t emitPadding1 = 0;
};

static_assert(sizeof(AirMistConstants) == 176);

// AIR.4F.2d.4 Stage 2a: must match the AirMistFluxConstants cbuffer of AirMistFlux.hlsl.
struct AirMistFluxConstants
{
    AirFieldBinding field;
    std::uint32_t apertureIndex = 0;
    std::uint32_t phiIndex = 0;
    std::uint32_t mistCurrent[2] = {0, 0};
    std::uint32_t mistOutgoing[2] = {0, 0};
    float timeStep = 0.0f;
    float dissipationFactor = 1.0f;
    float ambient[3] = {0.0f, 0.0f, 0.0f};
    float buoyancyImpulse = 0.0f;
    float referenceTemperature = 293.15f;
    // Stage 2b.1: the pass's substep (0 for the Courant and decision passes), the substep state's and arguments' writable views, and the
    // diagnostic forced count.
    std::uint32_t substep = 0;
    std::uint32_t substepStateIndex = 0;
    std::uint32_t forcedSubsteps = 0;
    std::uint32_t substepArgumentsIndex = 0;
    // AIR.4F.3 F2b.2: the effective-volume offset and wall source (UINT32_MAX: geometric volumes, the transport exactly as before).
    std::uint32_t effectiveOffsetIndex = UINT32_MAX;
    std::uint32_t wallSourceIndex = UINT32_MAX;
    std::uint32_t padding = 0;
};

static_assert(sizeof(AirMistFluxConstants) == 144);

// Stage 2b.1: the substep state's header (N, dt / N, the largest Courant number, padding), before one float per Courant thread group
// (SubstepGroupsOffset in AirMistFlux.hlsl), and the indirect records of substeps 2 to 4 (one D3D12_DISPATCH_ARGUMENTS each).
constexpr std::size_t MistSubstepHeaderBytes = 16;
constexpr std::uint32_t MaximumMistSubsteps = 4;
constexpr std::size_t MistSubstepArgumentBytes = (MaximumMistSubsteps - 1) * sizeof(D3D12_DISPATCH_ARGUMENTS);
static_assert(sizeof(GpuMistEmitter) == 112);
// AIR.4F F2: per emitter, uint words 0 origin state and 1 receiving pieces; AIR.4F.2b, for each step s of the frame, 2 + 2s a Point
// emitter's total weight and 3 + 2s its part outside the window; 6 and 7 unused. Matches StatusStride in AirMist.hlsl.
constexpr std::size_t MistStatusBytesPerEmitter = 32;

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
    // AIR.4F F4.5.1b: exp(-timeStep / relaxation time), the share of ΔT kept per step (1: no relaxation).
    float temperatureFactor = 1.0f;
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
// AIR.4E.1: the face connections (Air.hlsli, AirApertureBytesPerSlot).
constexpr std::size_t ApertureBytesPerCell = 8;
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

// Active rotation of a vector by a unit quaternion (x, y, z, w): v + 2w(q x v) + 2q x (q x v).
Vector3 RotateByQuaternion(const std::array<float, 4>& rotation, const Vector3& vector)
{
    const Vector3 axis{rotation[0], rotation[1], rotation[2]};
    const Vector3 twiceCross = Vector3::Cross(axis, vector) * 2.0f;
    return vector + twiceCross * rotation[3] + Vector3::Cross(axis, twiceCross);
}

void StoreVector(const Vector3& vector, float (&destination)[3])
{
    destination[0] = vector.x;
    destination[1] = vector.y;
    destination[2] = vector.z;
}

// AIR.4F.3 F1a: a solid in world space, with its world bounds.
GpuAirMovingSolid ToGpuMovingSolid(const AirMovingSolid& solid)
{
    GpuAirMovingSolid gpu;
    gpu.type = static_cast<std::uint32_t>(solid.shape);
    gpu.radius = solid.radius;
    const Vector3 axisX = RotateByQuaternion(solid.orientation, {1.0f, 0.0f, 0.0f});
    const Vector3 axisY = RotateByQuaternion(solid.orientation, {0.0f, 1.0f, 0.0f});
    const Vector3 axisZ = RotateByQuaternion(solid.orientation, {0.0f, 0.0f, 1.0f});
    const Vector3 center = solid.position + RotateByQuaternion(solid.orientation, solid.center);
    StoreVector(center, gpu.center);
    StoreVector(axisX, gpu.axisX);
    StoreVector(axisY, gpu.axisY);
    StoreVector(axisZ, gpu.axisZ);
    StoreVector(solid.halfExtents, gpu.halfExtents);
    Vector3 low = center;
    Vector3 high = center;

    if(solid.shape == AirMovingSolidShape::Box)
    {
        const Vector3 reach{
            std::abs(axisX.x) * solid.halfExtents.x + std::abs(axisY.x) * solid.halfExtents.y + std::abs(axisZ.x) * solid.halfExtents.z,
            std::abs(axisX.y) * solid.halfExtents.x + std::abs(axisY.y) * solid.halfExtents.y + std::abs(axisZ.y) * solid.halfExtents.z,
            std::abs(axisX.z) * solid.halfExtents.x + std::abs(axisY.z) * solid.halfExtents.y + std::abs(axisZ.z) * solid.halfExtents.z};
        low = center - reach;
        high = center + reach;
    }
    else if(solid.shape == AirMovingSolidShape::Sphere)
    {
        const Vector3 reach{solid.radius, solid.radius, solid.radius};
        low = center - reach;
        high = center + reach;
    }
    else
    {
        const Vector3 segmentA = solid.position + RotateByQuaternion(solid.orientation, solid.lowerCenter);
        const Vector3 segmentB = solid.position + RotateByQuaternion(solid.orientation, solid.upperCenter);
        StoreVector(segmentA, gpu.segmentA);
        StoreVector(segmentB, gpu.segmentB);
        low = {(std::min)(segmentA.x, segmentB.x) - solid.radius, (std::min)(segmentA.y, segmentB.y) - solid.radius,
            (std::min)(segmentA.z, segmentB.z) - solid.radius};
        high = {(std::max)(segmentA.x, segmentB.x) + solid.radius, (std::max)(segmentA.y, segmentB.y) + solid.radius,
            (std::max)(segmentA.z, segmentB.z) + solid.radius};
    }

    StoreVector(low, gpu.boundsMinimum);
    StoreVector(high, gpu.boundsMaximum);
    return gpu;
}

// Whether two applied solids carve the same sub-cells: the same shape and pose (the epoch alone changes nothing).
bool SameMovingSolidGeometry(const AirMovingSolid& left, const AirMovingSolid& right)
{
    const auto sameVector = [](const Vector3& a, const Vector3& b)
    {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    };

    return left.shape == right.shape && sameVector(left.position, right.position) && left.orientation == right.orientation &&
        sameVector(left.center, right.center) && sameVector(left.halfExtents, right.halfExtents) &&
        sameVector(left.lowerCenter, right.lowerCenter) && sameVector(left.upperCenter, right.upperCenter) && left.radius == right.radius;
}

// AIR.4F.3 F1b: an upper bound, in metres, on how far any surface point of the solid moved between two poses: the largest shift of its
// world bounds, plus the rotation angle times the half diagonal of its bounds (which a rotation inside unchanged bounds needs).
double MovingSolidReach(const AirMovingSolid& from, const AirMovingSolid& to)
{
    const GpuAirMovingSolid before = ToGpuMovingSolid(from);
    const GpuAirMovingSolid after = ToGpuMovingSolid(to);
    double boundsShift = 0.0;
    double halfDiagonalSquared = 0.0;

    for(std::size_t axis = 0; axis < 3; ++axis)
    {
        boundsShift = (std::max)(boundsShift, std::abs(double(after.boundsMinimum[axis]) - double(before.boundsMinimum[axis])));
        boundsShift = (std::max)(boundsShift, std::abs(double(after.boundsMaximum[axis]) - double(before.boundsMaximum[axis])));
        const double halfExtent = 0.5 * (double(before.boundsMaximum[axis]) - double(before.boundsMinimum[axis]));
        halfDiagonalSquared += halfExtent * halfExtent;
    }

    double dot = 0.0;

    for(std::size_t component = 0; component < 4; ++component)
    {
        dot += double(from.orientation[component]) * double(to.orientation[component]);
    }

    const double angle = 2.0 * std::acos(std::clamp(std::abs(dot), 0.0, 1.0));
    return boundsShift + angle * std::sqrt(halfDiagonalSquared);
}

// AIR.4F.3 F1a: the pose `fraction` of the way from the applied solid to the target: positions linearly, orientations along the
// shortest arc (normalized linear blend, exact at both ends). A solid without an applied pose, or in a new motion epoch, jumps.
AirMovingSolid InterpolateMovingSolid(const AirMovingSolid* applied, const AirMovingSolid& target, double fraction)
{
    if(applied == nullptr || applied->motionEpoch != target.motionEpoch || fraction >= 1.0)
    {
        return target;
    }

    AirMovingSolid result = target;
    const float weight = static_cast<float>(fraction);
    result.position = applied->position + (target.position - applied->position) * weight;
    float dot = 0.0f;

    for(std::size_t index = 0; index < 4; ++index)
    {
        dot += applied->orientation[index] * target.orientation[index];
    }

    const float sign = dot < 0.0f ? -1.0f : 1.0f;
    float length = 0.0f;

    for(std::size_t index = 0; index < 4; ++index)
    {
        result.orientation[index] = applied->orientation[index] * (1.0f - weight) + sign * target.orientation[index] * weight;
        length += result.orientation[index] * result.orientation[index];
    }

    length = std::sqrt(length);

    for(float& component : result.orientation)
    {
        component /= length;
    }

    return result;
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
    const MemoryCategoryScope memoryScope(MemoryCategory::AirVolumetrics);

    m_device = device;
    m_descriptorHeap = &descriptorHeap;
    // AIR.4E.3: no pipelines or storage until a diagnostic first uses the solver.
    m_pressure.Initialize(device, descriptorHeap, shaderLibrary);
    const bool pipelines = MakePipeline(device, shaderLibrary, "AirField.hlsl", "CSInitializeVelocity", m_initializePipeline) &&
        MakePipeline(device, shaderLibrary, "AirField.hlsl", "CSBuildObstacles", m_obstaclePipeline) &&
        MakePipeline(device, shaderLibrary, "AirField.hlsl", "CSBuildPieces", m_piecePipeline) &&
        MakePipeline(device, shaderLibrary, "AirField.hlsl", "CSBuildApertures", m_aperturePipeline) &&
        MakePipeline(device, shaderLibrary, "AirMovingSolids.hlsl", "CSMovingSolidPieces", m_movingSolidPiecePipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMovingSolids.hlsl", "CSMovingSolidApertures", m_movingSolidAperturePipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMovingSolids.hlsl", "CSMovingSolidTopologyReset", m_movingSolidTopologyResetPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMovingSolids.hlsl", "CSMovingSolidRemapClassify", m_movingSolidClassifyPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMovingSolids.hlsl", "CSMovingSolidRelayDistance", m_movingSolidRelayDistancePipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMovingSolids.hlsl", "CSMovingSolidRelayFlow", m_movingSolidRelayFlowPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMovingSolids.hlsl", "CSMovingSolidRemapDistribute", m_movingSolidDistributePipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMovingSolids.hlsl", "CSBoundaryCorrectionClear", m_boundaryCorrectionClearPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMovingSolids.hlsl", "CSBoundaryCorrectionSums", m_boundaryCorrectionSumsPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMovingSolids.hlsl", "CSBoundaryVolumes", m_boundaryVolumesPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMovingSolids.hlsl", "CSBoundaryGather", m_boundaryGatherPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMovingSolids.hlsl", "CSWallTangential", m_wallTangentialPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMovingSolids.hlsl", "CSBoundaryClear", m_boundaryClearPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMovingSolids.hlsl", "CSBoundarySnapshot", m_boundarySnapshotPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMovingSolids.hlsl", "CSBoundaryToGeometric", m_boundaryToGeometricPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMovingSolids.hlsl", "CSBoundaryZero", m_boundaryZeroPipeline, true) &&
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
        MakePipeline(device, shaderLibrary, "AirMist.hlsl", "CSMistEmitterStatus", m_mistEmitterStatusPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMist.hlsl", "CSMistPointSum", m_mistPointSumPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMist.hlsl", "CSMistPointDeposit", m_mistPointDepositPipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMist.hlsl", "CSMistVolume", m_mistVolumePipeline, true) &&
        MakePipeline(device, shaderLibrary, "AirMistFlux.hlsl", "CSMistFluxOut", m_mistFluxOutPipeline) &&
        MakePipeline(device, shaderLibrary, "AirMistFlux.hlsl", "CSMistFluxUpdate", m_mistFluxUpdatePipeline) &&
        MakePipeline(device, shaderLibrary, "AirMistFlux.hlsl", "CSMistCourant", m_mistCourantPipeline) &&
        MakePipeline(device, shaderLibrary, "AirMistFlux.hlsl", "CSMistSubsteps", m_mistSubstepsPipeline);

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

        MemoryRegistry::TrackResource(slot.readback);
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

        MemoryRegistry::TrackResource(slot.readback);
    }

    m_initialized = true;
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

    // Allocated with the mist (AllocateMist) and retired with it (RetireMist), but outside MistBuffers().
    m_mistSubstepState.Shutdown();
    m_mistSubstepArguments.Shutdown();

    m_mistAllocated = false;
    m_mistBinding = {};
    m_pressure.Shutdown();

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
    m_mistPointSumPipeline.Shutdown();
    m_mistPointDepositPipeline.Shutdown();
    m_mistVolumePipeline.Shutdown();
    m_allocated = false;
    m_binding = {};
    m_emission.clear();
    m_initializePipeline.Shutdown();
    m_obstaclePipeline.Shutdown();
    m_movingSolidPiecePipeline.Shutdown();
    m_movingSolidAperturePipeline.Shutdown();
    m_movingSolidTopologyResetPipeline.Shutdown();
    m_truncationCounters.Shutdown();
    m_movingSolidClassifyPipeline.Shutdown();
    m_movingSolidRelayDistancePipeline.Shutdown();
    m_movingSolidRelayFlowPipeline.Shutdown();
    m_movingSolidDistributePipeline.Shutdown();
    m_remapLedger.Shutdown();
    m_remapPredicate.Shutdown();
    m_boundaryCorrectionClearPipeline.Shutdown();
    m_boundaryCorrectionSumsPipeline.Shutdown();
    m_boundaryVolumesPipeline.Shutdown();
    m_boundaryGatherPipeline.Shutdown();
    m_wallTangentialPipeline.Shutdown();
    m_boundaryClearPipeline.Shutdown();
    m_boundarySnapshotPipeline.Shutdown();
    m_boundaryToGeometricPipeline.Shutdown();
    m_boundaryZeroPipeline.Shutdown();
    m_boundaryRecords.Shutdown();
    m_closedSources.Shutdown();
    m_effectiveOffset.Shutdown();
    m_wallSource.Shutdown();
    m_boundaryCorrection.Shutdown();
    m_boundaryDiagnostics.Shutdown();
    m_piecePipeline.Shutdown();
    m_aperturePipeline.Shutdown();
    m_stepPipeline.Shutdown();
    m_splitStepPipeline.Shutdown();
    m_prepareSplitPipeline.Shutdown();
    m_resetSplitPipeline.Shutdown();
    m_splitSignature.Reset();
    m_visibilityPipeline.Shutdown();
    m_statusPipeline.Shutdown();
    m_samplesPipeline.Shutdown();
    m_mistOpenPipeline.Shutdown();
    m_mistFluxOutPipeline.Shutdown();
    m_mistFluxUpdatePipeline.Shutdown();
    m_mistCourantPipeline.Shutdown();
    m_mistSubstepsPipeline.Shutdown();
    m_mistSplitPipeline.Shutdown();
    m_mistPreparePipeline.Shutdown();
    m_mistClearPipeline.Shutdown();
}

bool AirField::Allocate(const AirFieldSettings& settings)
{
    const MemoryCategoryScope memoryScope(MemoryCategory::AirVolumetrics);

    const std::size_t cells = CellCount(settings);
    const auto create = [this](Buffer& buffer, std::size_t bytes)
    {
        return buffer.InitializeGPUWritable(m_device, bytes) && buffer.CreateRawSRV(m_device, *m_descriptorHeap) &&
            buffer.CreateRawUAV(m_device, *m_descriptorHeap);
    };

    const bool created = create(m_velocity[0], cells * VelocityBytesPerCell) && create(m_velocity[1], cells * VelocityBytesPerCell) &&
        create(m_piece1Velocity[0], cells * VelocityBytesPerCell) && create(m_piece1Velocity[1], cells * VelocityBytesPerCell) &&
        create(m_obstacles, cells / SlotsPerObstacleWord * sizeof(std::uint32_t)) && create(m_pieces, cells * PieceBytesPerCell) &&
        create(m_visibility, std::size_t{m_visibilityCapacity} * (cells / SlotsPerObstacleWord) * sizeof(std::uint32_t)) &&
        create(m_emissionStatus, MaximumMotors * StatusBytesPerMotor) &&
        create(m_emissionSamples, std::size_t{m_visibilityCapacity} * SampleBytesPerMotor) &&
        create(m_splitList, (cells + 1) * sizeof(std::uint32_t)) && create(m_splitArguments, SplitArgumentBytes) &&
        create(m_mistStatus, MaximumMistEmitters * MistStatusBytesPerEmitter) && create(m_apertures, cells * ApertureBytesPerCell);

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

bool AirField::EnsureVisibilityCapacity(std::size_t motors, std::uint64_t fence)
{
    const MemoryCategoryScope memoryScope(MemoryCategory::AirVolumetrics);

    const std::uint32_t needed = static_cast<std::uint32_t>((std::min)(motors, std::size_t{MaximumMotors}));

    if(needed <= m_visibilityCapacity)
    {
        return true;
    }

    std::uint32_t capacity = m_visibilityCapacity;

    while(capacity < needed)
    {
        capacity *= 2;
    }

    capacity = (std::min)(capacity, MaximumMotors);
    const std::size_t cells = CellCount(m_settings);
    Buffer visibility;
    Buffer samples;
    const auto create = [this](Buffer& buffer, std::size_t bytes)
    {
        return buffer.InitializeGPUWritable(m_device, bytes) && buffer.CreateRawSRV(m_device, *m_descriptorHeap) &&
            buffer.CreateRawUAV(m_device, *m_descriptorHeap);
    };

    if(!create(visibility, std::size_t{capacity} * (cells / SlotsPerObstacleWord) * sizeof(std::uint32_t)) ||
        !create(samples, std::size_t{capacity} * SampleBytesPerMotor))
    {
        visibility.Shutdown();
        samples.Shutdown();
        return false;
    }

    for(Buffer* buffer : {&m_visibility, &m_emissionSamples})
    {
        RetiredBuffer retired;
        retired.buffer = std::move(*buffer);
        retired.fence = fence;
        m_retired.push_back(std::move(retired));
    }

    m_visibility = std::move(visibility);
    m_emissionSamples = std::move(samples);
    m_visibilityCapacity = capacity;
    // The new storage holds no slot's bits.
    m_visibilityEntries = {};
    return true;
}

bool AirField::AllocateMist(const AirFieldSettings& settings)
{
    const MemoryCategoryScope memoryScope(MemoryCategory::AirVolumetrics);

    const std::size_t bytes = CellCount(settings) * MistBytesPerCell;
    bool created = true;

    for(Buffer* buffer : MistBuffers())
    {
        created = created && buffer->InitializeGPUWritable(m_device, bytes) && buffer->CreateRawSRV(m_device, *m_descriptorHeap) &&
            buffer->CreateRawUAV(m_device, *m_descriptorHeap);
    }

    // AIR.4F.2d.4 Stage 2b.1: the substep decision's small buffers (one float per 8^3 group of the window, three dispatch records).
    const std::size_t groups = std::size_t{(settings.dimensions[0] + 7) / 8} * ((settings.dimensions[1] + 7) / 8) * ((settings.dimensions[2] + 7) / 8);
    created = created && m_mistSubstepState.InitializeGPUWritable(m_device, MistSubstepHeaderBytes + groups * sizeof(float)) &&
        m_mistSubstepState.CreateRawUAV(m_device, *m_descriptorHeap) &&
        m_mistSubstepArguments.InitializeGPUWritable(m_device, MistSubstepArgumentBytes) &&
        m_mistSubstepArguments.CreateRawUAV(m_device, *m_descriptorHeap);

    if(!created)
    {
        // Never recorded into: released at once.
        for(Buffer* buffer : MistBuffers())
        {
            buffer->Shutdown();
        }

        m_mistSubstepState.Shutdown();
        m_mistSubstepArguments.Shutdown();
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
    for(Buffer* buffer : {&m_mist[0], &m_mist[1], &m_mistPiece1[0], &m_mistPiece1[1], &m_mistSubstepState, &m_mistSubstepArguments})
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

    // AIR.4F.3 F1a.2 / F1b / F2b.1: the overlay's counters, remap ledger, relay predicate and boundary buffers go with the field (the
    // effective-volume offset and wall source are sized by the window).
    m_boundaryReceivers.clear();
    m_wallSourceWritten = false;

    m_boundaryRecordBytes = 0;

    for(Buffer* buffer : {&m_truncationCounters, &m_remapLedger, &m_remapPredicate, &m_effectiveOffset, &m_wallSource, &m_boundaryCorrection,
        &m_boundaryDiagnostics, &m_boundaryRecords, &m_closedSources})
    {
        if(buffer->GetResource() != nullptr)
        {
            RetiredBuffer retired;
            retired.buffer = std::move(*buffer);
            retired.fence = fence;
            m_retired.push_back(std::move(retired));
        }
    }

    // AIR.4E.3: the pressure solver's storage describes this window; it goes with it.
    RetirePressure(fence);
    m_allocated = false;
    m_binding.valid = 0;
}

void AirField::Mark(ID3D12GraphicsCommandList* commandList, const char* stage) const
{
    if(m_stageMarker)
    {
        m_stageMarker(commandList, stage);
    }
}

void AirField::RetirePressure(std::uint64_t fence)
{
    for(Buffer* buffer : m_pressure.StorageBuffers())
    {
        if(buffer->GetResource() != nullptr)
        {
            RetiredBuffer retired;
            retired.buffer = std::move(*buffer);
            retired.fence = fence;
            m_retired.push_back(std::move(retired));
        }
    }

    // Storage allocated again later is rebuilt and starts from phi = 0: nothing of this one (warm start included) survives.
    m_pressure.ForgetStorage();
    m_pressureDiagnostics = {};
    m_projectionStorage = false;
    m_pressurePhiCompatible = false;
}

AirPressureBuoyancy AirField::PressureBuoyancy(const AirFrameRequest& request)
{
    AirPressureBuoyancy buoyancy;
    const double gravity = static_cast<double>(request.pressure.buoyancyGravity);

    if(std::isfinite(gravity) && gravity > 0.0)
    {
        buoyancy.impulse = static_cast<float>(gravity * StepSeconds);
    }

    // The ambient is validated by the scene (AirEnvironmentSettings); bounded here as well, so T0 + ΔT stays positive in the shader.
    const float reference = request.solver.ambientTemperature;
    buoyancy.referenceTemperature = std::isfinite(reference) ?
        std::clamp(reference, AirEnvironmentSettings::MinimumAmbientTemperature, AirEnvironmentSettings::MaximumAmbientTemperature) :
        AirEnvironmentSettings{}.ambientTemperature;
    return buoyancy;
}

bool AirField::RecordProjection(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next,
    const AirFrameRequest& request, bool lastStep, bool conservativeMist)
{
    // The step just written is the current buffer; the solver reads it, the pieces and the face connections through read-only views.
    AirFieldBinding current = next;
    current.velocityIndex = m_velocity[m_current].GetDescriptorIndex();
    current.velocityOtherIndex = m_velocity[1 - m_current].GetDescriptorIndex();
    current.velocityPiece1Index = m_piece1Velocity[m_current].GetDescriptorIndex();
    current.velocityPiece1OtherIndex = m_piece1Velocity[1 - m_current].GetDescriptorIndex();
    const std::uint32_t apertures = m_apertures.GetDescriptorIndex();
    const Vector3& ambient = request.settings.ambientVelocity;
    const AirPressureRequest& pressure = request.pressure;
    m_projectionStorage = true;
    TransitionStorage(commandList, ReadState);

    // Components and hierarchy follow the face connections: rebuilt (phi zeroed) whenever maintenance rebuilt those.
    // AIR.4E.5b: after nothing but recentres, phi is kept in the cells the previous build's window shares with this one.
    bool recorded = m_pressure.IsBuiltFor(m_apertureBuilds);
    const bool rebuilt = !recorded;
    // AIR.4F.3 F1a.3: after nothing but moving-solid updates since a build of this window (with warm pressure), the solver is updated
    // locally over their cells; it relabels on the GPU when its topology check cannot rule out a change, and always after several
    // updates (the check's record holds the latest only) or for a box past the check's capacity.
    bool local = false;

    const bool warmPressure = request.movingSolids.enabled && !request.movingSolids.coldRebuildForDiagnostics;

    if(!recorded && warmPressure && m_pressurePendingUpdates > 0 && m_pressurePhiCompatible &&
        m_pressure.IsBuiltFor(m_apertureBuilds - m_pressurePendingUpdates) && m_pressureBuildMinimum == m_minimum)
    {
        AirPressureLocalUpdate update;
        std::uint64_t cells = 1;

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            const std::int32_t last = static_cast<std::int32_t>(m_settings.dimensions[axis]) - 1;
            update.minimum[axis] = std::clamp(m_pressurePendingLow[axis], 0, last);
            update.maximum[axis] = std::clamp(m_pressurePendingHigh[axis], 0, last);
            cells *= static_cast<std::uint64_t>(update.maximum[axis] - update.minimum[axis] + 1);
        }

        update.forceRelabel = m_pressurePendingUpdates > 1 || cells > PressureTopologyCellLimit;
        m_movingSolidStatistics.forcedRelabelsPending += m_pressurePendingUpdates > 1 ? 1 : 0;
        m_movingSolidStatistics.forcedRelabelsSize += cells > PressureTopologyCellLimit ? 1 : 0;
        m_movingSolidStatistics.lastLocalBoxCells = cells;
        local = m_pressure.RecordIncrementalRebuild(commandList, *m_descriptorHeap, upload, current, apertures, m_apertureBuilds, update);
        recorded = local;
        m_movingSolidStatistics.localPressureRebuilds += local ? 1 : 0;
    }

    if(!recorded)
    {
        recorded = m_pressure.RecordRebuild(commandList, *m_descriptorHeap, upload, current, apertures, m_apertureBuilds,
            AirPressure::DefaultLabelRounds, m_pressurePhiCompatible ? &m_pressureBuildMinimum : nullptr);
        m_pressureBuildMinimum = m_minimum;
        m_pressurePhiCompatible = recorded;
        m_movingSolidStatistics.fullPressureRebuilds += recorded && m_pressurePendingUpdates > 0 ? 1 : 0;
    }

    if(rebuilt)
    {
        m_pressurePendingUpdates = 0;
    }

    // AIR.4F.1b: buoyancy enters the solve's right-hand side and the correction as one force per vertical face; both get the same values.
    const AirPressureBuoyancy buoyancy = PressureBuoyancy(request);
    const bool buoyant = buoyancy.impulse > 0.0f;

    // AIR.4E.5b: a cold start (just rebuilt, phi zeroed) gets the cold cycle budget. AIR.4F.1b: while buoyancy acts, warm steps get the
    // buoyant budget, and a recentre (rebuilt, phi kept) and the steps just after it the buoyant recentre budget.
    AirPressureSolveSettings solve = pressure.solve;
    // AIR A2/T1 (AirMovingSolidCycleBudgetT1.md): four kinds of step choose the budget: a steady step (not rebuilt), a local moving-
    // solid update (rebuilt locally, phi kept), a recentre (rebuilt over a moved window, phi kept where the windows overlap) and a cold
    // start (rebuilt, phi zeroed). A local update also preserves phi, but it is not a recentre: it keeps the warm (or buoyant warm)
    // budget with recovery, and never starts the buoyant recentre countdown. Treating it as a recentre ran 12 fixed cycles every step
    // while anything moved.
    const bool recentred = rebuilt && !local && m_pressure.LastRebuildPreserved();

    if(rebuilt && !recentred)
    {
        solve.cycles = (std::max)(solve.cycles, pressure.coldCycles);
    }

    m_pressureRecentreSteps = recentred ? pressure.buoyantRecentreSteps : m_pressureRecentreSteps;

    // AIR.4F.3 F1a.3: a local rebuild keeps phi and the warm budget, with recovery cycles (on the GPU, only while the residual is above the
    // criterion) up to the cold budget. Warm pressure also recovers after full rebuilds (recentres, rebakes).
    if(local)
    {
        solve.cycles = pressure.solve.cycles;
    }

    if(buoyant)
    {
        solve.cycles = (std::max)(solve.cycles, pressure.buoyantCycles);

        if(m_pressureRecentreSteps > 0)
        {
            solve.cycles = (std::max)(solve.cycles, pressure.buoyantRecentreCycles);
        }
    }

    // Recovery fills the budget up to the cold cycles, counted from the step's final (buoyant-raised) cycles. Counting from the warm cycles
    // let a buoyant recentre step record 12 + 9 cycles, whose constants, at two steps per frame with moving solids, exhausted the frame's
    // upload ring (the renderer's next allocations then failed and the frame shut the application down).
    if(warmPressure)
    {
        solve.recoveryCycles = solve.cycles < pressure.coldCycles ? pressure.coldCycles - solve.cycles : 0;
    }

    m_pressureRecentreSteps = m_pressureRecentreSteps > 0 ? m_pressureRecentreSteps - 1 : 0;

    // AIR O2.1: adaptive warm cycles. Beyond the minimum, the budget's cycles become gated ones ahead of any recovery cycles: each runs
    // only while the residual is above the criterion, so the solve stops at the same criterion it is budgeted for, never later than the
    // fixed solver and never with less budget. A cold start (rebuilt from phi = 0) keeps its fixed cycles: it is measured to need them.
    // Buoyant solves keep the fixed budgets too: phi carries a large hydrostatic part, and stopping each step at the criterion leaves the
    // next step a warm start too poor for the recentre budget to recover (measured: budget exhausted above the criterion after stratified
    // recentres; AirOptimizationInvestigation.md, O2.1).
    const bool coldStart = rebuilt && !recentred && !local;

    if(pressure.adaptiveMinimumCycles > 0 && !coldStart && !buoyant && solve.cycles > pressure.adaptiveMinimumCycles)
    {
        solve.recoveryCycles += solve.cycles - pressure.adaptiveMinimumCycles;
        solve.cycles = pressure.adaptiveMinimumCycles;
        solve.smoothingSweeps = pressure.adaptiveSmoothingSweeps;
    }
    // AIR.4F.3 F2b.2: with moving solids, the moving-wall source (D, sub-cell volumes per step) enters the right-hand side as a rate,
    // D (cell / 4)^3 / (cell^2 dt) = D cell / (64 dt) in the divergence's units; the closed components' sums are zeroed first.
    AirPressureWallSource wallSource;

    if(request.movingSolids.enabled && m_wallSource.GetResource() != nullptr)
    {
        wallSource.sourceIndex = m_wallSource.GetUAVDescriptorIndex();
        wallSource.scale = static_cast<float>(double(next.cellSize) / (64.0 * StepSeconds));

        if(m_closedSources.GetResource() != nullptr && RecordClosedSourceClear(commandList, upload, next))
        {
            wallSource.closedSourceIndex = m_closedSources.GetUAVDescriptorIndex();
        }
    }

    recorded = recorded && m_pressure.RecordSolve(commandList, *m_descriptorHeap, upload, current, apertures, ambient, solve, buoyancy, wallSource);
    const bool diagnostics = recorded && pressure.diagnostics && lastStep;

    if(diagnostics)
    {
        recorded = m_pressure.RecordResidual(commandList, *m_descriptorHeap, upload, current, apertures);
    }

    // AIR.4F.2d.4 Stage 2a: only here are the projection's face velocities exact: the velocity buffers still hold the step's provisional
    // velocity the right-hand side was built from, and phi is the solve's. CSCorrect overwrites that velocity next.
    m_mistTransportedByFlux = recorded && conservativeMist && RecordMistFlux(commandList, upload, current, apertures, request, buoyancy);

    // The correction writes the current velocity in place (xyz); everything else stays readable.
    m_velocity[m_current].Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_piece1Velocity[m_current].Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    recorded = recorded && m_pressure.RecordCorrect(commandList, *m_descriptorHeap, upload, current, apertures,
        m_velocity[m_current].GetUAVDescriptorIndex(), m_piece1Velocity[m_current].GetUAVDescriptorIndex(), buoyancy);

    if(recorded && diagnostics)
    {
        m_pressureDiagnostics = m_pressure.PublishDiagnostics(commandList);
    }

    // The mist step (and any further step) works on writable storage, as before.
    TransitionStorage(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ++m_projections;
    return recorded;
}

bool AirField::RecordMistFlux(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& current,
    std::uint32_t apertures, const AirFrameRequest& request, const AirPressureBuoyancy& buoyancy)
{
    AirMistFluxConstants constants;
    constants.field = current;
    constants.apertureIndex = apertures;
    constants.phiIndex = m_pressure.GetPhiUnorderedIndex();
    constants.mistCurrent[0] = m_mist[m_mistCurrent].GetUAVDescriptorIndex();
    constants.mistCurrent[1] = m_mistPiece1[m_mistCurrent].GetUAVDescriptorIndex();
    // The spare buffers: the ones the advective step would write. Only the current ones are ever published, so between steps the spare
    // ones hold nothing anyone reads.
    constants.mistOutgoing[0] = m_mist[1 - m_mistCurrent].GetUAVDescriptorIndex();
    constants.mistOutgoing[1] = m_mistPiece1[1 - m_mistCurrent].GetUAVDescriptorIndex();
    constants.timeStep = static_cast<float>(StepSeconds);
    const double rate = std::isfinite(request.mist.dissipationRate) ? (std::max)(0.0, static_cast<double>(request.mist.dissipationRate)) : 0.0;
    constants.dissipationFactor = static_cast<float>(std::exp(-rate * StepSeconds));
    constants.ambient[0] = request.settings.ambientVelocity.x;
    constants.ambient[1] = request.settings.ambientVelocity.y;
    constants.ambient[2] = request.settings.ambientVelocity.z;
    constants.buoyancyImpulse = buoyancy.impulse;
    constants.referenceTemperature = buoyancy.referenceTemperature;
    constants.substepStateIndex = m_mistSubstepState.GetUAVDescriptorIndex();
    constants.forcedSubsteps = (std::min)(request.mist.forcedSubsteps, MaximumMistSubsteps);
    constants.substepArgumentsIndex = m_mistSubstepArguments.GetUAVDescriptorIndex();

    // AIR.4F.3 F2b.2: with moving solids, the control volumes (S, D) the latest update anchored.
    if(request.movingSolids.enabled && m_effectiveOffset.GetResource() != nullptr &&
        request.movingSolids.couplingForDiagnostics != AirMovingSolidRequest::Coupling::Naive)
    {
        constants.effectiveOffsetIndex = m_effectiveOffset.GetUAVDescriptorIndex();
        constants.wallSourceIndex = m_wallSource.GetUAVDescriptorIndex();
    }

    // One constant block per substep (0: the Courant and decision passes).
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, MaximumMistSubsteps + 1> addresses{};

    for(std::uint32_t substep = 0; substep <= MaximumMistSubsteps; ++substep)
    {
        constants.substep = substep;
        addresses[substep] = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(addresses[substep] == 0)
        {
            return false;
        }
    }

    // The field's storage is in its shader-read state (velocity, pieces, apertures). The mist buffers and the substep state become
    // writable, and the solve's last writes to phi complete first.
    for(Buffer* buffer : MistBuffers())
    {
        buffer->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    m_mistSubstepState.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_mistSubstepArguments.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    UnorderedAccessBarrier(commandList);

    // Stage 2b.1: this step's largest Courant number, its substep count and the optional substeps' dispatch records, before any transport.
    m_mistCourantPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, addresses[0]);
    DispatchCells(commandList, m_settings);
    UnorderedAccessBarrier(commandList);
    m_mistSubstepsPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, addresses[0]);
    commandList->Dispatch(1, 1, 1);
    UnorderedAccessBarrier(commandList);
    m_mistSubstepArguments.Transition(commandList, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    Mark(commandList, "conservative mist: substep count");

    // Substep 1 always; 2 to 4 from the decision's records (empty beyond the count). Every pass reads the count and dt / N from the state.
    for(std::uint32_t substep = 1; substep <= MaximumMistSubsteps; ++substep)
    {
        for(ComputePipeline* pipeline : {&m_mistFluxOutPipeline, &m_mistFluxUpdatePipeline})
        {
            pipeline->Bind(commandList);
            commandList->SetComputeRootConstantBufferView(0, addresses[substep]);

            if(substep == 1)
            {
                DispatchCells(commandList, m_settings);
            }
            else
            {
                const UINT64 record = static_cast<UINT64>(substep - 2) * sizeof(D3D12_DISPATCH_ARGUMENTS);
                commandList->ExecuteIndirect(m_splitSignature.Get(), 1, m_mistSubstepArguments.GetResource(), record, nullptr, 0);
            }

            UnorderedAccessBarrier(commandList);
        }

        Mark(commandList, substep == 1 ? "conservative mist: substep 1" : "conservative mist: substeps 2 to 4");
    }

    m_mistSubstepArguments.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // CSCorrect expects the field's other storage in its shader-read state.
    for(Buffer* buffer : MistBuffers())
    {
        buffer->Transition(commandList, ReadState);
    }

    ++m_conservativeMistSteps;
    return true;
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

    // AIR.4E.5b.1: the request's limit, 1 or 2; the accumulator's rules are the same for either.
    const std::uint32_t limit = std::clamp(request.maximumStepsPerFrame, 1u, MaximumStepsPerFrame);
    m_accumulator += request.deltaSeconds;
    // AIR.4F.3 F2b.2a: the same boundary tolerance as physics' fixed step (PhysicsSimulationController::ConsumeDueTicks), so frame times
    // that sum to a whole step (two 1/120 s frames) take it on the same frame at every frame rate instead of rounding just below it.
    constexpr double BoundaryToleranceSteps = 1.0e-9;
    const double due = std::floor(m_accumulator / StepSeconds + BoundaryToleranceSteps);
    const std::uint32_t steps = due >= limit ? limit : static_cast<std::uint32_t>(due);
    m_accumulator = (std::max)(m_accumulator - steps * StepSeconds, 0.0);

    // A long frame: run the maximum and drop the rest, so the simulation never falls further and further behind.
    if(due > limit)
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
    constants.apertureUnordered = m_apertures.GetUAVDescriptorIndex();

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

    // AIR.4E.1: every slot's face connections, from the pieces just written (a record also depends on its neighbours' pieces, so all
    // are rebuilt). Maintenance runs only when the window moved, the field was reset or the voxel data changed, never per step.
    m_aperturePipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    DispatchCells(commandList, m_settings);
    UnorderedAccessBarrier(commandList);
    ++m_apertureBuilds;

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

bool AirField::RecordMovingSolidUpdate(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next,
    const VoxelWorldBinding& voxels, const std::map<std::string, AirMovingSolid, std::less<>>& previous,
    const std::map<std::string, AirMovingSolid, std::less<>>& target, bool restampAll, bool warmPressure, bool continuousStep)
{
    // The cells each changed solid covered before or covers now, as boxes of world cells clipped to the window.
    struct CellBox
    {
        std::array<std::int32_t, 3> minimum;
        std::array<std::int32_t, 3> maximum;
    };

    std::vector<CellBox> boxes;
    const double cellSize = next.cellSize;

    const auto addBox = [&](const AirMovingSolid& solid)
    {
        const GpuAirMovingSolid gpu = ToGpuMovingSolid(solid);
        CellBox box;

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            const std::int32_t windowLow = next.minimum[axis];
            const std::int32_t windowHigh = next.minimum[axis] + static_cast<std::int32_t>(next.dimensions[axis]) - 1;
            const double low = std::floor(gpu.boundsMinimum[axis] / cellSize);
            const double high = std::floor(gpu.boundsMaximum[axis] / cellSize);
            box.minimum[axis] = static_cast<std::int32_t>(std::clamp(low, double(windowLow), double(windowHigh) + 1.0));
            box.maximum[axis] = static_cast<std::int32_t>(std::clamp(high, double(windowLow) - 1.0, double(windowHigh)));

            if(box.minimum[axis] > box.maximum[axis])
            {
                return;
            }
        }

        boxes.push_back(box);
    };

    for(const auto& [objectID, solid] : previous)
    {
        const auto now = target.find(objectID);

        if(restampAll || now == target.end() || !SameMovingSolidGeometry(solid, now->second))
        {
            addBox(solid);
        }
    }

    for(const auto& [objectID, solid] : target)
    {
        const auto before = previous.find(objectID);

        if(restampAll || before == previous.end() || !SameMovingSolidGeometry(before->second, solid))
        {
            addBox(solid);
        }
    }

    if(boxes.empty())
    {
        return true;
    }

    // F1a.2: whether this change may alter connectivity in a way the CPU can see: a solid appearing, leaving or jumping (a new motion
    // epoch), or two solids' cells (grown by one) meeting. A solid against static geometry is not visible here (F1a.3's detection).
    bool connectivityRisk = restampAll;

    for(const auto& [objectID, solid] : target)
    {
        const auto before = previous.find(objectID);
        connectivityRisk = connectivityRisk || before == previous.end() || before->second.motionEpoch != solid.motionEpoch;
    }

    for(const auto& [objectID, solid] : previous)
    {
        connectivityRisk = connectivityRisk || !target.contains(objectID);
    }

    if(!connectivityRisk && target.size() > 1)
    {
        std::vector<GpuAirMovingSolid> placed;

        for(const auto& [objectID, solid] : target)
        {
            placed.push_back(ToGpuMovingSolid(solid));
        }

        for(std::size_t first = 0; first < placed.size() && !connectivityRisk; ++first)
        {
            for(std::size_t second = first + 1; second < placed.size() && !connectivityRisk; ++second)
            {
                bool meet = true;

                for(std::size_t axis = 0; axis < 3; ++axis)
                {
                    meet = meet && placed[first].boundsMinimum[axis] - cellSize <= placed[second].boundsMaximum[axis] &&
                        placed[second].boundsMinimum[axis] - cellSize <= placed[first].boundsMaximum[axis];
                }

                connectivityRisk = meet;
            }
        }
    }

    // F1a.2: the truncation counters, created on first use (zeroed by creation; never in flight before this).
    if(m_truncationCounters.GetResource() == nullptr &&
        (!m_truncationCounters.InitializeGPUWritable(m_device, 32) || !m_truncationCounters.CreateRawUAV(m_device, *m_descriptorHeap)))
    {
        m_truncationCounters.Shutdown();
    }

    // F1b: the remap ledger and the relay predicate, the same way. The remap writes both, so without them nothing is recorded (as for a
    // full upload ring): the geometry and state stay as they are, and the next update retries.
    // F2b.1: the boundary buffers, the same way (the effective-volume offset and wall source zeroed by creation: geometric everywhere).
    const std::size_t slotPairBytes = static_cast<std::size_t>(CellCount(m_settings)) * 8;
    const std::pair<Buffer*, std::size_t> created[] = {{&m_remapLedger, RemapLedgerBytes}, {&m_remapPredicate, 8},
        {&m_effectiveOffset, slotPairBytes}, {&m_wallSource, slotPairBytes},
        {&m_boundaryCorrection, BoundaryCorrectionStride * MaximumMovingSolids * 2}, {&m_boundaryDiagnostics, BoundaryDiagnosticsBytes},
        {&m_closedSources, std::size_t{16} * AirPressure::ClosedComponentCapacity}};

    for(const auto& [buffer, bytes] : created)
    {
        if(buffer->GetResource() == nullptr &&
            (!buffer->InitializeGPUWritable(m_device, bytes) || !buffer->CreateRawUAV(m_device, *m_descriptorHeap)))
        {
            buffer->Shutdown();
            return false;
        }
    }

    // F1b: whether some orphan region may touch no fluid, so the relay must be recorded (it then runs on the GPU only if one does): a
    // restamp after maintenance (the whole solid is new to the pieces), a solid appearing or in a new motion epoch, or one whose surface
    // moved more than a sub-cell. Below that, every sub-cell a solid newly covers borders the fluid ahead of it, except where a solid is
    // pinned against static geometry, which the ledger reports as unresolved.
    const double subcellSize = cellSize / 4.0;
    bool relayRisk = restampAll;

    for(const auto& [objectID, solid] : target)
    {
        const auto before = previous.find(objectID);
        const bool jumped = before == previous.end() || before->second.motionEpoch != solid.motionEpoch;
        relayRisk = relayRisk || jumped || MovingSolidReach(before->second, solid) > subcellSize;
    }

    // F1a.2: phi is carried onto rebuilt pieces only when it will be kept. F1a.3: with a built solver, the pieces pass also records the
    // replaced pieces' codes and the volume change for the local topology check.
    const bool carryPhi = warmPressure && m_pressure.HasStorage();
    const bool recordTopology = carryPhi && m_pressure.IsBuiltFor(m_apertureBuilds - m_pressurePendingUpdates);

    if(carryPhi)
    {
        m_pressure.PreparePhiWrite(commandList);
    }

    // The face connections read a slot's own pieces and its +x, +y, +z neighbours': the slots one cell below a dirty cell also change.
    std::vector<CellBox> apertureBoxes = boxes;

    for(CellBox& box : apertureBoxes)
    {
        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            box.minimum[axis] = (std::max)(box.minimum[axis] - 1, next.minimum[axis]);
        }
    }

    const auto cellsOf = [](const std::vector<CellBox>& list)
    {
        std::uint64_t cells = 0;

        for(const CellBox& box : list)
        {
            cells += std::uint64_t(box.maximum[0] - box.minimum[0] + 1) * std::uint64_t(box.maximum[1] - box.minimum[1] + 1) *
                std::uint64_t(box.maximum[2] - box.minimum[2] + 1);
        }

        return cells;
    };

    // Too many boxes, or too much of the window: one box over the whole window (the same passes, including the remap: cells whose pieces
    // did not change keep their state and hold no orphans).
    const std::uint64_t windowCells = CellCount(m_settings);
    const bool fallback = apertureBoxes.size() > MovingSolidBoxCapacity || cellsOf(apertureBoxes) * MovingSolidFallbackDivisor > windowCells;

    if(fallback)
    {
        CellBox whole;

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            whole.minimum[axis] = next.minimum[axis];
            whole.maximum[axis] = next.minimum[axis] + static_cast<std::int32_t>(next.dimensions[axis]) - 1;
        }

        boxes = {whole};
        apertureBoxes = {whole};
        relayRisk = true;
    }

    // F1b: the receivers of displaced mist and the neighbours of exposed pieces: the dirty boxes grown by one each way, in the window.
    std::vector<CellBox> receiverBoxes = boxes;

    for(CellBox& box : receiverBoxes)
    {
        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            const std::int32_t windowHigh = next.minimum[axis] + static_cast<std::int32_t>(next.dimensions[axis]) - 1;
            box.minimum[axis] = (std::max)(box.minimum[axis] - 1, next.minimum[axis]);
            box.maximum[axis] = (std::min)(box.maximum[axis] + 1, windowHigh);
        }
    }

    // F2b.1: the boundary passes' cells: the dirty boxes grown by two. A loose group a cell's pieces do not reach belongs to its neighbours'
    // pieces in proportion to its contacts with all of them, so a change of pieces in a dirty cell alters the shares a cell one step out
    // gives to a cell two steps out.
    std::vector<CellBox> boundaryBoxes = boxes;

    for(CellBox& box : boundaryBoxes)
    {
        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            const std::int32_t windowHigh = next.minimum[axis] + static_cast<std::int32_t>(next.dimensions[axis]) - 1;
            box.minimum[axis] = (std::max)(box.minimum[axis] - 2, next.minimum[axis]);
            box.maximum[axis] = (std::min)(box.maximum[axis] + 2, windowHigh);
        }
    }

    // F1b: the scratch is the non-current half of each velocity and mist pair. Nothing reads it before the next step overwrites every slot
    // of it (CSStepOpen/CSStepSplit write all velocity; the advective step or CSMistFluxOut all mist), no shader reads the binding's
    // velocityOtherIndex, and the mist binding names the current buffers only. Mist is remapped (and relayed) only while allocated.
    const bool remapMist = m_mistAllocated;
    const bool recordRelay = remapMist && relayRisk;

    // Every solid of the target joins the union, changed or not: a dirty cell may also be covered by an unchanged solid.
    std::vector<GpuAirMovingSolid> solids;
    solids.reserve((std::max)(target.size() * 2, std::size_t{1}));

    for(const auto& [objectID, solid] : target)
    {
        solids.push_back(ToGpuMovingSolid(solid));
    }

    const std::uint32_t solidCount = static_cast<std::uint32_t>(solids.size());

    // F2b.1: each solid's classification. A continuous solid has the same motion epoch as its applied pose, a projected step following, and
    // a surface motion bound within one cell. F2b.2: the update is continuous only when every changed solid is (and no solid left): then
    // every solid is anchored at its applied pose, the wall source is the control-volume change between the two poses AIR applies, and the
    // mist follows the control volumes. Otherwise every solid is anchored at its new pose: no source, and F1b's geometric remap (a mixed
    // update is discontinuous as a whole, so no transition is both remapped geometrically and swept by a wall).
    bool continuousUpdate = continuousStep && !restampAll && m_couplingForDiagnostics != AirMovingSolidRequest::Coupling::NoWallSource;

    for(const auto& [objectID, solid] : previous)
    {
        continuousUpdate = continuousUpdate && target.contains(objectID);
    }

    for(const auto& [objectID, solid] : target)
    {
        const auto before = previous.find(objectID);
        const bool known = !restampAll && before != previous.end() && before->second.motionEpoch == solid.motionEpoch;
        const bool withinReach = known && MovingSolidReach(before->second, solid) <= cellSize;
        const bool continuous = known && continuousStep && withinReach;
        continuousUpdate = continuousUpdate && continuous;

        if(known && SameMovingSolidGeometry(before->second, solid))
        {
            continue;
        }

        m_movingSolidStatistics.continuousSolidUpdates += continuous ? 1 : 0;
        m_movingSolidStatistics.jumpedNewOrEpoch += known ? 0 : 1;
        m_movingSolidStatistics.jumpedNoStep += known && !continuousStep ? 1 : 0;
        m_movingSolidStatistics.jumpedReach += known && continuousStep && !withinReach ? 1 : 0;
    }

    for(const auto& [objectID, solid] : target)
    {
        const auto before = previous.find(objectID);
        solids.push_back(ToGpuMovingSolid(continuousUpdate ? before->second : solid));
    }

    // F2b.1: exact volumes (sub-cell volumes) and isolation per pose list: a solid whose bounds, grown by a sub-cell, meet no other solid's
    // bounds of its list never shares a sub-cell with another, so its own coverage can be corrected to its exact volume. Solids that may
    // overlap keep their raw coverage: the union's exact volume is not available, and the sum of theirs would be wrong.
    const double subcellVolume = subcellSize * subcellSize * subcellSize;
    std::vector<std::array<std::int32_t, 7>> correctionBoxes;

    for(std::uint32_t list = 0; list < 2; ++list)
    {
        const std::uint32_t first = list * solidCount;

        for(std::uint32_t index = first; index < first + solidCount; ++index)
        {
            GpuAirMovingSolid& solid = solids[index];
            const double radius = solid.radius;
            double volume = 0.0;

            if(solid.type == static_cast<std::uint32_t>(AirMovingSolidShape::Box))
            {
                volume = 8.0 * double(solid.halfExtents[0]) * double(solid.halfExtents[1]) * double(solid.halfExtents[2]);
            }
            else if(solid.type == static_cast<std::uint32_t>(AirMovingSolidShape::Sphere))
            {
                volume = 4.0 / 3.0 * std::numbers::pi * radius * radius * radius;
            }
            else
            {
                double segment = 0.0;

                for(std::size_t axis = 0; axis < 3; ++axis)
                {
                    const double delta = double(solid.segmentB[axis]) - double(solid.segmentA[axis]);
                    segment += delta * delta;
                }

                volume = std::numbers::pi * radius * radius * (4.0 / 3.0 * radius + std::sqrt(segment));
            }

            solid.exactVolume = static_cast<float>(volume / subcellVolume);
            bool isolated = true;

            for(std::uint32_t other = first; other < first + solidCount && isolated; ++other)
            {
                bool meet = other != index;

                for(std::size_t axis = 0; axis < 3 && meet; ++axis)
                {
                    meet = solid.boundsMinimum[axis] - subcellSize <= solids[other].boundsMaximum[axis] &&
                        solids[other].boundsMinimum[axis] - subcellSize <= solid.boundsMaximum[axis];
                }

                isolated = !meet;
            }

            solid.boundaryFlags = isolated ? 1u : 0u;
            m_movingSolidStatistics.isolatedSolids += isolated ? 1 : 0;
            m_movingSolidStatistics.uncorrectedSolids += isolated ? 0 : 1;

            // The sub-cells the correction sums cover: the solid's bounds (inclusive sub-cell indices), and the solid's entry.
            if(isolated)
            {
                std::array<std::int32_t, 7> box{};

                for(std::size_t axis = 0; axis < 3; ++axis)
                {
                    box[axis] = static_cast<std::int32_t>(std::floor(double(solid.boundsMinimum[axis]) / subcellSize));
                    box[axis + 3] = static_cast<std::int32_t>(std::floor(double(solid.boundsMaximum[axis]) / subcellSize));
                }

                box[6] = static_cast<std::int32_t>(index);
                correctionBoxes.push_back(box);
            }
        }
    }

    // A root view must name memory even when no solid is left.
    if(solids.empty())
    {
        solids.emplace_back();
    }

    const D3D12_GPU_VIRTUAL_ADDRESS solidAddress = upload.Allocate(solids.data(), solids.size() * sizeof(GpuAirMovingSolid), 256);

    // F2b.2: the boundary cells' records (CSBoundarySnapshot, CSBoundaryVolumes, CSBoundaryGather), grown to this update's need. Past
    // BoundaryRecordCapacity cells (the whole-window fallback) the boundary passes are skipped: the update is discontinuous and the
    // effective volumes revert to geometric (counted).
    std::uint64_t boundaryCells = 0;

    for(const CellBox& box : boundaryBoxes)
    {
        boundaryCells += std::uint64_t(box.maximum[0] - box.minimum[0] + 1) * std::uint64_t(box.maximum[1] - box.minimum[1] + 1) *
            std::uint64_t(box.maximum[2] - box.minimum[2] + 1);
    }

    // The records grow only before their first use in a frame (an earlier update of this frame may already have recorded work on them);
    // a later update that needs more takes the overflow path this frame, and the next frame grows them.
    const bool needsGrowth = boundaryCells * BoundaryRecordStride > m_boundaryRecordBytes;
    const bool boundaryOverflow = boundaryCells > BoundaryRecordCapacity || (needsGrowth && m_boundaryRecordsUsed);
    continuousUpdate = continuousUpdate && !boundaryOverflow;
    // The control-volume remap (production); a diagnostic naive update keeps the wall source but remaps mist geometrically.
    const bool naiveUpdate = continuousUpdate && m_couplingForDiagnostics == AirMovingSolidRequest::Coupling::Naive;
    const bool coupledRemap = continuousUpdate && !naiveUpdate;

    if(!boundaryOverflow && needsGrowth)
    {
        std::uint64_t bytes = 64 * BoundaryRecordStride;

        while(bytes < boundaryCells * BoundaryRecordStride)
        {
            bytes *= 2;
        }

        if(m_boundaryRecords.GetResource() != nullptr)
        {
            RetiredBuffer retired;
            retired.buffer = std::move(m_boundaryRecords);
            retired.fence = m_frameSubmissionFence;
            m_retired.push_back(std::move(retired));
        }

        if(!m_boundaryRecords.InitializeGPUWritable(m_device, static_cast<std::size_t>(bytes)) ||
            !m_boundaryRecords.CreateRawUAV(m_device, *m_descriptorHeap))
        {
            m_boundaryRecords.Shutdown();
            m_boundaryRecordBytes = 0;
            return false;
        }

        m_boundaryRecordBytes = bytes;
    }

    const auto constantsFor = [&](const std::vector<CellBox>& list)
    {
        AirMovingSolidConstants constants;
        constants.voxels = voxels;
        constants.field = next;
        constants.voxelLevel = m_settings.cellExponent - voxels.baseExponent;
        constants.pieceUnordered = m_pieces.GetUAVDescriptorIndex();
        constants.apertureUnordered = m_apertures.GetUAVDescriptorIndex();
        constants.solidCount = solidCount;
        constants.obstacleUnordered = m_obstacles.GetUAVDescriptorIndex();
        constants.phiUnordered = carryPhi ? m_pressure.GetPhiUnorderedIndex() : UINT32_MAX;
        constants.truncationUnordered = m_truncationCounters.GetResource() != nullptr ? m_truncationCounters.GetUAVDescriptorIndex() : UINT32_MAX;
        constants.componentUnordered = recordTopology ? m_pressure.GetComponentUnorderedIndex() : UINT32_MAX;
        constants.topologyUnordered = recordTopology ? m_pressure.GetTopologyUnorderedIndex() : UINT32_MAX;
        constants.velocityUnordered[0] = m_velocity[m_current].GetUAVDescriptorIndex();
        constants.velocityUnordered[1] = m_piece1Velocity[m_current].GetUAVDescriptorIndex();
        constants.scratchMasks = m_velocity[1 - m_current].GetUAVDescriptorIndex();
        constants.scratchState = m_piece1Velocity[1 - m_current].GetUAVDescriptorIndex();
        constants.ledgerUnordered = m_remapLedger.GetUAVDescriptorIndex();
        constants.predicateUnordered = m_remapPredicate.GetUAVDescriptorIndex();
        constants.relayParity = recordRelay ? MovingSolidRelayLevels % 2 : 0;

        if(remapMist)
        {
            constants.mistUnordered[0] = m_mist[m_mistCurrent].GetUAVDescriptorIndex();
            constants.mistUnordered[1] = m_mistPiece1[m_mistCurrent].GetUAVDescriptorIndex();
            constants.scratchAmount[0] = m_mist[1 - m_mistCurrent].GetUAVDescriptorIndex();
            constants.scratchAmount[1] = m_mistPiece1[1 - m_mistCurrent].GetUAVDescriptorIndex();
        }

        constants.donorBoxCount = static_cast<std::uint32_t>(boxes.size());
        constants.anchorOffset = solidCount;
        constants.anchorCount = solidCount;
        constants.boundaryEnabled = 1;
        constants.effectiveOffsetUnordered = m_effectiveOffset.GetUAVDescriptorIndex();
        constants.wallSourceUnordered = m_wallSource.GetUAVDescriptorIndex();
        constants.correctionUnordered = m_boundaryCorrection.GetUAVDescriptorIndex();
        constants.boundaryDiagnosticsUnordered = m_boundaryDiagnostics.GetUAVDescriptorIndex();
        constants.recordUnordered = m_boundaryRecords.GetResource() != nullptr ? m_boundaryRecords.GetUAVDescriptorIndex() : UINT32_MAX;
        constants.continuousRemap = coupledRemap ? 1u : 0u;
        constants.naiveCoupling = naiveUpdate ? 1u : 0u;

        for(std::size_t index = 0; index < boxes.size(); ++index)
        {
            for(std::size_t axis = 0; axis < 3; ++axis)
            {
                constants.donorMinimum[index][axis] = boxes[index].minimum[axis];
                constants.donorSize[index][axis] = boxes[index].maximum[axis] - boxes[index].minimum[axis] + 1;
            }
        }

        constants.boxCount = static_cast<std::uint32_t>(list.size());
        std::uint32_t first = 0;

        for(std::size_t index = 0; index < list.size(); ++index)
        {
            const CellBox& box = list[index];
            std::uint32_t count = 1;

            for(std::size_t axis = 0; axis < 3; ++axis)
            {
                const std::int32_t size = box.maximum[axis] - box.minimum[axis] + 1;
                constants.boxMinimum[index][axis] = box.minimum[axis];
                constants.boxSize[index][axis] = size;
                count *= static_cast<std::uint32_t>(size);
            }

            constants.boxMinimum[index][3] = static_cast<std::int32_t>(first);
            constants.boxSize[index][3] = static_cast<std::int32_t>(count);
            first += count;
        }

        constants.totalCells = first;
        return constants;
    };

    const AirMovingSolidConstants pieceConstants = constantsFor(boxes);
    const AirMovingSolidConstants apertureConstants = constantsFor(apertureBoxes);
    const D3D12_GPU_VIRTUAL_ADDRESS pieceAddress = upload.Allocate(&pieceConstants, sizeof(pieceConstants),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    const D3D12_GPU_VIRTUAL_ADDRESS apertureAddress = upload.Allocate(&apertureConstants, sizeof(apertureConstants),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    const AirMovingSolidConstants receiverConstants = constantsFor(receiverBoxes);
    const D3D12_GPU_VIRTUAL_ADDRESS receiverAddress = upload.Allocate(&receiverConstants, sizeof(receiverConstants),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    const AirMovingSolidConstants boundaryConstants = constantsFor(boundaryBoxes);
    const D3D12_GPU_VIRTUAL_ADDRESS boundaryAddress = upload.Allocate(&boundaryConstants, sizeof(boundaryConstants),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    // F2b.2: F1b's push. Discontinuous: over the dirty cells (its orphans), receivers grown by one. Continuous: over the boundary cells
    // (the fluid no new region owns can be in any of them), receivers grown by one more.
    AirMovingSolidConstants remapConstants = pieceConstants;
    AirMovingSolidConstants distributeConstants = receiverConstants;

    if(coupledRemap)
    {
        std::vector<CellBox> boundaryReceiverBoxes = boundaryBoxes;

        for(CellBox& box : boundaryReceiverBoxes)
        {
            for(std::size_t axis = 0; axis < 3; ++axis)
            {
                const std::int32_t windowHigh = next.minimum[axis] + static_cast<std::int32_t>(next.dimensions[axis]) - 1;
                box.minimum[axis] = (std::max)(box.minimum[axis] - 1, next.minimum[axis]);
                box.maximum[axis] = (std::min)(box.maximum[axis] + 1, windowHigh);
            }
        }

        remapConstants = boundaryConstants;
        distributeConstants = constantsFor(boundaryReceiverBoxes);

        for(AirMovingSolidConstants* constants : {&remapConstants, &distributeConstants})
        {
            constants->donorBoxCount = boundaryConstants.boxCount;

            for(std::size_t index = 0; index < boundaryBoxes.size(); ++index)
            {
                for(std::size_t axis = 0; axis < 3; ++axis)
                {
                    constants->donorMinimum[index][axis] = boundaryConstants.boxMinimum[index][axis];
                    constants->donorSize[index][axis] = boundaryConstants.boxSize[index][axis];
                }
            }
        }
    }

    const D3D12_GPU_VIRTUAL_ADDRESS remapAddress = upload.Allocate(&remapConstants, sizeof(remapConstants),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    const D3D12_GPU_VIRTUAL_ADDRESS distributeAddress = upload.Allocate(&distributeConstants, sizeof(distributeConstants),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    bool allocated = solidAddress != 0 && pieceAddress != 0 && apertureAddress != 0 && receiverAddress != 0 && boundaryAddress != 0 &&
        remapAddress != 0 && distributeAddress != 0;

    // F1b relay: the distance rounds 0 to MovingSolidRelayLevels - 1, then the flow levels MovingSolidRelayLevels down to 1, each with its
    // own constants. Everything is allocated before anything is recorded, so a full ring records nothing.
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> distanceAddresses;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> flowAddresses;

    for(std::uint32_t round = 0; recordRelay && allocated && round < MovingSolidRelayLevels; ++round)
    {
        AirMovingSolidConstants relayConstants = remapConstants;
        relayConstants.relayLevel = round;
        distanceAddresses.push_back(upload.Allocate(&relayConstants, sizeof(relayConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT));
        relayConstants.relayLevel = MovingSolidRelayLevels - round;
        flowAddresses.push_back(upload.Allocate(&relayConstants, sizeof(relayConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT));
        allocated = distanceAddresses.back() != 0 && flowAddresses.back() != 0;
    }

    if(!allocated)
    {
        return false;
    }

    // Everything before (maintenance, the previous step) has finished with the pieces before they change; the face connections read
    // every piece this update wrote; the next step reads both. F1a.3: the topology words are cleared before the pieces pass records.
    UnorderedAccessBarrier(commandList);

    const auto dispatchRemap = [&](ComputePipeline& pipeline, D3D12_GPU_VIRTUAL_ADDRESS address, std::uint32_t cells)
    {
        pipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, address);
        commandList->SetComputeRootShaderResourceView(1, solidAddress);
        commandList->Dispatch((cells + 63) / 64, 1, 1);
    };

    if(recordTopology)
    {
        m_movingSolidTopologyResetPipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, pieceAddress);
        commandList->SetComputeRootShaderResourceView(1, solidAddress);
        commandList->Dispatch(1, 1, 1);
        UnorderedAccessBarrier(commandList);
    }

    // F2b.2, before the pieces change (both read the old pieces and densities): a continuous update snapshots each boundary cell's old
    // pieces and old unreached density; any other converts the boundary cells' densities into F1b's per-sub-cell units.
    if(coupledRemap)
    {
        dispatchRemap(m_boundarySnapshotPipeline, boundaryAddress, boundaryConstants.totalCells);
        UnorderedAccessBarrier(commandList);
    }
    else if(boundaryOverflow)
    {
        // Too many cells for the records: geometric volumes everywhere, every density converted first (cells outside the boxes may hold
        // an unchanged solid's offsets).
        RecordBoundaryClear(commandList, upload, next, true);
    }
    else if(remapMist)
    {
        dispatchRemap(m_boundaryToGeometricPipeline, boundaryAddress, boundaryConstants.totalCells);
        UnorderedAccessBarrier(commandList);
    }

    m_movingSolidPiecePipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, pieceAddress);
    commandList->SetComputeRootShaderResourceView(1, solidAddress);
    commandList->Dispatch((pieceConstants.totalCells + 63) / 64, 1, 1);
    UnorderedAccessBarrier(commandList);
    m_movingSolidAperturePipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, apertureAddress);
    commandList->SetComputeRootShaderResourceView(1, solidAddress);
    commandList->Dispatch((apertureConstants.totalCells + 63) / 64, 1, 1);
    UnorderedAccessBarrier(commandList);
    Mark(commandList, "moving solids");

    // F1b's push of orphan mist: classification, the predicated relay (when there are no unreached regions the GPU skips every relay
    // dispatch; barriers are not predicated, and the predicate is in the PREDICATION state for that span only), then the distribution into
    // the receivers, which also fills newly exposed pieces' velocity and writes the ledger's outflow and unresolved terms.
    const auto recordPush = [&]
    {
        if(remapMist)
        {
            dispatchRemap(m_movingSolidClassifyPipeline, remapAddress, remapConstants.totalCells);
        }

        UnorderedAccessBarrier(commandList);

        if(recordRelay)
        {
            m_remapPredicate.Transition(commandList, D3D12_RESOURCE_STATE_PREDICATION);
            commandList->SetPredication(m_remapPredicate.GetResource(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);

            for(const D3D12_GPU_VIRTUAL_ADDRESS address : distanceAddresses)
            {
                dispatchRemap(m_movingSolidRelayDistancePipeline, address, remapConstants.totalCells);
                UnorderedAccessBarrier(commandList);
            }

            for(const D3D12_GPU_VIRTUAL_ADDRESS address : flowAddresses)
            {
                dispatchRemap(m_movingSolidRelayFlowPipeline, address, remapConstants.totalCells);
                UnorderedAccessBarrier(commandList);
            }

            commandList->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
            m_remapPredicate.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        dispatchRemap(m_movingSolidDistributePipeline, distributeAddress, distributeConstants.totalCells);
        UnorderedAccessBarrier(commandList);
        Mark(commandList, "moving solid remap");
    };

    // F2b.1: the boundary state. The correction sums of isolated solids first (their entries cleared, then summed over their bounds'
    // sub-cells); then the previous update's wall source cleared; then each boundary cell's group fluid (and, continuous, the masses its new
    // groups receive and its orphans) into its record; then each piece's control volumes, S, D and density.
    const auto recordBoundary = [&]
    {
        if(boundaryOverflow)
        {
            // Cleared (and converted) before the pieces changed; F1b's push left geometric densities, which S = 0 keeps.
            ++m_movingSolidStatistics.boundaryOverflows;
            return;
        }

        if(!correctionBoxes.empty())
        {
            AirMovingSolidConstants sums = boundaryConstants;
            sums.boxCount = static_cast<std::uint32_t>(correctionBoxes.size());
            std::uint32_t first = 0;

            for(std::size_t index = 0; index < correctionBoxes.size(); ++index)
            {
                const std::array<std::int32_t, 7>& box = correctionBoxes[index];
                std::uint32_t count = 1;

                for(std::size_t axis = 0; axis < 3; ++axis)
                {
                    const std::int32_t size = box[axis + 3] - box[axis] + 1;
                    sums.boxMinimum[index][axis] = box[axis];
                    sums.boxSize[index][axis] = size;
                    count *= static_cast<std::uint32_t>(size);
                }

                sums.boxMinimum[index][3] = static_cast<std::int32_t>(first);
                sums.boxSize[index][3] = static_cast<std::int32_t>(count);
                sums.donorMinimum[index][0] = box[6];
                first += count;
            }

            sums.totalCells = first;
            AirMovingSolidConstants clear = boundaryConstants;
            clear.totalCells = solidCount * 2;
            const D3D12_GPU_VIRTUAL_ADDRESS clearAddress = upload.Allocate(&clear, sizeof(clear), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            const D3D12_GPU_VIRTUAL_ADDRESS sumsAddress = upload.Allocate(&sums, sizeof(sums), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

            if(clearAddress != 0 && sumsAddress != 0)
            {
                dispatchRemap(m_boundaryCorrectionClearPipeline, clearAddress, clear.totalCells);
                UnorderedAccessBarrier(commandList);
                dispatchRemap(m_boundaryCorrectionSumsPipeline, sumsAddress, sums.totalCells);
                UnorderedAccessBarrier(commandList);
            }
        }

        if(!m_boundaryReceivers.empty())
        {
            RecordBoundaryClear(commandList, upload, next, false);
        }

        m_boundaryVolumesPipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, boundaryAddress);
        commandList->SetComputeRootShaderResourceView(1, solidAddress);
        // One group per boundary cell, in rows of BoundaryGroupRow groups (the dispatch limit is 65535 per dimension).
        const std::uint32_t groupRows = (boundaryConstants.totalCells + BoundaryGroupRow - 1) / BoundaryGroupRow;
        commandList->Dispatch((std::min)(boundaryConstants.totalCells, BoundaryGroupRow), groupRows, 1);
        UnorderedAccessBarrier(commandList);
        dispatchRemap(m_boundaryGatherPipeline, boundaryAddress, boundaryConstants.totalCells);
        UnorderedAccessBarrier(commandList);

        m_boundaryReceivers.clear();

        for(const CellBox& box : boundaryBoxes)
        {
            m_boundaryReceivers.push_back({box.minimum[0], box.minimum[1], box.minimum[2], box.maximum[0], box.maximum[1], box.maximum[2]});
        }

        m_wallSourceWritten = true;
        ++m_movingSolidStatistics.boundaryUpdates;
    };

    m_boundaryRecordsUsed = m_boundaryRecordsUsed || !boundaryOverflow;

    // F2b.2: a continuous update gathers the control volumes and densities first, then pushes only the fluid no region owns (its receivers
    // divide by the step's starting effective volume); a discontinuous one runs F1b's geometric remap and push, then the boundary state,
    // whose gather converts the densities back to the new effective volumes.
    if(coupledRemap)
    {
        recordBoundary();
        recordPush();
        ++m_movingSolidStatistics.continuousRemapUpdates;
    }
    else
    {
        recordPush();
        recordBoundary();
        ++m_movingSolidStatistics.discontinuousRemapUpdates;
    }

    // F2b.4: a continuous update's step drags the fluid along the walls (CSWallTangential, before that step's projection). A discontinuous
    // update (a jump, an appearance, a removal) has no wall velocity.
    m_tangentialPending = continuousUpdate && !boundaryOverflow && solidCount > 0;

    if(m_tangentialPending)
    {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&boundaryConstants);
        m_tangentialConstants.assign(bytes, bytes + sizeof(boundaryConstants));
        m_tangentialSolids = solidAddress;
    }

    // The geometry changed: the pressure solver's components and hierarchy are stale (rebuilt at its next solve), and every motor's
    // visibility must be recomputed. F1a.2: phi is kept only on request (carried onto the rebuilt pieces above); a change that may alter
    // connectivity keeps it but takes the cold cycle budget. Otherwise the next solve starts from phi = 0, as after a VX.5 change.
    ++m_apertureBuilds;
    ++m_obstacleEpoch;

    if(!warmPressure)
    {
        m_pressurePhiCompatible = false;
    }
    else
    {
        ++m_movingSolidStatistics.warmUpdates;
        m_movingSolidStatistics.coldBudgetUpdates += connectivityRisk || fallback ? 1 : 0;
    }

    // F1a.3: the fine cells (window-local) whose pieces or face connections changed, grown by one, joined with earlier pending updates.
    for(const CellBox& box : apertureBoxes)
    {
        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            const std::int32_t low = box.minimum[axis] - next.minimum[axis] - 1;
            const std::int32_t high = box.maximum[axis] - next.minimum[axis] + 1;
            const bool first = m_pressurePendingUpdates == 0 && &box == &apertureBoxes.front();
            m_pressurePendingLow[axis] = first ? low : (std::min)(m_pressurePendingLow[axis], low);
            m_pressurePendingHigh[axis] = first ? high : (std::max)(m_pressurePendingHigh[axis], high);
        }
    }

    ++m_pressurePendingUpdates;
    m_movingSolidStatistics.multiBoxUpdates += apertureBoxes.size() > 1 ? 1 : 0;

    for(std::size_t index = 0; index < (std::min)(apertureBoxes.size(), std::size_t{2}); ++index)
    {
        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            m_movingSolidStatistics.lastApertureBoxes[index][axis] = apertureBoxes[index].minimum[axis] - next.minimum[axis];
            m_movingSolidStatistics.lastApertureBoxes[index][axis + 3] = apertureBoxes[index].maximum[axis] - next.minimum[axis];
        }
    }

    ++m_movingSolidStatistics.updates;
    m_movingSolidStatistics.mistRemapUpdates += remapMist ? 1 : 0;
    m_movingSolidStatistics.relayUpdates += recordRelay ? 1 : 0;
    m_movingSolidStatistics.fallbacks += fallback ? 1 : 0;
    m_movingSolidStatistics.lastBoxes = static_cast<std::uint32_t>(boxes.size());
    m_movingSolidStatistics.lastPieceCells = pieceConstants.totalCells;
    m_movingSolidStatistics.lastApertureCells = apertureConstants.totalCells;
    m_movingSolidStatistics.lastSolids = solidCount;
    return true;
}

bool AirField::RecordClosedSourceClear(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next)
{
    AirMovingSolidConstants constants;
    constants.field = next;
    constants.correctionUnordered = m_closedSources.GetUAVDescriptorIndex();
    constants.totalCells = AirPressure::ClosedComponentCapacity;
    const D3D12_GPU_VIRTUAL_ADDRESS address = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(address == 0)
    {
        return false;
    }

    // The projection's storage is in a shader-read state here; this buffer stays writable. The constants stand in for the unused root view.
    m_boundaryZeroPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    commandList->SetComputeRootShaderResourceView(1, address);
    commandList->Dispatch((constants.totalCells + 63) / 64, 1, 1);
    UnorderedAccessBarrier(commandList);
    return true;
}

void AirField::RecordWallTangential(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, float rate)
{
    AirMovingSolidConstants constants;
    std::memcpy(&constants, m_tangentialConstants.data(), sizeof(constants));
    // The step just advected into the other half of each pair: the current halves now hold its velocity.
    constants.velocityUnordered[0] = m_velocity[m_current].GetUAVDescriptorIndex();
    constants.velocityUnordered[1] = m_piece1Velocity[m_current].GetUAVDescriptorIndex();
    constants.stepSeconds = static_cast<float>(StepSeconds);
    const float boundedRate = std::clamp(rate, 0.0f, AirMovingSolidRequest::MaximumTangentialCouplingRate);
    constants.tangentialRateStep = static_cast<float>(boundedRate * StepSeconds);
    const D3D12_GPU_VIRTUAL_ADDRESS address = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(address == 0 || constants.totalCells == 0)
    {
        return;
    }

    m_wallTangentialPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    commandList->SetComputeRootShaderResourceView(1, m_tangentialSolids);
    commandList->Dispatch((constants.totalCells + 63) / 64, 1, 1);
    UnorderedAccessBarrier(commandList);
    Mark(commandList, "tangential wall coupling");
}

bool AirField::RecordBoundaryClear(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next, bool all)
{
    if(m_wallSource.GetResource() == nullptr)
    {
        return false;
    }

    AirMovingSolidConstants constants;
    constants.field = next;
    constants.boundaryEnabled = all ? 2u : 1u;
    constants.effectiveOffsetUnordered = m_effectiveOffset.GetUAVDescriptorIndex();
    constants.wallSourceUnordered = m_wallSource.GetUAVDescriptorIndex();
    std::vector<std::array<std::int32_t, 6>> boxes = m_boundaryReceivers;

    if(all)
    {
        std::array<std::int32_t, 6> whole{};

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            whole[axis] = next.minimum[axis];
            whole[axis + 3] = next.minimum[axis] + static_cast<std::int32_t>(next.dimensions[axis]) - 1;
        }

        boxes = {whole};
    }

    std::uint32_t first = 0;

    for(std::size_t index = 0; index < boxes.size() && index < MovingSolidBoxCapacity; ++index)
    {
        std::uint32_t count = 1;

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            const std::int32_t size = boxes[index][axis + 3] - boxes[index][axis] + 1;
            constants.boxMinimum[index][axis] = boxes[index][axis];
            constants.boxSize[index][axis] = size;
            count *= static_cast<std::uint32_t>(size);
        }

        constants.boxMinimum[index][3] = static_cast<std::int32_t>(first);
        constants.boxSize[index][3] = static_cast<std::int32_t>(count);
        first += count;
        constants.boxCount = static_cast<std::uint32_t>(index + 1);
    }

    constants.totalCells = first;
    const D3D12_GPU_VIRTUAL_ADDRESS address = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(address == 0 || first == 0)
    {
        return false;
    }

    // Whatever wrote the buffers before has finished; whatever reads them next sees the clear. The constants stand in for the unused root view.
    UnorderedAccessBarrier(commandList);

    // F2b.2: stored mist is mass per effective volume G - S. Clearing S everywhere first converts every piece's density to geometric units
    // (with the pieces still the ones S was prepared for), so no mass changes with the representation.
    if(all && m_mistAllocated)
    {
        AirMovingSolidConstants convert = constants;
        convert.pieceUnordered = m_pieces.GetUAVDescriptorIndex();
        convert.mistUnordered[0] = m_mist[m_mistCurrent].GetUAVDescriptorIndex();
        convert.mistUnordered[1] = m_mistPiece1[m_mistCurrent].GetUAVDescriptorIndex();
        const D3D12_GPU_VIRTUAL_ADDRESS convertAddress = upload.Allocate(&convert, sizeof(convert), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(convertAddress == 0)
        {
            return false;
        }

        m_boundaryToGeometricPipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, convertAddress);
        commandList->SetComputeRootShaderResourceView(1, convertAddress);
        commandList->Dispatch((first + 63) / 64, 1, 1);
        UnorderedAccessBarrier(commandList);
    }

    m_boundaryClearPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    commandList->SetComputeRootShaderResourceView(1, address);
    commandList->Dispatch((first + 63) / 64, 1, 1);
    UnorderedAccessBarrier(commandList);
    return true;
}

std::uint32_t AirField::UploadMotors(UploadBuffer& upload, const AirFieldBinding& next, const AirFrameRequest& request,
    D3D12_GPU_VIRTUAL_ADDRESS& address, std::vector<std::uint32_t>& recompute)
{
    recompute.clear();
    m_frameTags.clear();
    m_motorsWithoutCapacity.clear();
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

        // AIR.4E.5d: at most the visibility capacity (MaximumMotors once grown; less only if a growth failed). AIR.4F.2b: the motors
        // beyond it that reach the window are recorded rather than silently stopping at the first, so a Point emitter's jet can
        // report that it found no slot. The motors applied are the same as before.
        if(motors.size() >= m_visibilityCapacity)
        {
            m_motorsWithoutCapacity.push_back(motor.tag);
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

        for(std::uint32_t slot = 0; slot < m_visibilityCapacity; ++slot)
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

        for(std::uint32_t slot = 0; slot < m_visibilityCapacity; ++slot)
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
    // AIR.4F F4.5.1b: a relaxation time of 0 (or an invalid one) keeps ΔT unchanged.
    const double relaxationTime = static_cast<double>(request.solver.temperatureRelaxationTime);
    constants.temperatureFactor = std::isfinite(relaxationTime) && relaxationTime > 0.0 ? static_cast<float>(std::exp(-StepSeconds / relaxationTime)) : 1.0f;
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
    m_frameVolumes = false;
    std::array<std::int64_t, 3> volumeLow{};
    std::array<std::int64_t, 3> volumeHigh{};
    std::vector<GpuMistEmitter> emitters;
    const double cellSize = next.cellSize;

    for(const MistEmitterInstance& emitter : request.mistEmitters)
    {
        if(emitters.size() >= MaximumMistEmitters)
        {
            break;
        }

        const bool point = emitter.mode == MistEmitterMode::Point;
        const bool volume = emitter.mode == MistEmitterMode::Volume;
        const Vector3& half = emitter.halfExtents;
        // AIR.4F.2c: a Volume is always a box.
        float boundingRadius = emitter.shape == MistEmitterShape::Sphere && !volume ? emitter.radius :
            std::sqrt(half.x * half.x + half.y * half.y + half.z * half.z);
        const double position[3] = {emitter.position.x, emitter.position.y, emitter.position.z};
        const float rate = point ? emitter.massRate : (volume ? emitter.targetDensity : emitter.densityRate);

        // AIR.4F.2b: a Point footprint is a sphere of at least MinimumPointFootprint and at most MaximumPointRadius (the GPU's
        // fixed-point weight budget); the component's validation already keeps the radius within it.
        if(point)
        {
            boundingRadius = std::clamp(emitter.radius, MistEmitterComponent::MinimumPointFootprint, MistEmitterComponent::MaximumPointRadius);
        }

        if(!std::isfinite(boundingRadius) || !std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2]) ||
            !(rate >= 0.0f) || !std::isfinite(rate))
        {
            continue;
        }

        // The cells holding the bounding sphere. Legacy: within the window. Point: all of them, because footprint cells outside the
        // window count toward the total weight (their share of the emission is lost, not moved inside). Either way, an emitter whose
        // box misses the window does no work at all.
        EmitterBox box;
        box.mode = emitter.mode;
        box.deposits = point && rate > 0.0f;
        bool reaches = true;

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            const std::int64_t windowLow = next.minimum[axis];
            const std::int64_t windowHigh = windowLow + next.dimensions[axis] - 1;
            const std::int64_t footprintLow = static_cast<std::int64_t>(std::floor((position[axis] - boundingRadius) / cellSize));
            const std::int64_t footprintHigh = static_cast<std::int64_t>(std::floor((position[axis] + boundingRadius) / cellSize));
            const std::int64_t low = (std::max)(footprintLow, windowLow);
            const std::int64_t high = (std::min)(footprintHigh, windowHigh);
            reaches = reaches && high >= low;
            box.minimum[axis] = static_cast<std::int32_t>(point ? footprintLow : low);
            box.size[axis] = reaches ? static_cast<std::uint32_t>(point ? footprintHigh - footprintLow + 1 : high - low + 1) : 0;
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
        gpu.radius = point ? boundingRadius : emitter.radius;
        CopyVector(emitter.halfExtents, gpu.halfExtents);
        CopyVector(emitter.color, gpu.color);
        gpu.mode = static_cast<std::uint32_t>(box.mode);
        gpu.temperatureRate = std::isfinite(emitter.temperatureRate) ? std::clamp(emitter.temperatureRate,
            MistEmitterComponent::MinimumTemperatureResponseRate, MistEmitterComponent::MaximumTemperatureResponseRate) : 1.0f;
        // AIR.4F F4.5.1b: within the field's ΔT range; a non-finite value heats nothing.
        gpu.temperature = std::isfinite(emitter.temperature) ? std::clamp(emitter.temperature, -200.0f, 1000.0f) : 0.0f;
        // Legacy: density per step. Point: density x m^3 per step. AIR.4F.2c, Volume: the target density itself.
        gpu.densityPerStep = volume ? rate : static_cast<float>(static_cast<double>(rate) * StepSeconds);

        // AIR.4F.2c: a Volume's maintenance constants, and its cells joined into the shared pass's box.
        if(volume)
        {
            const double replenishment = std::isfinite(emitter.replenishmentRate) ? (std::max)(0.0, static_cast<double>(emitter.replenishmentRate)) : 0.0;
            gpu.replenishment = static_cast<float>(1.0 - std::exp(-replenishment * StepSeconds));
            gpu.edgeSoftness = std::isfinite(emitter.edgeSoftness) ? (std::max)(emitter.edgeSoftness, 0.0f) : 0.0f;
            gpu.fillHeight = std::isfinite(emitter.fillHeight) ? (std::max)(emitter.fillHeight, 0.0f) : 0.0f;
            gpu.temperatureRate = std::isfinite(emitter.temperatureRate) ? std::clamp(emitter.temperatureRate, 0.0f,
                MistEmitterComponent::MaximumVolumeTemperatureRate) : 0.0f;

            for(std::size_t axis = 0; axis < 3; ++axis)
            {
                const std::int64_t low = box.minimum[axis];
                const std::int64_t high = low + box.size[axis] - 1;
                volumeLow[axis] = m_frameVolumes ? (std::min)(volumeLow[axis], low) : low;
                volumeHigh[axis] = m_frameVolumes ? (std::max)(volumeHigh[axis], high) : high;
            }

            m_frameVolumes = true;
        }
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

    for(std::size_t axis = 0; axis < 3; ++axis)
    {
        m_frameVolumeBox.minimum[axis] = static_cast<std::int32_t>(volumeLow[axis]);
        m_frameVolumeBox.size[axis] = m_frameVolumes ? static_cast<std::uint32_t>(volumeHigh[axis] - volumeLow[axis] + 1) : 0;
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
            std::vector<std::uint32_t> words(slot.tags.size() * (MistStatusBytesPerEmitter / 4));
            std::memcpy(words.data(), data, words.size() * sizeof(std::uint32_t));
            const D3D12_RANGE written{0, 0};
            slot.readback->Unmap(0, &written);
            m_mistEmitterStatus.clear();

            for(std::size_t index = 0; index < slot.tags.size(); ++index)
            {
                MistEmitterStatus status;
                const std::size_t first = index * (MistStatusBytesPerEmitter / 4);
                status.origin = static_cast<MistEmitterStatus::Origin>(words[first] & 3u);
                status.pieces = words[first + 1];
                // AIR.4F.2b: a Point emitter's first step: the weight outside the window over the total (0 when it deposited nothing).
                const std::uint32_t total = words[first + 2];
                status.outsideShare = total != 0 ? static_cast<float>(static_cast<double>(words[first + 3]) / total) : 0.0f;
                m_mistEmitterStatus[slot.tags[index]] = status;
            }

            m_mistStatusFence = slot.fence;
        }

        slot.fence = 0;
    }
}

bool AirField::RecordMistStep(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const AirFieldBinding& next,
    const AirMistSettings& mist, D3D12_GPU_VIRTUAL_ADDRESS emitters, std::uint32_t emitterCount, std::uint32_t step)
{
    const bool countPieces = step == 0;

    AirMistConstants constants;
    constants.field = next;
    constants.obstacleIndex = m_obstacles.GetUAVDescriptorIndex();
    constants.pieceIndex = m_pieces.GetUAVDescriptorIndex();
    // The velocity the solver's step just wrote (RecordStep has swapped m_current to it).
    constants.velocityIndex = m_velocity[m_current].GetUAVDescriptorIndex();
    constants.velocityPiece1Index = m_piece1Velocity[m_current].GetUAVDescriptorIndex();
    constants.mistRead[0] = m_mist[m_mistCurrent].GetUAVDescriptorIndex();
    constants.mistRead[1] = m_mistPiece1[m_mistCurrent].GetUAVDescriptorIndex();
    // AIR.4F.2d.4 Stage 2a: after a conservative transport the step's mist is already in the current buffers, so emission adds there.
    const bool transported = m_mistTransportedByFlux;
    m_mistTransportedByFlux = false;
    const std::uint32_t written = transported ? m_mistCurrent : 1 - m_mistCurrent;
    constants.mistWrite[0] = m_mist[written].GetUAVDescriptorIndex();
    constants.mistWrite[1] = m_mistPiece1[written].GetUAVDescriptorIndex();
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

    // The advective transport, unless this step was transported conservatively.
    if(!transported)
    {
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
    }

    constants.emitterCount = emitterCount;
    constants.countPieces = countPieces ? 1u : 0u;
    constants.statusIndex = m_mistStatus.GetUAVDescriptorIndex();
    constants.emitStep = step;

    // AIR.4F.2b: one Point pass (CSMistPointSum or CSMistPointDeposit) over emitter `index`'s footprint cells.
    const auto recordPointPass = [&](ComputePipeline& pipeline, std::uint32_t index)
    {
        const EmitterBox& box = m_frameEmitterBoxes[index];
        AirMistConstants passConstants = constants;
        passConstants.emitterIndex = index;

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            passConstants.emitBoxMinimum[axis] = box.minimum[axis];
            passConstants.emitBoxSize[axis] = box.size[axis];
        }

        const D3D12_GPU_VIRTUAL_ADDRESS passAddress = upload.Allocate(&passConstants, sizeof(passConstants),
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(passAddress == 0)
        {
            return false;
        }

        pipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, passAddress);
        commandList->SetComputeRootShaderResourceView(1, emitters);
        commandList->Dispatch((box.size[0] + 3) / 4, (box.size[1] + 3) / 4, (box.size[2] + 3) / 4);
        ++m_pointDispatches;
        return true;
    };

    // AIR.4F.2c: every Volume emitter at once, first (the order transport, volumes, then each emitter's own emission), so a Point or
    // Legacy emitter's mist always adds on top of a volume's target. One thread per cell of the volumes' joint box.
    if(m_frameVolumes)
    {
        AirMistConstants volumeConstants = constants;

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            volumeConstants.emitBoxMinimum[axis] = m_frameVolumeBox.minimum[axis];
            volumeConstants.emitBoxSize[axis] = m_frameVolumeBox.size[axis];
        }

        const D3D12_GPU_VIRTUAL_ADDRESS volumeAddress = upload.Allocate(&volumeConstants, sizeof(volumeConstants),
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(volumeAddress != 0)
        {
            const EmitterBox& box = m_frameVolumeBox;
            m_mistVolumePipeline.Bind(commandList);
            commandList->SetComputeRootConstantBufferView(0, volumeAddress);
            commandList->SetComputeRootShaderResourceView(1, emitters);
            commandList->Dispatch((box.size[0] + 3) / 4, (box.size[1] + 3) / 4, (box.size[2] + 3) / 4);
            UnorderedAccessBarrier(commandList);
            ++m_volumeDispatches;
        }
    }

    // AIR.4F.2b: every Point emitter's total weight for this step, before any emission. Each sums into its own words, so the sums
    // need no barrier between them; one barrier makes them all visible to the deposits.
    bool pointSums = false;

    for(std::uint32_t index = 0; index < emitterCount; ++index)
    {
        if(m_frameEmitterBoxes[index].deposits && recordPointPass(m_mistPointSumPipeline, index))
        {
            pointSums = true;
        }
    }

    if(pointSums)
    {
        UnorderedAccessBarrier(commandList);
    }

    // AIR.4F F2: emission into the step's new mist, one emitter at a time in upload order, a barrier after each so overlapping
    // emitters add deterministically. AIR.4F.2b: a Point emitter deposits in its place in that order.
    for(std::uint32_t index = 0; index < emitterCount; ++index)
    {
        const EmitterBox& box = m_frameEmitterBoxes[index];

        // AIR.4F.2c: maintained by the shared pass above.
        if(box.mode == MistEmitterMode::Volume)
        {
            continue;
        }

        if(box.mode == MistEmitterMode::Point)
        {
            if(!recordPointPass(m_mistPointDepositPipeline, index))
            {
                break;
            }

            UnorderedAccessBarrier(commandList);
            continue;
        }

        constants.emitterIndex = index;

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

    // The next step (solver or mist), or a reader, sees this step's mist (already current after a conservative transport).
    if(!transported)
    {
        m_mistCurrent = 1 - m_mistCurrent;
    }

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
    m_lastStepCount = 0;
    m_frameSubmissionFence = lastSubmissionFence;
    m_boundaryRecordsUsed = false;

    // R1.2a: without pipelines (a packaged runtime) every request is off, whatever it asks.
    if(!m_initialized || !request.enabled || !ValidateSettings(request.settings, nullptr))
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

    // AIR.4E.5d: room for this frame's motors (the request's count bounds the motors that reach the window). Without it the frame
    // keeps the current capacity: AssignVisibility then serves only that many motors.
    EnsureVisibilityCapacity(request.motors.size(), lastSubmissionFence);

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

    // AIR.4E.4: projection off holds no pressure storage; turned on again, the solver is allocated and rebuilt afresh.
    m_pressureDiagnostics = {};

    if(!request.pressure.enabled && m_projectionStorage)
    {
        RetirePressure(lastSubmissionFence);
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

    // AIR.4E.5b: a reset, a teleport or a geometry change can change any piece, so the solver's phi no longer fits.
    if(full || voxelsChanged)
    {
        m_pressurePhiCompatible = false;
    }

    // A re-initialized field starts its clock afresh.
    if(full)
    {
        m_accumulator = 0.0;
    }

    const std::uint32_t steps = TakeSteps(request);
    const bool mistClear = m_mistAllocated && m_mistClearPending;

    // AIR.4F.3 F1a: the solids this frame moves to (the first MaximumMovingSolids by object ID), and whether the overlay has work: a solid
    // to add, move or remove, or a maintenance pass that rebuilt pieces from residency alone and so must be carved again.
    std::map<std::string, AirMovingSolid, std::less<>> movingSolidTargets;

    if(request.movingSolids.enabled)
    {
        for(const AirMovingSolid& solid : request.movingSolids.solids)
        {
            movingSolidTargets.insert_or_assign(solid.objectID, solid);
        }

        if(movingSolidTargets.size() > MaximumMovingSolids)
        {
            movingSolidTargets.erase(std::next(movingSolidTargets.begin(), MaximumMovingSolids), movingSolidTargets.end());
            ++m_movingSolidStatistics.truncatedFrames;
        }
    }

    // F2b.2a: step-aligned geometry. A frame without an AIR step keeps the applied poses (a maintenance pass re-stamps them); the next
    // stepped frame takes the snapshot then current, so every transition has the projected step its wall source needs.
    const bool applyWithoutSteps = request.movingSolids.applyWithoutStepsForDiagnostics ||
        request.movingSolids.continuousWithoutStepsForDiagnostics;

    if(request.movingSolids.enabled && steps == 0 && !applyWithoutSteps)
    {
        movingSolidTargets = m_appliedMovingSolids;
        m_movingSolidStatistics.deferredFrames += 1;
    }

    bool movingSolidsDiffer = movingSolidTargets.size() != m_appliedMovingSolids.size();

    for(const auto& [objectID, solid] : movingSolidTargets)
    {
        const auto applied = m_appliedMovingSolids.find(objectID);
        movingSolidsDiffer = movingSolidsDiffer || applied == m_appliedMovingSolids.end() || !SameMovingSolidGeometry(applied->second, solid);
    }

    const bool movingSolidsPresent = !movingSolidTargets.empty() || !m_appliedMovingSolids.empty();
    const bool movingSolidWork = movingSolidsDiffer || (maintenance && movingSolidsPresent);

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

    if(maintenance || steps > 0 || mistClear || movingSolidWork)
    {
        ID3D12DescriptorHeap* heaps[]{descriptorHeap.GetHeap()};
        commandList->SetDescriptorHeaps(1, heaps);
        Mark(commandList, "start");
        TransitionStorage(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // A mist clear first (new mist storage, or ClearMist), so maintenance and steps work on cleared mist; then maintenance, so
        // entering cells are initialized before a step reads them.
        const bool cleared = !mistClear || RecordMistClear(commandList, upload, next);

        // F2b.1: maintenance rebuilds pieces from residency alone (toroidal slots may now hold other cells): the boundary state is geometric
        // everywhere until the restamp update prepares it again. F2b.2: cleared before the rebuild, while the pieces are still the ones the
        // effective volumes describe, so the densities convert to geometric units without changing mass.
        if(cleared && maintenance && m_effectiveOffset.GetResource() != nullptr)
        {
            RecordBoundaryClear(commandList, upload, next, true);
            m_wallSourceWritten = false;
        }

        if(!cleared || (maintenance && !RecordMaintenance(commandList, upload, next, full, shifted, voxelsChanged, voxels)))
        {
            // No room for constants this frame: the previous binding stays (invalid if never initialized); retried next frame.
            TransitionStorage(commandList, ReadState);
            m_binding.valid = m_resetPending ? 0u : m_binding.valid;
            m_mistBinding.valid = 0;
            return;
        }

        m_mistClearPending = m_mistClearPending && !mistClear;
        Mark(commandList, "transitions, mist clear and maintenance");

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
        Mark(commandList, "motor visibility and status");

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
        Mark(commandList, "mist emitter status");

        // AIR.4F.3 F1a: the overlay moves each solid from its applied pose to its target over the frame's geometry updates, one before each
        // step (one in all on a frame without steps), so the last update reaches the target and no motion is skipped or applied twice.
        const std::uint32_t geometryUpdates = (std::max)(steps, 1u);
        m_couplingForDiagnostics = request.movingSolids.couplingForDiagnostics;

        // F2b.5: a new physics session (or a step count behind the cursor) starts the cursor one step behind its latest pose.
        if(request.movingSolids.physicsStep > 0 &&
            (request.movingSolids.physicsSession != m_physicsCursorSession || request.movingSolids.physicsStep < m_physicsCursor))
        {
            m_physicsCursorSession = request.movingSolids.physicsSession;
            m_physicsCursor = request.movingSolids.physicsStep - 1;
        }

        const auto applyMovingSolids = [&](std::uint32_t update)
        {
            if(!movingSolidWork)
            {
                return;
            }

            std::map<std::string, AirMovingSolid, std::less<>> updateTargets;
            const double fraction = double(update + 1) / double(geometryUpdates);

            // F2b.5: one physics step of motion per AIR step. The step after the cursor, kept within the two steps the request holds
            // (n - 1: the previous poses, n: the latest), so AIR trails physics by up to a step but never takes two steps of motion at
            // once (after falling more than a step behind, it catches up once to n - 1) and holds when physics has not stepped.
            const std::uint64_t physicsStep = request.movingSolids.physicsStep;

            if(physicsStep > 0 && steps > 0)
            {
                const std::uint64_t latest = physicsStep;
                const std::uint64_t desired = m_physicsCursor + 1;
                const std::uint64_t chosen = desired > latest ? latest : (desired + 1 < latest ? latest - 1 : desired);
                m_physicsCursor = chosen;

                for(const auto& [objectID, solid] : movingSolidTargets)
                {
                    AirMovingSolid target = solid;

                    // The previous poses exist only within one motion epoch; otherwise the latest is the only pose there is.
                    if(chosen + 1 == latest && solid.hasPrevious)
                    {
                        target.position = solid.previousPosition;
                        target.orientation = solid.previousOrientation;
                    }

                    updateTargets.emplace(objectID, target);
                }
            }
            else
            {
                for(const auto& [objectID, solid] : movingSolidTargets)
                {
                    const auto applied = m_appliedMovingSolids.find(objectID);
                    const AirMovingSolid* from = applied != m_appliedMovingSolids.end() ? &applied->second : nullptr;
                    // F2b.2a: with several updates, the ones before the last move toward physics' previous pose (the end of the step before its
                    // latest) and the last takes the latest pose; never across an epoch change (InterpolateMovingSolid jumps then).
                    const bool towardPrevious = solid.hasPrevious && geometryUpdates > 1 && update + 1 < geometryUpdates;

                    if(towardPrevious)
                    {
                        AirMovingSolid previous = solid;
                        previous.position = solid.previousPosition;
                        previous.orientation = solid.previousOrientation;
                        const double previousFraction = double(update + 1) / double(geometryUpdates - 1);
                        updateTargets.emplace(objectID, InterpolateMovingSolid(from, previous, previousFraction));
                    }
                    else
                    {
                        updateTargets.emplace(objectID, InterpolateMovingSolid(from, solid, fraction));
                    }
                }
            }

            const bool restampAll = maintenance && update == 0;
            // F2b.1: an update is continuous only when a projected step (with the conservative transport, when mist is stepped) follows it.
            const bool projectedStep = steps > 0 && request.pressure.enabled && (!m_mistAllocated || request.mist.conservativeTransport);
            const bool continuousStep = projectedStep || request.movingSolids.continuousWithoutStepsForDiagnostics;
            // A failed recording leaves the GPU at the applied poses, which the next update starts from again.
            if(RecordMovingSolidUpdate(commandList, upload, next, voxels, m_appliedMovingSolids, updateTargets, restampAll,
                !request.movingSolids.coldRebuildForDiagnostics, continuousStep))
            {
                m_appliedMovingSolids = std::move(updateTargets);
            }

            // Inside the update, F1a's geometry passes are marked "moving solids" and F1b's remap "moving solid remap"; the time since is
            // F2b.1's boundary preparation.
            Mark(commandList, "moving boundary");
        };

        if(steps == 0)
        {
            applyMovingSolids(0);
        }

        // The frame's upload ring is shared with the rest of the renderer, which allocates after AIR. A step after the first starts only
        // while the ring still holds another step the size of the last one plus UploadReserveBytes; otherwise the remaining steps go back
        // to the accumulator before any of their work (geometry included) is recorded, and run next frame.
        std::size_t lastStepUploadBytes = 0;

        for(std::uint32_t step = 0; step < steps; ++step)
        {
            const std::size_t uploadRemaining = upload.GetCapacityPerFrame() - upload.GetFrameBytesUsed();

            if(step > 0 && uploadRemaining < lastStepUploadBytes + UploadReserveBytes)
            {
                m_accumulator += (steps - step) * StepSeconds;
                m_uploadDeferredSteps += steps - step;
                break;
            }

            const std::size_t stepUploadStart = upload.GetFrameBytesUsed();
            const std::uint64_t boundaryUpdates = m_movingSolidStatistics.boundaryUpdates;
            applyMovingSolids(step);

            // F2b.1: a step whose geometry did not change has no wall source (invariant 7: stationary geometry, none).
            if(m_movingSolidStatistics.boundaryUpdates == boundaryUpdates && m_wallSourceWritten)
            {
                m_wallSourceWritten = !RecordBoundaryClear(commandList, upload, next, false);
            }

            if(!RecordStep(commandList, upload, next, request, motors, motorCount))
            {
                break;
            }

            ++m_lastStepCount;

            Mark(commandList, "solver step");

            // F2b.4: the tangential wall coupling of the update just before this step, on the step's advected velocity, before its projection.
            if(m_tangentialPending && request.movingSolids.tangentialCouplingRate > 0.0f && request.pressure.enabled)
            {
                RecordWallTangential(commandList, upload, request.movingSolids.tangentialCouplingRate);
            }

            m_tangentialPending = false;

            // AIR.4E.4: the projection between the solver step and its mist step. A failed recording (no room for constants, no
            // storage) leaves this step unprojected.
            if(request.pressure.enabled)
            {
                RecordProjection(commandList, upload, next, request, step + 1 == steps, mistStepping && request.mist.conservativeTransport);
                Mark(commandList, "pressure projection");
            }

            if(mistStepping && !RecordMistStep(commandList, upload, next, request.mist, emitters, emitterCount, step))
            {
                mistStepping = false;
            }

            Mark(commandList, "mist step");
            lastStepUploadBytes = upload.GetFrameBytesUsed() - stepUploadStart;
        }

        if(emitterCount > 0)
        {
            CopyMistEmitterStatus(commandList, frameIndex);
        }

        // Readers of this frame, and of later frames, see the latest velocity, mist and mask in a shader-read state.
        TransitionStorage(commandList, ReadState);
        Mark(commandList, "status copy and transitions");

        ++next.generation;
        m_pressureDiagnostics.generation = m_pressureDiagnostics.valid != 0 ? next.generation : 0;
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
