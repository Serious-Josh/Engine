#ifndef ENGINE_PSR_MIRROR_RECOVERY
#define ENGINE_PSR_MIRROR_RECOVERY

// PSR.2: the eligible mirror of a pixel recovered from its raster identity alone (instance, triangle, camera-ray
// intersection, normals, UV, material), as the path tracer would decode the mirror's own G-buffer sample. Shared by the
// replacement pass (PsrInPlaceReplace.hlsl), which needs only the reflection, and the PSR.2 lighting libraries
// (PathTraceReference.hlsl with PATH_TRACE_PSR_GBUFFER), whose vertex 0 is this mirror. The includer provides
// PathTraceLoadPosition, PathTraceLoadNormal, PathTraceLoadUV, PathTraceLoadTangent and PathTraceTriangleIndices (both
// carry identical copies).

#include "PsrEligibility.hlsli"
#include "RayTracingInstanceData.hlsli"

// fp16 rounding of the mirror's emulated G-buffer normals (compile-time; the mirror-normal quantization study):
//   0  PsrStoredHalf (f32tof16, which rounds toward zero on the target GPU)
//   1  round to nearest even, as the render-target store rounds
#ifndef PSR_MIRROR_HALF_ROUNDING
#define PSR_MIRROR_HALF_ROUNDING 0
#endif

// Per render object (TLAS InstanceID): the geometry pass's ObjectConstants model and normal matrix, and the
// current-to-previous world transform (the exact identity for an unchanged object).
struct PsrObjectTransform
{
    row_major float4x4 model;
    row_major float4x4 normalMatrix;
    row_major float4x4 currentToPrevious;
};

// The geometry pass's camera: the decode's camera (its inverse render view-projection without the view translation)
// and the forward render view-projection, also without it, which stores the mirror's depth.
struct PsrMirrorCamera
{
    PathTracePrimaryCamera camera;
    float4x4 cameraRelativeViewProjection;
};

struct PsrRecoveredMirror
{
    // The mirror decoded and finalized from the emulated G-buffer sample, as PathTraceLoadRasterPrimaryVertex decodes it.
    PathTracePrimaryVertex vertex;
    PathTraceGBufferSample gBuffer;
    // The exact camera-ray intersection, relative to the camera: the point the virtual image is reflected through.
    float3 mirrorPoint;
    uint instanceID;
};

// Two-sided Moller-Trumbore from the camera, unbounded (barycentrics outside the triangle are returned as they are, so
// neighbouring pixels' rays give the plane's UV derivatives). x, y = b1, b2; z = distance.
bool PsrIntersectPlane(float3 direction, float3 corners[3], out float3 result)
{
    const float3 edge1 = corners[1] - corners[0];
    const float3 edge2 = corners[2] - corners[0];
    const float3 p = cross(direction, edge2);
    const float determinantValue = dot(edge1, p);
    result = float3(0.0f, 0.0f, 0.0f);

    if(!(abs(determinantValue) > 1.0e-20f))
    {
        return false;
    }

    const float inverseDeterminant = 1.0f / determinantValue;
    const float3 toOrigin = -corners[0];
    const float3 q = cross(toOrigin, edge1);
    result = float3(dot(toOrigin, p), dot(direction, q), dot(edge2, q)) * inverseDeterminant;
    return all(isfinite(result));
}

float3 PsrPixelDirection(float2 pixel, PathTracePrimaryCamera camera)
{
    return normalize(PathTraceUnprojectCameraRelative(uint2(pixel), 1.0f, camera));
}

float2 PsrInterpolate(float2 values[3], float2 barycentrics)
{
    return values[0] * (1.0f - barycentrics.x - barycentrics.y) + values[1] * barycentrics.x +
        values[2] * barycentrics.y;
}

float3 PsrReflectPoint(float3 position, float3 planePoint, float3 planeNormal)
{
    return position - 2.0f * planeNormal * dot(planeNormal, position - planePoint);
}

float3 PsrReflectDirection(float3 direction, float3 planeNormal)
{
    return direction - 2.0f * planeNormal * dot(planeNormal, direction);
}

// fp32 to fp16 bits, rounded to nearest even (finite values; normals and material indices).
uint PsrHalfBitsNearestEven(float value)
{
    const uint bits = asuint(value);
    const uint sign = (bits >> 16) & 0x8000u;
    const uint magnitude = bits & 0x7FFFFFFFu;

    if(magnitude >= 0x477FF000u)
    {
        return sign | 0x7C00u;
    }

    // Below 2^-14: an fp16 subnormal, the value in units of 2^-24 (an exact power-of-two scale), rounded to even.
    if(magnitude < 0x38800000u)
    {
        return sign | uint(round(asfloat(magnitude) * 16777216.0f));
    }

    const uint mantissa = magnitude & 0x007FFFFFu;
    uint half = (((magnitude >> 23) - 112u) << 10) | (mantissa >> 13);
    const uint remainder = mantissa & 0x1FFFu;

    // A carry out of the mantissa increments the exponent, which is the correct rounding.
    if(remainder > 0x1000u || (remainder == 0x1000u && (half & 1u) != 0u))
    {
        half += 1u;
    }

    return sign | half;
}

