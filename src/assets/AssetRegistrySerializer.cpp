#include "assets/AssetRegistrySerializer.h"
#include "assets/MeshImportUnitsJson.h"

#include <Windows.h>

#include <fstream>
#include <stdexcept>
#include <system_error>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>

namespace
{
    constexpr int kSchemaVersion = 2;

    const char* AssetTypeToString(AssetType type)
    {
        switch (type){
            case AssetType::Material:
                return "material";
            case AssetType::Mesh:
                return "mesh";
            case AssetType::Texture:
                return "texture";
            default:
                return nullptr;
        }
    }

    bool ParseAssetType(
        const std::string& value,
        AssetType& type)
    {
        if (value == "material")
        {
            type = AssetType::Material;
            return true;
        }

        if (value == "mesh")
        {
            type = AssetType::Mesh;
            return true;
        }

        if (value == "texture")
        {
            type = AssetType::Texture;
            return true;
        }

        return false;
    }

    bool Fail(
        std::string* errorMessage,
        const std::string& message)
    {
        if (errorMessage)
        {
            *errorMessage = message;
        }

        return false;
    }
}

nlohmann::json AssetRegistrySerializer::Serialize(const ProjectAssetRegistry& registry)
{
    nlohmann::json document = {
        {"schemaVersion", 1},
        {"assets", nlohmann::json::array()}
    };

    for(const AssetRecord& record : registry.GetAssets())
    {
        const char* typeName = AssetTypeToString(record.type);

        if(!typeName || !ValidateAssetRecord(record))
        {
            throw std::runtime_error("Registry contains an invalid asset record.");
        }

        document["assets"].push_back({
            {"id", record.id.ToString()},
            {"type", typeName},
            {"path", record.relativePath.generic_string()}
        });

        if(record.type == AssetType::Mesh && record.meshImportUnits != MeshImportUnits{})
        {
            // Older executables must reject policy-bearing registries rather than silently
            // ignoring units. All-Legacy projects retain their historical schema-1 output.
            document["schemaVersion"] = kSchemaVersion;
            document["assets"].back()["importUnits"] = SerializeMeshImportUnits(record.meshImportUnits);
        }
    }

    return document;
}

bool AssetRegistrySerializer::Deserialize(
    const nlohmann::json& document,
    ProjectAssetRegistry& registry,
    std::string* errorMessage)
{
    if (errorMessage)
    {
        errorMessage->clear();
    }

    try
    {
        if (!document.is_object() ||
            !document.contains("schemaVersion") ||
            !document["schemaVersion"].is_number_integer() ||
            document["schemaVersion"].get<int>() < 1 || document["schemaVersion"].get<int>() > kSchemaVersion)
        {
            return Fail(errorMessage, "Unsupported or missing asset registry schema version.");
        }

        if (!document.contains("assets") ||
            !document["assets"].is_array())
        {
            return Fail(errorMessage, "Asset registry must contain an assets array.");
        }

        ProjectAssetRegistry stagedRegistry;

        for (const auto& entry : document["assets"])
        {
            if (!entry.is_object() ||
                !entry.contains("id") ||
                !entry["id"].is_string() ||
                !entry.contains("type") ||
                !entry["type"].is_string() ||
                !entry.contains("path") ||
                !entry["path"].is_string())
            {
                return Fail(errorMessage, "Asset registry contains a malformed entry.");
            }

            const std::string idText = entry["id"].get<std::string>();

            const auto id = AssetID::Parse(idText);

            if (!id)
            {
                return Fail(errorMessage, "Asset registry contains an invalid asset ID.");
            }

            AssetType type;

            if (!ParseAssetType(entry["type"].get<std::string>(), type))
            {
                return Fail(errorMessage, "Asset registry contains an unsupported asset type.");
            }

            const std::string pathText = entry["path"].get<std::string>();

            // Manifest paths use forward slashes.
            // Reject native Windows separators in JSON.
            if (pathText.find('\\') != std::string::npos)
            {
                return Fail(errorMessage, "Asset registry paths must use forward slashes.");
            }

            AssetRecord record{*id, type, std::filesystem::path(pathText)};

            if(entry.contains("importUnits"))
            {
                if(type != AssetType::Mesh || document["schemaVersion"].get<int>() != kSchemaVersion)
                {
                    return Fail(errorMessage, "Import units require a mesh asset and registry schema 2.");
                }

                record.meshImportUnits = DeserializeMeshImportUnits(entry.at("importUnits"));
            }

            if (!ValidateAssetRecord(record))
            {
                return Fail(errorMessage, "Asset registry contains an invalid relative path.");
            }

            if (!stagedRegistry.Register(record))
            {
                return Fail(errorMessage, "Asset registry contains duplicate IDs or paths.");
            }
        }

        // Commit only after every entry has passed validation.
        registry = std::move(stagedRegistry);
        return true;
    }
    catch (const std::exception& exception)
    {
        return Fail(errorMessage, std::string("Failed to deserialize asset registry: ") + exception.what());
    }
}


bool AssetRegistrySerializer::SaveToFile(
    const ProjectAssetRegistry& registry,
    const std::filesystem::path& path,
    std::string* errorMessage)
{
    std::filesystem::path temporaryPath;

    try
    {
        // Complete serialization must succeed before file creation.
        const std::string serialized = Serialize(registry).dump(4) + '\n';

        if (path.empty() || !path.has_filename())
        {
            return Fail(errorMessage, "Invalid asset registry path.");
        }

        const std::filesystem::path directory = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");

        wchar_t temporaryName[MAX_PATH + 1]{};

        if (GetTempFileNameW(directory.c_str(), L"AST", 0, temporaryName) == 0)
        {
            return Fail(errorMessage, "Failed to create temporary asset registry file.");
        }

        temporaryPath = temporaryName;

        {
            std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);

            if (!file)
            {
                throw std::runtime_error("Failed to open temporary asset registry file.");
            }

            file.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));

            file.flush();

            if (!file)
            {
                throw std::runtime_error("Failed to write temporary asset registry file.");
            }

            file.close();

            if (!file)
            {
                throw std::runtime_error("Failed to close temporary asset registry file.");
            }
        }

        if (!MoveFileExW(temporaryPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            const DWORD error = GetLastError();

            throw std::runtime_error("Failed to publish asset registry (Windows error " + std::to_string(error) + ").");
        }

        temporaryPath.clear();

        if (errorMessage)
        {
            errorMessage->clear();
        }

        return true;
    }
    catch (const std::exception& exception)
    {
        if (!temporaryPath.empty())
        {
            std::error_code cleanupError;
            std::filesystem::remove(temporaryPath, cleanupError);
        }

        return Fail(errorMessage, exception.what());
    }
}

bool AssetRegistrySerializer::LoadFromFile(
    const std::filesystem::path& path,
    ProjectAssetRegistry& registry,
    std::string* errorMessage)
{
    try
    {
        std::ifstream file(path, std::ios::binary);

        if (!file)
        {
            return Fail(errorMessage, "Failed to open asset registry file.");
        }

        const nlohmann::json document = nlohmann::json::parse(file);

        return Deserialize(document, registry, errorMessage);
    }
    catch (const std::exception& exception)
    {
        return Fail(errorMessage, exception.what());
    }
}
