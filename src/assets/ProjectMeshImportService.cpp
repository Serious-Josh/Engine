#include "assets/ProjectMeshImportService.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/ProjectAssetRegistry.h"
#include "assets/ModelImporter.h"
#include "assets/SupportedAssetImportFormats.h"
#include "assets/ProjectAssetResolver.h"
#include "Project/ProjectPaths.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <limits>
#include <vector>

namespace
{
    bool Contained(const std::filesystem::path& root, const std::filesystem::path& path)
    {
        const auto relative = path.lexically_relative(root);

        if(relative.empty() || relative.is_absolute())
        {
            return false;
        }

        for(const auto& part : relative)
        {
            if(part == "..")
            {
                return false;
            }
        }

        return true;
    }

    std::runtime_error WindowsFailure(const char* operation)
    {
        return std::runtime_error(std::string(operation) + " Windows error " + std::to_string(GetLastError()));
    }

    class DirectoryLocks
    {
    public:
        ~DirectoryLocks()
        {
            for(HANDLE directory : m_handles)
            {
                CloseHandle(directory);
            }
        }

        void Pin(const std::filesystem::path& path)
        {
            std::filesystem::path prefix;

            for(const auto& part : path)
            {
                prefix /= part;

                if(!prefix.has_root_directory())
                {
                    continue;
                }

                HANDLE directory = CreateFileW(prefix.c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);

                if(directory == INVALID_HANDLE_VALUE)
                {
                    throw WindowsFailure("Cannot pin mesh import directory.");
                }

                BY_HANDLE_FILE_INFORMATION info{};
                const bool valid = GetFileInformationByHandle(directory, &info) &&
                    (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);

                if(!valid)
                {
                    CloseHandle(directory);
                    throw std::runtime_error("Mesh import directories must be ordinary, non-linked directories.");
                }

                try
                {
                    m_handles.push_back(directory);
                }
                catch(...)
                {
                    CloseHandle(directory);
                    throw;
                }
            }
        }

    private:
        std::vector<HANDLE> m_handles;
    };
}

