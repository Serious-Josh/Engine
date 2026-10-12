#pragma once

#include "assets/ProjectAssetRegistry.h"

#include <nlohmann/json.hpp>

#include <string>
#include <filesystem>

class AssetRegistrySerializer
{
    public:
        static nlohmann::json Serialize(const ProjectAssetRegistry& registry);

        static bool Deserialize(
            const nlohmann::json& document,
            ProjectAssetRegistry& registry,
            std::string* errorMessage = nullptr);

        static bool SaveToFile(
            const ProjectAssetRegistry& registry,
            const std::filesystem::path& path,
            std::string* errorMessage = nullptr);

        static bool LoadFromFile(
            const std::filesystem::path& path,
            ProjectAssetRegistry& registry,
            std::string* errorMessage = nullptr);
};