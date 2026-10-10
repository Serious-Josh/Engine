#include "MaterialLighting.hlsli"
#include "DiffuseHDRIConfig.hlsli"

#include "GIModeConfig.hlsli"
#ifndef GI_PRODUCTION_CLAMP_SAMPLES
#define GI_PRODUCTION_CLAMP_SAMPLES 64
#endif
#ifndef GI_PRODUCTION_CLAMP_DISPLAY
// 0 = A (mean clamped), 1 = B (clamped mean), 2 = A-B (signed).
#define GI_PRODUCTION_CLAMP_DISPLAY 0
#endif
#ifndef GI_PRODUCTION_CLAMP_BLACK_SECONDARY
#define GI_PRODUCTION_CLAMP_BLACK_SECONDARY 0
#endif
#ifndef GI_PRODUCTION_CLAMP_NUMERIC_OUTPUT
// Mode 13 only: raw RGB = luminance(A), luminance(B), luminance(A-B); alpha = validity.
#define GI_PRODUCTION_CLAMP_NUMERIC_OUTPUT 1
#endif
#if GI_PRODUCTION_CLAMP_SAMPLES != 16 && GI_PRODUCTION_CLAMP_SAMPLES != 64
#error GI_PRODUCTION_CLAMP_SAMPLES must be 16 or 64
#endif
#ifndef GI_CUBEMAP_TEXEL_DISPLAY
#define GI_CUBEMAP_TEXEL_DISPLAY 3
#endif
#ifndef GI_SAME_NORMAL_DISPLAY
// Mode 11: 0 = A, 1 = B, 2 = C, 3 = C-B, 4 = A-C.
#define GI_SAME_NORMAL_DISPLAY 4
#endif
#ifndef GI_BASELINE_REFERENCE_SAMPLES
#define GI_BASELINE_REFERENCE_SAMPLES 4096
#endif
#ifndef GI_BASELINE_RESIDUAL_DISPLAY
// Mode 10: 0 = stored baseline, 1 = mip-0 reference, 2 = signed residual.
#define GI_BASELINE_RESIDUAL_DISPLAY 2
#endif
#ifndef GI_BASELINE_RESIDUAL_SCALE
// Absolute irradiance difference required for full positive/negative display intensity.
#define GI_BASELINE_RESIDUAL_SCALE 10.0f
#endif
#ifndef GI_CLAMP_AB_DISPLAY
// Mode 9: 0 = mean clamped samples, 1 = clamped signed mean, 2 = bounded bias.
#define GI_CLAMP_AB_DISPLAY 1
#endif
#ifndef GI_SECONDARY_ENV_VISIBILITY
#define GI_SECONDARY_ENV_VISIBILITY 1
#endif
#if (GI_DIAGNOSTIC_MODE == GI_MODE_BASELINE_RESIDUAL_DEBUG || GI_DIAGNOSTIC_MODE == GI_MODE_SAME_NORMAL_INTEGRATION_DEBUG || GI_DIAGNOSTIC_MODE == GI_MODE_CUBEMAP_TEXEL_DEBUG) && GI_BASELINE_REFERENCE_SAMPLES < 1
#error GI_BASELINE_REFERENCE_SAMPLES must be positive
#endif

struct GIPayload
{
    float hitDistance;
    float3 worldNormal;
    float2 uv;
    uint materialIndex;
    float3 shadingNormal;
};

struct GIVisibilityPayload
{
    uint visible;
};

struct RayTracingInstanceData
{
    uint vertexBufferIndex;
    uint indexBufferIndex;
    uint materialIndex;
    uint vertexStride;

    uint normalOffset;
    uint uvOffset;
    uint indexed;
    uint tangentOffset;
};

struct DirectionalLightConstants
{
    float3 lightDirection;
    float lightIntensity;

    float3 lightColor;
    float ambientStrength;
};

ConstantBuffer<DirectionalLightConstants> lightConstants : register(b1);

SamplerState materialSampler : register(s0);
SamplerState environmentSampler : register(s1);

cbuffer GIConstants : register(b0)
{
    row_major float4x4 inverseViewProjection;
    uint depthIndex;
    uint surfaceIndex;
    uint frameIndex;
    uint instanceDataIndex;
    uint materialBufferIndex;
    uint environmentIndex;
    uint diffuseIrradianceIndex;
    uint diffuseIrradianceArrayIndex;
    uint geometricNormalIndex;
    uint2 fullResolution;
    uint signedCorrectionIndex;
};

RaytracingAccelerationStructure scene : register(t0);

RWTexture2D<float4> giTexture : register(u0);
RWTexture2D<float> hitDistanceTexture : register(u1);

float3 LoadVertexNormal(
    ByteAddressBuffer vertexBuffer,
    uint vertexIndex,
    uint vertexStride,
    uint normalOffset)
{
    uint address =
        vertexIndex * vertexStride +
        normalOffset;

    uint3 packedNormal =
        vertexBuffer.Load3(address);

    return asfloat(packedNormal);
}

float4 LoadVertexTangent(
    ByteAddressBuffer vertexBuffer,
    uint vertexIndex,
    uint vertexStride,
    uint tangentOffset)
{
    return asfloat(vertexBuffer.Load4(vertexIndex * vertexStride + tangentOffset));
}

float2 LoadVertexUV(
    ByteAddressBuffer vertexBuffer,
    uint vertexIndex,
    uint vertexStride,
    uint uvOffset)
{
    uint address =
        vertexIndex * vertexStride +
        uvOffset;

    uint2 packedUV =
        vertexBuffer.Load2(address);

    return asfloat(packedUV);
}

void GetTriangleVertexIndices(
    RayTracingInstanceData instanceData,
    uint primitiveIndex,
    out uint vertexIndex0,
    out uint vertexIndex1,
    out uint vertexIndex2)
{
    if(instanceData.indexed != 0)
    {
        ByteAddressBuffer indexBuffer =
            ResourceDescriptorHeap[
                NonUniformResourceIndex(
                    instanceData.indexBufferIndex
                )
            ];

        uint indexAddress =
            primitiveIndex *
            3 *
            sizeof(uint);

        vertexIndex0 =
            indexBuffer.Load(
                indexAddress + 0
            );

        vertexIndex1 =
            indexBuffer.Load(
                indexAddress + 4
            );

        vertexIndex2 =
            indexBuffer.Load(
                indexAddress + 8
            );
    }
    else
    {
        vertexIndex0 =
            primitiveIndex * 3 + 0;

        vertexIndex1 =
            primitiveIndex * 3 + 1;

        vertexIndex2 =
            primitiveIndex * 3 + 2;
    }
}

float3 SampleGIEnvironment(float3 rayDirection, float sourceMip = 0.0f)
{
    Texture2D<float4> environmentTexture = ResourceDescriptorHeap[environmentIndex];
    float2 environmentUV = DirectionToEquirectangularUV(normalize(rayDirection));
    return environmentTexture.SampleLevel(environmentSampler, environmentUV, sourceMip).rgb;
}

