// RTXPT.1 progressive Monte Carlo reference path tracer (experimental, opt-in).
//
// One raygen thread per pixel runs an iterative path loop: camera ray, BSDF continuation and next-event
// estimation at every non-delta vertex. Environment radiance arrives only along traced rays that miss
// all geometry, so an opaque sealed room receives exactly zero environment light. No prefiltered IBL,
// irradiance cube, AO or history from the legacy renderer is read.
//
// Accumulation: accumulationTexture stores the running mean in RGBA32F. With n previous samples and s new
// samples, mean' = mean * n / (n + s) + sum / (n + s). n = 0 ignores previous contents entirely.

#include "PathTraceBSDF.hlsli"
#include "PathTraceLights.hlsli"
#include "PathTracePrimarySurface.hlsli"
#include "RayTracingInstanceData.hlsli"

// RTXPT.3: PathTraceReSTIRDI.hlsl compiles this file with PATH_TRACE_RESTIR_DI_LIBRARY defined, for the ReSTIR DI
// direct pass. Every RTXDI addition below is inside that specialization, so this library (the reference,
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
    // RTXPT.3 ReSTIR DI: the raw UAV of the per-sample vertex-0 direct radiance, written by
    // PathTraceReSTIRDISplitDirectRayGeneration and read by PathTraceReSTIRDISplitIndirectRayGeneration.
    uint splitDirectRadianceIndex;

#ifdef PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY
    // NRD.RELAX.2, appended: the raw UAV of the R32F raw primary diffuse hit distance, written only by the extraction ray
    // generations; PATH_TRACE_NO_RAW_DIFFUSE_HIT_DISTANCE (the default) writes nothing. NRD.RELAX.2B: declared only in the
    // raw-distance libraries (PathTraceReference*RawDiffuseHitDistance*.hlsl); every other library keeps the original
    // layout, which the trailing bytes of PathTraceReferenceConstants extend.
    uint rawDiffuseHitDistanceIndex;
    uint3 rawDiffuseHitDistancePadding;
#else
    uint4 sampleDiagnosticReservedRawDistance;
#endif
    uint sampleDiagnosticIndex;
    uint2 sampleDiagnosticPixel;
    float sampleDiagnosticThreshold;
    uint sampleDiagnosticFrame;
    uint sampleDiagnosticTarget;
    // R.9I.1D automatic capture bucket buffer (UINT_MAX disables) and its detection threshold.
    uint sampleAutoIndex;
    float sampleAutoThreshold;
};

cbuffer SceneLightConstants : register(b1)
{
    uint sceneLightCount;
    uint3 sceneLightPadding;
    GPULight sceneLights[SCENE_LIGHT_CAPACITY];
};

#ifdef PATH_TRACE_RESTIR_DI_LIBRARY
// RTXDI light records, two per register: RtxdiLightSet::Capacity / 2.
#define RTXDI_LIGHT_RECORD_REGISTERS 33

