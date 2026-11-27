#ifndef ENGINE_PRIMARY_SURFACE_LIFT
#define ENGINE_PRIMARY_SURFACE_LIFT

// How far a point decoded from the G-buffer's depth is moved along its geometric normal before rays start from it,
// shared by the path tracer's raster primary (PathTracePrimarySurface.hlsli), the G-buffer pass (Triangle.hlsl) and the
// passes that trace from the depth target with a world-space inverse view-projection (ray traced shadows, ambient
// occlusion, global illumination, reflections and the NRD confidence probes).
//
// The lift is a distance that depends only on the pixel (its depth and its distance from the camera), times a lift scale
// the G-buffer pass stores per pixel as the length of the geometric normal: 1 for an ordinary pixel, so every reader of
// that target normalizes it and none may filter it. Two kinds of pixel get a larger scale:
//   - a pixel whose decoded point lies measurably under its surface (PathTracePrimaryRasterLiftScale), on path-traced
//     frames with a raster primary: a power of two;
//   - a parallax occlusion mapping pixel whose depth is the relief's, below the mesh (PathTracePrimaryReliefLiftScale),
//     on every frame: any value, so the lifted point returns to the mesh surface and no further.

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

// The largest lift scale (fp16 holds a normal of this length, and every power of two up to it exactly).
static const float PATH_TRACE_PRIMARY_MAXIMUM_LIFT_SCALE = 32768.0f;

// A relief pixel's scale is never between 1 and this: the decode below reads lengths up to the square root of 2 as a
// unit normal, whatever its fp16 rounding.
static const float PATH_TRACE_PRIMARY_MINIMUM_RELIEF_LIFT_SCALE = 1.5f;

// A relief pixel's scale is stored this much larger than measured. The fp16 target keeps a length to about 0.1% and the
// decode may move it 0.2% onto a power of two, either of which could otherwise leave the lifted point short of the
// mesh surface. The cost is a lift 0.4% of the relief's depth too long (0.16 mm for 4 cm).
static const float PATH_TRACE_PRIMARY_RELIEF_LIFT_HEADROOM = 1.0f + 1.0f / 256.0f;

// How far a decoded point is lifted along its geometric normal, before the lift scale: the proportional lift, or the
// depth target's resolution at this pixel where that is larger. The resolution is an error along the view ray, so only
// its component along the normal counts (a grazing surface's point mostly slides within the surface). With W the
// unprojection's homogeneous coordinate, the distance changes by distance x |dW/d(depth)| / |W| per unit of depth.
// inverseViewProjectionRow3 is the last row of the camera's inverse view-projection, which is the same row whether or
// not the matrix is relative to the camera.
float PrimarySurfaceLiftDistance(uint2 pixel, uint2 resolution, float depth, float3 cameraRelativePosition,
    float3 geometricNormal, float4 inverseViewProjectionRow3)
{
    const float proportionalLift = PATH_TRACE_PRIMARY_SURFACE_LIFT * length(cameraRelativePosition);

    const float2 uv = (float2(pixel) + 0.5f) / float2(resolution);
    const float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    const float homogeneousW = dot(inverseViewProjectionRow3, ndc);
    const float depthLift = abs(dot(geometricNormal, cameraRelativePosition)) *
        abs(inverseViewProjectionRow3[2]) * PATH_TRACE_PRIMARY_DEPTH_RESOLUTION / abs(homogeneousW);

    // Near surfaces keep exactly the proportional lift they always had.
    return max(proportionalLift, depthLift);
}

// The lift scale the G-buffer pass stored for this pixel: the stored geometric normal's length. A unit normal gives
// exactly 1 and a power of two exactly itself, whatever their fp16 rounding (three fp16 components put the length within
// about 0.1% of the value written); any other length is a relief pixel's scale and is used as stored.
float PathTracePrimaryLiftScale(float3 storedGeometricNormal)
{
    const float lengthSquared = dot(storedGeometricNormal, storedGeometricNormal);

    if(!(lengthSquared > 2.0f))
    {
        return 1.0f;
    }

    const float storedScale = sqrt(lengthSquared);
    const float powerOfTwo = exp2(round(0.5f * log2(lengthSquared)));
    const float scale = abs(storedScale - powerOfTwo) <= powerOfTwo / 512.0f ? powerOfTwo : storedScale;

    return min(scale, PATH_TRACE_PRIMARY_MAXIMUM_LIFT_SCALE);
}

// For a pass that rebuilds worldPosition from the depth target with inverseViewProjection (world space) and starts rays
// there with its own small bias: the extra distance to move the origin along the geometric normal first. It is 0 for
// an ordinary pixel, whose origin stays exactly what the pass always used, and the whole scaled lift for a pixel with a
// stored lift scale, which puts a relief pixel's origin back on the mesh surface (PathTracePrimaryReliefLiftScale).
float PrimarySurfaceStoredLift(uint2 pixel, uint2 resolution, float depth, float3 worldPosition,
    float3 storedGeometricNormal, float4x4 inverseViewProjection)
{
    const float scale = PathTracePrimaryLiftScale(storedGeometricNormal);

    if(!(scale > 1.0f))
    {
        return 0.0f;
    }

    // The camera is where the unprojected view rays meet: the image of the clip-space point (0, 0, 1, 0).
    const float4 camera = float4(inverseViewProjection[0][2], inverseViewProjection[1][2], inverseViewProjection[2][2],
        inverseViewProjection[3][2]);
    const float3 cameraRelativePosition = worldPosition - camera.xyz / camera.w;
    const float3 geometricNormal = storedGeometricNormal * rsqrt(dot(storedGeometricNormal, storedGeometricNormal));
    const float lift = scale * PrimarySurfaceLiftDistance(pixel, resolution, depth, cameraRelativePosition,
        geometricNormal, inverseViewProjection[3]);

    // A non-finite lift would move the origin nowhere useful; the pass then keeps its own bias only.
    return isfinite(lift) ? lift : 0.0f;
}

#endif
