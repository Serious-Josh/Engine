#ifndef ENGINE_SURFACE_DETAIL_HLSLI
#define ENGINE_SURFACE_DETAIL_HLSLI

// Parallax occlusion mapping (POM), shared by the G-buffer pass (Triangle.hlsl) and the material preview
// (MaterialPreview.hlsl). Design: Documentation/Rendering/POM_Improvement_Investigation.md.
//
// Heights are world distances along the surface normal: height = heightScale * h + heightBias, h the height texture's
// red channel in [0, 1], 0 the mesh surface. The march finds where the view ray meets that height field and returns the
// UV there. `referenceHeight` is the height the rendered geometry already represents at this pixel: 0 for an ordinary
// mesh; a displaced mesh will pass its interpolated vertex height, so the same march covers the residual relief.
//
// The caller must sample every material texture at the returned UV with SurfaceDetailSample, which uses SampleGrad with
// the derivatives in SurfaceDetailResult where its explicitGradients says so. The displaced UV jumps between
// neighbouring pixels at height edges, so its implicit derivatives would select a far too small mip there. The
// undisplaced UV's derivatives are wrong the other way: at a low view angle they blur the sides of ridges, which face
// the viewer and fill most of what is seen of the relief, as much as the flat surface around them. The march therefore
// returns the displaced UV's own derivatives, worked out from the height field's slope at the hit.

#include "MaterialLighting.hlsli"

// Relief fades out towards grazing views, where the UV offset grows without bound: fully on from this cosine up...
static const float SurfaceDetailGrazingFadeEnd = 0.08f;
// ...and off below this one (about 1.1 degrees from grazing).
static const float SurfaceDetailGrazingFadeStart = 0.02f;
// Relief also fades out, but only where the whole parallax shift is well under a pixel (a shift of one or two pixels is
// still clearly visible, more so when DLSS renders at a lower internal resolution). The march is skipped below the lower
// bound: distant surfaces then cost nothing.
static const float SurfaceDetailPixelFadeStart = 0.1f;
static const float SurfaceDetailPixelFadeEnd = 0.25f;
// The linear search takes about one step per height texel the ray crosses at the sampled mip, within these bounds, and
// never fewer than SurfaceDetailObliqueSteps asks for (more steps the lower the view angle and the longer the ray's
// path in UV, whatever the mip).
static const uint SurfaceDetailMinimumSteps = 8u;
static const uint SurfaceDetailMaximumSteps = 48u;
// A slope facing away from the viewer stretches the displaced UV's derivatives without bound as it turns edge-on; they
// grow to at most this multiple of the undisplaced ones.
static const float SurfaceDetailMaximumForeshortening = 1.0f;
// The UV offset never exceeds this distance (in UV units); taller relief is scaled down to fit.
static const float SurfaceDetailMaximumUVOffset = 1.0f;

struct SurfaceDetailResult
{
    // Where to sample the material.
    float2 uv;
    // The visible point's height above the mesh surface in world units (after the fades), for depth and shadowing.
    float height;
    // The cosine between the view direction and the interpolated normal the height is measured along (0 without relief).
    float viewCosine;
    // 0 to 1: how much of the authored relief is applied here.
    float fade;
    // False where the UV is the interpolated one (no relief).
    bool displaced;
    // True for every pixel of a parallax material, with or without relief: the whole surface samples with the derivatives
    // below, so no 2x2 quad mixes implicit and explicit derivatives (the implicit ones would be wrong there).
    bool explicitGradients;
    // The screen derivatives of `uv` on the relief (the undisplaced UV's where there is no relief).
    float2 uvDx;
    float2 uvDy;
};

// The fewest linear steps for a view: 8 head on, 32 at grazing angles or where the ray crosses a whole UV unit.
uint SurfaceDetailObliqueSteps(float uvTravel, float viewCosine)
{
    return min(8u + uint(ceil(24.0f * max(saturate(uvTravel), 1.0f - saturate(viewCosine)))), 32u);
}