// RTXPT.3, read only by PathTraceReSTIRDISplitDirectRayGeneration, whose pipeline alone binds b2.
// Identical layout to PathTraceRtxdiConstants (RtxdiShaderData.h).
cbuffer RtxdiConstants : register(b2)
{
    RTXDI_LightBufferParameters rtxdiLightBuffer;
    RTXDI_ReservoirBufferParameters rtxdiReservoirBuffer;
    RTXDI_DIBufferIndices rtxdiBufferIndices;

    uint rtxdiReservoirBufferIndex;
    uint rtxdiLocalLightSamples;
    uint rtxdiInfiniteLightSamples;
    uint rtxdiPadding;

    uint4 rtxdiLightRecords[RTXDI_LIGHT_RECORD_REGISTERS];

    // R3.5/R3.6 fused spatiotemporal reuse.
    RTXDI_RuntimeParameters rtxdiRuntime;
    RTXDI_DISpatioTemporalResamplingParameters rtxdiSpatioTemporal;

    // 1 when previous reservoirs, previous surfaces and the translation below may be read.
    uint rtxdiHistoryValid;
    uint rtxdiNeighborOffsetsIndex;
    uint rtxdiMotionIndex;
    uint rtxdiCurrentLightCount;

    uint rtxdiPreviousLightCount;
    uint rtxdiPreviousDepthIndex;
    uint rtxdiPreviousSurfaceIndex;
    uint rtxdiPreviousGeometricNormalIndex;

    uint rtxdiPreviousMaterialIndex;
    uint3 rtxdiPadding2;

    float3 rtxdiPreviousCameraPosition;
    float rtxdiPadding3;
    float3 rtxdiCameraForward;
    float rtxdiPadding4;
    float3 rtxdiPreviousCameraForward;
    float rtxdiPadding5;

    row_major float4x4 rtxdiPreviousInverseCameraRelativeViewProjection;

    // Current-to-previous entries (rtxdiCurrentLightCount), then previous-to-current (rtxdiPreviousLightCount).
    int4 rtxdiLightTranslation[RTXDI_LIGHT_RECORD_REGISTERS];
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
#include "PathTraceSampleDiagnostic.hlsli"

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

RtxdiLightRecord RtxdiBridgeLoadLightRecord(uint lightIndex)
{
    uint4 pair = rtxdiLightRecords[lightIndex >> 1u];
    bool second = (lightIndex & 1u) != 0u;

    RtxdiLightRecord record;
    record.kind = second ? pair.z : pair.x;
    record.sourceIndex = second ? pair.w : pair.y;
    return record;
}

GPULight RtxdiBridgeLoadTableLight(uint sourceIndex)
{
    return sceneLights[sourceIndex];
}

PathTraceSun RtxdiBridgeSun()
{
    return PathTraceFrameSun();
}

PathTraceEnvironment RtxdiBridgeEnvironment()
{
    return PathTraceFrameEnvironment();
}

RWStructuredBuffer<RTXDI_PackedDIReservoir> RtxdiReservoirs()
{
    return ResourceDescriptorHeap[rtxdiReservoirBufferIndex];
}

// R3.2 strict translation: -1 when a light has no counterpart whose whole identity is unchanged.
int RtxdiBridgeTranslateLightIndex(int lightIndex, bool currentToPrevious)
{
    uint count = currentToPrevious ? rtxdiCurrentLightCount : rtxdiPreviousLightCount;

    if(rtxdiHistoryValid == 0u || lightIndex < 0 || uint(lightIndex) >= count)
    {
        return -1;
    }

    uint entry = currentToPrevious ? uint(lightIndex) : rtxdiCurrentLightCount + uint(lightIndex);
    return rtxdiLightTranslation[entry >> 2u][entry & 3u];
}

#define RTXDI_BRIDGE_ENVIRONMENT_SAMPLER environmentSampler
#define RTXDI_LIGHT_RESERVOIR_BUFFER RtxdiReservoirs()
#define RTXDI_BRIDGE_PREVIOUS_TO_CURRENT_LIGHT(index) RtxdiBridgeTranslateLightIndex(int(index), false)
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
    (defined(PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY) && !defined(PATH_TRACE_RESTIR_DI_LIBRARY) && !defined(PATH_TRACE_RADIANCE_CACHE_TRAINING))
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
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

float3 PathTraceMissRadiance(float3 direction, bool previousDelta, float previousPdf)
{
    PathTraceEnvironment environment = PathTraceFrameEnvironment();
    PathTraceSun sun = PathTraceFrameSun();

    float3 environmentRadiance = PathTraceEnvironmentRadiance(environment, environmentSampler, direction);
    float3 sunRadiance = PathTraceSunRadiance(sun, direction);

    if(previousDelta)
    {
        // Camera rays and delta-mirror continuations: no light-sampling strategy could produce them.
        return environmentRadiance + sunRadiance;
    }

    float environmentWeight = PathTraceEnvironmentSamplingEnabled(environment) ?
        PathTracePowerHeuristic(previousPdf, PathTraceEnvironmentPdf(environment, direction)) : 1.0f;
    float sunWeight = PathTracePowerHeuristic(previousPdf, PathTraceSunPdf(sun, direction));
    return environmentRadiance * environmentWeight + sunRadiance * sunWeight;
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

    if(g_sampleDiagnostic) PathTraceDiagnosticLight(surface, lightSample, value, diffuseValue, cosine, weight);
    diffuseRadiance = diffuseValue * cosine * lightSample.radiance * weight;
    return value * weight;
}

float3 PathTraceShadeLightSample(PathTraceSurface surface, float3 position, PathTraceLightSample lightSample,
    inout uint rayCount)
{
    float3 diffuseRadiance;
    return PathTraceShadeLightSampleLobes(surface, position, lightSample, rayCount, diffuseRadiance);
}

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
        if(g_sampleDiagnostic) { g_sampleLight = 0xFFFFFFFEu; g_sampleLightType = 3u; }
        radiance += PathTraceShadeLightSampleLobes(surface, position, PathTraceSampleSun(sun, sunRandom), rayCount,
            sampleDiffuse);
        diffuseRadiance += sampleDiffuse;
    }

    uint lightCount = min(sceneLightCount, SCENE_LIGHT_CAPACITY);

    for(uint lightIndex = 0u; lightIndex < lightCount; ++lightIndex)
    {
        if(g_sampleDiagnostic) { g_sampleLight = lightIndex; g_sampleLightType = sceneLights[lightIndex].type; }
        PathTraceLightSample lightSample = PathTraceSampleTableLight(sceneLights[lightIndex], position);
        radiance += PathTraceShadeLightSampleLobes(surface, position, lightSample, rayCount, sampleDiffuse);
        diffuseRadiance += sampleDiffuse;
    }

    PathTraceEnvironment environment = PathTraceFrameEnvironment();

    if(PathTraceEnvironmentSamplingEnabled(environment))
    {
        if(g_sampleDiagnostic) { g_sampleLight = 0xFFFFFFFDu; g_sampleLightType = 4u; }
        PathTraceLightSample environmentSample = PathTraceSampleEnvironment(environment, environmentSampler,
            environmentRandom);
        radiance += PathTraceShadeLightSampleLobes(surface, position, environmentSample, rayCount, sampleDiffuse);
        diffuseRadiance += sampleDiffuse;
    }

    return radiance;
}

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
// then add no sun or HDRI radiance. The six randoms PathTraceDirectLighting would draw are drawn and discarded,
// matching PathTraceSplitDirectLighting.
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

    RAB_LightSample selectedSample;
    RTXDI_DIReservoir reservoir = RtxdiBridgeSampleInitialReservoir(bridgeSurface, g_rtxdiPixel, g_rtxdiSampleSeed,
        RtxdiBridgeInitialSamplingParameters(rtxdiLocalLightSamples, rtxdiInfiniteLightSamples), rtxdiLightBuffer,
        selectedSample);

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
    }

    float3 contribution = float3(0.0f, 0.0f, 0.0f);
    bool visible = false;

    if(RTXDI_IsValidDIReservoir(reservoir))
    {
        float weight = RTXDI_GetDIReservoirInvPdf(reservoir);
        float3 unshadowedDiffuse;
        float3 unshadowed = RtxdiBridgeSampleContributionLobes(selectedSample, bridgeSurface, unshadowedDiffuse);

        if(weight > 0.0f && isfinite(weight) && any(unshadowed > 0.0f))
        {
            visible = PathTraceVisible(position, surface.geometricNormal, selectedSample.sample.direction,
                selectedSample.sample.distance, rayCount);
            contribution = visible ? unshadowed * weight : float3(0.0f, 0.0f, 0.0f);
            diffuseContribution = visible ? unshadowedDiffuse * weight : float3(0.0f, 0.0f, 0.0f);
        }
    }

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
struct PathTraceRasterFirstVertex
{
    int state;
    float3 position;
    PathTraceSurface surface;
    float3 backgroundDirection;
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
    return firstVertex;
}

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
// SHARC.4D: learning hooks of the cache library only (PathTraceRadianceCacheHarvest.hlsli). In every other library
// PATH_TRACE_RADIANCE_CACHE_HARVEST expands to nothing, leaving an empty statement.
#ifdef PATH_TRACE_RADIANCE_CACHE_LIBRARY
#include "PathTraceRadianceCacheHarvest.hlsli"
#define PATH_TRACE_RADIANCE_CACHE_HARVEST(statement) statement
#else
#define PATH_TRACE_RADIANCE_CACHE_HARVEST(statement)
#endif

