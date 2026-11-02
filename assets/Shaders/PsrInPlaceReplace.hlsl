// PSR.2 Stage 2 in-place replacement prototype (development only; PsrInPlaceReplacePass). One fullscreen triangle after
// the geometry pass on Path Tracing frames. For each pixel whose identity carries the PSR eligibility flag
// (PsrIdentity.hlsli), the original mirror is recovered from the identity alone (instance, triangle, camera-ray
// intersection, normals, UV, material), its reflected ray is traced with inline RayQuery exactly as PSR.1's record pass
// traces it, and the pixel's G-buffer is overwritten in place with the reflected surface in virtual camera space.
// Every other pixel, and every eligible pixel that falls back, is discarded and keeps its contents.
//
// The reflected ray reproduces PSR.1 (PathTracePsrRecordRayGeneration): the mirror's G-buffer values are emulated as
// the geometry pass stores them (PsrEligibility.hlsli's storage emulation) and decoded by the shared
// PathTraceReconstructPrimaryVertex, so the lifted origin, the finalized normals and the delta direction are PSR.1's.
// The mirror's normal map is sampled as Triangle.hlsl samples it (trilinear clamp, s0), with the UV derivatives of the
// pixel's 2x2 quad computed analytically from the recovered triangle instead of by the rasterizer.
//
// G-buffer of a replaced pixel (Triangle.hlsl's encodings, the reflected surface B mirrored into the virtual image Bv):
//   material        B's base colour and roughness, PSR.1's PathTraceBuildSurface (texture LOD 0, wrap, s1)
//   surface         reflect(B's shading normal facing the reflected ray), -(B's material index + 1)
//   geometricNormal reflect(B's face normal facing the reflected ray), B's metallic. The face normal, not the raster's
//                   interpolated normal, because PSR.1's vertex 1 is built from it.
//   motion          currentUV - previousUV of Bv (unjittered), previous view depth - current view depth, AO 1 (PSR.1
//                   does not apply material AO). The previous Bv is B moved by its instance's current-to-previous
//                   transform, reflected in the mirror plane moved by the mirror's.
//   depth           Bv through the geometry pass's (jittered) projection
// Fallbacks (discard, the mirror stays): the reflected ray misses (sky; no convention yet), the hit is not a valid
// surface or is itself a pure-delta mirror (PSR.1's fallback states), its material index is not representable as
// -(i + 1) in fp16, or Bv is not strictly inside the finite depth range.
// The identity target is not bound and keeps the mirror's identity.
//
// Multibounce PSR (PsrInPlaceReplaceK2.hlsl, PSR_MAX_CHAIN 2): a reflected hit that is a pure-delta mirror B is followed
// by one more delta reflection to the terminal surface D. The pixel then holds Dv = R_A(R_B(D)) with D's material and
// normals reflected through B and A, motion from D moved by its instance and reflected in B's and A's moved planes, and
// motion.w 2 (the chain length; the lighting passes retrace B and verify D against these guides). Every other fallback is
// unchanged, and a second mirror, a mixed-smooth B or a miss after B falls back.

#ifndef PSR_MAX_CHAIN
#define PSR_MAX_CHAIN 1
#endif

#include "PathTracePrimarySurface.hlsli"
#include "PsrEligibility.hlsli"
#include "RayTracingInstanceData.hlsli"

cbuffer PsrInPlaceConstants : register(b0)
{
    // The geometry pass's render (jittered) view-projection without the view translation, and its inverse.
    row_major float4x4 cameraRelativeViewProjection;
    row_major float4x4 inverseCameraRelativeViewProjection;
    // Triangle.hlsl's motion: unjittered current and previous view-projection (world space), and row 2 of the current
    // and previous views (view depth).
    row_major float4x4 unjitteredViewProjection;
    row_major float4x4 previousViewProjection;
    float4 viewDepthRow;
    float4 previousViewDepthRow;
    float3 cameraPosition;
    uint identityIndex;
    uint instanceDataIndex;
    uint transformCount;
    uint2 resolution;
    uint materialBufferIndex;
    // PSR_DIAGNOSTIC_HITS builds only: the raw UAV of the per-pixel diagnostic hit records, and the raw UAV of PSR.1's
    // records (their reflected-hit UV, for sampling at PSR.1's UV).
    uint diagnosticHitsIndex;
    uint psr1RecordsIndex;
    // The geometry pass's texture mip bias (PsrRecoverMirror).
    float textureMipBias;
};