bool SurfaceDetailUsesParallax(GPUMaterial material)
{
    // GeometryWithParallaxFallback renders as its fallback until displaced geometry exists.
    return (material.surfaceDetailMode == SurfaceDetailModeParallaxOcclusion ||
            material.surfaceDetailMode == SurfaceDetailModeGeometryWithParallaxFallback) &&
        material.heightTextureIndex != 0xFFFFFFFFu && abs(material.heightScale) > 1.0e-6f;
}

// The UV a ray crosses per unit of height gained, for W its sideways travel per unit of height (in the tangent plane of
// the unit normal N). positionDx/Dy and uvDx/Dy are the screen derivatives of the world position and UV (any consistent
// pair of surface directions works). `tangent` is the interpolated world tangent with its handedness; it gives the UV
// axes their direction where it agrees with the UV mapping, so the offset varies smoothly over curved meshes instead of
// jumping at triangle edges. The result is linear in W. Returns false where no stable mapping exists.
bool SurfaceDetailUVPerTravel(float3 N, float4 tangent, float3 positionDx, float3 positionDy, float2 uvDx,
    float2 uvDy, float3 W, out float2 uvPerHeight)
{
    uvPerHeight = float2(0.0f, 0.0f);

    // World travel per unit of U and of V, from the UV Jacobian. Normalizing by the UV scale first keeps the determinant
    // test relative. Signed determinants preserve mirrored charts.
    float uvScale = max(max(abs(uvDx.x), abs(uvDx.y)), max(abs(uvDy.x), abs(uvDy.y)));

    if(!(uvScale > 1.0e-15f))
    {
        return false;
    }

    float2 Ux = uvDx / uvScale;
    float2 Uy = uvDy / uvScale;
    float jacobian = Ux.x * Uy.y - Ux.y * Uy.x;

    if(!(abs(jacobian) > 1.0e-6f * (dot(Ux, Ux) + dot(Uy, Uy))))
    {
        return false;
    }

    float3 Pu = (positionDx * Uy.y - positionDy * Ux.y) / (jacobian * uvScale);
    float3 Pv = (positionDy * Ux.x - positionDx * Uy.x) / (jacobian * uvScale);
    // Into the interpolated normal's tangent plane.
    Pu -= N * dot(N, Pu);
    Pv -= N * dot(N, Pv);
    float lengthU = length(Pu);
    float lengthV = length(Pv);

    if(!(lengthU > 1.0e-15f) || !(lengthV > 1.0e-15f))
    {
        return false;
    }

    // Preferred: the vertex tangent frame for directions, the Jacobian for UV density only.
    float3 T = tangent.xyz - N * dot(N, tangent.xyz);
    float tangentLengthSquared = dot(T, T);

    if(tangentLengthSquared > 1.0e-12f)
    {
        T *= rsqrt(tangentLengthSquared);
        float3 B = cross(N, T);
        float alignU = dot(T, Pu) / lengthU;
        float alignV = dot(B, Pv) / lengthV;

        // Only where the tangent really follows U and the UV axes are close to perpendicular.
        if(abs(alignU) > 0.9f && abs(alignV) > 0.9f)
        {
            uvPerHeight = float2(dot(W, T) * sign(alignU) / lengthU, dot(W, B) * sign(alignV) / lengthV);
            return all(isfinite(uvPerHeight));
        }
    }

    // General mapping (sheared or tangent-less UVs): solve with the metric of Pu and Pv.
    float g00 = lengthU * lengthU;
    float g01 = dot(Pu, Pv);
    float g11 = lengthV * lengthV;
    float determinant = g00 * g11 - g01 * g01;

    if(!(determinant > 1.0e-6f * g00 * g11))
    {
        return false;
    }

    float2 rhs = float2(dot(Pu, W), dot(Pv, W));
    uvPerHeight = float2(g11 * rhs.x - g01 * rhs.y, g00 * rhs.y - g01 * rhs.x) / determinant;
    return all(isfinite(uvPerHeight));
}

