#ifndef ENGINE_PATH_TRACE_PRIMARY_SURFACE
#define ENGINE_PATH_TRACE_PRIMARY_SURFACE

// RTXPT.2 R2: the first path vertex reconstructed from the raster G-buffer instead of a camera ray.
// Consumed by the raster-primary mode (RTXPT.2 R3) and the R2 validation; the reference mode keeps its
// own camera-ray constructor.
//
// G-buffer encodings (Triangle.hlsl):
//   material         RGBA8 UNORM: base colour (clamped to [0, 1], 8-bit), final roughness
//   surface          RGBA16F: shading normal (normal-mapped, unflipped), material index as float
//   geometric normal RGBA16F: normalized interpolated vertex normal (not the triangle face normal the
//                    reference path tracer uses), effective model-0 metallic (0 for other models)
//   depth            D3D depth, near 0, far 1; 1 is background
// These encodings, and the parallax UV offset the G-buffer applies, are the known differences from an
// RT-primary hit. R3 measures them; nothing here hides them.

#include "PathTraceBSDF.hlsli"

// The material index is stored as fp16, which represents every integer exactly only up to 2048. The
// renderer refuses the raster-primary mode for larger material tables
// (PathTraceRasterPrimaryMaximumMaterials); this check rejects anything else that is not an exact index.
static const float PATH_TRACE_PRIMARY_MAXIMUM_MATERIALS = 2048.0f;

// A reconstructed point lies up to ~5e-5 x camera distance off the true surface, either side (RTXPT.2 R3,
// measured), while an RT hit lies on it. Behind the surface, PathTraceOffsetRayOrigin's offset (as small as
// 1/65536 near the world axes) cannot clear it, and every ray from the point self-shadows. The shaded point is
// therefore lifted along the facing geometric normal by 4x that error. RT hits are unaffected.
static const float PATH_TRACE_PRIMARY_SURFACE_LIFT = 2.0e-4f;

struct PathTraceGBufferSample
{
    float depth;
    float4 material;
    float4 surface;
    float4 geometricNormal;
};

struct PathTracePrimaryCamera
{
    // Inverse of projection * view with the view translation removed: the unprojection then yields the
    // offset from the camera, which keeps precision far from the world origin.
    float4x4 inverseCameraRelativeViewProjection;
    float3 cameraPosition;
    uint2 resolution;
};

struct PathTracePrimaryVertex
{
    PathTraceSurface surface;
    // World position, and the same point relative to the camera (the more precise of the two).
    float3 position;
    float3 cameraRelativePosition;
    uint materialIndex;
};

PathTraceGBufferSample PathTraceLoadGBuffer(uint2 pixel, Texture2D<float> depthTexture, Texture2D<float4> materialTexture,
    Texture2D<float4> surfaceTexture, Texture2D<float4> geometricNormalTexture)
{
    PathTraceGBufferSample gBuffer;
    gBuffer.depth = depthTexture.Load(int3(pixel, 0));
    gBuffer.material = materialTexture.Load(int3(pixel, 0));
    gBuffer.surface = surfaceTexture.Load(int3(pixel, 0));
    gBuffer.geometricNormal = geometricNormalTexture.Load(int3(pixel, 0));
    return gBuffer;
}

// The pixel centre's point on the G-buffer surface, relative to the camera. The raster pass rasterizes at
// pixel centres, so the reconstruction uses the same NDC convention as PathTraceCameraRay at jitter 0.5.
float3 PathTraceUnprojectCameraRelative(uint2 pixel, float depth, PathTracePrimaryCamera camera)
{
    float2 uv = (float2(pixel) + 0.5f) / float2(camera.resolution);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 relative = mul(camera.inverseCameraRelativeViewProjection, float4(ndc, depth, 1.0f));
    return relative.xyz / relative.w;
}

