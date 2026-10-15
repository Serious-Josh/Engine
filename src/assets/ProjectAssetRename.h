#pragma once

#include <filesystem>
#include <string>
#include <string_view>

class ProjectPaths;
class ProjectAssetRegistry;
class AssetID;

namespace ProjectAssetRename
{
    bool MoveAssetFile(
        const ProjectPaths& paths, ProjectAssetRegistry& registry, const AssetID& id,
        const std::filesystem::path& source, const std::filesystem::path& directory,
        std::filesystem::path& movedPath, std::string& error);

    bool RenameFile(
        const ProjectPaths& paths,
        ProjectAssetRegistry& registry,
        const std::filesystem::path& source,
        std::string_view stem,
        std::filesystem::path& renamedPath,
        std::string& error);
}
