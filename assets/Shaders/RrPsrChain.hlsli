#ifndef ENGINE_RR_PSR_CHAIN
#define ENGINE_RR_PSR_CHAIN

// RR.1C: the second mirror B of a PSR.2 two-mirror chain (A -> B -> D), for the Ray Reconstruction albedo guides. The path
// tracer traces a chain pixel's A -> B and B -> D as ordinary vertices from A's exact intersection
// (PathTraceReference.hlsl: PathTracePsrChainFirstRay, PathTracePsrChainDirectSurface), so the radiance it extracts at D
// carries both delta weights, F_A * F_B. B is not in the G-buffer (it holds D through both mirror planes), so it is
// retraced here with exactly the path tracer's first chain ray: origin PathTraceOffsetRayOrigin(A's exact point, A's
// geometric normal), A's delta direction, TMax PATH_TRACE_DISTANT_LIGHT_DISTANCE, the closest hit's surface built as
// PathTraceBuildSurface builds it, and B's pure-delta weight (PathTraceSampleBSDFLobes). Nothing here changes transport.
//
// The includer includes PsrDenoise.hlsli first (the vertex loads, PathTraceTriangleIndices, the mirror recovery) and
// defines RR_PSR_CHAIN_SCENE_REGISTER and RR_PSR_CHAIN_SAMPLER_REGISTER (a t and an s register) for the TLAS and the path
// tracer's material sampler (trilinear wrap).
//
// The helpers below are verbatim copies of PathTraceReference.hlsl's (as PsrInPlaceReplace.hlsl keeps its own; factoring
// them into a shared header changed every production library's DXIL). The closest-hit evaluation is the shared
// PathTraceClosestHitBody.hlsli itself.

RaytracingAccelerationStructure scene : register(RR_PSR_CHAIN_SCENE_REGISTER);
SamplerState materialSampler : register(RR_PSR_CHAIN_SAMPLER_REGISTER);

// The names PathTraceClosestHitBody.hlsli and PathTraceBuildSurface read, set from PsrDenoiseConstants by
// RrPsrSecondMirror before either runs.
static uint instanceDataIndex = 0u;
static uint materialBufferIndex = 0u;
static uint materialTableIndex = 0u;
static uint materialAverageTableIndex = 0u;
static uint materialTableCosineMapping = 0u;

// PSR.1's reflected-ray TMax (PathTraceLights.hlsli's PATH_TRACE_DISTANT_LIGHT_DISTANCE).
static const float RR_PSR_CHAIN_MAXIMUM_DISTANCE = 1.0e27f;

struct PathTracePayload
{
    float hitDistance;
    float3 geometricNormal;
    float3 shadingNormal;
    float2 uv;
    uint materialIndex;
    // OpenPBR's packed coat smooth normal and anisotropy tangent (PathTraceReference.hlsl's payload).
    uint packedSmoothNormal;
    uint packedTangent;
};

float3 PathTraceOffsetRayOrigin(float3 position, float3 normal)
{
    const float originThreshold = 1.0f / 32.0f;
    const float floatScale = 1.0f / 65536.0f;
    const float integerScale = 256.0f;

    int3 integerOffset = int3(integerScale * normal);
    float3 integerOffsetPosition = asfloat(asint(position) + select(position < 0.0f, -integerOffset, integerOffset));
    return select(abs(position) < originThreshold, position + floatScale * normal, integerOffsetPosition);
}

float PathTraceResolveMetallic(GPUMaterial material, float2 uv, bool useARM, float sampledARMMetallic)
{
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
    surface.packedSmoothNormal = payload.packedSmoothNormal;
    surface.packedTangent = payload.packedTangent;

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

// PathTraceTraceRadianceInline's closest-hit evaluation (the shared body), for the chain's first segment.
void RrPsrTraceRadiance(RayDesc ray, inout PathTracePayload payload)
{
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);

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

// B's delta weight F_B for the recovered first mirror A (PsrRecoverMirror) and A's delta sample, false when the chain's
// first segment does not reach a pure-delta mirror (the path tracer then has no two-mirror chain there either). Also
// returns B's material index and finalized N.V (diagnostics).
bool RrPsrSecondMirror(PsrRecoveredMirror mirror, PathTraceBSDFSample mirrorSample, out float3 weight,
    out uint materialIndex, out float normalDotView)
{
    weight = float3(1.0f, 1.0f, 1.0f);
    materialIndex = 0u;
    normalDotView = 0.0f;
    instanceDataIndex = psrDenoise.instanceDataIndex;
    materialBufferIndex = psrDenoise.materialBufferIndex;
    materialTableIndex = psrDenoise.materialTableIndex;
    materialAverageTableIndex = psrDenoise.materialAverageTableIndex;
    materialTableCosineMapping = psrDenoise.materialTableCosineMapping;

    RayDesc ray;
    ray.Origin = PathTraceOffsetRayOrigin(psrDenoise.cameraPosition + mirror.mirrorPoint,
        mirror.vertex.surface.geometricNormal);
    ray.Direction = mirrorSample.direction;
    ray.TMin = 0.0f;
    ray.TMax = RR_PSR_CHAIN_MAXIMUM_DISTANCE;

    PathTracePayload payload;
    payload.hitDistance = -1.0f;
    payload.geometricNormal = float3(0.0f, 0.0f, 1.0f);
    payload.shadingNormal = float3(0.0f, 0.0f, 1.0f);
    payload.uv = float2(0.0f, 0.0f);
    payload.materialIndex = 0u;
    RrPsrTraceRadiance(ray, payload);
    PathTraceSurface chainSurface;

    // PathTracePsrPureMirror: B must be a pure-delta mirror, as the path tracer's chain requires.
    if(payload.hitDistance < 0.0f || !PathTraceBuildSurface(payload, ray.Direction, chainSurface) ||
        !(chainSurface.deltaSpecular && chainSurface.deltaProbability >= 1.0f))
    {
        return false;
    }

    float3 diffuseShare;
    bool specularStrategy;
    const PathTraceBSDFSample chainSample = PathTraceSampleBSDFLobes(chainSurface, float3(0.0f, 0.0f, 0.0f), diffuseShare,
        specularStrategy);

    if(!chainSample.valid || !chainSample.delta || !all(isfinite(chainSample.weight)))
    {
        return false;
    }

    weight = max(chainSample.weight, 0.0f);
    materialIndex = payload.materialIndex;
    normalDotView = chainSurface.normalDotView;
    return true;
}

#endif
