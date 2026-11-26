#ifndef ENGINE_PATH_TRACE_BSDF
#define ENGINE_PATH_TRACE_BSDF

// The path tracer's BSDF: OpenPBR Surface 1.1.2 reduced to a mixture of lobes (OpenPBR "Reduction to a mixture of
// lobes"; Documentation/Rendering/OpenPBR_Material_Gap_Analysis.md). One definition serves evaluation, sampling and
// density for every path vertex of the Reference, ReSTIR DI and ReSTIR PT modes, so Sample/Eval/Pdf agree by
// construction. Legacy deferred/RTGI/reflection shading keeps its own model (MaterialLighting.hlsli).
//
// Opaque reflection only (transmission, subsurface and emission are later work):
//
//   f = fuzz_weight f_fuzz + coatScale f_coat + A(l, v) (f_spec + f_ms + f_diffuse)
//
// - Base specular f_spec = D G2 F_base(v.h) / (4 n.l n.v): stable GGX D (anisotropic with OpenPBR's alphas, Eq. 20),
//   height-correlated Smith G2, roughness clamped to PATH_TRACE_MINIMUM_GGX_ROUGHNESS. The base Fresnel is OpenPBR's
//   statistical metalness mix F_base = (1 - M) specular_color Schlick(F0_d) + M F82(base_color, specular_color), with
//   F0_d the dielectric F0 from specular_ior, specular_weight and the coat (CPU-derived), and optionally a thin film.
// - Multiple scatter f_ms = Fms M(l) M(v) / (PI Mavg) per mixed part (Kulla-Conty in complement form): Fms of the
//   tinted dielectric and of the metal from their average Fresnels.
// - Diffuse f_diffuse = (1 - M) f_EON(base_weight base_color, base_diffuse_roughness) (1 - Es(l)) (1 - Es(v)) /
//   (1 - Es_avg), the reciprocal Kelemen layering with the untinted dielectric specular albedo Es(mu) = F0_d S + B +
//   Fms_d M (OpenPBR glossy-diffuse, Eq. 42). Base diffuse roughness 0 is Lambertian, and exact.
// - Coat f_coat = D_c G_c Schlick(coatF0) / (4 n_c.l n_c.v), its own roughness and normal (smooth or the base normal).
//   The base below sees A(l, v) = fuzzFactor ((1 - C) + C T(l, v) (1 - E_c(v)) lerp(1, darkening, coat_darkening)):
//   non-reciprocal albedo scaling by the coat's single-scatter albedo E_c(v), the view-dependent coat absorption T
//   (OpenPBR Eq. 85), and the OpenPBR coat darkening (Eq. 77-84). A rough coat also roughens the base (Eq. 86). The
//   coat's own multiple scattering is left to the base through 1 - E_c, which keeps the layer energy-preserving.
// - Fuzz f_fuzz = fuzz_color E_fuzz(v) D_LTC(l) / n.l (Zeltner et al. 2022), over everything with fuzzFactor =
//   1 - fuzz_weight E_fuzz(v) (untinted, OpenPBR Eq. 92).
// - Delta events: a base with roughness <= PATH_TRACE_DELTA_ROUGHNESS (and no coat) or a coat with that roughness is a
//   mirror with weight F(n.v) times its layer factor. A coated base is never a mirror (its roughness is at least the
//   GGX minimum), so a surface has at most one delta direction.
//
// Sampling is a one-sample mixture of three strategies, with the full mixture density for MIS:
//   delta (deltaProbability), glossy (specularProbability - deltaProbability: bounded VNDF of the coat or the base,
//   sub-selected by coatSelection), broad (1 - specularProbability: cosine, or the fuzz LTC by fuzzSelection).
// ReSTIR PT's lobe-indexed BSDF uses the same partition (RESTIR_PT_LOBE_GGX = glossy, _COSINE = broad, _DELTA).
//
// M (missing single-scatter energy, 1 - E) and B (Schlick bias) come from the bundled RTXPT.1A.1 tables
// (assets/MaterialTables/GGXSingleScatterComplement.emt, see Renderer/MaterialTables/MaterialTableFile.h). 1 - E is
// never formed. The cosine axis is read at u = sqrt(mu) for the squared mapping. Anisotropic lobes read the tables at
// the equivalent isotropic roughness sqrt(sqrt(alpha_t alpha_b)).
//
// Reciprocity is broken by CorrectShadingNormalViewHorizon (n.v < 0.02), by the coat and fuzz layering, and by the fuzz
// LTC, all by design (OpenPBR's reference layering is non-reciprocal). Every evaluation keeps omega_o on the camera
// side. All directions are world space and point away from the surface.

#include "MaterialLighting.hlsli"
#include "ShadingNormal.hlsli"
#include "OpenPBRLobes.hlsli"

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

    // Builders set the smooth (un-normal-mapped) interpolated normal and the surface tangent, packed
    // (OpenPBRPackUnitVector; OPENPBR_PACKED_VECTOR_NONE when unavailable). PathTraceFinalizeSurface replaces them by
    // the resolved coat normal and the rotated anisotropy tangent, which only coated or anisotropic lobes unpack.
    uint packedSmoothNormal;
    uint packedTangent;

    bool deltaSpecular;         // The surface has a delta lobe (a mirror base, or a smooth coat).
    float3 deltaFresnel;        // The delta event's weight before division by its probability.
    float specularProbability;  // P(delta or glossy strategy).
    float deltaProbability;     // P(delta strategy).
    float coatSelection;        // Within the glossy strategy: P(coat VNDF) (0 without a rough coat).
    float fuzzSelection;        // Within the broad strategy: P(fuzz LTC) (0 without fuzz).
    // The thin film's reflectance, evaluated once per surface at n.v by PathTraceFinalizeSurface (only with a film).
    float3 thinFilmFresnel;
#if OPENPBR_LAYERED_LOBES
    // The coat and fuzz layers' view factors (PathTraceLayerViewTerms), once per surface by PathTraceFinalizeSurface,
    // so no evaluation repeats the coat's table read and darkening (PathTraceSurfaceLayers). The base tier has none.
    float layerUncoated;
    float3 layerThroughCoat;
    float layerCoatScale;
    // The fuzz lobe's view terms, packed once per surface (only with fuzz): its LTC frame's X axis
    // (OpenPBRPackUnitVector), the LTC inverse coefficients (A, B) and the tint fuzz_weight fuzz_color E_fuzz(v), as
    // halves. Packed, they hold fewer registers through the light loops than the material fields they replace.
    uint packedFuzzFrame;
    uint packedFuzzLtc;
    uint2 packedFuzzTint;
#endif

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

// -----------------------------------------------------------------------------
// Energy tables
// -----------------------------------------------------------------------------

// One row pair of a bilinear table read: the texel rows around a coordinate v and its blend between them. Every axis
// of the read is independent, so a caller reading many u at the same v (the roughness row of one surface) computes it
// once.
struct PathTraceLUT2Row
{
    int row;
    int nextRow;
    float blend;
};

PathTraceLUT2Row PathTraceLUT2RowAt(uint height, float v)
{
    float texel = saturate(v) * float(height) - 0.5f;
    texel = clamp(texel, 0.0f, float(height - 1u));

    PathTraceLUT2Row row;
    row.row = int(floor(texel));
    row.nextRow = min(row.row + 1, int(height - 1u));
    row.blend = texel - float(row.row);
    return row;
}

// The bilinear read at u on a row pair, with the same explicit clamp to texel centres along u.
float2 PathTraceLoadLUT2Row(Texture2D<float2> lut, uint width, PathTraceLUT2Row row, float u)
{
    float texel = saturate(u) * float(width) - 0.5f;
    texel = clamp(texel, 0.0f, float(width - 1u));
    int column = int(floor(texel));
    int nextColumn = min(column + 1, int(width - 1u));
    float blend = texel - float(column);

    float2 top = lerp(lut.Load(int3(column, row.row, 0)), lut.Load(int3(nextColumn, row.row, 0)), blend);
    float2 bottom = lerp(lut.Load(int3(column, row.nextRow, 0)), lut.Load(int3(nextColumn, row.nextRow, 0)), blend);
    return lerp(top, bottom, row.blend);
}

// Bilinear table read with explicit clamp to texel centres. A wrapping sampler would blend the mu = 1 and
// r = 1 edges with mu = 0 / r = 0.
float2 PathTraceLoadLUT2(Texture2D<float2> lut, float2 uv)
{
    uint width;
    uint height;
    lut.GetDimensions(width, height);
    return PathTraceLoadLUT2Row(lut, width, PathTraceLUT2RowAt(height, uv.y), uv.x);
}

// The (M, B) table's cosine coordinate (the cosine axis is read at sqrt(mu) for the squared mapping).
float PathTraceTableCosineCoordinate(PathTraceSurface surface, float cosine)
{
    return surface.materialTableCosineMapping == PATH_TRACE_TABLE_COSINE_SQUARED ? sqrt(saturate(cosine)) :
        saturate(cosine);
}

// (M, B) at a cosine for a lobe of the given (table) roughness. Never forms 1 - E.
float2 PathTraceMissingEnergyAndBiasAt(PathTraceSurface surface, float cosine, float roughness)
{
    Texture2D<float2> table = ResourceDescriptorHeap[surface.materialTableIndex];
    return max(PathTraceLoadLUT2(table, float2(PathTraceTableCosineCoordinate(surface, cosine), roughness)), 0.0f);
}