RaytracingAccelerationStructure scene : register(t1);
// Triangle.hlsl's material sampler (the mirror's normal map) and PSR.1's (the reflected material).
SamplerState mirrorSampler : register(s0);
SamplerState materialSampler : register(s1);

// The reflected ray's origin (compile-time). The pass uses 2; the diagnostic variants under PsrDiagnostics/ keep 0 and 1
// for the origin study (PsrRayQueryVerification's ENGINE_PSR2S2_LIFTSTUDY):
//   0  PSR.1's: the decoded mirror vertex, lifted by PATH_TRACE_PRIMARY_SURFACE_LIFT
//   1  the decoded mirror vertex without the lift
//   2  the exact recovered intersection (camera ray against the mirror triangle), without the lift. The lift covers the
//      raster depth reconstruction's error, which the recovered point does not have; PathTraceOffsetRayOrigin's
//      ulp-scaled offset alone kept every studied case (grazing, 0.01x and 100x scale, transformed and thin geometry,
//      contact) free of self-hits and missed hits, with the hit 15x closer to the exact reflection.
// Each is moved off the surface by PathTraceOffsetRayOrigin; the virtual image is reflected through the unlifted point.
#ifndef PSR_REPLACEMENT_ORIGIN
#define PSR_REPLACEMENT_ORIGIN 2
#endif

// Development diagnostics: per eligible pixel, a 128-byte record of the outcome, the committed hit and the material
// sampling stages (PsrStoreDiagnostic). Off in the production pass.
#ifndef PSR_DIAGNOSTIC_HITS
#define PSR_DIAGNOSTIC_HITS 0
#endif

// The material sampling stages of the committed hit (PSR_DIAGNOSTIC_HITS): written by PsrClosestHit and
// PathTraceBuildSurface, read by PsrStoreDiagnostic.
struct PsrSamplingDiagnostics
{
    float2 rawNormal;
    float3 tangentNormal;
    float3 worldNormal;
    float3 tangentDirection;
    float handedness;
    uint normalTextureIndex;
    float3 rawBaseColor;
    float rawRoughness;
    float rawMetallic;
    float roughness;
    float metallic;
    uint4 mirrorHalves;
};

static PsrSamplingDiagnostics g_psrSampling = (PsrSamplingDiagnostics)0;

// The largest material index whose -(i + 1) fp16 stores exactly (PATH_TRACE_PRIMARY_MAXIMUM_MATERIALS).
static const uint PSR_MAXIMUM_REPLACEMENT_MATERIAL = 2047u;
// PSR.1's reflected-ray TMax (PathTraceLights.hlsli's PATH_TRACE_DISTANT_LIGHT_DISTANCE).
static const float PSR_REFLECTED_RAY_MAXIMUM_DISTANCE = 1.0e27f;

struct VertexOutput
{
    float4 position : SV_Position;
};

struct PixelOutput
{
    float4 material : SV_Target0;
    float4 surface : SV_Target1;
    float4 motion : SV_Target2;
    float4 geometricNormal : SV_Target3;
    float depth : SV_Depth;
};

