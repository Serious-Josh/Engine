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

struct PathTracePayload
{
    float hitDistance; // Negative on a miss.
    float3 geometricNormal;
    float3 shadingNormal;
    float2 uv;
    uint materialIndex;
};

struct PathTraceVisibilityPayload
{
    uint visible;
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
    uint3 padding;

    // RTXPT.2 R3, appended: read only by PathTraceRasterPrimaryRayGeneration.
    row_major float4x4 inverseCameraRelativeViewProjection;

    float3 cameraPosition;
    uint gBufferDepthIndex;

    uint gBufferMaterialIndex;
    uint gBufferSurfaceIndex;
    uint gBufferGeometricNormalIndex;
    uint rasterPrimaryPadding;
};

cbuffer SceneLightConstants : register(b1)
{
    uint sceneLightCount;
    uint3 sceneLightPadding;
    GPULight sceneLights[SCENE_LIGHT_CAPACITY];
};

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

    PathTraceVisibilityPayload payload;
    payload.visible = 0u;
    TraceRay(scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF, 0, 1,
        PATH_TRACE_MISS_VISIBILITY, ray, payload);
    ++rayCount;
    return payload.visible != 0u;
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

// Adds one light sample: f * cos * Li * visibility * MIS / pdf. Delta lights (pdf 0) use weight 1.
float3 PathTraceShadeLightSample(PathTraceSurface surface, float3 position, PathTraceLightSample lightSample,
    inout uint rayCount)
{
    if(!lightSample.valid || !PathTraceDirectionAboveSurface(surface, lightSample.direction))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 value = PathTraceEvaluateBSDF(surface, lightSample.direction) * dot(surface.normal, lightSample.direction) *
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

    return value * weight;
}

