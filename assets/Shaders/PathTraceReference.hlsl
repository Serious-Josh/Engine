// RTXPT.1 progressive Monte Carlo reference path tracer (experimental, opt-in).
//
// One raygen thread per pixel runs an iterative path loop: camera ray, BSDF continuation and next-event
// estimation at every non-delta vertex. Environment radiance arrives only along traced rays that miss
// all geometry, so an opaque sealed room receives exactly zero environment light. No prefiltered IBL,
// irradiance cube, AO or history from the legacy renderer is read.
//
// Accumulation: accumulationTexture stores the running mean in RGBA32F. With n previous samples and s new
// samples, mean' = mean * n / (n + s) + sum / (n + s). n = 0 ignores previous contents entirely.

// Physical sky S5B: table lights through the clouds' moon shadow (SceneLights.hlsli; SceneLightMoonCloudShadow is defined below).
#define SCENE_LIGHTS_MOON_CLOUD_SHADOW 1
#include "PathTraceBSDF.hlsli"
#include "PathTraceLights.hlsli"
#include "SkyFrame.hlsli"
// Physical sky S5: RtxdiLights.hlsli shades the sun through the cloud shadow (SkyFrameInfoIndexForLights, defined below).
#define SKY_FRAME_HAS_INFO_FUNCTION 1
uint SkyFrameInfoIndexForLights();
#include "PathTracePrimarySurface.hlsli"
#include "RayTracingInstanceData.hlsli"
// PT.2B: who estimates the HDRI at the ReSTIR DI vertex; read by the ReSTIR DI direct pass and by every split indirect pass.
#include "RtxdiEnvironmentConfig.hlsli"

// RTXPT.3: the compute wrappers in DirectLighting/ compile this file with PATH_TRACE_RESTIR_DI_LIBRARY defined, for the
// ReSTIR DI direct pass. Every RTXDI addition below is inside that specialization, so this library (the reference,
// raster-primary, pixel-centre and ReSTIR DI indirect pipelines) contains no RTXDI code and does not depend on the
// RTXDI SDK headers.
#ifdef PATH_TRACE_RESTIR_DI_LIBRARY
// RTXDI types for constants b2 and the application bridge. RTXDI presampling is not used.
#define RTXDI_ENABLE_PRESAMPLING 0
#include "RtxdiLights.hlsli"
#include "Rtxdi/RtxdiParameters.h"
#include "Rtxdi/DI/ReSTIRDIParameters.h"
#endif

// SER S1: SM 6.7+ libraries (the SM69 wrappers) require payload access qualifiers. Both macros expand to
// nothing below SM 6.7, so the lib_6_6 bytecode is unchanged. Every field stays caller-written and caller-read,
// so the caller's initial values survive a miss exactly as they do without qualifiers.
#if __SHADER_TARGET_MAJOR > 6 || (__SHADER_TARGET_MAJOR == 6 && __SHADER_TARGET_MINOR >= 7)
#define PATH_TRACE_RAY_PAYLOAD [raypayload]
#define PATH_TRACE_PAYLOAD_ACCESS(access) : access
#else
#define PATH_TRACE_RAY_PAYLOAD
#define PATH_TRACE_PAYLOAD_ACCESS(access)
#endif

struct PATH_TRACE_RAY_PAYLOAD PathTracePayload
{
    // Negative on a miss.
    float hitDistance PATH_TRACE_PAYLOAD_ACCESS(write(caller, closesthit, miss) : read(caller));
    float3 geometricNormal PATH_TRACE_PAYLOAD_ACCESS(write(caller, closesthit) : read(caller));
    float3 shadingNormal PATH_TRACE_PAYLOAD_ACCESS(write(caller, closesthit) : read(caller));
    float2 uv PATH_TRACE_PAYLOAD_ACCESS(write(caller, closesthit) : read(caller));
    uint materialIndex PATH_TRACE_PAYLOAD_ACCESS(write(caller, closesthit) : read(caller));
};

struct PATH_TRACE_RAY_PAYLOAD PathTraceVisibilityPayload
{
    uint visible PATH_TRACE_PAYLOAD_ACCESS(write(caller, miss) : read(caller));
};

#ifdef PATH_TRACE_PSR_LIBRARY
#include "PsrIdentity.hlsli"

// PSR.1b: PathTracePayload plus the hit's identity, traced only by the guide pass (its own hit group and miss).
struct PATH_TRACE_RAY_PAYLOAD PathTracePsrGuidePayload
{
    float hitDistance PATH_TRACE_PAYLOAD_ACCESS(write(caller, closesthit, miss) : read(caller));
    float3 geometricNormal PATH_TRACE_PAYLOAD_ACCESS(write(caller, closesthit) : read(caller));
    float3 shadingNormal PATH_TRACE_PAYLOAD_ACCESS(write(caller, closesthit) : read(caller));
    float2 uv PATH_TRACE_PAYLOAD_ACCESS(write(caller, closesthit) : read(caller));
    uint materialIndex PATH_TRACE_PAYLOAD_ACCESS(write(caller, closesthit) : read(caller));
    uint instanceID PATH_TRACE_PAYLOAD_ACCESS(write(caller, closesthit) : read(caller));
    uint primitiveIndex PATH_TRACE_PAYLOAD_ACCESS(write(caller, closesthit) : read(caller));
};
#endif

cbuffer PathTraceConstants : register(b0)
{
    row_major float4x4 inverseViewProjection;

    uint2 resolution;
    uint accumulatedSamples;
    uint samplesPerFrame;

    uint maxPathDepth;
    uint russianRouletteDepth;
    uint instanceDataIndex;
    uint materialBufferIndex;

    uint environmentTextureIndex;
    uint environmentConditionalCDFIndex;
    uint environmentMarginalCDFIndex;
    uint environmentWidth;

    uint environmentHeight;
    float environmentTotalWeight;
    uint materialTableIndex;
    uint materialAverageTableIndex;

    float3 sunDirectionToLight;
    float sunCosAngularRadius;

    float3 sunIrradiance;
    uint randomSeed;

    uint materialTableCosineMapping;
    // RTXPT D.1B.2 signal extraction, read only by the *Extract* ray generations: the raw UAVs of the diffuse and
    // specular RGBA16F outputs, and of the per-sample vertex-0 diffuse direct radiance of split ReSTIR DI (laid out
    // like splitDirectRadianceIndex).
    uint diffuseOutputIndex;
    uint specularOutputIndex;
    uint splitDirectDiffuseIndex;

    // RTXPT.2 R3, appended: read only by PathTraceRasterPrimaryRayGeneration.
    row_major float4x4 inverseCameraRelativeViewProjection;

    float3 cameraPosition;
    uint gBufferDepthIndex;

    uint gBufferMaterialIndex;
    uint gBufferSurfaceIndex;
    uint gBufferGeometricNormalIndex;
    // RTXPT.3 ReSTIR DI: the raw UAV of the per-sample vertex-0 direct radiance, written by the direct pass
    // (PathTraceReSTIRDIDirectCS) and read by PathTraceReSTIRDISplitIndirectRayGeneration.
    uint splitDirectRadianceIndex;

#ifdef PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY
    // NRD.RELAX.2, appended: the raw UAV of the R32F raw primary diffuse hit distance, written only by the extraction ray
    // generations; PATH_TRACE_NO_RAW_DIFFUSE_HIT_DISTANCE (the default) writes nothing.
    // The following padding word now holds the optional Reference counter UAV index.
    // Ordinary libraries reserve the same row so its offset is always 276.
    uint rawDiffuseHitDistanceIndex;
    uint referenceRayCountIndex;
#ifdef PATH_TRACE_PSR_LIBRARY
    // PSR.2 Stage 3B: a PSR library's raw-distance build keeps the PSR words at the same offsets (280, 284).
    uint psrRecordIndex;
    uint psrPadding;
#else
    uint2 rawDiffuseHitDistancePadding;
#endif
#else
    uint unusedRawDiffuseHitDistanceIndex;
    uint referenceRayCountIndex;
#ifdef PATH_TRACE_PSR_LIBRARY
    // PSR.1a, in the same padding row: the raw UAV of the per-pixel replacement records (PATH_TRACE_PSR_RECORD_STRIDE),
    // written by PathTracePsrRayGeneration and read by this library's PathTraceRasterPrimaryRayGeneration.
    uint psrRecordIndex;
    uint psrPadding;
#else
    uint2 referenceRayCountPadding;
#endif
#endif

    // Physical sky S3, appended: the published sky-view map for camera rays and delta-mirror misses, or 0xFFFFFFFF.
    uint skyBackgroundIndex;
    // S4: 1 when skyBackgroundIndex is an HDRI sky's background equirect, 0 for a sky-view map.
    uint skyBackgroundEquirect;
    // S5: the physical sky's frame information (SkyFrame.hlsli), or 0xFFFFFFFF.
    uint skyFrameInfoIndex;
    uint skyBackgroundPadding;
};

cbuffer SceneLightConstants : register(b1)
{
    uint sceneLightCount;
    uint sceneLightBufferIndex;
    uint2 sceneLightPadding;
};

#ifdef PATH_TRACE_SAMPLED_NEE
// M4.2A experimental sampled indirect NEE: compiled only into the M42A twin libraries (ENGINE_SAMPLED_NEE=1); production
// libraries never read sceneLightPadding. sceneLightPadding.x is the shared light-sampling buffer's descriptor index
// (LightSamplingBuffer, published with this frame's light table) and sceneLightPadding.y the mode:
//   bit 0      sampled finite-light NEE at vertices >= 1 (otherwise the exhaustive loop below);
//   bits 1-4   K, the RIS candidate count (2..8);
//   bit 5      test only: no indirect luminance clamp on next-event estimates;
//   bit 6      test only: no indirect luminance clamp anywhere (next-event estimates, misses, cache queries);
//   bits 7-8   M4.2B clamp policy for the sampled finite-light estimate: 0 joint (clamped with the vertex's sun, HDRI and
//              directional estimates, as exhaustive NEE), 1 exempt (only the rest is clamped), 2 separate (each part clamped
//              on its own);
//   bits 9-10  M4.2B RIS target tempering: p_hat^gamma with gamma 1, 0.75 or 0.5 (any target positive wherever the
//              contribution is keeps the estimator unbiased; a flatter one lets occluded bright lights dominate less).
#include "LightSampling.hlsli"

static const uint PATH_TRACE_SAMPLED_NEE_ENABLED = 1u;
static const uint PATH_TRACE_SAMPLED_NEE_CANDIDATE_SHIFT = 1u;
static const uint PATH_TRACE_SAMPLED_NEE_CANDIDATE_MASK = 0xFu;
static const uint PATH_TRACE_SAMPLED_NEE_NO_NEE_CLAMP = 1u << 5;
static const uint PATH_TRACE_SAMPLED_NEE_NO_CLAMP = 1u << 6;
static const uint PATH_TRACE_SAMPLED_NEE_CLAMP_POLICY_SHIFT = 7u;
static const uint PATH_TRACE_SAMPLED_NEE_CLAMP_JOINT = 0u;
static const uint PATH_TRACE_SAMPLED_NEE_CLAMP_EXEMPT = 1u;
static const uint PATH_TRACE_SAMPLED_NEE_CLAMP_SEPARATE = 2u;
static const uint PATH_TRACE_SAMPLED_NEE_TEMPER_SHIFT = 9u;
// M4.2C: SHARC training's own switch and clamp policy, so its estimator is chosen apart from the image's.
static const uint PATH_TRACE_SAMPLED_NEE_TRAINING = 1u << 11;
static const uint PATH_TRACE_SAMPLED_NEE_TRAINING_CLAMP_POLICY_SHIFT = 12u;

// The mode bit that enables the sampled estimator in this library: SHARC training's, or the image's.
#ifdef PATH_TRACE_SHARC_NV_TRAINING
#define PATH_TRACE_SAMPLED_NEE_ACTIVE PATH_TRACE_SAMPLED_NEE_TRAINING
#else
#define PATH_TRACE_SAMPLED_NEE_ACTIVE PATH_TRACE_SAMPLED_NEE_ENABLED
#endif

// M4.2B: the sampled finite-light estimate of the current vertex when the clamp policy keeps it apart from the rest of
// the vertex's next-event estimate (exempt or separate), consumed and reset by the vertex loop's clamp.
static float3 g_pathTraceSampledFinite = float3(0.0f, 0.0f, 0.0f);
static float3 g_pathTraceSampledFiniteDiffuse = float3(0.0f, 0.0f, 0.0f);

uint PathTraceSampledNeeClampPolicy()
{
#ifdef PATH_TRACE_SHARC_NV_TRAINING
    return (sceneLightPadding.y >> PATH_TRACE_SAMPLED_NEE_TRAINING_CLAMP_POLICY_SHIFT) & 3u;
#else
    return (sceneLightPadding.y >> PATH_TRACE_SAMPLED_NEE_CLAMP_POLICY_SHIFT) & 3u;
#endif
}

// The RIS target from the unshadowed contribution's luminance, tempered per the mode.
float PathTraceSampledNeeTarget(float luminance)
{
    const uint temper = (sceneLightPadding.y >> PATH_TRACE_SAMPLED_NEE_TEMPER_SHIFT) & 3u;
    return temper == 0u ? luminance : (temper == 1u ? pow(luminance, 0.75f) : sqrt(luminance));
}

// The path vertex whose next-event estimate is being computed (set by the vertex loop; 0 elsewhere, so primary and
// non-loop callers keep the exhaustive estimator).
static uint g_pathTraceNeeVertex = 0u;

#endif

#ifdef PATH_TRACE_RESTIR_DI_LIBRARY
// RTXPT.3, read only by the ReSTIR DI direct pass (PathTraceReSTIRDIDirectCS), whose pipeline alone binds b2.
// Identical layout to PathTraceRtxdiConstants (RtxdiShaderData.h).
cbuffer RtxdiConstants : register(b2)
{
    RTXDI_LightBufferParameters rtxdiLightBuffer;
    RTXDI_ReservoirBufferParameters rtxdiReservoirBuffer;
    RTXDI_DIBufferIndices rtxdiBufferIndices;

    uint rtxdiReservoirBufferIndex;
    uint rtxdiLocalLightSamples;
    uint rtxdiInfiniteLightSamples;
    // WNP.2: 1 shadow-tests the selected initial sample before reuse (RtxdiBridgeInitialVisible).
    uint rtxdiInitialVisibility;

    // PT.2B: the raw SRV of the frame's light data (RtxdiLightDataLayout: current records, previous records, then the
    // current-to-previous and previous-to-current translation) and its two record counts.
    uint rtxdiLightDataIndex;
    uint rtxdiCurrentLightCount;
    uint rtxdiPreviousLightCount;
    // PT.2B, Reservoir environment strategy only: environment and BSDF candidates per path sample.
    uint rtxdiEnvironmentSamples;

    uint rtxdiBrdfSamples;
    // 1 when previous reservoirs, previous surfaces, previous light records and the translation may be read.
    uint rtxdiHistoryValid;
    uint rtxdiNeighborOffsetsIndex;
    uint rtxdiMotionIndex;

    // R3.5/R3.6 fused spatiotemporal reuse.
    RTXDI_RuntimeParameters rtxdiRuntime;
    RTXDI_DISpatioTemporalResamplingParameters rtxdiSpatioTemporal;

    uint rtxdiPreviousDepthIndex;
    uint rtxdiPreviousSurfaceIndex;
    uint rtxdiPreviousGeometricNormalIndex;
    uint rtxdiPreviousMaterialIndex;

    // PT.2A: 1 lets final shading reuse the initial-visibility result for a provably identical ray; 2 (diagnostics) also
    // traces it and counts disagreements (RTXDI_DIAGNOSTIC_FINAL_REUSE_MISMATCHES).
    uint rtxdiReuseInitialVisibility;
    // PT.2A diagnostics: RWByteAddressBuffer of RTXDI_DIAGNOSTIC_* counters, or 0xFFFFFFFF (none).
    uint rtxdiDiagnosticCountersIndex;
    uint rtxdiPadding0;
    uint rtxdiPadding1;

    float3 rtxdiPreviousCameraPosition;
    float rtxdiPadding2;
    float3 rtxdiCameraForward;
    float rtxdiPadding3;
    float3 rtxdiPreviousCameraForward;
    float rtxdiPadding4;

    row_major float4x4 rtxdiPreviousInverseCameraRelativeViewProjection;
};
#endif

SamplerState materialSampler : register(s0);
SamplerState environmentSampler : register(s1);

RaytracingAccelerationStructure scene : register(t0);

RWTexture2D<float4> accumulationTexture : register(u0);
RWTexture2D<float4> outputTexture : register(u1);
// [0..1] rays traced (64-bit), [2..3] completed path samples (64-bit), [4] rejected non-finite samples.
RWTexture2D<uint> statisticsTexture : register(u2);

static const uint PATH_TRACE_MISS_RADIANCE = 0u;
static const uint PATH_TRACE_MISS_VISIBILITY = 1u;
static const float PATH_TRACE_HALF_MAXIMUM = 65504.0f;

// -----------------------------------------------------------------------------
// Random numbers: PCG32, seeded per pixel and per global sample index.
// -----------------------------------------------------------------------------

uint PathTraceHash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

struct PathTraceRandom
{
    uint state;
};

PathTraceRandom PathTraceCreateRandom(uint2 pixel, uint sampleIndex)
{
    PathTraceRandom random;
    random.state = PathTraceHash(pixel.x ^ PathTraceHash(pixel.y ^ PathTraceHash(sampleIndex ^ PathTraceHash(randomSeed))));
    return random;
}

float PathTraceNextFloat(inout PathTraceRandom random)
{
    random.state = random.state * 747796405u + 2891336453u;
    uint word = ((random.state >> ((random.state >> 28u) + 4u)) ^ random.state) * 277803737u;
    word = (word >> 22u) ^ word;
    return float(word >> 8u) * (1.0f / 16777216.0f);
}

#ifdef PATH_TRACE_SAMPLED_NEE
// The same PCG step as PathTraceNextFloat, returning the whole uniformly distributed 32-bit output word (the light-sampling
// draws' probabilities assume uniform uint32 inputs).
uint PathTraceNextUint(inout PathTraceRandom random)
{
    random.state = random.state * 747796405u + 2891336453u;
    uint word = ((random.state >> ((random.state >> 28u) + 4u)) ^ random.state) * 277803737u;
    return (word >> 22u) ^ word;
}
#endif

float2 PathTraceNextFloat2(inout PathTraceRandom random)
{
    float x = PathTraceNextFloat(random);
    float y = PathTraceNextFloat(random);
    return float2(x, y);
}

float3 PathTraceNextFloat3(inout PathTraceRandom random)
{
    float x = PathTraceNextFloat(random);
    float y = PathTraceNextFloat(random);
    float z = PathTraceNextFloat(random);
    return float3(x, y, z);
}

float4 PathTraceNextFloat4(inout PathTraceRandom random)
{
    float2 first = PathTraceNextFloat2(random);
    float2 second = PathTraceNextFloat2(random);
    return float4(first, second);
}

// -----------------------------------------------------------------------------
// Scene access
// -----------------------------------------------------------------------------

PathTraceEnvironment PathTraceFrameEnvironment()
{
    PathTraceEnvironment environment;
    environment.textureIndex = environmentTextureIndex;
    environment.conditionalCDFIndex = environmentConditionalCDFIndex;
    environment.marginalCDFIndex = environmentMarginalCDFIndex;
    environment.width = environmentWidth;
    environment.height = environmentHeight;
    environment.totalWeight = environmentTotalWeight;
    return environment;
}

uint SkyFrameInfoIndexForLights()
{
    return skyFrameInfoIndex;
}

float SceneLightMoonCloudShadow(float3 surfacePosition)
{
    return SkyFrameMoonCloudShadow(skyFrameInfoIndex, surfacePosition);
}

PathTraceSun PathTraceFrameSun()
{
    PathTraceSun sun;
    sun.directionToSun = sunDirectionToLight;
    sun.cosAngularRadius = sunCosAngularRadius;
    sun.irradiance = sunIrradiance;
    return sun;
}

#ifdef PATH_TRACE_RESTIR_DI_LIBRARY
// -----------------------------------------------------------------------------
// RTXPT.3 RTXDI application bridge inputs (ReSTIR DI variant only): the frame's own lights and HDRI.
// -----------------------------------------------------------------------------

// PT.2B: a light of this frame's set, or (previousFrame) of the set the previous reservoirs were computed with, from the
// light data. Previous records exist only while history is valid; any index outside its frame's set is no light.
GPULight RtxdiBridgeLoadLight(uint lightIndex, bool previousFrame)
{
    uint count = previousFrame ? (rtxdiHistoryValid != 0u ? rtxdiPreviousLightCount : 0u) : rtxdiCurrentLightCount;

    if(lightIndex >= count)
    {
        return RtxdiInvalidLightRecord();
    }

    ByteAddressBuffer lightData = ResourceDescriptorHeap[rtxdiLightDataIndex];
    uint first = previousFrame ? rtxdiCurrentLightCount : 0u;
    return RtxdiLoadLightRecord(lightData, (first + lightIndex) * RTXDI_LIGHT_RECORD_BYTES);
}

PathTraceEnvironment RtxdiBridgeEnvironment()
{
    return PathTraceFrameEnvironment();
}

RWStructuredBuffer<RTXDI_PackedDIReservoir> RtxdiReservoirs()
{
    return ResourceDescriptorHeap[rtxdiReservoirBufferIndex];
}

// The light's index in the other frame (RtxdiLightBridge.h BuildRtxdiLightTranslation): -1 when it has no reusable
// counterpart there.
int RtxdiBridgeTranslateLightIndex(int lightIndex, bool currentToPrevious)
{
    uint count = currentToPrevious ? rtxdiCurrentLightCount : rtxdiPreviousLightCount;

    if(rtxdiHistoryValid == 0u || lightIndex < 0 || uint(lightIndex) >= count)
    {
        return -1;
    }

    ByteAddressBuffer lightData = ResourceDescriptorHeap[rtxdiLightDataIndex];
    uint entry = currentToPrevious ? uint(lightIndex) : rtxdiCurrentLightCount + uint(lightIndex);
    uint translationOffset = (rtxdiCurrentLightCount + rtxdiPreviousLightCount) * RTXDI_LIGHT_RECORD_BYTES;
    return asint(lightData.Load(translationOffset + entry * 4u));
}

#define RTXDI_BRIDGE_ENVIRONMENT_SAMPLER environmentSampler
#define RTXDI_LIGHT_RESERVOIR_BUFFER RtxdiReservoirs()
#define RTXDI_BRIDGE_TRACED_INITIAL_VISIBILITY 1
#include "RtxdiApplicationBridge.hlsli"
#endif

// Ray Tracing Gems, chapter 6 (Waechter and Binder): scale-independent self-intersection offset.
float3 PathTraceOffsetRayOrigin(float3 position, float3 normal)
{
    const float originThreshold = 1.0f / 32.0f;
    const float floatScale = 1.0f / 65536.0f;
    const float integerScale = 256.0f;

    int3 integerOffset = int3(integerScale * normal);
    float3 integerOffsetPosition = asfloat(asint(position) + select(position < 0.0f, -integerOffset, integerOffset));
    return select(abs(position) < originThreshold, position + floatScale * normal, integerOffsetPosition);
}

// Enabled only by the Reference raygen. Other modes never write these counters.
static bool g_referenceRayCounts = false;
void PathTraceCountReferenceRay(uint category)
{
    if(g_referenceRayCounts)
    {
        RWByteAddressBuffer counts = ResourceDescriptorHeap[referenceRayCountIndex];
        uint previous;
        counts.InterlockedAdd(category * 8u, 1u, previous);
        // A carry pair works on SM6.6 without requiring 64-bit atomic feature support.
        if(previous == 0xFFFFFFFFu) counts.InterlockedAdd(category * 8u + 4u, 1u);
    }
}

