#pragma once

#include "Renderer/ShaderData.h"
#include "Renderer/Texture.h"
#include "Renderer/MaterialTypes.h"
#include "Renderer/OpenPBRMaterialParameters.h"

#include <climits>
#include <algorithm>
#include <cmath>

class Pipeline;

// Whether a published material renders with parallax occlusion mapping: SurfaceDetailUsesParallax in
// assets/Shaders/SurfaceDetail.hlsli, which this must match (the renderer picks the G-buffer pipeline with it).
// GeometryWithParallaxFallback renders as its fallback until displaced geometry exists.
inline bool SurfaceDetailUsesParallax(const GPUMaterial& material)
{
    const bool parallaxMode =
        material.surfaceDetailMode == static_cast<std::uint32_t>(SurfaceDetailMode::ParallaxOcclusion) ||
        material.surfaceDetailMode == static_cast<std::uint32_t>(SurfaceDetailMode::GeometryWithParallaxFallback);

    return parallaxMode && material.heightTextureIndex != UINT_MAX && std::abs(material.heightScale) > 1.0e-6f;
}

// A runtime material: OpenPBR Surface parameters (OpenPBR 1.1.2 names in comments) plus its texture bindings. The
// asset form is MaterialAssetData; MaterialParameterPropagation copies one into the other.
struct Material
{
    Pipeline* pipeline = nullptr;

    Texture* texture = nullptr;
    Texture* normalTexture = nullptr;
    Texture* roughnessTexture = nullptr;
    Texture* metallicTexture = nullptr;

    Texture* armTexture = nullptr;
    Texture* aoTexture = nullptr;
    Texture* heightTexture = nullptr;
    // Layered prep (F5): the material's asset names a height texture, resident or still streaming in (heightTexture is the
    // fallback meanwhile), so the ReSTIR PT tier it needs is known from its definition before the texture arrives.
    bool heightTextureAssigned = false;

    SurfaceDetailMode surfaceDetailMode = SurfaceDetailMode::NormalOnly;

    float heightScale = 0.0f;
    float heightBias = 0.0f;

    bool preferARM = true;

    // Base: base_color, base_weight, base_metalness, base_diffuse_roughness.
    float baseColor[3] = {1.0f, 1.0f, 1.0f};
    float baseWeight = 1.0f;
    float metallic = 0.0f;
    float diffuseRoughness = 0.0f;

    // Specular: specular_roughness, specular_ior, specular_weight, specular_color, specular_roughness_anisotropy and
    // its rotation (radians about the shading normal, from the surface tangent).
    float roughness = 0.5f;
    float ior = 1.5f;
    float specularWeight = 1.0f;
    float specularColor[3] = {1.0f, 1.0f, 1.0f};
    float anisotropy = 0.0f;
    float anisotropyRotation = 0.0f;

    // Coat: coat_weight, coat_color, coat_roughness, coat_ior, coat_darkening; the coat normal follows the base's
    // normal map, or the smooth interpolated normal.
    float coatWeight = 0.0f;
    float coatColor[3] = {1.0f, 1.0f, 1.0f};
    float coatRoughness = 0.0f;
    float coatIor = OpenPBRMaterialParameters::DefaultCoatIor;
    float coatDarkening = 1.0f;
    bool coatFollowsBaseNormal = false;
    // Silhouette parallax occlusion mapping: with parallax, the relief cuts the mesh's outline at its open and sharp
    // edges (the default; off keeps the mesh's straight outline).
    bool parallaxSilhouette = true;

    // Fuzz: fuzz_weight, fuzz_color, fuzz_roughness.
    float fuzzWeight = 0.0f;
    float fuzzColor[3] = {1.0f, 1.0f, 1.0f};
    float fuzzRoughness = OpenPBRMaterialParameters::DefaultFuzzRoughness;

    // Thin film: thin_film_weight, thin_film_thickness (micrometres), thin_film_ior.
    float thinFilmWeight = 0.0f;
    float thinFilmThickness = OpenPBRMaterialParameters::DefaultThinFilmThickness;
    float thinFilmIor = OpenPBRMaterialParameters::DefaultThinFilmIor;

