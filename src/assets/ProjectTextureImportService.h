#pragma once

#include <filesystem>
#include <string>

class ProjectPaths;
class ProjectAssetRegistry;

class ProjectTextureImportService
{
public:
    // CPU-only copy and registration. Source files are never modified.
    static bool Import(
        const ProjectPaths& paths,
        ProjectAssetRegistry& registry,
        const std::filesystem::path& source,
        const std::filesystem::path& destinationDirectory,
        std::filesystem::path& importedPath,
        std::string& error);
};