bool PathTraceVisible(float3 position, float3 geometricNormal, float3 direction, float distance, inout uint rayCount)
{
    RayDesc ray;
    ray.Origin = PathTraceOffsetRayOrigin(position, geometricNormal);
    ray.Direction = direction;
    ray.TMin = 0.0f;
    // A positional light is reached when nothing intersects before it; stop just short of it.
    ray.TMax = distance >= PATH_TRACE_DISTANT_LIGHT_DISTANCE ? PATH_TRACE_DISTANT_LIGHT_DISTANCE :
        distance * (1.0f - 1.0e-4f);

#if (defined(PATH_TRACE_PRIMARY_DI_SHADOW_RAY_QUERY) && defined(PATH_TRACE_RESTIR_DI_LIBRARY)) || \
    (defined(PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY) && !defined(PATH_TRACE_RESTIR_DI_LIBRARY))
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    PathTraceCountReferenceRay(2u);
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while(query.Proceed())
    {
        if(query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            query.CommitNonOpaqueTriangleHit();
        }
    }
    ++rayCount;
    return query.CommittedStatus() == COMMITTED_NOTHING;
#else
    PathTraceVisibilityPayload payload;
    payload.visible = 0u;
    PathTraceCountReferenceRay(2u);
    TraceRay(scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF, 0, 1,
        PATH_TRACE_MISS_VISIBILITY, ray, payload);
    ++rayCount;
    return payload.visible != 0u;
#endif
}

float PathTraceResolveMetallic(GPUMaterial material, float2 uv, bool useARM, float sampledARMMetallic)
{
    if(material.model != 0u)
    {
        return material.metallic;
    }

    float metallic = saturate(material.metallic);

    if(useARM)
    {
        return saturate(metallic * sampledARMMetallic);
    }

    if(material.metallicTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> metallicTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.metallicTextureIndex)];
        return saturate(metallic * metallicTexture.SampleLevel(materialSampler, uv, 0.0f).r);
    }

    return metallic;
}

// Resolves the material exactly as the G-buffer pixel shader does (base color, ARM/roughness/metallic
// textures), at texture LOD 0. Parallax UV offsets and material AO are not applied (see the report).
bool PathTraceBuildSurface(PathTracePayload payload, float3 rayDirection, out PathTraceSurface surface)
{
    surface = (PathTraceSurface)0;
    surface.view = -rayDirection;
    surface.geometricNormal = payload.geometricNormal;
    surface.normal = payload.shadingNormal;
    surface.materialTableIndex = materialTableIndex;
    surface.materialAverageTableIndex = materialAverageTableIndex;
    surface.materialTableCosineMapping = materialTableCosineMapping;

    // Surfaces are two-sided: face both normals towards the arriving ray.
    if(dot(surface.geometricNormal, surface.view) < 0.0f)
    {
        surface.geometricNormal = -surface.geometricNormal;
        surface.normal = -surface.normal;
    }

    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[materialBufferIndex];
    uint materialCount;
    uint materialStride;
    materials.GetDimensions(materialCount, materialStride);

    if(payload.materialIndex >= materialCount || !all(isfinite(payload.uv)))
    {
        return false;
    }

    GPUMaterial material = materials[payload.materialIndex];

    if(material.model > 2u)
    {
        return false;
    }

    Texture2D<float4> baseColorTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.textureIndex)];
    surface.baseColor = max(material.baseColor * baseColorTexture.SampleLevel(materialSampler, payload.uv, 0.0f).rgb, 0.0f);

    bool useARM = material.armTextureIndex != 0xFFFFFFFFu && material.preferARM != 0u;
    float2 armRoughnessMetallic = float2(1.0f, 1.0f);

    if(useARM)
    {
        Texture2D<float4> armTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.armTextureIndex)];
        armRoughnessMetallic = armTexture.SampleLevel(materialSampler, payload.uv, 0.0f).gb;
    }

    float roughness = material.roughness;

    if(useARM)
    {
        roughness = saturate(roughness * armRoughnessMetallic.x);
    }
    else if(material.roughnessTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> roughnessTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.roughnessTextureIndex)];
        roughness = saturate(roughness * roughnessTexture.SampleLevel(materialSampler, payload.uv, 0.0f).r);
    }

    material.roughness = roughness;
    material.metallic = PathTraceResolveMetallic(material, payload.uv, useARM, armRoughnessMetallic.y);
    surface.material = material;

    if(!all(isfinite(surface.baseColor)) || !isfinite(material.roughness) || !isfinite(material.metallic))
    {
        return false;
    }

    return PathTraceFinalizeSurface(surface);
}

// -----------------------------------------------------------------------------
// Transport
// -----------------------------------------------------------------------------

// Indirect luminance clamp: the scene-linear luminance above which one indirect contribution (last scattering vertex
// >= 1) is scaled down to it, preserving RGB ratios. Primary direct lighting, ReSTIR DI and camera-ray misses are exempt.
static const float PATH_TRACE_INDIRECT_LUMINANCE_CLAMP = 10.0f;

// True when a contribution whose last scattering vertex is `vertex` is subject to the clamp.
bool PathTraceIndirectClampActive(uint vertex)
{
    return vertex >= 1u;
}

// The factor that brings contribution's luminance down to the threshold (1 at or below it). The same factor scales
// the contribution's diffuse-extraction share and its radiance-cache harvest, so the signals stay consistent.
float PathTraceIndirectClampScale(float3 contribution)
{
#ifdef PATH_TRACE_SAMPLED_NEE
    if((sceneLightPadding.y & PATH_TRACE_SAMPLED_NEE_NO_CLAMP) != 0u)
    {
        return 1.0f;
    }
#endif

    float luminance = PathTraceLuminance(contribution);
    return luminance > PATH_TRACE_INDIRECT_LUMINANCE_CLAMP ? PATH_TRACE_INDIRECT_LUMINANCE_CLAMP / luminance : 1.0f;
}

// PT.2B Approach B (RTXDI_ENVIRONMENT_STRATEGY_SEPARATE_MIS): only the HDRI term of PathTraceMissRadiance for a non-delta
// continuation, power-heuristic weighted against the HDRI light sample exactly as there.
float3 PathTraceMissEnvironmentRadiance(float3 direction, float previousPdf)
{
    PathTraceEnvironment environment = PathTraceFrameEnvironment();
    float3 environmentRadiance = PathTraceEnvironmentRadiance(environment, environmentSampler, direction);
    float environmentWeight = PathTraceEnvironmentSamplingEnabled(environment) ?
        PathTracePowerHeuristic(previousPdf, PathTraceEnvironmentPdf(environment, direction)) : 1.0f;
    return environmentRadiance * environmentWeight;
}

// Sun disk edge (2026-11-25): the angular size of one camera pixel toward a direction (radians): the pixel at the view axis (two
// pixel-centre rays through the frame's inverse view-projection, as PathTraceCameraRay builds them), times cos^2 of the direction's
// angle from the axis, as a pinhole's pixel shrinks off axis. The visible sun disk's edge is filtered over it.
float PathTraceCameraPixelAngle(float3 direction)
{
    const float2 step = 2.0f / float2(resolution);
    const float4 axisNear = mul(inverseViewProjection, float4(0.0f, 0.0f, 0.0f, 1.0f));
    const float4 axisFar = mul(inverseViewProjection, float4(0.0f, 0.0f, 1.0f, 1.0f));
    const float4 nextFar = mul(inverseViewProjection, float4(0.0f, step.y, 1.0f, 1.0f));
    const float3 origin = axisNear.xyz / axisNear.w;
    const float3 axis = normalize(axisFar.xyz / axisFar.w - origin);
    const float3 next = normalize(nextFar.xyz / nextFar.w - origin);
    const float cosAxis = saturate(dot(normalize(direction), axis));
    return length(next - axis) * cosAxis * cosAxis;
}

// S5: origin is the vertex the ray left (world metres): the clouds' shadow on a BSDF-sampled sun. cameraRay: the camera's own ray,
// whose clouds the full-resolution composite adds; a delta continuation sees the published set's cloud map instead.
float3 PathTraceMissRadiance(float3 origin, float3 direction, bool previousDelta, float previousPdf, bool cameraRay)
{
    PathTraceEnvironment environment = PathTraceFrameEnvironment();
    PathTraceSun sun = PathTraceFrameSun();

    float3 environmentRadiance = PathTraceEnvironmentRadiance(environment, environmentSampler, direction);
    float3 sunRadiance = PathTraceSunRadiance(sun, direction);

    if(previousDelta)
    {
        // Camera rays and delta-mirror continuations: no light-sampling strategy could produce them, so they may show the
        // sharper sky-view map instead of the sampled equirect (Physical sky S3), or an HDRI sky's higher-resolution background
        // (S4). The disk is shown within the display maximum (Physical sky S2).
        bool clearSkyView = false;

        if(skyBackgroundIndex != 0xFFFFFFFFu)
        {
            if(skyBackgroundEquirect != 0u)
            {
                Texture2D<float4> background = ResourceDescriptorHeap[skyBackgroundIndex];
                environmentRadiance = background.SampleLevel(environmentSampler, DirectionToEquirectangularUV(direction), 0.0f).rgb;
            }
            else
            {
                environmentRadiance = SampleSkyView(skyBackgroundIndex, direction);
                clearSkyView = true;
            }
        }

        // Physical sky S5: the stars and the moon's disk per pixel. The sky-view map is cloud-free: a delta continuation adds the
        // published set's clouds over it; the camera's own rays get theirs from the full-resolution composite (never both).
        // Sun disk edge (2026-11-25): the disk limb-darkened and filtered over the camera pixel's footprint (Common.hlsli's
        // SunDiskDisplayRadiance), evaluated within a degree of the disk only.
        // The stars keep footprint 0 here (the samples accumulate, S5B); the moon's disk edge is filtered like the sun's.
        const float pixelAngle = PathTraceCameraPixelAngle(direction);
        float3 sunDisk = float3(0.0f, 0.0f, 0.0f);

        if(!PathTraceSunIsDelta(sun) && PathTraceSunHasEnergy(sun) &&
            dot(direction, sun.directionToSun) >= cos(acos(sun.cosAngularRadius) + 0.0175f))
        {
            const float sinSquared = (1.0f - sun.cosAngularRadius) * (1.0f + sun.cosAngularRadius);
            sunDisk = SunDiskDisplayRadiance(sun.irradiance / (PI * sinSquared), sun.directionToSun, direction, sun.cosAngularRadius,
                pixelAngle);
        }

        float3 background = environmentRadiance + sunDisk + SkyFrameNightBackground(skyFrameInfoIndex, direction, 0.0f, pixelAngle);

        if(clearSkyView && !cameraRay)
        {
            const float4 clouds = SkyFrameCloudEnvironment(skyFrameInfoIndex, direction);
            background = background * clouds.a + clouds.rgb;
        }

        return background;
    }

    float environmentWeight = PathTraceEnvironmentSamplingEnabled(environment) ?
        PathTracePowerHeuristic(previousPdf, PathTraceEnvironmentPdf(environment, direction)) : 1.0f;
    float sunWeight = PathTracePowerHeuristic(previousPdf, PathTraceSunPdf(sun, direction));
    // S5: the clouds' shadow on the sun, as on its light samples.
    return environmentRadiance * environmentWeight + sunRadiance * sunWeight * SkyFrameCloudShadow(skyFrameInfoIndex, origin);
}

// Adds one light sample: f * cos * Li * visibility * MIS / pdf. Delta lights (pdf 0) use weight 1. diffuseRadiance
// is the same expression with the BSDF's diffuse lobe in place of f (signal extraction only): the MIS weight and
// the densities are those of the full estimator, so the two lobes' parts of a sample are split exactly.
float3 PathTraceShadeLightSampleLobes(PathTraceSurface surface, float3 position, PathTraceLightSample lightSample,
    inout uint rayCount, out float3 diffuseRadiance)
{
    diffuseRadiance = float3(0.0f, 0.0f, 0.0f);

    if(!lightSample.valid || !PathTraceDirectionAboveSurface(surface, lightSample.direction))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 diffuseValue;
    float cosine = dot(surface.normal, lightSample.direction);
    float3 value = PathTraceEvaluateBSDFLobes(surface, lightSample.direction, diffuseValue) * cosine *
        lightSample.radiance;

    if(!any(value > 0.0f))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float weight = 1.0f;

    if(lightSample.pdf > 0.0f)
    {
        weight = PathTracePowerHeuristic(lightSample.pdf, PathTracePdf(surface, lightSample.direction)) / lightSample.pdf;
    }

    if(!PathTraceVisible(position, surface.geometricNormal, lightSample.direction, lightSample.distance, rayCount))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    diffuseRadiance = diffuseValue * cosine * lightSample.radiance * weight;
    return value * weight;
}

float3 PathTraceShadeLightSample(PathTraceSurface surface, float3 position, PathTraceLightSample lightSample,
    inout uint rayCount)
{
    float3 diffuseRadiance;
    return PathTraceShadeLightSampleLobes(surface, position, lightSample, rayCount, diffuseRadiance);
}

#ifdef PATH_TRACE_SAMPLED_NEE
// PathTraceShadeLightSampleLobes before its visibility test: the light sample's unshadowed contribution f * cos * Li (and
// its diffuse-lobe part), zero exactly where the shade helper returns zero without tracing. Table lights are delta lights
// (pdf 0), so the shade helper's MIS weight is one for them.
float3 PathTraceUnshadowedLightSampleLobes(PathTraceSurface surface, PathTraceLightSample lightSample,
    out float3 diffuseValue)
{
    diffuseValue = float3(0.0f, 0.0f, 0.0f);

    if(!lightSample.valid || !PathTraceDirectionAboveSurface(surface, lightSample.direction))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 diffuseLobe;
    const float cosine = dot(surface.normal, lightSample.direction);
    const float3 value = PathTraceEvaluateBSDFLobes(surface, lightSample.direction, diffuseLobe) * cosine *
        lightSample.radiance;

    if(!any(value > 0.0f))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    diffuseValue = diffuseLobe * cosine * lightSample.radiance;
    return value;
}

// M4.2A sampled table-light NEE at an indirect vertex: the directional table lights enumerated as in the exhaustive loop,
// then fixed-allocation RIS over the disjoint finite-light pools (LightSamplingAllocate) with one visibility test for the
// selected light. Candidate weights w = p_hat / (K_t p_t), p_hat the luminance of the unshadowed contribution; the
// selected light Y contributes f(Y) V(Y) * sum(w) / p_hat(Y). Adds to radiance and diffuseRadiance and returns true; returns
// false (nothing added, no random numbers drawn) when the exhaustive loop must run instead: mode off, a primary vertex,
// or no light-sampling structures published for exactly this light table.
bool PathTraceSampledTableLighting(PathTraceSurface surface, float3 position, inout PathTraceRandom random,
    inout uint rayCount, inout float3 radiance, inout float3 diffuseRadiance)
{
    const uint mode = sceneLightPadding.y;

    if((mode & PATH_TRACE_SAMPLED_NEE_ACTIVE) == 0u || g_pathTraceNeeVertex == 0u)
    {
        return false;
    }

    const LightSamplingConstants sampling = LightSamplingLoadConstants(sceneLightPadding.x);

    if(sampling.valid == 0u || sampling.tableLightCount != sceneLightCount)
    {
        return false;
    }

    float3 sampleDiffuse;

    for(uint directional = 0u; directional < sampling.directionalCount; ++directional)
    {
        const uint lightIndex = LightSamplingDirectionalLight(sampling, directional);
        PathTraceLightSample lightSample = PathTraceSampleTableLight(SceneLightLoad(sceneLightBufferIndex, lightIndex),
            position);
        radiance += PathTraceShadeLightSampleLobes(surface, position, lightSample, rayCount, sampleDiffuse);
        diffuseRadiance += sampleDiffuse;
    }

    const LightSamplingPools pools = LightSamplingPoolsAt(sampling, position);
    const uint candidates = clamp((mode >> PATH_TRACE_SAMPLED_NEE_CANDIDATE_SHIFT) & PATH_TRACE_SAMPLED_NEE_CANDIDATE_MASK,
        2u, 8u);
    const LightSamplingAllocation allocation = LightSamplingAllocate(sampling, pools, candidates);
    const uint candidateCount = LightSamplingCandidateCount(allocation);
    float weightSum = 0.0f;
    float selectedTarget = 0.0f;
    float3 selectedValue = float3(0.0f, 0.0f, 0.0f);
    float3 selectedDiffuse = float3(0.0f, 0.0f, 0.0f);
    PathTraceLightSample selected = (PathTraceLightSample)0;

    [loop]
    for(uint candidateIndex = 0u; candidateIndex < candidateCount; ++candidateIndex)
    {
        const uint random0 = PathTraceNextUint(random);
        const uint random1 = PathTraceNextUint(random);
        const float selectionRandom = PathTraceNextFloat(random);
        const LightSamplingCandidate candidate = LightSamplingDrawCandidate(sampling, pools, allocation, candidateIndex,
            random0, random1);

        if(!candidate.valid)
        {
            continue;
        }

        const PathTraceLightSample lightSample = PathTraceSampleTableLight(SceneLightLoad(sceneLightBufferIndex,
            candidate.light), position);
        float3 diffuseValue;
        const float3 value = PathTraceUnshadowedLightSampleLobes(surface, lightSample, diffuseValue);
        const float target = PathTraceSampledNeeTarget(PathTraceLuminance(value));
        const float weight = target / candidate.density;

        // A zero, negative or non-finite weight is a candidate that contributes nothing; it still counts in K.
        if(!(weight > 0.0f) || !isfinite(weight) || !all(isfinite(value)))
        {
            continue;
        }

        weightSum += weight;

        if(selectionRandom * weightSum < weight)
        {
            selected = lightSample;
            selectedTarget = target;
            selectedValue = value;
            selectedDiffuse = diffuseValue;
        }
    }

    if(weightSum > 0.0f && isfinite(weightSum) &&
        PathTraceVisible(position, surface.geometricNormal, selected.direction, selected.distance, rayCount))
    {
        const float scale = weightSum / selectedTarget;

        // M4.2B: with the exempt or separate clamp policy the vertex loop clamps this estimate apart from the rest.
        if(PathTraceSampledNeeClampPolicy() == PATH_TRACE_SAMPLED_NEE_CLAMP_JOINT)
        {
            radiance += selectedValue * scale;
            diffuseRadiance += selectedDiffuse * scale;
        }
        else
        {
            g_pathTraceSampledFinite = selectedValue * scale;
            g_pathTraceSampledFiniteDiffuse = selectedDiffuse * scale;
        }
    }

    return true;
}
#endif

float3 PathTraceDirectLightingLobes(PathTraceSurface surface, float3 position, inout PathTraceRandom random,
    inout uint rayCount, out float3 diffuseRadiance)
{
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    diffuseRadiance = float3(0.0f, 0.0f, 0.0f);
    float3 sampleDiffuse;

    // Random numbers are consumed unconditionally so sample streams stay aligned across scenes.
    float2 sunRandom = PathTraceNextFloat2(random);
    float4 environmentRandom = PathTraceNextFloat4(random);

    PathTraceSun sun = PathTraceFrameSun();

    if(PathTraceSunHasEnergy(sun))
    {
        // S5: through the clouds' shadow at this vertex.
        PathTraceLightSample sunSample = PathTraceSampleSun(sun, sunRandom);
        sunSample.radiance *= SkyFrameCloudShadow(skyFrameInfoIndex, position);
        radiance += PathTraceShadeLightSampleLobes(surface, position, sunSample, rayCount, sampleDiffuse);
        diffuseRadiance += sampleDiffuse;
    }

    uint lightCount = sceneLightCount;

#ifdef PATH_TRACE_SAMPLED_NEE
    // M4.2A: the sampled estimator replaces the table loop below at indirect vertices when it is active.
    if(PathTraceSampledTableLighting(surface, position, random, rayCount, radiance, diffuseRadiance))
    {
        lightCount = 0u;
    }
#endif

    for(uint lightIndex = 0u; lightIndex < lightCount; ++lightIndex)
    {
        PathTraceLightSample lightSample = PathTraceSampleTableLight(SceneLightLoad(sceneLightBufferIndex, lightIndex), position);
        radiance += PathTraceShadeLightSampleLobes(surface, position, lightSample, rayCount, sampleDiffuse);
        diffuseRadiance += sampleDiffuse;
    }

    PathTraceEnvironment environment = PathTraceFrameEnvironment();

    if(PathTraceEnvironmentSamplingEnabled(environment))
    {
        PathTraceLightSample environmentSample = PathTraceSampleEnvironment(environment, environmentSampler,
            environmentRandom);
        radiance += PathTraceShadeLightSampleLobes(surface, position, environmentSample, rayCount, sampleDiffuse);
        diffuseRadiance += sampleDiffuse;
    }

    return radiance;
}

#ifdef PATH_TRACE_SHARC_NV_TRAINING
// NVIDIA SHARC training's next-event estimation: the sun and environment as PathTraceDirectLightingLobes, but one table
// light instead of all of them. A single-pass weighted reservoir picks light i with probability w_i / sum(w), where w_i
// is the luminance of its unshadowed incident radiance here (no rays). w_i is zero only where the light contributes
// nothing (out of range, outside its cone), so dividing the shaded sample by its probability is unbiased. Table lights
// are delta lights (pdf 0), so no BSDF-sampled MIS weight depends on the selection.
float3 PathTraceSharcNvTrainingDirectLighting(PathTraceSurface surface, float3 position, inout PathTraceRandom random,
    inout uint rayCount, out float3 diffuseRadiance)
{
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    diffuseRadiance = float3(0.0f, 0.0f, 0.0f);
    float3 sampleDiffuse;
    float2 sunRandom = PathTraceNextFloat2(random);
    float4 environmentRandom = PathTraceNextFloat4(random);
    float selectionRandom = PathTraceNextFloat(random);

    PathTraceSun sun = PathTraceFrameSun();

    if(PathTraceSunHasEnergy(sun))
    {
        // S5: through the clouds' shadow at this vertex.
        PathTraceLightSample sunSample = PathTraceSampleSun(sun, sunRandom);
        sunSample.radiance *= SkyFrameCloudShadow(skyFrameInfoIndex, position);
        radiance += PathTraceShadeLightSampleLobes(surface, position, sunSample, rayCount, sampleDiffuse);
        diffuseRadiance += sampleDiffuse;
    }

    uint lightCount = sceneLightCount;
    float totalWeight = 0.0f;
    float selectedWeight = 0.0f;
    PathTraceLightSample selected = (PathTraceLightSample)0;

#ifdef PATH_TRACE_SAMPLED_NEE
    // M4.2C: the M4.2B sampled estimator (directionals enumerated, RIS over the disjoint finite-light pools, one shadow
    // ray) replaces the one-light reservoir over every table light when the training mode bit selects it. Both are
    // unbiased estimates of the vertex's table-light NEE; the clamp policy of the vertex loop applies to either.
    if(PathTraceSampledTableLighting(surface, position, random, rayCount, radiance, diffuseRadiance))
    {
        lightCount = 0u;
    }
#endif

    for(uint lightIndex = 0u; lightIndex < lightCount; ++lightIndex)
    {
        PathTraceLightSample lightSample = PathTraceSampleTableLight(SceneLightLoad(sceneLightBufferIndex, lightIndex), position);
        float weight = lightSample.valid ? PathTraceLuminance(lightSample.radiance) : 0.0f;

        if(!(weight > 0.0f) || !isfinite(weight))
        {
            continue;
        }

        totalWeight += weight;

        // Keeps candidate i with probability w_i / (running total); the survivor has probability w_i / sum(w).
        if(selectionRandom * totalWeight < weight)
        {
            selected = lightSample;
            selectedWeight = weight;
            selectionRandom = selectionRandom * totalWeight / weight;
        }
        else
        {
            selectionRandom = (selectionRandom * totalWeight - weight) / (totalWeight - weight);
        }
    }

    if(selectedWeight > 0.0f && isfinite(totalWeight))
    {
        float inverseProbability = totalWeight / selectedWeight;
        radiance += PathTraceShadeLightSampleLobes(surface, position, selected, rayCount, sampleDiffuse) *
            inverseProbability;
        diffuseRadiance += sampleDiffuse * inverseProbability;
    }

    PathTraceEnvironment environment = PathTraceFrameEnvironment();

    if(PathTraceEnvironmentSamplingEnabled(environment))
    {
        PathTraceLightSample environmentSample = PathTraceSampleEnvironment(environment, environmentSampler,
            environmentRandom);
        radiance += PathTraceShadeLightSampleLobes(surface, position, environmentSample, rayCount, sampleDiffuse);
        diffuseRadiance += sampleDiffuse;
    }

    return radiance;
}
#endif

