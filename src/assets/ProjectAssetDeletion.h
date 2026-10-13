#pragma once
#include <filesystem>
#include <string>
class ProjectPaths;
class ProjectAssetRegistry;
namespace ProjectAssetDeletion
{
    bool RemoveFile(const ProjectPaths& paths, ProjectAssetRegistry& registry,
        const std::filesystem::path& target, std::string& error);
}
