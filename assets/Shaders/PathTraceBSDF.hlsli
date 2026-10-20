#ifndef ENGINE_PATH_TRACE_BSDF
#define ENGINE_PATH_TRACE_BSDF

// Unified reflection BSDF of the path-traced reference (RTXPT.1, energy-conserving model RTXPT.1A.2).
// One definition serves evaluation, sampling and density for every path vertex, so Sample/Eval/Pdf agree by
// construction and a future RTXDI bridge (RAB_*) can call it. Legacy deferred/RTGI/reflection shading does not
// use this file and keeps its own model (see Documentation/agents/RTXPT1AEnergyConservingMaterialModel.md).
//
// Lobes (all reciprocal):
// - GGX single scatter D G F / (4 n.l n.v): stable D (PathTraceDistributionGGX), height-correlated Smith G,
//   Schlick or complex-IOR conductor Fresnel, roughness clamped to PATH_TRACE_MINIMUM_GGX_ROUGHNESS.
// - Multiple scatter fms = Fms M(l) M(v) / (PI Mavg),  Fms = Favg^2 (1 - Mavg) / (1 - Favg Mavg).
// - Diffuse fd = f_EON (1 - Es(l)) (1 - Es(v)) / (1 - Es_avg), with the total specular albedo
//   Es(mu) = F0 S(mu) + B(mu) + Fms M(mu) and S = 1 - M - B (Schlick models; conductors have no diffuse).
// - roughness <= PATH_TRACE_DELTA_ROUGHNESS: the single-scatter lobe becomes a delta mirror with weight
//   F(n.v); a mirror has no masking, so it has no multiple-scatter lobe and Es = F(mu) exactly.
//
// M (missing single-scatter energy, 1 - E) and B (Schlick bias) come from the bundled RTXPT.1A.1 tables
// (assets/MaterialTables/GGXSingleScatterComplement.emt, see Renderer/MaterialTables/MaterialTableFile.h).
// 1 - E is never formed: near-smooth surfaces have E within 1e-6 of 1 and float32 cancellation would
// destroy M. The cosine axis is read at u = sqrt(mu) for the squared mapping.
//
// Numerical limitation (accepted in RTXPT.1A.1): the diffuse normalization is exact for a Lambertian lobe
// only. With EON, white surfaces reach +0.16% / -0.66% of unit albedo. This is not exact energy
// conservation. Reciprocity is broken only by CorrectShadingNormalViewHorizon (n.v < 0.02), by design.
// All directions are world space and point away from the surface. Opaque reflection only.

#include "MaterialLighting.hlsli"
#include "ShadingNormal.hlsli"

static const float PATH_TRACE_DELTA_ROUGHNESS = 0.001f;
// Matches the clamp inside DistributionGGX/GeometrySmithGGX so sampled and evaluated alpha agree.
static const float PATH_TRACE_MINIMUM_GGX_ROUGHNESS = 0.045f;
static const float PATH_TRACE_MINIMUM_SELECTION = 0.1f;
// MaterialTables::CosineMapping values.
static const uint PATH_TRACE_TABLE_COSINE_LINEAR = 0u;
static const uint PATH_TRACE_TABLE_COSINE_SQUARED = 1u;

struct PathTraceSurface
{
    GPUMaterial material;   // Copy with roughness/metallic replaced by their resolved texture values.
    float3 baseColor;       // Resolved base color (factor * texture).
    float3 normal;          // Shading normal, horizon-corrected against the view direction.
    float3 geometricNormal; // Triangle normal, facing the view direction.
    float3 view;
    float normalDotView;

    bool deltaSpecular;
    float3 deltaFresnel;
    float specularProbability; // GGX-VNDF (or delta event) selection probability.

    uint materialTableIndex;          // Texture2D<float2> (M, B) by (cosine coordinate, roughness).
    uint materialAverageTableIndex;   // Texture2D<float2> (Mavg, Bavg) by roughness.
    uint materialTableCosineMapping;  // PATH_TRACE_TABLE_COSINE_*.
};

struct PathTraceBSDFSample
{
    bool valid;
    bool delta;
    float3 direction;
    // f * cos / pdf for non-delta events, F / P(delta) for the delta mirror.
    float3 weight;
    // Solid-angle density of the non-delta mixture at direction; 0 for a delta event.
    float pdf;
};

float PathTraceLuminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