float3 PathTraceDirectLighting(PathTraceSurface surface, float3 position, inout PathTraceRandom random,
    inout uint rayCount)
{
    float3 diffuseRadiance;
    return PathTraceDirectLightingLobes(surface, position, random, rayCount, diffuseRadiance);
}

void PathTraceCameraRay(uint2 pixel, float2 jitter, out float3 origin, out float3 direction, out float maximumDistance)
{
    // D3D depth: near plane at 0, far plane at 1. The ray spans exactly the raster frustum.
    float2 uv = (float2(pixel) + jitter) / float2(resolution);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 nearPoint = mul(inverseViewProjection, float4(ndc, 0.0f, 1.0f));
    float4 farPoint = mul(inverseViewProjection, float4(ndc, 1.0f, 1.0f));
    origin = nearPoint.xyz / nearPoint.w;
    float3 segment = farPoint.xyz / farPoint.w - origin;
    maximumDistance = length(segment);
    direction = segment / max(maximumDistance, 1.0e-20f);
}

#ifdef PATH_TRACE_SER
// SER S3A/S5A (the SER wrappers only, lib_6_9): a radiance ray traced as a HitObject, then executed by
// Invoke, which runs the same closest-hit or miss shader with the same payload as TraceRay would. Only reorder
// moves the thread: per-pixel state (RNG, throughput, radiance, ray counts) is thread-local and the path tracer
// writes its UAVs at the pixel's own address after the loop, so reordering changes no result (the wave sums of
// the statistics still count every thread once, in whichever wave it ends). The hint is the hit instance's
// InstanceID (its RayTracingInstanceData index), truncated to the hint bits; misses use the top hint value. A
// collision only costs coherence. Visibility rays never come here.
static const uint PATH_TRACE_SER_MISS_HINT = 0xFFFFu;
static const uint PATH_TRACE_SER_HINT_BITS = 16u;

void PathTraceTraceRadianceReordered(RayDesc ray, uint instanceMask, bool reorder, inout PathTracePayload payload)
{
    PathTraceCountReferenceRay(reorder ? 1u : 0u);
    dx::HitObject hit = dx::HitObject::TraceRay(scene, RAY_FLAG_NONE, instanceMask, 0, 1, PATH_TRACE_MISS_RADIANCE,
        ray, payload);

    if(reorder)
    {
        uint hint = hit.IsHit() ? (hit.GetInstanceID() & PATH_TRACE_SER_MISS_HINT) : PATH_TRACE_SER_MISS_HINT;
        dx::MaybeReorderThread(hit, hint, PATH_TRACE_SER_HINT_BITS);
    }

    dx::HitObject::Invoke(hit, payload);
}
#endif

#ifdef PATH_TRACE_DIRECT_COMPUTE
// The ReSTIR DI direct pass as a compute shader (DirectLighting/): a radiance ray traced with inline RayQuery, defined
// after the closest-hit helpers it shares with PathTraceClosestHit.
void PathTraceTraceRadianceInline(RayDesc ray, uint instanceMask, inout PathTracePayload payload);
#endif

// How the path loop forms its first vertex. Every ray-generation entry passes one of these as a literal,
// so the compiler specialises the loop per entry and folds the other cases away: the reference entry's loop
// is exactly the RTXPT.1 loop (RTXPT.2 R2 and R3 verify its accumulation bit for bit).
static const uint PATH_TRACE_PRIMARY_CAMERA = 0u;
static const uint PATH_TRACE_PRIMARY_PIXEL_CENTER = 1u;
static const uint PATH_TRACE_PRIMARY_RASTER = 2u;
// RTXPT.3 ReSTIR DI indirect pass: the raster primary whose vertex-0 direct light is the RTXDI estimate the ReSTIR
// DI direct pass stored for this pixel and sample. (3 was the retired fused ReSTIR DI primary.)
static const uint PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT = 4u;

// -----------------------------------------------------------------------------
// RTXPT.3 ReSTIR DI: per-sample vertex-0 direct radiance, three floats per pixel and sample, sample-major. The
// direct pass writes every active pixel's slots before the indirect pass reads them.
// -----------------------------------------------------------------------------

static uint2 g_splitPixel;
static uint g_splitSampleOffset;

uint PathTraceSplitDirectRadianceAddress(uint2 pixel, uint sampleOffset)
{
    uint element = (sampleOffset * resolution.y + pixel.y) * resolution.x + pixel.x;
    return element * 12u;
}

// The indirect pass's vertex-0 direct light. The six randoms PathTraceDirectLighting would draw are drawn and
// discarded, so every later random number of the path matches the raster-primary mode's.
float3 PathTraceSplitDirectLighting(inout PathTraceRandom random)
{
    PathTraceNextFloat2(random);
    PathTraceNextFloat4(random);

    RWByteAddressBuffer directRadiance = ResourceDescriptorHeap[splitDirectRadianceIndex];
    return asfloat(directRadiance.Load3(PathTraceSplitDirectRadianceAddress(g_splitPixel, g_splitSampleOffset)));
}

// RTXPT D.1B.2: the diffuse part of the same sample's vertex-0 direct radiance, written by the direct pass's
// extraction ray generation.
float3 PathTraceSplitDirectDiffuse()
{
    RWByteAddressBuffer directDiffuse = ResourceDescriptorHeap[splitDirectDiffuseIndex];
    return asfloat(directDiffuse.Load3(PathTraceSplitDirectRadianceAddress(g_splitPixel, g_splitSampleOffset)));
}

#ifdef PATH_TRACE_RESTIR_DI_LIBRARY
// RTXPT.3 ReSTIR DI direct pass: per-path-sample RTXDI inputs, set by its ray generation before each sample.
static uint2 g_rtxdiPixel;
static uint g_rtxdiSampleSeed;
// Sample 0 stores its final reservoir in the SDK's output array for this frame (shadingInputBufferIndex, which
// fused mode makes equal to initialSamplingOutputBufferIndex); the next frame reads it as history. It is also the
// only sample that reuses that history (split history, PathTraceRtxdiDirectLighting).
static bool g_rtxdiStoreReservoir;
static bool g_rtxdiReservoirStored;
// Visibility rays traced by RTXDI's bias correction, added to the pixel's ray count after reuse.
static uint g_rtxdiReuseRays;
// PT.2A: the initial-visibility ray of this path sample (RtxdiBridgeInitialVisible). It leaves the current surface at the
// position and geometric normal shading uses, so a final ray with the same direction and distance bits is the same ray.
static bool g_rtxdiInitialRayKnown;
static float3 g_rtxdiInitialRayDirection;
static float g_rtxdiInitialRayDistance;
static bool g_rtxdiInitialRayVisible;
// PT.2A diagnostics: initial-visibility rays of this path sample.
static uint g_rtxdiInitialRays;

// PT.2A diagnostics: word offsets (x4 bytes) of the counters the ReSTIR DI direct pass adds to when
// rtxdiDiagnosticCountersIndex is bound (Renderer::m_rtxdiDiagnosticCountersIndexForTesting).
static const uint RTXDI_DIAGNOSTIC_SAMPLES = 0u;
static const uint RTXDI_DIAGNOSTIC_REUSED = 1u;
static const uint RTXDI_DIAGNOSTIC_INITIAL_RAYS = 2u;
static const uint RTXDI_DIAGNOSTIC_REUSE_RAYS = 3u;
static const uint RTXDI_DIAGNOSTIC_FINAL_RAYS = 4u;
static const uint RTXDI_DIAGNOSTIC_FINAL_RAYS_REUSED = 5u;
static const uint RTXDI_DIAGNOSTIC_FINAL_VISIBLE = 6u;
static const uint RTXDI_DIAGNOSTIC_FINAL_REUSE_MISMATCHES = 7u;
// PT.2B, separate-MIS strategy: the RTXDI random-sampler pass of the HDRI light sample (unused by the SDK's passes).
static const uint PATH_TRACE_RTXDI_ENVIRONMENT_RANDOM_SEED = 41u;

// PT.2B: BSDF-candidate rays (Reservoir environment strategy) and HDRI light-sample shadow rays (separate-MIS strategy).
static const uint RTXDI_DIAGNOSTIC_BRDF_RAYS = 8u;
static const uint RTXDI_DIAGNOSTIC_ENVIRONMENT_RAYS = 9u;

void RtxdiCountDiagnostic(uint counter, uint value)
{
    if(rtxdiDiagnosticCountersIndex == 0xFFFFFFFFu)
    {
        return;
    }

    // One atomic per wave.
    uint total = WaveActiveSum(value);

    if(WaveIsFirstLane() && total != 0u)
    {
        RWByteAddressBuffer counters = ResourceDescriptorHeap[rtxdiDiagnosticCountersIndex];
        counters.InterlockedAdd(counter * 4u, total);
    }
}

// -----------------------------------------------------------------------------
// RTXPT.3 R3.5/R3.6 reuse inputs: previous surfaces from the R4 G-buffer history, visibility through the
// current TLAS (equal to the previous one whenever history is valid).
// -----------------------------------------------------------------------------

RAB_Surface RtxdiBridgeLoadSurface(int2 pixel, bool previousFrame)
{
    RAB_Surface bridgeSurface = RAB_EmptySurface();

    if(any(pixel < 0) || any(pixel >= int2(resolution)))
    {
        return bridgeSurface;
    }

    Texture2D<float> depthTexture = ResourceDescriptorHeap[previousFrame ? rtxdiPreviousDepthIndex : gBufferDepthIndex];
    Texture2D<float4> materialTexture = ResourceDescriptorHeap[previousFrame ? rtxdiPreviousMaterialIndex :
        gBufferMaterialIndex];
    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[previousFrame ? rtxdiPreviousSurfaceIndex :
        gBufferSurfaceIndex];
    Texture2D<float4> geometricNormalTexture = ResourceDescriptorHeap[previousFrame ?
        rtxdiPreviousGeometricNormalIndex : gBufferGeometricNormalIndex];
    PathTraceGBufferSample gBuffer = PathTraceLoadGBuffer(uint2(pixel), depthTexture, materialTexture, surfaceTexture,
        geometricNormalTexture);

    // Background pixels have no surface.
    if(gBuffer.depth >= 1.0f)
    {
        return bridgeSurface;
    }

    PathTracePrimaryCamera camera;
    camera.inverseCameraRelativeViewProjection = previousFrame ? rtxdiPreviousInverseCameraRelativeViewProjection :
        inverseCameraRelativeViewProjection;
    camera.cameraPosition = previousFrame ? rtxdiPreviousCameraPosition : cameraPosition;
    camera.resolution = resolution;

    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[materialBufferIndex];
    PathTracePrimaryVertex vertex;

    if(!PathTraceReconstructPrimaryVertex(uint2(pixel), gBuffer, camera, materials, materialTableIndex,
        materialAverageTableIndex, materialTableCosineMapping, vertex))
    {
        return bridgeSurface;
    }

    float3 forward = previousFrame ? rtxdiPreviousCameraForward : rtxdiCameraForward;
    bridgeSurface.position = vertex.position;
    bridgeSurface.surface = vertex.surface;
    bridgeSurface.valid = true;
    bridgeSurface.linearDepth = dot(forward, vertex.position - camera.cameraPosition);
    return bridgeSurface;
}

bool RtxdiBridgeVisible(RAB_Surface bridgeSurface, RAB_LightSample lightSample)
{
    return PathTraceVisible(bridgeSurface.position, bridgeSurface.surface.geometricNormal,
        lightSample.sample.direction, lightSample.sample.distance, g_rtxdiReuseRays);
}

// WNP.2 initial visibility: the same shadow ray shading uses, from the current surface, counted with the reuse rays.
// PT.2A: the ray and its result are remembered for final shading.
bool RtxdiBridgeInitialVisible(RAB_Surface bridgeSurface, RAB_LightSample lightSample)
{
    if(!lightSample.sample.valid)
    {
        return false;
    }

    ++g_rtxdiInitialRays;
    g_rtxdiInitialRayVisible = RtxdiBridgeVisible(bridgeSurface, lightSample);
    g_rtxdiInitialRayDirection = lightSample.sample.direction;
    g_rtxdiInitialRayDistance = lightSample.sample.distance;
    g_rtxdiInitialRayKnown = true;
    return g_rtxdiInitialRayVisible;
}

#if RTXDI_ENVIRONMENT_STRATEGY == RTXDI_ENVIRONMENT_STRATEGY_RESERVOIR
// PT.2B: BSDF-candidate rays of this path sample (RAB_TraceRayForLocalLight), also counted with the reuse rays.
static uint g_rtxdiBrdfRays;

// A BSDF candidate's ray: the shadow ray shading would trace toward a distant light in that direction.
bool RtxdiBridgeEscapes(RAB_Surface bridgeSurface, float3 direction)
{
    ++g_rtxdiBrdfRays;
    return PathTraceVisible(bridgeSurface.position, bridgeSurface.surface.geometricNormal, direction,
        PATH_TRACE_DISTANT_LIGHT_DISTANCE, g_rtxdiReuseRays);
}
#endif

int2 RtxdiBridgeViewSize()
{
    return int2(resolution);
}

Buffer<float2> RtxdiNeighborOffsets()
{
    return ResourceDescriptorHeap[rtxdiNeighborOffsetsIndex];
}

#define RTXDI_NEIGHBOR_OFFSETS_BUFFER RtxdiNeighborOffsets()
#include "RtxdiReuseBridge.hlsli"

// RTXPT.3 Gate 1: vertex-0 direct light from one RTXDI reservoir, with MIS weight 1: a fresh initial reservoir,
// then (R3.5/R3.6) fused spatiotemporal reuse of the previous frame's reservoirs when history is valid. The
// estimate is F(y) * V(y) * W (RtxdiApplicationBridge.hlsli); the indirect pass's non-delta continuation misses
// then add no sun radiance, and no HDRI radiance unless the HDRI is estimated separately (PT.2B, below). The six randoms
// PathTraceDirectLighting would draw are drawn and discarded, matching PathTraceSplitDirectLighting; the caller's
// stream is a placeholder (zero state), so nothing here may use those values.
// RTXPT D.1B.2: diffuseContribution is the diffuse-lobe part of the returned estimate, F_diffuse(y) V(y) W with the
// same selected sample and reservoir weight (signal extraction only).
float3 PathTraceRtxdiDirectLighting(PathTraceSurface surface, float3 position, inout PathTraceRandom random,
    inout uint rayCount, out float3 diffuseContribution)
{
    diffuseContribution = float3(0.0f, 0.0f, 0.0f);
    PathTraceNextFloat2(random);
    PathTraceNextFloat4(random);

    RAB_Surface bridgeSurface;
    bridgeSurface.position = position;
    bridgeSurface.surface = surface;
    bridgeSurface.valid = true;
    bridgeSurface.linearDepth = dot(rtxdiCameraForward, position - cameraPosition);

    // WNP.2: with initial visibility, an occluded initial sample is discarded (W = 0, M kept) before reuse. Its shaded
    // value would have been zero anyway (F V W with V = 0), so without history the estimate is unchanged; with history
    // it can no longer outweigh a visible reused sample. This matches ray-traced bias correction, whose stored reservoirs
    // are already visibility-filtered (RtxdiReuseBridge.hlsli). The ray is counted with the reuse rays.
    RAB_LightSample selectedSample;
    g_rtxdiReuseRays = 0u;
    g_rtxdiInitialRays = 0u;
    g_rtxdiInitialRayKnown = false;
#if RTXDI_ENVIRONMENT_STRATEGY == RTXDI_ENVIRONMENT_STRATEGY_RESERVOIR
    g_rtxdiBrdfRays = 0u;
#endif
    RTXDI_DIReservoir reservoir = RtxdiBridgeSampleInitialReservoir(bridgeSurface, g_rtxdiPixel, g_rtxdiSampleSeed,
        RtxdiBridgeInitialSamplingParameters(rtxdiLocalLightSamples, rtxdiInfiniteLightSamples, rtxdiInitialVisibility,
            rtxdiEnvironmentSamples, rtxdiBrdfSamples), rtxdiLightBuffer, selectedSample);
    rayCount += g_rtxdiReuseRays;

    // Split history: only sample 0, the sample that publishes the pixel's reservoir, reuses the previous frame. Every
    // later sample of the frame keeps its fresh initial reservoir, so it is independent of that shared history
    // (reusing it from every sample made a frame's samples strongly correlated). Invalid history: no reuse at all;
    // the initial reservoir keeps its own normalization.
    bool reuseHistory = rtxdiHistoryValid != 0u && g_rtxdiStoreReservoir;

    if(reuseHistory)
    {
        // G-buffer motion is current UV - previous UV and the view-depth change; RTXDI wants the previous pixel
        // offset and the expected depth change.
        Texture2D<float4> motionTexture = ResourceDescriptorHeap[rtxdiMotionIndex];
        float4 motion = motionTexture.Load(int3(g_rtxdiPixel, 0));
        float3 screenSpaceMotion = float3(-motion.xy * float2(resolution), motion.z);

        RTXDI_RandomSamplerState reuseRng = RTXDI_InitRandomSampler(g_rtxdiPixel, g_rtxdiSampleSeed,
            RTXDI_DI_TEMPORAL_RESAMPLING_RANDOM_SEED);
        int2 temporalSamplePixel;
        g_rtxdiReuseRays = 0u;
        reservoir = RTXDI_DISpatioTemporalResampling(g_rtxdiPixel, bridgeSurface, reservoir, reuseRng,
            screenSpaceMotion, rtxdiBufferIndices.temporalResamplingInputBufferIndex, rtxdiRuntime,
            rtxdiReservoirBuffer, rtxdiSpatioTemporal, temporalSamplePixel, selectedSample);
        rayCount += g_rtxdiReuseRays;
        RtxdiCountDiagnostic(RTXDI_DIAGNOSTIC_REUSE_RAYS, g_rtxdiReuseRays);
    }

    float3 contribution = float3(0.0f, 0.0f, 0.0f);
    bool visible = false;
    uint finalRays = 0u;
    uint finalRaysReused = 0u;
    uint finalReuseMismatches = 0u;

    if(RTXDI_IsValidDIReservoir(reservoir))
    {
        float weight = RTXDI_GetDIReservoirInvPdf(reservoir);
        float3 unshadowedDiffuse;
        float3 unshadowed = RtxdiBridgeSampleContributionLobes(selectedSample, bridgeSurface, unshadowedDiffuse);

        if(weight > 0.0f && isfinite(weight) && any(unshadowed > 0.0f))
        {
            // PT.2A: the initial-visibility ray of this dispatch left the same surface (position and geometric normal) through
            // the same TLAS. When the final sample's direction and distance are bit-identical, the final ray is that ray, so
            // its result is reused; anything else (a reused or reconnected sample, a different light point) is traced.
            bool sameRay = rtxdiReuseInitialVisibility != 0u && g_rtxdiInitialRayKnown &&
                all(asuint(selectedSample.sample.direction) == asuint(g_rtxdiInitialRayDirection)) &&
                asuint(selectedSample.sample.distance) == asuint(g_rtxdiInitialRayDistance);

            if(sameRay)
            {
                visible = g_rtxdiInitialRayVisible;
                finalRaysReused = 1u;

                if(rtxdiReuseInitialVisibility == 2u)
                {
                    uint verificationRays = 0u;
                    bool traced = PathTraceVisible(position, surface.geometricNormal, selectedSample.sample.direction,
                        selectedSample.sample.distance, verificationRays);
                    finalReuseMismatches = traced != visible ? 1u : 0u;
                    visible = traced;
                }
            }
            else
            {
                visible = PathTraceVisible(position, surface.geometricNormal, selectedSample.sample.direction,
                    selectedSample.sample.distance, rayCount);
                finalRays = 1u;
            }

            contribution = visible ? unshadowed * weight : float3(0.0f, 0.0f, 0.0f);
            diffuseContribution = visible ? unshadowedDiffuse * weight : float3(0.0f, 0.0f, 0.0f);
        }
    }

#if RTXDI_ENVIRONMENT_STRATEGY == RTXDI_ENVIRONMENT_STRATEGY_SEPARATE_MIS
    // PT.2B Approach B: the HDRI is no RTXDI light. One HDRI light sample, power-heuristic weighted against the BSDF
    // continuation, which the indirect pass adds when it escapes (PathTraceMissEnvironmentRadiance): the reference path
    // tracer's estimator of the HDRI at this vertex. Its randoms are an independent per-pixel, per-sample RTXDI sequence.
    PathTraceEnvironment environment = PathTraceFrameEnvironment();

    if(PathTraceEnvironmentSamplingEnabled(environment))
    {
        RTXDI_RandomSamplerState environmentRng = RTXDI_InitRandomSampler(g_rtxdiPixel, g_rtxdiSampleSeed,
            PATH_TRACE_RTXDI_ENVIRONMENT_RANDOM_SEED);
        float4 environmentRandom;
        environmentRandom.x = RTXDI_GetNextRandom(environmentRng);
        environmentRandom.y = RTXDI_GetNextRandom(environmentRng);
        environmentRandom.z = RTXDI_GetNextRandom(environmentRng);
        environmentRandom.w = RTXDI_GetNextRandom(environmentRng);
        uint raysBefore = rayCount;
        float3 environmentDiffuse;
        contribution += PathTraceShadeLightSampleLobes(surface, position,
            PathTraceSampleEnvironment(environment, environmentSampler, environmentRandom), rayCount, environmentDiffuse);
        diffuseContribution += environmentDiffuse;
        RtxdiCountDiagnostic(RTXDI_DIAGNOSTIC_ENVIRONMENT_RAYS, rayCount - raysBefore);
    }
#endif

    RtxdiCountDiagnostic(RTXDI_DIAGNOSTIC_SAMPLES, 1u);
    RtxdiCountDiagnostic(RTXDI_DIAGNOSTIC_REUSED, reuseHistory ? 1u : 0u);
    RtxdiCountDiagnostic(RTXDI_DIAGNOSTIC_INITIAL_RAYS, g_rtxdiInitialRays);
    RtxdiCountDiagnostic(RTXDI_DIAGNOSTIC_FINAL_RAYS, finalRays);
    RtxdiCountDiagnostic(RTXDI_DIAGNOSTIC_FINAL_RAYS_REUSED, finalRaysReused);
    RtxdiCountDiagnostic(RTXDI_DIAGNOSTIC_FINAL_VISIBLE, visible ? 1u : 0u);
    RtxdiCountDiagnostic(RTXDI_DIAGNOSTIC_FINAL_REUSE_MISMATCHES, finalReuseMismatches);
#if RTXDI_ENVIRONMENT_STRATEGY == RTXDI_ENVIRONMENT_STRATEGY_RESERVOIR
    RtxdiCountDiagnostic(RTXDI_DIAGNOSTIC_BRDF_RAYS, g_rtxdiBrdfRays);
#endif

    // Ray-traced bias correction treats stored reservoirs as visibility-filtered: an occluded sample is removed
    // (its M kept) before the reservoir becomes history.
    if(g_rtxdiStoreReservoir)
    {
        if(RTXDI_IsValidDIReservoir(reservoir))
        {
            RTXDI_StoreVisibilityInDIReservoir(reservoir, visible ? float3(1.0f, 1.0f, 1.0f) : float3(0.0f, 0.0f, 0.0f),
                true);
        }

        RTXDI_StoreDIReservoir(reservoir, rtxdiReservoirBuffer, g_rtxdiPixel, rtxdiBufferIndices.shadingInputBufferIndex);
        g_rtxdiStoreReservoir = false;
        g_rtxdiReservoirStored = true;
    }

    return contribution;
}
#endif