VertexOutput VSMain(uint vertexID : SV_VertexID)
{
    // Vertices (-1, 1), (3, 1), (-1, -3) cover the viewport with one triangle.
    const float2 corner = float2((vertexID << 1) & 2, vertexID & 2);
    VertexOutput output;
    output.position = float4(corner * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

// -----------------------------------------------------------------------------
// Verbatim copies of PathTraceReference.hlsl (PathTraceOffsetRayOrigin, the vertex loads, PathTraceClosestHit's
// evaluation and PathTraceBuildSurface): factoring them into a shared header changed every production library's DXIL.
// The PSR.2 Stage 2 gates compare their results with PSR.1's records.
// -----------------------------------------------------------------------------

float3 PathTraceOffsetRayOrigin(float3 position, float3 normal)
{
    const float originThreshold = 1.0f / 32.0f;
    const float floatScale = 1.0f / 65536.0f;
    const float integerScale = 256.0f;

    int3 integerOffset = int3(integerScale * normal);
    float3 integerOffsetPosition = asfloat(asint(position) + select(position < 0.0f, -integerOffset, integerOffset));
    return select(abs(position) < originThreshold, position + floatScale * normal, integerOffsetPosition);
}

float3 PathTraceLoadPosition(ByteAddressBuffer vertexBuffer, uint vertexIndex, uint vertexStride)
{
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

// The mirror recovery and the virtual image's reflections (PSR_MIRROR_HALF_ROUNDING), on the loads above.
#include "PsrMirrorRecovery.hlsli"

StructuredBuffer<PsrObjectTransform> objectTransforms : register(t0);

struct PathTracePayload
{
    float hitDistance;
    float3 geometricNormal;
    float3 shadingNormal;
    float2 uv;
    uint materialIndex;
};

// PathTraceClosestHit with the committed RayQuery hit in place of the closest-hit intrinsics.
PathTracePayload PsrClosestHit(RayQuery<RAY_FLAG_NONE> query)
{
    StructuredBuffer<RayTracingInstanceData> instances = ResourceDescriptorHeap[instanceDataIndex];
    RayTracingInstanceData instance = instances[query.CommittedInstanceID()];
    instance.materialIndex = ResolveHitMaterial(instance, query.CommittedPrimitiveIndex());

    ByteAddressBuffer vertexBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(instance.vertexBufferIndex)];
    uint3 indices = PathTraceTriangleIndices(instance, query.CommittedPrimitiveIndex());
    float2 attributes = query.CommittedTriangleBarycentrics();
    float3 barycentrics = float3(1.0f - attributes.x - attributes.y, attributes.x, attributes.y);

    // Normals use the inverse transpose of object-to-world (row vector times world-to-object).
    float3x3 normalTransform = (float3x3)query.CommittedWorldToObject3x4();
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
        geometricNormal = -query.WorldRayDirection();
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
        float3x3 objectToWorld = (float3x3)query.CommittedObjectToWorld3x4();
        float4 objectTangent = PathTraceLoadTangent(vertexBuffer, indices.x, instance) * barycentrics.x +
            PathTraceLoadTangent(vertexBuffer, indices.y, instance) * barycentrics.y +
            PathTraceLoadTangent(vertexBuffer, indices.z, instance) * barycentrics.z;
        float3 tangent = mul(objectToWorld, objectTangent.xyz);
        tangent -= worldNormal * dot(worldNormal, tangent);
        float tangentLengthSquared = dot(tangent, tangent);
        float instanceDeterminant = determinant(objectToWorld);
        float handedness = objectTangent.w * (instanceDeterminant < 0.0f ? -1.0f : 1.0f);

        Texture2D<float4> normalTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.normalTextureIndex)];
        float2 normalXY = normalTexture.SampleLevel(materialSampler, uv, 0.0f).rg * 2.0f - 1.0f;
        float3 sampledNormal = float3(normalXY, sqrt(saturate(1.0f - dot(normalXY, normalXY))));
#if PSR_DIAGNOSTIC_HITS
        g_psrSampling.rawNormal = normalTexture.SampleLevel(materialSampler, uv, 0.0f).rg;
        g_psrSampling.normalTextureIndex = material.normalTextureIndex;
        g_psrSampling.worldNormal = worldNormal;
        g_psrSampling.handedness = handedness;
#endif
        float sampledLengthSquared = dot(sampledNormal, sampledNormal);

        bool frameValid = all(isfinite(tangent)) && isfinite(tangentLengthSquared) && tangentLengthSquared > 1.0e-8f &&
            all(isfinite(sampledNormal)) && sampledLengthSquared > 1.0e-8f && isfinite(handedness) &&
            abs(handedness) > 1.0e-8f && isfinite(instanceDeterminant) && instanceDeterminant != 0.0f;

        if(frameValid)
        {
            sampledNormal = normalize(sampledNormal);
            float3 tangentDirection = normalize(tangent);
#if PSR_DIAGNOSTIC_HITS
            g_psrSampling.tangentNormal = sampledNormal;
            g_psrSampling.tangentDirection = tangentDirection;
#endif
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

    PathTracePayload payload;
    payload.hitDistance = query.CommittedRayT();
    payload.geometricNormal = geometricNormal;
    payload.shadingNormal = shadingNormal;
    payload.uv = uv;
    payload.materialIndex = instance.materialIndex;
    return payload;
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

// PathTraceBuildSurface without the material tables (the replacement reads only B's material and deltaSpecular).
bool PathTraceBuildSurface(PathTracePayload payload, float3 rayDirection, out PathTraceSurface surface)
{
    surface = (PathTraceSurface)0;
    surface.view = -rayDirection;
    surface.geometricNormal = payload.geometricNormal;
    surface.normal = payload.shadingNormal;

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
#if PSR_DIAGNOSTIC_HITS
    g_psrSampling.rawBaseColor = baseColorTexture.SampleLevel(materialSampler, payload.uv, 0.0f).rgb;
#endif

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
#if PSR_DIAGNOSTIC_HITS
    g_psrSampling.rawRoughness = useARM ? armRoughnessMetallic.x : 1.0f;
    g_psrSampling.rawMetallic = useARM ? armRoughnessMetallic.y : 1.0f;

    if(!useARM && material.roughnessTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> roughnessTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.roughnessTextureIndex)];
        g_psrSampling.rawRoughness = roughnessTexture.SampleLevel(materialSampler, payload.uv, 0.0f).r;
    }

    if(!useARM && material.metallicTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> metallicTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.metallicTextureIndex)];
        g_psrSampling.rawMetallic = metallicTexture.SampleLevel(materialSampler, payload.uv, 0.0f).r;
    }

    g_psrSampling.roughness = material.roughness;
    g_psrSampling.metallic = material.metallic;