// (Mavg, Bavg): cosine-weighted averages of the same bilinear reads.
float2 PathTraceAverageMissingEnergyAndBiasAt(PathTraceSurface surface, float roughness)
{
    Texture2D<float2> table = ResourceDescriptorHeap[surface.materialAverageTableIndex];
    return max(PathTraceLoadLUT2(table, float2(roughness, 0.5f)), 0.0f);
}

// -----------------------------------------------------------------------------
// Resolved material terms (cheap, recomputed where needed instead of stored per surface)
// -----------------------------------------------------------------------------

bool PathTraceHasLobe(PathTraceSurface surface, uint lobeFlag)
{
    return (surface.material.lobeFlags & lobeFlag) != 0u;
}

float PathTraceCoatWeight(PathTraceSurface surface)
{
    return PathTraceHasLobe(surface, GPU_MATERIAL_LOBE_COAT) ? saturate(surface.material.coatWeight) : 0.0f;
}

float PathTraceFuzzWeight(PathTraceSurface surface)
{
    return PathTraceHasLobe(surface, GPU_MATERIAL_LOBE_FUZZ) ? saturate(surface.material.fuzzWeight) : 0.0f;
}

bool PathTraceBaseIsMirror(PathTraceSurface surface)
{
    // A coated base is never a mirror (OpenPBR coat roughening keeps it at least at the GGX minimum here), so the
    // surface has at most one delta direction.
    return surface.material.roughness <= PATH_TRACE_DELTA_ROUGHNESS && PathTraceCoatWeight(surface) <= 0.0f;
}

// The base lobes' roughness: the resolved specular_roughness, roughened by a rough coat in proportion to coat weight
// (OpenPBR Eq. 86: r' = lerp(r, min(1, r^4 + 2 r_c^4)^(1/4), C)). The surface keeps the unroughened value.
float PathTraceBaseRoughness(PathTraceSurface surface)
{
    float roughness = surface.material.roughness;
    float coatWeight = PathTraceCoatWeight(surface);

    if(coatWeight <= 0.0f)
    {
        return roughness;
    }

    float coatRoughness = saturate(surface.material.coatRoughness);
    float roughenedFourth = min(1.0f, roughness * roughness * roughness * roughness +
        2.0f * coatRoughness * coatRoughness * coatRoughness * coatRoughness);
    return lerp(roughness, sqrt(sqrt(roughenedFourth)), coatWeight);
}

bool PathTraceAnisotropic(PathTraceSurface surface)
{
    return PathTraceHasLobe(surface, GPU_MATERIAL_LOBE_ANISOTROPY) && surface.packedTangent != OPENPBR_PACKED_VECTOR_NONE;
}

float PathTraceGGXAlpha(float roughness)
{
    float perceptualRoughness = max(roughness, PATH_TRACE_MINIMUM_GGX_ROUGHNESS);
    return perceptualRoughness * perceptualRoughness;
}

// The base specular lobe's (alpha_t, alpha_b); both the clamped isotropic alpha without anisotropy.
float2 PathTraceBaseAlphas(PathTraceSurface surface)
{
    float anisotropy = PathTraceAnisotropic(surface) ? surface.material.anisotropy : 0.0f;
    return OpenPBRAnisotropicAlphas(PathTraceBaseRoughness(surface), anisotropy, PATH_TRACE_MINIMUM_GGX_ROUGHNESS);
}

// The roughness the energy tables are read at for the base lobe: the roughness itself, or the equivalent isotropic
// roughness of an anisotropic lobe.
float PathTraceBaseTableRoughness(PathTraceSurface surface)
{
    if(!PathTraceAnisotropic(surface))
    {
        return PathTraceBaseRoughness(surface);
    }

    float2 alphas = PathTraceBaseAlphas(surface);
    return sqrt(sqrt(alphas.x * alphas.y));
}

float2 PathTraceMissingEnergyAndBias(PathTraceSurface surface, float cosine)
{
    return PathTraceMissingEnergyAndBiasAt(surface, cosine, PathTraceBaseTableRoughness(surface));
}

float2 PathTraceAverageMissingEnergyAndBias(PathTraceSurface surface)
{
    return PathTraceAverageMissingEnergyAndBiasAt(surface, PathTraceBaseTableRoughness(surface));
}

float PathTraceMetalness(PathTraceSurface surface)
{
    return saturate(surface.material.metallic);
}

float3 PathTraceMetalF0(PathTraceSurface surface)
{
    return saturate(saturate(surface.material.baseWeight) * surface.baseColor);
}

float3 PathTraceMetalF82Coefficient(PathTraceSurface surface)
{
    return OpenPBRMetalF82Coefficient(PathTraceMetalF0(surface), surface.material.specularColor);
}

float PathTraceDielectricF0(PathTraceSurface surface)
{
    return saturate(surface.material.dielectricF0);
}

float3 PathTraceDielectricTint(PathTraceSurface surface)
{
    return saturate(surface.material.specularColor);
}

// The base's thin film over the dielectric or the metal (Belcour and Barla 2017; OpenPBR thin film), at the incident
// cosine on the base microfacet. The film sits below the coat, so its outer medium blends the ambient and the coat IOR
// by coat weight.
float3 PathTraceThinFilmFresnel(PathTraceSurface surface, float cosine, float metalness)
{
    float coatWeight = PathTraceCoatWeight(surface);
    float outerIor = lerp(1.0f, max(surface.material.coatIor, 1.0f), coatWeight);
    float filmIor = max(surface.material.thinFilmIor, outerIor);
    float ratio = outerIor / filmIor;
    float refractedSquared = 1.0f - (1.0f - saturate(cosine) * saturate(cosine)) * ratio * ratio;
    float refractedCosine = sqrt(max(refractedSquared, 0.0f));
    float3 reflectance = float3(0.0f, 0.0f, 0.0f);

    if(metalness < 1.0f)
    {
        // The dielectric below the film: its IOR relative to the film, phase PI when it is the less dense side.
        float baseIor = max(surface.material.ior, 1.0e-3f);
        float insideReflectance = OpenPBRDielectricFresnel(refractedCosine, baseIor / filmIor);
        float phase = baseIor < filmIor ? OPENPBR_PI : 0.0f;
        reflectance += (1.0f - metalness) * PathTraceDielectricTint(surface) * OpenPBRThinFilmReflectance(cosine,
            outerIor, filmIor, surface.material.thinFilmThickness, insideReflectance.xxx, phase.xxx, refractedCosine);
    }

    if(metalness > 0.0f)
    {
        // The F82-tint metal inside the film; its effective IOR from F0 exceeds any film IOR, so no phase flip.
        float3 insideReflectance = OpenPBRMetalFresnel(refractedCosine, PathTraceMetalF0(surface),
            PathTraceMetalF82Coefficient(surface), surface.material.specularWeight);
        reflectance += metalness * OpenPBRThinFilmReflectance(cosine, outerIor, filmIor,
            surface.material.thinFilmThickness, insideReflectance, float3(0.0f, 0.0f, 0.0f), refractedCosine);
    }

    return reflectance;
}

// The base's Fresnel at a cosine: the metalness mix of the tinted dielectric and the F82-tint metal, then the thin film
// blended in by its weight. The film's reflectance is the surface's, evaluated once at n.v (PathTraceFinalizeSurface), as
// the glTF iridescence reference does: exact for the delta lobe (h = n), an approximation at a rough base's other
// microfacets, and it keeps the film's interference sum out of every light evaluation.
float3 PathTraceBaseFresnel(PathTraceSurface surface, float cosine)
{
    float metalness = PathTraceMetalness(surface);
    float3 fresnel = float3(0.0f, 0.0f, 0.0f);

    if(metalness < 1.0f)
    {
        float dielectricF0 = PathTraceDielectricF0(surface);
        fresnel = (1.0f - metalness) * PathTraceDielectricTint(surface) *
            FresnelSchlick(cosine, float3(dielectricF0, dielectricF0, dielectricF0));
    }

    if(metalness > 0.0f)
    {
        fresnel += metalness * OpenPBRMetalFresnel(cosine, PathTraceMetalF0(surface),
            PathTraceMetalF82Coefficient(surface), surface.material.specularWeight);
    }

    if(PathTraceHasLobe(surface, GPU_MATERIAL_LOBE_THIN_FILM))
    {
        fresnel = lerp(fresnel, surface.thinFilmFresnel, saturate(surface.material.thinFilmWeight));
    }

    return fresnel;
}

// Fms = Favg^2 (1 - Mavg) / (1 - Favg Mavg), the legacy EvaluateGGXMultipleScatterFresnel in complement form.
float3 PathTraceMultipleScatterFresnelFromAverage(float3 averageFresnel, float averageMissing)
{
    averageFresnel = saturate(averageFresnel);
    return averageFresnel * averageFresnel * (1.0f - averageMissing) /
        max(1.0f - averageFresnel * averageMissing, 1.0e-6f);
}

// The untinted dielectric's Fms, for its specular albedo (the diffuse weighting).
float3 PathTraceDielectricMultipleScatterFresnel(PathTraceSurface surface, float averageMissing)
{
    float dielectricF0 = PathTraceDielectricF0(surface);
    return PathTraceMultipleScatterFresnelFromAverage(AverageSchlickFresnel(float3(dielectricF0, dielectricF0,
        dielectricF0)), averageMissing);
}

