#include "MaterialLighting.hlsli"

// Production defaults to baseline correction; retain legacy and diagnostic selections for A/B tests.
#define GI_MODE_LEGACY_SECONDARY_SURFACE 0
#define GI_MODE_SECONDARY_SURFACE GI_MODE_LEGACY_SECONDARY_SURFACE
#define GI_MODE_ENVIRONMENT_ONLY 1
#define GI_MODE_ENVIRONMENT_REFERENCE_4096 2
#define GI_MODE_SECONDARY_ENVIRONMENT_ONLY 3
#define GI_MODE_WHITE_DIFFUSE_SECONDARY 4
#define GI_MODE_BASELINE_CORRECTION 5
#define GI_MODE_PRODUCTION GI_MODE_BASELINE_CORRECTION
#ifndef GI_DIAGNOSTIC_MODE
#define GI_DIAGNOSTIC_MODE GI_MODE_PRODUCTION
#endif

struct GIPayload
{
    float hitDistance;
    float3 worldNormal;
    float2 uv;
    uint materialIndex;
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
    uint padding;
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
    uint padding1;
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

float3 SampleGIEnvironment(float3 rayDirection)
{
    Texture2D<float4> environmentTexture = ResourceDescriptorHeap[environmentIndex];
    float2 environmentUV = DirectionToEquirectangularUV(normalize(rayDirection));
    return environmentTexture.SampleLevel(environmentSampler, environmentUV, 0.0f).rgb;
}

// Called from raygen only, after the surface trace has returned.
// Diagnostic counterpart: same material/environment evaluation, no directional lighting or rays.
float3 ShadeGIWhiteDiffuseSecondary(float3 secondaryNormal)
{
    float normalLengthSquared = dot(secondaryNormal, secondaryNormal);
    if(!all(isfinite(secondaryNormal)) || !isfinite(normalLengthSquared) || normalLengthSquared < 1.0e-8f)
        return 0.0f;

    float3 normal = normalize(secondaryNormal);
    TextureCube<float4> diffuseIrradiance = ResourceDescriptorHeap[diffuseIrradianceIndex];
    float3 environmentIrradiance = diffuseIrradiance.SampleLevel(materialSampler, normal, 0.0f).rgb;
    // Unit Lambertian reflectance: no secondary material lookup or weighting.
    return environmentIrradiance / PI;
}

float3 ShadeGISecondaryEnvironmentOnly(GIPayload surface, float3 secondaryPosition)
{
    float normalLengthSquared = dot(surface.worldNormal, surface.worldNormal);
    if(!all(isfinite(secondaryPosition)) || !all(isfinite(surface.worldNormal)) ||
       !all(isfinite(surface.uv)) || !isfinite(normalLengthSquared) || normalLengthSquared < 1.0e-8f)
        return 0.0f;

    float3 normal = normalize(surface.worldNormal);

    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[materialBufferIndex];
    uint materialCount, materialStride;
    materials.GetDimensions(materialCount, materialStride);
    if(surface.materialIndex >= materialCount)
        return 0.0f;
    GPUMaterial material = materials[surface.materialIndex];
    // Model 0 is metallic-roughness, model 1 dielectric; conductors have no diffuse lobe.
    if(material.model > 1u || !all(isfinite(material.baseColor)) ||
       (material.model == 0u && !isfinite(material.metallic)))
        return 0.0f;

    Texture2D<float4> materialTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.textureIndex)];
    float3 sampledColor = materialTexture.SampleLevel(materialSampler, surface.uv, 0.0f).rgb;
    if(!all(isfinite(sampledColor)))
        return 0.0f;
    float3 diffuseColor = max(material.baseColor * sampledColor, 0.0f);
    if(material.model == 0u)
        diffuseColor *= 1.0f - saturate(material.metallic);
    if(!all(isfinite(diffuseColor)) || !any(diffuseColor > 0.0f))
        return 0.0f;

    TextureCube<float4> diffuseIrradiance = ResourceDescriptorHeap[diffuseIrradianceIndex];
    float3 environmentIrradiance = diffuseIrradiance.SampleLevel(materialSampler, normal, 0.0f).rgb;
    float3 environmentRadiance = (diffuseColor / PI) * environmentIrradiance;
    if(!all(isfinite(environmentRadiance)))
        environmentRadiance = 0.0f;

    return environmentRadiance;
}

