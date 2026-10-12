#pragma once

#include "assets/MaterialAssetData.h"

#include <nlohmann/json.hpp>
#include <string>

class MaterialAssetSerializer
{
    public:
        static nlohmann::json Serialize(const MaterialAssetData& material);

        static bool Deserialize(
            const nlohmann::json& document,
            MaterialAssetData& material,
            std::string* errorMessage = nullptr);

        static bool SaveToFile(
            const MaterialAssetData& material,
            const std::filesystem::path& path,
            std::string* errorMessage = nullptr);

        static bool LoadFromFile(
            const std::filesystem::path& path,
            MaterialAssetData& material,
            std::string* errorMessage = nullptr);
};