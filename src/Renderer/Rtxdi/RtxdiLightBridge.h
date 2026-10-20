#pragma once

#include "Renderer/ShaderData.h"
#include "Scene/DirectionalLight.h"

#include "Rtxdi/DI/ReSTIRDI.h"

#include <cstdint>
#include <string>
#include <vector>

struct FrameLighting;

// RTXPT.3 R3.2: Engine's lights as RTXDI light indices, for the raster-primary ReSTIR DI variant only. The
// existing deferred and path-tracing light bindings are untouched; a record points back at the data those
// paths already use, so RTXDI evaluates and samples every light with the path tracer's own functions
// (RtxdiLights.hlsli).
//
// Index order is RTXDI's region layout: the local region holds the Point and Spot table lights, the infinite
// region the Directional table lights, then the sun, then the HDRI; each group keeps table order. All table
// lights are delta lights (no artificial radius): RTXDI's sample uv is ignored for them. The sun maps uv onto
// its cone exactly as PathTraceSampleSun does, and the HDRI inverts Engine's marginal/conditional CDF. RTXDI's
// environment region and presampling are not used.
enum class RtxdiLightKind : std::uint32_t
{
    // Equal to the GPULight table types.
    Point = 0,
    Spot = 1,
    Directional = 2,
    Sun = 3,
    Environment = 4
};

// One GPU record per RTXDI light index; RtxdiLights.hlsli matches this layout. sourceIndex is the
// SceneLightConstants index for table lights and 0 for the sun and HDRI, whose data stays in the path
// tracer's frame constants.
struct RtxdiLightRecord
{
    std::uint32_t kind = 0;
    std::uint32_t sourceIndex = 0;
};

static_assert(sizeof(RtxdiLightRecord) == 8);

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

// Everything that decides a light's samples, target PDFs and uv mapping. Only the fields of its kind are set.
// Translation between frames requires the whole identity to be equal: a stable object ID alone never
// authorises reusing a sample whose light moved, changed or was re-parameterised.
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

// Builds the RTXDI light set of one frame. The sun is included only while it has energy and the HDRI only
// while its sampling is enabled, the same conditions under which the path tracer samples them. Fails,
// leaving `set` untouched, on a table record of an unknown type.
bool BuildRtxdiLightSet(const FrameLighting& lighting, const RtxdiEnvironmentSource& environment,
    RtxdiLightSet& set);

// Previous/current index translation, as RAB_TranslateLightIndex consumes it: -1 where a light has no
// reusable counterpart. Lights match only by kind and object ID, and only when exactly one light carries that
// key in each frame and the full identities are equal; removed, added, changed and ambiguous lights map to -1.
void BuildRtxdiLightTranslation(const RtxdiLightSet& previous, const RtxdiLightSet& current,
    std::vector<std::int32_t>& currentToPrevious, std::vector<std::int32_t>& previousToCurrent);