float3 ShadeGISecondarySurface(GIPayload surface, float3 secondaryPosition)
{
    float normalLengthSquared = dot(surface.worldNormal, surface.worldNormal);
    if(!all(isfinite(secondaryPosition)) || !all(isfinite(surface.worldNormal)) ||
       !all(isfinite(surface.uv)) || !isfinite(normalLengthSquared) || normalLengthSquared < 1.0e-8f)
        return 0.0f;

    float3 normal = normalize(surface.worldNormal);

    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[materialBufferIndex];
    uint materialCount, materialStride;
    materials.GetDimensions(materialCount, materialStride);
    if(surface.materialIndex >= materialCount)
        return 0.0f;
    GPUMaterial material = materials[surface.materialIndex];
    // Model 0 is metallic-roughness, model 1 dielectric; conductors have no diffuse lobe.
    if(material.model > 1u || !all(isfinite(material.baseColor)) ||
       (material.model == 0u && !isfinite(material.metallic)))
        return 0.0f;

    Texture2D<float4> materialTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.textureIndex)];
    float3 sampledColor = materialTexture.SampleLevel(materialSampler, surface.uv, 0.0f).rgb;
    if(!all(isfinite(sampledColor)))
        return 0.0f;
    float3 diffuseColor = max(material.baseColor * sampledColor, 0.0f);
    if(material.model == 0u)
        diffuseColor *= 1.0f - saturate(material.metallic);
    if(!all(isfinite(diffuseColor)) || !any(diffuseColor > 0.0f))
        return 0.0f;

    TextureCube<float4> diffuseIrradiance = ResourceDescriptorHeap[diffuseIrradianceIndex];
    float3 environmentIrradiance = diffuseIrradiance.SampleLevel(materialSampler, normal, 0.0f).rgb;
    float3 environmentRadiance = (diffuseColor / PI) * environmentIrradiance;
    if(!all(isfinite(environmentRadiance)))
        environmentRadiance = 0.0f;

    // Invalid or back-facing directional lighting must not suppress environment diffuse.
    float lightLengthSquared = dot(lightConstants.lightDirection, lightConstants.lightDirection);
    if(!all(isfinite(lightConstants.lightDirection)) || !isfinite(lightLengthSquared) || lightLengthSquared < 1.0e-8f ||
       !all(isfinite(lightConstants.lightColor)) || !isfinite(lightConstants.lightIntensity) ||
       lightConstants.lightIntensity <= 0.0f)
        return environmentRadiance;
    float3 directionToLight = normalize(-lightConstants.lightDirection);
    float NdotL = saturate(dot(normal, directionToLight));
    if(NdotL <= 0.0f)
        return environmentRadiance;

    RayDesc visibilityRay;
    visibilityRay.Origin = secondaryPosition + normal * 0.001f;
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