// Called from raygen only, after the surface trace has returned.
// Diagnostic counterpart: same material/environment evaluation, no directional lighting or rays.
float3 ShadeGIWhiteDiffuseSecondary(float3 secondaryNormal)
{
    float normalLengthSquared = dot(secondaryNormal, secondaryNormal);

    if(!all(isfinite(secondaryNormal)) || !isfinite(normalLengthSquared) || normalLengthSquared < 1.0e-8f)
    {
        return 0.0f;
    }

    float3 normal = normalize(secondaryNormal);
    TextureCube<float4> diffuseIrradiance = ResourceDescriptorHeap[diffuseIrradianceIndex];
    float3 environmentIrradiance = diffuseIrradiance.SampleLevel(materialSampler, normal, 0.0f).rgb;
    // Unit Lambertian reflectance: no secondary material lookup or weighting.
    return environmentIrradiance / PI;
}

float3 ShadeGISecondaryEnvironmentOnly(GIPayload surface, float3 secondaryPosition)
{
    float normalLengthSquared = dot(surface.shadingNormal, surface.shadingNormal);

    if(!all(isfinite(secondaryPosition)) || !all(isfinite(surface.shadingNormal)) ||
       !all(isfinite(surface.uv)) || !isfinite(normalLengthSquared) || normalLengthSquared < 1.0e-8f)
    {
        return 0.0f;
    }

    float3 normal = normalize(surface.shadingNormal);

    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[materialBufferIndex];
    uint materialCount, materialStride;
    materials.GetDimensions(materialCount, materialStride);

    if(surface.materialIndex >= materialCount)
    {
        return 0.0f;
    }

    GPUMaterial material = materials[surface.materialIndex];
    // Model 0 is metallic-roughness, model 1 dielectric; conductors have no diffuse lobe.

    if(material.model > 1u || !all(isfinite(material.baseColor)) ||
       (material.model == 0u && !isfinite(material.metallic)))
    {
        return 0.0f;
    }

    Texture2D<float4> materialTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.textureIndex)];
    float3 sampledColor = materialTexture.SampleLevel(materialSampler, surface.uv, 0.0f).rgb;

    if(!all(isfinite(sampledColor)))
    {
        return 0.0f;
    }

    float3 diffuseColor = max(material.baseColor * sampledColor, 0.0f);

    if(material.model == 0u)
    {
        diffuseColor *= 1.0f - saturate(material.metallic);
    }

    if(!all(isfinite(diffuseColor)) || !any(diffuseColor > 0.0f))
    {
        return 0.0f;
    }

    TextureCube<float4> diffuseIrradiance = ResourceDescriptorHeap[diffuseIrradianceIndex];
    float3 environmentIrradiance = diffuseIrradiance.SampleLevel(materialSampler, normal, 0.0f).rgb;
    float3 environmentRadiance = (diffuseColor / PI) * environmentIrradiance;

    if(!all(isfinite(environmentRadiance)))
    {
        environmentRadiance = 0.0f;
    }

    return environmentRadiance;
}

float3 SampleGICosineHemisphere(float3 normal, uint2 pixel, uint sampleIndex);

float3 ShadeGISecondarySurface(GIPayload surface, float3 secondaryPosition, uint2 pixel, uint sampleIndex, float sourceMip = 0.0f)
{
    float normalLengthSquared = dot(surface.shadingNormal, surface.shadingNormal);

    if(!all(isfinite(secondaryPosition)) || !all(isfinite(surface.shadingNormal)) ||
       !all(isfinite(surface.uv)) || !isfinite(normalLengthSquared) || normalLengthSquared < 1.0e-8f)
    {
        return 0.0f;
    }

    float3 normal = normalize(surface.shadingNormal);

    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[materialBufferIndex];
    uint materialCount, materialStride;
    materials.GetDimensions(materialCount, materialStride);

    if(surface.materialIndex >= materialCount)
    {
        return 0.0f;
    }

    GPUMaterial material = materials[surface.materialIndex];
    // Model 0 is metallic-roughness, model 1 dielectric; conductors have no diffuse lobe.

    if(material.model > 1u || !all(isfinite(material.baseColor)) ||
       (material.model == 0u && !isfinite(material.metallic)))
    {
        return 0.0f;
    }

    Texture2D<float4> materialTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.textureIndex)];
    float3 sampledColor = materialTexture.SampleLevel(materialSampler, surface.uv, 0.0f).rgb;

    if(!all(isfinite(sampledColor)))
    {
        return 0.0f;
    }

    float3 diffuseColor = max(material.baseColor * sampledColor, 0.0f);

    if(material.model == 0u)
    {
        diffuseColor *= 1.0f - saturate(material.metallic);
    }

    if(!all(isfinite(diffuseColor)) || !any(diffuseColor > 0.0f))
    {
        return 0.0f;
    }

    float3 environmentRadiance = 0.0f;
#if GI_SECONDARY_ENV_VISIBILITY
    // Average four independently seeded secondary environment visibility samples.
    for(uint environmentSample = 0u; environmentSample < 4u; ++environmentSample)
    {
        uint environmentSampleIndex = sampleIndex ^ 0xD1B54A35u ^ (environmentSample * 0x9E3779B9u);
        float3 environmentDirection = SampleGICosineHemisphere(normal, pixel, environmentSampleIndex);
        RayDesc environmentRay;
        environmentRay.Origin = secondaryPosition + normalize(surface.worldNormal) * 0.001f;
        environmentRay.Direction = environmentDirection;
        environmentRay.TMin = 0.001f;
        environmentRay.TMax = 10000.0f;
        if(all(isfinite(environmentRay.Origin)) && all(isfinite(environmentDirection)))
        {
            GIVisibilityPayload environmentPayload;
            environmentPayload.visible = 0u;
            // Launched from raygen after the primary trace; hit group 0 is skipped, miss record 1.
            TraceRay(scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                0xFF, 0, 1, 1, environmentRay, environmentPayload);
            if(environmentPayload.visible != 0u)
            {
                // Lambertian BRDF * cosine / cosine-weighted PDF cancels PI.
                environmentRadiance += diffuseColor * SampleGIEnvironment(environmentDirection, sourceMip);
            }
        }
    }
    environmentRadiance *= 0.25f;
#endif

    if(!all(isfinite(environmentRadiance)))
    {
        environmentRadiance = 0.0f;
    }

    // Invalid or back-facing directional lighting must not suppress environment diffuse.
    float lightLengthSquared = dot(lightConstants.lightDirection, lightConstants.lightDirection);

    if(!all(isfinite(lightConstants.lightDirection)) || !isfinite(lightLengthSquared) || lightLengthSquared < 1.0e-8f ||
       !all(isfinite(lightConstants.lightColor)) || !isfinite(lightConstants.lightIntensity) ||
       lightConstants.lightIntensity <= 0.0f)
    {
        return environmentRadiance;
    }

    float3 directionToLight = normalize(-lightConstants.lightDirection);
    float NdotL = saturate(dot(normal, directionToLight));

    if(NdotL <= 0.0f)
    {
        return environmentRadiance;
    }

    RayDesc visibilityRay;
    visibilityRay.Origin = secondaryPosition + normalize(surface.worldNormal) * 0.001f;
    visibilityRay.Direction = directionToLight;
    visibilityRay.TMin = 0.001f;
    visibilityRay.TMax = 10000.0f;
    GIVisibilityPayload visibilityPayload;
    visibilityPayload.visible = 0u; // A hit leaves this unchanged because closest-hit is skipped.
    // Reuse hit group 0 (skipped), geometry stride 1, visibility miss record 1.
    TraceRay(scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
        0xFF, 0, 1, 1, visibilityRay, visibilityPayload);

    return environmentRadiance + (diffuseColor / PI) * max(lightConstants.lightColor, 0.0f) *
        lightConstants.lightIntensity * NdotL * float(visibilityPayload.visible);
}