bool ProjectMeshImportService::Import(
    const ProjectPaths& paths,
    ProjectAssetRegistry& registry,
    const std::filesystem::path& source,
    std::filesystem::path& importedPath,
    std::string& error,
    MeshImportUnits units)
{
    namespace fs = std::filesystem;
    HANDLE input = INVALID_HANDLE_VALUE;
    HANDLE output = INVALID_HANDLE_VALUE;
    DirectoryLocks directories;

    importedPath.clear();
    error.clear();

    try
    {
        if(!units.IsValid())
        {
            throw std::runtime_error("Invalid mesh import-unit policy.");
        }

        if(SupportedAssetImportFormats::Lookup(source) != AssetType::Mesh)
        {
            error = "Unsupported mesh import format. Use .fbx.";
            return false;
        }

        const auto filename = source.filename().wstring();

        if(filename.empty() || filename.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos ||
           filename.back() == L'.' || filename.back() == L' ')
        {
            throw std::runtime_error("Unsafe mesh filename.");
        }

        if(paths.GetRoot().empty() || !paths.GetRoot().is_absolute())
        {
            throw std::runtime_error("Mesh import requires an absolute project root.");
        }

        // Pin ancestors before canonicalization so neither the file destination
        // nor the registry directory can be redirected during publication.
        directories.Pin(paths.GetMeshesDirectory().lexically_normal());
        const auto root = fs::canonical(paths.GetRoot());
        const auto destination = fs::canonical(paths.GetMeshesDirectory());

        if(!Contained(root, destination))
        {
            throw std::runtime_error("Mesh directory must be within the active project root.");
        }

        const auto sourcePath = fs::canonical(source);
        directories.Pin(sourcePath.parent_path());

        if(!fs::is_regular_file(sourcePath))
        {
            throw std::runtime_error("Source is not a regular file.");
        }

        const auto id = AssetID::Generate();

        if(!id || registry.Find(*id))
        {
            throw std::runtime_error("Failed to generate a unique mesh AssetID.");
        }

        const auto temporary = destination / (L".import-" + fs::path(id->ToString()).wstring() + L".tmp");
        input = CreateFileW(sourcePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

        if(input == INVALID_HANDLE_VALUE)
        {
            throw WindowsFailure("Cannot open mesh source.");
        }

        output = CreateFileW(temporary.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);

        if(output == INVALID_HANDLE_VALUE)
        {
            throw WindowsFailure("Cannot create import staging file.");
        }

        std::array<char, 65536> buffer{};
        DWORD read = 0;

        for(;;)
        {
            if(!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            {
                throw WindowsFailure("Cannot read mesh source.");
            }

            if(read == 0)
            {
                break;
            }

            DWORD offset = 0;

            while(offset < read)
            {
                DWORD written = 0;

                if(!WriteFile(output, buffer.data() + offset, read - offset, &written, nullptr) || written == 0)
                {
                    throw WindowsFailure("Cannot copy mesh data.");
                }

                offset += written;
            }
        }

        // The source handle excludes writes/deletion. Assimp validates the same
        // bytes just copied while the writable staging handle stays protected.
        ModelImporter::LoadFlattened(sourcePath, MeshUVOrientation::PreserveSource,
            128 * 1024 * 1024, units);

        CloseHandle(input);
        input = INVALID_HANDLE_VALUE;

        if(!FlushFileBuffers(output))
        {
            throw WindowsFailure("Cannot flush mesh copy.");
        }

        fs::path finalPath;
        ProjectAssetRegistry staged;

        for(unsigned int suffix = 0;; ++suffix)
        {
            if(suffix == (std::numeric_limits<unsigned int>::max)())
            {
                throw std::runtime_error("No available mesh filename suffix.");
            }

            const auto name = suffix == 0 ? source.filename() :
                fs::path(source.stem().wstring() + L" (" + std::to_wstring(suffix) + L")" + source.extension().wstring());
            finalPath = destination / name;
            const AssetRecord record{*id, AssetType::Mesh, finalPath.lexically_relative(root), units};
            staged = registry;

            if(!ValidateAssetRecord(record))
            {
                throw std::runtime_error("Invalid destination registry path.");
            }

            if(!staged.Register(record))
            {
                continue;
            }

            AssetRegistrySerializer::Serialize(staged);
            const auto wide = finalPath.wstring();
            // Win32 expects FileName to be null-terminated; FileNameLength still
            // excludes the terminator. Without it, publication can append heap bytes.
            const size_t bytes = offsetof(FILE_RENAME_INFO, FileName) + (wide.size() + 1) * sizeof(wchar_t);
            std::vector<unsigned char> storage(bytes);
            auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
            rename->ReplaceIfExists = FALSE;
            rename->RootDirectory = nullptr;
            rename->FileNameLength = static_cast<DWORD>(wide.size() * sizeof(wchar_t));
            std::memcpy(rename->FileName, wide.data(), rename->FileNameLength);
            rename->FileName[wide.size()] = L'\0';

            if(SetFileInformationByHandle(output, FileRenameInfo, rename, static_cast<DWORD>(bytes)))
            {
                break;
            }

            const DWORD failure = GetLastError();

            if(failure != ERROR_ALREADY_EXISTS && failure != ERROR_FILE_EXISTS)
            {
                throw WindowsFailure("Cannot publish mesh copy.");
            }
        }

        // Keep the exact published file locked against replacement until registration commits.
        if(!AssetRegistrySerializer::SaveToFile(staged, paths.GetAssetRegistryPath(), &error))
        {
            throw std::runtime_error("Cannot persist mesh registry: " + error);
        }

        registry = std::move(staged);
        importedPath = std::move(finalPath);
        CloseHandle(output);
        return true;
    }
    catch(const std::exception& exception)
    {
        error = exception.what();

        if(input != INVALID_HANDLE_VALUE)
        {
            CloseHandle(input);
        }

        if(output != INVALID_HANDLE_VALUE)
        {
            FILE_DISPOSITION_INFO disposition{};
            disposition.DeleteFile = TRUE;

            if(!SetFileInformationByHandle(output, FileDispositionInfo, &disposition, sizeof(disposition)))
            {
                error += " Rollback failed; an unregistered import file may remain.";
            }

            CloseHandle(output);
        }

        return false;
    }
}

bool ProjectMeshImportService::ChangeImportUnits(const ProjectPaths& paths, ProjectAssetRegistry& registry,
    const AssetRecord& expected, MeshImportUnits units, std::string& error)
{
    HANDLE source = INVALID_HANDLE_VALUE;
    error.clear();

    try
    {
        const auto* current = registry.Find(expected.id);

        if(!units.IsValid() || !current || current->type != AssetType::Mesh ||
            current->relativePath != expected.relativePath || current->meshImportUnits != expected.meshImportUnits)
        {
            throw std::runtime_error("Mesh registration or import policy changed; reopen Import Units.");
        }

        DirectoryLocks directories;
        directories.Pin(std::filesystem::absolute(paths.GetRoot()).lexically_normal());
        const auto resolved = ProjectAssetResolver::Resolve(paths, registry, expected.id);

        if(resolved.status != AssetAvailability::Available ||
            _wcsicmp(resolved.resolvedPath.extension().c_str(), L".fbx") != 0 ||
            std::filesystem::file_size(resolved.resolvedPath) > 64 * 1024 * 1024)
        {
            throw std::runtime_error("Import-unit changes require an available FBX source no larger than 64 MiB.");
        }

        directories.Pin(resolved.resolvedPath.parent_path());
        source = CreateFileW(resolved.resolvedPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

        if(source == INVALID_HANDLE_VALUE)
        {
            throw WindowsFailure("Cannot pin mesh source for policy validation.");
        }

        LARGE_INTEGER sourceBytes{};

        if(!GetFileSizeEx(source, &sourceBytes) || sourceBytes.QuadPart <= 0 || sourceBytes.QuadPart > 64 * 1024 * 1024)
        {
            throw std::runtime_error("Pinned mesh source is empty or exceeds the 64 MiB policy-change limit.");
        }

        ModelImporter::LoadFlattened(resolved.resolvedPath, MeshUVOrientation::FlipV, 128 * 1024 * 1024, units);
        auto staged = registry;

        if(!staged.UpdateMeshImportUnits(expected.id, units) ||
            !AssetRegistrySerializer::SaveToFile(staged, paths.GetAssetRegistryPath(), &error))
        {
            throw std::runtime_error("Cannot persist mesh import units: " + error);
        }

        registry = std::move(staged);
        CloseHandle(source);
        return true;
    }
    catch(const std::exception& exception)
    {
        if(source != INVALID_HANDLE_VALUE)
        {
            CloseHandle(source);
        }

        error = exception.what();
        return false;
    }
}