[shader("raygeneration")]
void GIRayGeneration()
{
    const uint2 pixel = DispatchRaysIndex().xy;

    giTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
    hitDistanceTexture[pixel] = -2.0f;

    Texture2D<float> depthTexture = ResourceDescriptorHeap[depthIndex];
    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[surfaceIndex];

    const float depth = depthTexture.Load(int3(pixel, 0));
    if(!isfinite(depth) || depth < 0.0f || depth >= 1.0f)
        return;

    // Match reflection/shadow reconstruction: pixel centers, flipped NDC Y,
    // D3D depth in [0, 1], and a row-major matrix multiplying a column vector.
    const float2 uv = (float2(pixel) + 0.5f) / float2(DispatchRaysDimensions().xy);
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 worldPosition = mul(inverseViewProjection, float4(ndc, depth, 1.0f));
    if(!all(isfinite(worldPosition)) || abs(worldPosition.w) < 1.0e-8f)
        return;

    worldPosition /= worldPosition.w;
    if(!all(isfinite(worldPosition)))
        return;

    // Surface RGB already stores a world-space normal, without remapping.
    const float3 surfaceNormal = surfaceTexture.Load(int3(pixel, 0)).rgb;
    const float normalLengthSquared = dot(surfaceNormal, surfaceNormal);
    if(!all(isfinite(surfaceNormal)) || !isfinite(normalLengthSquared) ||
       normalLengthSquared < 1.0e-8f)
        return;

    const float3 normal = normalize(surfaceNormal);

    RayDesc ray;
    ray.Origin = worldPosition.xyz + normal * 0.001f;
    ray.Direction = SampleGICosineHemisphere(normal, pixel, frameIndex);
    ray.TMin = 0.001f;
    ray.TMax = 10000.0f;

    GIPayload payload = (GIPayload)0;
    payload.hitDistance = -2.0f;

    // Keep tracing in ray generation so later iterative bounces need no recursion.
    TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

    hitDistanceTexture[pixel] = payload.hitDistance;
#if GI_DIAGNOSTIC_MODE == GI_MODE_BASELINE_CORRECTION
    TextureCube<float4> diffuseIrradiance = ResourceDescriptorHeap[diffuseIrradianceIndex];
    float3 baselineIrradiance = diffuseIrradiance.SampleLevel(materialSampler, normal, 0.0f).rgb;
    float3 environmentRadiance = SampleGIEnvironment(ray.Direction);
    float3 tracedRadiance = 0.0f;
    if(payload.hitDistance > 0.0f)
    {
        float3 secondaryPosition = ray.Origin + ray.Direction * payload.hitDistance;
        tracedRadiance = ShadeGISecondarySurface(payload, secondaryPosition);
    }
    else if(payload.hitDistance == -1.0f)
        tracedRadiance = environmentRadiance; // Identical finite values give exactly zero correction.

    // Keep the correction signed until it has been added to the irradiance baseline.
    float3 irradiance = baselineIrradiance + PI * (tracedRadiance - environmentRadiance);
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
            incomingRadiance = ShadeGISecondarySurface(payload, secondaryPosition);
        }
        else if(payload.hitDistance == -1.0f)
            incomingRadiance = SampleGIEnvironment(ray.Direction);
#elif GI_DIAGNOSTIC_MODE == GI_MODE_SECONDARY_ENVIRONMENT_ONLY
    incomingRadiance = 0.0f;
    if(payload.hitDistance > 0.0f)
    {
        float3 secondaryPosition = ray.Origin + ray.Direction * payload.hitDistance;
        incomingRadiance = ShadeGISecondaryEnvironmentOnly(payload, secondaryPosition);
    }
    else if(payload.hitDistance == -1.0f)
        incomingRadiance = SampleGIEnvironment(ray.Direction);
#elif GI_DIAGNOSTIC_MODE == GI_MODE_WHITE_DIFFUSE_SECONDARY
    incomingRadiance = 0.0f;
    if(payload.hitDistance > 0.0f)
        incomingRadiance = ShadeGIWhiteDiffuseSecondary(payload.worldNormal);
    else if(payload.hitDistance == -1.0f)
        incomingRadiance = SampleGIEnvironment(ray.Direction);
#else
#error Unsupported GI_DIAGNOSTIC_MODE
#endif

    // cos(theta) / PDF = PI; reference samples are averaged before this conversion.
    float3 irradiance = incomingRadiance * PI;
#endif
    if(all(isfinite(irradiance)))
        giTexture[pixel] = float4(clamp(irradiance, 0.0f, 65504.0f), 1.0f);
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

    payload.worldNormal = worldNormal;
    payload.uv = hitUV;
    payload.materialIndex = instanceData.materialIndex;
}