uint GIHash(uint value)
{
    // Match the reflection shader's 32-bit MurmurHash3 finalizer.
    value ^= value >> 16u;
    value *= 0x85EBCA6Bu;
    value ^= value >> 13u;
    value *= 0xC2B2AE35u;
    value ^= value >> 16u;
    return value;
}

float3 SampleGICosineHemisphere(float3 normal, uint2 pixel, uint sampleIndex)
{
    uint seed = GIHash(
        GIHash(pixel.x ^ 0xA511E9B3u) ^
        GIHash(pixel.y ^ 0x63D83595u) ^
        GIHash(sampleIndex ^ 0x369DEA0Fu));
    // Distinct salts decorrelate dimensions; the upper 24 bits map to [0, 1).
    float u1 = float(GIHash(seed ^ 0xA24BAED5u) >> 8u) * (1.0f / 16777216.0f);
    float u2 = float(GIHash(seed ^ 0x9FB21C65u) >> 8u) * (1.0f / 16777216.0f);

    float r = sqrt(u1);
    float phi = 6.28318530718f * u2;
    float3 localDirection = float3(
        r * cos(phi),
        r * sin(phi),
        sqrt(max(0.0f, 1.0f - u1)));

    // Use the reflection shader's helper-axis switch to avoid parallel vectors.
    float3 tangent = normalize(abs(normal.z) < 0.999f
        ? cross(float3(0.0f, 0.0f, 1.0f), normal)
        : cross(float3(0.0f, 1.0f, 0.0f), normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(tangent * localDirection.x + bitangent * localDirection.y +
        normal * localDirection.z);
}

float CalculateGIReferenceSourceMip(
    Texture2D<float4> sourceTexture,
    float3 sampleDirection,
    float pdf,
    uint sampleCount)
{
    uint sourceWidth;
    uint sourceHeight;
    uint sourceMipCount;

    sourceTexture.GetDimensions(
        0,
        sourceWidth,
        sourceHeight,
        sourceMipCount
    );

    float sampleSolidAngle =
        1.0f /
        max(
            float(sampleCount) * pdf,
            0.000001f
        );

    float cosLatitude =
        sqrt(
            max(
                1.0f -
                sampleDirection.y *
                sampleDirection.y,
                0.000001f
            )
        );

    float longitudeStep =
        (2.0f * PI) /
        float(sourceWidth);

    float latitudeStep =
        PI /
        float(sourceHeight);

    float texelSolidAngle =
        longitudeStep *
        latitudeStep *
        cosLatitude;

    float sourceMip =
        0.5f *
        log2(
            sampleSolidAngle /
            max(
                texelSolidAngle,
                1.0e-12f
            )
        );

    return clamp(
        sourceMip,
        0.0f,
        float(sourceMipCount - 1)
    );
}

float3 SampleGIEnvironmentReference4096(float3 normal)
{
    const uint sampleCount = 4096u;
    Texture2D<float4> sourceTexture = ResourceDescriptorHeap[environmentIndex];
    // Match the convolution's basis, Hammersley sequence, and source-mip filtering.
    float3 up = abs(normal.y) < 0.999f
        ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    float3 radianceSum = 0.0f;
    [loop]
    for(uint i = 0u; i < sampleCount; ++i)
    {
        float2 xi = float2(float(i) / float(sampleCount),
            float(reversebits(i)) * 2.3283064365386963e-10f);
        float r = sqrt(xi.x);
        float phi = 2.0f * PI * xi.y;
        float3 localDirection = float3(r * cos(phi), r * sin(phi),
            sqrt(max(0.0f, 1.0f - xi.x)));
        float3 direction = normalize(tangent * localDirection.x +
            bitangent * localDirection.y + normal * localDirection.z);
        float pdf = saturate(dot(normal, direction)) / PI;
        float sourceMip = CalculateGIReferenceSourceMip(sourceTexture, direction, pdf, sampleCount);
        float2 environmentUV = DirectionToEquirectangularUV(direction);
        radianceSum += sourceTexture.SampleLevel(environmentSampler, environmentUV, sourceMip).rgb;
    }
    return radianceSum / float(sampleCount);
}

#if GI_DIAGNOSTIC_MODE == GI_MODE_SAME_NORMAL_INTEGRATION_DEBUG || GI_DIAGNOSTIC_MODE == GI_MODE_CUBEMAP_TEXEL_DEBUG
// Independent transcription of DiffuseIrradiance.hlsl's forced-mip-0 path.
// Keep its explicit radical inverse and direct source sampling separate from B.
float3 IntegrateGISameNormalConvolution(float3 normal)
{
    Texture2D<float4> sourceTexture = ResourceDescriptorHeap[environmentIndex];
    float3 up = abs(normal.y) < 0.999f
        ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    float3 irradiance = 0.0f;
    [loop]
    for(uint i = 0u; i < GI_BASELINE_REFERENCE_SAMPLES; ++i)
    {
        uint bits = i;
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        float2 xi = float2(float(i) / float(GI_BASELINE_REFERENCE_SAMPLES),
            float(bits) * 2.3283064365386963e-10f);
        float radius = sqrt(xi.x);
        float phi = 2.0f * PI * xi.y;
        float x = radius * cos(phi);
        float y = radius * sin(phi);
        float z = sqrt(max(0.0f, 1.0f - xi.x));
        float3 localDirection = float3(x, y, z);
        float3 sampleDirection = normalize(tangent * localDirection.x +
            bitangent * localDirection.y + normal * localDirection.z);
        float2 environmentUV = DirectionToEquirectangularUV(sampleDirection);
        float3 sampleColor = sourceTexture.SampleLevel(
            environmentSampler, environmentUV, 0.0f).rgb;
        // Propagate invalid samples to the caller's magenta handling.

        if(!all(isfinite(sampleColor)))
        {
            return sampleColor;
        }

        irradiance += sampleColor;
    }
    irradiance *= PI / float(GI_BASELINE_REFERENCE_SAMPLES);
    return irradiance;
}
#endif

[shader("raygeneration")]
void GIRayGeneration()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 gbufferPixel = min(
        pixel * 2u,
        fullResolution - 1u);

    giTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
#if GI_DIAGNOSTIC_MODE == GI_MODE_PRODUCTION_CLAMP_BIAS_DEBUG && GI_PRODUCTION_CLAMP_NUMERIC_OUTPUT
    giTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
#endif
#if GI_DIAGNOSTIC_MODE == GI_MODE_PRIMARY_VISIBILITY_DEBUG
    // Magenta distinguishes invalid/background pixels. Alpha -2 selects raw grayscale in GI Raw.
    giTexture[pixel] = float4(1.0f, 0.0f, 1.0f, -2.0f);
#endif
    hitDistanceTexture[pixel] = -2.0f;
#if GI_DIAGNOSTIC_MODE == GI_MODE_BASELINE_CORRECTION
    if(signedCorrectionIndex != 0xFFFFFFFFu)
    {
        RWTexture2D<float4> signedCorrectionTexture = ResourceDescriptorHeap[signedCorrectionIndex];
        signedCorrectionTexture[pixel] = 0.0f; // Invalid/background: no signed sample.
    }
#endif

    Texture2D<float> depthTexture = ResourceDescriptorHeap[depthIndex];
    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[surfaceIndex];

    const float depth = depthTexture.Load(int3(gbufferPixel, 0));

    if(!isfinite(depth) || depth < 0.0f || depth >= 1.0f)
    {
        return;
    }

    // Match reflection/shadow reconstruction: pixel centers, flipped NDC Y,
    // D3D depth in [0, 1], and a row-major matrix multiplying a column vector.
    const float2 uv = (float2(gbufferPixel) + 0.5f) / float2(fullResolution);
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 worldPosition = mul(inverseViewProjection, float4(ndc, depth, 1.0f));

    if(!all(isfinite(worldPosition)) || abs(worldPosition.w) < 1.0e-8f)
    {
        return;
    }

    worldPosition /= worldPosition.w;

    if(!all(isfinite(worldPosition)))
    {
        return;
    }

    // Surface RGB already stores a world-space normal, without remapping.
    const float3 surfaceNormal = surfaceTexture.Load(int3(gbufferPixel, 0)).rgb;
    const float normalLengthSquared = dot(surfaceNormal, surfaceNormal);

    if(!all(isfinite(surfaceNormal)) || !isfinite(normalLengthSquared) ||
       normalLengthSquared < 1.0e-8f)
    {
        return;
    }

    const float3 normal = normalize(surfaceNormal);

    Texture2D<float4> geometricNormalTexture = ResourceDescriptorHeap[geometricNormalIndex];
    const float3 geometricNormalValue = geometricNormalTexture.Load(int3(gbufferPixel, 0)).rgb;
    const float geometricNormalLengthSquared = dot(geometricNormalValue, geometricNormalValue);

    if(!all(isfinite(geometricNormalValue)) || !isfinite(geometricNormalLengthSquared) ||
       geometricNormalLengthSquared < 1.0e-8f)
    {
        return;
    }

    const float3 geometricNormal = normalize(geometricNormalValue);

#if GI_DIAGNOSTIC_MODE == GI_MODE_PRIMARY_VISIBILITY_DEBUG
    // Temporary geometry-only diagnostic: production primary parameters, without hit shading.
    uint environmentMisses = 0u;
    [loop]
    for(uint sampleIndex = 0u; sampleIndex < 64u; ++sampleIndex)
    {
        RayDesc visibilityRay;
        visibilityRay.Origin = worldPosition.xyz + geometricNormal * 0.001f;
        visibilityRay.Direction = SampleGICosineHemisphere(normal, pixel, frameIndex * 64u + sampleIndex);
        visibilityRay.TMin = 0.001f;
        visibilityRay.TMax = 10000.0f;

        if(!all(isfinite(visibilityRay.Origin)) || !all(isfinite(visibilityRay.Direction)))
        {
            return;
        }

        GIVisibilityPayload visibilityPayload;
        visibilityPayload.visible = 0u;
        TraceRay(scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
            0xFF, 0, 1, 1, visibilityRay, visibilityPayload);
        environmentMisses += visibilityPayload.visible;
    }
    const float visibility = float(environmentMisses) / 64.0f;
    giTexture[pixel] = float4(visibility, visibility, visibility, -2.0f);
    // Leave hit distance at no-data: this visualization is not an NRD lighting input.
    return;
#elif GI_DIAGNOSTIC_MODE == GI_MODE_CUBEMAP_TEXEL_DEBUG
    Texture2DArray<float4> arrayTexture = ResourceDescriptorHeap[diffuseIrradianceArrayIndex];
    TextureCube<float4> cubeTexture = ResourceDescriptorHeap[diffuseIrradianceIndex];
    uint width, height, layers;
    arrayTexture.GetDimensions(width, height, layers);
    if(width == 0u || height == 0u || layers != 6u)
    {
        giTexture[pixel] = float4(1.0f, 0.0f, 1.0f, 1.0f);
        return;
    }
    // Deterministic ties: X before Y before Z. Inverse of convolution face mapping.
    float3 axis = abs(normal);
    uint face;
    float2 faceCoordinates;
    if(axis.x >= axis.y && axis.x >= axis.z)
    {
        face = normal.x >= 0.0f ? 0u : 1u;
        faceCoordinates = float2(normal.x >= 0.0f ? -normal.z : normal.z, -normal.y) / axis.x;
    }
    else if(axis.y >= axis.z)
    {
        face = normal.y >= 0.0f ? 2u : 3u;
        faceCoordinates = float2(normal.x, normal.y >= 0.0f ? normal.z : -normal.z) / axis.y;
    }
    else
    {
        face = normal.z >= 0.0f ? 4u : 5u;
        faceCoordinates = float2(normal.z >= 0.0f ? normal.x : -normal.x, -normal.y) / axis.z;
    }
    uint2 texel = min(uint2(saturate(faceCoordinates * 0.5f + 0.5f) * float2(width, height)),
        uint2(width - 1u, height - 1u));
    float2 centerUV = (float2(texel) + 0.5f) / float2(width, height);
    float2 coordinates = centerUV * 2.0f - 1.0f;
    coordinates.y = -coordinates.y;
    float3 centerDirection;
    switch(face)
    {
        case 0: centerDirection = float3(1.0f, coordinates.y, -coordinates.x); break;
        case 1: centerDirection = float3(-1.0f, coordinates.y, coordinates.x); break;
        case 2: centerDirection = float3(coordinates.x, 1.0f, -coordinates.y); break;
        case 3: centerDirection = float3(coordinates.x, -1.0f, coordinates.y); break;
        case 4: centerDirection = float3(coordinates.x, coordinates.y, 1.0f); break;
        default: centerDirection = float3(-coordinates.x, coordinates.y, -1.0f); break;
    }
    centerDirection = normalize(centerDirection);
    float3 stored = arrayTexture.Load(int4(texel, face, 0)).rgb;
    float3 reference = IntegrateGISameNormalConvolution(centerDirection);
    float3 outputValue;
#if GI_CUBEMAP_TEXEL_DISPLAY == 0
    outputValue = stored;
#elif GI_CUBEMAP_TEXEL_DISPLAY == 1
    outputValue = reference;
#elif GI_CUBEMAP_TEXEL_DISPLAY == 2
    outputValue = stored - reference;
#elif GI_CUBEMAP_TEXEL_DISPLAY == 3
    // Actual IEEE binary16 conversion, followed by conversion back to float32.
    float3 rounded = f16tof32(f32tof16(reference));
    outputValue = stored - rounded;
    if(!all(isfinite(rounded)))
    {
        reference = rounded;
    }
#elif GI_CUBEMAP_TEXEL_DISPLAY == 4
    float3 interpolated = cubeTexture.SampleLevel(materialSampler, normal, 0.0f).rgb;
    float3 convolution = IntegrateGISameNormalConvolution(normal);
    outputValue = interpolated - convolution;
#elif GI_CUBEMAP_TEXEL_DISPLAY == 5
    outputValue = cubeTexture.SampleLevel(materialSampler, centerDirection, 0.0f).rgb - stored;
#else
#error Unsupported GI_CUBEMAP_TEXEL_DISPLAY
#endif
    if(!all(isfinite(stored)) || !all(isfinite(reference)) || !all(isfinite(outputValue)))
    {
        giTexture[pixel] = float4(1.0f, 0.0f, 1.0f, 1.0f);
        return;
    }
#if GI_CUBEMAP_TEXEL_DISPLAY >= 2
    float positive = saturate(max(outputValue.r, max(outputValue.g, outputValue.b)) / GI_BASELINE_RESIDUAL_SCALE);
    float negative = saturate(-min(outputValue.r, min(outputValue.g, outputValue.b)) / GI_BASELINE_RESIDUAL_SCALE);
    outputValue = 0.5f * (1.0f - max(positive, negative)) +
        float3(positive, 0.0f, negative) / (1.0f + min(positive, negative));
#endif
    giTexture[pixel] = float4(outputValue, 1.0f);
    return; // Texture sampling only; retain hit-distance sentinel -2.
#elif GI_DIAGNOSTIC_MODE == GI_MODE_BASELINE_RESIDUAL_DEBUG || GI_DIAGNOSTIC_MODE == GI_MODE_SAME_NORMAL_INTEGRATION_DEBUG
    TextureCube<float4> diagnosticIrradiance = ResourceDescriptorHeap[diffuseIrradianceIndex];
    float3 baseline = diagnosticIrradiance.SampleLevel(materialSampler, normal, 0.0f).rgb;
    float3 up = abs(normal.y) < 0.999f
        ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    float3 radianceSum = 0.0f;
    bool referenceFinite = all(isfinite(baseline));
    [loop]
    for(uint i = 0u; i < GI_BASELINE_REFERENCE_SAMPLES; ++i)
    {
        // Deterministic Hammersley cosine sampling; no convolution source-mip filtering.
        float2 xi = float2(float(i) / float(GI_BASELINE_REFERENCE_SAMPLES),
            float(reversebits(i)) * 2.3283064365386963e-10f);
        float r = sqrt(xi.x);
        float phi = 2.0f * PI * xi.y;
        float3 localDirection = float3(r * cos(phi), r * sin(phi),
            sqrt(max(0.0f, 1.0f - xi.x)));
        float3 direction = normalize(tangent * localDirection.x +
            bitangent * localDirection.y + normal * localDirection.z);
        float3 radiance = SampleGIEnvironment(direction);
        if(!all(isfinite(radiance)))
        {
            referenceFinite = false;
            continue;
        }
        radianceSum += radiance;
    }
    float3 reference = radianceSum * (PI / float(GI_BASELINE_REFERENCE_SAMPLES));
    float3 residual = baseline - reference;
#if GI_DIAGNOSTIC_MODE == GI_MODE_SAME_NORMAL_INTEGRATION_DEBUG
    float3 convolution = IntegrateGISameNormalConvolution(normal);
    referenceFinite = referenceFinite && all(isfinite(convolution));
#if GI_SAME_NORMAL_DISPLAY == 3
    residual = convolution - reference;
#elif GI_SAME_NORMAL_DISPLAY == 4
    residual = baseline - convolution;
#endif
#endif
    if(!referenceFinite || !all(isfinite(reference)) || !all(isfinite(residual)))
    {
        giTexture[pixel] = float4(1.0f, 0.0f, 1.0f, 1.0f);
        return;
    }
#if GI_DIAGNOSTIC_MODE == GI_MODE_SAME_NORMAL_INTEGRATION_DEBUG
#if GI_SAME_NORMAL_DISPLAY == 0
    giTexture[pixel] = float4(baseline, 1.0f);
#elif GI_SAME_NORMAL_DISPLAY == 1
    giTexture[pixel] = float4(reference, 1.0f);
#elif GI_SAME_NORMAL_DISPLAY == 2
    giTexture[pixel] = float4(convolution, 1.0f);
#elif GI_SAME_NORMAL_DISPLAY == 3 || GI_SAME_NORMAL_DISPLAY == 4
    const float residualScale = GI_BASELINE_RESIDUAL_SCALE;
    float positive = saturate(max(residual.r, max(residual.g, residual.b)) / residualScale);
    float negative = saturate(-min(residual.r, min(residual.g, residual.b)) / residualScale);
    float3 displayResidual = 0.5f * (1.0f - max(positive, negative)) +
        float3(positive, 0.0f, negative) / (1.0f + min(positive, negative));
    giTexture[pixel] = float4(displayResidual, 1.0f);
#else
#error Unsupported GI_SAME_NORMAL_DISPLAY
#endif
#else
#if GI_BASELINE_RESIDUAL_DISPLAY == 0
    giTexture[pixel] = float4(baseline, 1.0f);
#elif GI_BASELINE_RESIDUAL_DISPLAY == 1
    giTexture[pixel] = float4(reference, 1.0f);
#elif GI_BASELINE_RESIDUAL_DISPLAY == 2
    // Fixed configurable scale reaches full red/blue. Mixed RGB signs
    // show purple; use the largest positive and negative channel without cancellation.
    const float residualScale = GI_BASELINE_RESIDUAL_SCALE;
    float positive = saturate(max(residual.r, max(residual.g, residual.b)) / residualScale);
    float negative = saturate(-min(residual.r, min(residual.g, residual.b)) / residualScale);
    float3 displayResidual = 0.5f * (1.0f - max(positive, negative)) +
        float3(positive, 0.0f, negative) / (1.0f + min(positive, negative));
    giTexture[pixel] = float4(displayResidual, 1.0f);
#else
#error Unsupported GI_BASELINE_RESIDUAL_DISPLAY
#endif
#endif
    // No TraceRay calls: retain the initialized hit-distance sentinel (-2).
    return;
#elif GI_DIAGNOSTIC_MODE == GI_MODE_PRODUCTION_CLAMP_BIAS_DEBUG
    // Compare both averaging orders using exactly the same paths, before NRD.
    const uint sampleCount = GI_PRODUCTION_CLAMP_SAMPLES;
    TextureCube<float4> diagnosticIrradiance = ResourceDescriptorHeap[diffuseIrradianceIndex];
    float3 diagnosticBaseline = diagnosticIrradiance.SampleLevel(materialSampler, normal, 0.0f).rgb;
    if(!all(isfinite(diagnosticBaseline)))
    {
#if !GI_PRODUCTION_CLAMP_NUMERIC_OUTPUT
        giTexture[pixel] = float4(1.0f, 0.0f, 1.0f, -1.0f);
#endif
        return;
    }

    float3 clampedSum = 0.0f;
    float3 signedSum = 0.0f;
    bool estimatesFinite = true;
    [loop]
    for(uint sample = 0u; sample < sampleCount; ++sample)
    {
        // Distinct indices within a frame; the existing secondary helper salts this again.
        uint sampleSeed = frameIndex * sampleCount + sample;
        RayDesc diagnosticRay;
        diagnosticRay.Origin = worldPosition.xyz + geometricNormal * 0.001f;
        diagnosticRay.Direction = SampleGICosineHemisphere(normal, pixel, sampleSeed);
        diagnosticRay.TMin = 0.001f;
        diagnosticRay.TMax = 60.0f;

        GIPayload diagnosticPayload = (GIPayload)0;
        diagnosticPayload.hitDistance = -2.0f;
        TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, diagnosticRay, diagnosticPayload);
        // Leave output hit distance at -2: diagnostic colors must not enter valid NRD GI input.

        float3 diagnosticEnvironment = SampleGIEnvironment(diagnosticRay.Direction, float(DIFFUSE_HDRI_SOURCE_MIP));
        float3 diagnosticTraced = 0.0f;
        if(diagnosticPayload.hitDistance > 0.0f)
        {
            float3 secondaryPosition = diagnosticRay.Origin +
                diagnosticRay.Direction * diagnosticPayload.hitDistance;
#if !GI_PRODUCTION_CLAMP_BLACK_SECONDARY
            diagnosticTraced = ShadeGISecondarySurface(
                diagnosticPayload, secondaryPosition, pixel, sampleSeed, float(DIFFUSE_HDRI_SOURCE_MIP));
#endif
        }
        else if(diagnosticPayload.hitDistance == -1.0f)
        {
            diagnosticTraced = diagnosticEnvironment;
        }

        float3 estimate = diagnosticBaseline + PI * (diagnosticTraced - diagnosticEnvironment);
        if(!all(isfinite(diagnosticEnvironment)) || !all(isfinite(diagnosticTraced)) ||
           !all(isfinite(estimate)))
        {
            estimatesFinite = false;
            continue; // Do not replace invalid paths with zeros or change the averaging denominator.
        }
        clampedSum += clamp(estimate, 0.0f, 65504.0f);
        signedSum += estimate;
    }

    if(!estimatesFinite || !all(isfinite(clampedSum)) || !all(isfinite(signedSum)))
    {
#if !GI_PRODUCTION_CLAMP_NUMERIC_OUTPUT
        giTexture[pixel] = float4(1.0f, 0.0f, 1.0f, -1.0f);
#endif
        return;
    }
    float3 averageClamped = clampedSum / float(sampleCount);
    float3 clampedAverage = clamp(signedSum / float(sampleCount), 0.0f, 65504.0f);
#if GI_PRODUCTION_CLAMP_NUMERIC_OUTPUT
    const float3 luminanceWeights = float3(0.2126f, 0.7152f, 0.0722f);
    giTexture[pixel] = float4(
        dot(averageClamped, luminanceWeights),
        dot(clampedAverage, luminanceWeights),
        dot(averageClamped - clampedAverage, luminanceWeights),
        1.0f);
#else
#if GI_PRODUCTION_CLAMP_DISPLAY == 0
    float3 selected = averageClamped;
#elif GI_PRODUCTION_CLAMP_DISPLAY == 1
    float3 selected = clampedAverage;
#elif GI_PRODUCTION_CLAMP_DISPLAY == 2
    float3 selected = averageClamped - clampedAverage;
#else
#error GI_PRODUCTION_CLAMP_DISPLAY must be 0, 1 or 2
#endif
    // Identical signed HDR encoding for all three views, scale = 1 irradiance unit.
    // Zero = 0.5 linear, +1 = 0.75, -1 = 0.25. No per-pixel normalization.
    // Alpha -1 marks pre-encoded diagnostic color for ToneMap's GI Raw path.
    float3 display = 0.5f + 0.5f * (selected / (1.0f + abs(selected)));
    giTexture[pixel] = float4(display, -1.0f);
#endif
    return;
#elif GI_DIAGNOSTIC_MODE == GI_MODE_CLAMP_BIAS_DEBUG
    // Compare both averaging orders using exactly the same paths, before NRD.
    const uint sampleCount = 16u;
    TextureCube<float4> diagnosticIrradiance = ResourceDescriptorHeap[diffuseIrradianceIndex];
    float3 diagnosticBaseline = diagnosticIrradiance.SampleLevel(materialSampler, normal, 0.0f).rgb;
    if(!all(isfinite(diagnosticBaseline)))
    {
        giTexture[pixel] = float4(1.0f, 0.0f, 1.0f, 1.0f);
        return;
    }

    float3 clampedSum = 0.0f;
    float3 signedSum = 0.0f;
    bool estimatesFinite = true;
    [loop]
    for(uint sample = 0u; sample < sampleCount; ++sample)
    {
        // Distinct indices within a frame; the existing secondary helper salts this again.
        uint sampleSeed = frameIndex * sampleCount + sample;
        RayDesc diagnosticRay;
        diagnosticRay.Origin = worldPosition.xyz + geometricNormal * 0.001f;
        diagnosticRay.Direction = SampleGICosineHemisphere(normal, pixel, sampleSeed);
        diagnosticRay.TMin = 0.001f;
        diagnosticRay.TMax = 10000.0f;

        GIPayload diagnosticPayload = (GIPayload)0;
        diagnosticPayload.hitDistance = -2.0f;
        TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, diagnosticRay, diagnosticPayload);
        // Keep a representative primary hit distance with the existing sentinel conventions.

        if(sample == 0u)
        {
            hitDistanceTexture[pixel] = diagnosticPayload.hitDistance;
        }

        float3 diagnosticEnvironment = SampleGIEnvironment(diagnosticRay.Direction);
        float3 diagnosticTraced = 0.0f;
        if(diagnosticPayload.hitDistance > 0.0f)
        {
            float3 secondaryPosition = diagnosticRay.Origin +
                diagnosticRay.Direction * diagnosticPayload.hitDistance;
            diagnosticTraced = ShadeGISecondarySurface(
                diagnosticPayload, secondaryPosition, pixel, sampleSeed);
        }
        else if(diagnosticPayload.hitDistance == -1.0f)
        {
            diagnosticTraced = diagnosticEnvironment;
        }

        float3 estimate = diagnosticBaseline + PI * (diagnosticTraced - diagnosticEnvironment);
        if(!all(isfinite(diagnosticEnvironment)) || !all(isfinite(diagnosticTraced)) ||
           !all(isfinite(estimate)))
        {
            estimatesFinite = false;
            continue; // Do not replace invalid paths with zeros or change the averaging denominator.
        }
        clampedSum += max(estimate, 0.0f);
        signedSum += estimate;
    }

    if(!estimatesFinite || !all(isfinite(clampedSum)) || !all(isfinite(signedSum)))
    {
        giTexture[pixel] = float4(1.0f, 0.0f, 1.0f, 1.0f);
        return;
    }
    float3 averageClamped = clampedSum / float(sampleCount);
    float3 clampedAverage = max(signedSum / float(sampleCount), 0.0f);