// Bilinear table read with explicit clamp to texel centres. A wrapping sampler would blend the mu = 1 and
// r = 1 edges with mu = 0 / r = 0.
float2 PathTraceLoadLUT2(Texture2D<float2> lut, float2 uv)
{
    uint width;
    uint height;
    lut.GetDimensions(width, height);

    float2 texel = saturate(uv) * float2(width, height) - 0.5f;
    texel = clamp(texel, 0.0f, float2(width - 1u, height - 1u));
    int2 base = int2(floor(texel));
    int2 next = min(base + 1, int2(width - 1u, height - 1u));
    float2 blend = texel - float2(base);

    float2 top = lerp(lut.Load(int3(base.x, base.y, 0)), lut.Load(int3(next.x, base.y, 0)), blend.x);
    float2 bottom = lerp(lut.Load(int3(base.x, next.y, 0)), lut.Load(int3(next.x, next.y, 0)), blend.x);
    return lerp(top, bottom, blend.y);
}

// (M, B) at a cosine. Never forms 1 - E.
float2 PathTraceMissingEnergyAndBias(PathTraceSurface surface, float cosine)
{
    Texture2D<float2> table = ResourceDescriptorHeap[surface.materialTableIndex];
    float coordinate = surface.materialTableCosineMapping == PATH_TRACE_TABLE_COSINE_SQUARED ?
        sqrt(saturate(cosine)) : saturate(cosine);
    return max(PathTraceLoadLUT2(table, float2(coordinate, surface.material.roughness)), 0.0f);
}

// (Mavg, Bavg): cosine-weighted averages of the same bilinear reads.
float2 PathTraceAverageMissingEnergyAndBias(PathTraceSurface surface)
{
    Texture2D<float2> table = ResourceDescriptorHeap[surface.materialAverageTableIndex];
    return max(PathTraceLoadLUT2(table, float2(surface.material.roughness, 0.5f)), 0.0f);
}

float PathTraceGGXAlpha(float roughness)
{
    float perceptualRoughness = max(roughness, PATH_TRACE_MINIMUM_GGX_ROUGHNESS);
    return perceptualRoughness * perceptualRoughness;
}

float3 PathTraceSchlickF0(PathTraceSurface surface)
{
    if(surface.material.model == 0u)
    {
        return lerp(float3(0.04f, 0.04f, 0.04f), surface.baseColor, saturate(surface.material.metallic));
    }

    float iorDifference = surface.material.ior - 1.0f;
    float iorSum = surface.material.ior + 1.0f;
    float dielectricF0 = (iorDifference * iorDifference) / max(iorSum * iorSum, 1.0e-6f);
    return float3(dielectricF0, dielectricF0, dielectricF0);
}

float3 PathTraceDiffuseAlbedo(PathTraceSurface surface)
{
    if(surface.material.model == 0u)
    {
        return surface.baseColor * (1.0f - saturate(surface.material.metallic));
    }

    if(surface.material.model == 1u)
    {
        return surface.baseColor;
    }

    return float3(0.0f, 0.0f, 0.0f);
}

// Fresnel at normal-to-view incidence, used for the delta mirror weight and for lobe selection.
float3 PathTraceViewFresnel(PathTraceSurface surface)
{
    if(surface.material.model == 2u)
    {
        return FresnelConductor(surface.normalDotView, surface.material.conductorN, surface.material.conductorK);
    }

    return FresnelSchlick(surface.normalDotView, PathTraceSchlickF0(surface));
}

float3 PathTraceAverageFresnel(PathTraceSurface surface)
{
    return surface.material.model == 2u ? surface.material.averageConductorFresnel :
        AverageSchlickFresnel(PathTraceSchlickF0(surface));
}

// Fms = Favg^2 (1 - Mavg) / (1 - Favg Mavg), the legacy EvaluateGGXMultipleScatterFresnel in complement form.
float3 PathTraceMultipleScatterFresnel(PathTraceSurface surface, float averageMissing)
{
    float3 averageFresnel = saturate(PathTraceAverageFresnel(surface));
    return averageFresnel * averageFresnel * (1.0f - averageMissing) /
        max(1.0f - averageFresnel * averageMissing, 1.0e-6f);
}