float3 PathTraceDirectLighting(PathTraceSurface surface, float3 position, inout PathTraceRandom random,
    inout uint rayCount)
{
    float3 radiance = float3(0.0f, 0.0f, 0.0f);

    // Random numbers are consumed unconditionally so sample streams stay aligned across scenes.
    float2 sunRandom = PathTraceNextFloat2(random);
    float4 environmentRandom = PathTraceNextFloat4(random);

    PathTraceSun sun = PathTraceFrameSun();

    if(PathTraceSunHasEnergy(sun))
    {
        radiance += PathTraceShadeLightSample(surface, position, PathTraceSampleSun(sun, sunRandom), rayCount);
    }

    uint lightCount = min(sceneLightCount, SCENE_LIGHT_CAPACITY);

    for(uint lightIndex = 0u; lightIndex < lightCount; ++lightIndex)
    {
        PathTraceLightSample lightSample = PathTraceSampleTableLight(sceneLights[lightIndex], position);
        radiance += PathTraceShadeLightSample(surface, position, lightSample, rayCount);
    }

    PathTraceEnvironment environment = PathTraceFrameEnvironment();

    if(PathTraceEnvironmentSamplingEnabled(environment))
    {
        PathTraceLightSample environmentSample = PathTraceSampleEnvironment(environment, environmentSampler,
            environmentRandom);
        radiance += PathTraceShadeLightSample(surface, position, environmentSample, rayCount);
    }

    return radiance;
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

// How the path loop forms its first vertex. Every ray-generation entry passes one of these as a literal,
// so the compiler specialises the loop per entry and folds the other cases away: the reference entry's loop
// is exactly the RTXPT.1 loop (RTXPT.2 R2 and R3 verify its accumulation bit for bit).
static const uint PATH_TRACE_PRIMARY_CAMERA = 0u;
static const uint PATH_TRACE_PRIMARY_PIXEL_CENTER = 1u;
static const uint PATH_TRACE_PRIMARY_RASTER = 2u;

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

// The path loop shared by every primary constructor. The raster primary takes vertex 0 from its decoded
// G-buffer sample where the camera primaries trace and build their first hit; everything after it (sampling,
// BSDF, MIS, bounces, roulette) is the same code.
float3 PathTraceTracePath(RayDesc ray, uint primary, PathTraceRasterFirstVertex rasterFirstVertex,
    inout PathTraceRandom random, inout uint rayCount)
{
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    bool previousDelta = true;
    float previousPdf = 0.0f;

    // vertex counts surface interactions. The ray traced after the last allowed vertex is still
    // traced so distant lights reached by BSDF sampling pair with that vertex's light samples.
    for(uint vertex = 0u; vertex <= maxPathDepth; ++vertex)
    {
        float3 position;
        PathTraceSurface surface;

        if(primary == PATH_TRACE_PRIMARY_RASTER)
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
            TraceRay(scene, RAY_FLAG_NONE, gBufferVertex ? 0x00 : 0xFF, 0, 1, PATH_TRACE_MISS_RADIANCE, ray, payload);

            if(gBufferVertex)
            {
                if(rasterFirstVertex.state == 0)
                {
                    radiance += throughput * PathTraceMissRadiance(rasterFirstVertex.backgroundDirection, previousDelta,
                        previousPdf);
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
                // The camera primaries' hit handling, unchanged.
                ++rayCount;

                if(payload.hitDistance < 0.0f)
                {
                    radiance += throughput * PathTraceMissRadiance(ray.Direction, previousDelta, previousPdf);
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
            TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, PATH_TRACE_MISS_RADIANCE, ray, payload);
            ++rayCount;

            if(payload.hitDistance < 0.0f)
            {
                radiance += throughput * PathTraceMissRadiance(ray.Direction, previousDelta, previousPdf);
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

        bool hasNonDeltaLobe = !surface.deltaSpecular || surface.specularProbability < 1.0f;

        if(hasNonDeltaLobe)
        {
            radiance += throughput * PathTraceDirectLighting(surface, position, random, rayCount);
        }

        PathTraceBSDFSample bsdfSample = PathTraceSampleBSDF(surface, PathTraceNextFloat3(random));

        if(!bsdfSample.valid)
        {
            break;
        }

        throughput *= bsdfSample.weight;
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

    return PathTraceTracePath(ray, PATH_TRACE_PRIMARY_CAMERA, (PathTraceRasterFirstVertex)0, random, rayCount);
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

    return PathTraceTracePath(ray, PATH_TRACE_PRIMARY_PIXEL_CENTER, (PathTraceRasterFirstVertex)0, random, rayCount);
}

// RTXPT.2 R3 raster primary: no camera ray; vertex 0 is the pixel's decoded G-buffer sample. The vertex-0
// TraceRay of the loop uses this well-formed, empty ray (and instance mask 0).
float3 PathTraceRasterPrimaryRadiance(PathTraceRasterFirstVertex rasterFirstVertex, inout PathTraceRandom random,
    inout uint rayCount)
{
    PathTraceNextFloat2(random);

    RayDesc emptyRay;
    emptyRay.Origin = float3(0.0f, 0.0f, 0.0f);
    emptyRay.Direction = float3(0.0f, 0.0f, 1.0f);
    emptyRay.TMin = 0.0f;
    emptyRay.TMax = 0.0f;
    return PathTraceTracePath(emptyRay, PATH_TRACE_PRIMARY_RASTER, rasterFirstVertex, random, rayCount);
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
    inout PathTraceRandom random, inout uint rayCount)
{
    if(primary == PATH_TRACE_PRIMARY_RASTER)
    {
        return PathTraceRasterPrimaryRadiance(rasterFirstVertex, random, rayCount);
    }

    if(primary == PATH_TRACE_PRIMARY_PIXEL_CENTER)
    {
        return PathTracePixelCenterRadiance(pixel, random, rayCount);
    }

    return PathTraceRadiance(pixel, random, rayCount);
}

void PathTraceRunRayGeneration(uint primary)
{
    uint2 pixel = DispatchRaysIndex().xy;
    bool active = all(pixel < resolution);
    uint rayCount = 0u;
    uint rejectedSamples = 0u;
    uint completedSamples = 0u;

    if(active)
    {
        float3 sampleSum = float3(0.0f, 0.0f, 0.0f);

        // The raster primary's first vertex is decoded once for all of this pixel's samples.
        PathTraceRasterFirstVertex rasterFirstVertex = (PathTraceRasterFirstVertex)0;

        if(primary == PATH_TRACE_PRIMARY_RASTER)
        {
            rasterFirstVertex = PathTraceDecodeRasterFirstVertex(pixel);
        }

        for(uint sampleOffset = 0u; sampleOffset < samplesPerFrame; ++sampleOffset)
        {
            PathTraceRandom random = PathTraceCreateRandom(pixel, accumulatedSamples + sampleOffset);
            float3 sampleRadiance = PathTracePrimaryRadiance(primary, pixel, rasterFirstVertex, random, rayCount);

            // A non-finite estimate is a defect, never silently averaged; it is counted and dropped.
            if(!all(isfinite(sampleRadiance)) || any(sampleRadiance < 0.0f))
            {
                sampleRadiance = float3(0.0f, 0.0f, 0.0f);
                ++rejectedSamples;
            }

            sampleSum += sampleRadiance;
            ++completedSamples;
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
    PathTraceRunRayGeneration(PATH_TRACE_PRIMARY_CAMERA);
}

// RTXPT.2 raster-primary mode.
[shader("raygeneration")]
void PathTraceRasterPrimaryRayGeneration()
{
    PathTraceRunRayGeneration(PATH_TRACE_PRIMARY_RASTER);
}

// RTXPT.2 diagnostic: unjittered camera rays, for raster-primary agreement measurements.
[shader("raygeneration")]
void PathTracePixelCenterRayGeneration()
{
    PathTraceRunRayGeneration(PATH_TRACE_PRIMARY_PIXEL_CENTER);
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