// RTXPT.2 raster primary: this pixel's first vertex, decoded once per pixel before the sample loop (it is the
// same for every sample; only the random numbers differ). state: 1 surface, 0 background, -1 unusable sample.
#ifdef PATH_TRACE_PSR_LIBRARY
// PSR.1a: the closest-hit payload of the vertex-0 mirror's specular continuation, as PathTracePayload carries it.
struct PathTracePsrHit
{
    float hitDistance;
    float3 geometricNormal;
    float3 shadingNormal;
    float2 uv;
    uint materialIndex;
};
#endif

struct PathTraceRasterFirstVertex
{
    int state;
    float3 position;
    PathTraceSurface surface;
    float3 backgroundDirection;
#ifdef PATH_TRACE_PSR_LIBRARY
    // PSR.1a: PATH_TRACE_PSR_* of this pixel's record. PSR.1a-P: the replaced hit itself is read at vertex 1
    // (PathTracePsrLoadHit), so only this word stays live through the sample loop.
    uint psrState;
    // Selected image pixel, independent of the sparse training dispatch index.
    uint2 psrPixel;
#ifdef PATH_TRACE_PSR_GBUFFER
    // PSR.2: a replaced pixel's mirror plane point (its exact camera-ray intersection, camera-relative).
    float3 psrPlanePoint;
#if PATH_TRACE_PSR_CHAIN >= 2
    // Multibounce PSR: a replaced pixel's chain length, motion.w (1, or 2 for a two-mirror chain).
    uint psrChain;
#endif
#endif
#endif
};

// RTXPT.2 raster primary: the first vertex from this pixel's G-buffer sample, without a camera ray.
// Returns 1 for a surface, 0 for a background pixel (backgroundDirection is then the pixel-centre camera
// direction, as a camera ray that misses would see), and -1 for a sample that cannot form a surface (it
// contributes nothing, as a failed PathTraceBuildSurface does for an RT hit).
int PathTraceLoadRasterPrimaryVertex(uint2 pixel, out float3 position, out PathTraceSurface surface,
    out float3 backgroundDirection)
{
    position = float3(0.0f, 0.0f, 0.0f);
    surface = (PathTraceSurface)0;
    backgroundDirection = float3(0.0f, 0.0f, 1.0f);

    Texture2D<float> depthTexture = ResourceDescriptorHeap[gBufferDepthIndex];
    Texture2D<float4> materialTexture = ResourceDescriptorHeap[gBufferMaterialIndex];
    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[gBufferSurfaceIndex];
    Texture2D<float4> geometricNormalTexture = ResourceDescriptorHeap[gBufferGeometricNormalIndex];
    PathTraceGBufferSample gBuffer = PathTraceLoadGBuffer(pixel, depthTexture, materialTexture, surfaceTexture,
        geometricNormalTexture);

    if(gBuffer.depth >= 1.0f)
    {
        float3 origin;
        float maximumDistance;
        PathTraceCameraRay(pixel, float2(0.5f, 0.5f), origin, backgroundDirection, maximumDistance);
        return 0;
    }

    PathTracePrimaryCamera camera;
    camera.inverseCameraRelativeViewProjection = inverseCameraRelativeViewProjection;
    camera.cameraPosition = cameraPosition;
    camera.resolution = resolution;

    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[materialBufferIndex];
    PathTracePrimaryVertex vertex;

    if(!PathTraceReconstructPrimaryVertex(pixel, gBuffer, camera, materials, materialTableIndex,
        materialAverageTableIndex, materialTableCosineMapping, vertex))
    {
        return -1;
    }

    position = vertex.position;
    surface = vertex.surface;
    return 1;
}

PathTraceRasterFirstVertex PathTraceDecodeRasterFirstVertex(uint2 pixel)
{
    PathTraceRasterFirstVertex firstVertex;
    firstVertex.state = PathTraceLoadRasterPrimaryVertex(pixel, firstVertex.position, firstVertex.surface,
        firstVertex.backgroundDirection);
#ifdef PATH_TRACE_PSR_LIBRARY
    firstVertex.psrState = 0u;
    firstVertex.psrPixel = pixel;
#ifdef PATH_TRACE_PSR_GBUFFER
    firstVertex.psrPlanePoint = float3(0.0f, 0.0f, 0.0f);
#if PATH_TRACE_PSR_CHAIN >= 2
    firstVertex.psrChain = 1u;
#endif
#endif
#endif
    return firstVertex;
}

#ifdef PATH_TRACE_PSR_LIBRARY
// -----------------------------------------------------------------------------
// PSR.1a primary surface replacement (Raster Primary, denoiser Off). PathTracePsrRayGeneration traces the specular
// continuation of every pure-delta vertex-0 mirror once per pixel and records its closest-hit payload. This library's
// raster primary then takes vertex 1 from that record instead of tracing it. Vertex 0 itself is unchanged: its delta
// event (Fresnel weight, randoms, roulette, depth) is still evaluated by the path loop, which applies the mirror
// throughput exactly once. Only the traversal moves, so the transport is the production loop's.
// -----------------------------------------------------------------------------

// Not a pure-delta mirror (or no path continues past vertex 0): the production loop, untouched.
static const uint PATH_TRACE_PSR_NONE = 0u;
// Replaced: the continuation hit a surface with a non-delta lobe.
static const uint PATH_TRACE_PSR_SURFACE = 1u;
// Replaced: the continuation missed all geometry (the mirror shows the environment).
static const uint PATH_TRACE_PSR_SKY = 2u;
// Fallbacks to the production loop, which traces the continuation itself: the replacement is another delta surface
// (mirror chains are out of scope), cannot form a surface, or the delta sample is invalid (reflection below the
// geometric surface).
static const uint PATH_TRACE_PSR_FALLBACK_MIRROR = 3u;
static const uint PATH_TRACE_PSR_FALLBACK_SURFACE = 4u;
static const uint PATH_TRACE_PSR_FALLBACK_DIRECTION = 5u;

// PSR.1a-P: 10 words per pixel. Word 0 is the state (3 bits) and the material index (29 bits; Raster Primary refuses
// tables above PathTraceRasterPrimaryMaximumMaterials); then hitDistance, geometricNormal, shadingNormal, uv, all at full
// precision, because vertex 1's PathTraceBuildSurface must see exactly the payload. Ineligible pixels write word 0 only;
// the other words are read only for a replaced state.
static const uint PATH_TRACE_PSR_RECORD_STRIDE = 40u;
static const uint PATH_TRACE_PSR_STATE_BITS = 3u;
static const uint PATH_TRACE_PSR_STATE_MASK = (1u << PATH_TRACE_PSR_STATE_BITS) - 1u;
static const uint PATH_TRACE_PSR_MAXIMUM_MATERIAL_INDEX = (1u << (32u - PATH_TRACE_PSR_STATE_BITS)) - 1u;

bool PathTracePsrReplaced(uint state)
{
    return state == PATH_TRACE_PSR_SURFACE || state == PATH_TRACE_PSR_SKY;
}

// The production BSDF's own definition of a pure-delta mirror: the delta lobe is the only strategy (no diffuse or
// multiple-scatter energy). Mixed smooth dielectrics, which choose the delta lobe stochastically, are not eligible.
bool PathTracePsrEligible(PathTraceRasterFirstVertex firstVertex)
{
    return firstVertex.state > 0 && maxPathDepth > 0u && firstVertex.surface.deltaSpecular &&
        firstVertex.surface.specularProbability >= 1.0f;
}

uint PathTracePsrRecordAddress(uint2 pixel)
{
    return (pixel.y * resolution.x + pixel.x) * PATH_TRACE_PSR_RECORD_STRIDE;
}

// An ineligible pixel's record: word 0 only.
void PathTracePsrStoreState(uint2 pixel, uint state)
{
    RWByteAddressBuffer records = ResourceDescriptorHeap[psrRecordIndex];
    records.Store(PathTracePsrRecordAddress(pixel), state);
}

void PathTracePsrStoreRecord(uint2 pixel, uint state, PathTracePayload payload)
{
    RWByteAddressBuffer records = ResourceDescriptorHeap[psrRecordIndex];
    uint address = PathTracePsrRecordAddress(pixel);
    records.Store(address, state | (payload.materialIndex << PATH_TRACE_PSR_STATE_BITS));
    records.Store4(address + 4u, uint4(asuint(payload.hitDistance), asuint(payload.geometricNormal)));
    records.Store4(address + 20u, uint4(asuint(payload.shadingNormal), asuint(payload.uv.x)));
    records.Store(address + 36u, asuint(payload.uv.y));
}

void PathTracePsrLoadRecord(uint2 pixel, inout PathTraceRasterFirstVertex firstVertex)
{
    RWByteAddressBuffer records = ResourceDescriptorHeap[psrRecordIndex];
    firstVertex.psrState = records.Load(PathTracePsrRecordAddress(pixel)) & PATH_TRACE_PSR_STATE_MASK;
}

PathTracePsrHit PathTracePsrLoadHit(uint2 pixel);

// -----------------------------------------------------------------------------
// PSR.2a effective primary surface. The only interface through which path-tracing consumers see PSR; the record
// functions above are its storage and are called from nowhere else. The effective primary surface of a pixel is its
// raster vertex 0, or for a replaced pure-delta mirror the physical surface B its reflection reaches. Physical B is
// what shading uses; the virtual guides (PathTracePsrGuideRayGeneration) are a separate, reprojection-only view of it.
// The mirror stays the path's vertex 0: its delta event (Fresnel, randoms, roulette, depth) is still evaluated by the
// path loop, so B is the path's vertex 1.
// -----------------------------------------------------------------------------

#ifdef PATH_TRACE_PSR_GBUFFER
// -----------------------------------------------------------------------------
// PSR.2 Stage 3A storage (PathTraceReSTIRDIPsrGBuffer*, PathTraceReferencePsrGBuffer*): the G-buffer itself. A replaced
// pixel (identity.x's eligibility flag and surface.w = -(materialIndex + 1), PsrInPlaceReplace.hlsl) holds the virtual
// image Bv of its reflected surface B; the mirror is recovered from the identity (PsrMirrorRecovery.hlsli) with the
// replacement pass's camera and transforms, and B is Bv reflected back through the mirror plane the replacement pass
// used. An eligible pixel whose replacement fell back still holds its mirror and takes the production path.
// -----------------------------------------------------------------------------

// Defined with the closest-hit shader below.
float3 PathTraceLoadPosition(ByteAddressBuffer vertexBuffer, uint vertexIndex, uint vertexStride);
float3 PathTraceLoadNormal(ByteAddressBuffer vertexBuffer, uint vertexIndex, RayTracingInstanceData instance);
float2 PathTraceLoadUV(ByteAddressBuffer vertexBuffer, uint vertexIndex, RayTracingInstanceData instance);
float4 PathTraceLoadTangent(ByteAddressBuffer vertexBuffer, uint vertexIndex, RayTracingInstanceData instance);
uint3 PathTraceTriangleIndices(RayTracingInstanceData instance, uint primitiveIndex);

#include "PsrMirrorRecovery.hlsli"

// b4: the replacement pass's camera (PsrInPlaceReplacePass::Constants) and the identity target; t2: its object
// transforms; s2: Triangle.hlsl's material sampler (trilinear clamp), which the mirror's textures were sampled with.
cbuffer PathTracePsrGBufferConstants : register(b4)
{
    row_major float4x4 psrGBufferCameraRelativeViewProjection;
    row_major float4x4 psrGBufferInverseCameraRelativeViewProjection;
    float3 psrGBufferCameraPosition;
    uint psrGBufferIdentityIndex;
    uint psrGBufferTransformCount;
    // x: the verification probe's UAV (PATH_TRACE_PSR_PROBE_INDEX); y: the geometry pass's texture mip bias
    // (PsrRecoverMirror), as a float's bits.
    uint3 psrGBufferPadding;
};

StructuredBuffer<PsrObjectTransform> psrObjectTransforms : register(t2);
SamplerState psrMirrorSampler : register(s2);

PsrMirrorCamera PathTracePsrMirrorCamera()
{
    PsrMirrorCamera mirrorCamera;
    mirrorCamera.camera.inverseCameraRelativeViewProjection = psrGBufferInverseCameraRelativeViewProjection;
    mirrorCamera.camera.cameraPosition = psrGBufferCameraPosition;
    mirrorCamera.camera.resolution = resolution;
    mirrorCamera.cameraRelativeViewProjection = psrGBufferCameraRelativeViewProjection;
    return mirrorCamera;
}

// A replaced pixel becomes its recovered mirror with state SURFACE (the production decode rejects its negative material
// index); every other pixel is left as decoded. A replaced pixel whose mirror cannot be recovered stays unusable.
void PathTraceResolveEffectivePrimary(uint2 pixel, inout PathTraceRasterFirstVertex firstVertex)
{
    Texture2D<uint2> identityTexture = ResourceDescriptorHeap[psrGBufferIdentityIndex];
    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[gBufferSurfaceIndex];
    const uint2 identity = identityTexture.Load(int3(pixel, 0));

    if(!PsrIdentityEligible(identity.x) || !(surfaceTexture.Load(int3(pixel, 0)).w < 0.0f))
    {
        return;
    }

    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[materialBufferIndex];
    PsrRecoveredMirror mirror;

    if(!PsrRecoverMirror(pixel, identity, PathTracePsrMirrorCamera(), psrObjectTransforms, psrGBufferTransformCount,
        instanceDataIndex, materials, materialTableIndex, materialAverageTableIndex, materialTableCosineMapping,
        psrMirrorSampler, asfloat(psrGBufferPadding.y), true, mirror))
    {
        return;
    }

    firstVertex.state = 1;
    firstVertex.position = mirror.vertex.position;
    firstVertex.surface = mirror.vertex.surface;
    firstVertex.psrState = PATH_TRACE_PSR_SURFACE;
    firstVertex.psrPlanePoint = mirror.mirrorPoint;
#if PATH_TRACE_PSR_CHAIN >= 2
    // psrGBufferPadding.z: the motion target's SRV, whose w the replacement pass sets to the chain length.
    Texture2D<float4> motionTexture = ResourceDescriptorHeap[psrGBufferPadding.z];
    firstVertex.psrChain = motionTexture.Load(int3(pixel, 0)).w == 2.0f ? 2u : 1u;
#endif
}
#else
// Sets firstVertex.psrState. Only eligible vertices read storage: both record producers decode the same G-buffer
// vertex and write PATH_TRACE_PSR_NONE for every vertex PathTracePsrEligible rejects, so an ineligible vertex's
// state is NONE without reading it.
void PathTraceResolveEffectivePrimary(uint2 pixel, inout PathTraceRasterFirstVertex firstVertex)
{
    if(PathTracePsrEligible(firstVertex))
    {
        PathTracePsrLoadRecord(pixel, firstVertex);
    }
}
#endif

// The path vertex at which the effective primary surface is shaded: 1 for a replaced surface the path reaches (vertex 1
// is shaded only below maxPathDepth), otherwise 0.
uint PathTraceEffectivePrimaryVertex(PathTraceRasterFirstVertex firstVertex)
{
#if PATH_TRACE_PSR_CHAIN >= 2
    // Multibounce PSR: the terminal surface D of a two-mirror chain is vertex 2.
    return firstVertex.psrState == PATH_TRACE_PSR_SURFACE && maxPathDepth > firstVertex.psrChain ? firstVertex.psrChain :
        0u;
#else
    return firstVertex.psrState == PATH_TRACE_PSR_SURFACE && maxPathDepth > 1u ? 1u : 0u;
#endif
}

#ifdef PATH_TRACE_PSR_GBUFFER
// PSR.2 Stage 3B: the G-buffer's virtual point Bv of a replaced pixel (world space), the surface the denoisers see.
float3 PathTracePsrVirtualPosition(uint2 pixel)
{
    Texture2D<float> depthTexture = ResourceDescriptorHeap[gBufferDepthIndex];
    return psrGBufferCameraPosition + PathTraceUnprojectCameraRelative(pixel, depthTexture.Load(int3(pixel, 0)),
        PathTracePsrMirrorCamera().camera);
}

// PSR.2 keeps no hit: vertex 1 is built by PathTracePsrReplacementSurface. A non-negative distance only marks the hit.
PathTracePayload PathTraceEffectivePrimaryReplacementPayload(uint2 pixel)
{
    PathTracePayload payload;
    payload.hitDistance = 0.0f;
    payload.geometricNormal = float3(0.0f, 0.0f, 1.0f);
    payload.shadingNormal = float3(0.0f, 0.0f, 1.0f);
    payload.uv = float2(0.0f, 0.0f);
    payload.materialIndex = 0u;
    return payload;
}

// PSR.2: the physical replacement surface B of a SURFACE pixel. The G-buffer's virtual surface Bv (decoded with its
// material index -(i + 1) restored, from the replacement pass's camera, which projected it) is reflected back through
// the plane the replacement pass reflected B through: the mirror's finalized shading normal at the exact intersection.
// Position, both normals and the view reflect; the view becomes the reflected ray's -d (camera, mirror point and Bv are
// collinear). The two-sided facing and the horizon correction use only dot products, so they commute with the
// reflection, and the lift uses Bv's camera distance, the path length camera-mirror-B.
bool PathTracePsrReplacementSurface(uint2 pixel, PathTraceRasterFirstVertex firstVertex, out float3 position,
    out PathTraceSurface surface)
{
    position = float3(0.0f, 0.0f, 0.0f);
    surface = (PathTraceSurface)0;

    Texture2D<float> depthTexture = ResourceDescriptorHeap[gBufferDepthIndex];
    Texture2D<float4> materialTexture = ResourceDescriptorHeap[gBufferMaterialIndex];
    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[gBufferSurfaceIndex];
    Texture2D<float4> geometricNormalTexture = ResourceDescriptorHeap[gBufferGeometricNormalIndex];
    PathTraceGBufferSample gBuffer = PathTraceLoadGBuffer(pixel, depthTexture, materialTexture, surfaceTexture,
        geometricNormalTexture);
    gBuffer.surface.w = -gBuffer.surface.w - 1.0f;

    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[materialBufferIndex];
    PathTracePrimaryVertex virtualVertex;

    if(!PathTraceDecodePrimaryVertex(pixel, gBuffer, PathTracePsrMirrorCamera().camera, materials, materialTableIndex,
        materialAverageTableIndex, materialTableCosineMapping, virtualVertex))
    {
        return false;
    }

    const float3 planeNormal = firstVertex.surface.normal;
    surface = virtualVertex.surface;
    surface.view = PsrReflectDirection(surface.view, planeNormal);
    surface.normal = PsrReflectDirection(surface.normal, planeNormal);
    surface.geometricNormal = PsrReflectDirection(surface.geometricNormal, planeNormal);
    const float lift = PATH_TRACE_PRIMARY_SURFACE_LIFT * length(virtualVertex.cameraRelativePosition);
    const float3 relative = PsrReflectPoint(virtualVertex.cameraRelativePosition, firstVertex.psrPlanePoint, planeNormal) +
        surface.geometricNormal * lift;
    position = psrGBufferCameraPosition + relative;
    return all(isfinite(position)) && PathTraceFinalizeSurface(surface);
}

#if PATH_TRACE_PSR_CHAIN >= 2
// -----------------------------------------------------------------------------
// Multibounce PSR (K = 2). The G-buffer holds Dv = R_A(R_B(D)) of a two-mirror chain A -> B -> D. Nothing stores B, so
// each lighting pass retraces A -> B with the replacement pass's convention (origin: A's exact intersection with A's
// geometric-normal offset; direction: A's delta sample) and reconstructs D from Dv through A's and B's planes. The
// verification (M2-A.1, the same tolerances: production's lift 2e-4 L and 0.5 degrees) accepts only when the retraced
// chain physically reaches the guides' surface:
//   direct pass    B is a pure-delta mirror and D lies on B's delta ray, in front of it;
//   indirect pass  additionally, the path loop's own B -> D ray hits a non-delta surface at D's distance with the
//                  G-buffer's material and virtual face normal.
// A rejection by either pass makes the indirect pass's path the ordinary production path for the pixel (its traced hits,
// its own next-event estimation, the direct pass's value unused), with the extraction still split at vertex 2.
// -----------------------------------------------------------------------------
static const float PATH_TRACE_PSR_CHAIN_TOLERANCE = 2.0e-4f;
static const float PATH_TRACE_PSR_CHAIN_NORMAL_COSINE = 0.99996192f;

// The direct pass's split-direct value of a sample whose chain it rejected (x < 0; valid values are non-negative).
static const float PATH_TRACE_PSR_CHAIN_REJECTED = -1.0f;

// D reconstructed from the G-buffer's Dv through A's plane, then the plane of B (its exact hit point and finalized shading
// normal, camera-relative): its lifted shading position, its exact point (both world space), the surface, the unfolded
// path length L = |Dv - camera| and the stored material index and virtual geometric normal.
bool PathTracePsrChainSurface(uint2 pixel, PathTraceRasterFirstVertex firstVertex, float3 chainPoint, float3 chainNormal,
    out float3 position, out float3 exactPoint, out PathTraceSurface surface, out float pathLength,
    out uint storedMaterial, out float3 storedNormal)
{
    position = float3(0.0f, 0.0f, 0.0f);
    exactPoint = float3(0.0f, 0.0f, 0.0f);
    surface = (PathTraceSurface)0;
    pathLength = 0.0f;

    Texture2D<float> depthTexture = ResourceDescriptorHeap[gBufferDepthIndex];
    Texture2D<float4> materialTexture = ResourceDescriptorHeap[gBufferMaterialIndex];
    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[gBufferSurfaceIndex];
    Texture2D<float4> geometricNormalTexture = ResourceDescriptorHeap[gBufferGeometricNormalIndex];
    PathTraceGBufferSample gBuffer = PathTraceLoadGBuffer(pixel, depthTexture, materialTexture, surfaceTexture,
        geometricNormalTexture);
    gBuffer.surface.w = -gBuffer.surface.w - 1.0f;
    storedMaterial = uint(gBuffer.surface.w);
    storedNormal = gBuffer.geometricNormal.xyz;

    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[materialBufferIndex];
    PathTracePrimaryVertex virtualVertex;

    if(!PathTraceDecodePrimaryVertex(pixel, gBuffer, PathTracePsrMirrorCamera().camera, materials, materialTableIndex,
        materialAverageTableIndex, materialTableCosineMapping, virtualVertex))
    {
        return false;
    }

    const float3 planeNormal = firstVertex.surface.normal;
    surface = virtualVertex.surface;
    surface.view = PsrReflectDirection(PsrReflectDirection(surface.view, planeNormal), chainNormal);
    surface.normal = PsrReflectDirection(PsrReflectDirection(surface.normal, planeNormal), chainNormal);
    surface.geometricNormal = PsrReflectDirection(PsrReflectDirection(surface.geometricNormal, planeNormal), chainNormal);
    pathLength = length(virtualVertex.cameraRelativePosition);
    const float3 relative = PsrReflectPoint(PsrReflectPoint(virtualVertex.cameraRelativePosition,
        firstVertex.psrPlanePoint, planeNormal), chainPoint, chainNormal);
    exactPoint = psrGBufferCameraPosition + relative;
    position = exactPoint + surface.geometricNormal * (PATH_TRACE_PRIMARY_SURFACE_LIFT * pathLength);
    return all(isfinite(position)) && pathLength > 0.0f && PathTraceFinalizeSurface(surface);
}