    // Engine extension (not OpenPBR): how much the material AO map affects path tracing, as a darkening of the
    // diffuse albedo and of specular_weight. Legacy shading always applies the AO map to indirect lighting.
    float aoDiffuse = 0.0f;
    float aoSpecularWeight = 0.0f;

    unsigned int gpuIndex = 0;

    // Writes every parameter field of data (everything but the texture descriptor indices), including the derived
    // terms, so a material table update and a full rebuild produce the same record.
    void WriteParameters(GPUMaterial& data) const
    {
        data.roughness = roughness;
        data.ior = ior;
        data.metallic = metallic;

        for(size_t component = 0; component < 3; ++component)
        {
            data.baseColor[component] = baseColor[component];
            data.specularColor[component] = specularColor[component];
            data.coatColor[component] = coatColor[component];
            data.fuzzColor[component] = fuzzColor[component];
        }

        data.preferARM = preferARM ? 1u : 0u;
        data.surfaceDetailMode = static_cast<uint32_t>(surfaceDetailMode);
        data.heightScale = heightScale;
        data.heightBias = heightBias;

        data.baseWeight = baseWeight;
        data.diffuseRoughness = diffuseRoughness;
        data.specularWeight = specularWeight;
        data.anisotropy = anisotropy;
        data.anisotropyRotation = anisotropyRotation;

        data.coatWeight = coatWeight;
        data.coatRoughness = coatRoughness;
        data.coatIor = coatIor;
        data.coatDarkening = coatDarkening;

        data.fuzzWeight = fuzzWeight;
        data.fuzzRoughness = fuzzRoughness;

        data.thinFilmWeight = thinFilmWeight;
        data.thinFilmThickness = thinFilmThickness;
        data.thinFilmIor = thinFilmIor;

        data.aoDiffuse = aoDiffuse;
        data.aoSpecularWeight = aoSpecularWeight;
        data.diffuseOcclusion = 1.0f;
        data.reserved1 = 0u;

        // Derived terms.
        data.dielectricF0 = OpenPBRMaterialParameters::DielectricF0(ior, specularWeight, coatWeight, coatIor);
        data.coatF0 = OpenPBRMaterialParameters::CoatF0(coatIor);
        data.coatInternalReflection = OpenPBRMaterialParameters::CoatInternalReflection(coatIor);
        data.lobeFlags = LobeFlags();
    }

    unsigned int LobeFlags() const
    {
        unsigned int flags = 0u;

        if(coatWeight > 0.0f)
        {
            flags |= GpuMaterialLobeFlags::Coat;
        }

        if(fuzzWeight > 0.0f)
        {
            flags |= GpuMaterialLobeFlags::Fuzz;
        }

        if(thinFilmWeight > 0.0f && thinFilmThickness > 0.0f)
        {
            flags |= GpuMaterialLobeFlags::ThinFilm;
        }

        if(anisotropy > 0.0f)
        {
            flags |= GpuMaterialLobeFlags::Anisotropy;
        }

        if(coatFollowsBaseNormal)
        {
            flags |= GpuMaterialLobeFlags::CoatFollowsBaseNormal;
        }

        if(aoDiffuse > 0.0f || aoSpecularWeight > 0.0f)
        {
            flags |= GpuMaterialLobeFlags::PathTracedAO;
        }

        if(parallaxSilhouette)
        {
            flags |= GpuMaterialLobeFlags::ParallaxSilhouette;
        }

        return flags;
    }

    GPUMaterial GetGPUData() const
    {
        GPUMaterial data{};
        WriteParameters(data);

        data.textureIndex = texture ? texture->GetDescriptorIndex() : UINT_MAX;
        data.normalTextureIndex = normalTexture ? normalTexture->GetDescriptorIndex() : UINT_MAX;
        data.roughnessTextureIndex = roughnessTexture ? roughnessTexture->GetDescriptorIndex() : UINT_MAX;
        data.metallicTextureIndex = metallicTexture ? metallicTexture->GetDescriptorIndex() : UINT_MAX;

        data.armTextureIndex = armTexture ? armTexture->GetDescriptorIndex() : UINT_MAX;
        data.aoTextureIndex = aoTexture ? aoTexture->GetDescriptorIndex() : UINT_MAX;
        data.heightTextureIndex = heightTexture ? heightTexture->GetDescriptorIndex() : UINT_MAX;

        return data;
    }
};
