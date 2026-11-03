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
    // Audio.1B: FMOD Studio's build output for this project (Content/Audio/Banks). Not created on project setup;
    // FMOD Studio writes it, and discovery treats a missing directory as "no banks".
    std::filesystem::path GetAudioBanksDirectory() const;
    std::filesystem::path GetAssetRegistryPath() const;
    // Shared, packaged project configuration (PS.1); separate from the .engineproj manifest.
    std::filesystem::path GetProjectSettingsPath() const;

    // Creates the standard project content directories.
    bool EnsureDirectories(std::string* errorMessage = nullptr) const;

private:
    std::filesystem::path m_root;
};