// UV travel per unit of height along the view ray, in the frame of the interpolated normal, and the cosine between that
// normal and the view direction (SurfaceDetailUVPerTravel for the view ray). Returns false where no stable mapping
// exists (the caller keeps the plain UV).
bool SurfaceDetailUVPerHeight(float3 normal, float4 tangent, float3 viewDirection, float3 positionDx,
    float3 positionDy, float2 uvDx, float2 uvDy, out float2 uvPerHeight, out float viewCosine)
{
    uvPerHeight = float2(0.0f, 0.0f);
    viewCosine = 0.0f;

    float normalLengthSquared = dot(normal, normal);

    if(!(normalLengthSquared > 1.0e-12f))
    {
        return false;
    }

    float3 N = normal * rsqrt(normalLengthSquared);
    viewCosine = dot(viewDirection, N);

    if(!(viewCosine > SurfaceDetailGrazingFadeStart))
    {
        return false;
    }

    // The view ray's sideways travel per unit of height.
    float3 W = viewDirection / viewCosine - N;

    return SurfaceDetailUVPerTravel(N, tangent, positionDx, positionDy, uvDx, uvDy, W, uvPerHeight);
}

// Marches the view ray through the material's height field. uvDx/uvDy are the undisplaced UV's screen derivatives (they
// choose the height mip and measure the pixel's footprint).
SurfaceDetailResult SurfaceDetailMarch(GPUMaterial material, SamplerState heightSampler, float2 uv, float2 uvDx,
    float2 uvDy, float2 uvPerHeight, float viewCosine, float referenceHeight)
{
    SurfaceDetailResult result;
    result.uv = uv;
    result.height = referenceHeight;
    result.viewCosine = 0.0f;
    result.fade = 0.0f;
    result.displaced = false;
    // Only parallax materials are marched.
    result.explicitGradients = true;
    result.uvDx = uvDx;
    result.uvDy = uvDy;

    // The relief's extent relative to the rendered geometry.
    float heightEnd = material.heightBias + material.heightScale;
    float low = min(material.heightBias, heightEnd) - referenceHeight;
    float high = max(material.heightBias, heightEnd) - referenceHeight;
    float travel = length(uvPerHeight) * (high - low);
    float footprintMajor = max(length(uvDx), length(uvDy));
    float footprintMinor = min(length(uvDx), length(uvDy));

    float fade = smoothstep(SurfaceDetailGrazingFadeStart, SurfaceDetailGrazingFadeEnd, viewCosine);
    fade *= smoothstep(SurfaceDetailPixelFadeStart, SurfaceDetailPixelFadeEnd, travel / max(footprintMinor, 1.0e-20f));
    float maximumOffset = length(uvPerHeight) * max(abs(low), abs(high)) * fade;

    if(maximumOffset > SurfaceDetailMaximumUVOffset)
    {
        fade *= SurfaceDetailMaximumUVOffset / maximumOffset;
    }

    // Also rejects non-finite inputs: every comparison with a NaN is false.
    if(!(fade > 0.0f) || !(fade <= 1.0f) || !all(isfinite(uv + uvPerHeight * (high * fade))) ||
        !all(isfinite(uv + uvPerHeight * (low * fade))))
    {
        return result;
    }

    Texture2D<float4> heightTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.heightTextureIndex)];
    uint width;
    uint height;
    heightTexture.GetDimensions(width, height);
    float texelsPerUV = float(max(width, height));
    // Texels the ray crosses at the mip SampleGrad selects (mip 0 while the texture is magnified).
    float texelTravel = travel * fade / max(footprintMajor, 1.0f / max(texelsPerUV, 1.0f));
    uint stepCount = clamp(uint(ceil(min(texelTravel, float(SurfaceDetailMaximumSteps)))), SurfaceDetailMinimumSteps,
        SurfaceDetailMaximumSteps);
    stepCount = max(stepCount, SurfaceDetailObliqueSteps(travel, viewCosine));

    float scale = material.heightScale * fade;
    float offset = (material.heightBias - referenceHeight) * fade;
    float top = high * fade;
    float bottom = low * fade;

    // Linear search down from the top of the relief for the first sample under the surface.
    float previousRay = top;
    float previousSurface = scale * heightTexture.SampleGrad(heightSampler, uv + uvPerHeight * top, uvDx, uvDy).r +
        offset;
    float ray = top;
    float surface = previousSurface;

    if(previousSurface < previousRay)
    {
        [loop]
        for(uint step = 1u; step <= stepCount; ++step)
        {
            ray = lerp(top, bottom, float(step) / float(stepCount));
            surface = scale * heightTexture.SampleGrad(heightSampler, uv + uvPerHeight * ray, uvDx, uvDy).r + offset;

            if(ray <= surface)
            {
                break;
            }

            previousRay = ray;
            previousSurface = surface;
        }

        // Bisection narrows the bracket onto the surface, so the edge of a cliff (a grid bar, a brick) stays an edge.
        // Without a bracket the ray left the relief's extent above the surface and ends at the bottom.
        if(ray <= surface)
        {
            [unroll]
            for(uint refinement = 0u; refinement < 4u; ++refinement)
            {
                float middle = 0.5f * (previousRay + ray);
                float middleSurface = scale * heightTexture.SampleGrad(heightSampler, uv + uvPerHeight * middle, uvDx,
                    uvDy).r + offset;

                if(middle <= middleSurface)
                {
                    ray = middle;
                    surface = middleSurface;
                }
                else
                {
                    previousRay = middle;
                    previousSurface = middleSurface;
                }
            }
        }

        // Secant across what is left: where the straight lines through the bracket's two samples cross. `above` and
        // `below` are the ray's clearances over and under the surface at the bracket's ends.
        float above = max(previousRay - previousSurface, 0.0f);
        float below = max(surface - ray, 0.0f);
        ray = lerp(previousRay, ray, above / max(above + below, 1.0e-20f));
    }

    float2 displacedUV = uv + uvPerHeight * ray;

    if(!all(isfinite(displacedUV)))
    {
        return result;
    }

    result.uv = displacedUV;
    result.height = referenceHeight + ray;
    result.viewCosine = viewCosine;
    result.fade = fade;
    result.displaced = true;

    // The displaced UV's screen derivatives. With q = uvPerHeight, the hit satisfies uv' = uv + q h and h = H(uv'), H the
    // height field, so d(uv') = d(uv) + q (grad H . d(uv)) / (1 - grad H . q). The slope is taken across one texel of
    // the sampled mip. On a side facing the viewer the quotient cancels d(uv) along the view direction (the side is seen
    // almost face on, so its texture is sharp); on a slope facing away it grows, up to the limit.
    float texel = max(footprintMajor, 1.0f / max(texelsPerUV, 1.0f));
    float2 texelU = float2(0.5f * texel, 0.0f);
    float2 texelV = float2(0.0f, 0.5f * texel);
    float2 slope = float2(
        heightTexture.SampleGrad(heightSampler, displacedUV + texelU, uvDx, uvDy).r -
        heightTexture.SampleGrad(heightSampler, displacedUV - texelU, uvDx, uvDy).r,
        heightTexture.SampleGrad(heightSampler, displacedUV + texelV, uvDx, uvDy).r -
        heightTexture.SampleGrad(heightSampler, displacedUV - texelV, uvDx, uvDy).r) * (scale / texel);
    float foreshortening = min(1.0f / max(1.0f - dot(slope, uvPerHeight), 1.0e-3f), SurfaceDetailMaximumForeshortening);

    if(all(isfinite(slope)))
    {
        result.uvDx = uvDx + uvPerHeight * (dot(slope, uvDx) * foreshortening);
        result.uvDy = uvDy + uvPerHeight * (dot(slope, uvDy) * foreshortening);
    }

    return result;
}