// The mirror's stored G-buffer value (PSR_MIRROR_HALF_ROUNDING).
float4 PsrStoredHalfMirror(float4 value)
{
#if PSR_MIRROR_HALF_ROUNDING == 1
    return f16tof32(uint4(PsrHalfBitsNearestEven(value.x), PsrHalfBitsNearestEven(value.y),
        PsrHalfBitsNearestEven(value.z), PsrHalfBitsNearestEven(value.w)));
#else
    return PsrStoredHalf(value);
#endif
}

// The mirror of an eligible pixel (identity.x carries the flag). Triangle.hlsl's implicit derivatives are the 2x2 quad's
// fine differences; here the same quad pixels' UVs come from rays through their centres. tint: false stores the base
// colour as (1, 1, 1), which the reflection direction does not depend on (the replacement pass); true samples it as
// Triangle.hlsl does (base colour factor times the base colour texture), which the mirror's Fresnel needs. The material
// tables are the decode's (0 where only the reflection is needed). Fails where the mirror cannot be recovered.
// textureMipBias is the geometry pass's (CameraConstants::textureMipBias, non-zero only while DLSS reconstructs the
// frame): Triangle.hlsl samples with SampleBias, which adds it to the LOD of the quad's implicit derivatives. Scaling
// the explicit gradients by 2^bias shifts the computed LOD by the same amount (isotropic and anisotropic footprints
// scale alike), so the recovered mirror samples the mip the geometry pass sampled. A zero bias leaves them unchanged.
bool PsrRecoverMirror(uint2 pixel, uint2 identity, PsrMirrorCamera mirrorCamera,
    StructuredBuffer<PsrObjectTransform> objectTransforms, uint transformCount, uint instanceDataIndex,
    StructuredBuffer<GPUMaterial> materials, uint materialTableIndex, uint materialAverageTableIndex,
    uint materialTableCosineMapping, SamplerState mirrorSampler, float textureMipBias, bool tint,
    out PsrRecoveredMirror mirror)
{
    mirror = (PsrRecoveredMirror)0;
    const uint instanceID = PsrIdentityInstance(identity.x) - 1u;
    mirror.instanceID = instanceID;

    if(instanceID >= transformCount)
    {
        return false;
    }

    StructuredBuffer<RayTracingInstanceData> instances = ResourceDescriptorHeap[instanceDataIndex];
    RayTracingInstanceData instance = instances[instanceID];
    const PsrObjectTransform transform = objectTransforms[instanceID];
    ByteAddressBuffer vertexBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(instance.vertexBufferIndex)];
    const uint3 indices = PathTraceTriangleIndices(instance, identity.y);
    const PathTracePrimaryCamera camera = mirrorCamera.camera;

    // The mirror triangle in camera-relative world space, transformed as the geometry pass's vertex shader does.
    float3 corners[3];
    float3 normals[3];
    float2 uvs[3];
    float4 tangents[3];

    [unroll]
    for(uint corner = 0u; corner < 3u; ++corner)
    {
        const uint vertexIndex = indices[corner];
        corners[corner] = mul(transform.model,
            float4(PathTraceLoadPosition(vertexBuffer, vertexIndex, instance.vertexStride), 1.0f)).xyz -
            camera.cameraPosition;
        normals[corner] = mul((float3x3)transform.normalMatrix, PathTraceLoadNormal(vertexBuffer, vertexIndex, instance));
        uvs[corner] = PathTraceLoadUV(vertexBuffer, vertexIndex, instance);
        tangents[corner] = instance.tangentOffset != 0xFFFFFFFFu ?
            PathTraceLoadTangent(vertexBuffer, vertexIndex, instance) : float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    // The pixel-centre camera ray (the decode's convention), from the camera.
    float3 hitResult;

    if(!PsrIntersectPlane(PsrPixelDirection(float2(pixel), camera), corners, hitResult) || !(hitResult.z > 0.0f))
    {
        return false;
    }

    const float barycentric0 = 1.0f - hitResult.x - hitResult.y;
    const float3 mirrorPoint = PsrPixelDirection(float2(pixel), camera) * hitResult.z;
    const float4 mirrorClip = mul(mirrorCamera.cameraRelativeViewProjection, float4(mirrorPoint, 1.0f));
    const float3 interpolatedNormal = normalize(normals[0] * barycentric0 + normals[1] * hitResult.x +
        normals[2] * hitResult.y);
    const float2 mirrorUV = PsrInterpolate(uvs, hitResult.xy);
    const uint mirrorMaterialIndex = ResolveHitMaterial(instance, identity.y);
    const GPUMaterial mirrorMaterial = materials[mirrorMaterialIndex];

    // The UV derivatives of the pixel's quad, for every texture the geometry pass sampled here.
    const bool normalMapped = mirrorMaterial.normalTextureIndex != 0xFFFFFFFFu;
    bool derivatives = false;
    float2 uvDx = float2(0.0f, 0.0f);
    float2 uvDy = float2(0.0f, 0.0f);

    if(normalMapped || tint)
    {
        const uint2 quad = pixel & ~1u;
        float3 x0;
        float3 x1;
        float3 y0;
        float3 y1;
        derivatives =
            PsrIntersectPlane(PsrPixelDirection(float2(quad.x, pixel.y), camera), corners, x0) &&
            PsrIntersectPlane(PsrPixelDirection(float2(quad.x + 1u, pixel.y), camera), corners, x1) &&
            PsrIntersectPlane(PsrPixelDirection(float2(pixel.x, quad.y), camera), corners, y0) &&
            PsrIntersectPlane(PsrPixelDirection(float2(pixel.x, quad.y + 1u), camera), corners, y1);
        const float biasScale = exp2(textureMipBias);
        uvDx = (PsrInterpolate(uvs, x1.xy) - PsrInterpolate(uvs, x0.xy)) * biasScale;
        uvDy = (PsrInterpolate(uvs, y1.xy) - PsrInterpolate(uvs, y0.xy)) * biasScale;
    }

    // The mirror's shading normal as Triangle.hlsl computes it.
    float3 mappedNormal = interpolatedNormal;

    if(normalMapped)
    {
        Texture2D<float4> normalTexture = ResourceDescriptorHeap[NonUniformResourceIndex(mirrorMaterial.normalTextureIndex)];
        const float2 normalXY = (derivatives ? normalTexture.SampleGrad(mirrorSampler, mirrorUV, uvDx, uvDy) :
            normalTexture.SampleLevel(mirrorSampler, mirrorUV, 0.0f)).rg * 2.0f - 1.0f;
        const float3 sampledNormal = float3(normalXY, sqrt(saturate(1.0f - dot(normalXY, normalXY))));
        const float4 objectTangent = tangents[0] * barycentric0 + tangents[1] * hitResult.x + tangents[2] * hitResult.y;
        const float modelOrientation = determinant((float3x3)transform.model) < 0.0f ? -1.0f : 1.0f;
        const float3 worldTangent = mul((float3x3)transform.model, objectTangent.xyz);
        const float3 tangent = worldTangent - interpolatedNormal * dot(interpolatedNormal, worldTangent);
        const float tangentLengthSquared = dot(tangent, tangent);
        const float sampledLengthSquared = dot(sampledNormal, sampledNormal);
        const float handedness = objectTangent.w * modelOrientation;

        if(all(isfinite(tangent)) && isfinite(tangentLengthSquared) && tangentLengthSquared > 1.0e-8f &&
            all(isfinite(sampledNormal)) && isfinite(sampledLengthSquared) && sampledLengthSquared > 1.0e-8f &&
            isfinite(handedness) && abs(handedness) > 1.0e-8f)
        {
            const float3 tangentDirection = normalize(tangent);
            const float3 bitangent = cross(interpolatedNormal, tangentDirection) * handedness;
            const float3 worldMappedNormal = tangentDirection * sampledNormal.x + bitangent * sampledNormal.y +
                interpolatedNormal * sampledNormal.z;
            const float mappedLengthSquared = dot(worldMappedNormal, worldMappedNormal);

            if(all(isfinite(bitangent)) && all(isfinite(worldMappedNormal)) && isfinite(mappedLengthSquared) &&
                mappedLengthSquared > 1.0e-8f)
            {
                mappedNormal = normalize(worldMappedNormal);
            }
        }
    }

    // The base colour the material target stored (8-bit sRGB): the factor times the texture, as Triangle.hlsl writes it.
    float3 baseColor = float3(1.0f, 1.0f, 1.0f);

    if(tint)
    {
        Texture2D<float4> baseColorTexture = ResourceDescriptorHeap[NonUniformResourceIndex(mirrorMaterial.textureIndex)];
        const float3 written = mirrorMaterial.baseColor * (derivatives ?
            baseColorTexture.SampleGrad(mirrorSampler, mirrorUV, uvDx, uvDy) :
            baseColorTexture.SampleLevel(mirrorSampler, mirrorUV, 0.0f)).rgb;
        baseColor = float3(PsrStoredSrgb8(written.r), PsrStoredSrgb8(written.g), PsrStoredSrgb8(written.b));
    }

    // The path tracer's vertex 0: the stored G-buffer values decoded by the path tracer. Eligibility already holds (the
    // flag), so roughness is the zero byte and metallic takes its eligible value.
    PathTraceGBufferSample gBuffer;
    gBuffer.depth = mirrorClip.z / mirrorClip.w;
    gBuffer.material = float4(baseColor, 0.0f);
    gBuffer.surface = PsrStoredHalfMirror(float4(mappedNormal, float(mirrorMaterialIndex)));
    gBuffer.geometricNormal = PsrStoredHalfMirror(float4(interpolatedNormal, 1.0f));
    mirror.gBuffer = gBuffer;
    mirror.mirrorPoint = mirrorPoint;
    return PathTraceReconstructPrimaryVertex(pixel, gBuffer, camera, materials, materialTableIndex,
        materialAverageTableIndex, materialTableCosineMapping, mirror.vertex);
}

#endif