#if GI_CLAMP_AB_DISPLAY == 0
    giTexture[pixel] = float4(averageClamped, 1.0f);
#elif GI_CLAMP_AB_DISPLAY == 1
    giTexture[pixel] = float4(clampedAverage, 1.0f);
#elif GI_CLAMP_AB_DISPLAY == 2
    float3 clampBias = max(averageClamped - clampedAverage, 0.0f);
    float3 displayBias = clampBias / (1.0f + clampBias);
    giTexture[pixel] = float4(displayBias, 1.0f);
#else
#error Unsupported GI_CLAMP_AB_DISPLAY
#endif
    return;
#else
    RayDesc ray;
    ray.Origin = worldPosition.xyz + geometricNormal * 0.001f;
    ray.Direction = SampleGICosineHemisphere(normal, pixel, frameIndex);
    ray.TMin = 0.001f;
#if GI_DIAGNOSTIC_MODE == GI_MODE_BASELINE_CORRECTION
    ray.TMax = 60.0f;
#else
    ray.TMax = 10000.0f;
#endif

    GIPayload payload = (GIPayload)0;
    payload.hitDistance = -2.0f;

    // Keep tracing in ray generation so later iterative bounces need no recursion.
    TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

    hitDistanceTexture[pixel] = payload.hitDistance;
