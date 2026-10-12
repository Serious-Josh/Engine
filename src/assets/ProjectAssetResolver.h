#pragma once

#include "assets/ProjectAssetRegistry.h"

#include "Project/ProjectPaths.h"

#include <filesystem>

enum class AssetAvailability
{
    Available,
    Missing,
    InvalidPath,
    Unreadable,
    NotRegistered
};

struct AssetResolution
{
    AssetAvailability status = AssetAvailability::NotRegistered;
    std::filesystem::path resolvedPath;
};

class ProjectAssetResolver
{
    public:
        static AssetResolution Resolve(
            const ProjectPaths& projectPaths,
            const ProjectAssetRegistry& registry,
            const AssetID& id);
};