
#include "assets/ProjectMaterialAssetService.h"

#include "assets/AssetRecord.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/MaterialAssetSerializer.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace
{
    bool Fail(std::string* errorMessage, const std::string& message)
    {
        if(errorMessage)
        {
            *errorMessage = message;
        }

        return false;
    }

    bool IsContained(
        const std::filesystem::path& directory,
        const std::filesystem::path& candidate)
    {
        const auto relative = candidate.lexically_relative(directory);

        if(relative.empty() || relative.is_absolute())
        {
            return false;
        }

        for(const auto& component : relative)
        {
            if(component == "..")
            {
                return false;
            }
        }

        return true;
    }

    std::string WindowsError(const std::string& message)
    {
        return message + " Windows error " + std::to_string(GetLastError()) + ".";
    }

    bool WriteAll(HANDLE file, const std::string& contents)
    {
        std::size_t offset = 0;

        while(offset < contents.size())
        {
            const std::size_t remaining = contents.size() - offset;

            const DWORD requested = static_cast<DWORD>(
                std::min<std::size_t>(
                    remaining,
                    (std::numeric_limits<DWORD>::max)()));

            DWORD written = 0;

            if(!WriteFile(
                    file,
                    contents.data() + offset,
                    requested,
                    &written,
                    nullptr))
            {
                return false;
            }

            if(written == 0)
            {
                return false;
            }

            offset += written;
        }

        return true;
    }

    bool DeleteOpenedFile(HANDLE file)
    {
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;

        return SetFileInformationByHandle(
            file,
            FileDispositionInfo,
            &disposition,
            sizeof(disposition)) != 0;
    }
}

bool ProjectMaterialAssetService::Create(
    const ProjectPaths& projectPaths,
    ProjectAssetRegistry& registry,
    const std::filesystem::path& relativePath,
    const MaterialAssetData& material,
    AssetID& createdID,
    std::string* errorMessage)
{
    namespace fs = std::filesystem;

    if(errorMessage)
    {
        errorMessage->clear();
    }

    HANDLE file = INVALID_HANDLE_VALUE;
    bool fileCreated = false;

    try
    {
        // Validate material contents before creating any files.
        const std::string serialized = MaterialAssetSerializer::Serialize(material).dump(4) + '\n';

        const auto generatedID = AssetID::Generate();

        if(!generatedID)
        {
            return Fail(errorMessage, "Failed to generate material asset ID.");
        }

        const AssetRecord record{
            *generatedID,
            AssetType::Material,
            relativePath
        };

        if(!ValidateAssetRecord(record) ||
           relativePath.extension() != L".material")
        {
            return Fail(errorMessage, "Invalid project material path.");
        }

        // Stage the registry without modifying the live registry.
        ProjectAssetRegistry stagedRegistry = registry;

        if(!stagedRegistry.Register(record))
        {
            return Fail(errorMessage, "Material asset ID or relative path is already registered.");
        }

        // Validate the registry before creating the material file.
        AssetRegistrySerializer::Serialize(stagedRegistry);

        std::error_code ec;

        const fs::path materialsDirectory = fs::canonical(projectPaths.GetMaterialsDirectory(), ec);

        if(ec)
        {
            return Fail(errorMessage, "Failed to resolve project Materials directory: " + ec.message());
        }

        const fs::path destination = projectPaths.GetRoot() / relativePath;

        const fs::path destinationDirectory = fs::canonical(destination.parent_path(), ec);

        if(ec)
        {
            return Fail(errorMessage, "Failed to resolve material destination directory: " + ec.message());
        }

        if(!IsContained(materialsDirectory, destinationDirectory) &&
           destinationDirectory != materialsDirectory)
        {
            return Fail(errorMessage, "Material destination escapes the project Materials directory.");
        }

        const fs::path finalPath = destinationDirectory / destination.filename();

        // CREATE_NEW fails if the destination already exists.
        // Keeping this handle open also prevents replacement/deletion
        // through ordinary Windows sharing semantics.
        file = CreateFileW(
            finalPath.c_str(),
            GENERIC_WRITE | DELETE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if(file == INVALID_HANDLE_VALUE)
        {
            return Fail(errorMessage, WindowsError("Failed to create material file."));
        }

        fileCreated = true;

        if(!WriteAll(file, serialized))
        {
            throw std::runtime_error(WindowsError("Failed to write material file."));
        }

        if(!FlushFileBuffers(file))
        {
            throw std::runtime_error(WindowsError("Failed to flush material file."));
        }

        std::string registryError;

        if(!AssetRegistrySerializer::SaveToFile(
                stagedRegistry,
                projectPaths.GetAssetRegistryPath(),
                &registryError))
        {
            throw std::runtime_error("Failed to persist material registration: " + registryError);
        }

        // Registry persistence succeeded. Commit the in-memory state.
        registry = std::move(stagedRegistry);
        createdID = *generatedID;

        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;

        if(errorMessage)
        {
            errorMessage->clear();
        }

        return true;
    }
    catch(const std::exception& exception)
    {
        std::string failure = exception.what();

        if(file != INVALID_HANDLE_VALUE)
        {
            if(fileCreated)
            {
                // Delete the exact file we created through its open handle,
                // rather than deleting by path after another process
                // might have replaced it.
                if(!DeleteOpenedFile(file))
                {
                    failure += " Rollback failed; material file may remain. " + WindowsError("Deletion failed.");
                }
            }

            CloseHandle(file);
        }

        return Fail(errorMessage, failure);
    }
}