#if GI_DIAGNOSTIC_MODE == GI_MODE_BASELINE_CORRECTION || \
    GI_DIAGNOSTIC_MODE == GI_MODE_PRIMARY_BASELINE_ONLY || \
    GI_DIAGNOSTIC_MODE == GI_MODE_SIGNED_CORRECTION_DEBUG || \
    GI_DIAGNOSTIC_MODE == GI_MODE_PRECLAMP_NEGATIVE_DEBUG
    TextureCube<float4> diffuseIrradiance = ResourceDescriptorHeap[diffuseIrradianceIndex];
    float3 baselineIrradiance = diffuseIrradiance.SampleLevel(materialSampler, normal, 0.0f).rgb;
#if GI_DIAGNOSTIC_MODE == GI_MODE_BASELINE_CORRECTION
    const float diffuseSourceMip = float(DIFFUSE_HDRI_SOURCE_MIP);
#else
    // Preserve diagnostic radiance sampling conventions (mip 0).
    const float diffuseSourceMip = 0.0f;
#endif
    float3 environmentRadiance = SampleGIEnvironment(ray.Direction, diffuseSourceMip);
    float3 tracedRadiance = 0.0f;
    if(payload.hitDistance > 0.0f)
    {
        float3 secondaryPosition = ray.Origin + ray.Direction * payload.hitDistance;
        tracedRadiance = ShadeGISecondarySurface(payload, secondaryPosition, pixel, frameIndex, diffuseSourceMip);
    }
    else if(payload.hitDistance == -1.0f)
    {
        tracedRadiance = environmentRadiance; // Identical finite values give exactly zero correction.
    }

