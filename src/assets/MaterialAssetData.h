#pragma once

#include "Renderer/MaterialTypes.h"

#include <array>
#include <optional>
#include <string>

struct MaterialAssetData
{
    MaterialModel model = MaterialModel::Dielectric;

    std::array<float, 3> baseColor = {1.0f, 1.0f, 1.0f};

    float roughness = 0.5f;
    float metallic = 0.0f;
    float ior = 1.536f;

    std::array<float, 3> conductorN = {1.0f, 1.0f, 1.0f};
    std::array<float, 3> conductorK = {0.0f, 0.0f, 0.0f};

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