#pragma once

#include "BRDF.hlsli"

static const uint SurfaceDetailModeNormalOnly = 0u;
static const uint SurfaceDetailModeParallaxOcclusion = 1u;
static const uint SurfaceDetailModeGeometryWithParallaxFallback = 2u;

// OPENPBR_LAYERED_LOBES (default 1) compiles the optional lobes (coat, fuzz, thin film, anisotropy) into the BSDF. Their
// code costs registers even where no material uses them (ReSTIR PT initial sampling measured +4.2 ms on ManyLight_1024),
// so the ReSTIR PT kernels also build a base tier with 0 (ShaderVariantTable.inl), which ReSTIRPTPass runs while no
// published material has one of those lobes. With 0 their flag bits read as 0 here, so every lobe branch folds away.
#ifndef OPENPBR_LAYERED_LOBES
#define OPENPBR_LAYERED_LOBES 1
#endif

// GPUMaterial::lobeFlags (Renderer/ShaderData.h GpuMaterialLobeFlags): which optional OpenPBR lobes a material has, so
// the BSDF skips a lobe's work entirely where its weight is 0.
#if OPENPBR_LAYERED_LOBES
static const uint GPU_MATERIAL_LOBE_COAT = 1u;
static const uint GPU_MATERIAL_LOBE_FUZZ = 2u;
static const uint GPU_MATERIAL_LOBE_THIN_FILM = 4u;
static const uint GPU_MATERIAL_LOBE_ANISOTROPY = 8u;
#else
static const uint GPU_MATERIAL_LOBE_COAT = 0u;
static const uint GPU_MATERIAL_LOBE_FUZZ = 0u;
static const uint GPU_MATERIAL_LOBE_THIN_FILM = 0u;
static const uint GPU_MATERIAL_LOBE_ANISOTROPY = 0u;
#endif
// The coat follows the base's (normal-mapped) shading normal; otherwise it uses the smooth interpolated normal.
static const uint GPU_MATERIAL_COAT_FOLLOWS_BASE_NORMAL = 16u;
// aoDiffuse or aoSpecularWeight is non-zero: the path tracer applies the material AO (Engine extension, see the
// OpenPBR report §9.2).
static const uint GPU_MATERIAL_PATH_TRACED_AO = 32u;

// One material's OpenPBR Surface parameters (Renderer/ShaderData.h has the C++ twin; 192 bytes in 16-byte rows). The
// authored parameters keep their OpenPBR meaning; dielectricF0, coatF0, coatInternalReflection and lobeFlags are
// derived on the CPU (Renderer/OpenPBRMaterialParameters.h).
struct GPUMaterial
{
    uint lobeFlags;
    float roughness;        // specular_roughness (the path tracer replaces it by the resolved texture value)
    float ior;              // specular_ior
    float metallic;         // base_metalness (the path tracer replaces it by the resolved texture value)

    float3 baseColor;
    uint textureIndex;

    float3 specularColor;   // dielectric specular tint and metal F82 edge tint
    uint roughnessTextureIndex;

    float3 coatColor;       // the coat's transmittance squared at normal incidence
    uint metallicTextureIndex;

    float3 fuzzColor;
    uint normalTextureIndex;

    uint armTextureIndex;
    uint preferARM;
    uint aoTextureIndex;
    uint surfaceDetailMode;

    uint heightTextureIndex;
    float heightScale;
    float heightBias;
    float baseWeight;

    float diffuseRoughness;     // base_diffuse_roughness (EON sigma)
    float specularWeight;
    float dielectricF0;         // derived: the base dielectric's Schlick F0 (specular_ior, specular_weight, coat)
    float anisotropy;           // specular_roughness_anisotropy

    float anisotropyRotation;   // radians about the shading normal, from the surface tangent
    float coatWeight;
    float coatRoughness;
    float coatIor;

    float coatDarkening;
    float coatF0;                   // derived: the coat's Schlick F0
    float coatInternalReflection;   // derived: K_r = 1 - (1 - E_F(coat_ior)) / coat_ior^2 (OpenPBR coat darkening)
    float fuzzWeight;

    float fuzzRoughness;
    float thinFilmWeight;
    float thinFilmThickness;    // micrometres
    float thinFilmIor;

    float aoDiffuse;
    float aoSpecularWeight;
    float diffuseOcclusion;     // 1 in the table; the path tracer's surface builders set the AO-to-diffuse factor
    uint reserved1;
};

// The material AO the path-traced AO controls read (OpenPBR report §9.2): ARM red where the material prefers ARM (the
// caller already fetched it), otherwise the AO texture, otherwise none.
float OpenPBRMaterialOcclusion(GPUMaterial material, SamplerState materialSampler, float2 uv, bool useARM,
    float sampledARMOcclusion)
{
    if(useARM)
    {
        return saturate(sampledARMOcclusion);
    }

    if(material.aoTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> aoTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.aoTextureIndex)];
        return saturate(aoTexture.SampleLevel(materialSampler, uv, 0.0f).r);
    }

    return 1.0f;
}