// The whole of it for a rasterized pixel. The derivatives must be taken by the caller before any divergent control flow.
SurfaceDetailResult SurfaceDetailResolve(GPUMaterial material, SamplerState heightSampler, float2 uv, float2 uvDx,
    float2 uvDy, float3 positionDx, float3 positionDy, float3 normal, float4 tangent, float3 viewDirection,
    float referenceHeight)
{
    SurfaceDetailResult result;
    result.uv = uv;
    result.height = referenceHeight;
    result.viewCosine = 0.0f;
    result.fade = 0.0f;
    result.displaced = false;
    result.explicitGradients = false;
    result.uvDx = uvDx;
    result.uvDy = uvDy;

    if(!SurfaceDetailUsesParallax(material))
    {
        return result;
    }

    // From here on every return path keeps it, including the ones without relief.
    result.explicitGradients = true;
    float2 uvPerHeight;
    float viewCosine;

    if(!SurfaceDetailUVPerHeight(normal, tangent, viewDirection, positionDx, positionDy, uvDx, uvDy, uvPerHeight,
        viewCosine))
    {
        return result;
    }

    return SurfaceDetailMarch(material, heightSampler, uv, uvDx, uvDy, uvPerHeight, viewCosine, referenceHeight);
}

// The world offset from the rasterized point to the point the view ray meets on the relief. viewDirection is the unit
// direction to the camera that SurfaceDetailResolve was given: a point at height h lies h / viewCosine along it. Only
// relief below the mesh surface moves the point. Relief above it would lie in front of the triangle, where neither a
// conservative depth output nor the mesh's silhouette can follow, so such a pixel stays on the triangle.
float3 SurfaceDetailWorldOffset(SurfaceDetailResult detail, float3 viewDirection)
{
    if(!detail.displaced || !(detail.height < 0.0f) || !(detail.viewCosine > 0.0f))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    return viewDirection * (detail.height / detail.viewCosine);
}

