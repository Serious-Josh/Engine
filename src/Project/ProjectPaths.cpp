#include "ProjectPaths.h"

#include <array>
#include <system_error>
#include <utility>

ProjectPaths::ProjectPaths(std::filesystem::path projectRoot)
    : m_root(std::move(projectRoot))
{
}

const std::filesystem::path& ProjectPaths::GetRoot() const noexcept
{
    return m_root;
}

std::filesystem::path ProjectPaths::GetScenesDirectory() const
{
    return m_root / "Scenes";
}

std::filesystem::path ProjectPaths::GetMaterialsDirectory() const
{
    return m_root / "Materials";
}

std::filesystem::path ProjectPaths::GetMeshesDirectory() const
{
    return m_root / "Meshes";
}

std::filesystem::path ProjectPaths::GetTexturesDirectory() const
{
    return m_root / "Textures";
}

std::filesystem::path ProjectPaths::GetAssetRegistryPath() const
{
    return m_root / "Assets.registry.json";
}

bool ProjectPaths::EnsureDirectories(
    std::string* errorMessage) const
{
    if (errorMessage)
    {
        errorMessage->clear();
    }

    if (m_root.empty() || !m_root.is_absolute())
    {
        if (errorMessage)
        {
            *errorMessage = "Project root must be an absolute path.";
        }

        return false;
    }

    const std::array directories = {
        GetScenesDirectory(),
        GetMaterialsDirectory(),
        GetMeshesDirectory(),
        GetTexturesDirectory()
    };

    for (const auto& directory : directories)
    {
        std::error_code error;
        std::filesystem::create_directories(directory, error);

        if (error)
        {
            if (errorMessage)
            {
                *errorMessage =
                    "Failed to create project directory: " +
                    directory.string() + " (" + error.message() + ")";
            }

            return false;
        }
    }

    return true;
}