// The first chain segment's ray: A's exact intersection, offset along A's geometric normal, in A's delta direction.
bool PathTracePsrChainFirstRay(PathTraceRasterFirstVertex firstVertex, out RayDesc ray)
{
    float3 diffuseShare;
    bool specularStrategy;
    const PathTraceBSDFSample mirrorSample = PathTraceSampleBSDFLobes(firstVertex.surface, float3(0.0f, 0.0f, 0.0f),
        diffuseShare, specularStrategy);
    ray.Origin = PathTraceOffsetRayOrigin(psrGBufferCameraPosition + firstVertex.psrPlanePoint,
        firstVertex.surface.geometricNormal);
    ray.Direction = mirrorSample.direction;
    ray.TMin = 0.0f;
    ray.TMax = PATH_TRACE_DISTANT_LIGHT_DISTANCE;
    return mirrorSample.valid && mirrorSample.delta;
}

// B's delta ray towards D, and whether D (exact) lies on it, in front of its origin, within the tolerance. along is D's
// distance along the ray.
bool PathTracePsrChainOnRay(float3 chainHit, PathTraceSurface chainSurface, float3 exactPoint, float pathLength,
    out RayDesc terminalRay, out float along)
{
    float3 diffuseShare;
    bool specularStrategy;
    const PathTraceBSDFSample chainSample = PathTraceSampleBSDFLobes(chainSurface, float3(0.0f, 0.0f, 0.0f), diffuseShare,
        specularStrategy);
    terminalRay.Origin = PathTraceOffsetRayOrigin(chainHit, chainSurface.geometricNormal);
    terminalRay.Direction = chainSample.direction;
    terminalRay.TMin = 0.0f;
    terminalRay.TMax = PATH_TRACE_DISTANT_LIGHT_DISTANCE;
    const float3 toPoint = exactPoint - terminalRay.Origin;
    along = dot(toPoint, terminalRay.Direction);
    return chainSample.valid && chainSample.delta && along > 0.0f &&
        length(toPoint - terminalRay.Direction * along) <= PATH_TRACE_PSR_CHAIN_TOLERANCE * pathLength;
}

bool PathTracePsrPureMirror(PathTraceSurface surface)
{
    return surface.deltaSpecular && surface.specularProbability >= 1.0f;
}

// The direct pass's K = 2 shading vertex: retraces A -> B (one ray) and accepts D if it lies on B's delta ray.
bool PathTracePsrChainDirectSurface(uint2 pixel, PathTraceRasterFirstVertex firstVertex, out float3 position,
    out PathTraceSurface surface)
{
    position = float3(0.0f, 0.0f, 0.0f);
    surface = (PathTraceSurface)0;
    RayDesc ray;

    if(!PathTracePsrChainFirstRay(firstVertex, ray))
    {
        return false;
    }

    PathTracePayload payload;
    payload.hitDistance = -1.0f;
    payload.geometricNormal = float3(0.0f, 0.0f, 1.0f);
    payload.shadingNormal = float3(0.0f, 0.0f, 1.0f);
    payload.uv = float2(0.0f, 0.0f);
    payload.materialIndex = 0u;
#if defined(PATH_TRACE_DIRECT_COMPUTE)
    // The compute direct pass: the same ray and closest-hit evaluation with inline RayQuery.
    PathTraceTraceRadianceInline(ray, 0xFF, payload);
#elif defined(PATH_TRACE_SER)
    PathTraceTraceRadianceReordered(ray, 0xFF, false, payload);
#else
    TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, PATH_TRACE_MISS_RADIANCE, ray, payload);
#endif
    PathTraceSurface chainSurface;

    if(payload.hitDistance < 0.0f || !PathTraceBuildSurface(payload, ray.Direction, chainSurface) ||
        !PathTracePsrPureMirror(chainSurface))
    {
        return false;
    }

    const float3 chainHit = ray.Origin + ray.Direction * payload.hitDistance;
    float3 exactPoint;
    float pathLength;
    uint storedMaterial;
    float3 storedNormal;
    RayDesc terminalRay;
    float along;
    return PathTracePsrChainSurface(pixel, firstVertex, chainHit - psrGBufferCameraPosition, chainSurface.normal,
        position, exactPoint, surface, pathLength, storedMaterial, storedNormal) &&
        PathTracePsrChainOnRay(chainHit, chainSurface, exactPoint, pathLength, terminalRay, along);
}

// The indirect pass's terminal check of its traced B -> D hit against the reconstruction.
bool PathTracePsrChainTerminalMatches(PathTracePayload payload, RayDesc terminalRay, float along, float pathLength,
    uint storedMaterial, float3 storedNormal, float3 chainNormal, float3 planeNormal)
{
    if(payload.hitDistance < 0.0f || payload.materialIndex != storedMaterial ||
        !(abs(payload.hitDistance - along) <= PATH_TRACE_PSR_CHAIN_TOLERANCE * pathLength))
    {
        return false;
    }

    const float facing = dot(payload.geometricNormal, -terminalRay.Direction) < 0.0f ? -1.0f : 1.0f;
    const float3 virtualNormal = PsrReflectDirection(PsrReflectDirection(payload.geometricNormal * facing, chainNormal),
        planeNormal);
    return dot(normalize(virtualNormal), normalize(storedNormal)) >= PATH_TRACE_PSR_CHAIN_NORMAL_COSINE;
}
#endif
#else
// The closest-hit payload of a replaced pixel's vertex 1, as the path loop's continuation would have returned it.
PathTracePayload PathTraceEffectivePrimaryReplacementPayload(uint2 pixel)
{
    PathTracePsrHit hit = PathTracePsrLoadHit(pixel);
    PathTracePayload payload;
    payload.hitDistance = hit.hitDistance;
    payload.geometricNormal = hit.geometricNormal;
    payload.shadingNormal = hit.shadingNormal;
    payload.uv = hit.uv;
    payload.materialIndex = hit.materialIndex;
    return payload;
}

// PSR.1c-A: the physical replacement surface of a SURFACE pixel and its position, built from the record exactly as the
// path loop builds vertex 1: the vertex-0 delta sample's direction (independent of the randoms), the loop's origin
// offset, the recorded payload and PathTraceBuildSurface, whose view is the reflected direction -d.
bool PathTracePsrReplacementSurface(uint2 pixel, PathTraceRasterFirstVertex firstVertex, out float3 position,
    out PathTraceSurface surface)
{
    position = float3(0.0f, 0.0f, 0.0f);
    surface = (PathTraceSurface)0;
    float3 diffuseShare;
    bool specularStrategy;
    PathTraceBSDFSample mirrorSample = PathTraceSampleBSDFLobes(firstVertex.surface, float3(0.0f, 0.0f, 0.0f),
        diffuseShare, specularStrategy);

    if(!mirrorSample.valid || !mirrorSample.delta)
    {
        return false;
    }

    PathTracePayload payload = PathTraceEffectivePrimaryReplacementPayload(pixel);
    float3 origin = PathTraceOffsetRayOrigin(firstVertex.position, firstVertex.surface.geometricNormal);
    position = origin + mirrorSample.direction * payload.hitDistance;
    return PathTraceBuildSurface(payload, mirrorSample.direction, surface);
}
#endif

// The physical vertex at which the effective primary surface is shaded. True for a replaced surface (position and
// surface are B's); otherwise position and surface are the raster vertex 0's.
bool PathTraceEffectivePrimaryShadingVertex(uint2 pixel, PathTraceRasterFirstVertex firstVertex, out float3 position,
    out PathTraceSurface surface)
{
#if PATH_TRACE_PSR_CHAIN >= 2
    // Multibounce PSR: a two-mirror chain's D, if the retraced chain verifies; otherwise the mirror (nothing is lit).
    if(PathTraceEffectivePrimaryVertex(firstVertex) == 2u &&
        PathTracePsrChainDirectSurface(pixel, firstVertex, position, surface))
    {
        return true;
    }
#endif
    if(PathTraceEffectivePrimaryVertex(firstVertex) == 1u &&
        PathTracePsrReplacementSurface(pixel, firstVertex, position, surface))
    {
        return true;
    }

    position = firstVertex.position;
    surface = firstVertex.surface;
    return false;
}

// A replaced pixel's hit, read where vertex 1 consumes it.
PathTracePsrHit PathTracePsrLoadHit(uint2 pixel)
{
    RWByteAddressBuffer records = ResourceDescriptorHeap[psrRecordIndex];
    uint address = PathTracePsrRecordAddress(pixel);
    uint header = records.Load(address);
    uint4 first = records.Load4(address + 4u);
    uint4 second = records.Load4(address + 20u);
    PathTracePsrHit hit;
    hit.materialIndex = header >> PATH_TRACE_PSR_STATE_BITS;
    hit.hitDistance = asfloat(first.x);
    hit.geometricNormal = asfloat(first.yzw);
    hit.shadingNormal = asfloat(second.xyz);
    hit.uv = float2(asfloat(second.w), asfloat(records.Load(address + 36u)));
    return hit;
}
#endif

// RTXPT D.1B.2 signal extraction for the raster primaries. A sample's radiance splits exactly into the parts its
// vertex-0 BSDF lobes carry: vertex-0 direct light is evaluated per lobe with the full estimator's MIS weights and
// densities, and everything after vertex 0 is scaled by the diffuse share f_diffuse / f of the vertex-0 BSDF
// sample (carried as a diffuse throughput), never classified by the sampled lobe. The specular part is the rest.
static const float PATH_TRACE_EXTRACTION_NO_SPECULAR = -1.0f;
static const float PATH_TRACE_EXTRACTION_SPECULAR_PENDING = -2.0f;
static const float PATH_TRACE_EXTRACTION_NO_DIFFUSE = -1.0f;
static const float PATH_TRACE_EXTRACTION_DIFFUSE_PENDING = -2.0f;
// NRD.4A: the diffuse output's alpha is REBLUR's normalized hit distance (NRD.hlsli REBLUR_FrontEnd_GetNormHitDist),
// with the nrd::ReblurHitDistanceParameters defaults A and B; a REBLUR instance consuming it must be given them. For the
// diffuse lobe (roughness 1) the normalization distance is A + B |viewZ| (_NRD_GetSpecMagicCurve(1) = 1, so C drops out).
static const float PATH_TRACE_EXTRACTION_HIT_DISTANCE_A = 3.0f;
static const float PATH_TRACE_EXTRACTION_HIT_DISTANCE_B = 0.1f;
// NRD_EPS: a taken lobe never normalizes to 0, which means "no data".
static const float PATH_TRACE_EXTRACTION_MINIMUM_NORMALIZED_DISTANCE = 1.0e-6f;
#ifdef PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY
// NRD.RELAX.2: rawDiffuseHitDistanceIndex when no raw diffuse hit distance is requested.
static const uint PATH_TRACE_NO_RAW_DIFFUSE_HIT_DISTANCE = 0xFFFFFFFFu;
#endif

struct PathTraceExtraction
{
    // The diffuse part of the sample's radiance.
    float3 diffuse;
    // The diffuse part of the path throughput: f_diffuse / f of the vertex-0 BSDF sample times the throughput,
    // carried beside it with the same BSDF weights and roulette.
    float3 diffuseThroughput;
    // Primary specular hit distance: the length of the vertex-0 continuation ray when the specular strategy was
    // selected there (delta mirrors included), PATH_TRACE_HALF_MAXIMUM when that ray missed, and negative when the
    // sample made no specular continuation.
    float hitDistance;
    // NRD.3 primary diffuse hit distance: the same for the diffuse (cosine) strategy. Negative when the sample made no
    // diffuse continuation.
    float diffuseHitDistance;
};

// NRD.4A: REBLUR's diffuse normalization distance at a raster-primary vertex 0. View Z is the distance along the camera's
// forward axis (the RTXDI bridge's linear depth), the axis taken from the camera-relative inverse view-projection.
float PathTraceExtractionDiffuseNormalization(float3 position)
{
    float4 nearPoint = mul(inverseCameraRelativeViewProjection, float4(0.0f, 0.0f, 0.0f, 1.0f));
    float4 farPoint = mul(inverseCameraRelativeViewProjection, float4(0.0f, 0.0f, 1.0f, 1.0f));
    float3 forward = normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);
    float viewZ = abs(dot(forward, position - cameraPosition));
    return PATH_TRACE_EXTRACTION_HIT_DISTANCE_A + viewZ * PATH_TRACE_EXTRACTION_HIT_DISTANCE_B;
}

// NRD.4A: one sample's normalized distance; a missed continuation counts as 1, as in NRD's reference integration.
float PathTraceExtractionNormalizedHitDistance(float hitDistance, float normalization)
{
    if(hitDistance >= PATH_TRACE_HALF_MAXIMUM)
    {
        return 1.0f;
    }

    return max(saturate(hitDistance / normalization), PATH_TRACE_EXTRACTION_MINIMUM_NORMALIZED_DISTANCE);
}

PathTraceExtraction PathTraceEmptyExtraction()
{
    PathTraceExtraction extraction;
    extraction.diffuse = float3(0.0f, 0.0f, 0.0f);
    extraction.diffuseThroughput = float3(0.0f, 0.0f, 0.0f);
    extraction.hitDistance = PATH_TRACE_EXTRACTION_NO_SPECULAR;
    extraction.diffuseHitDistance = PATH_TRACE_EXTRACTION_NO_DIFFUSE;
    return extraction;
}

// The path loop shared by every primary constructor. The raster primary takes vertex 0 from its decoded
// G-buffer sample where the camera primaries trace and build their first hit; everything after it (sampling,
// BSDF, MIS, bounces, roulette) is the same code. extract is a literal at every call site; without it the
// extraction state is never read, and the loop is the production loop.

// NVIDIA SHARC v1.8.3 (the SharcNv/ libraries only, PathTraceSharcNv.hlsli): its update and render hooks. In every other
// library PATH_TRACE_SHARC_NV expands to nothing, the loop's depth is maxPathDepth and every vertex is shaded.
#if defined(PATH_TRACE_SHARC_NV_TRAINING) || defined(PATH_TRACE_SHARC_NV_RENDER)
#include "PathTraceSharcNv.hlsli"
#define PATH_TRACE_SHARC_NV(statement) statement
#define PATH_TRACE_LOOP_MAX_DEPTH PATH_TRACE_SHARC_NV_MAX_DEPTH
#else
#define PATH_TRACE_SHARC_NV(statement)
#define PATH_TRACE_LOOP_MAX_DEPTH maxPathDepth
#define PATH_TRACE_SHARC_NV_SHADES(vertex) true
#define PATH_TRACE_SHARC_NV_CLAMPS true
#endif

float3 PathTraceTracePath(RayDesc ray, uint primary, PathTraceRasterFirstVertex rasterFirstVertex,
    inout PathTraceRandom random, inout uint rayCount, bool extract, inout PathTraceExtraction extraction)
{
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    bool previousDelta = true;
    float previousPdf = 0.0f;
    PATH_TRACE_SHARC_NV(PathTraceSharcNvPath sharcNvPath = PathTraceSharcNvBegin(rasterFirstVertex));
#ifdef PATH_TRACE_PSR_LIBRARY
    // PSR.1c-A: the vertex whose direct light RTXDI estimated (the direct pass's split direct radiance).
    uint rtxdiVertex = primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT ?
        PathTraceEffectivePrimaryVertex(rasterFirstVertex) : 0u;
#endif
#if PATH_TRACE_PSR_CHAIN >= 2
    // Multibounce PSR: a two-mirror chain traces A -> B (vertex 1) and B -> D (vertex 2) as ordinary vertices, from the
    // replacement pass's first origin, and verifies D (PathTracePsrChainTerminalMatches). Accepted, vertex 2 is the
    // guides' D, lit by RTXDI; rejected here or by the direct pass, rtxdiVertex becomes 0 (mirror A, which has no
    // non-delta lobe) and the path is the ordinary one. The extraction splits at vertex 2 either way.
    const bool psrChain = primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT && rtxdiVertex == 2u;
    const uint psrExtractionVertex = rtxdiVertex;
    bool psrChainValid = psrChain;
    float3 psrChainPoint = float3(0.0f, 0.0f, 0.0f);
    PathTraceSurface psrChainSurface = (PathTraceSurface)0;

    if(psrChain)
    {
        RWByteAddressBuffer chainDirect = ResourceDescriptorHeap[splitDirectRadianceIndex];

        if(asfloat(chainDirect.Load(PathTraceSplitDirectRadianceAddress(g_splitPixel, g_splitSampleOffset))) < 0.0f)
        {
            psrChainValid = false;
            rtxdiVertex = 0u;
        }
    }
#endif
#ifdef PATH_TRACE_PSR_GBUFFER
    // PSR.2 Stage 3B: the vertex whose lobes the extraction splits by, the effective primary: B (vertex 1) for a replaced
    // pixel, which the denoisers filter as the G-buffer's virtual surface, and the mirror's throughput only scales it.
#if PATH_TRACE_PSR_CHAIN >= 2
#define PATH_TRACE_EXTRACTION_VERTEX psrExtractionVertex
#else
#define PATH_TRACE_EXTRACTION_VERTEX rtxdiVertex
#endif
#else
#define PATH_TRACE_EXTRACTION_VERTEX 0u
#endif

    // vertex counts surface interactions. The ray traced after the last allowed vertex is still
    // traced so distant lights reached by BSDF sampling pair with that vertex's light samples.
    for(uint vertex = 0u; vertex <= PATH_TRACE_LOOP_MAX_DEPTH; ++vertex)
    {
        float3 position;
        PathTraceSurface surface;

        if(primary == PATH_TRACE_PRIMARY_RASTER || primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT)
        {
            // RTXPT.2 R3-F: every lane executes this one TraceRay call at every vertex. Lanes of a warp run
            // different samples at different vertices; a call skipped by some of them (the G-buffer vertex)
            // serialized the warp and cost up to 3.7x at 8 spp (R3-P). At vertex 0 the ray includes no instance
            // (mask 0) over an empty interval, so nothing is traversed and no intersection is counted; the
            // vertex is the decoded G-buffer sample.
            bool gBufferVertex = vertex == 0u;
            PathTracePayload payload;
            payload.hitDistance = -1.0f;
            payload.geometricNormal = float3(0.0f, 0.0f, 1.0f);
            payload.shadingNormal = float3(0.0f, 0.0f, 1.0f);
            payload.uv = float2(0.0f, 0.0f);
            payload.materialIndex = 0u;
#ifdef PATH_TRACE_PSR_LIBRARY
            // PSR.1a: a replaced pixel's vertex 1 is its recorded hit. The call site stays uniform (R3-F): the ray is
            // the loop's own continuation with mask 0, so nothing is traversed, and the payload is then the record's.
#if PATH_TRACE_PSR_CHAIN >= 2
            // Multibounce PSR: a chain's replacement vertex is D (vertex 2), traced for its verification.
            bool replacementVertex = (primary == PATH_TRACE_PRIMARY_RASTER ||
                primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT) && vertex == (psrChain ? 2u : 1u) &&
                PathTracePsrReplaced(rasterFirstVertex.psrState);
            bool untracedVertex = gBufferVertex || (replacementVertex && !psrChain);
#else
            bool replacementVertex = (primary == PATH_TRACE_PRIMARY_RASTER ||
                primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT) && vertex == 1u &&
                PathTracePsrReplaced(rasterFirstVertex.psrState);
            bool untracedVertex = gBufferVertex || replacementVertex;
#endif
            // PSR.1a-P: the call stays unconditional. A wave-uniform skip (WaveActiveAllTrue(replacementVertex)) is
            // equivalent, but placing the call under a branch made this pass 0.5-1.8 ms slower (PSR1aPPerformance.md).
#ifdef PATH_TRACE_SER
            PathTraceTraceRadianceReordered(ray, untracedVertex ? 0x00 : 0xFF, !untracedVertex, payload);
#else
            TraceRay(scene, RAY_FLAG_NONE, untracedVertex ? 0x00 : 0xFF, 0, 1, PATH_TRACE_MISS_RADIANCE, ray, payload);
#endif

#if PATH_TRACE_PSR_CHAIN >= 2
            if(replacementVertex && !psrChain)
#else
            if(replacementVertex)
#endif
            {
                payload = PathTraceEffectivePrimaryReplacementPayload(rasterFirstVertex.psrPixel);
            }
#else
#ifdef PATH_TRACE_SER
            // SER S5A: the same single call site, as a HitObject. The empty vertex-0 ray keeps mask 0 and is never
            // reordered (it misses without traversal, exactly as before); continuation rays reorder.
            PathTraceTraceRadianceReordered(ray, gBufferVertex ? 0x00 : 0xFF, !gBufferVertex, payload);
#else
            TraceRay(scene, RAY_FLAG_NONE, gBufferVertex ? 0x00 : 0xFF, 0, 1, PATH_TRACE_MISS_RADIANCE, ray, payload);
#endif
#endif

            if(gBufferVertex)
            {
                if(rasterFirstVertex.state == 0)
                {
                    float3 background = PathTraceMissRadiance(ray.Origin, rasterFirstVertex.backgroundDirection, previousDelta,
                        previousPdf, true);
                    radiance += throughput * background;
                    break;
                }

                if(rasterFirstVertex.state < 0 || vertex == PATH_TRACE_LOOP_MAX_DEPTH)
                {
                    break;
                }

                position = rasterFirstVertex.position;
                surface = rasterFirstVertex.surface;
            }
            else
            {
                // The camera primaries' hit handling, unchanged.
                ++rayCount;

                if(extract && vertex == PATH_TRACE_EXTRACTION_VERTEX + 1u &&
                    extraction.hitDistance == PATH_TRACE_EXTRACTION_SPECULAR_PENDING)
                {
                    extraction.hitDistance = payload.hitDistance < 0.0f ? PATH_TRACE_HALF_MAXIMUM : payload.hitDistance;
                }

                if(extract && vertex == PATH_TRACE_EXTRACTION_VERTEX + 1u &&
                    extraction.diffuseHitDistance == PATH_TRACE_EXTRACTION_DIFFUSE_PENDING)
                {
                    extraction.diffuseHitDistance = payload.hitDistance < 0.0f ? PATH_TRACE_HALF_MAXIMUM :
                        payload.hitDistance;
                }

                if(payload.hitDistance < 0.0f)
                {
                    // RTXPT.3 Gate 1: after a ReSTIR DI vertex 0, RTXDI alone estimated the sun and HDRI through
                    // the non-delta lobes, so a non-delta continuation that misses adds nothing. A delta-lobe
                    // continuation keeps its full radiance, as no light sample can produce it. The indirect pass's
                    // vertex 0 is that RTXDI estimate, made by the direct pass.
#ifdef PATH_TRACE_PSR_LIBRARY
                    // PSR.1c-A: the first non-delta continuation leaving the RTXDI vertex, which is vertex 1 (the
                    // replacement surface) for a SURFACE pixel. The mirror's own delta continuation keeps its radiance.
                    bool directFromRtxdi = primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT &&
                        vertex == rtxdiVertex + 1u && !previousDelta;
#else
                    bool directFromRtxdi = primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT && vertex == 1u &&
                        !previousDelta;
#endif

                    if(!directFromRtxdi)
                    {
                        float3 missRadiance = PathTraceMissRadiance(ray.Origin, ray.Direction, previousDelta, previousPdf,
                            vertex == 0u);

                        // The miss of the ray leaving vertex - 1 (vertex 0's own BSDF-sampled light stays unclamped).
                        if(PathTraceIndirectClampActive(vertex - 1u) && PATH_TRACE_SHARC_NV_CLAMPS)
                        {
                            missRadiance *= PathTraceIndirectClampScale(throughput * missRadiance);
                        }

                        radiance += throughput * missRadiance;
                        PATH_TRACE_SHARC_NV(PathTraceSharcNvMiss(sharcNvPath, missRadiance));

                        if(extract)
                        {
                            extraction.diffuse += extraction.diffuseThroughput * missRadiance;
                        }
                    }
#if RTXDI_ENVIRONMENT_STRATEGY == RTXDI_ENVIRONMENT_STRATEGY_SEPARATE_MIS
                    else
                    {
                        // PT.2B Approach B: RTXDI estimated only the sun and the table lights through the non-delta lobes; the
                        // HDRI's BSDF-sampled share is this escaped continuation, power-heuristic weighted against the direct
                        // pass's HDRI light sample. It is the RTXDI vertex's direct light, so it is never clamped.
                        float3 missRadiance = PathTraceMissEnvironmentRadiance(ray.Direction, previousPdf);
                        radiance += throughput * missRadiance;
                        PATH_TRACE_SHARC_NV(PathTraceSharcNvMiss(sharcNvPath, missRadiance));

                        if(extract)
                        {
                            extraction.diffuse += extraction.diffuseThroughput * missRadiance;
                        }
                    }
#endif

                    break;
                }

                if(vertex == PATH_TRACE_LOOP_MAX_DEPTH)
                {
                    break;
                }

#if PATH_TRACE_PSR_CHAIN >= 2
                // Multibounce PSR: the chain's traced D. Verified, it becomes the guides' D; otherwise the traced hit.
                if(replacementVertex && psrChain)
                {
                    bool accepted = false;

                    if(psrChainValid)
                    {
                        float3 exactPoint;
                        float pathLength;
                        uint storedMaterial;
                        float3 storedNormal;
                        RayDesc terminalRay;
                        float along;
                        accepted = PathTracePsrChainSurface(rasterFirstVertex.psrPixel, rasterFirstVertex,
                            psrChainPoint - psrGBufferCameraPosition, psrChainSurface.normal, position, exactPoint, surface,
                            pathLength, storedMaterial, storedNormal) &&
                            PathTracePsrChainOnRay(psrChainPoint, psrChainSurface, exactPoint, pathLength, terminalRay,
                            along) && PathTracePsrChainTerminalMatches(payload, terminalRay, along, pathLength,
                            storedMaterial, storedNormal, psrChainSurface.normal, rasterFirstVertex.surface.normal);
                    }
#ifdef PATH_TRACE_PSR_CHAIN_FORCE_REJECT
                    // Verification only: the indirect pass also rejects every chain pixel with (x + 2y) % 6 == 0.
                    accepted = accepted && (rasterFirstVertex.psrPixel.x + 2u * rasterFirstVertex.psrPixel.y) % 6u != 0u;
#endif

                    if(!accepted)
                    {
                        rtxdiVertex = 0u;
                        position = ray.Origin + ray.Direction * payload.hitDistance;

                        if(!PathTraceBuildSurface(payload, ray.Direction, surface))
                        {
                            break;
                        }
                    }
                }
                else
#endif
#ifdef PATH_TRACE_PSR_GBUFFER
                // PSR.2: a replaced pixel's vertex 1 is B reconstructed from the G-buffer, not a hit.
                if(replacementVertex)
                {
                    if(!PathTracePsrReplacementSurface(rasterFirstVertex.psrPixel, rasterFirstVertex, position, surface))
                    {
                        break;
                    }
                }
                else
#endif
                {
                    position = ray.Origin + ray.Direction * payload.hitDistance;

                    if(!PathTraceBuildSurface(payload, ray.Direction, surface))
                    {
                        break;
                    }
                }
            }
        }
        else
        {
            PathTracePayload payload;
            payload.hitDistance = -1.0f;
            payload.geometricNormal = float3(0.0f, 0.0f, 1.0f);
            payload.shadingNormal = float3(0.0f, 0.0f, 1.0f);
            payload.uv = float2(0.0f, 0.0f);
            payload.materialIndex = 0u;
#ifdef PATH_TRACE_SER
            // SER S3A: of the camera primaries, Reference only (Pixel Centre keeps TraceRay). Its camera ray
            // (vertex 0) is already coherent in launch order, so only the continuation rays reorder; every vertex
            // still uses this one call site, as all lanes reach it.
            if(primary == PATH_TRACE_PRIMARY_CAMERA)
            {
                PathTraceTraceRadianceReordered(ray, 0xFF, vertex > 0u, payload);
            }
            else
            {
                TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, PATH_TRACE_MISS_RADIANCE, ray, payload);
            }
#else
            PathTraceCountReferenceRay(vertex == 0u ? 0u : 1u);
            TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, PATH_TRACE_MISS_RADIANCE, ray, payload);
#endif
            ++rayCount;

            if(payload.hitDistance < 0.0f)
            {
                float3 missRadiance = PathTraceMissRadiance(ray.Origin, ray.Direction, previousDelta, previousPdf, vertex == 0u);

                // Camera-ray misses (vertex 0) and vertex 0's BSDF-sampled light stay unclamped.
                if(vertex >= 1u && PathTraceIndirectClampActive(vertex - 1u))
                {
                    missRadiance *= PathTraceIndirectClampScale(throughput * missRadiance);
                }

                radiance += throughput * missRadiance;
                break;
            }

            if(vertex == maxPathDepth)
            {
                break;
            }

            position = ray.Origin + ray.Direction * payload.hitDistance;

            if(!PathTraceBuildSurface(payload, ray.Direction, surface))
            {
                break;
            }
        }