// A material texture at a SurfaceDetailResult's UV. `mipBias` is the caller's LOD bias (DLSS), 0 for none. Materials
// without parallax keep the plain Sample / SampleBias they always used.
float4 SurfaceDetailSample(Texture2D<float4> materialTexture, SamplerState materialSampler, SurfaceDetailResult detail,
    float mipBias)
{
    if(detail.explicitGradients)
    {
        float gradientScale = exp2(mipBias);
        return materialTexture.SampleGrad(materialSampler, detail.uv, detail.uvDx * gradientScale,
            detail.uvDy * gradientScale);
    }

    if(mipBias != 0.0f)
    {
        return materialTexture.SampleBias(materialSampler, detail.uv, mipBias);
    }

    return materialTexture.Sample(materialSampler, detail.uv);
}

// ---------------------------------------------------------------------------------------------------------------------
// Self-shadowing: whether the relief itself blocks a light from a point on it. For the passes that light a pixel after
// the G-buffer pass (ray traced shadows, the path tracer's next-event estimation at the raster primary). The relief lies
// below the mesh that is in the acceleration structure, so no traced ray sees it: this march covers the part of the
// light's path inside the relief, and the traced ray, which starts on the mesh surface, covers the rest.
//
// The caller recovers the point from what the G-buffer already holds. The mesh point directly above a relief point
// along the normal has exactly the relief point's UV (that is what the UV travel per height means), and the distance
// between the two is the height: so a pass needs the triangle under the lifted origin (from the identity target or one
// short ray) and the decoded depth, and no further G-buffer target.
// ---------------------------------------------------------------------------------------------------------------------

