#include "ProjectAssetDeletion.h"
#include "Project/ProjectPaths.h"
#include "assets/ProjectAssetRegistry.h"
#include "assets/AssetRegistrySerializer.h"
#include <Windows.h>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace
{
    bool Rename(HANDLE file, const std::filesystem::path& destination)
    {
        const auto name = destination.wstring();
        std::vector<unsigned char> storage(offsetof(FILE_RENAME_INFO, FileName) + (name.size() + 1) * sizeof(wchar_t));
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        info->ReplaceIfExists = FALSE;
        info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
        memcpy(info->FileName, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
        return SetFileInformationByHandle(file, FileRenameInfo, info, static_cast<DWORD>(storage.size())) != FALSE;
    }

    std::runtime_error Failure(const char* operation)
    {
        return std::runtime_error(std::string(operation) + " (Windows error " + std::to_string(GetLastError()) + ").");
    }
}

bool ProjectAssetDeletion::RemoveFile(
    const ProjectPaths& paths,
    ProjectAssetRegistry& registry,
    const std::filesystem::path& target,
    std::string& error)
{
    HANDLE file = INVALID_HANDLE_VALUE;
    std::vector<HANDLE> directories;
    std::filesystem::path original;
    std::filesystem::path stagedPath;
    bool renamed = false;
    bool deletionArmed = false;
    error.clear();

    try
    {
        const auto root = std::filesystem::canonical(paths.GetRoot());
        original = std::filesystem::absolute(target).lexically_normal();
        const auto* script = registry.FindByPath(original.lexically_relative(root));

        if(_wcsicmp(original.extension().c_str(), L".as") == 0 || (script && script->type == AssetType::Script))
        {
            throw std::runtime_error("Script deletion is deferred until dependency-aware file management.");
        }

        const auto relative = original.lexically_relative(root);

        if(relative.empty() || relative.is_absolute() || relative == ".")
        {
            throw std::runtime_error("File is outside the project asset root.");
        }

        for(const auto& part : relative)
        {
            if(part == ".." || part.native().find(L':') != std::wstring::npos)
            {
                throw std::runtime_error("Unsafe project file path.");
            }
        }

        if(_wcsicmp(original.c_str(), paths.GetAssetRegistryPath().lexically_normal().c_str()) == 0)
        {
            throw std::runtime_error("The project asset registry cannot be deleted here.");
        }

        // Pin every ancestor: no junction traversal or rename races during the transaction.
        std::filesystem::path prefix;

        for(const auto& part : original.parent_path())
        {
            prefix /= part;

            if(!prefix.has_root_directory())
            {
                continue;
            }

            HANDLE directory = CreateFileW(prefix.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            BY_HANDLE_FILE_INFORMATION info{};

            if(directory == INVALID_HANDLE_VALUE)
            {
                throw Failure("Cannot lock project directory");
            }

            directories.push_back(directory);

            if(!GetFileInformationByHandle(directory, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            {
                throw std::runtime_error("Directory links are not eligible for deletion.");
            }
        }

        file = CreateFileW(original.c_str(), DELETE | FILE_READ_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION info{};

        if(file == INVALID_HANDLE_VALUE)
        {
            throw Failure("Cannot open file for deletion; it may be locked or read-only");
        }

        if(!GetFileInformationByHandle(file, &info) ||
           (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.nNumberOfLinks != 1)
        {
            throw std::runtime_error("Only ordinary, non-linked project files can be deleted.");
        }

        ProjectAssetRegistry candidate = registry;
        bool registered = false;

        for(const auto& record : registry.GetAssets())
        {
            if(ProjectAssetRegistry::PathsEqual(record.relativePath, relative))
            {
                candidate.Unregister(record.id);
                registered = true;
            }
        }

        const auto id = AssetID::Generate();

        if(!id)
        {
            throw std::runtime_error("Cannot generate deletion staging identity.");
        }

        stagedPath = original.parent_path() / (".delete-" + id->ToString() + ".tmp");

        if(!Rename(file, stagedPath))
        {
            throw Failure("Cannot stage file deletion");
        }

        renamed = true;
        FILE_DISPOSITION_INFO disposition{TRUE};

        if(!SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)))
        {
            throw Failure("Cannot prepare physical file deletion");
        }

        deletionArmed = true;

        if(registered && !AssetRegistrySerializer::SaveToFile(candidate, paths.GetAssetRegistryPath(), &error))
        {
            throw std::runtime_error("Cannot persist registry removal: " + error);
        }

        // Deletion was checked before registry commit. Closing the exclusive handle
        // commits the already-armed deletion; no other file handles can retain it.
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        registry = std::move(candidate);
    }
    catch(const std::exception& exception)
    {
        error = exception.what();

        if(file != INVALID_HANDLE_VALUE)
        {
            FILE_DISPOSITION_INFO disposition{FALSE};
            const bool disarmed = !deletionArmed || SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));

            if(!disarmed || (renamed && !Rename(file, original)))
            {
                error += " Rollback failed; recovery staging path: " + stagedPath.string();
            }

            CloseHandle(file);
        }
    }

    for(HANDLE directory : directories)
    {
        CloseHandle(directory);
    }

    return error.empty();
}
