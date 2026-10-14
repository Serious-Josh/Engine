#pragma once

#include <filesystem>
#include <string>

class ProjectPaths;
class ProjectAssetRegistry;

class ProjectMeshImportService
{
public:
    // CPU-only static FBX validation, copy to Meshes, and persistent registration.
    // Duplicate imports receive independent AssetIDs; the source is never modified.
    static bool Import(
        const ProjectPaths& paths,
        ProjectAssetRegistry& registry,
        const std::filesystem::path& source,
        std::filesystem::path& importedPath,
        std::string& error);
};