// The march towards the light takes about one step per height texel crossed at the sampled mip, within these bounds.
static const uint SurfaceDetailShellMinimumSteps = 6u;
static const uint SurfaceDetailShellMaximumSteps = 24u;

// A point where the height texture changes by more than this (0 to 1) across one texel of the sampled mip is on the wall
// of a step, which the relief does not shadow (SurfaceDetailShellClearance).
static const float SurfaceDetailShellWallSlope = 0.25f;

// The clearance of a light the relief cannot block (no relief above the point, or nothing to judge): a light overhead.
static const float SurfaceDetailShellOpenClearance = 1.5707964f;

// The part of a material the shell march reads, so a caller can keep it without the whole material.
struct SurfaceDetailHeightField
{
    uint textureIndex;
    float scale;
    float bias;
};

SurfaceDetailHeightField SurfaceDetailHeightFieldOf(GPUMaterial material)
{
    SurfaceDetailHeightField field;
    field.textureIndex = material.heightTextureIndex;
    field.scale = material.heightScale;
    field.bias = material.heightBias;
    return field;
}

// How far the light stands above the relief's horizon in its direction, in radians: positive where nothing of the relief
// stands between the point and the light, negative where the relief hides it.
//
// hitUV and hitHeight are the point (its height at or below 0, the mesh surface). uvFootprint is the size of the pixel
// in UV, which chooses the height mip. lightUVPerHeight and lightCosine are SurfaceDetailUVPerHeight's results for the
// direction to the light: the UV its ray crosses per unit of height gained, and its cosine against the normal.
float SurfaceDetailShellClearance(SurfaceDetailHeightField field, SamplerState heightSampler, float2 hitUV,
    float hitHeight, float uvFootprint, float2 lightUVPerHeight, float lightCosine)
{
    Texture2D<float4> heightTexture = ResourceDescriptorHeap[NonUniformResourceIndex(field.textureIndex)];
    uint width;
    uint height;
    heightTexture.GetDimensions(width, height);
    float texelsPerUV = float(max(width, height));
    float mip = log2(max(uvFootprint * texelsPerUV, 1.0f));

    // The G-buffer pass scaled the relief by its fades (grazing view, distance, the offset limit), which need the screen
    // derivatives this pass does not have. The point itself gives the factor back: the faded surface at hitUV is at
    // hitHeight.
    float authoredHeight = field.scale * heightTexture.SampleLevel(heightSampler, hitUV, mip).r + field.bias;
    float fade = authoredHeight < -1.0e-6f ? saturate(hitHeight / authoredHeight) : 1.0f;
    float scale = field.scale * fade;
    float offset = field.bias * fade;
    float top = max(field.bias, field.bias + field.scale) * fade;

    // The G-buffer pass found the point on its own filtered height field (SampleGrad with the pixel's derivatives),
    // which near a steep step lies millimetres below this pass's: the point then sits under this pass's surface, every
    // direction looks blocked, and thin dark lines ran along the steps. The march therefore starts on this pass's
    // surface where that is higher.
    float startHeight = max(hitHeight, scale * heightTexture.SampleLevel(heightSampler, hitUV, mip).r + offset);
    float climb = top - startHeight;

    // Also the answer for a non-finite point: every comparison with a NaN is false.
    if(!(climb > 0.0f) || !(lightCosine > 0.0f) || !all(isfinite(lightUVPerHeight)))
    {
        return SurfaceDetailShellOpenClearance;
    }

    float texel = max(uvFootprint, 1.0f / max(texelsPerUV, 1.0f));

    // A point on the wall of a step (the height field changes by a large share of the relief's depth within one texel)
    // is drawn with POM's stretched wall texels and its normal map already darkens it; its own step above it would
    // shadow it once more, and tiny differences in where the point lands on the wall turned that into ragged dark dashes.
    // Walls are left unshadowed by the relief.
    float2 texelU = float2(0.5f * texel, 0.0f);
    float2 texelV = float2(0.0f, 0.5f * texel);
    float2 slope = float2(
        heightTexture.SampleLevel(heightSampler, hitUV + texelU, mip).r -
        heightTexture.SampleLevel(heightSampler, hitUV - texelU, mip).r,
        heightTexture.SampleLevel(heightSampler, hitUV + texelV, mip).r -
        heightTexture.SampleLevel(heightSampler, hitUV - texelV, mip).r);

    if(max(abs(slope.x), abs(slope.y)) > SurfaceDetailShellWallSlope)
    {
        return SurfaceDetailShellOpenClearance;
    }

    float texelTravel = length(lightUVPerHeight) * climb / texel;
    uint stepCount = clamp(uint(ceil(min(texelTravel, float(SurfaceDetailShellMaximumSteps)))),
        SurfaceDetailShellMinimumSteps, SurfaceDetailShellMaximumSteps);

    // How high the relief reaches along the way, as a share of the height the light's ray has gained there: 1 is a
    // surface exactly on the ray, more is above it. The largest share is the relief's horizon in the light's direction.
    float horizon = 0.0f;

    [loop]
    for(uint step = 1u; step <= stepCount; ++step)
    {
        float gained = climb * float(step) / float(stepCount);
        float surface = scale * heightTexture.SampleLevel(heightSampler, hitUV + lightUVPerHeight * gained, mip).r +
            offset;
        horizon = max(horizon, (surface - startHeight) / gained);
    }

    // As angles above the mesh surface: the light's elevation and the horizon's. Both rise over the same distance along
    // the surface, so the horizon's tangent is the light's times the share.
    float lightSine = sqrt(saturate(1.0f - lightCosine * lightCosine));
    float lightTangent = lightCosine / max(lightSine, 1.0e-4f);
    return atan(lightTangent) - atan(horizon * lightTangent);
}