#endif
    surface.material = material;

    if(!all(isfinite(surface.baseColor)) || !isfinite(material.roughness) || !isfinite(material.metallic))
    {
        return false;
    }

    return PathTraceFinalizeSurface(surface);
}

// -----------------------------------------------------------------------------
// Mirror recovery and the virtual image
// -----------------------------------------------------------------------------

float3 PsrTransformPoint(float4x4 transform, float3 position)
{
    return mul(transform, float4(position, 1.0f)).xyz;
}

// Diagnostic outcomes (PSR_DIAGNOSTIC_HITS).
static const uint PSR_DIAGNOSTIC_REPLACED = 1u;
static const uint PSR_DIAGNOSTIC_MISS = 2u;
static const uint PSR_DIAGNOSTIC_REFLECTED_MIRROR_OR_INVALID = 3u;
static const uint PSR_DIAGNOSTIC_DEPTH_RANGE = 4u;
static const uint PSR_DIAGNOSTIC_RECOVERY_FAILED = 5u;

// Record (128 bytes): outcome, committed InstanceID, PrimitiveIndex, RayT | barycentrics (b1, b2), UV | raw normal
// texel rg, raw roughness and metallic channels | raw base colour, metallic | tangent-space normal, roughness | world
// shading normal (before encoding), PSR.1 state | at PSR.1's UV with this hit's frame: raw normal rg, world normal |
// the mirror's emulated fp16 surface.xyz and geometricNormal.xyz, packed.
void PsrStoreDiagnostic(uint2 pixel, uint outcome, uint instanceID, uint primitiveIndex, float hitDistance,
    float2 barycentrics, float2 uv)
{
#if PSR_DIAGNOSTIC_HITS
    RWByteAddressBuffer hits = ResourceDescriptorHeap[diagnosticHitsIndex];
    const uint address = (pixel.y * resolution.x + pixel.x) * 128u;
    hits.Store4(address, uint4(outcome, instanceID, primitiveIndex, asuint(hitDistance)));
    hits.Store4(address + 16u, uint4(asuint(barycentrics), asuint(uv)));
    hits.Store4(address + 32u, uint4(asuint(g_psrSampling.rawNormal), asuint(g_psrSampling.rawRoughness),
        asuint(g_psrSampling.rawMetallic)));
    hits.Store4(address + 48u, uint4(asuint(g_psrSampling.rawBaseColor), asuint(g_psrSampling.metallic)));
    hits.Store4(address + 64u, uint4(asuint(g_psrSampling.tangentNormal), asuint(g_psrSampling.roughness)));

    // PSR.1's reflected-hit UV for this pixel (its record pass ran on the previous, in-place-off frame), sampled with this
    // hit's normal map and tangent frame: the two GPU implementations at identical UV on the same face.
    RWByteAddressBuffer psr1Records = ResourceDescriptorHeap[psr1RecordsIndex];
    const uint psr1Address = (pixel.y * resolution.x + pixel.x) * 40u;
    const uint psr1State = psr1Records.Load(psr1Address) & 0x7u;
    const float2 psr1UV = asfloat(uint2(psr1Records.Load(psr1Address + 32u), psr1Records.Load(psr1Address + 36u)));
    float2 rawAtPsr1 = float2(0.0f, 0.0f);
    float3 worldAtPsr1 = float3(0.0f, 0.0f, 0.0f);

    if(g_psrSampling.normalTextureIndex != 0xFFFFFFFFu && psr1State == 1u)
    {
        Texture2D<float4> normalTexture = ResourceDescriptorHeap[NonUniformResourceIndex(g_psrSampling.normalTextureIndex)];
        rawAtPsr1 = normalTexture.SampleLevel(materialSampler, psr1UV, 0.0f).rg;
        const float2 normalXY = rawAtPsr1 * 2.0f - 1.0f;
        const float3 sampledNormal = normalize(float3(normalXY, sqrt(saturate(1.0f - dot(normalXY, normalXY)))));
        const float3 bitangent = cross(g_psrSampling.worldNormal, g_psrSampling.tangentDirection) * g_psrSampling.handedness;
        worldAtPsr1 = normalize(g_psrSampling.tangentDirection * sampledNormal.x + bitangent * sampledNormal.y +
            g_psrSampling.worldNormal * sampledNormal.z);
    }

    hits.Store4(address + 80u, uint4(asuint(g_psrSampling.worldNormal), psr1State));
    hits.Store4(address + 96u, uint4(asuint(rawAtPsr1), asuint(worldAtPsr1.xy)));
    hits.Store4(address + 112u, uint4(asuint(worldAtPsr1.z), g_psrSampling.mirrorHalves.xyz));
#endif
}