// Engine's opt-in path-traced AO (OpenPBR has none; report §9.2), applied once where the path tracer builds a surface
// of a GPU_MATERIAL_PATH_TRACED_AO material. AO to diffuse scales the diffuse albedo, as Omniverse's "AO to diffuse"
// does. AO to specular weight scales specular_weight, Adobe's suggested emulation of micro-occlusion; the derived
// dielectric F0 is linear in specular_weight (Renderer/OpenPBRMaterialParameters.h DielectricF0), so it scales too.
void OpenPBRApplyOcclusion(inout GPUMaterial material, float occlusion)
{
    float ao = saturate(occlusion);
    float specularFactor = lerp(1.0f, ao, saturate(material.aoSpecularWeight));
    material.diffuseOcclusion = lerp(1.0f, ao, saturate(material.aoDiffuse));
    material.specularWeight *= specularFactor;
    material.dielectricF0 *= specularFactor;
}

// Legacy shading (deferred, RT reflections, RTGI, material previews) keeps its metallic-roughness BRDF and maps every
// OpenPBR material onto it: the base dielectric's F0 (from specular_ior and specular_weight) blended towards the metal
// colour by metalness, and the weighted base colour as the diffuse albedo. Coat, fuzz, thin film and anisotropy are
// path-tracer features; Legacy ignores them (OpenPBR report §4.15).
float3 LegacyMaterialF0(GPUMaterial material, float3 baseColor, float metallic)
{
    float dielectricF0 = saturate(material.dielectricF0);
    return lerp(float3(dielectricF0, dielectricF0, dielectricF0), saturate(material.baseWeight) * baseColor,
        saturate(metallic));
}

float3 LegacyMaterialDiffuseColor(GPUMaterial material, float3 baseColor, float metallic)
{
    return saturate(material.baseWeight) * baseColor * (1.0f - saturate(metallic));
}

// The diffuse reflectance and specular F0 of an OpenPBR material for the denoiser guides (NRD demodulation and DLSS-RR's
// albedo guides), from the resolved base colour and metalness, at normal incidence: the diffuse lobe's
// (1 - metalness) base_weight base_color and the metalness mix of the tinted dielectric F0 and the metal F0, both seen
// through the coat (its transmittance (1 - coatF0) coat_color, and the coat's own F0 added to the specular).
void OpenPBRMaterialReflectanceGuide(GPUMaterial material, float3 baseColor, float metallic, out float3 diffuseAlbedo,
    out float3 specularF0)
{
    float metalness = saturate(metallic);
    float3 metalF0 = saturate(saturate(material.baseWeight) * baseColor);
    float dielectricF0 = saturate(material.dielectricF0);
    diffuseAlbedo = (1.0f - metalness) * saturate(material.baseWeight) * baseColor;
    specularF0 = lerp(dielectricF0 * saturate(material.specularColor), saturate(material.specularWeight) * metalF0,
        metalness);

    if((material.lobeFlags & GPU_MATERIAL_LOBE_COAT) != 0u)
    {
        float coatWeight = saturate(material.coatWeight);
        float coatF0 = saturate(material.coatF0);
        float3 throughCoat = lerp(float3(1.0f, 1.0f, 1.0f), (1.0f - coatF0) * saturate(material.coatColor), coatWeight);
        diffuseAlbedo *= throughCoat;
        specularF0 = specularF0 * throughCoat + coatWeight * coatF0;
    }
}

float3 EvaluateMetallicRoughnessDirectBRDF(
    GPUMaterial material,
    float3 baseColor,
    float3 normal,
    float3 directionToLight,
    float3 viewDirection,
    float directionalAlbedoLight,
    float directionalAlbedoView,
    float averageAlbedo)
{
    float metallic =
        saturate(
            material.metallic
        );

    float3 F0 = LegacyMaterialF0(material, baseColor, metallic);
    float3 diffuseColor = LegacyMaterialDiffuseColor(material, baseColor, metallic);

    float3 fresnel =
        EvaluateGGXFresnel(
            directionToLight,
            viewDirection,
            F0
        );

    float3 diffuseBRDF =
        EvaluateEONDiffuse(
            diffuseColor,
            material.diffuseRoughness,
            normal,
            directionToLight,
            viewDirection
        );

    float3 specularBRDF =
        EvaluateGGXSpecular(
            material.roughness,
            normal,
            directionToLight,
            viewDirection,
            F0
        );

    specularBRDF +=
        EvaluateGGXMultipleScatteringSchlick(
            directionalAlbedoLight,
            directionalAlbedoView,
            averageAlbedo,
            F0
        );

    float3 diffuseWeight =
        1.0f - fresnel;

    return
        diffuseWeight *
        diffuseBRDF +
        specularBRDF;
}
