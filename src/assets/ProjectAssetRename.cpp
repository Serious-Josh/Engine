#include "ProjectAssetRename.h"
#include "Project/ProjectPaths.h"
#include "assets/ProjectAssetRegistry.h"
#include "assets/AssetRegistrySerializer.h"
#include <Windows.h>
#include <cstring>
#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <vector>

namespace
{
    std::filesystem::path Filename(std::string_view stem, const std::filesystem::path& extension)
    {
        if(stem.empty() || stem.size() > 1024 || stem.find("..") != std::string_view::npos)
        {
            throw std::runtime_error("Enter a nonempty filename stem without traversal.");
        }

        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, stem.data(), static_cast<int>(stem.size()), nullptr, 0);

        if(length == 0)
        {
            throw std::runtime_error("The filename is not valid UTF-8.");
        }

        std::wstring name(length, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, stem.data(), static_cast<int>(stem.size()), name.data(), length);

        if(name.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos ||
           std::any_of(name.begin(), name.end(), [](wchar_t value) { return value < 32; }))
        {
            throw std::runtime_error("The filename contains invalid Windows characters.");
        }

        if(name.back() == L' ' || name.back() == L'.' ||
           std::all_of(name.begin(), name.end(), [](wchar_t value) { return std::iswspace(value) != 0; }))
        {
            throw std::runtime_error("The filename cannot be blank or end with a space or period.");
        }

        auto device = name.substr(0, name.find(L'.'));

        while(!device.empty() && device.back() == L' ')
        {
            device.pop_back();
        }

        std::transform(device.begin(), device.end(), device.begin(), [](wchar_t value) { return std::towupper(value); });
        const bool numbered = device.size() == 4 && (device.starts_with(L"COM") || device.starts_with(L"LPT")) &&
            ((device[3] >= L'1' && device[3] <= L'9') || device[3] == L'\u00b9' || device[3] == L'\u00b9' || device[3] == L'\u00b9');

        if(device == L"CON" || device == L"PRN" || device == L"AUX" || device == L"NUL")
        {
            throw std::runtime_error("The filename is a reserved Windows device name.");
        }

        if(numbered || device == L"CONIN$" || device == L"CONOUT$")
        {
            throw std::runtime_error("The filename is a reserved Windows device name.");
        }

        name += extension.native();

        if(name.size() > 255)
        {
            throw std::runtime_error("The filename exceeds the supported 255-character limit.");
        }

        return name;
    }

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

bool ProjectAssetRename::RenameFile(
    const ProjectPaths& paths,
    ProjectAssetRegistry& registry,
    const std::filesystem::path& target,
    std::string_view stem,
    std::filesystem::path& renamedPath,
    std::string& error)
{
    HANDLE file = INVALID_HANDLE_VALUE;
    std::vector<HANDLE> directories;
    std::filesystem::path original;
    std::filesystem::path stagedPath;
    bool renamed = false;
    std::filesystem::path currentPath;
    const auto requested = target;
    error.clear();
    renamedPath.clear();

    try
    {
        const auto root = std::filesystem::canonical(paths.GetRoot());
        original = std::filesystem::absolute(requested).lexically_normal();
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
            throw std::runtime_error("The project asset registry cannot be renamed here.");
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
                throw std::runtime_error("Directory links are not eligible for renaming.");
            }
        }

        file = CreateFileW(original.c_str(), DELETE | FILE_READ_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION info{};

        if(file == INVALID_HANDLE_VALUE)
        {
            throw Failure("Cannot open file for renaming; it may be locked or read-only");
        }

        if(!GetFileInformationByHandle(file, &info) ||
           (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.nNumberOfLinks != 1)
        {
            throw std::runtime_error("Only ordinary, non-linked project files can be renamed.");
        }

        const auto destination = original.parent_path() / Filename(stem, original.extension());

        if(destination.native().size() >= MAX_PATH)
        {
            throw std::runtime_error("The destination exceeds the supported Windows path length.");
        }

        // Allocate the returned path before any physical or registry mutation.
        renamedPath = destination;
        const bool unchanged = original.filename() == destination.filename();
        const bool caseOnly = ProjectAssetRegistry::PathsEqual(original, destination);

        const auto destinationRelative = destination.lexically_relative(root);
        const auto* sourceRecord = registry.FindByPath(relative);
        const auto* conflict = registry.FindByPath(destinationRelative);

        // Compare stable identity before changing the candidate registry. The source's
        // own registration is valid for unchanged and case-only names.
        if(conflict && (!sourceRecord || conflict->id != sourceRecord->id))
        {
            std::error_code statusError;
            const bool exists = std::filesystem::exists(root / conflict->relativePath, statusError);
            std::string detail = "Another asset is registered at '" + conflict->relativePath.generic_string() +
                "' (" + conflict->id.ToReference() + ").";

            if(!statusError && !exists)
            {
                detail += " Its file is missing, but its stable registration is retained. Restore that asset or choose another filename.";
            }

            throw std::runtime_error(detail);
        }

        if(!caseOnly && std::filesystem::exists(destination))
        {
            throw std::runtime_error("A file or directory already uses that filename.");
        }

        ProjectAssetRegistry candidate = registry;
        const bool registered = sourceRecord != nullptr;

        if(registered && !candidate.UpdatePath(sourceRecord->id, destinationRelative))
        {
            throw std::runtime_error("Cannot update the registered asset path.");
        }

        if(!unchanged)
        {
            const auto id = AssetID::Generate();

            if(!id)
            {
                throw std::runtime_error("Cannot generate rename staging identity.");
            }

            // Two no-replacement handle renames also support case-only changes.
            stagedPath = original.parent_path() / (".rename-" + id->ToString() + ".tmp");

            if(!Rename(file, stagedPath))
            {
                throw Failure("Cannot stage file rename");
            }

            renamed = true;
            currentPath = stagedPath;

            if(!Rename(file, destination))
            {
                throw Failure("Cannot rename file; the destination may already exist");
            }

            currentPath = destination;

            if(registered && !AssetRegistrySerializer::SaveToFile(candidate, paths.GetAssetRegistryPath(), &error))
            {
                throw std::runtime_error("Cannot persist renamed asset: " + error);
            }

            registry = std::move(candidate);
        }

        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
        renamedPath.clear();

        if(file != INVALID_HANDLE_VALUE)
        {
            if(renamed && !Rename(file, original))
            {
                error += " Rollback failed; file recovery path: " + currentPath.string() +
                    "; original path: " + original.string();
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