void PsrStoreDiagnosticOutcome(uint2 pixel, uint outcome)
{
    PsrStoreDiagnostic(pixel, outcome, 0xFFFFFFFFu, 0xFFFFFFFFu, 0.0f, float2(0.0f, 0.0f), float2(0.0f, 0.0f));
}

PixelOutput PSMain(VertexOutput input)
{
    const uint2 pixel = uint2(input.position.xy);
    Texture2D<uint2> identityTexture = ResourceDescriptorHeap[identityIndex];
    const uint2 identity = identityTexture.Load(int3(pixel, 0));
#if PSR_DIAGNOSTIC_HITS
    g_psrSampling.normalTextureIndex = 0xFFFFFFFFu;
#endif

    if(!PsrIdentityEligible(identity.x))
    {
        discard;
    }

    // The mirror, decoded as PSR.1 decodes its G-buffer sample. The reflection does not depend on its base colour.
    PathTracePrimaryCamera camera;
    camera.inverseCameraRelativeViewProjection = inverseCameraRelativeViewProjection;
    camera.cameraPosition = cameraPosition;
    camera.resolution = resolution;
    PsrMirrorCamera mirrorCamera;
    mirrorCamera.camera = camera;
    mirrorCamera.cameraRelativeViewProjection = cameraRelativeViewProjection;
    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[materialBufferIndex];
    PsrRecoveredMirror recovered;
    const bool recoveredMirror = PsrRecoverMirror(pixel, identity, mirrorCamera, objectTransforms, transformCount,
        instanceDataIndex, materials, 0u, 0u, 0u, mirrorSampler, textureMipBias, false, recovered);
#if PSR_DIAGNOSTIC_HITS
    const uint3 surfaceHalves = f32tof16(recovered.gBuffer.surface.xyz);
    const uint3 geometricHalves = f32tof16(recovered.gBuffer.geometricNormal.xyz);
    g_psrSampling.mirrorHalves = uint4(surfaceHalves.x | (surfaceHalves.y << 16), surfaceHalves.z |
        (geometricHalves.x << 16), geometricHalves.y | (geometricHalves.z << 16), 0u);
#endif

    if(!recoveredMirror)
    {
        PsrStoreDiagnosticOutcome(pixel, PSR_DIAGNOSTIC_RECOVERY_FAILED);
        discard;
    }

    const PsrObjectTransform transform = objectTransforms[recovered.instanceID];
    const PathTracePrimaryVertex mirror = recovered.vertex;
    const float3 mirrorPoint = recovered.mirrorPoint;

    float3 diffuseShare;
    bool specularStrategy;
    const PathTraceBSDFSample mirrorSample = PathTraceSampleBSDFLobes(mirror.surface, float3(0.0f, 0.0f, 0.0f),
        diffuseShare, specularStrategy);

    if(!mirrorSample.valid || !mirrorSample.delta)
    {
        PsrStoreDiagnosticOutcome(pixel, PSR_DIAGNOSTIC_RECOVERY_FAILED);
        discard;
    }

    // The reflected ray (PSR_REPLACEMENT_ORIGIN), traced inline. No any-hit shaders exist, so every candidate is
    // accepted as TraceRay would. planePoint is the unlifted mirror point the virtual image is reflected through.
#if PSR_REPLACEMENT_ORIGIN == 2
    const float3 planePoint = mirrorPoint;
    const float3 originPoint = cameraPosition + mirrorPoint;
#else
    const float lift = PATH_TRACE_PRIMARY_SURFACE_LIFT * length(mirror.cameraRelativePosition);
    const float3 planePoint = mirror.cameraRelativePosition - mirror.surface.geometricNormal * lift;
#if PSR_REPLACEMENT_ORIGIN == 1
    const float3 originPoint = cameraPosition + planePoint;
#else
    const float3 originPoint = mirror.position;
#endif
#endif
    RayDesc ray;
    ray.Origin = PathTraceOffsetRayOrigin(originPoint, mirror.surface.geometricNormal);
    ray.Direction = mirrorSample.direction;
    ray.TMin = 0.0f;
    ray.TMax = PSR_REFLECTED_RAY_MAXIMUM_DISTANCE;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);

    while(query.Proceed())
    {
        if(query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            query.CommitNonOpaqueTriangleHit();
        }
    }

    if(query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        PsrStoreDiagnosticOutcome(pixel, PSR_DIAGNOSTIC_MISS);
        discard;
    }

    PathTracePayload payload = PsrClosestHit(query);
    PathTraceSurface replacement;
    uint replacementInstance = query.CommittedInstanceID();
    uint replacementPrimitive = query.CommittedPrimitiveIndex();
    float2 replacementBarycentrics = query.CommittedTriangleBarycentrics();
    bool built = PathTraceBuildSurface(payload, ray.Direction, replacement);
