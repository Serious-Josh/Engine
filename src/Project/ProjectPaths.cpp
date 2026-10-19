#include "ProjectPaths.h"

#include <array>
#include <system_error>
#include <utility>
#include <Windows.h>

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

std::filesystem::path ProjectPaths::GetScriptsDirectory() const
{
    return m_root / "Content" / "Scripts";
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

    // Reject an existing Content/Scripts junction before creating anything through it.
    // Hold Content against relocation while creating/validating its Scripts child.
    HANDLE content = INVALID_HANDLE_VALUE;
    const auto closeContent = [&]
    {
        if(content != INVALID_HANDLE_VALUE)
        {
            CloseHandle(content);
        }
    };

    for(const auto& directory : {m_root / "Content", GetScriptsDirectory()})
    {
        std::error_code error;
        std::filesystem::create_directory(directory, error);
        const DWORD attributes = GetFileAttributesW(directory.c_str());

        if(error || attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            closeContent();

            if(errorMessage)
            {
                *errorMessage = "Scripts require ordinary project-owned Content/Scripts directories.";
            }

            return false;
        }

        if(content == INVALID_HANDLE_VALUE)
        {
            content = CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            BY_HANDLE_FILE_INFORMATION info{};

            if(content == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(content, &info) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            {
                closeContent();

                if(errorMessage)
                {
                    *errorMessage = "Cannot pin the ordinary project Content directory.";
                }

                return false;
            }
        }
    }

    closeContent();

    return true;
}