#if GI_DIAGNOSTIC_MODE == GI_MODE_PRIMARY_BASELINE_ONLY
    float3 irradiance = baselineIrradiance;
#elif GI_DIAGNOSTIC_MODE == GI_MODE_SIGNED_CORRECTION_DEBUG
    float3 correction = PI * (tracedRadiance - environmentRadiance);
    const float correctionDisplayScale = 1.0f;
    // Neutral gray also marks nonfinite corrections; invalid primary pixels remain black.
    float3 encodedCorrection = float3(0.5f, 0.5f, 0.5f);

    if(all(isfinite(correction)))
    {
        encodedCorrection = 0.5f + 0.5f *
            (correction / (correctionDisplayScale + abs(correction)));
    }

    // Display encoding only: inspect in GI Raw, not as irradiance or through NRD.
    giTexture[pixel] = float4(encodedCorrection, 1.0f);
    return;
#else
    // Keep the correction signed until it has been added to the irradiance baseline.
    float3 irradiance = baselineIrradiance + PI * (tracedRadiance - environmentRadiance);
#if GI_DIAGNOSTIC_MODE == GI_MODE_BASELINE_CORRECTION
    if(signedCorrectionIndex != 0xFFFFFFFFu)
    {
        float3 signedCorrection = PI * (tracedRadiance - environmentRadiance);
        if((payload.hitDistance > 0.0f || payload.hitDistance == -1.0f) &&
           all(isfinite(baselineIrradiance)) && all(isfinite(environmentRadiance)) &&
           all(isfinite(tracedRadiance)) && all(isfinite(irradiance)) && all(isfinite(signedCorrection)))
        {
            RWTexture2D<float4> signedCorrectionTexture = ResourceDescriptorHeap[signedCorrectionIndex];
            signedCorrectionTexture[pixel] = float4(signedCorrection, 1.0f);
        }
    }