// The base's multiple-scatter Fresnel: the tinted dielectric's and the metal's Fms, mixed by metalness.
float3 PathTraceMultipleScatterFresnel(PathTraceSurface surface, float averageMissing)
{
    float metalness = PathTraceMetalness(surface);
    float3 multipleScatter = float3(0.0f, 0.0f, 0.0f);

    if(metalness < 1.0f)
    {
        multipleScatter = (1.0f - metalness) * PathTraceDielectricTint(surface) *
            PathTraceDielectricMultipleScatterFresnel(surface, averageMissing);
    }

    if(metalness > 0.0f)
    {
        float3 averageFresnel = OpenPBRMetalAverageFresnel(PathTraceMetalF0(surface),
            PathTraceMetalF82Coefficient(surface), surface.material.specularWeight);
        multipleScatter += metalness * PathTraceMultipleScatterFresnelFromAverage(averageFresnel, averageMissing);
    }

    return multipleScatter;
}

// The multiple-scatter lobe Fms M(l) M(v) / (PI Mavg) from its terms; none for a mirror base or where Mavg is 0.
float3 PathTraceMultipleScatterFromTerms(PathTraceSurface surface, float3 multipleScatterFresnel, float averageMissing,
    float missingLight, float missingView)
{
    if(PathTraceBaseIsMirror(surface) || averageMissing <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    return multipleScatterFresnel * missingLight * missingView / (PI * averageMissing);
}

// The untinted dielectric's total specular albedo Es(mu) = F0 S + B + Fms M, from Fms and (M, B) at mu; a delta mirror
// has Es = F(mu) exactly (Fms and (M, B) unused). This is what the diffuse layering takes away (OpenPBR's
// specular_color removes energy rather than redirecting it).
float3 PathTraceSpecularAlbedoFromTerms(PathTraceSurface surface, float dielectricF0, float3 multipleScatterFresnel,
    float cosine, float2 missingBias)
{
    float3 F0 = float3(dielectricF0, dielectricF0, dielectricF0);

    if(PathTraceBaseIsMirror(surface))
    {
        return FresnelSchlick(cosine, F0);
    }

    float scale = max(1.0f - missingBias.x - missingBias.y, 0.0f);
    return F0 * scale + missingBias.y + multipleScatterFresnel * missingBias.x;
}

float3 PathTraceSpecularAlbedo(PathTraceSurface surface, float cosine)
{
    float dielectricF0 = PathTraceDielectricF0(surface);

    if(PathTraceBaseIsMirror(surface))
    {
        return PathTraceSpecularAlbedoFromTerms(surface, dielectricF0, float3(0.0f, 0.0f, 0.0f), cosine,
            float2(0.0f, 0.0f));
    }

    float2 missingBias = PathTraceMissingEnergyAndBias(surface, cosine);
    float averageMissing = PathTraceAverageMissingEnergyAndBias(surface).x;
    return PathTraceSpecularAlbedoFromTerms(surface, dielectricF0,
        PathTraceDielectricMultipleScatterFresnel(surface, averageMissing), cosine, missingBias);
}

float3 PathTraceAverageSpecularAlbedo(PathTraceSurface surface)
{
    float dielectricF0 = PathTraceDielectricF0(surface);
    float3 F0 = float3(dielectricF0, dielectricF0, dielectricF0);

    if(PathTraceBaseIsMirror(surface))
    {
        return AverageSchlickFresnel(F0);
    }

    float2 average = PathTraceAverageMissingEnergyAndBias(surface);
    float scale = max(1.0f - average.x - average.y, 0.0f);
    return F0 * scale + average.y + PathTraceDielectricMultipleScatterFresnel(surface, average.x) * average.x;
}

// The diffuse weight's view factor 1 - Es(v) and its average factor max(1 - Es_avg, 1e-6), saturated.
float3 PathTraceTransmittedView(PathTraceSurface surface)
{
    return saturate(1.0f - PathTraceSpecularAlbedo(surface, surface.normalDotView));
}

float3 PathTraceTransmittedAverage(PathTraceSurface surface)
{
    return max(saturate(1.0f - PathTraceAverageSpecularAlbedo(surface)), 1.0e-6f);
}

// Reciprocal diffuse weight (1 - Es(l)) (1 - Es(v)) / (1 - Es_avg) from Es(l) and the two factors above.
float3 PathTraceDiffuseWeightFromTerms(float3 specularAlbedoLight, float3 transmittedView, float3 transmittedAverage)
{
    float3 transmittedLight = saturate(1.0f - specularAlbedoLight);
    return transmittedLight * transmittedView / transmittedAverage;
}

float3 PathTraceDiffuseWeight(PathTraceSurface surface, float cosineLight)
{
    return PathTraceDiffuseWeightFromTerms(PathTraceSpecularAlbedo(surface, cosineLight),
        PathTraceTransmittedView(surface), PathTraceTransmittedAverage(surface));
}

// The diffuse lobe's albedo, (1 - metalness) base_weight base_color (OpenPBR glossy-diffuse: C = base_weight
// base_color, mixed with the metal by metalness), times the opt-in AO-to-diffuse factor (OpenPBRApplyOcclusion).
float3 PathTraceDiffuseAlbedo(PathTraceSurface surface)
{
    float3 albedo = (1.0f - PathTraceMetalness(surface)) * saturate(surface.material.baseWeight) * surface.baseColor;

    if(PathTraceHasLobe(surface, GPU_MATERIAL_PATH_TRACED_AO))
    {
        albedo *= saturate(surface.material.diffuseOcclusion);
    }

    return albedo;
}

// A single F0 summarizing the base's reflectivity for the denoiser guides (NRD and DLSS-RR demodulation): the metalness
// mix of the tinted dielectric F0 and the metal F0.
float3 PathTraceSchlickF0(PathTraceSurface surface)
{
    float dielectricF0 = PathTraceDielectricF0(surface);
    float3 metalF0 = saturate(surface.material.specularWeight) * PathTraceMetalF0(surface);
    return lerp(dielectricF0 * PathTraceDielectricTint(surface), metalF0, PathTraceMetalness(surface));
}

// -----------------------------------------------------------------------------
// Coat and fuzz layer terms
// -----------------------------------------------------------------------------

// The coat's normal: the resolved coat normal PathTraceFinalizeSurface packs (only meaningful with a coat).
float3 PathTraceCoatNormal(PathTraceSurface surface)
{
    return OpenPBRUnpackUnitVector(surface.packedSmoothNormal);
}

bool PathTraceCoatIsMirror(PathTraceSurface surface)
{
    return saturate(surface.material.coatRoughness) <= PATH_TRACE_DELTA_ROUGHNESS;
}

// The coat's single-scatter albedo E_c(mu) = coatF0 S + B (a mirror coat: Schlick(coatF0, mu) exactly).
float PathTraceCoatAlbedo(PathTraceSurface surface, float cosine)
{
    float coatF0 = saturate(surface.material.coatF0);

    if(PathTraceCoatIsMirror(surface))
    {
        return FresnelSchlick(cosine, float3(coatF0, coatF0, coatF0)).x;
    }

    float2 missingBias = PathTraceMissingEnergyAndBiasAt(surface, cosine, saturate(surface.material.coatRoughness));
    float scale = max(1.0f - missingBias.x - missingBias.y, 0.0f);
    return coatF0 * scale + missingBias.y;
}

// The fuzz layer's albedo E_fuzz(v) (untinted) at the base normal.
float PathTraceFuzzAlbedo(PathTraceSurface surface)
{
    return OpenPBRFuzzAlbedo(surface.normalDotView, surface.material.fuzzRoughness);
}

// The view-dependent factors of the layers above the base: the base sees uncoated + throughCoat T(l, v) (T the coat's
// absorption, 1 without a coloured coat), the coat lobe coatScale. Identity (1, 0, 1) without coat and fuzz.
struct PathTraceLayerTerms
{
    float uncoated;
    float3 throughCoat;
    float coatScale;
    // The coat's normal and n_c . v (only with a coat).
    float3 coatNormal;
    float coatNormalDotView;
};

PathTraceLayerTerms PathTraceLayerViewTerms(PathTraceSurface surface)
{
    PathTraceLayerTerms terms;
    terms.uncoated = 1.0f;
    terms.throughCoat = float3(0.0f, 0.0f, 0.0f);
    terms.coatScale = 0.0f;
    terms.coatNormal = surface.normal;
    terms.coatNormalDotView = surface.normalDotView;

    float fuzzFactor = 1.0f - PathTraceFuzzWeight(surface) * (PathTraceFuzzWeight(surface) > 0.0f ?
        PathTraceFuzzAlbedo(surface) : 0.0f);
    float coatWeight = PathTraceCoatWeight(surface);

    if(coatWeight > 0.0f)
    {
        terms.coatNormal = PathTraceCoatNormal(surface);
        terms.coatNormalDotView = saturate(dot(terms.coatNormal, surface.view));
        float coatAlbedo = PathTraceCoatAlbedo(surface, terms.coatNormalDotView);

        // Coat darkening (OpenPBR Eq. 77-84): Delta = (1 - K) / (1 - E_b K) with K blended between the smooth-base
        // value K_s = F_c(n.v) and the rough-base value K_r (CPU-derived) by the estimated base roughness, and E_b the
        // base's normal-incidence albedo estimate (MaterialX's estimate: the base colour, the metal with its weight).
        float metalness = PathTraceMetalness(surface);
        float coatF0 = saturate(surface.material.coatF0);
        float smoothReflection = FresnelSchlick(terms.coatNormalDotView, float3(coatF0, coatF0, coatF0)).x;
        float specularRoughness = PathTraceBaseRoughness(surface);
        float dielectricRoughness = lerp(1.0f, specularRoughness, saturate(PathTraceDielectricF0(surface)));
        float baseRoughness = lerp(dielectricRoughness, specularRoughness, metalness);
        float internalReflection = lerp(smoothReflection, saturate(surface.material.coatInternalReflection),
            saturate(baseRoughness));
        float3 baseAlbedo = lerp(saturate(surface.material.baseWeight) * surface.baseColor,
            PathTraceMetalF0(surface) * saturate(surface.material.specularWeight), metalness);
        float3 darkening = (1.0f - internalReflection) / max(1.0f - baseAlbedo * internalReflection, 1.0e-6f);
        float3 modulatedDarkening = lerp(float3(1.0f, 1.0f, 1.0f), darkening, saturate(surface.material.coatDarkening));

        terms.uncoated = (1.0f - coatWeight) * fuzzFactor;
        terms.throughCoat = coatWeight * (1.0f - coatAlbedo) * modulatedDarkening * fuzzFactor;
        terms.coatScale = coatWeight * fuzzFactor;
        return terms;
    }

    terms.uncoated = fuzzFactor;
    return terms;
}

// The layer factors PathTraceFinalizeSurface stored (the coat normal and n_c . v recomputed, which reads no table).
PathTraceLayerTerms PathTraceSurfaceLayers(PathTraceSurface surface)
{
#if OPENPBR_LAYERED_LOBES
    PathTraceLayerTerms terms;
    terms.uncoated = surface.layerUncoated;
    terms.throughCoat = surface.layerThroughCoat;
    terms.coatScale = surface.layerCoatScale;
    terms.coatNormal = surface.normal;
    terms.coatNormalDotView = surface.normalDotView;

    if(terms.coatScale > 0.0f)
    {
        terms.coatNormal = PathTraceCoatNormal(surface);
        terms.coatNormalDotView = saturate(dot(terms.coatNormal, surface.view));
    }

    return terms;
#else
    return PathTraceLayerViewTerms(surface);
#endif
}

// The coat's absorption T(l, v) along both rays (1 for a white coat colour).
float3 PathTraceCoatTransmittance(PathTraceSurface surface, PathTraceLayerTerms layers, float3 direction)
{
    float3 coatColor = saturate(surface.material.coatColor);

    if(all(coatColor >= 1.0f))
    {
        return float3(1.0f, 1.0f, 1.0f);
    }

    float coatIor = max(surface.material.coatIor, 1.0f);
    float refractedLight = OpenPBRRefractedCosine(dot(layers.coatNormal, direction), coatIor);
    float refractedView = OpenPBRRefractedCosine(layers.coatNormalDotView, coatIor);
    return OpenPBRCoatTransmittance(0.5f * log(max(coatColor, 1.0e-6f)), refractedLight, refractedView);
}

// The base's layer factor A(l, v) for a light direction.
float3 PathTraceBaseLayerFactor(PathTraceSurface surface, PathTraceLayerTerms layers, float3 direction)
{
    if(layers.coatScale <= 0.0f)
    {
        return layers.uncoated.xxx;
    }

    return layers.uncoated + layers.throughCoat * PathTraceCoatTransmittance(surface, layers, direction);
}

// -----------------------------------------------------------------------------
// GGX lobes
// -----------------------------------------------------------------------------

void PathTraceBuildFrame(float3 normal, out float3 tangent, out float3 bitangent)
{
    // Duff et al. 2017, branchless orthonormal basis.
    float signZ = normal.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (signZ + normal.z);
    float b = normal.x * normal.y * a;
    tangent = float3(1.0f + signZ * normal.x * normal.x * a, signZ * b, -signZ * normal.x);
    bitangent = float3(b, signZ + normal.y * normal.y * a, -normal.y);
}

// The base lobe's local frame: the anisotropy tangent (packed by PathTraceFinalizeSurface) or Duff's frame.
void PathTraceBaseFrame(PathTraceSurface surface, out float3 tangent, out float3 bitangent)
{
    if(PathTraceAnisotropic(surface))
    {
        tangent = OpenPBRUnpackUnitVector(surface.packedTangent);
        bitangent = cross(surface.normal, tangent);
        return;
    }

    PathTraceBuildFrame(surface.normal, tangent, bitangent);
}

// GGX normal distribution, algebraically identical to DistributionGGX:
//   alpha^2 / (PI ((n.h)^2 (alpha^2 - 1) + 1)^2) = alpha^2 / (PI ((n.h)^2 alpha^2 + |n x h|^2)^2).
// The legacy form cancels catastrophically in float32 when alpha is near the 0.045^2 floor and n.h is
// within a few ulps of 1; the cross-product form keeps full precision near the peak.
float PathTraceDistributionGGXFromAlpha(float3 normal, float3 halfVector, float alphaSquared)
{
    float cosine = saturate(dot(normal, halfVector));
    float3 sine = cross(normal, halfVector);
    float denominator = cosine * cosine * alphaSquared + dot(sine, sine);
    return alphaSquared / max(PI * denominator * denominator, 1.0e-30f);
}

float PathTraceDistributionGGX(float3 normal, float3 halfVector, float roughness)
{
    float alpha = PathTraceGGXAlpha(roughness);
    return PathTraceDistributionGGXFromAlpha(normal, halfVector, alpha * alpha);
}

// An isotropic GGX reflection lobe D G2 F / (4 n.l n.v) about a normal, with the legacy G and denominator clamp and the
// stable D, from the view terms n.v, alpha^2 and Smith G's view root.
float PathTraceGGXIsotropicValue(float3 normal, float3 view, float normalDotView, float alphaSquared,
    float smithViewRoot, float3 direction, out float viewDotHalf)
{
    viewDotHalf = 0.0f;
    float normalDotLight = saturate(dot(normal, direction));

    if(normalDotLight <= 0.0f || normalDotView <= 0.0f)
    {
        return 0.0f;
    }

    float3 halfVector = normalize(direction + view);
    viewDotHalf = saturate(dot(view, halfVector));
    float distribution = PathTraceDistributionGGXFromAlpha(normal, halfVector, alphaSquared);
    float geometry = GeometrySmithGGXPrepared(normalDotLight, normalDotView, alphaSquared, smithViewRoot);
    return distribution * geometry / max(4.0f * normalDotLight * normalDotView, 1.0e-6f);
}

// The anisotropic base lobe's D G2 / (4 n.l n.v) in the anisotropy frame.
float PathTraceGGXAnisotropicValue(PathTraceSurface surface, float2 alphas, float3 direction, out float viewDotHalf)
{
    viewDotHalf = 0.0f;
    float normalDotLight = saturate(dot(surface.normal, direction));
    float normalDotView = saturate(surface.normalDotView);

    if(normalDotLight <= 0.0f || normalDotView <= 0.0f)
    {
        return 0.0f;
    }

    float3 tangent;
    float3 bitangent;
    PathTraceBaseFrame(surface, tangent, bitangent);
    float3 halfVector = normalize(direction + surface.view);
    viewDotHalf = saturate(dot(surface.view, halfVector));

    float distribution = OpenPBRDistributionAnisotropic(OpenPBRToLocal(halfVector, tangent, bitangent, surface.normal),
        alphas);
    float lightRoot = OpenPBRSmithRootAnisotropic(OpenPBRToLocal(direction, tangent, bitangent, surface.normal), alphas);
    float viewRoot = OpenPBRSmithRootAnisotropic(OpenPBRToLocal(surface.view, tangent, bitangent, surface.normal),
        alphas);
    float geometry = OpenPBRSmithG2FromRoots(normalDotLight, normalDotView, lightRoot, viewRoot);
    return distribution * geometry / max(4.0f * normalDotLight * normalDotView, 1.0e-6f);
}

// The base specular single scatter D G2 F_base / (4 n.l n.v), without its layer factor, from the view terms
// (isotropic: n.v, alpha^2, Smith G's view root; anisotropic surfaces recompute their own).
float3 PathTraceEvaluateBaseSpecularFromTerms(PathTraceSurface surface, float normalDotView, float alphaSquared,
    float smithViewRoot, float3 direction)
{
    float viewDotHalf;
    float value;

    if(PathTraceAnisotropic(surface))
    {
        value = PathTraceGGXAnisotropicValue(surface, PathTraceBaseAlphas(surface), direction, viewDotHalf);
    }
    else
    {
        value = PathTraceGGXIsotropicValue(surface.normal, surface.view, normalDotView, alphaSquared, smithViewRoot,
            direction, viewDotHalf);
    }

    if(value <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    return value * PathTraceBaseFresnel(surface, viewDotHalf);
}

float3 PathTraceEvaluateBaseSpecular(PathTraceSurface surface, float3 direction)
{
    float normalDotView = saturate(dot(surface.normal, surface.view));

    // The early exit before the view terms, as before the split.
    if(saturate(dot(surface.normal, direction)) <= 0.0f || normalDotView <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float alpha = PathTraceGGXAlpha(PathTraceBaseRoughness(surface));
    float alphaSquared = alpha * alpha;
    return PathTraceEvaluateBaseSpecularFromTerms(surface, normalDotView, alphaSquared,
        GeometrySmithGGXRoot(normalDotView, alphaSquared), direction);
}

// The coat's single scatter D G2 Schlick(coatF0) / (4 n_c.l n_c.v), without its layer scale.
float3 PathTraceEvaluateCoatSpecular(PathTraceSurface surface, PathTraceLayerTerms layers, float3 direction)
{
    if(PathTraceCoatIsMirror(surface))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float alpha = PathTraceGGXAlpha(saturate(surface.material.coatRoughness));
    float alphaSquared = alpha * alpha;
    float viewDotHalf;
    float value = PathTraceGGXIsotropicValue(layers.coatNormal, surface.view, layers.coatNormalDotView, alphaSquared,
        GeometrySmithGGXRoot(layers.coatNormalDotView, alphaSquared), direction, viewDotHalf);

    if(value <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float coatF0 = saturate(surface.material.coatF0);
    return value * FresnelSchlick(viewDotHalf, float3(coatF0, coatF0, coatF0));
}

// The fuzz lobe's view terms: its LTC frame's X axis and inverse coefficients (unpacked in the layered tier).
void PathTraceFuzzTerms(PathTraceSurface surface, out float3 axisX, out float2 inverseLtc)
{
#if OPENPBR_LAYERED_LOBES
    axisX = OpenPBRUnpackUnitVector(surface.packedFuzzFrame);
    inverseLtc = f16tof32(uint2(surface.packedFuzzLtc, surface.packedFuzzLtc >> 16));
#else
    float3 axisY;
    OpenPBRFuzzFrame(surface.normal, surface.view, surface.normalDotView, axisX, axisY);
    inverseLtc = OpenPBRFuzzInverseLtc(surface.normalDotView, surface.material.fuzzRoughness);
#endif
}

// fuzz_weight fuzz_color E_fuzz(v).
float3 PathTraceFuzzTint(PathTraceSurface surface)
{
#if OPENPBR_LAYERED_LOBES
    return f16tof32(uint3(surface.packedFuzzTint.x, surface.packedFuzzTint.x >> 16, surface.packedFuzzTint.y));
#else
    return PathTraceFuzzWeight(surface) * saturate(surface.material.fuzzColor) * PathTraceFuzzAlbedo(surface);
#endif
}

// The fuzz lobe fuzz_color E_fuzz(v) D_LTC(l) / n.l, scaled by fuzz_weight.
float3 PathTraceEvaluateFuzz(PathTraceSurface surface, float3 direction)
{
    float normalDotLight = dot(surface.normal, direction);

    if(normalDotLight <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 axisX;
    float2 inverseLtc;
    PathTraceFuzzTerms(surface, axisX, inverseLtc);
    float distribution = OpenPBRFuzzDistributionFromTerms(surface.normal, axisX, inverseLtc, direction);
    return PathTraceFuzzTint(surface) * distribution / max(normalDotLight, 1.0e-6f);
}

// -----------------------------------------------------------------------------
// Surface completion: resolved layer normals and tangent, delta events, strategy selection
// -----------------------------------------------------------------------------

// Completes a surface whose material, baseColor, normals, view and packed smooth normal and tangent are filled.
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
    float coatWeight = PathTraceCoatWeight(surface);

    // The coat's normal: the base's, or the smooth interpolated normal, both horizon-corrected.
    if(coatWeight > 0.0f)
    {
        float3 coatNormal = surface.normal;

        // Builders without a smooth normal (PSR's stored hits) leave OPENPBR_PACKED_VECTOR_NONE: the coat then follows
        // the base.
        if(!PathTraceHasLobe(surface, GPU_MATERIAL_COAT_FOLLOWS_BASE_NORMAL) &&
           surface.packedSmoothNormal != OPENPBR_PACKED_VECTOR_NONE)
        {
            float3 smoothNormal = OpenPBRUnpackUnitVector(surface.packedSmoothNormal);

            if(dot(smoothNormal, surface.geometricNormal) < 0.0f)
            {
                smoothNormal = -smoothNormal;
            }

            coatNormal = CorrectShadingNormalViewHorizon(smoothNormal, surface.geometricNormal, surface.view);

            if(!(dot(coatNormal, surface.view) > 0.0f) || !all(isfinite(coatNormal)))
            {
                coatNormal = surface.normal;
            }
        }

        surface.packedSmoothNormal = OpenPBRPackUnitVector(coatNormal);
    }

    // The anisotropy tangent: the surface tangent made orthogonal to the shading normal and rotated about it.
    if(PathTraceHasLobe(surface, GPU_MATERIAL_LOBE_ANISOTROPY) && surface.packedTangent != OPENPBR_PACKED_VECTOR_NONE)
    {
        float3 tangent = OpenPBRUnpackUnitVector(surface.packedTangent);
        tangent -= surface.normal * dot(surface.normal, tangent);
        float lengthSquared = dot(tangent, tangent);

        if(lengthSquared > 1.0e-8f && isfinite(lengthSquared))
        {
            tangent *= rsqrt(lengthSquared);
            float3 bitangent = cross(surface.normal, tangent);
            float sine;
            float cosine;
            sincos(surface.material.anisotropyRotation, sine, cosine);
            surface.packedTangent = OpenPBRPackUnitVector(cosine * tangent + sine * bitangent);
        }
        else
        {
            surface.packedTangent = OPENPBR_PACKED_VECTOR_NONE;
        }
    }
    else
    {
        surface.packedTangent = OPENPBR_PACKED_VECTOR_NONE;
    }

    // The thin film's reflectance at n.v, once per surface (PathTraceBaseFresnel).
    surface.thinFilmFresnel = float3(0.0f, 0.0f, 0.0f);

    if(PathTraceHasLobe(surface, GPU_MATERIAL_LOBE_THIN_FILM))
    {
        surface.thinFilmFresnel = PathTraceThinFilmFresnel(surface, surface.normalDotView,
            PathTraceMetalness(surface));
    }

    bool baseMirror = PathTraceBaseIsMirror(surface);
    bool coatMirror = coatWeight > 0.0f && PathTraceCoatIsMirror(surface);
    surface.deltaSpecular = baseMirror || coatMirror;

    PathTraceLayerTerms layers = PathTraceLayerViewTerms(surface);
    float3 baseLayer = layers.uncoated + layers.throughCoat;
#if OPENPBR_LAYERED_LOBES
    surface.layerUncoated = layers.uncoated;
    surface.layerThroughCoat = layers.throughCoat;
    surface.layerCoatScale = layers.coatScale;
    surface.packedFuzzFrame = OPENPBR_PACKED_VECTOR_NONE;
    surface.packedFuzzLtc = 0u;
    surface.packedFuzzTint = uint2(0u, 0u);

    if(PathTraceFuzzWeight(surface) > 0.0f)
    {
        float3 fuzzAxisX;
        float3 fuzzAxisY;
        OpenPBRFuzzFrame(surface.normal, surface.view, surface.normalDotView, fuzzAxisX, fuzzAxisY);
        uint2 fuzzLtc = f32tof16(OpenPBRFuzzInverseLtc(surface.normalDotView, surface.material.fuzzRoughness));
        uint3 fuzzTint = f32tof16(PathTraceFuzzWeight(surface) * saturate(surface.material.fuzzColor) *
            PathTraceFuzzAlbedo(surface));
        surface.packedFuzzFrame = OpenPBRPackUnitVector(fuzzAxisX);
        surface.packedFuzzLtc = fuzzLtc.x | (fuzzLtc.y << 16);
        surface.packedFuzzTint = uint2(fuzzTint.x | (fuzzTint.y << 16), fuzzTint.z);
    }
#endif

    // Selection estimates (the strategies' captured albedos; only variance depends on them, every strategy covers the
    // hemisphere).
    float2 missingBias = baseMirror ? float2(0.0f, 0.0f) :
        PathTraceMissingEnergyAndBias(surface, surface.normalDotView);
    float averageMissing = baseMirror ? 0.0f : PathTraceAverageMissingEnergyAndBias(surface).x;
    float3 baseEstimate;

    if(baseMirror)
    {
        baseEstimate = PathTraceBaseFresnel(surface, surface.normalDotView);
    }
    else
    {
        // The base's single-scatter albedo, F0 S + B per mixed part (the F82 correction is negligible here).
        float singleScatterScale = max(1.0f - missingBias.x - missingBias.y, 0.0f);
        float metalness = PathTraceMetalness(surface);
        float dielectricF0 = PathTraceDielectricF0(surface);
        float3 dielectricAlbedo = PathTraceDielectricTint(surface) * (dielectricF0 * singleScatterScale + missingBias.y);
        float3 metalAlbedo = saturate(surface.material.specularWeight) *
            (PathTraceMetalF0(surface) * singleScatterScale + missingBias.y);
        baseEstimate = lerp(dielectricAlbedo, metalAlbedo, metalness);
    }

    float baseSpecularEstimate = max(PathTraceLuminance(baseLayer * baseEstimate), 0.0f);
    float coatEstimate = coatWeight > 0.0f ?
        max(layers.coatScale * PathTraceCoatAlbedo(surface, layers.coatNormalDotView), 0.0f) : 0.0f;

    float3 multipleScatterAlbedo = baseMirror ? float3(0.0f, 0.0f, 0.0f) :
        PathTraceMultipleScatterFresnel(surface, averageMissing) * missingBias.x;
    float3 diffuseAlbedo = PathTraceDiffuseAlbedo(surface) * saturate(1.0f - PathTraceSpecularAlbedo(surface,
        surface.normalDotView));
    float cosineEstimate = max(PathTraceLuminance(baseLayer * (diffuseAlbedo + multipleScatterAlbedo)), 0.0f);
    float fuzzEstimate = PathTraceFuzzWeight(surface) > 0.0f ?
        max(PathTraceFuzzWeight(surface) * PathTraceLuminance(saturate(surface.material.fuzzColor)) *
        PathTraceFuzzAlbedo(surface), 0.0f) : 0.0f;
    float broadEstimate = cosineEstimate + fuzzEstimate;

    float deltaEstimate = (baseMirror ? baseSpecularEstimate : 0.0f) + (coatMirror ? coatEstimate : 0.0f);
    float glossyBaseEstimate = baseMirror ? 0.0f : baseSpecularEstimate;
    float glossyCoatEstimate = coatWeight > 0.0f && !coatMirror ? coatEstimate : 0.0f;
    float glossyEstimate = glossyBaseEstimate + glossyCoatEstimate;
    float specularEstimate = deltaEstimate + glossyEstimate;

    if(broadEstimate <= 0.0f)
    {
        surface.specularProbability = 1.0f;
    }
    else if(specularEstimate <= 0.0f)
    {
        surface.specularProbability = 0.0f;
    }
    else
    {
        surface.specularProbability = clamp(specularEstimate / (specularEstimate + broadEstimate),
            PATH_TRACE_MINIMUM_SELECTION, 1.0f - PATH_TRACE_MINIMUM_SELECTION);
    }

    // The delta event's share of the specular strategies (all of it without a glossy lobe).
    float deltaShare = 0.0f;

    if(deltaEstimate > 0.0f || (surface.deltaSpecular && glossyEstimate <= 0.0f))
    {
        deltaShare = glossyEstimate <= 0.0f ? 1.0f : clamp(deltaEstimate / specularEstimate,
            PATH_TRACE_MINIMUM_SELECTION, 1.0f - PATH_TRACE_MINIMUM_SELECTION);
    }

    surface.deltaProbability = surface.deltaSpecular ? surface.specularProbability * deltaShare : 0.0f;

    // The coat's share of the glossy strategy, and the fuzz LTC's share of the broad strategy.
    surface.coatSelection = 0.0f;

    if(glossyCoatEstimate > 0.0f)
    {
        surface.coatSelection = glossyBaseEstimate <= 0.0f ? 1.0f : clamp(glossyCoatEstimate / glossyEstimate,
            PATH_TRACE_MINIMUM_SELECTION, 1.0f - PATH_TRACE_MINIMUM_SELECTION);
    }

    surface.fuzzSelection = 0.0f;

    if(fuzzEstimate > 0.0f)
    {
        surface.fuzzSelection = cosineEstimate <= 0.0f ? 1.0f : clamp(fuzzEstimate / broadEstimate,
            PATH_TRACE_MINIMUM_SELECTION, 1.0f - PATH_TRACE_MINIMUM_SELECTION);
    }

    // The delta event's weight: the base mirror's F(n.v) through its layers, or the smooth coat's Fresnel.
    surface.deltaFresnel = float3(0.0f, 0.0f, 0.0f);

    if(baseMirror)
    {
        surface.deltaFresnel = baseLayer * PathTraceBaseFresnel(surface, surface.normalDotView);
    }
    else if(coatMirror)
    {
        float coatF0 = saturate(surface.material.coatF0);
        surface.deltaFresnel = layers.coatScale * FresnelSchlick(layers.coatNormalDotView,
            float3(coatF0, coatF0, coatF0));
    }

    return all(isfinite(surface.deltaFresnel)) && isfinite(surface.specularProbability) &&
        isfinite(surface.deltaProbability);
}

bool PathTraceDirectionAboveSurface(PathTraceSurface surface, float3 direction)
{
    return dot(surface.normal, direction) > 0.0f && dot(surface.geometricNormal, direction) > 0.0f;
}

// The delta event's direction: the mirror reflection about the base normal, or about the coat normal.
float3 PathTraceDeltaDirection(PathTraceSurface surface)
{
    if(PathTraceBaseIsMirror(surface))
    {
        return reflect(-surface.view, surface.normal);
    }

    return reflect(-surface.view, PathTraceCoatNormal(surface));
}

// The probability of the glossy (non-delta specular) strategy.
float PathTraceGlossyProbability(PathTraceSurface surface)
{
    return max(surface.specularProbability - surface.deltaProbability, 0.0f);
}

// -----------------------------------------------------------------------------
// Prepare / evaluate split (2026-11-24; Documentation/Rendering/ReSTIR_PT_Initial_Sampling_Optimization.md)
// -----------------------------------------------------------------------------
// Everything the BSDF value needs that does not depend on the light direction, computed once per shading point. A
// caller that evaluates many directions at one point (ReSTIR PT's light candidate loops evaluate dozens) prepares once.
// The prepared evaluation and PathTraceEvaluateBSDFLobes share every lobe term (the *FromTerms helpers) and sum them in
// the same order, so the two agree bit for bit under strict IEEE. The struct stays live through those loops, so it
// holds only the view, table and layer terms, not a copy of the material: a lobe's raw parameters (the metal colours,
// the coat and fuzz parameters) are read from the surface inside that lobe's branch. The strategy probabilities are not
// repeated here: PathTraceFinalizeSurface already stores them once per surface, and the value never reads them.
struct PathTraceBSDFPrepared
{
    // saturate(n . v), the base GGX alpha^2 (D and Smith G clamp roughness alike) and Smith G's view root.
    float normalDotView;
    float alphaSquared;
    float smithViewRoot;
    float dielectricF0;

    // Multiple scatter, non-delta surfaces only (0 otherwise): Mavg, M(n.v), the base's mixed Fms and the untinted
    // dielectric Fms of the diffuse weighting.
    float averageMissing;
    float missingView;
    float3 multipleScatterFresnel;
    float3 dielectricMultipleScatterFresnel;

    // The diffuse weight's view and average factors, and EON's light-independent terms (0 for a pure metal).
    float3 transmittedView;
    float3 transmittedAverage;
    EONDiffusePrepared diffuse;

    // The layers above the base (identity without coat and fuzz).
    float layerUncoated;
    float3 layerThroughCoat;
    float coatScale;

    // The (M, B) table's width and the surface's roughness row, for the per-light read at n.l (non-delta surfaces).
    uint tableWidth;
    PathTraceLUT2Row tableRow;
};

PathTraceBSDFPrepared PathTracePrepareBSDF(PathTraceSurface surface)
{
    PathTraceBSDFPrepared prepared = (PathTraceBSDFPrepared)0;
    prepared.normalDotView = saturate(dot(surface.normal, surface.view));
    float alpha = PathTraceGGXAlpha(PathTraceBaseRoughness(surface));
    prepared.alphaSquared = alpha * alpha;
    prepared.smithViewRoot = GeometrySmithGGXRoot(prepared.normalDotView, prepared.alphaSquared);
    prepared.dielectricF0 = PathTraceDielectricF0(surface);
    bool baseMirror = PathTraceBaseIsMirror(surface);

    if(!baseMirror)
    {
        prepared.averageMissing = PathTraceAverageMissingEnergyAndBias(surface).x;
        prepared.missingView = PathTraceMissingEnergyAndBias(surface, surface.normalDotView).x;
        prepared.multipleScatterFresnel = PathTraceMultipleScatterFresnel(surface, prepared.averageMissing);
        prepared.dielectricMultipleScatterFresnel = PathTraceDielectricMultipleScatterFresnel(surface,
            prepared.averageMissing);

        Texture2D<float2> table = ResourceDescriptorHeap[surface.materialTableIndex];
        uint width;
        uint height;
        table.GetDimensions(width, height);
        prepared.tableWidth = width;
        prepared.tableRow = PathTraceLUT2RowAt(height, PathTraceBaseTableRoughness(surface));
    }

    // The diffuse lobe's terms: a pure metal has none.
    if(PathTraceMetalness(surface) < 1.0f)
    {
        prepared.transmittedView = PathTraceTransmittedView(surface);
        prepared.transmittedAverage = PathTraceTransmittedAverage(surface);
        prepared.diffuse = PrepareEONDiffuse(PathTraceDiffuseAlbedo(surface), surface.material.diffuseRoughness,
            surface.normal, surface.view);
    }

    PathTraceLayerTerms layers = PathTraceSurfaceLayers(surface);
    prepared.layerUncoated = layers.uncoated;
    prepared.layerThroughCoat = layers.throughCoat;
    prepared.coatScale = layers.coatScale;
    return prepared;
}

// (M, B) at the light's cosine, on the prepared roughness row (PathTraceMissingEnergyAndBias's read). Non-delta only.
float2 PathTracePreparedMissingEnergyAndBias(PathTraceSurface surface, PathTraceBSDFPrepared prepared, float cosine)
{
    Texture2D<float2> table = ResourceDescriptorHeap[surface.materialTableIndex];
    return max(PathTraceLoadLUT2Row(table, prepared.tableWidth, prepared.tableRow,
        PathTraceTableCosineCoordinate(surface, cosine)), 0.0f);
}

// The layer terms a prepared evaluation reuses (the coat normal is unpacked again, which costs no table read).
PathTraceLayerTerms PathTracePreparedLayers(PathTraceSurface surface, PathTraceBSDFPrepared prepared)
{
    PathTraceLayerTerms layers;
    layers.uncoated = prepared.layerUncoated;
    layers.throughCoat = prepared.layerThroughCoat;
    layers.coatScale = prepared.coatScale;
    layers.coatNormal = surface.normal;
    layers.coatNormalDotView = surface.normalDotView;

    if(prepared.coatScale > 0.0f)
    {
        layers.coatNormal = PathTraceCoatNormal(surface);
        layers.coatNormalDotView = saturate(dot(layers.coatNormal, surface.view));
    }

    return layers;
}

// The optional upper layers' own lobes (coat and fuzz) for a direction; 0 without them.
float3 PathTraceEvaluateUpperLobes(PathTraceSurface surface, PathTraceLayerTerms layers, float3 direction)
{
    float3 value = float3(0.0f, 0.0f, 0.0f);

    if(layers.coatScale > 0.0f)
    {
        value = layers.coatScale * PathTraceEvaluateCoatSpecular(surface, layers, direction);
    }

    if(PathTraceFuzzWeight(surface) > 0.0f)
    {
        value += PathTraceEvaluateFuzz(surface, direction);
    }

    return value;
}

// Non-delta BSDF value f(l, v), without the cosine, and its diffuse lobe (the energy-compensated EON term through its
// layers), from the prepared terms. The rest (base single and multiple scatter, coat, fuzz) is specular-like. Each lobe
// is its own step of a running sum, so its temporaries end before the next lobe starts; the sum keeps the established
// association ((diffuse + single scatter) + multiple scatter), then the layer factor, then the upper lobes. Zero outside
// both hemispheres.
float3 PathTraceEvaluateBSDFLobesPrepared(PathTraceSurface surface, PathTraceBSDFPrepared prepared, float3 direction,
    out float3 diffuseValue)
{
    diffuseValue = float3(0.0f, 0.0f, 0.0f);

    if(!PathTraceDirectionAboveSurface(surface, direction))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float normalDotLight = dot(surface.normal, direction);
    bool baseMirror = PathTraceBaseIsMirror(surface);
    // The light's (M, B) read, shared by the multiple-scatter lobe and the diffuse weight's Es(l).
    float2 missingBiasLight = baseMirror ? float2(0.0f, 0.0f) :
        PathTracePreparedMissingEnergyAndBias(surface, prepared, normalDotLight);

    // A delta mirror removes the single-scatter density; its energy is the delta event in PathTraceSampleBSDF.
    float3 value = baseMirror ? float3(0.0f, 0.0f, 0.0f) :
        PathTraceEvaluateBaseSpecularFromTerms(surface, prepared.normalDotView, prepared.alphaSquared,
        prepared.smithViewRoot, direction);

    if(PathTraceMetalness(surface) < 1.0f)
    {
        float3 diffuse = EvaluateEONDiffusePrepared(prepared.diffuse, surface.normal, direction, surface.view);
        float3 specularAlbedoLight = PathTraceSpecularAlbedoFromTerms(surface, prepared.dielectricF0,
            prepared.dielectricMultipleScatterFresnel, normalDotLight, missingBiasLight);
        float3 diffuseWeight = PathTraceDiffuseWeightFromTerms(specularAlbedoLight, prepared.transmittedView,
            prepared.transmittedAverage);
        diffuseValue = diffuseWeight * diffuse;
        value = diffuseWeight * diffuse + value;
    }

    value = value + PathTraceMultipleScatterFromTerms(surface, prepared.multipleScatterFresnel, prepared.averageMissing,
        missingBiasLight.x, prepared.missingView);

    // The layers above the base, and their own lobes (none for most materials).
    if(prepared.coatScale > 0.0f || PathTraceFuzzWeight(surface) > 0.0f)
    {
        PathTraceLayerTerms layers = PathTracePreparedLayers(surface, prepared);
        float3 layerFactor = PathTraceBaseLayerFactor(surface, layers, direction);
        diffuseValue *= layerFactor;
        value = value * layerFactor + PathTraceEvaluateUpperLobes(surface, layers, direction);
    }

    return value;
}

// Non-delta BSDF value f(l, v) for a single direction, and its diffuse lobe: the same lobe terms and sum as
// PathTraceEvaluateBSDFLobesPrepared, each lobe through its own helper, so nothing that this direction's lobes skip is
// prepared (the shift kernels, which evaluate one direction per vertex, measured slower with prepare then evaluate). A
// caller that uses diffuseValue gives the optimizer another use of the lobe's product, which can change how the total's
// multiply-adds are fused under relaxed floating point (RTXPT D.1B.2 measured a few ulp in the extraction ray
// generations). Zero outside both hemispheres.
float3 PathTraceEvaluateBSDFLobes(PathTraceSurface surface, float3 direction, out float3 diffuseValue)
{
    diffuseValue = float3(0.0f, 0.0f, 0.0f);

    if(!PathTraceDirectionAboveSurface(surface, direction))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float normalDotLight = dot(surface.normal, direction);
    bool baseMirror = PathTraceBaseIsMirror(surface);

    // A delta mirror removes the single-scatter density; its energy is the delta event in PathTraceSampleBSDF.
    float3 value = baseMirror ? float3(0.0f, 0.0f, 0.0f) : PathTraceEvaluateBaseSpecular(surface, direction);

    if(PathTraceMetalness(surface) < 1.0f)
    {
        float3 diffuse = EvaluateEONDiffuse(PathTraceDiffuseAlbedo(surface), surface.material.diffuseRoughness,
            surface.normal, direction, surface.view);
        float3 diffuseWeight = PathTraceDiffuseWeight(surface, normalDotLight);
        diffuseValue = diffuseWeight * diffuse;
        value = diffuseWeight * diffuse + value;
    }

    if(!baseMirror)
    {
        float averageMissing = PathTraceAverageMissingEnergyAndBias(surface).x;

        // The early exit before the two table reads, as before the split.
        if(averageMissing > 0.0f)
        {
            value = value + PathTraceMultipleScatterFromTerms(surface,
                PathTraceMultipleScatterFresnel(surface, averageMissing), averageMissing,
                PathTraceMissingEnergyAndBias(surface, normalDotLight).x,
                PathTraceMissingEnergyAndBias(surface, surface.normalDotView).x);
        }
    }

    if(PathTraceCoatWeight(surface) > 0.0f || PathTraceFuzzWeight(surface) > 0.0f)
    {
        PathTraceLayerTerms layers = PathTraceSurfaceLayers(surface);
        float3 layerFactor = PathTraceBaseLayerFactor(surface, layers, direction);
        diffuseValue *= layerFactor;
        value = value * layerFactor + PathTraceEvaluateUpperLobes(surface, layers, direction);
    }

    return value;
}

float3 PathTraceEvaluateBSDF(PathTraceSurface surface, float3 direction)
{
    float3 diffuseValue;
    return PathTraceEvaluateBSDFLobes(surface, direction, diffuseValue);
}

// -----------------------------------------------------------------------------
// Lobe partition (ReSTIR PT's lobe-indexed BSDF; their sum is PathTraceEvaluateBSDFLobes)
// -----------------------------------------------------------------------------

// The glossy strategy's lobes: the base single scatter through its layers, and the rough coat.
float3 PathTraceEvaluateGlossyLobe(PathTraceSurface surface, float3 direction)
{
    if(!PathTraceDirectionAboveSurface(surface, direction))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 value = PathTraceBaseIsMirror(surface) ? float3(0.0f, 0.0f, 0.0f) :
        PathTraceEvaluateBaseSpecular(surface, direction);

    if(PathTraceCoatWeight(surface) > 0.0f || PathTraceFuzzWeight(surface) > 0.0f)
    {
        PathTraceLayerTerms layers = PathTraceSurfaceLayers(surface);
        value *= PathTraceBaseLayerFactor(surface, layers, direction);

        if(layers.coatScale > 0.0f)
        {
            value += layers.coatScale * PathTraceEvaluateCoatSpecular(surface, layers, direction);
        }
    }

    return value;
}

// The broad strategy's lobes: the base multiple scatter and diffuse through their layers, and the fuzz. diffuseValue is
// the diffuse part.
float3 PathTraceEvaluateBroadLobe(PathTraceSurface surface, float3 direction, out float3 diffuseValue)
{
    diffuseValue = float3(0.0f, 0.0f, 0.0f);

    if(!PathTraceDirectionAboveSurface(surface, direction))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float normalDotLight = dot(surface.normal, direction);
    float3 value = float3(0.0f, 0.0f, 0.0f);

    if(!PathTraceBaseIsMirror(surface))
    {
        float averageMissing = PathTraceAverageMissingEnergyAndBias(surface).x;

        if(averageMissing > 0.0f)
        {
            value = PathTraceMultipleScatterFromTerms(surface, PathTraceMultipleScatterFresnel(surface, averageMissing),
                averageMissing, PathTraceMissingEnergyAndBias(surface, normalDotLight).x,
                PathTraceMissingEnergyAndBias(surface, surface.normalDotView).x);
        }
    }

    if(PathTraceMetalness(surface) < 1.0f)
    {
        diffuseValue = PathTraceDiffuseWeight(surface, normalDotLight) * EvaluateEONDiffuse(
            PathTraceDiffuseAlbedo(surface), surface.material.diffuseRoughness, surface.normal, direction, surface.view);
        value = diffuseValue + value;
    }

    if(PathTraceCoatWeight(surface) > 0.0f || PathTraceFuzzWeight(surface) > 0.0f)
    {
        PathTraceLayerTerms layers = PathTraceSurfaceLayers(surface);
        float3 layerFactor = PathTraceBaseLayerFactor(surface, layers, direction);
        diffuseValue *= layerFactor;
        value *= layerFactor;

        if(PathTraceFuzzWeight(surface) > 0.0f)
        {
            value += PathTraceEvaluateFuzz(surface, direction);
        }
    }

    return value;
}

float PathTraceSmithG1(float alpha, float cosine)
{
    float alphaSquared = alpha * alpha;
    return 2.0f * cosine / max(cosine + sqrt(alphaSquared + (1.0f - alphaSquared) * cosine * cosine), 1.0e-7f);
}

// The bounded VNDF density of the base lobe (Eto and Tokuyoshi 2023), in solid angle of l.
float PathTraceBaseVisibleNormalPdf(PathTraceSurface surface, float3 direction)
{
    float3 tangent;
    float3 bitangent;
    PathTraceBaseFrame(surface, tangent, bitangent);
    float3 localView = OpenPBRToLocal(surface.view, tangent, bitangent, surface.normal);
    localView.z = surface.normalDotView;
    return OpenPBRBoundedVndfPdf(localView, OpenPBRToLocal(direction, tangent, bitangent, surface.normal),
        PathTraceBaseAlphas(surface));
}

// The bounded VNDF density of the rough coat's lobe.
float PathTraceCoatVisibleNormalPdf(PathTraceSurface surface, float3 direction)
{
    float3 coatNormal = PathTraceCoatNormal(surface);
    float3 tangent;
    float3 bitangent;
    PathTraceBuildFrame(coatNormal, tangent, bitangent);
    float alpha = PathTraceGGXAlpha(saturate(surface.material.coatRoughness));
    return OpenPBRBoundedVndfPdf(OpenPBRToLocal(surface.view, tangent, bitangent, coatNormal),
        OpenPBRToLocal(direction, tangent, bitangent, coatNormal), float2(alpha, alpha));
}

// The glossy strategy's density: the coat and base VNDFs mixed by coatSelection.
float PathTraceVisibleNormalPdf(PathTraceSurface surface, float3 direction)
{
    float density = 0.0f;

    if(surface.coatSelection < 1.0f)
    {
        density = (1.0f - surface.coatSelection) * PathTraceBaseVisibleNormalPdf(surface, direction);
    }

    if(surface.coatSelection > 0.0f)
    {
        density += surface.coatSelection * PathTraceCoatVisibleNormalPdf(surface, direction);
    }

    return density;
}

// The broad strategy's density: cosine and the fuzz LTC mixed by fuzzSelection.
float PathTraceBroadPdf(PathTraceSurface surface, float3 direction)
{
    float density = 0.0f;

    if(surface.fuzzSelection < 1.0f)
    {
        density = (1.0f - surface.fuzzSelection) * max(dot(surface.normal, direction), 0.0f) * INV_PI;
    }

    if(surface.fuzzSelection > 0.0f)
    {
        float3 axisX;
        float2 inverseLtc;
        PathTraceFuzzTerms(surface, axisX, inverseLtc);
        density += surface.fuzzSelection * OpenPBRFuzzDistributionFromTerms(surface.normal, axisX, inverseLtc,
            direction);
    }

    return density;
}

// Density with which PathTraceSampleBSDF generates a non-delta direction.
float PathTracePdf(PathTraceSurface surface, float3 direction)
{
    float broadPdf = (1.0f - surface.specularProbability) * PathTraceBroadPdf(surface, direction);
    float glossyProbability = PathTraceGlossyProbability(surface);

    if(glossyProbability <= 0.0f)
    {
        return broadPdf;
    }

    return glossyProbability * PathTraceVisibleNormalPdf(surface, direction) + broadPdf;
}

// Samples the base lobe's bounded VNDF.
float3 PathTraceSampleBaseVisibleNormal(PathTraceSurface surface, float2 random)
{
    float3 tangent;
    float3 bitangent;
    PathTraceBaseFrame(surface, tangent, bitangent);
    float3 localView = OpenPBRToLocal(surface.view, tangent, bitangent, surface.normal);
    localView.z = surface.normalDotView;
    float3 local = OpenPBRSampleBoundedVndf(localView, PathTraceBaseAlphas(surface), random);
    return normalize(OpenPBRFromLocal(local, tangent, bitangent, surface.normal));
}

// Samples the rough coat's bounded VNDF.
float3 PathTraceSampleCoatVisibleNormal(PathTraceSurface surface, float2 random)
{
    float3 coatNormal = PathTraceCoatNormal(surface);
    float3 tangent;
    float3 bitangent;
    PathTraceBuildFrame(coatNormal, tangent, bitangent);
    float alpha = PathTraceGGXAlpha(saturate(surface.material.coatRoughness));
    float3 local = OpenPBRSampleBoundedVndf(OpenPBRToLocal(surface.view, tangent, bitangent, coatNormal),
        float2(alpha, alpha), random);
    return normalize(OpenPBRFromLocal(local, tangent, bitangent, coatNormal));
}

// The glossy strategy's direction: the coat (selection < coatSelection) or the base VNDF.
float3 PathTraceSampleGlossyDirection(PathTraceSurface surface, float selection, float2 random)
{
    if(selection < surface.coatSelection)
    {
        return PathTraceSampleCoatVisibleNormal(surface, random);
    }

    return PathTraceSampleBaseVisibleNormal(surface, random);
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

// The broad strategy's direction: the fuzz LTC (selection < fuzzSelection) or cosine.
float3 PathTraceSampleBroadDirection(PathTraceSurface surface, float selection, float2 random)
{
    if(selection < surface.fuzzSelection)
    {
        float3 axisX;
        float2 inverseLtc;
        PathTraceFuzzTerms(surface, axisX, inverseLtc);
        return OpenPBRSampleFuzzFromTerms(surface.normal, axisX, inverseLtc, random);
    }

    return PathTraceSampleCosine(surface, random);
}

// random.x selects the strategy (and, rescaled, the lobe within it); random.yz drive the chosen lobe. Also returns, for
// signal extraction, the diffuse share of the sample's weight (f_diffuse / f per channel, 0 where f is 0 and for a delta
// event, which is wholly specular) and whether a specular (delta or glossy) strategy was selected.
PathTraceBSDFSample PathTraceSampleBSDFLobes(PathTraceSurface surface, float3 random, out float3 diffuseShare,
    out bool specularStrategy)
{
    PathTraceBSDFSample result;
    result.valid = false;
    result.delta = false;
    result.direction = surface.normal;
    result.weight = float3(0.0f, 0.0f, 0.0f);
    result.pdf = 0.0f;
    diffuseShare = float3(0.0f, 0.0f, 0.0f);

    bool chooseSpecular = random.x < surface.specularProbability;
    specularStrategy = chooseSpecular;

    if(surface.deltaSpecular && random.x < surface.deltaProbability)
    {
        result.direction = PathTraceDeltaDirection(surface);
        result.delta = true;

        if(dot(surface.geometricNormal, result.direction) <= 0.0f || surface.deltaProbability <= 0.0f)
        {
            return result;
        }

        result.weight = surface.deltaFresnel / surface.deltaProbability;
        result.valid = all(isfinite(result.weight));
        return result;
    }

    if(chooseSpecular)
    {
        float glossyProbability = PathTraceGlossyProbability(surface);
        float selection = glossyProbability > 0.0f ? saturate((random.x - surface.deltaProbability) / glossyProbability) :
            0.0f;
        result.direction = PathTraceSampleGlossyDirection(surface, selection, random.yz);
    }
    else
    {
        float broadProbability = 1.0f - surface.specularProbability;
        float selection = broadProbability > 0.0f ?
            saturate((random.x - surface.specularProbability) / broadProbability) : 0.0f;
        result.direction = PathTraceSampleBroadDirection(surface, selection, random.yz);
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

    float3 diffuseValue;
    float3 value = PathTraceEvaluateBSDFLobes(surface, result.direction, diffuseValue);
    result.weight = value * dot(surface.normal, result.direction) / result.pdf;
    result.valid = all(isfinite(result.weight));
    diffuseShare = select(value > 0.0f, saturate(diffuseValue / max(value, 1.0e-30f)), 0.0f);
    return result;
}

PathTraceBSDFSample PathTraceSampleBSDF(PathTraceSurface surface, float3 random)
{
    float3 diffuseShare;
    bool specularStrategy;
    return PathTraceSampleBSDFLobes(surface, random, diffuseShare, specularStrategy);
}

// The smallest GGX alpha (raw perceptual roughness squared, as ReSTIR PT's reconnection criteria use it) of the glossy
// strategy's lobes: the base (its narrower anisotropic axis) and a rough coat.
float PathTraceGlossyMinimumAlpha(PathTraceSurface surface)
{
    float anisotropy = PathTraceAnisotropic(surface) ? surface.material.anisotropy : 0.0f;
    float alpha = OpenPBRAnisotropicAlphas(PathTraceBaseRoughness(surface), anisotropy, 0.0f).y;

    if(PathTraceCoatWeight(surface) > 0.0f && !PathTraceCoatIsMirror(surface))
    {
        float coatRoughness = saturate(surface.material.coatRoughness);
        alpha = min(alpha, coatRoughness * coatRoughness);
    }

    return alpha;
}

#endif
