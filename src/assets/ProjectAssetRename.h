#pragma once

#include <filesystem>
#include <string>
#include <string_view>

class ProjectPaths;
class ProjectAssetRegistry;

namespace ProjectAssetRename
{
    bool RenameFile(
        const ProjectPaths& paths,
        ProjectAssetRegistry& registry,
        const std::filesystem::path& source,
        std::string_view stem,
        std::filesystem::path& renamedPath,
        std::string& error);
}