#endif
#if GI_DIAGNOSTIC_MODE == GI_MODE_PRECLAMP_NEGATIVE_DEBUG
    float3 visualization = float3(1.0f, 0.0f, 1.0f); // Nonfinite estimate.
    if(all(isfinite(irradiance)))
    {
        const float negativeDisplayScale = 1.0f;
        float3 negativeMagnitude = max(-irradiance, 0.0f);
        visualization = negativeMagnitude / (negativeDisplayScale + negativeMagnitude);
    }
    // Display encoding only: inspect in GI Raw. Invalid primary pixels remain black.
    giTexture[pixel] = float4(visualization, 1.0f);
    return;
#endif
#endif
#else
    float3 incomingRadiance;
#if GI_DIAGNOSTIC_MODE == GI_MODE_ENVIRONMENT_REFERENCE_4096
    incomingRadiance = SampleGIEnvironmentReference4096(normal);
#elif GI_DIAGNOSTIC_MODE == GI_MODE_ENVIRONMENT_ONLY
    incomingRadiance = SampleGIEnvironment(ray.Direction);
#elif GI_DIAGNOSTIC_MODE == GI_MODE_SECONDARY_SURFACE
        incomingRadiance = 0.0f;
        if(payload.hitDistance > 0.0f)
        {
            float3 secondaryPosition = ray.Origin + ray.Direction * payload.hitDistance;
            incomingRadiance = ShadeGISecondarySurface(payload, secondaryPosition, pixel, frameIndex);
        }
        else if(payload.hitDistance == -1.0f)
        {
            incomingRadiance = SampleGIEnvironment(ray.Direction);
        }

#elif GI_DIAGNOSTIC_MODE == GI_MODE_SECONDARY_ENVIRONMENT_ONLY
    incomingRadiance = 0.0f;
    if(payload.hitDistance > 0.0f)
    {
        float3 secondaryPosition = ray.Origin + ray.Direction * payload.hitDistance;
        incomingRadiance = ShadeGISecondaryEnvironmentOnly(payload, secondaryPosition);
    }
    else if(payload.hitDistance == -1.0f)
    {
        incomingRadiance = SampleGIEnvironment(ray.Direction);
    }