#if PATH_TRACE_PSR_CHAIN >= 2
        // Multibounce PSR: the chain's B, which must be a pure-delta mirror.
        if(psrChain && vertex == 1u)
        {
            psrChainPoint = position;
            psrChainSurface = surface;

            if(!PathTracePsrPureMirror(surface))
            {
                psrChainValid = false;
                rtxdiVertex = 0u;
            }
        }
#endif
#ifdef PATH_TRACE_SHARC_NV_RENDER
        // NVIDIA SHARC: an admitted, cached vertex supplies everything from its own next-event estimation on.
        if(PathTraceSharcNvQuery(sharcNvPath, primary, vertex, position, ray.Origin, surface, throughput, radiance, extract,
            extraction))
        {
            break;
        }
#endif
        bool hasNonDeltaLobe = !surface.deltaSpecular || surface.specularProbability < 1.0f;

        if(hasNonDeltaLobe && PATH_TRACE_SHARC_NV_SHADES(vertex))
        {
            float3 direct;
            float3 directDiffuse = float3(0.0f, 0.0f, 0.0f);

#ifdef PATH_TRACE_PSR_LIBRARY
            // PSR.1c-A: the RTXDI estimate at the RTXDI vertex, drawing and discarding the randoms exhaustive NEE draws.
            if(primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT && vertex == rtxdiVertex)
#else
            if(primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT && vertex == 0u)
#endif
            {
                direct = PathTraceSplitDirectLighting(random);

                if(extract)
                {
                    directDiffuse = PathTraceSplitDirectDiffuse();
                }
            }
            else
            {
#ifdef PATH_TRACE_SHARC_NV_TRAINING
#ifdef PATH_TRACE_SAMPLED_NEE
                g_pathTraceNeeVertex = vertex;
#endif
                direct = PathTraceSharcNvTrainingDirectLighting(surface, position, random, rayCount, directDiffuse);
#ifdef PATH_TRACE_SAMPLED_NEE
                g_pathTraceNeeVertex = 0u;
#endif
#else
#ifdef PATH_TRACE_SAMPLED_NEE
                g_pathTraceNeeVertex = vertex;
#endif
                direct = PathTraceDirectLightingLobes(surface, position, random, rayCount, directDiffuse);
#ifdef PATH_TRACE_SAMPLED_NEE
                g_pathTraceNeeVertex = 0u;
#endif
#endif
            }

            // Indirect next-event estimation (vertex >= 1): one contribution, scaled with its diffuse share.
#ifdef PATH_TRACE_SAMPLED_NEE
            const bool clampNee = PathTraceIndirectClampActive(vertex) && PATH_TRACE_SHARC_NV_CLAMPS &&
                (sceneLightPadding.y & PATH_TRACE_SAMPLED_NEE_NO_NEE_CLAMP) == 0u;

            if(clampNee)
#else
            if(PathTraceIndirectClampActive(vertex) && PATH_TRACE_SHARC_NV_CLAMPS)
#endif
            {
                direct *= PathTraceIndirectClampScale(throughput * direct);
            }

#ifdef PATH_TRACE_SAMPLED_NEE
            // M4.2B: the sampled finite-light estimate kept apart by the exempt or separate clamp policy.
            {
                float3 sampledFinite = g_pathTraceSampledFinite;
                float3 sampledFiniteDiffuse = g_pathTraceSampledFiniteDiffuse;
                g_pathTraceSampledFinite = float3(0.0f, 0.0f, 0.0f);
                g_pathTraceSampledFiniteDiffuse = float3(0.0f, 0.0f, 0.0f);

                if(clampNee && PathTraceSampledNeeClampPolicy() == PATH_TRACE_SAMPLED_NEE_CLAMP_SEPARATE)
                {
                    const float sampledScale = PathTraceIndirectClampScale(throughput * sampledFinite);
                    sampledFinite *= sampledScale;
                    sampledFiniteDiffuse *= sampledScale;
                }

                direct += sampledFinite;
                directDiffuse += sampledFiniteDiffuse;
            }
#endif

            radiance += throughput * direct;
#ifdef PATH_TRACE_SHARC_NV_TRAINING
            if(!PathTraceSharcNvDeposit(sharcNvPath, vertex, position, surface, direct, random))
            {
                break;
            }
#endif

            // Vertex 0's direct light is split per lobe; later vertices' is carried by the diffuse throughput.
            if(extract)
            {
                extraction.diffuse += vertex == PATH_TRACE_EXTRACTION_VERTEX ? throughput * directDiffuse :
                    extraction.diffuseThroughput * direct;
            }
        }

        float3 diffuseShare;
        bool specularStrategy;
        PathTraceBSDFSample bsdfSample = PathTraceSampleBSDFLobes(surface, PathTraceNextFloat3(random), diffuseShare,
            specularStrategy);

        if(!bsdfSample.valid)
        {
            break;
        }

        if(extract)
        {
            if(vertex == PATH_TRACE_EXTRACTION_VERTEX)
            {
#ifdef PATH_TRACE_PSR_GBUFFER
                // PSR.2: the throughput before B's sample is the mirror's delta weight.
                extraction.diffuseThroughput = throughput * diffuseShare * bsdfSample.weight;
#else
                // The throughput before vertex 0's sample is 1.
                extraction.diffuseThroughput = diffuseShare * bsdfSample.weight;
#endif
                extraction.hitDistance = specularStrategy ? PATH_TRACE_EXTRACTION_SPECULAR_PENDING :
                    PATH_TRACE_EXTRACTION_NO_SPECULAR;
                extraction.diffuseHitDistance = specularStrategy ? PATH_TRACE_EXTRACTION_NO_DIFFUSE :
                    PATH_TRACE_EXTRACTION_DIFFUSE_PENDING;
            }
            else
            {
                extraction.diffuseThroughput *= bsdfSample.weight;
            }
        }

        throughput *= bsdfSample.weight;
        PATH_TRACE_SHARC_NV(PathTraceSharcNvSegment(sharcNvPath, vertex, bsdfSample, specularStrategy, surface));
        previousDelta = bsdfSample.delta;
        previousPdf = bsdfSample.pdf;

        // Russian roulette before continuing to a vertex at or beyond the configured depth. NVIDIA SHARC's update pass has
        // none: a deposit's importance to the paths that later read the voxel is not this path's throughput.
        float survivalRandom = PathTraceNextFloat(random);

#ifdef PATH_TRACE_SHARC_NV_TRAINING
        if(false)
#else
        if(vertex + 1u >= russianRouletteDepth)
#endif
        {
            float survival = min(max(throughput.r, max(throughput.g, throughput.b)), 0.95f);

            if(!(survival > 0.0f) || survivalRandom >= survival)
            {
                break;
            }

            throughput /= survival;

            if(extract)
            {
                extraction.diffuseThroughput /= survival;
            }
        }

        if(!all(isfinite(throughput)) || !any(throughput > 0.0f))
        {
            break;
        }

        ray.Origin = PathTraceOffsetRayOrigin(position, surface.geometricNormal);
#if PATH_TRACE_PSR_CHAIN >= 2
        // Multibounce PSR: A -> B leaves A's exact intersection, as the replacement pass's ray does.
        if(psrChain && vertex == 0u)
        {
            ray.Origin = PathTraceOffsetRayOrigin(psrGBufferCameraPosition + rasterFirstVertex.psrPlanePoint,
                surface.geometricNormal);
        }
#endif
        ray.Direction = bsdfSample.direction;
        ray.TMin = 0.0f;
        ray.TMax = PATH_TRACE_DISTANT_LIGHT_DISTANCE;
    }

#undef PATH_TRACE_EXTRACTION_VERTEX
#undef PATH_TRACE_LOOP_MAX_DEPTH
    return radiance;
}

// The reference mode's primary constructor: a jittered camera ray traced by the loop itself.
float3 PathTraceRadiance(uint2 pixel, inout PathTraceRandom random, inout uint rayCount)
{
    RayDesc ray;
    float maximumDistance;
    PathTraceCameraRay(pixel, PathTraceNextFloat2(random), ray.Origin, ray.Direction, maximumDistance);
    ray.TMin = 0.0f;
    ray.TMax = maximumDistance;

    PathTraceExtraction unusedExtraction = PathTraceEmptyExtraction();
    return PathTraceTracePath(ray, PATH_TRACE_PRIMARY_CAMERA, (PathTraceRasterFirstVertex)0, random, rayCount, false,
        unusedExtraction);
}

// RTXPT.2 R3 diagnostic primary: an unjittered camera ray through the pixel centre, the RT counterpart of
// the raster G-buffer sample. The jitter numbers are still drawn, so its random stream (and that of the
// raster primary below) stays aligned with the reference mode's.
float3 PathTracePixelCenterRadiance(uint2 pixel, inout PathTraceRandom random, inout uint rayCount)
{
    PathTraceNextFloat2(random);

    RayDesc ray;
    float maximumDistance;
    PathTraceCameraRay(pixel, float2(0.5f, 0.5f), ray.Origin, ray.Direction, maximumDistance);
    ray.TMin = 0.0f;
    ray.TMax = maximumDistance;

    PathTraceExtraction unusedExtraction = PathTraceEmptyExtraction();
    return PathTraceTracePath(ray, PATH_TRACE_PRIMARY_PIXEL_CENTER, (PathTraceRasterFirstVertex)0, random, rayCount,
        false, unusedExtraction);
}

// RTXPT.2 R3 raster primary: no camera ray; vertex 0 is the pixel's decoded G-buffer sample. The vertex-0
// TraceRay of the loop uses this well-formed, empty ray (and instance mask 0).
float3 PathTraceRasterPrimaryRadiance(PathTraceRasterFirstVertex rasterFirstVertex, inout PathTraceRandom random,
    inout uint rayCount, bool extract, inout PathTraceExtraction extraction)
{
    PathTraceNextFloat2(random);

    RayDesc emptyRay;
    emptyRay.Origin = float3(0.0f, 0.0f, 0.0f);
    emptyRay.Direction = float3(0.0f, 0.0f, 1.0f);
    emptyRay.TMin = 0.0f;
    emptyRay.TMax = 0.0f;
    return PathTraceTracePath(emptyRay, PATH_TRACE_PRIMARY_RASTER, rasterFirstVertex, random, rayCount, extract,
        extraction);
}

// RTXPT.3 ReSTIR DI indirect pass: the raster primary above, with vertex-0 direct light read from the direct pass.
float3 PathTraceRasterSplitIndirectRadiance(PathTraceRasterFirstVertex rasterFirstVertex, inout PathTraceRandom random,
    inout uint rayCount, bool extract, inout PathTraceExtraction extraction)
{
    PathTraceNextFloat2(random);

    RayDesc emptyRay;
    emptyRay.Origin = float3(0.0f, 0.0f, 0.0f);
    emptyRay.Direction = float3(0.0f, 0.0f, 1.0f);
    emptyRay.TMin = 0.0f;
    emptyRay.TMax = 0.0f;
    return PathTraceTracePath(emptyRay, PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT, rasterFirstVertex, random, rayCount,
        extract, extraction);
}

void PathTraceAddStatistic64(uint lowIndex, uint value)
{
    uint previous;
    InterlockedAdd(statisticsTexture[uint2(lowIndex, 0u)], value, previous);

    if(previous + value < previous)
    {
        InterlockedAdd(statisticsTexture[uint2(lowIndex + 1u, 0u)], 1u);
    }
}

float3 PathTracePrimaryRadiance(uint primary, uint2 pixel, PathTraceRasterFirstVertex rasterFirstVertex,
    inout PathTraceRandom random, inout uint rayCount, bool extract, inout PathTraceExtraction extraction)
{
    if(primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT)
    {
        return PathTraceRasterSplitIndirectRadiance(rasterFirstVertex, random, rayCount, extract, extraction);
    }

    if(primary == PATH_TRACE_PRIMARY_RASTER)
    {
        return PathTraceRasterPrimaryRadiance(rasterFirstVertex, random, rayCount, extract, extraction);
    }

    if(primary == PATH_TRACE_PRIMARY_PIXEL_CENTER)
    {
        return PathTracePixelCenterRadiance(pixel, random, rayCount);
    }

    return PathTraceRadiance(pixel, random, rayCount);
}

// extract (a literal, as primary): RTXPT D.1B.2 signal extraction, raster primaries only. The accumulation is
// written exactly as without it; in addition the frame's own diffuse and specular means go to the extraction
// outputs (the specular mean being the frame mean minus the diffuse one), with the primary specular hit
// distance in the specular alpha and (NRD.4A) the primary diffuse hit distance in the diffuse alpha, normalized for
// REBLUR (PathTraceExtractionNormalizedHitDistance). Pixels without a vertex-0 surface write zero radiance and no hit
// distance.
// NRD.RELAX.2: when requested, the same samples' raw diffuse hit distances also go to rawDiffuseHitDistanceIndex, for
// RELAX: the mean of the diffuse-strategy samples' first continuation lengths (misses as PATH_TRACE_HALF_MAXIMUM), NRD's
// reference estimator before REBLUR normalization; -1 where no sample took the diffuse strategy or there is no surface.
// NRD.RELAX.2B: compiled only into the raw-distance libraries (PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY). Every other
// library preprocesses to the pre-RELAX.2 function: the extra loop-carried sum alone changed the driver's rounding of the
// existing extraction arithmetic (NRDRelax2.md section 10).
void PathTraceRunRayGeneration(uint primary, bool extract)
{
    uint2 pixel = DispatchRaysIndex().xy;
    bool active = all(pixel < resolution);
    uint rayCount = 0u;
    uint rejectedSamples = 0u;
    uint completedSamples = 0u;

    if(active)
    {
        float3 sampleSum = float3(0.0f, 0.0f, 0.0f);
        float3 diffuseSum = float3(0.0f, 0.0f, 0.0f);
        // FOUNDATION.7: NRD_FrontEnd_SpecHitDistAveraging - the nearest specular hit of the frame's samples.
        float hitDistanceMinimum = PATH_TRACE_HALF_MAXIMUM;
        uint hitSamples = 0u;
        uint missSamples = 0u;
        float normalizedDiffuseHitDistanceSum = 0.0f;
#ifdef PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY
        float rawDiffuseHitDistanceSum = 0.0f;
#endif
        uint diffuseHitDistanceSamples = 0u;

        // The raster primary's first vertex is decoded once for all of this pixel's samples.
        PathTraceRasterFirstVertex rasterFirstVertex = (PathTraceRasterFirstVertex)0;

        if(primary == PATH_TRACE_PRIMARY_RASTER || primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT)
        {
            rasterFirstVertex = PathTraceDecodeRasterFirstVertex(pixel);
        }

#ifdef PATH_TRACE_PSR_LIBRARY
        // PSR.1a: Raster Primary; PSR.1c-A: and the split ReSTIR DI indirect pass. Extraction never runs from a PSR.1
        // library; PSR.2 Stage 3B extracts at the effective primary.
#ifdef PATH_TRACE_PSR_GBUFFER
        if(primary == PATH_TRACE_PRIMARY_RASTER || primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT)
#else
        if((primary == PATH_TRACE_PRIMARY_RASTER || primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT) && !extract)
#endif
        {
            PathTraceResolveEffectivePrimary(pixel, rasterFirstVertex);
        }
#endif

        if(primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT)
        {
            g_splitPixel = pixel;
        }

        float diffuseNormalization = 1.0f;

        if(extract && rasterFirstVertex.state > 0)
        {
#ifdef PATH_TRACE_PSR_GBUFFER
            // PSR.2: the view Z of the surface the denoisers see, the G-buffer's virtual point.
            diffuseNormalization = PathTraceExtractionDiffuseNormalization(
                rasterFirstVertex.psrState == PATH_TRACE_PSR_SURFACE ? PathTracePsrVirtualPosition(pixel) :
                rasterFirstVertex.position);
#else
            diffuseNormalization = PathTraceExtractionDiffuseNormalization(rasterFirstVertex.position);
#endif
        }

        for(uint sampleOffset = 0u; sampleOffset < samplesPerFrame; ++sampleOffset)
        {
            PathTraceRandom random = PathTraceCreateRandom(pixel, accumulatedSamples + sampleOffset);

            if(primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT)
            {
                g_splitSampleOffset = sampleOffset;
            }

            PathTraceExtraction extraction = PathTraceEmptyExtraction();
            float3 sampleRadiance = PathTracePrimaryRadiance(primary, pixel, rasterFirstVertex, random, rayCount, extract,
                extraction);

            // A non-finite estimate is a defect, never silently averaged; it is counted and dropped.
            if(!all(isfinite(sampleRadiance)) || any(sampleRadiance < 0.0f))
            {
                sampleRadiance = float3(0.0f, 0.0f, 0.0f);
                extraction.diffuse = float3(0.0f, 0.0f, 0.0f);
                ++rejectedSamples;
            }

            sampleSum += sampleRadiance;
            ++completedSamples;

            if(extract)
            {
                diffuseSum += all(isfinite(extraction.diffuse)) ? extraction.diffuse : float3(0.0f, 0.0f, 0.0f);

                if(extraction.hitDistance == PATH_TRACE_HALF_MAXIMUM)
                {
                    ++missSamples;
                }
                else if(extraction.hitDistance >= 0.0f)
                {
                    hitDistanceMinimum = min(hitDistanceMinimum, extraction.hitDistance);
                    ++hitSamples;
                }

                if(extraction.diffuseHitDistance >= 0.0f)
                {
                    normalizedDiffuseHitDistanceSum += PathTraceExtractionNormalizedHitDistance(
                        extraction.diffuseHitDistance, diffuseNormalization);
#ifdef PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY
                    rawDiffuseHitDistanceSum += extraction.diffuseHitDistance;
#endif
                    ++diffuseHitDistanceSamples;
                }
            }
        }

        float previousCount = float(accumulatedSamples);
        float totalCount = float(accumulatedSamples + samplesPerFrame);
        float3 mean = sampleSum / totalCount;

        if(accumulatedSamples > 0u)
        {
            mean += accumulationTexture[pixel].rgb * (previousCount / totalCount);
        }

        accumulationTexture[pixel] = float4(mean, 1.0f);
        outputTexture[pixel] = float4(min(mean, PATH_TRACE_HALF_MAXIMUM), 1.0f);

        if(extract)
        {
            float3 diffuseMean = float3(0.0f, 0.0f, 0.0f);
            float3 specularMean = float3(0.0f, 0.0f, 0.0f);
            float hitDistance = PATH_TRACE_EXTRACTION_NO_SPECULAR;
            float diffuseHitDistance = PATH_TRACE_EXTRACTION_NO_DIFFUSE;
#ifdef PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY
            float rawDiffuseHitDistance = PATH_TRACE_EXTRACTION_NO_DIFFUSE;
#endif

            if(rasterFirstVertex.state > 0)
            {
                float frameSamples = float(samplesPerFrame);
                diffuseMean = diffuseSum / frameSamples;
                specularMean = (sampleSum - diffuseSum) / frameSamples;

                // NRD 4.17.3: the minimum over the samples whose specular continuation hit (misses normalize to 1, so
                // they never win while a hit exists); all-miss is "at infinity".
                if(hitSamples > 0u)
                {
                    hitDistance = hitDistanceMinimum;
                }
                else if(missSamples > 0u)
                {
                    hitDistance = PATH_TRACE_HALF_MAXIMUM;
                }

                // NRD.4A: NRD's reference estimator, the mean of the diffuse-strategy samples' normalized distances
                // (misses as 1); no such sample stays "none".
                if(diffuseHitDistanceSamples > 0u)
                {
                    diffuseHitDistance = normalizedDiffuseHitDistanceSum / float(diffuseHitDistanceSamples);
#ifdef PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY
                    rawDiffuseHitDistance = rawDiffuseHitDistanceSum / float(diffuseHitDistanceSamples);
#endif
                }
            }

#ifdef PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY
            if(rawDiffuseHitDistanceIndex != PATH_TRACE_NO_RAW_DIFFUSE_HIT_DISTANCE)
            {
                RWTexture2D<float> rawDiffuseHitDistanceOutput = ResourceDescriptorHeap[rawDiffuseHitDistanceIndex];
                rawDiffuseHitDistanceOutput[pixel] = min(rawDiffuseHitDistance, PATH_TRACE_HALF_MAXIMUM);
            }
#endif

            RWTexture2D<float4> diffuseOutput = ResourceDescriptorHeap[diffuseOutputIndex];
            RWTexture2D<float4> specularOutput = ResourceDescriptorHeap[specularOutputIndex];
            diffuseOutput[pixel] = float4(clamp(diffuseMean, -PATH_TRACE_HALF_MAXIMUM, PATH_TRACE_HALF_MAXIMUM),
                min(diffuseHitDistance, PATH_TRACE_HALF_MAXIMUM));
            specularOutput[pixel] = float4(clamp(specularMean, -PATH_TRACE_HALF_MAXIMUM, PATH_TRACE_HALF_MAXIMUM),
                min(hitDistance, PATH_TRACE_HALF_MAXIMUM));
        }

    }

    // One atomic per wave keeps instrumentation cheap relative to the traced work.
    uint waveRays = WaveActiveSum(rayCount);
    uint waveSamples = WaveActiveSum(completedSamples);
    uint waveRejected = WaveActiveSum(rejectedSamples);

    if(WaveIsFirstLane())
    {
        PathTraceAddStatistic64(0u, waveRays);
        PathTraceAddStatistic64(2u, waveSamples);

        if(waveRejected > 0u)
        {
            InterlockedAdd(statisticsTexture[uint2(4u, 0u)], waveRejected);
        }
    }
}