#if PSR_MAX_CHAIN >= 2
    // K = 2: a pure-delta mirror B is followed by its delta reflection, traced from B's exact hit with the path loop's
    // offset, exactly as the lighting passes retrace it. B's plane is its finalized shading normal through the unlifted
    // hit, as A's is.
    bool chained = false;
    float3 chainPlanePoint = float3(0.0f, 0.0f, 0.0f);
    float3 chainPlaneNormal = float3(0.0f, 0.0f, 1.0f);
    uint chainInstance = 0u;

    if(built && replacement.deltaSpecular && replacement.specularProbability >= 1.0f)
    {
        float3 chainShare;
        bool chainStrategy;
        const PathTraceBSDFSample chainSample = PathTraceSampleBSDFLobes(replacement, float3(0.0f, 0.0f, 0.0f), chainShare,
            chainStrategy);

        if(!chainSample.valid || !chainSample.delta)
        {
            discard;
        }

        const float3 chainHit = ray.Origin + ray.Direction * payload.hitDistance;
        chainPlanePoint = chainHit - cameraPosition;
        chainPlaneNormal = replacement.normal;
        chainInstance = replacementInstance;
        ray.Origin = PathTraceOffsetRayOrigin(chainHit, replacement.geometricNormal);
        ray.Direction = chainSample.direction;
        RayQuery<RAY_FLAG_NONE> terminalQuery;
        terminalQuery.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);

        while(terminalQuery.Proceed())
        {
            if(terminalQuery.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
            {
                terminalQuery.CommitNonOpaqueTriangleHit();
            }
        }

        if(terminalQuery.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        {
            discard;
        }

        payload = PsrClosestHit(terminalQuery);
        replacementInstance = terminalQuery.CommittedInstanceID();
        replacementPrimitive = terminalQuery.CommittedPrimitiveIndex();
        replacementBarycentrics = terminalQuery.CommittedTriangleBarycentrics();
        built = PathTraceBuildSurface(payload, ray.Direction, replacement);
        chained = true;
    }
#endif

    if(!built || replacement.deltaSpecular || payload.materialIndex > PSR_MAXIMUM_REPLACEMENT_MATERIAL)
    {
        PsrStoreDiagnostic(pixel, PSR_DIAGNOSTIC_REFLECTED_MIRROR_OR_INVALID, replacementInstance, replacementPrimitive,
            payload.hitDistance, replacementBarycentrics, payload.uv);
        discard;
    }

    // The virtual image, camera-relative: B reflected in the plane of the delta reflection, through the unlifted mirror
    // point with the finalized shading normal the delta direction reflects about. This unfolds the reflected ray onto
    // the pixel's camera ray, so the stored depth reconstructs the virtual point. For an unmapped planar mirror the
    // plane is PSR.1b's (the geometric normal); for a normal-mapped one PSR.1b's plane would put the point off the ray.
    const float3 planeNormal = mirror.surface.normal;
    const float3 replacementPoint = (ray.Origin - cameraPosition) + ray.Direction * payload.hitDistance;
#if PSR_MAX_CHAIN >= 2
    // K = 2: D through B's plane, then A's (Dv = R_A(R_B(D)), also on the pixel's camera ray).
    const float3 chainedPoint = chained ? PsrReflectPoint(replacementPoint, chainPlanePoint, chainPlaneNormal) :
        replacementPoint;
    const float3 virtualPoint = PsrReflectPoint(chainedPoint, planePoint, planeNormal);
#else
    const float3 virtualPoint = PsrReflectPoint(replacementPoint, planePoint, planeNormal);
#endif
    const float4 virtualClip = mul(cameraRelativeViewProjection, float4(virtualPoint, 1.0f));
    const float virtualDepth = virtualClip.z / virtualClip.w;

    if(!(virtualClip.w > 0.0f) || !(virtualDepth > 0.0f) || !(virtualDepth < 1.0f))
    {
        PsrStoreDiagnostic(pixel, PSR_DIAGNOSTIC_DEPTH_RANGE, replacementInstance, replacementPrimitive,
            payload.hitDistance, replacementBarycentrics, payload.uv);
        discard;
    }

    // Motion: the previous virtual point is the previous B in the previous mirror plane. Both points come from the same
    // world-space reflection; an exact identity transform returns its input unchanged, so unchanged poses give
    // bit-identical points and zero motion, as the geometry pass gives for static geometry. precise keeps the compiler
    // from contracting or reassociating the two reflections differently (HLSL arithmetic is otherwise fast-math).
    const float4x4 replacementToPrevious = replacementInstance < transformCount ?
        objectTransforms[replacementInstance].currentToPrevious : transform.currentToPrevious;
    const float3 worldReplacementPoint = cameraPosition + replacementPoint;
    const float3 worldPlanePoint = cameraPosition + planePoint;
#if PSR_MAX_CHAIN >= 2
    // K = 2: D moved by its instance, reflected in B's plane moved by B's instance, then in A's moved plane.
    float3 worldChainedPoint = worldReplacementPoint;
    float3 previousChainedPoint = PsrTransformPoint(replacementToPrevious, worldReplacementPoint);

    if(chained)
    {
        const float4x4 chainToPrevious = chainInstance < transformCount ?
            objectTransforms[chainInstance].currentToPrevious : transform.currentToPrevious;
        const float3 worldChainPlanePoint = cameraPosition + chainPlanePoint;
        worldChainedPoint = PsrReflectPoint(worldReplacementPoint, worldChainPlanePoint, chainPlaneNormal);
        previousChainedPoint = PsrReflectPoint(previousChainedPoint, PsrTransformPoint(chainToPrevious, worldChainPlanePoint),
            mul((float3x3)chainToPrevious, chainPlaneNormal));
    }

    precise const float3 worldVirtualPoint = PsrReflectPoint(worldChainedPoint, worldPlanePoint, planeNormal);
    precise const float3 previousVirtualPoint = PsrReflectPoint(previousChainedPoint,
        PsrTransformPoint(transform.currentToPrevious, worldPlanePoint),
        mul((float3x3)transform.currentToPrevious, planeNormal));
#else
    precise const float3 worldVirtualPoint = PsrReflectPoint(worldReplacementPoint, worldPlanePoint, planeNormal);
    // A plane's normal moves with the inverse transpose; for the rigid and uniformly scaled transforms of the scene the
    // linear part gives the same direction (unit length, so no renormalization is needed).
    precise const float3 previousVirtualPoint = PsrReflectPoint(PsrTransformPoint(replacementToPrevious, worldReplacementPoint),
        PsrTransformPoint(transform.currentToPrevious, worldPlanePoint),
        mul((float3x3)transform.currentToPrevious, planeNormal));
#endif
    precise const float4 currentMotionClip = mul(unjitteredViewProjection, float4(worldVirtualPoint, 1.0f));
    precise const float4 previousMotionClip = mul(previousViewProjection, float4(previousVirtualPoint, 1.0f));
    float4 motion = float4(0.0f, 0.0f, 0.0f, 1.0f);

    if(abs(currentMotionClip.w) > 1.0e-6f && abs(previousMotionClip.w) > 1.0e-6f)
    {
        precise const float2 currentUV = currentMotionClip.xy / currentMotionClip.w * float2(0.5f, -0.5f) + 0.5f;
        precise const float2 previousUV = previousMotionClip.xy / previousMotionClip.w * float2(0.5f, -0.5f) + 0.5f;
        precise const float3 candidate = float3(currentUV - previousUV,
            dot(previousViewDepthRow, float4(previousVirtualPoint, 1.0f)) -
            dot(viewDepthRow, float4(worldVirtualPoint, 1.0f)));

        if(all(isfinite(candidate)))
        {
            motion.xyz = candidate;
        }
    }

    // B's normals faced towards the reflected ray as PathTraceBuildSurface faces them, before PathTraceFinalizeSurface's
    // horizon correction, which the decode applies again (it uses only dot products, so it commutes with the reflection).
    const float facing = dot(payload.geometricNormal, -ray.Direction) < 0.0f ? -1.0f : 1.0f;
    PixelOutput output;
    output.material = float4(replacement.baseColor, replacement.material.roughness);
#if PSR_MAX_CHAIN >= 2
    const float3 chainedShadingNormal = chained ?
        PsrReflectDirection(payload.shadingNormal * facing, chainPlaneNormal) : payload.shadingNormal * facing;
    const float3 chainedGeometricNormal = chained ?
        PsrReflectDirection(payload.geometricNormal * facing, chainPlaneNormal) : payload.geometricNormal * facing;
    output.surface = float4(PsrReflectDirection(chainedShadingNormal, planeNormal), -(float(payload.materialIndex) + 1.0f));
    output.geometricNormal = float4(PsrReflectDirection(chainedGeometricNormal, planeNormal),
        replacement.material.model == 0u ? replacement.material.metallic : 0.0f);
    // The chain length (M2-A.1's key): 1.0 as the K = 1 pass writes, 2.0 for a two-mirror chain.
    motion.w = chained ? 2.0f : 1.0f;
#else
    output.surface = float4(PsrReflectDirection(payload.shadingNormal * facing, planeNormal),
        -(float(payload.materialIndex) + 1.0f));
    output.geometricNormal = float4(PsrReflectDirection(payload.geometricNormal * facing, planeNormal),
        replacement.material.model == 0u ? replacement.material.metallic : 0.0f);
#endif
    output.motion = motion;
    output.depth = virtualDepth;
#if PSR_DIAGNOSTIC_HITS
    {
        RWByteAddressBuffer hits = ResourceDescriptorHeap[diagnosticHitsIndex];
        PsrStoreDiagnostic(pixel, PSR_DIAGNOSTIC_REPLACED, replacementInstance, replacementPrimitive, payload.hitDistance,
            replacementBarycentrics, payload.uv);
        // Slot 80: the hit's world shading normal before encoding (the probe above used the frame normal).
        hits.Store3((pixel.y * resolution.x + pixel.x) * 128u + 80u, asuint(payload.shadingNormal));
    }
#else
    PsrStoreDiagnostic(pixel, PSR_DIAGNOSTIC_REPLACED, replacementInstance, replacementPrimitive, payload.hitDistance,
        replacementBarycentrics, payload.uv);
#endif
    return output;
}