#elif GI_DIAGNOSTIC_MODE == GI_MODE_WHITE_DIFFUSE_SECONDARY
    incomingRadiance = 0.0f;

    if(payload.hitDistance > 0.0f)
    {
        incomingRadiance = ShadeGIWhiteDiffuseSecondary(payload.shadingNormal);
    }
    else if(payload.hitDistance == -1.0f)
    {
        incomingRadiance = SampleGIEnvironment(ray.Direction);
    }

#else
#error Unsupported GI_DIAGNOSTIC_MODE
#endif

    // cos(theta) / PDF = PI; reference samples are averaged before this conversion.
    float3 irradiance = incomingRadiance * PI;
#endif
#if GI_DIAGNOSTIC_MODE != GI_MODE_SIGNED_CORRECTION_DEBUG && \
    GI_DIAGNOSTIC_MODE != GI_MODE_PRECLAMP_NEGATIVE_DEBUG

    if(all(isfinite(irradiance)))
    {
        giTexture[pixel] = float4(clamp(irradiance, 0.0f, 65504.0f), 1.0f);
    }

#endif
#endif // GI_MODE_CLAMP_BIAS_DEBUG
}

[shader("miss")]
void GIVisibilityMiss(inout GIVisibilityPayload payload)
{
    payload.visible = 1u;
}

[shader("miss")]
void GIMiss(inout GIPayload payload)
{
    payload.hitDistance = -1.0f;
}

[shader("closesthit")]
void GIClosestHit(
    inout GIPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    payload.hitDistance =
        RayTCurrent();

    StructuredBuffer<RayTracingInstanceData>
        instanceDataBuffer =
            ResourceDescriptorHeap[
                instanceDataIndex
            ];

    RayTracingInstanceData instanceData =
        instanceDataBuffer[
            InstanceID()
        ];

    ByteAddressBuffer vertexBuffer =
        ResourceDescriptorHeap[
            NonUniformResourceIndex(
                instanceData.vertexBufferIndex
            )
        ];

    uint vertexIndex0;
    uint vertexIndex1;
    uint vertexIndex2;

    GetTriangleVertexIndices(
        instanceData,
        PrimitiveIndex(),
        vertexIndex0,
        vertexIndex1,
        vertexIndex2
    );

    float barycentric0 =
        1.0f -
        attributes.barycentrics.x -
        attributes.barycentrics.y;

    float barycentric1 =
        attributes.barycentrics.x;

    float barycentric2 =
        attributes.barycentrics.y;

    float2 uv0 =
        LoadVertexUV(
            vertexBuffer,
            vertexIndex0,
            instanceData.vertexStride,
            instanceData.uvOffset
        );

    float2 uv1 =
        LoadVertexUV(
            vertexBuffer,
            vertexIndex1,
            instanceData.vertexStride,
            instanceData.uvOffset
        );

    float2 uv2 =
        LoadVertexUV(
            vertexBuffer,
            vertexIndex2,
            instanceData.vertexStride,
            instanceData.uvOffset
        );

    float2 hitUV =
        uv0 * barycentric0 +
        uv1 * barycentric1 +
        uv2 * barycentric2;

    float3 normal0 =
        LoadVertexNormal(
            vertexBuffer,
            vertexIndex0,
            instanceData.vertexStride,
            instanceData.normalOffset
        );

    float3 normal1 =
        LoadVertexNormal(
            vertexBuffer,
            vertexIndex1,
            instanceData.vertexStride,
            instanceData.normalOffset
        );

    float3 normal2 =
        LoadVertexNormal(
            vertexBuffer,
            vertexIndex2,
            instanceData.vertexStride,
            instanceData.normalOffset
        );

    float3 objectNormal =
        normalize(
            normal0 * barycentric0 +
            normal1 * barycentric1 +
            normal2 * barycentric2
        );

    float3 worldNormal =
        normalize(mul(objectNormal, (float3x3)WorldToObject3x4()));

    float3 shadingNormal = worldNormal;
    StructuredBuffer<GPUMaterial> materialBuffer =
        ResourceDescriptorHeap[materialBufferIndex];
    GPUMaterial material = materialBuffer[instanceData.materialIndex];
    if(material.normalTextureIndex != 0xFFFFFFFFu && instanceData.tangentOffset != 0xFFFFFFFFu)
    {
        float4 objectTangent =
            LoadVertexTangent(vertexBuffer, vertexIndex0, instanceData.vertexStride, instanceData.tangentOffset) * barycentric0 +
            LoadVertexTangent(vertexBuffer, vertexIndex1, instanceData.vertexStride, instanceData.tangentOffset) * barycentric1 +
            LoadVertexTangent(vertexBuffer, vertexIndex2, instanceData.vertexStride, instanceData.tangentOffset) * barycentric2;
        // Row-vector multiplication by world-to-object equals the raster
        // inverse-transpose normalMatrix convention.
        float3 tangent = mul(objectTangent.xyz, (float3x3)WorldToObject3x4());
        tangent -= worldNormal * dot(worldNormal, tangent);
        float tangentLengthSquared = dot(tangent, tangent);
        // A reflected instance reverses the cross-product frame orientation.
        float instanceDeterminant = determinant((float3x3)ObjectToWorld3x4());
        float handedness = objectTangent.w * (instanceDeterminant < 0.0f ? -1.0f : 1.0f);
        Texture2D<float4> normalTexture =
            ResourceDescriptorHeap[NonUniformResourceIndex(material.normalTextureIndex)];
        float3 sampledNormal = normalTexture.SampleLevel(materialSampler, hitUV, 0.0f).rgb * 2.0f - 1.0f;
        float sampledLengthSquared = dot(sampledNormal, sampledNormal);
        if(all(isfinite(worldNormal)) && all(isfinite(tangent)) &&
           isfinite(tangentLengthSquared) && tangentLengthSquared > 1.0e-8f &&
           all(isfinite(sampledNormal)) && isfinite(sampledLengthSquared) &&
           sampledLengthSquared > 1.0e-8f && isfinite(handedness) && abs(handedness) > 1.0e-8f &&
           isfinite(instanceDeterminant) && instanceDeterminant != 0.0f)
        {
            float3 T = normalize(tangent);
            float3 B = cross(worldNormal, T) * handedness;
            float3 worldMappedNormal = T * sampledNormal.x + B * sampledNormal.y + worldNormal * sampledNormal.z;
            float mappedLengthSquared = dot(worldMappedNormal, worldMappedNormal);

            if(all(isfinite(B)) && all(isfinite(worldMappedNormal)) &&
               isfinite(mappedLengthSquared) && mappedLengthSquared > 1.0e-8f)
            {
                shadingNormal = normalize(worldMappedNormal);
            }
        }
    }

    payload.worldNormal = worldNormal;
    payload.shadingNormal = shadingNormal;
    payload.uv = hitUV;
    payload.materialIndex = instanceData.materialIndex;
}