// The reference mode (RTXPT.1): jittered camera rays.
[shader("raygeneration")]
void PathTraceRayGeneration()
{
    g_referenceRayCounts = referenceRayCountIndex != 0xFFFFFFFFu;
    PathTraceRunRayGeneration(PATH_TRACE_PRIMARY_CAMERA, false);
}

// RTXPT.2 raster-primary mode.
[shader("raygeneration")]
void PathTraceRasterPrimaryRayGeneration()
{
    PathTraceRunRayGeneration(PATH_TRACE_PRIMARY_RASTER, false);
}

#ifdef PATH_TRACE_PSR_LIBRARY
// PSR.1a: one record per pixel, before this library's raster primary. The continuation is built exactly as the path
// loop builds vertex 0's: the delta sample of the decoded G-buffer vertex (its direction does not depend on the
// randoms; random.x = 0 selects the delta lobe, whose probability is 1), the same origin offset and interval. Pixels
// that are not pure-delta mirrors trace nothing.
[shader("raygeneration")]
void PathTracePsrRayGeneration()
{
    uint2 pixel = DispatchRaysIndex().xy;

    if(any(pixel >= resolution))
    {
        return;
    }

    // PSR.1a-P: exact early rejection before the full decode. An eligible vertex is a surface (depth below 1) whose
    // finalized roughness is at most PATH_TRACE_DELTA_ROUGHNESS, and the 8-bit G-buffer roughness holds only k / 255,
    // so only 0 can be eligible.
    Texture2D<float> depthTexture = ResourceDescriptorHeap[gBufferDepthIndex];
    Texture2D<float4> materialTexture = ResourceDescriptorHeap[gBufferMaterialIndex];

    if(maxPathDepth == 0u || !(depthTexture.Load(int3(pixel, 0)) < 1.0f) || materialTexture.Load(int3(pixel, 0)).w > 0.0f)
    {
        PathTracePsrStoreState(pixel, PATH_TRACE_PSR_NONE);
        return;
    }

    PathTraceRasterFirstVertex firstVertex = PathTraceDecodeRasterFirstVertex(pixel);
    PathTracePayload payload;
    payload.hitDistance = -1.0f;
    payload.geometricNormal = float3(0.0f, 0.0f, 1.0f);
    payload.shadingNormal = float3(0.0f, 0.0f, 1.0f);
    payload.uv = float2(0.0f, 0.0f);
    payload.materialIndex = 0u;
    uint state = PATH_TRACE_PSR_NONE;

    if(PathTracePsrEligible(firstVertex))
    {
        float3 diffuseShare;
        bool specularStrategy;
        PathTraceBSDFSample mirrorSample = PathTraceSampleBSDFLobes(firstVertex.surface, float3(0.0f, 0.0f, 0.0f),
            diffuseShare, specularStrategy);
        state = PATH_TRACE_PSR_FALLBACK_DIRECTION;

        if(mirrorSample.valid && mirrorSample.delta)
        {
            RayDesc ray;
            ray.Origin = PathTraceOffsetRayOrigin(firstVertex.position, firstVertex.surface.geometricNormal);
            ray.Direction = mirrorSample.direction;
            ray.TMin = 0.0f;
            ray.TMax = PATH_TRACE_DISTANT_LIGHT_DISTANCE;
            TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, PATH_TRACE_MISS_RADIANCE, ray, payload);

            PathTraceSurface replacement;

            if(payload.hitDistance < 0.0f)
            {
                state = PATH_TRACE_PSR_SKY;
            }
            else if(!PathTraceBuildSurface(payload, ray.Direction, replacement))
            {
                state = PATH_TRACE_PSR_FALLBACK_SURFACE;
            }
            else if(payload.materialIndex > PATH_TRACE_PSR_MAXIMUM_MATERIAL_INDEX)
            {
                state = PATH_TRACE_PSR_FALLBACK_SURFACE;
            }
            else
            {
                state = replacement.deltaSpecular ? PATH_TRACE_PSR_FALLBACK_MIRROR : PATH_TRACE_PSR_SURFACE;
            }
        }
    }

    if(PathTracePsrReplaced(state))
    {
        PathTracePsrStoreRecord(pixel, state, payload);
    }
    else
    {
        PathTracePsrStoreState(pixel, state);
    }
}

// -----------------------------------------------------------------------------
// NVIDIA SHARC's libraries bind their cache constants at b3, which the guides use.
#if !defined(PATH_TRACE_SHARC_NV_TRAINING) && !defined(PATH_TRACE_SHARC_NV_RENDER)
// PSR.1b virtual surface guides (diagnostic consumers only). PathTracePsrGuideRayGeneration replaces the record pass on
// guide frames: it writes the identical record, from the identical ray and closest-hit evaluation, and in addition one
// guide per pixel. Physical coordinates (lighting, visibility) stay in the record and the raster vertex 0; the guide
// holds only virtual coordinates (reprojection) and identity.
//
// Conventions (single planar mirror at vertex 0; P the unlifted raster mirror point, n its viewer-facing geometric
// normal; B = origin + d t the physical replacement hit):
//   reflect(X)     = X - 2 n dot(n, X - P), the mirror image of a point; normals reflect as m - 2 n dot(n, m)
//   virtual point  Bv = reflect(B), on the pixel's camera ray
//   virtual normal reflect of B's shading normal, after facing it towards the incoming ray -d (so it faces the camera)
//   viewZ          dot(view row 2, Bv), +Z forward, linear, as Triangle.hlsl's view depth
//   motion         Triangle.hlsl's convention: currentUV - previousUV of the unjittered projections, and
//                  previous view depth - current view depth. The previous virtual point is reflect(T B), with T the
//                  replacement instance's current-to-previous world transform and the current plane (static mirror).
//   sky            a mirror-to-sky pixel's virtual image is the camera direction v at infinity: motion of the direction
//                  only (w = 0, rotation), viewZ 0.
// -----------------------------------------------------------------------------

cbuffer PathTracePsrGuideConstants : register(b3)
{
    // Unjittered current and previous view-projection; row 2 of the current and previous views (view depth).
    row_major float4x4 psrViewProjection;
    row_major float4x4 psrPreviousViewProjection;
    float4 psrViewDepthRow;
    float4 psrPreviousViewDepthRow;
    uint psrGuideIndex;
    // 1 when the previous camera and the previous instance transforms describe the immediately previous frame.
    uint psrHistoryValid;
    uint psrTransformCount;
    uint psrIdentityIndex;
};

// Current-to-previous world transform of each render object (TLAS InstanceID), rows of a 3x4 matrix; valid is 0 for
// objects without a resident instance.
struct PathTracePsrTransform
{
    float4 row0;
    float4 row1;
    float4 row2;
    uint valid;
    uint3 padding;
};

StructuredBuffer<PathTracePsrTransform> psrTransforms : register(t1);

// Guide states (word 2 bits 24-31).
static const uint PATH_TRACE_PSR_GUIDE_NONE = 0u;
static const uint PATH_TRACE_PSR_GUIDE_VALID = 1u;
static const uint PATH_TRACE_PSR_GUIDE_SKY = 2u;
// Current-frame guides valid, motion invalid:
static const uint PATH_TRACE_PSR_GUIDE_HISTORY_INVALID = 3u;
static const uint PATH_TRACE_PSR_GUIDE_TRANSFORM_INVALID = 4u;
static const uint PATH_TRACE_PSR_GUIDE_MIRROR_MOVED = 5u;
static const uint PATH_TRACE_PSR_GUIDE_PROJECTION_INVALID = 6u;
// replacement instance, replacement primitive, mirror identity | state << 24, viewZ, motion xyz, virtual normal (oct).
static const uint PATH_TRACE_PSR_GUIDE_STRIDE = 32u;

float2 PathTracePsrOctEncode(float3 normal)
{
    float2 projected = normal.xy / (abs(normal.x) + abs(normal.y) + abs(normal.z));

    if(normal.z < 0.0f)
    {
        projected = (1.0f - abs(projected.yx)) * select(projected.xy >= 0.0f, 1.0f, -1.0f);
    }

    return projected * 0.5f + 0.5f;
}

float3 PathTracePsrReflectPoint(float3 position, float3 planePoint, float3 planeNormal)
{
    return position - 2.0f * planeNormal * dot(planeNormal, position - planePoint);
}

float3 PathTracePsrReflectDirection(float3 direction, float3 planeNormal)
{
    return direction - 2.0f * planeNormal * dot(planeNormal, direction);
}

float3 PathTracePsrTransformPoint(PathTracePsrTransform transform, float3 position)
{
    float4 homogeneous = float4(position, 1.0f);
    return float3(dot(transform.row0, homogeneous), dot(transform.row1, homogeneous), dot(transform.row2, homogeneous));
}

bool PathTracePsrIsIdentity(PathTracePsrTransform transform)
{
    float4 difference = abs(transform.row0 - float4(1.0f, 0.0f, 0.0f, 0.0f)) + abs(transform.row1 -
        float4(0.0f, 1.0f, 0.0f, 0.0f)) + abs(transform.row2 - float4(0.0f, 0.0f, 1.0f, 0.0f));
    return transform.valid != 0u && all(difference < 1.0e-6f);
}

// Projects a point (w = 1) or direction (w = 0) to unjittered texture coordinates; false behind the camera.
bool PathTracePsrProject(float4x4 viewProjection, float4 position, out float2 uv)
{
    float4 clip = mul(viewProjection, position);
    uv = float2(0.0f, 0.0f);

    if(!(clip.w > 1.0e-6f) || !all(isfinite(clip)))
    {
        return false;
    }

    uv = clip.xy / clip.w * float2(0.5f, -0.5f) + 0.5f;
    return true;
}

void PathTracePsrStoreGuide(uint2 pixel, uint replacementInstance, uint replacementPrimitive, uint mirrorIdentity,
    uint guideState, float viewZ, float3 motion, float3 virtualNormal)
{
    RWByteAddressBuffer guides = ResourceDescriptorHeap[psrGuideIndex];
    uint address = (pixel.y * resolution.x + pixel.x) * PATH_TRACE_PSR_GUIDE_STRIDE;
    uint2 octal = uint2(round(saturate(PathTracePsrOctEncode(virtualNormal)) * 65535.0f));
    guides.Store4(address, uint4(replacementInstance, replacementPrimitive,
        (mirrorIdentity & 0x00FFFFFFu) | (guideState << 24), asuint(viewZ)));
    guides.Store4(address + 16u, uint4(asuint(motion), octal.x | (octal.y << 16)));
}

void PathTracePsrStoreGuideState(uint2 pixel, uint guideState)
{
    RWByteAddressBuffer guides = ResourceDescriptorHeap[psrGuideIndex];
    guides.Store((pixel.y * resolution.x + pixel.x) * PATH_TRACE_PSR_GUIDE_STRIDE + 8u, guideState << 24);
}

[shader("raygeneration")]
void PathTracePsrGuideRayGeneration()
{
    uint2 pixel = DispatchRaysIndex().xy;

    if(any(pixel >= resolution))
    {
        return;
    }

    // The record pass, statement for statement, with the guide payload.
    Texture2D<float> depthTexture = ResourceDescriptorHeap[gBufferDepthIndex];
    Texture2D<float4> materialTexture = ResourceDescriptorHeap[gBufferMaterialIndex];

    if(maxPathDepth == 0u || !(depthTexture.Load(int3(pixel, 0)) < 1.0f) || materialTexture.Load(int3(pixel, 0)).w > 0.0f)
    {
        PathTracePsrStoreState(pixel, PATH_TRACE_PSR_NONE);
        PathTracePsrStoreGuideState(pixel, PATH_TRACE_PSR_GUIDE_NONE);
        return;
    }

    PathTraceRasterFirstVertex firstVertex = PathTraceDecodeRasterFirstVertex(pixel);
    PathTracePsrGuidePayload guidePayload;
    guidePayload.hitDistance = -1.0f;
    guidePayload.geometricNormal = float3(0.0f, 0.0f, 1.0f);
    guidePayload.shadingNormal = float3(0.0f, 0.0f, 1.0f);
    guidePayload.uv = float2(0.0f, 0.0f);
    guidePayload.materialIndex = 0u;
    guidePayload.instanceID = 0xFFFFFFFFu;
    guidePayload.primitiveIndex = 0xFFFFFFFFu;
    PathTracePayload payload;
    payload.hitDistance = -1.0f;
    payload.geometricNormal = float3(0.0f, 0.0f, 1.0f);
    payload.shadingNormal = float3(0.0f, 0.0f, 1.0f);
    payload.uv = float2(0.0f, 0.0f);
    payload.materialIndex = 0u;
    uint state = PATH_TRACE_PSR_NONE;
    RayDesc ray;
    ray.Origin = float3(0.0f, 0.0f, 0.0f);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMin = 0.0f;
    ray.TMax = 0.0f;

    if(PathTracePsrEligible(firstVertex))
    {
        float3 diffuseShare;
        bool specularStrategy;
        PathTraceBSDFSample mirrorSample = PathTraceSampleBSDFLobes(firstVertex.surface, float3(0.0f, 0.0f, 0.0f),
            diffuseShare, specularStrategy);
        state = PATH_TRACE_PSR_FALLBACK_DIRECTION;

        if(mirrorSample.valid && mirrorSample.delta)
        {
            ray.Origin = PathTraceOffsetRayOrigin(firstVertex.position, firstVertex.surface.geometricNormal);
            ray.Direction = mirrorSample.direction;
            ray.TMin = 0.0f;
            ray.TMax = PATH_TRACE_DISTANT_LIGHT_DISTANCE;
            TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, PATH_TRACE_MISS_RADIANCE, ray, guidePayload);
            payload.hitDistance = guidePayload.hitDistance;
            payload.geometricNormal = guidePayload.geometricNormal;
            payload.shadingNormal = guidePayload.shadingNormal;
            payload.uv = guidePayload.uv;
            payload.materialIndex = guidePayload.materialIndex;

            PathTraceSurface replacement;

            if(payload.hitDistance < 0.0f)
            {
                state = PATH_TRACE_PSR_SKY;
            }
            else if(!PathTraceBuildSurface(payload, ray.Direction, replacement))
            {
                state = PATH_TRACE_PSR_FALLBACK_SURFACE;
            }
            else if(payload.materialIndex > PATH_TRACE_PSR_MAXIMUM_MATERIAL_INDEX)
            {
                state = PATH_TRACE_PSR_FALLBACK_SURFACE;
            }
            else
            {
                state = replacement.deltaSpecular ? PATH_TRACE_PSR_FALLBACK_MIRROR : PATH_TRACE_PSR_SURFACE;
            }
        }
    }

    if(!PathTracePsrReplaced(state))
    {
        PathTracePsrStoreState(pixel, state);
        PathTracePsrStoreGuideState(pixel, PATH_TRACE_PSR_GUIDE_NONE);
        return;
    }

    PathTracePsrStoreRecord(pixel, state, payload);

    // The mirror plane: the raster point before PATH_TRACE_PRIMARY_SURFACE_LIFT, and its viewer-facing geometric normal.
    float3 planeNormal = firstVertex.surface.geometricNormal;
    // The lift is along n, so the lifted point's camera distance differs from the raster point's only to second order.
    float lift = PATH_TRACE_PRIMARY_SURFACE_LIFT * length(firstVertex.position - cameraPosition);
    float3 planePoint = firstVertex.position - planeNormal * lift;
    Texture2D<uint2> identityTexture = ResourceDescriptorHeap[psrIdentityIndex];
    uint mirrorIdentity = PsrIdentityInstance(identityTexture.Load(int3(pixel, 0)).x);
    uint mirrorInstance = mirrorIdentity - 1u;
    bool mirrorStatic = mirrorIdentity != 0u && mirrorIdentity != 0xFFFFFFFFu && mirrorInstance < psrTransformCount &&
        PathTracePsrIsIdentity(psrTransforms[mirrorInstance]);

    if(state == PATH_TRACE_PSR_SKY)
    {
        // The virtual image is the camera direction at infinity; with a static mirror its motion is that of the
        // direction under the camera's rotation.
        float3 virtualDirection = -firstVertex.surface.view;
        float2 currentUV;
        float2 previousUV;
        uint skyState = PATH_TRACE_PSR_GUIDE_SKY;
        float3 skyMotion = float3(0.0f, 0.0f, 0.0f);

        if(psrHistoryValid == 0u)
        {
            skyState = PATH_TRACE_PSR_GUIDE_HISTORY_INVALID;
        }
        else if(!mirrorStatic)
        {
            skyState = PATH_TRACE_PSR_GUIDE_MIRROR_MOVED;
        }
        else if(!PathTracePsrProject(psrViewProjection, float4(virtualDirection, 0.0f), currentUV) ||
            !PathTracePsrProject(psrPreviousViewProjection, float4(virtualDirection, 0.0f), previousUV))
        {
            skyState = PATH_TRACE_PSR_GUIDE_PROJECTION_INVALID;
        }
        else
        {
            skyMotion = float3(currentUV - previousUV, 0.0f);
        }

        PathTracePsrStoreGuide(pixel, 0xFFFFFFFFu, 0xFFFFFFFFu, mirrorIdentity, skyState, 0.0f, skyMotion,
            -virtualDirection);
        return;
    }

    float3 replacementPoint = ray.Origin + ray.Direction * payload.hitDistance;
    float3 virtualPoint = PathTracePsrReflectPoint(replacementPoint, planePoint, planeNormal);
    float3 replacementNormal = dot(payload.shadingNormal, ray.Direction) > 0.0f ? -payload.shadingNormal :
        payload.shadingNormal;
    float3 virtualNormal = PathTracePsrReflectDirection(replacementNormal, planeNormal);
    float viewZ = dot(psrViewDepthRow, float4(virtualPoint, 1.0f));
    uint guideState = PATH_TRACE_PSR_GUIDE_VALID;
    float3 motion = float3(0.0f, 0.0f, 0.0f);
    uint replacementInstance = guidePayload.instanceID;

    if(psrHistoryValid == 0u)
    {
        guideState = PATH_TRACE_PSR_GUIDE_HISTORY_INVALID;
    }
    else if(replacementInstance >= psrTransformCount || psrTransforms[replacementInstance].valid == 0u)
    {
        guideState = PATH_TRACE_PSR_GUIDE_TRANSFORM_INVALID;
    }
    else if(!mirrorStatic)
    {
        guideState = PATH_TRACE_PSR_GUIDE_MIRROR_MOVED;
    }
    else
    {
        float3 previousPoint = PathTracePsrTransformPoint(psrTransforms[replacementInstance], replacementPoint);
        float3 previousVirtualPoint = PathTracePsrReflectPoint(previousPoint, planePoint, planeNormal);
        float2 currentUV;
        float2 previousUV;

        if(!PathTracePsrProject(psrViewProjection, float4(virtualPoint, 1.0f), currentUV) ||
            !PathTracePsrProject(psrPreviousViewProjection, float4(previousVirtualPoint, 1.0f), previousUV))
        {
            guideState = PATH_TRACE_PSR_GUIDE_PROJECTION_INVALID;
        }
        else
        {
            motion = float3(currentUV - previousUV,
                dot(psrPreviousViewDepthRow, float4(previousVirtualPoint, 1.0f)) - viewZ);
        }
    }

    PathTracePsrStoreGuide(pixel, replacementInstance, guidePayload.primitiveIndex, mirrorIdentity, guideState, viewZ,
        motion, virtualNormal);
}

#endif // PSR.1 guide b3 is not part of the combined PSR.2 library.

// PSR.2 Stage 3A verification probe (development; dispatched only by a test hook after the split pair): per pixel, what
// the split passes see through the effective primary surface interface, with this library's storage (PSR.1's records
// or PSR.2's G-buffer). 48 words: flags (1 vertex 0 is a surface, 2 SURFACE, 4 B shaded, 8 delta sample valid), the
// vertex-0 position, shading normal, geometric normal, base colour, delta weight and direction, then B's position,
// shading normal, geometric normal, view, base colour, roughness, metallic, specular probability, normal dot view, and
// the vertex-0 view. The raw UAV is the library's spare constant word.
#ifdef PATH_TRACE_PSR_GBUFFER
#define PATH_TRACE_PSR_PROBE_INDEX psrGBufferPadding.x
#else
#define PATH_TRACE_PSR_PROBE_INDEX psrPadding
#endif