// 1 where nothing of the relief stands between the point and the light, 0 where the relief hides it, in between across
// `softness` radians around the relief's horizon (the light's angular radius, or a small angle that hides the steps
// when the caller already samples a direction inside the light). The arguments are SurfaceDetailShellClearance's.
float SurfaceDetailShellVisibility(SurfaceDetailHeightField field, SamplerState heightSampler, float2 hitUV,
    float hitHeight, float uvFootprint, float2 lightUVPerHeight, float lightCosine, float softness)
{
    float clearance = SurfaceDetailShellClearance(field, heightSampler, hitUV, hitHeight, uvFootprint, lightUVPerHeight,
        lightCosine);
    return saturate(0.5f + 0.5f * clearance / max(softness, 1.0e-4f));
}

// SurfaceDetailUVPerTravel as a pair of world vectors, for a caller that keeps the mapping of one point and applies it to
// many directions: the UV per unit of height gained is (dot(axisU, W), dot(axisV, W)). The arguments are
// SurfaceDetailUVPerTravel's. Returns false where no stable mapping exists.
bool SurfaceDetailUVAxes(float3 N, float4 tangent, float3 positionDx, float3 positionDy, float2 uvDx, float2 uvDy,
    out float3 axisU, out float3 axisV)
{
    float2 alongX;
    float2 alongY;
    float2 alongZ;
    bool valid = SurfaceDetailUVPerTravel(N, tangent, positionDx, positionDy, uvDx, uvDy, float3(1.0f, 0.0f, 0.0f),
            alongX) &&
        SurfaceDetailUVPerTravel(N, tangent, positionDx, positionDy, uvDx, uvDy, float3(0.0f, 1.0f, 0.0f), alongY) &&
        SurfaceDetailUVPerTravel(N, tangent, positionDx, positionDy, uvDx, uvDy, float3(0.0f, 0.0f, 1.0f), alongZ);
    axisU = float3(alongX.x, alongY.x, alongZ.x);
    axisV = float3(alongX.y, alongY.y, alongZ.y);
    return valid;
}

#endif
