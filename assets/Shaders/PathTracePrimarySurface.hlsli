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
//                    reference path tracer uses), effective metalness (textured)
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

// Far from the camera the depth target's resolution takes over. The stored depth is near 1 there, so one unit in its
// last place (2^-24) moves the reconstructed point by distance^2 / near x 2^-24 along the view ray: 0.6 m at 1 km and
// 15 m at 5 km with a 0.1 m near plane, against a proportional lift of 0.2 m and 1 m. A float32 model of the raster path
// (Triangle.hlsl's vertex shader, an ideal rasterizer, the D32 store and the unprojection below) puts points up to 5
// units behind distant surfaces. The lift covers 4 units, which passes the proportional lift about 80 m away; the
// G-buffer pass's lift scale (PathTracePrimaryRasterLiftScale) covers whatever is left.
static const float PATH_TRACE_PRIMARY_DEPTH_RESOLUTION = 4.0f / 16777216.0f;

// The lift scale is stored as the length of the G-buffer's geometric normal, a power of two up to this (fp16 holds it
// exactly, and every reader of that target normalizes it).
static const float PATH_TRACE_PRIMARY_MAXIMUM_LIFT_SCALE = 32768.0f;

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
    // The distance PathTraceReconstructPrimaryVertex moved the point along the geometric normal (0 after a plain decode).
    float lift;
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

// How far a decoded point is lifted along its geometric normal, before the lift scale: the proportional lift, or the
// depth target's resolution at this pixel where that is larger. The resolution is an error along the view ray, so only
// its component along the normal counts (a grazing surface's point mostly slides within the surface). With W the
// unprojection's homogeneous coordinate, the distance changes by distance x |dW/d(depth)| / |W| per unit of depth.
float PathTracePrimarySurfaceLift(uint2 pixel, float depth, float3 cameraRelativePosition, float3 geometricNormal,
    PathTracePrimaryCamera camera)
{
    const float proportionalLift = PATH_TRACE_PRIMARY_SURFACE_LIFT * length(cameraRelativePosition);

    const float2 uv = (float2(pixel) + 0.5f) / float2(camera.resolution);
    const float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    const float homogeneousW = dot(camera.inverseCameraRelativeViewProjection[3], ndc);
    const float depthLift = abs(dot(geometricNormal, cameraRelativePosition)) *
        abs(camera.inverseCameraRelativeViewProjection[3][2]) * PATH_TRACE_PRIMARY_DEPTH_RESOLUTION / abs(homogeneousW);

    // Near surfaces keep exactly the proportional lift they always had.
    return max(proportionalLift, depthLift);
}

// The lift scale the G-buffer pass stored for this pixel (PathTracePrimaryRasterLiftScale): the stored geometric
// normal's length, rounded to its power of two. A unit normal gives exactly 1, whatever its fp16 rounding.
float PathTracePrimaryLiftScale(float3 storedGeometricNormal)
{
    const float lengthSquared = dot(storedGeometricNormal, storedGeometricNormal);

    if(!(lengthSquared > 2.0f))
    {
        return 1.0f;
    }

    return min(exp2(round(0.5f * log2(lengthSquared))), PATH_TRACE_PRIMARY_MAXIMUM_LIFT_SCALE);
}

