#ifndef ENGINE_PSR_DENOISE
#define ENGINE_PSR_DENOISE

// PSR.2 Stage 3B: what the denoisers' PSR variants (PATH_TRACE_PSR_DENOISE) need about a replaced pixel, the G-buffer's
// virtual surface Bv of a reflected surface B. The extraction split its radiance by B's lobes, both parts scaled by the
// mirror's throughput F (the path tracer's vertex-0 delta weight: Fresnel and tint). The variants filter the signal with
// F divided out and multiply it back, so the mirror's texture never travels with the virtual image's motion.
//
// The includer defines PSR_DENOISE_CONSTANTS_REGISTER, PSR_DENOISE_TRANSFORMS_REGISTER and PSR_DENOISE_SAMPLER_REGISTER
// (a b, a t and an s register) for PsrDenoiseConstants, the replacement pass's object transforms and Triangle.hlsl's
// material sampler (trilinear clamp).

#include "PathTracePrimarySurface.hlsli"
#include "RayTracingInstanceData.hlsli"

// The replacement pass's camera and identity target (as PathTracePsrGBufferConstants), and the path tracer's material
// tables, so the mirror decodes exactly as the path tracer's vertex 0 does.
struct PsrDenoiseConstantsData
{
    row_major float4x4 cameraRelativeViewProjection;
    row_major float4x4 inverseCameraRelativeViewProjection;
    float3 cameraPosition;
    uint identityIndex;
    uint transformCount;
    uint instanceDataIndex;
    uint materialBufferIndex;
    uint materialTableIndex;
    uint materialAverageTableIndex;
    uint materialTableCosineMapping;
    uint2 resolution;
    // The geometry pass's texture mip bias (PsrRecoverMirror).
    float textureMipBias;
};

ConstantBuffer<PsrDenoiseConstantsData> psrDenoise : register(PSR_DENOISE_CONSTANTS_REGISTER);

// Verbatim copies of PsrInPlaceReplace.hlsl's vertex loads (PsrMirrorRecovery.hlsli's includer contract).
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

#include "PsrMirrorRecovery.hlsli"

StructuredBuffer<PsrObjectTransform> psrDenoiseTransforms : register(PSR_DENOISE_TRANSFORMS_REGISTER);
SamplerState psrDenoiseMirrorSampler : register(PSR_DENOISE_SAMPLER_REGISTER);

// The per-channel floor of F, as the diffuse albedo's (0.02): demodulation and remodulation divide and multiply by the
// same floored value, so an unfiltered value returns exactly, and a dark tint never amplifies the signal.
static const float PSR_DENOISE_MINIMUM_THROUGHPUT = 0.02f;

// The G-buffer's material word as a surface identity: an ordinary surface's index i, a replaced pixel's -(i + 1). Equal
// words mean the same class (ordinary or replaced) and the same material. maximum is the shader's material limit.
bool PsrDenoiseExactMaterial(float stored, float maximum)
{
    return isfinite(stored) && stored == floor(stored) && stored < maximum && stored >= -maximum;
}

// A replaced pixel's mirror: its TLAS instance (exact in a float below 2^24), or -1 for every other pixel. Equal keys mean
// the same mirror, so the same virtual space.
float PsrDenoiseMirrorKey(int2 pixel, float storedMaterial)
{
    if(!(storedMaterial < 0.0f) || any(pixel < 0) || any(pixel >= int2(psrDenoise.resolution)))
    {
        return -1.0f;
    }

    Texture2D<uint2> identityTexture = ResourceDescriptorHeap[psrDenoise.identityIndex];
    uint identityX = identityTexture.Load(int3(pixel, 0)).x;
    return PsrIdentityEligible(identityX) ? float(PsrIdentityInstance(identityX)) : -1.0f;
}

// The mirror's delta weight F of a replaced pixel, floored per channel; 1 for every other pixel and for a mirror that
// cannot be recovered (its signal then keeps F, as the path tracer's estimate does).
float3 PsrDenoiseThroughput(uint2 pixel, float storedMaterial)
{
    if(!(storedMaterial < 0.0f))
    {
        return float3(1.0f, 1.0f, 1.0f);
    }

    Texture2D<uint2> identityTexture = ResourceDescriptorHeap[psrDenoise.identityIndex];
    uint2 identity = identityTexture.Load(int3(pixel, 0));

    if(!PsrIdentityEligible(identity.x))
    {
        return float3(1.0f, 1.0f, 1.0f);
    }

    PsrMirrorCamera mirrorCamera;
    mirrorCamera.camera.inverseCameraRelativeViewProjection = psrDenoise.inverseCameraRelativeViewProjection;
    mirrorCamera.camera.cameraPosition = psrDenoise.cameraPosition;
    mirrorCamera.camera.resolution = psrDenoise.resolution;
    mirrorCamera.cameraRelativeViewProjection = psrDenoise.cameraRelativeViewProjection;
    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[psrDenoise.materialBufferIndex];
    PsrRecoveredMirror mirror;

    if(!PsrRecoverMirror(pixel, identity, mirrorCamera, psrDenoiseTransforms, psrDenoise.transformCount,
        psrDenoise.instanceDataIndex, materials, psrDenoise.materialTableIndex, psrDenoise.materialAverageTableIndex,
        psrDenoise.materialTableCosineMapping, psrDenoiseMirrorSampler, psrDenoise.textureMipBias, true, mirror))
    {
        return float3(1.0f, 1.0f, 1.0f);
    }

    float3 diffuseShare;
    bool specularStrategy;
    PathTraceBSDFSample sample = PathTraceSampleBSDFLobes(mirror.vertex.surface, float3(0.0f, 0.0f, 0.0f), diffuseShare,
        specularStrategy);

    if(!sample.valid || !sample.delta || !all(isfinite(sample.weight)))
    {
        return float3(1.0f, 1.0f, 1.0f);
    }

    return max(sample.weight, PSR_DENOISE_MINIMUM_THROUGHPUT);
}

#endif
