#pragma once

#include <filesystem>
#include <string>
#include "assets/AssetRecord.h"

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
        std::string& error,
        MeshImportUnits units = {MeshImportUnitMode::FileUnits, 1.0});

    // Caller rejects active-scene references and conflicting Editor operations first.
    // Validate the new geometry under a source pin, persist a staged registry, then adopt it.
    static bool ChangeImportUnits(const ProjectPaths& paths, ProjectAssetRegistry& registry,
        const AssetRecord& expected, MeshImportUnits units, std::string& error);
};
