#pragma once

#include <filesystem>
#include <string>

class ProjectPaths
{
public:
    explicit ProjectPaths(std::filesystem::path projectRoot);

    const std::filesystem::path& GetRoot() const noexcept;
    std::filesystem::path GetScenesDirectory() const;
    std::filesystem::path GetMaterialsDirectory() const;
    std::filesystem::path GetMeshesDirectory() const;
    std::filesystem::path GetTexturesDirectory() const;
    std::filesystem::path GetScriptsDirectory() const;
    std::filesystem::path GetAssetRegistryPath() const;

    // Creates the standard project content directories.
    bool EnsureDirectories(std::string* errorMessage = nullptr) const;

private:
    std::filesystem::path m_root;
};