// Decodes the G-buffer into a surface without PathTraceFinalizeSurface (which applies the BSDF's own
// conventions, such as the horizon correction), so the decoding can be validated on its own. Returns
// false for background pixels and for any sample that cannot be a valid surface (non-finite data, a
// material index that is not exact or not in the table, a model the BSDF does not support).
bool PathTraceDecodePrimaryVertex(uint2 pixel, PathTraceGBufferSample gBuffer, PathTracePrimaryCamera camera,
    StructuredBuffer<GPUMaterial> materials, uint materialTableIndex, uint materialAverageTableIndex,
    uint materialTableCosineMapping, out PathTracePrimaryVertex vertex)
{
    vertex = (PathTracePrimaryVertex)0;

    if(!(gBuffer.depth < 1.0f) || !(gBuffer.depth >= 0.0f))
    {
        return false;
    }

    // Material index: an exact integer in [0, min(material count, 2048)).
    uint materialCount;
    uint materialStride;
    materials.GetDimensions(materialCount, materialStride);
    float storedIndex = gBuffer.surface.w;
    bool exactIndex = isfinite(storedIndex) && storedIndex >= 0.0f && storedIndex == floor(storedIndex) &&
        storedIndex < PATH_TRACE_PRIMARY_MAXIMUM_MATERIALS && storedIndex < float(materialCount);

    if(!exactIndex)
    {
        return false;
    }

    vertex.materialIndex = uint(storedIndex);
    GPUMaterial material = materials[vertex.materialIndex];

    if(material.model > 2u)
    {
        return false;
    }

    // Position: unprojected relative to the camera, then placed in the world.
    vertex.cameraRelativePosition = PathTraceUnprojectCameraRelative(pixel, gBuffer.depth, camera);
    vertex.position = camera.cameraPosition + vertex.cameraRelativePosition;
    float distanceSquared = dot(vertex.cameraRelativePosition, vertex.cameraRelativePosition);

    if(!all(isfinite(vertex.cameraRelativePosition)) || !(distanceSquared > 0.0f))
    {
        return false;
    }

    PathTraceSurface surface = (PathTraceSurface)0;
    surface.view = -vertex.cameraRelativePosition * rsqrt(distanceSquared);

    // Normals: both encoded unit-length; renormalize after the fp16 round trip.
    float shadingLengthSquared = dot(gBuffer.surface.xyz, gBuffer.surface.xyz);
    float geometricLengthSquared = dot(gBuffer.geometricNormal.xyz, gBuffer.geometricNormal.xyz);

    if(!(shadingLengthSquared > 1.0e-8f) || !isfinite(shadingLengthSquared) ||
        !(geometricLengthSquared > 1.0e-8f) || !isfinite(geometricLengthSquared))
    {
        return false;
    }

    surface.normal = gBuffer.surface.xyz * rsqrt(shadingLengthSquared);
    surface.geometricNormal = gBuffer.geometricNormal.xyz * rsqrt(geometricLengthSquared);

    // Surfaces are two-sided, exactly as PathTraceBuildSurface: face both normals towards the viewer.
    if(dot(surface.geometricNormal, surface.view) < 0.0f)
    {
        surface.geometricNormal = -surface.geometricNormal;
        surface.normal = -surface.normal;
    }

    // Material: the G-buffer already holds the textured base colour, roughness and model-0 metallic, so the
    // textures are not sampled again. Other models keep their parametric metallic, as PathTraceResolveMetallic.
    material.roughness = gBuffer.material.w;

    if(material.model == 0u)
    {
        material.metallic = gBuffer.geometricNormal.w;
    }

    surface.material = material;
    surface.baseColor = gBuffer.material.rgb;
    surface.materialTableIndex = materialTableIndex;
    surface.materialAverageTableIndex = materialAverageTableIndex;
    surface.materialTableCosineMapping = materialTableCosineMapping;

    if(!all(isfinite(surface.baseColor)) || !isfinite(material.roughness) || !isfinite(material.metallic))
    {
        return false;
    }

    vertex.surface = surface;
    return true;
}

// The primary vertex the raster-primary mode shades: decoded, lifted off the surface (see
// PATH_TRACE_PRIMARY_SURFACE_LIFT), then finalized exactly as PathTraceBuildSurface finalizes an RT hit.
bool PathTraceReconstructPrimaryVertex(uint2 pixel, PathTraceGBufferSample gBuffer, PathTracePrimaryCamera camera,
    StructuredBuffer<GPUMaterial> materials, uint materialTableIndex, uint materialAverageTableIndex,
    uint materialTableCosineMapping, out PathTracePrimaryVertex vertex)
{
    if(!PathTraceDecodePrimaryVertex(pixel, gBuffer, camera, materials, materialTableIndex, materialAverageTableIndex,
        materialTableCosineMapping, vertex))
    {
        return false;
    }

    float lift = PATH_TRACE_PRIMARY_SURFACE_LIFT * length(vertex.cameraRelativePosition);
    vertex.cameraRelativePosition += vertex.surface.geometricNormal * lift;
    vertex.position = camera.cameraPosition + vertex.cameraRelativePosition;
    return PathTraceFinalizeSurface(vertex.surface);
}

#endif