void PathTracePsrProbe(uint2 pixel)
{

    if(any(pixel >= resolution))
    {
        return;
    }

    PathTraceRasterFirstVertex firstVertex = PathTraceDecodeRasterFirstVertex(pixel);
    PathTraceResolveEffectivePrimary(pixel, firstVertex);
    float3 diffuseShare;
    bool specularStrategy;
    PathTraceBSDFSample mirrorSample = (PathTraceBSDFSample)0;

    if(firstVertex.state > 0)
    {
        mirrorSample = PathTraceSampleBSDFLobes(firstVertex.surface, float3(0.0f, 0.0f, 0.0f), diffuseShare,
            specularStrategy);
    }

    float3 position = float3(0.0f, 0.0f, 0.0f);
    PathTraceSurface surface = (PathTraceSurface)0;
    bool shaded = firstVertex.state > 0 && PathTraceEffectivePrimaryShadingVertex(pixel, firstVertex, position, surface);
    uint flags = (firstVertex.state > 0 ? 1u : 0u) | (firstVertex.psrState == PATH_TRACE_PSR_SURFACE ? 2u : 0u) |
        (shaded ? 4u : 0u) | (mirrorSample.valid && mirrorSample.delta ? 8u : 0u);

    RWByteAddressBuffer probe = ResourceDescriptorHeap[PATH_TRACE_PSR_PROBE_INDEX];
    uint address = (pixel.y * resolution.x + pixel.x) * 192u;
    probe.Store4(address, uint4(flags, asuint(firstVertex.position)));
    probe.Store4(address + 16u, uint4(asuint(firstVertex.surface.normal), asuint(firstVertex.surface.geometricNormal.x)));
    probe.Store4(address + 32u, uint4(asuint(firstVertex.surface.geometricNormal.yz), asuint(firstVertex.surface.baseColor.xy)));
    probe.Store4(address + 48u, uint4(asuint(firstVertex.surface.baseColor.z), asuint(mirrorSample.weight)));
    probe.Store4(address + 64u, uint4(asuint(mirrorSample.direction), asuint(position.x)));
    probe.Store4(address + 80u, uint4(asuint(position.yz), asuint(surface.normal.xy)));
    probe.Store4(address + 96u, uint4(asuint(surface.normal.z), asuint(surface.geometricNormal)));
    probe.Store4(address + 112u, uint4(asuint(surface.view), asuint(surface.baseColor.x)));
    probe.Store4(address + 128u, uint4(asuint(surface.baseColor.yz), asuint(surface.material.roughness),
        asuint(surface.material.metallic)));
    probe.Store4(address + 144u, uint4(asuint(surface.specularProbability), asuint(surface.normalDotView),
        asuint(firstVertex.surface.view.xy)));
    probe.Store4(address + 160u, uint4(asuint(firstVertex.surface.view.z), 0u, 0u, 0u));
}

[shader("raygeneration")]
void PathTracePsrProbeRayGeneration()
{
    PathTracePsrProbe(DispatchRaysIndex().xy);
}
#endif

#ifdef PATH_TRACE_RESTIR_DI_LIBRARY
// RTXPT.3 ReSTIR DI direct pass (DirectLighting/ only): the RTXDI work of vertex 0 for every sample of this
// dispatch, initial sampling, sample 0's spatiotemporal reuse, visibility and reservoir publication, with no path
// continuation. Each sample's estimate is stored for the indirect pass; samples whose path never reaches vertex-0
// direct lighting store zero, and the indirect pass never reads them. Every active pixel writes its reservoir each
// dispatch; a pixel whose sample 0 ran no RTXDI sampling (background, unusable sample, pure delta mirror) writes
// an empty one. Only rays are counted here; the indirect pass counts samples. extract (a literal) also stores each
// sample's diffuse-lobe part, at the same address in the splitDirectDiffuseIndex buffer (RTXPT D.1B.2).
#ifdef PATH_TRACE_DIRECT_COMPUTE
// RayQuery 2M E3: the compute direct pass's pixel, its dispatch thread (PathTraceReSTIRDIDirectCS).
static uint2 g_pathTraceDirectComputePixel = uint2(0u, 0u);
#endif

void PathTraceRunReSTIRDISplitDirect(bool extract)
{
    uint2 pixel = g_pathTraceDirectComputePixel;
    bool active = all(pixel < resolution);
    uint rayCount = 0u;

    if(active)
    {
        PathTraceRasterFirstVertex rasterFirstVertex = PathTraceDecodeRasterFirstVertex(pixel);

        // The conditions under which PathTraceTracePath calls PathTraceRtxdiDirectLighting at vertex 0: a G-buffer
        // surface, a path that continues past vertex 0, and a non-delta lobe.
        bool hasNonDeltaLobe = !rasterFirstVertex.surface.deltaSpecular ||
            rasterFirstVertex.surface.specularProbability < 1.0f;
        bool directLighting = rasterFirstVertex.state > 0 && maxPathDepth > 0u && hasNonDeltaLobe;
#ifdef PATH_TRACE_PSR_LIBRARY
        // PSR.1c-A: a SURFACE pixel (a pure-delta mirror; the production pass computes nothing for it) is lit at its
        // replacement surface, the path loop's vertex 1, which is shaded only below maxPathDepth. Reuse is disabled for
        // it: sample 0 never reuses history (the fused pass would also gather spatial neighbors) and the pixel publishes
        // the empty reservoir the production pass publishes for a pure mirror, so no other pixel's history changes.
        // An ordinary lit vertex is never eligible (a pure-delta mirror has no non-delta lobe), so it reads no record.
        PathTraceResolveEffectivePrimary(pixel, rasterFirstVertex);
        float3 directPosition;
        PathTraceSurface directSurface;
        bool replacedSurface = PathTraceEffectivePrimaryShadingVertex(pixel, rasterFirstVertex, directPosition,
            directSurface);
        directLighting = directLighting || replacedSurface;
#if PATH_TRACE_PSR_CHAIN >= 2
        // Multibounce PSR: a two-mirror chain this pass rejected publishes the sentinel, so the indirect pass takes the
        // ordinary path (and its own next-event estimation) for the pixel.
#ifdef PATH_TRACE_PSR_CHAIN_FORCE_REJECT
        // Verification only (tests/PsrK2Reject/): the direct pass also rejects every chain pixel with (x + 2y) % 6 == 3.
        const bool chainRejected = PathTraceEffectivePrimaryVertex(rasterFirstVertex) == 2u &&
            (!replacedSurface || (pixel.x + 2u * pixel.y) % 6u == 3u);
#else
        const bool chainRejected = PathTraceEffectivePrimaryVertex(rasterFirstVertex) == 2u && !replacedSurface;
#endif
#endif
#endif

        g_rtxdiPixel = pixel;
        g_rtxdiReservoirStored = false;
        RWByteAddressBuffer directRadiance = ResourceDescriptorHeap[splitDirectRadianceIndex];

        for(uint sampleOffset = 0u; sampleOffset < samplesPerFrame; ++sampleOffset)
        {
            g_rtxdiSampleSeed = PathTraceHash((accumulatedSamples + sampleOffset) ^ PathTraceHash(randomSeed ^
                0x52545844u));
            g_rtxdiStoreReservoir = sampleOffset == 0u;
#ifdef PATH_TRACE_PSR_LIBRARY
            g_rtxdiStoreReservoir = g_rtxdiStoreReservoir && !replacedSurface;
#endif
            float3 sampleDirect = float3(0.0f, 0.0f, 0.0f);
            float3 sampleDirectDiffuse = float3(0.0f, 0.0f, 0.0f);

            if(directLighting)
            {
                // RTXDI draws from its own sequence; the path randoms it discards are the indirect pass's.
                PathTraceRandom discardedRandom = (PathTraceRandom)0;
#ifdef PATH_TRACE_PSR_LIBRARY
                sampleDirect = PathTraceRtxdiDirectLighting(directSurface, directPosition, discardedRandom, rayCount,
                    sampleDirectDiffuse);
#else
                sampleDirect = PathTraceRtxdiDirectLighting(rasterFirstVertex.surface, rasterFirstVertex.position,
                    discardedRandom, rayCount, sampleDirectDiffuse);
#endif
            }

#if PATH_TRACE_PSR_CHAIN >= 2
            if(chainRejected)
            {
                sampleDirect = float3(PATH_TRACE_PSR_CHAIN_REJECTED, 0.0f, 0.0f);
            }
#endif
            directRadiance.Store3(PathTraceSplitDirectRadianceAddress(pixel, sampleOffset), asuint(sampleDirect));

            if(extract)
            {
                RWByteAddressBuffer directDiffuse = ResourceDescriptorHeap[splitDirectDiffuseIndex];
                directDiffuse.Store3(PathTraceSplitDirectRadianceAddress(pixel, sampleOffset), asuint(sampleDirectDiffuse));
            }
        }

        if(!g_rtxdiReservoirStored)
        {
            RTXDI_StoreDIReservoir(RTXDI_EmptyDIReservoir(), rtxdiReservoirBuffer, pixel,
                rtxdiBufferIndices.shadingInputBufferIndex);
        }
    }

    uint waveRays = WaveActiveSum(rayCount);

    if(WaveIsFirstLane())
    {
        PathTraceAddStatistic64(0u, waveRays);
    }
}

// The extraction wrappers (DirectLighting/*Extract.hlsl) define PATH_TRACE_RESTIR_DI_EXTRACTION_LIBRARY.
#if defined(PATH_TRACE_DIRECT_COMPUTE)
// RayQuery 2M E3: the production direct pass, compiled as a compute shader (DirectLighting/, cs_6_6) outside any DXR
// state object: a DXR direct pass's visibility depended on the library it was linked from (RayQuery 2C-2M). One thread
// per pixel in PATH_TRACE_DIRECT_COMPUTE_GROUP_SIZE squares (RayTracingPipelineDescription::computeThreadGroupSize).
static const uint PATH_TRACE_DIRECT_COMPUTE_GROUP_SIZE = 8u;

[numthreads(PATH_TRACE_DIRECT_COMPUTE_GROUP_SIZE, PATH_TRACE_DIRECT_COMPUTE_GROUP_SIZE, 1)]
void PathTraceReSTIRDIDirectCS(uint3 thread : SV_DispatchThreadID)
{
    g_pathTraceDirectComputePixel = thread.xy;
#ifdef PATH_TRACE_RESTIR_DI_EXTRACTION_LIBRARY
    PathTraceRunReSTIRDISplitDirect(true);
#else
    PathTraceRunReSTIRDISplitDirect(false);
#endif
}
#endif
#endif

#ifndef PATH_TRACE_RESTIR_DI_LIBRARY
// RTXPT.3 ReSTIR DI indirect pass (PathTraceReference.hlsl only, so its compiled code contains no RTXDI sampling or
// reuse): the ReSTIR DI path after the direct pass. It accumulates and counts samples.
[shader("raygeneration")]
void PathTraceReSTIRDISplitIndirectRayGeneration()
{
    PathTraceRunRayGeneration(PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT, false);
}

// RTXPT D.1B.2 signal extraction: the ReSTIR DI indirect pass and the raster primary, each also writing the
// diffuse and specular outputs. Reference library only, like the indirect pass.
[shader("raygeneration")]
void PathTraceReSTIRDISplitIndirectExtractRayGeneration()
{
    PathTraceRunRayGeneration(PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT, true);
}

[shader("raygeneration")]
void PathTraceRasterPrimaryExtractRayGeneration()
{
    PathTraceRunRayGeneration(PATH_TRACE_PRIMARY_RASTER, true);
}
#endif

// RTXPT.2 diagnostic: unjittered camera rays, for raster-primary agreement measurements.
[shader("raygeneration")]
void PathTracePixelCenterRayGeneration()
{
    PathTraceRunRayGeneration(PATH_TRACE_PRIMARY_PIXEL_CENTER, false);
}

[shader("miss")]
void PathTraceMiss(inout PathTracePayload payload)
{
    payload.hitDistance = -1.0f;
}

[shader("miss")]
void PathTraceVisibilityMiss(inout PathTraceVisibilityPayload payload)
{
    payload.visible = 1u;
}

float3 PathTraceLoadPosition(ByteAddressBuffer vertexBuffer, uint vertexIndex, uint vertexStride)
{
    // The BLAS reads R32G32B32 positions at vertex offset 0 (BottomLevelAccelerationStructure).
    return asfloat(vertexBuffer.Load3(vertexIndex * vertexStride));
}

float3 PathTraceLoadNormal(ByteAddressBuffer vertexBuffer, uint vertexIndex, RayTracingInstanceData instance)
{
    return asfloat(vertexBuffer.Load3(vertexIndex * instance.vertexStride + instance.normalOffset));
}

float2 PathTraceLoadUV(ByteAddressBuffer vertexBuffer, uint vertexIndex, RayTracingInstanceData instance)
{
    return asfloat(vertexBuffer.Load2(vertexIndex * instance.vertexStride + instance.uvOffset));
}

float4 PathTraceLoadTangent(ByteAddressBuffer vertexBuffer, uint vertexIndex, RayTracingInstanceData instance)
{
    return asfloat(vertexBuffer.Load4(vertexIndex * instance.vertexStride + instance.tangentOffset));
}

uint3 PathTraceTriangleIndices(RayTracingInstanceData instance, uint primitiveIndex)
{
    if(instance.indexed == 0u)
    {
        return uint3(primitiveIndex * 3u, primitiveIndex * 3u + 1u, primitiveIndex * 3u + 2u);
    }

    ByteAddressBuffer indexBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(instance.indexBufferIndex)];
    return indexBuffer.Load3(primitiveIndex * 3u * 4u);
}

[shader("closesthit")]
void PathTraceClosestHit(inout PathTracePayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    // The evaluation is shared with the compute direct pass's inline radiance ray (PathTraceClosestHitBody.hlsli).
#define PATH_TRACE_HIT_INSTANCE_ID InstanceID()
#define PATH_TRACE_HIT_PRIMITIVE_INDEX PrimitiveIndex()
#define PATH_TRACE_HIT_BARYCENTRICS attributes.barycentrics
#define PATH_TRACE_HIT_WORLD_TO_OBJECT WorldToObject3x4()
#define PATH_TRACE_HIT_OBJECT_TO_WORLD ObjectToWorld3x4()
#define PATH_TRACE_HIT_WORLD_RAY_DIRECTION WorldRayDirection()
#define PATH_TRACE_HIT_T RayTCurrent()
#include "PathTraceClosestHitBody.hlsli"
#undef PATH_TRACE_HIT_INSTANCE_ID
#undef PATH_TRACE_HIT_PRIMITIVE_INDEX
#undef PATH_TRACE_HIT_BARYCENTRICS
#undef PATH_TRACE_HIT_WORLD_TO_OBJECT
#undef PATH_TRACE_HIT_OBJECT_TO_WORLD
#undef PATH_TRACE_HIT_WORLD_RAY_DIRECTION
#undef PATH_TRACE_HIT_T
}

#ifdef PATH_TRACE_DIRECT_COMPUTE
// RayQuery 2M E3: a radiance ray exactly as the DXR direct pipelines trace it (TraceRay with PATH_TRACE_MISS_RADIANCE),
// for the compute direct pass, whose only radiance ray is the two-bounce PSR chain retrace. Equivalence relies on the
// geometry contract of the direct pipelines (RayQuery_Compute_Direct_Pass_Production_Integration_2M.md):
// - one hit group, PathTraceHitGroup (PathTraceClosestHit, no any-hit shader), and hit-group contribution 0 on every TLAS
//   instance, so every hit runs PathTraceClosestHit: the shared PathTraceClosestHitBody.hlsli below;
// - triangles only: no procedural geometry. Without an any-hit shader DXR accepts every non-opaque candidate, so each is
//   committed (Engine's geometry is opaque, so none occurs); without ACCEPT_FIRST_HIT the committed hit is the closest;
// - a miss runs PathTraceMiss, which sets only hitDistance.
// An any-hit (alpha-tested) path, a second hit group or procedural geometry needs matching handling here.
void PathTraceTraceRadianceInline(RayDesc ray, uint instanceMask, inout PathTracePayload payload)
{
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, instanceMask, ray);

    while(query.Proceed())
    {
        if(query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            query.CommitNonOpaqueTriangleHit();
        }
    }

    if(query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
#define PATH_TRACE_HIT_INSTANCE_ID query.CommittedInstanceID()
#define PATH_TRACE_HIT_PRIMITIVE_INDEX query.CommittedPrimitiveIndex()
#define PATH_TRACE_HIT_BARYCENTRICS query.CommittedTriangleBarycentrics()
#define PATH_TRACE_HIT_WORLD_TO_OBJECT query.CommittedWorldToObject3x4()
#define PATH_TRACE_HIT_OBJECT_TO_WORLD query.CommittedObjectToWorld3x4()
#define PATH_TRACE_HIT_WORLD_RAY_DIRECTION query.WorldRayDirection()
#define PATH_TRACE_HIT_T query.CommittedRayT()
#include "PathTraceClosestHitBody.hlsli"
#undef PATH_TRACE_HIT_INSTANCE_ID
#undef PATH_TRACE_HIT_PRIMITIVE_INDEX
#undef PATH_TRACE_HIT_BARYCENTRICS
#undef PATH_TRACE_HIT_WORLD_TO_OBJECT
#undef PATH_TRACE_HIT_OBJECT_TO_WORLD
#undef PATH_TRACE_HIT_WORLD_RAY_DIRECTION
#undef PATH_TRACE_HIT_T
    }
    else
    {
        payload.hitDistance = -1.0f;
    }
}
#endif

#ifdef PATH_TRACE_PSR_LIBRARY
// PSR.1b guide hit group: PathTraceClosestHit plus the hit's TLAS instance and primitive. The evaluation is a verbatim
// copy (factoring it into a shared function changed every production library's DXIL); it must stay identical, which
// the PSR bit-identity gates check, because the guide pass writes the records the raster primary consumes.
[shader("closesthit")]
void PathTracePsrGuideClosestHit(inout PathTracePsrGuidePayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    StructuredBuffer<RayTracingInstanceData> instances = ResourceDescriptorHeap[instanceDataIndex];
    RayTracingInstanceData instance = instances[InstanceID()];
    instance.materialIndex = ResolveHitMaterial(instance, PrimitiveIndex());

    ByteAddressBuffer vertexBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(instance.vertexBufferIndex)];
    uint3 indices = PathTraceTriangleIndices(instance, PrimitiveIndex());
    float3 barycentrics = float3(1.0f - attributes.barycentrics.x - attributes.barycentrics.y,
        attributes.barycentrics.x, attributes.barycentrics.y);

    // Normals use the inverse transpose of object-to-world (row vector times world-to-object).
    float3x3 normalTransform = (float3x3)WorldToObject3x4();
    float3 position0 = PathTraceLoadPosition(vertexBuffer, indices.x, instance.vertexStride);
    float3 position1 = PathTraceLoadPosition(vertexBuffer, indices.y, instance.vertexStride);
    float3 position2 = PathTraceLoadPosition(vertexBuffer, indices.z, instance.vertexStride);
    float3 objectGeometricNormal = cross(position1 - position0, position2 - position0);
    float3 geometricNormal = mul(objectGeometricNormal, normalTransform);
    float geometricLengthSquared = dot(geometricNormal, geometricNormal);

    float3 objectNormal = PathTraceLoadNormal(vertexBuffer, indices.x, instance) * barycentrics.x +
        PathTraceLoadNormal(vertexBuffer, indices.y, instance) * barycentrics.y +
        PathTraceLoadNormal(vertexBuffer, indices.z, instance) * barycentrics.z;
    float3 worldNormal = mul(objectNormal, normalTransform);
    float worldNormalLengthSquared = dot(worldNormal, worldNormal);

    float2 uv = PathTraceLoadUV(vertexBuffer, indices.x, instance) * barycentrics.x +
        PathTraceLoadUV(vertexBuffer, indices.y, instance) * barycentrics.y +
        PathTraceLoadUV(vertexBuffer, indices.z, instance) * barycentrics.z;

    if(!(geometricLengthSquared > 1.0e-30f) || !isfinite(geometricLengthSquared))
    {
        geometricNormal = -WorldRayDirection();
    }
    else
    {
        geometricNormal *= rsqrt(geometricLengthSquared);
    }

    if(!(worldNormalLengthSquared > 1.0e-16f) || !isfinite(worldNormalLengthSquared))
    {
        worldNormal = geometricNormal;
    }
    else
    {
        worldNormal *= rsqrt(worldNormalLengthSquared);
    }

    // Interpolated normals keep the triangle's orientation so two-sided flipping stays consistent.
    if(dot(worldNormal, geometricNormal) < 0.0f)
    {
        geometricNormal = -geometricNormal;
    }

    float3 shadingNormal = worldNormal;
    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[materialBufferIndex];
    uint materialCount;
    uint materialStride;
    materials.GetDimensions(materialCount, materialStride);

    // Tangent-space normal mapping, identical to the RTGI/G-buffer convention.
    if(instance.materialIndex < materialCount && instance.tangentOffset != 0xFFFFFFFFu &&
        materials[instance.materialIndex].normalTextureIndex != 0xFFFFFFFFu)
    {
        GPUMaterial material = materials[instance.materialIndex];
        float4 objectTangent = PathTraceLoadTangent(vertexBuffer, indices.x, instance) * barycentrics.x +
            PathTraceLoadTangent(vertexBuffer, indices.y, instance) * barycentrics.y +
            PathTraceLoadTangent(vertexBuffer, indices.z, instance) * barycentrics.z;
        float3 tangent = mul((float3x3)ObjectToWorld3x4(), objectTangent.xyz);
        tangent -= worldNormal * dot(worldNormal, tangent);
        float tangentLengthSquared = dot(tangent, tangent);
        float instanceDeterminant = determinant((float3x3)ObjectToWorld3x4());
        float handedness = objectTangent.w * (instanceDeterminant < 0.0f ? -1.0f : 1.0f);

        Texture2D<float4> normalTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.normalTextureIndex)];
        float2 normalXY = normalTexture.SampleLevel(materialSampler, uv, 0.0f).rg * 2.0f - 1.0f;
        float3 sampledNormal = float3(normalXY, sqrt(saturate(1.0f - dot(normalXY, normalXY))));
        float sampledLengthSquared = dot(sampledNormal, sampledNormal);

        bool frameValid = all(isfinite(tangent)) && isfinite(tangentLengthSquared) && tangentLengthSquared > 1.0e-8f &&
            all(isfinite(sampledNormal)) && sampledLengthSquared > 1.0e-8f && isfinite(handedness) &&
            abs(handedness) > 1.0e-8f && isfinite(instanceDeterminant) && instanceDeterminant != 0.0f;

        if(frameValid)
        {
            sampledNormal = normalize(sampledNormal);
            float3 tangentDirection = normalize(tangent);
            float3 bitangent = cross(worldNormal, tangentDirection) * handedness;
            float3 mappedNormal = tangentDirection * sampledNormal.x + bitangent * sampledNormal.y +
                worldNormal * sampledNormal.z;
            float mappedLengthSquared = dot(mappedNormal, mappedNormal);

            if(all(isfinite(mappedNormal)) && isfinite(mappedLengthSquared) && mappedLengthSquared > 1.0e-8f)
            {
                shadingNormal = mappedNormal * rsqrt(mappedLengthSquared);
            }
        }
    }

    payload.hitDistance = RayTCurrent();
    payload.geometricNormal = geometricNormal;
    payload.shadingNormal = shadingNormal;
    payload.uv = uv;
    payload.materialIndex = instance.materialIndex;
    payload.instanceID = InstanceID();
    payload.primitiveIndex = PrimitiveIndex();
}

[shader("miss")]
void PathTracePsrGuideMiss(inout PathTracePsrGuidePayload payload)
{
    payload.hitDistance = -1.0f;
}
#endif
