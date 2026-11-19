// NVIDIA SHARC v1.8.3 (NVIDIA-RTX/SHARC 4e21b585) integration, shared by its path-tracing libraries and its resolve
// (Documentation/Rendering/NVIDIA_SHARC_Integration_A.md). Only shaders under SharcNv/ include this file: they compile
// with -enable-16bit-types (ShaderCompiler::UsesNative16BitTypes), which SharcTypes.h needs. NVIDIA's headers are used
// unmodified from Sharc/ (deployed by CMake from ENGINE_SHARC_ROOT); everything Engine-specific is configured through
// their macros here.
//
// The includer defines SHARC_UPDATE 1 for the update (training) pass and leaves it undefined otherwise.

#ifndef SHARC_NV_COMMON_HLSLI
#define SHARC_NV_COMMON_HLSLI

// Engine's path tracer has no emissive surfaces (lights are analytic and the environment is reached by misses), and A
// stores absolute radiance: no separate emission and no material demodulation.
#define SHARC_SEPARATE_EMISSIVE 0
#define SHARC_MATERIAL_DEMODULATION 0
#define HASH_GRID_USE_NORMALS 1
#define HASH_GRID_ENABLE_64_BIT_ATOMICS 1

#include "Sharc/SharcCommon.h"

#if SHARC_VERSION_MAJOR != 1 || SHARC_VERSION_MINOR != 8 || SHARC_VERSION_BUILD != 3
#error "Engine's NVIDIA SHARC integration is validated against v1.8.3 only."
#endif

// Must match SharcCache::Constants (Renderer/Sharc/SharcCache.h): 8 rows of 16 bytes.
struct SharcNvConstants
{
    uint hashEntriesIndex;
    uint accumulationIndex;
    uint resolvedIndex;
    uint capacity;

    float3 cameraPosition;
    float sceneScale;

    float3 previousCameraPosition;
    float radianceScale;

    uint accumulationFrames;
    uint staleFrames;
    uint frameIndex;
    uint trainingTile;

    uint trainingTilesX;
    uint trainingTilesY;
    // The render pass's cache vertex (2: x2, the hit of x1's continuation).
    uint queryVertex;
    // SHARC_NV_FLAG_*.
    uint flags;

    // The statistics buffer (raw UAV, SHARC_NV_STATISTIC_*); used only with SHARC_NV_FLAG_STATISTICS.
    uint statisticsIndex;
    // The training dispatch's path depth (it replaces maxPathDepth in the update library).
    uint trainingMaxDepth;
    // The first and last path vertex whose outgoing radiance the update pass deposits.
    uint depositFirstVertex;
    uint depositLastVertex;

    // Diagnostics: a positive value clamps each deposited radiance to this luminance (0: off).
    float depositLuminanceLimit;
    float3 padding0;

    float4 padding1;
};

static const uint SHARC_NV_FLAG_STATISTICS = 1u;

// Statistics words (cleared every frame they are recorded).
static const uint SHARC_NV_STATISTIC_QUERY_REACHED = 0u;
static const uint SHARC_NV_STATISTIC_QUERY_ADMITTED = 1u;
static const uint SHARC_NV_STATISTIC_QUERY_HIT = 2u;
static const uint SHARC_NV_STATISTIC_TRAINING_PATHS = 3u;
static const uint SHARC_NV_STATISTIC_DEPOSITS = 4u;
static const uint SHARC_NV_STATISTIC_DEPOSIT_STOPS = 5u;
static const uint SHARC_NV_STATISTIC_MISSES = 6u;
static const uint SHARC_NV_STATISTIC_MAXIMUM_DEPOSIT = 7u;
static const uint SHARC_NV_STATISTIC_CLAMPED_DEPOSITS = 8u;
static const uint SHARC_NV_STATISTIC_OCCUPIED = 9u;
static const uint SHARC_NV_STATISTIC_SAMPLED = 10u;
static const uint SHARC_NV_STATISTIC_MAXIMUM_ACCUMULATION = 11u;
static const uint SHARC_NV_STATISTIC_HIGH_ACCUMULATION = 12u;
static const uint SHARC_NV_STATISTIC_MAXIMUM_SAMPLES = 13u;
static const uint SHARC_NV_STATISTIC_SATURATED = 14u;
static const uint SHARC_NV_STATISTIC_RESOLVED_OCCUPIED = 15u;
static const uint SHARC_NV_STATISTIC_COUNT = 16u;

HashGridParameters SharcNvGridParameters(SharcNvConstants constants)
{
    HashGridParameters grid;
    grid.cameraPosition = constants.cameraPosition;
    grid.logarithmBase = SHARC_GRID_LOGARITHM_BASE;
    grid.sceneScale = constants.sceneScale;
    grid.levelBias = SHARC_GRID_LEVEL_BIAS;
    return grid;
}

// Every field is assigned, and the struct is never zero-initialized: RTXPT records that `(SharcParameters)0` on a struct
// holding descriptor-heap handles makes the NVIDIA driver fault while creating the DXR state object.
SharcParameters SharcNvParameters(SharcNvConstants constants)
{
    RWStructuredBuffer<uint64_t> hashEntries = ResourceDescriptorHeap[constants.hashEntriesIndex];
    RWStructuredBuffer<SharcAccumulationData> accumulation = ResourceDescriptorHeap[constants.accumulationIndex];
    RWStructuredBuffer<SharcPackedData> resolved = ResourceDescriptorHeap[constants.resolvedIndex];

    SharcParameters parameters;
    parameters.hashGridParameters = SharcNvGridParameters(constants);
    parameters.hashGridData.capacity = constants.capacity;
    parameters.hashGridData.hashEntriesBuffer = hashEntries;
    parameters.radianceScale = constants.radianceScale;
    parameters.accumulationBuffer = accumulation;
    parameters.resolvedBuffer = resolved;
    return parameters;
}

// Adds count to a statistic once per wave.
void SharcNvCount(SharcNvConstants constants, uint statistic, bool condition)
{
    if((constants.flags & SHARC_NV_FLAG_STATISTICS) == 0u)
    {
        return;
    }

    uint count = WaveActiveCountBits(condition);

    if(WaveIsFirstLane() && count > 0u)
    {
        RWByteAddressBuffer statistics = ResourceDescriptorHeap[constants.statisticsIndex];
        statistics.InterlockedAdd(statistic * 4u, count);
    }
}

// The wave's maximum of a non-negative float (its bits order like the value) into a statistic.
void SharcNvMaximum(SharcNvConstants constants, uint statistic, bool active, float value)
{
    if((constants.flags & SHARC_NV_FLAG_STATISTICS) == 0u)
    {
        return;
    }

    uint bits = active && value > 0.0f ? asuint(min(value, 3.0e38f)) : 0u;
    uint maximum = WaveActiveMax(bits);

    if(WaveIsFirstLane() && maximum > 0u)
    {
        RWByteAddressBuffer statistics = ResourceDescriptorHeap[constants.statisticsIndex];
        statistics.InterlockedMax(statistic * 4u, maximum);
    }
}

#endif