// For the G-buffer pass (Triangle.hlsl): the lift scale of a pixel, 1 unless the lift above cannot clear the surface.
//
// The case it exists for is a triangle that reaches kilometres from the camera and passes under or behind it, such as a
// ground plane built from two 60 km triangles. Its far vertices' clip-space z and w are both about their distance, and
// float32 keeps them only to 2 mm at 30 km, while the depth of everything near the camera hangs on their difference
// from the near plane's 0.1 m. The rasterized plane therefore sits a constant few millimetres off the real one (6.5 mm
// below it in the scene that showed this), at every distance, so every ray from a nearer point than 33 m started under
// the ground and hit it. No property of the pixel bounds that error, but the pass can measure it: this function repeats
// the path tracer's decode of the depth the pixel is about to store and compares it with the surface point itself.
//
// surfacePosition must be that point with the precision the ray-traced surface has: the interpolated object-space
// position put through the model matrix. (The interpolated world position is not: it is interpolated between vertices
// already rounded at their distance.) geometricNormal is the unit normal the pass stores.
//
// The scale leaves the lifted point at least half the unscaled lift above the surface. Ordinary pixels are never more
// than a quarter of it below (the 5e-5 above), so they keep scale 1 and their stored normal is unchanged.
float PathTracePrimaryRasterLiftScale(uint2 pixel, float depth, float3 surfacePosition, float3 geometricNormal,
    PathTracePrimaryCamera camera)
{
    const float3 cameraRelativePosition = PathTraceUnprojectCameraRelative(pixel, depth, camera);

    if(!all(isfinite(cameraRelativePosition)) || !(dot(cameraRelativePosition, cameraRelativePosition) > 0.0f))
    {
        return 1.0f;
    }

    // Facing the viewer, as the decode turns it.
    const float3 facingNormal = dot(geometricNormal, cameraRelativePosition) > 0.0f ? -geometricNormal : geometricNormal;
    const float height = dot(facingNormal, cameraRelativePosition - (surfacePosition - camera.cameraPosition));
    const float lift = PathTracePrimarySurfaceLift(pixel, depth, cameraRelativePosition, facingNormal, camera);
    const float requiredLift = 0.5f * lift - height;

    // Also the answer for a non-finite measurement.
    if(!(requiredLift > lift))
    {
        return 1.0f;
    }

    return min(exp2(ceil(log2(requiredLift / lift))), PATH_TRACE_PRIMARY_MAXIMUM_LIFT_SCALE);
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

    // Material: the G-buffer already holds the textured base colour, roughness and metalness, so the textures are not
    // sampled again.
    material.roughness = gBuffer.material.w;
    material.metallic = gBuffer.geometricNormal.w;

#ifdef PATH_TRACE_PRIMARY_HAS_OCCLUSION
    // The opt-in path-traced AO from the G-buffer's material AO (the includer's PathTracePrimaryOcclusion). A previous
    // frame's surface reads the current frame's AO (the G-buffer history keeps no motion target).
    if((material.lobeFlags & GPU_MATERIAL_PATH_TRACED_AO) != 0u)
    {
        OpenPBRApplyOcclusion(material, PathTracePrimaryOcclusion(pixel));
    }
#endif

    // OpenPBR: a coat's smooth normal is the G-buffer's interpolated normal. The G-buffer has no tangent, so an
    // anisotropic pixel gets it from one camera ray where the includer provides PathTracePrimaryTangent; otherwise
    // anisotropy is off at the raster primary (it always applies at ray-traced hits).
    surface.packedSmoothNormal = (material.lobeFlags & GPU_MATERIAL_LOBE_COAT) != 0u ?
        OpenPBRPackUnitVector(surface.geometricNormal) : OPENPBR_PACKED_VECTOR_NONE;
    surface.packedTangent = OPENPBR_PACKED_VECTOR_NONE;

#ifdef PATH_TRACE_PRIMARY_HAS_TANGENT
    if((material.lobeFlags & GPU_MATERIAL_LOBE_ANISOTROPY) != 0u)
    {
        surface.packedTangent = PathTracePrimaryTangent(pixel, camera.cameraPosition, vertex.position,
            vertex.materialIndex);
    }
#endif

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

// The primary vertex the raster-primary mode shades: decoded, lifted off the surface (PathTracePrimarySurfaceLift times
// the pixel's stored lift scale), then finalized exactly as PathTraceBuildSurface finalizes an RT hit.
bool PathTraceReconstructPrimaryVertex(uint2 pixel, PathTraceGBufferSample gBuffer, PathTracePrimaryCamera camera,
    StructuredBuffer<GPUMaterial> materials, uint materialTableIndex, uint materialAverageTableIndex,
    uint materialTableCosineMapping, out PathTracePrimaryVertex vertex)
{
    if(!PathTraceDecodePrimaryVertex(pixel, gBuffer, camera, materials, materialTableIndex, materialAverageTableIndex,
        materialTableCosineMapping, vertex))
    {
        return false;
    }

    float lift = PathTracePrimarySurfaceLift(pixel, gBuffer.depth, vertex.cameraRelativePosition,
        vertex.surface.geometricNormal, camera) * PathTracePrimaryLiftScale(gBuffer.geometricNormal.xyz);
    vertex.lift = lift;
    vertex.cameraRelativePosition += vertex.surface.geometricNormal * lift;
    vertex.position = camera.cameraPosition + vertex.cameraRelativePosition;
    return PathTraceFinalizeSurface(vertex.surface);
}

#endif
