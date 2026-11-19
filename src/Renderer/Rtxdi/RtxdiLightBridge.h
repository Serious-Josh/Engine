#pragma once

#include "Renderer/ShaderData.h"
#include "Scene/DirectionalLight.h"

#include "Rtxdi/DI/ReSTIRDI.h"

#include <cstdint>
#include <string>
#include <vector>

struct FrameLighting;

// RTXPT.3 R3.2: Engine's lights as RTXDI light indices, for the raster-primary ReSTIR DI variant only. The
// existing deferred and path-tracing light bindings are untouched; a record carries the data those paths already
// use, so RTXDI evaluates and samples every light with the path tracer's own functions (RtxdiLights.hlsli).
//
// Index order is RTXDI's region layout: the local region holds the Point and Spot table lights, the infinite
// region the Directional table lights, then the sun, then (by environment strategy, below) the HDRI; each group
// keeps table order. All table lights are delta lights (no artificial radius): RTXDI's sample uv is ignored for
// them. The sun maps uv onto its cone exactly as PathTraceSampleSun does, and the HDRI inverts Engine's
// marginal/conditional CDF.
enum class RtxdiLightKind : std::uint32_t
{
    // Equal to the GPULight table types.
    Point = 0,
    Spot = 1,
    Directional = 2,
    Sun = 3,
    Environment = 4
};

// PT.2B: who estimates the HDRI's direct light at the ReSTIR DI vertex; the values of RtxdiEnvironmentConfig.hlsli.
// InfiniteLight: the HDRI is the last infinite light (pre-PT.2B). Reservoir: the HDRI is RTXDI's environment light,
// sampled with environment and BSDF candidates. SeparateMis: the HDRI is not an RTXDI light; the path tracer estimates it
// with its own light sample and BSDF continuation, power-heuristic weighted.
enum class RtxdiEnvironmentStrategy : std::uint32_t
{
    InfiniteLight = 0,
    Reservoir = 1,
    SeparateMis = 2
};

// The strategy this build's path tracer libraries were compiled with (RTXDI_ENVIRONMENT_STRATEGY).
RtxdiEnvironmentStrategy CompiledRtxdiEnvironmentStrategy() noexcept;

// PT.2B: one GPU record per RTXDI light index, in the GPULight layout (RtxdiLights.hlsli reads 64 bytes per light).
// type is the RtxdiLightKind. A table light is its SceneLightConstants record verbatim. The sun stores its direction
// to the sun in direction, its irradiance in radiance and the cosine of its angular radius in cosOuterCone (1 for a
// delta sun). The HDRI stores only its kind: its distribution is the frame's, and it translates between frames only
// while that distribution is unchanged, so the frame's is also the previous one's. A record is self-contained, so a
// previous frame's record describes the previous light even after the light moved or changed.
using RtxdiLightRecord = GPULight;

static_assert(sizeof(RtxdiLightRecord) == 64);

// The path tracer's sun, derived from FrameLighting::sun exactly as RecordPathTraceReference derives it.
struct RtxdiSunSource
{
    float directionToSun[3]{};
    // 1 for a delta sun.
    float cosAngularRadius = 1.0f;
    float irradiance[3]{};

    bool operator==(const RtxdiSunSource&) const = default;
};

RtxdiSunSource MakeRtxdiSunSource(const DirectionalLight& sun);

// The path tracer's HDRI importance distribution: the same descriptors and extents it binds.
struct RtxdiEnvironmentSource
{
    std::uint32_t textureIndex = 0xFFFFFFFFu;
    std::uint32_t conditionalCDFIndex = 0xFFFFFFFFu;
    std::uint32_t marginalCDFIndex = 0xFFFFFFFFu;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float totalWeight = 0.0f;

    bool operator==(const RtxdiEnvironmentSource&) const = default;
};

// Everything that decides a light's samples, target PDFs and uv mapping. Only the fields of its kind are set. The
// stable key is (kind, objectID); the rest is the light's state in one frame.
struct RtxdiLightIdentity
{
    RtxdiLightKind kind = RtxdiLightKind::Point;
    // Stable scene object ID; empty for the legacy sun and the HDRI.
    std::string objectID;
    GPULight tableLight{};
    RtxdiSunSource sun{};
    RtxdiEnvironmentSource environment{};

    bool operator==(const RtxdiLightIdentity&) const = default;
};

struct RtxdiLightSet
{
    // Every table light plus the sun and the HDRI.
    static constexpr std::uint32_t Capacity = SceneLightConstants::Capacity + 2;

    std::vector<RtxdiLightRecord> records;
    // Parallel to records.
    std::vector<RtxdiLightIdentity> identities;
    RTXDI_LightBufferParameters bufferParameters{};
};

// Builds the RTXDI light set of one frame. The sun is included only while it has energy, the same condition under which
// the path tracer samples it. The HDRI is included only while its sampling is enabled and the strategy makes it an RTXDI
// light: the last infinite light (InfiniteLight), or after the infinite region as RTXDI's environment light (Reservoir).
// Fails, leaving `set` untouched, on a table record of an unknown type.
bool BuildRtxdiLightSet(const FrameLighting& lighting, const RtxdiEnvironmentSource& environment,
    RtxdiEnvironmentStrategy strategy, RtxdiLightSet& set);

// Previous/current index translation, as RAB_TranslateLightIndex consumes it: -1 where a light has no reusable
// counterpart. Lights match only by kind and object ID, and only when exactly one light carries that key in each frame:
// removed, added, re-typed and ambiguous lights map to -1, and an object ID is never confused with a table index, so a
// reordered or recycled table slot never stands in for another light.
//
// PT.2B: a matched light's samples stay reusable through moves, rotations and property changes when
// `preserveThroughLightChanges` is set: the sample is re-evaluated with the current light at the current surface, and
// bias correction evaluates it with the previous record at the previous surface (RtxdiApplicationBridge.hlsli), so no
// stale light state is ever shaded. Two exceptions: the HDRI always requires an identical distribution (its previous CDF
// and texture may be gone), and a spot light requires an unchanged cone (position, direction, angles and range; only its
// radiance may change), as reused samples near a moving cone's edge measured noisier than fresh ones. Without the policy
// every light requires its whole identity to be unchanged (pre-PT.2B).
void BuildRtxdiLightTranslation(const RtxdiLightSet& previous, const RtxdiLightSet& current,
    bool preserveThroughLightChanges, std::vector<std::int32_t>& currentToPrevious,
    std::vector<std::int32_t>& previousToCurrent);
