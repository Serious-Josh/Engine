#pragma once

#include "assets/AssetID.h"
#include "assets/MaterialAssetData.h"
#include "assets/ProjectAssetRegistry.h"

#include "Project/ProjectPaths.h"

#include <filesystem>
#include <string>

class ProjectMaterialAssetService
{
    public:
        static bool Create(
            const ProjectPaths& projectPaths,
            ProjectAssetRegistry& registry,
            const std::filesystem::path& relativePath,
            const MaterialAssetData& material,
            AssetID& createdID,
            std::string* errorMessage = nullptr);
};