float3 PathTraceTracePath(RayDesc ray, uint primary, PathTraceRasterFirstVertex rasterFirstVertex,
    inout PathTraceRandom random, inout uint rayCount, bool extract, inout PathTraceExtraction extraction)
{
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    bool previousDelta = true;
    float previousPdf = 0.0f;
    PATH_TRACE_RADIANCE_CACHE_HARVEST(PathTraceRadianceCacheHarvest harvest = PathTraceRadianceCacheHarvestBegin());

    // vertex counts surface interactions. The ray traced after the last allowed vertex is still
    // traced so distant lights reached by BSDF sampling pair with that vertex's light samples.
    for(uint vertex = 0u; vertex <= maxPathDepth; ++vertex)
    {
        if(g_sampleDiagnostic)
        {
            g_sampleVertex = vertex;
            g_sampleThroughput = throughput;
            g_samplePreviousPdf = previousPdf;
        }
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
#ifdef PATH_TRACE_SER
            // SER S5A: the same single call site, as a HitObject. The empty vertex-0 ray keeps mask 0 and is never
            // reordered (it misses without traversal, exactly as before); continuation rays reorder.
            PathTraceTraceRadianceReordered(ray, gBufferVertex ? 0x00 : 0xFF, !gBufferVertex, payload);
#else
            TraceRay(scene, RAY_FLAG_NONE, gBufferVertex ? 0x00 : 0xFF, 0, 1, PATH_TRACE_MISS_RADIANCE, ray, payload);
#endif

            if(gBufferVertex)
            {
                if(rasterFirstVertex.state == 0)
                {
                    float3 background = PathTraceMissRadiance(rasterFirstVertex.backgroundDirection, previousDelta,
                        previousPdf);
                    radiance += throughput * background;
                    if(g_sampleDiagnostic) PathTraceDiagnosticMiss(vertex, throughput * background);
                    break;
                }

                if(rasterFirstVertex.state < 0 || vertex == maxPathDepth)
                {
                    break;
                }

                position = rasterFirstVertex.position;
                surface = rasterFirstVertex.surface;
            }
            else
            {
                if(g_sampleDiagnostic && vertex == 1u) g_sampleRecord.hit.x = payload.hitDistance;
                if(g_sampleDiagnostic) g_sampleMaterial = payload.materialIndex;
                // The camera primaries' hit handling, unchanged.
                ++rayCount;

                if(extract && vertex == 1u && extraction.hitDistance == PATH_TRACE_EXTRACTION_SPECULAR_PENDING)
                {
                    extraction.hitDistance = payload.hitDistance < 0.0f ? PATH_TRACE_HALF_MAXIMUM : payload.hitDistance;
                }

                if(extract && vertex == 1u && extraction.diffuseHitDistance == PATH_TRACE_EXTRACTION_DIFFUSE_PENDING)
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
                    bool directFromRtxdi = primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT && vertex == 1u &&
                        !previousDelta;

                    if(!directFromRtxdi)
                    {
                        float3 missRadiance = PathTraceMissRadiance(ray.Direction, previousDelta, previousPdf);
                        radiance += throughput * missRadiance;
                        if(g_sampleDiagnostic) PathTraceDiagnosticMiss(vertex, throughput * missRadiance);
                        PATH_TRACE_RADIANCE_CACHE_HARVEST(PathTraceRadianceCacheHarvestMiss(harvest, vertex, missRadiance));

                        if(extract)
                        {
                            extraction.diffuse += extraction.diffuseThroughput * missRadiance;
                        }
                    }

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
            TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, PATH_TRACE_MISS_RADIANCE, ray, payload);
#endif
            ++rayCount;
            if(g_sampleDiagnostic && vertex == 1u) g_sampleRecord.hit.x = payload.hitDistance;
            if(g_sampleDiagnostic) g_sampleMaterial = payload.materialIndex;

            if(payload.hitDistance < 0.0f)
            {
                float3 missRadiance = PathTraceMissRadiance(ray.Direction, previousDelta, previousPdf);
                radiance += throughput * missRadiance;
                if(g_sampleDiagnostic) PathTraceDiagnosticMiss(vertex, throughput * missRadiance);
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

        PATH_TRACE_RADIANCE_CACHE_HARVEST(PathTraceRadianceCacheHarvestVertex(harvest, primary, vertex, position, surface));
        bool hasNonDeltaLobe = !surface.deltaSpecular || surface.specularProbability < 1.0f;

        if(hasNonDeltaLobe)
        {
            float3 direct;
            float3 directDiffuse = float3(0.0f, 0.0f, 0.0f);

            if(primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT && vertex == 0u)
            {
                direct = PathTraceSplitDirectLighting(random);

                if(extract)
                {
                    directDiffuse = PathTraceSplitDirectDiffuse();
                }
            }
            else
            {
                direct = PathTraceDirectLightingLobes(surface, position, random, rayCount, directDiffuse);
            }

            radiance += throughput * direct;
            if(g_sampleDiagnostic) PathTraceDiagnosticDirect(throughput * direct);
            PATH_TRACE_RADIANCE_CACHE_HARVEST(PathTraceRadianceCacheHarvestDirect(harvest, vertex, direct));

            // Vertex 0's direct light is split per lobe; later vertices' is carried by the diffuse throughput.
            if(extract)
            {
                extraction.diffuse += vertex == 0u ? throughput * directDiffuse : extraction.diffuseThroughput * direct;
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
            if(vertex == 0u)
            {
                // The throughput before vertex 0's sample is 1.
                extraction.diffuseThroughput = diffuseShare * bsdfSample.weight;
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
        PATH_TRACE_RADIANCE_CACHE_HARVEST(PathTraceRadianceCacheHarvestContinue(harvest, vertex, bsdfSample));
        previousDelta = bsdfSample.delta;
        previousPdf = bsdfSample.pdf;

        // Russian roulette before continuing to a vertex at or beyond the configured depth.
        float survivalRandom = PathTraceNextFloat(random);

        if(vertex + 1u >= russianRouletteDepth)
        {
            float survival = min(max(throughput.r, max(throughput.g, throughput.b)), 0.95f);

            if(!(survival > 0.0f) || survivalRandom >= survival)
            {
                break;
            }

            throughput /= survival;
            PATH_TRACE_RADIANCE_CACHE_HARVEST(PathTraceRadianceCacheHarvestSurvive(harvest, vertex, survival));

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
        ray.Direction = bsdfSample.direction;
        ray.TMin = 0.0f;
        ray.TMax = PATH_TRACE_DISTANT_LIGHT_DISTANCE;
    }

    PATH_TRACE_RADIANCE_CACHE_HARVEST(PathTraceRadianceCacheHarvestEnd(harvest));
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

        if(primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT)
        {
            g_splitPixel = pixel;
        }

        float diffuseNormalization = 1.0f;

        if(extract && rasterFirstVertex.state > 0)
        {
            diffuseNormalization = PathTraceExtractionDiffuseNormalization(rasterFirstVertex.position);
        }

        g_sampleManual = sampleDiagnosticIndex != 0xFFFFFFFFu && all(pixel == sampleDiagnosticPixel);
        g_sampleDiagnostic = g_sampleManual || sampleAutoIndex != 0xFFFFFFFFu;
        g_samplePixel = pixel;
        for(uint sampleOffset = 0u; sampleOffset < samplesPerFrame; ++sampleOffset)
        {
            if(g_sampleDiagnostic)
            {
                g_sampleRecord = (PathTraceSampleRecord)0;
                g_sampleRecord.info = 0xFFFFFFFFu;
                g_sampleRecord.hit.x = -1.0f;
                g_sampleMaterial = 0xFFFFFFFFu;
            }
            PathTraceRandom random = PathTraceCreateRandom(pixel, accumulatedSamples + sampleOffset);

            if(primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT)
            {
                g_splitSampleOffset = sampleOffset;
            }

            PathTraceExtraction extraction = PathTraceEmptyExtraction();
#if defined(PATH_TRACE_RADIANCE_CACHE_RENDER) && PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
            PathTraceSharcDiagnosticBeginSample();
#endif
            float3 sampleRadiance = PathTracePrimaryRadiance(primary, pixel, rasterFirstVertex, random, rayCount, extract,
                extraction);
#if defined(PATH_TRACE_RADIANCE_CACHE_RENDER) && PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
            g_pathTraceSharcDiagnosticSum += g_pathTraceSharcDiagnosticSample;
#endif

            if(g_sampleDiagnostic) PathTraceDiagnosticStore(sampleOffset, sampleRadiance,
                !all(isfinite(sampleRadiance)) || any(sampleRadiance < 0.0f));
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

        if(g_sampleDiagnostic) PathTraceDiagnosticMean(mean);
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

#if defined(PATH_TRACE_RADIANCE_CACHE_RENDER) && PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
        // Display only: leave the production accumulation and extracted signals above unchanged.
        float3 diagnostic = g_pathTraceSharcDiagnosticSum / float(max(samplesPerFrame, 1u));
#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC == 3
        // Fixed visualization of the frame-mean cached RGB: c/(1+c), then sRGB. Never physical illumination.
        diagnostic = diagnostic / (1.0f + diagnostic);
        diagnostic = select(diagnostic <= 0.0031308f, diagnostic * 12.92f,
            1.055f * pow(diagnostic, 1.0f / 2.4f) - 0.055f);
#endif
        outputTexture[pixel] = float4(diagnostic, 1.0f);
#endif
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
    PathTraceRunRayGeneration(PATH_TRACE_PRIMARY_CAMERA, false);
}

// RTXPT.2 raster-primary mode.
[shader("raygeneration")]
void PathTraceRasterPrimaryRayGeneration()
{
    PathTraceRunRayGeneration(PATH_TRACE_PRIMARY_RASTER, false);
}

#ifdef PATH_TRACE_RESTIR_DI_LIBRARY
// RTXPT.3 ReSTIR DI direct pass (PathTraceReSTIRDI.hlsl only): the RTXDI work of vertex 0 for every sample of this
// dispatch, initial sampling, sample 0's spatiotemporal reuse, visibility and reservoir publication, with no path
// continuation. Each sample's estimate is stored for the indirect pass; samples whose path never reaches vertex-0
// direct lighting store zero, and the indirect pass never reads them. Every active pixel writes its reservoir each
// dispatch; a pixel whose sample 0 ran no RTXDI sampling (background, unusable sample, pure delta mirror) writes
// an empty one. Only rays are counted here; the indirect pass counts samples. extract (a literal) also stores each
// sample's diffuse-lobe part, at the same address in the splitDirectDiffuseIndex buffer (RTXPT D.1B.2).
void PathTraceRunReSTIRDISplitDirect(bool extract)
{
    uint2 pixel = DispatchRaysIndex().xy;
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

        g_rtxdiPixel = pixel;
        g_rtxdiReservoirStored = false;
        RWByteAddressBuffer directRadiance = ResourceDescriptorHeap[splitDirectRadianceIndex];

        for(uint sampleOffset = 0u; sampleOffset < samplesPerFrame; ++sampleOffset)
        {
            g_rtxdiSampleSeed = PathTraceHash((accumulatedSamples + sampleOffset) ^ PathTraceHash(randomSeed ^
                0x52545844u));
            g_rtxdiStoreReservoir = sampleOffset == 0u;
            float3 sampleDirect = float3(0.0f, 0.0f, 0.0f);
            float3 sampleDirectDiffuse = float3(0.0f, 0.0f, 0.0f);

            if(directLighting)
            {
                // RTXDI draws from its own sequence; the path randoms it discards are the indirect pass's.
                PathTraceRandom discardedRandom = (PathTraceRandom)0;
                sampleDirect = PathTraceRtxdiDirectLighting(rasterFirstVertex.surface, rasterFirstVertex.position,
                    discardedRandom, rayCount, sampleDirectDiffuse);
            }

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

// A DXR library holds one of the two direct passes, never both: with a second RTXDI direct-pass ray generation in
// the same library (even an exact duplicate of the production one), the production direct pass rendered different,
// darker images at runtime (RTXPT D.1B.2 bisection; the compiled DXIL of the production export was unchanged).
// PathTraceReSTIRDIExtract*.hlsl define PATH_TRACE_RESTIR_DI_EXTRACTION_LIBRARY for the extraction one.
#ifndef PATH_TRACE_RESTIR_DI_EXTRACTION_LIBRARY
[shader("raygeneration")]
void PathTraceReSTIRDISplitDirectRayGeneration()
{
    PathTraceRunReSTIRDISplitDirect(false);
}
#else
// RTXPT D.1B.2: the direct pass with signal extraction.
[shader("raygeneration")]
void PathTraceReSTIRDISplitDirectExtractRayGeneration()
{
    PathTraceRunReSTIRDISplitDirect(true);
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
}
