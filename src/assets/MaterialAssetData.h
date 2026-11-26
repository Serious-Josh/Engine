#pragma once

#include "Renderer/MaterialTypes.h"
#include "Renderer/OpenPBRMaterialParameters.h"

#include <array>
#include <optional>
#include <string>

// A material asset's authored parameters: the OpenPBR Surface model (OpenPBR 1.1.2) plus Engine's surface-detail and
// texture bindings. MaterialAssetSerializer stores it as schema 2 under OpenPBR's parameter names and migrates the
// schema-1 material models (metallic-roughness, dielectric, conductor) on load.
struct MaterialAssetData
{
    // Base: base_color, base_weight, base_metalness, base_diffuse_roughness.
    std::array<float, 3> baseColor = {1.0f, 1.0f, 1.0f};
    float baseWeight = 1.0f;
    float metallic = 0.0f;
    float diffuseRoughness = 0.0f;

    // Specular: specular_roughness, specular_ior, specular_weight, specular_color,
    // specular_roughness_anisotropy and its rotation in degrees.
    float roughness = 0.5f;
    float ior = 1.5f;
    float specularWeight = 1.0f;
    std::array<float, 3> specularColor = {1.0f, 1.0f, 1.0f};
    float anisotropy = 0.0f;
    float anisotropyRotationDegrees = 0.0f;

    // Coat: coat_weight, coat_color, coat_roughness, coat_ior, coat_darkening, and whether the coat follows the base
    // normal map (otherwise it uses the smooth interpolated normal, the default).
    float coatWeight = 0.0f;
    std::array<float, 3> coatColor = {1.0f, 1.0f, 1.0f};
    float coatRoughness = 0.0f;
    float coatIor = OpenPBRMaterialParameters::DefaultCoatIor;
    float coatDarkening = 1.0f;
    bool coatFollowsBaseNormal = false;

    // Fuzz: fuzz_weight, fuzz_color, fuzz_roughness.
    float fuzzWeight = 0.0f;
    std::array<float, 3> fuzzColor = {1.0f, 1.0f, 1.0f};
    float fuzzRoughness = OpenPBRMaterialParameters::DefaultFuzzRoughness;

    // Thin film: thin_film_weight, thin_film_thickness (micrometres), thin_film_ior.
    float thinFilmWeight = 0.0f;
    float thinFilmThickness = OpenPBRMaterialParameters::DefaultThinFilmThickness;
    float thinFilmIor = OpenPBRMaterialParameters::DefaultThinFilmIor;

    // Engine extension: how much the AO map affects path tracing (0 = not at all, the OpenPBR behaviour).
    float aoDiffuse = 0.0f;
    float aoSpecularWeight = 0.0f;

    SurfaceDetailMode surfaceDetailMode = SurfaceDetailMode::NormalOnly;

    float heightScale = 0.0f;
    float heightBias = 0.0f;

    bool preferARM = true;

    std::optional<std::string> baseColorTexture;
    std::optional<std::string> normalTexture;
    std::optional<std::string> roughnessTexture;
    std::optional<std::string> metallicTexture;
    std::optional<std::string> armTexture;
    std::optional<std::string> aoTexture;
    std::optional<std::string> heightTexture;
};