float3 PathTraceEvaluateMultipleScatter(PathTraceSurface surface, float cosineLight)
{
    if(surface.deltaSpecular)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float averageMissing = PathTraceAverageMissingEnergyAndBias(surface).x;

    if(averageMissing <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float missingLight = PathTraceMissingEnergyAndBias(surface, cosineLight).x;
    float missingView = PathTraceMissingEnergyAndBias(surface, surface.normalDotView).x;
    return PathTraceMultipleScatterFresnel(surface, averageMissing) * missingLight * missingView /
        (PI * averageMissing);
}

// Total specular albedo Es(mu) = F0 S + B + Fms M for the Schlick models.
float3 PathTraceSpecularAlbedo(PathTraceSurface surface, float cosine)
{
    float3 F0 = PathTraceSchlickF0(surface);

    if(surface.deltaSpecular)
    {
        return FresnelSchlick(cosine, F0);
    }

    float2 missingBias = PathTraceMissingEnergyAndBias(surface, cosine);
    float scale = max(1.0f - missingBias.x - missingBias.y, 0.0f);
    float averageMissing = PathTraceAverageMissingEnergyAndBias(surface).x;
    return F0 * scale + missingBias.y + PathTraceMultipleScatterFresnel(surface, averageMissing) * missingBias.x;
}

float3 PathTraceAverageSpecularAlbedo(PathTraceSurface surface)
{
    float3 F0 = PathTraceSchlickF0(surface);

    if(surface.deltaSpecular)
    {
        return AverageSchlickFresnel(F0);
    }

    float2 average = PathTraceAverageMissingEnergyAndBias(surface);
    float scale = max(1.0f - average.x - average.y, 0.0f);
    return F0 * scale + average.y + PathTraceMultipleScatterFresnel(surface, average.x) * average.x;
}

// Reciprocal diffuse weight (1 - Es(l)) (1 - Es(v)) / (1 - Es_avg).
float3 PathTraceDiffuseWeight(PathTraceSurface surface, float cosineLight)
{
    float3 transmittedLight = saturate(1.0f - PathTraceSpecularAlbedo(surface, cosineLight));
    float3 transmittedView = saturate(1.0f - PathTraceSpecularAlbedo(surface, surface.normalDotView));
    float3 transmittedAverage = saturate(1.0f - PathTraceAverageSpecularAlbedo(surface));
    return transmittedLight * transmittedView / max(transmittedAverage, 1.0e-6f);
}

// Completes a surface whose material, baseColor, normals and view are already filled.
// Returns false when the view lies below the geometric or shading hemisphere.
bool PathTraceFinalizeSurface(inout PathTraceSurface surface)
{
    if(!all(isfinite(surface.view)) || !all(isfinite(surface.normal)) || !all(isfinite(surface.geometricNormal)))
    {
        return false;
    }

    if(dot(surface.geometricNormal, surface.view) <= 0.0f)
    {
        return false;
    }

    // Same horizon correction as the deferred path (enabled by default there).
    surface.normal = CorrectShadingNormalViewHorizon(surface.normal, surface.geometricNormal, surface.view);
    surface.normalDotView = dot(surface.normal, surface.view);

    if(!(surface.normalDotView > 0.0f))
    {
        return false;
    }

    surface.material.roughness = saturate(surface.material.roughness);
    surface.material.metallic = saturate(surface.material.metallic);
    surface.deltaSpecular = surface.material.roughness <= PATH_TRACE_DELTA_ROUGHNESS;
    surface.deltaFresnel = PathTraceViewFresnel(surface);

    // Selection estimates: multiple-scatter albedo Fms M(v) and the transmitted diffuse albedo.
    float3 multipleScatterAlbedo = surface.deltaSpecular ? float3(0.0f, 0.0f, 0.0f) :
        PathTraceMultipleScatterFresnel(surface, PathTraceAverageMissingEnergyAndBias(surface).x) *
        PathTraceMissingEnergyAndBias(surface, surface.normalDotView).x;
    float3 diffuseAlbedo = PathTraceDiffuseAlbedo(surface) *
        saturate(1.0f - PathTraceSpecularAlbedo(surface, surface.normalDotView));

    // Selection only changes variance: both strategies have full hemispherical support.
    float specularEstimate = max(PathTraceLuminance(surface.deltaFresnel), 0.0f);
    float cosineEstimate = max(PathTraceLuminance(diffuseAlbedo + multipleScatterAlbedo), 0.0f);

    if(cosineEstimate <= 0.0f)
    {
        surface.specularProbability = 1.0f;
    }
    else if(specularEstimate <= 0.0f)
    {
        surface.specularProbability = 0.0f;
    }
    else
    {
        surface.specularProbability = clamp(specularEstimate / (specularEstimate + cosineEstimate),
            PATH_TRACE_MINIMUM_SELECTION, 1.0f - PATH_TRACE_MINIMUM_SELECTION);
    }

    return all(isfinite(surface.deltaFresnel)) && isfinite(surface.specularProbability);
}

bool PathTraceDirectionAboveSurface(PathTraceSurface surface, float3 direction)
{
    return dot(surface.normal, direction) > 0.0f && dot(surface.geometricNormal, direction) > 0.0f;
}

// GGX normal distribution, algebraically identical to DistributionGGX:
//   alpha^2 / (PI ((n.h)^2 (alpha^2 - 1) + 1)^2) = alpha^2 / (PI ((n.h)^2 alpha^2 + |n x h|^2)^2).
// The legacy form cancels catastrophically in float32 when alpha is near the 0.045^2 floor and n.h is
// within a few ulps of 1; the cross-product form keeps full precision near the peak.
float PathTraceDistributionGGX(float3 normal, float3 halfVector, float roughness)
{
    float alpha = PathTraceGGXAlpha(roughness);
    float alphaSquared = alpha * alpha;
    float cosine = saturate(dot(normal, halfVector));
    float3 sine = cross(normal, halfVector);
    float denominator = cosine * cosine * alphaSquared + dot(sine, sine);
    return alphaSquared / max(PI * denominator * denominator, 1.0e-30f);
}

// GGX single scatter D G F / (4 n.l n.v) with the legacy G, F and denominator clamp, and the stable D.
float3 PathTraceEvaluateGGXSingleScatter(PathTraceSurface surface, float3 direction)
{
    float normalDotLight = saturate(dot(surface.normal, direction));
    float normalDotView = saturate(dot(surface.normal, surface.view));

    if(normalDotLight <= 0.0f || normalDotView <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 halfVector = normalize(direction + surface.view);
    float viewDotHalf = saturate(dot(surface.view, halfVector));
    float distribution = PathTraceDistributionGGX(surface.normal, halfVector, surface.material.roughness);
    float geometry = GeometrySmithGGX(normalDotLight, normalDotView, surface.material.roughness);
    float3 fresnel = surface.material.model == 2u ?
        FresnelConductor(viewDotHalf, surface.material.conductorN, surface.material.conductorK) :
        FresnelSchlick(viewDotHalf, PathTraceSchlickF0(surface));
    return distribution * geometry * fresnel / max(4.0f * normalDotLight * normalDotView, 1.0e-6f);
}

// Non-delta BSDF value f(l, v), without the cosine. Zero outside both hemispheres.
float3 PathTraceEvaluateBSDF(PathTraceSurface surface, float3 direction)
{
    if(!PathTraceDirectionAboveSurface(surface, direction))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float normalDotLight = dot(surface.normal, direction);

    // A delta mirror removes the single-scatter density; its energy is the delta event in PathTraceSampleBSDF.
    float3 singleScatter = surface.deltaSpecular ? float3(0.0f, 0.0f, 0.0f) :
        PathTraceEvaluateGGXSingleScatter(surface, direction);
    float3 multipleScatter = PathTraceEvaluateMultipleScatter(surface, normalDotLight);

    if(surface.material.model == 2u)
    {
        return singleScatter + multipleScatter;
    }

    float3 diffuse = EvaluateEONDiffuse(PathTraceDiffuseAlbedo(surface), surface.material.roughness,
        surface.normal, direction, surface.view);
    float3 diffuseWeight = PathTraceDiffuseWeight(surface, normalDotLight);
    return diffuseWeight * diffuse + singleScatter + multipleScatter;
}

void PathTraceBuildFrame(float3 normal, out float3 tangent, out float3 bitangent)
{
    // Duff et al. 2017, branchless orthonormal basis.
    float signZ = normal.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (signZ + normal.z);
    float b = normal.x * normal.y * a;
    tangent = float3(1.0f + signZ * normal.x * normal.x * a, signZ * b, -signZ * normal.x);
    bitangent = float3(b, signZ + normal.y * normal.y * a, -normal.y);
}

float PathTraceSmithG1(float alpha, float cosine)
{
    float alphaSquared = alpha * alpha;
    return 2.0f * cosine / max(cosine + sqrt(alphaSquared + (1.0f - alphaSquared) * cosine * cosine), 1.0e-7f);
}

// Density of reflecting v about a GGX visible normal (Heitz 2018), in solid angle of l.
float PathTraceVisibleNormalPdf(PathTraceSurface surface, float3 direction)
{
    float3 halfVector = surface.view + direction;
    float halfLengthSquared = dot(halfVector, halfVector);

    if(halfLengthSquared <= 1.0e-12f)
    {
        return 0.0f;
    }

    halfVector *= rsqrt(halfLengthSquared);

    if(dot(surface.view, halfVector) <= 0.0f || dot(surface.normal, halfVector) <= 0.0f)
    {
        return 0.0f;
    }

    float alpha = PathTraceGGXAlpha(surface.material.roughness);
    float distribution = PathTraceDistributionGGX(surface.normal, halfVector, surface.material.roughness);
    return PathTraceSmithG1(alpha, surface.normalDotView) * distribution / (4.0f * surface.normalDotView);
}

// Density with which PathTraceSampleBSDF generates a non-delta direction.
float PathTracePdf(PathTraceSurface surface, float3 direction)
{
    float cosinePdf = max(dot(surface.normal, direction), 0.0f) * INV_PI;

    if(surface.deltaSpecular)
    {
        return (1.0f - surface.specularProbability) * cosinePdf;
    }

    return surface.specularProbability * PathTraceVisibleNormalPdf(surface, direction) +
        (1.0f - surface.specularProbability) * cosinePdf;
}

float3 PathTraceSampleVisibleNormal(PathTraceSurface surface, float2 random)
{
    float3 tangent;
    float3 bitangent;
    PathTraceBuildFrame(surface.normal, tangent, bitangent);

    float alpha = PathTraceGGXAlpha(surface.material.roughness);
    float3 localView = float3(dot(surface.view, tangent), dot(surface.view, bitangent), surface.normalDotView);
    float3 stretchedView = normalize(float3(alpha * localView.x, alpha * localView.y, localView.z));

    float lengthSquared = stretchedView.x * stretchedView.x + stretchedView.y * stretchedView.y;
    float3 axis1 = lengthSquared > 0.0f ?
        float3(-stretchedView.y, stretchedView.x, 0.0f) * rsqrt(lengthSquared) : float3(1.0f, 0.0f, 0.0f);
    float3 axis2 = cross(stretchedView, axis1);

    float radius = sqrt(random.x);
    float angle = 2.0f * PI * random.y;
    float t1 = radius * cos(angle);
    float t2 = radius * sin(angle);
    float blend = 0.5f * (1.0f + stretchedView.z);
    t2 = (1.0f - blend) * sqrt(max(1.0f - t1 * t1, 0.0f)) + blend * t2;

    float3 stretchedNormal = t1 * axis1 + t2 * axis2 +
        sqrt(max(1.0f - t1 * t1 - t2 * t2, 0.0f)) * stretchedView;
    float3 localNormal = normalize(float3(alpha * stretchedNormal.x, alpha * stretchedNormal.y,
        max(stretchedNormal.z, 0.0f)));

    return normalize(localNormal.x * tangent + localNormal.y * bitangent + localNormal.z * surface.normal);
}

float3 PathTraceSampleCosine(PathTraceSurface surface, float2 random)
{
    float3 tangent;
    float3 bitangent;
    PathTraceBuildFrame(surface.normal, tangent, bitangent);

    float radius = sqrt(random.x);
    float angle = 2.0f * PI * random.y;
    float3 local = float3(radius * cos(angle), radius * sin(angle), sqrt(max(1.0f - random.x, 0.0f)));
    return normalize(local.x * tangent + local.y * bitangent + local.z * surface.normal);
}

// random.x selects the strategy; random.yz drive the chosen strategy.
PathTraceBSDFSample PathTraceSampleBSDF(PathTraceSurface surface, float3 random)
{
    PathTraceBSDFSample result;
    result.valid = false;
    result.delta = false;
    result.direction = surface.normal;
    result.weight = float3(0.0f, 0.0f, 0.0f);
    result.pdf = 0.0f;

    bool chooseSpecular = random.x < surface.specularProbability;

    if(surface.deltaSpecular && chooseSpecular)
    {
        result.direction = reflect(-surface.view, surface.normal);
        result.delta = true;

        if(dot(surface.geometricNormal, result.direction) <= 0.0f || surface.specularProbability <= 0.0f)
        {
            return result;
        }

        result.weight = surface.deltaFresnel / surface.specularProbability;
        result.valid = all(isfinite(result.weight));
        return result;
    }

    if(chooseSpecular)
    {
        float3 halfVector = PathTraceSampleVisibleNormal(surface, random.yz);
        result.direction = reflect(-surface.view, halfVector);
    }
    else
    {
        result.direction = PathTraceSampleCosine(surface, random.yz);
    }

    if(!PathTraceDirectionAboveSurface(surface, result.direction))
    {
        // A zero-valued event: the density still counts it, so no renormalization is applied.
        return result;
    }

    result.pdf = PathTracePdf(surface, result.direction);

    if(!(result.pdf > 0.0f) || !isfinite(result.pdf))
    {
        return result;
    }

    float3 value = PathTraceEvaluateBSDF(surface, result.direction);
    result.weight = value * dot(surface.normal, result.direction) / result.pdf;
    result.valid = all(isfinite(result.weight));
    return result;
}

#endif
