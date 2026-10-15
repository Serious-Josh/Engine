#include "Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <stdexcept>

namespace
{
    using Json = nlohmann::json;

    Json ToJson(const Vector3& value)
    {
        return Json::array({value.x, value.y, value.z});
    }

    Vector3 ReadVector3(const Json& value)
    {
        if (!value.is_array() || value.size() != 3)
        {
            throw std::runtime_error("Expected a 3-element vector.");
        }

        return {
            value.at(0).get<float>(),
            value.at(1).get<float>(),
            value.at(2).get<float>()
        };
    }

    Json ToJson(const Transform& value)
    {
        return {
            {"position", ToJson(value.position)},
            {"rotation", ToJson(value.rotation)},
            {"scale", ToJson(value.scale)}
        };
    }

    Transform ReadTransform(const Json& value)
    {
        Transform result;
        result.position = ReadVector3(value.at("position"));
        result.rotation = ReadVector3(value.at("rotation"));
        result.scale = ReadVector3(value.at("scale"));
        return result;
    }

    Json ToJson(const SceneDocument& document)
    {
        Json objects = Json::array();

        for (const SceneObjectDocument& object : document.objects)
        {
            objects.push_back({
                {"id", object.id},
                {"name", object.name},
                {"transform", ToJson(object.transform)},
                {"meshReference", object.meshReference},
                {"materialReference", object.materialReference}
            });

            if(!object.materialSlotBindings.empty())
            {
                Json bindings = Json::array();

                for(const auto& [slotID, reference] : object.materialSlotBindings)
                {
                    bindings.push_back({{"slotID", slotID}, {"materialReference", reference}});
                }

                objects.back()["materialSlotBindings"] = std::move(bindings);
            }
        }

        const auto& camera = document.camera;
        const auto& light = document.directionalLight;

        return {
            {"schemaVersion", document.schemaVersion},
            {"id", document.id},
            {"name", document.name},
            {"environmentReference",document.environmentReference},
            {"objects", std::move(objects)},
            {"camera", {
                {"position", ToJson(camera.position)},
                {"target", ToJson(camera.target)},
                {"verticalFov", camera.verticalFov},
                {"aspectRatio", camera.aspectRatio},
                {"nearPlane", camera.nearPlane},
                {"farPlane", camera.farPlane}
            }},
            {"directionalLight", {
                {"direction", ToJson(light.direction)},
                {"color", ToJson(light.color)},
                {"intensity", light.intensity},
                {"ambientStrength", light.ambientStrength},
                {"angularRadius", light.angularRadius}
            }}
        };
    }

    SceneDocument FromJson(const Json& value)
    {
        SceneDocument result;

        result.schemaVersion = value.at("schemaVersion").get<std::uint32_t>();
        result.id = value.at("id").get<std::string>();
        result.name = value.at("name").get<std::string>();
        result.environmentReference=value.value("environmentReference",std::string{});

        const Json& camera = value.at("camera");
        result.camera.position = ReadVector3(camera.at("position"));
        result.camera.target = ReadVector3(camera.at("target"));
        result.camera.verticalFov = camera.at("verticalFov").get<float>();
        result.camera.aspectRatio = camera.at("aspectRatio").get<float>();
        result.camera.nearPlane = camera.at("nearPlane").get<float>();
        result.camera.farPlane = camera.at("farPlane").get<float>();

        const Json& light = value.at("directionalLight");
        result.directionalLight.direction = ReadVector3(light.at("direction"));
        result.directionalLight.color = ReadVector3(light.at("color"));
        result.directionalLight.intensity = light.at("intensity").get<float>();
        result.directionalLight.ambientStrength =
            light.at("ambientStrength").get<float>();
        result.directionalLight.angularRadius =
            light.at("angularRadius").get<float>();

        const Json& objects = value.at("objects");

        if (!objects.is_array())
        {
            throw std::runtime_error("Scene objects must be an array.");
        }

        for (const Json& item : objects)
        {
            SceneObjectDocument object;
            object.id = item.at("id").get<std::string>();
            object.name = item.at("name").get<std::string>();
            object.transform = ReadTransform(item.at("transform"));
            object.meshReference =
                item.at("meshReference").get<std::string>();
            object.materialReference =
                item.at("materialReference").get<std::string>();

            if(item.contains("materialSlotBindings"))
            {
                const auto& bindings = item.at("materialSlotBindings");

                if(!bindings.is_array())
                {
                    throw std::runtime_error("Material-slot bindings must be an array.");
                }

                for(const auto& binding : bindings)
                {
                    const auto slotID = binding.at("slotID").get<std::string>();
                    const auto reference = binding.at("materialReference").get<std::string>();

                    if(!object.materialSlotBindings.emplace(slotID, reference).second)
                    {
                        throw std::runtime_error("Duplicate material-slot ID: " + slotID);
                    }
                }
            }

            result.objects.push_back(std::move(object));
        }

        return result;
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

static CreateSceneDocumentResult WriteSceneDocument(
    const SceneDocument& document,
    const std::filesystem::path& path,
    std::string* errorMessage,
    bool replaceExisting)
{
    std::string validationError;

    if (!ValidateSceneDocument(document, &validationError))
    {
        Fail(errorMessage, validationError);
        return CreateSceneDocumentResult::Error;
    }

    std::filesystem::path temporaryPath;

    try
    {
        // Serialize before touching the filesystem.
        const std::string serialized = ToJson(document).dump(4) + '\n';

        if (path.empty() || path.filename().empty())
        {
            Fail(errorMessage, "Invalid scene file path.");
            return CreateSceneDocumentResult::Error;
        }

        // The destination directory must already exist.
        const std::filesystem::path directory =
            path.has_parent_path()
                ? path.parent_path()
                : std::filesystem::path(".");

        wchar_t temporaryName[MAX_PATH + 1]{};

        // Creates a unique, empty file in the destination directory.
        if (GetTempFileNameW(
                directory.c_str(),
                L"SCN",
                0,
                temporaryName) == 0)
        {
            Fail(
                errorMessage,
                "Failed to create temporary scene file.");
            return CreateSceneDocumentResult::Error;
        }

        temporaryPath = temporaryName;

        {
            std::ofstream file(
                temporaryPath,
                std::ios::binary | std::ios::trunc);

            if (!file)
            {
                throw std::runtime_error(
                    "Failed to open temporary scene file.");
            }

            file.write(serialized.data(),
                       static_cast<std::streamsize>(serialized.size()));

            file.flush();

            if (!file)
            {
                throw std::runtime_error(
                    "Failed to write temporary scene file.");
            }

            file.close();

            if (!file)
            {
                throw std::runtime_error(
                    "Failed to close temporary scene file.");
            }
        }

        // Same-directory rename publishes the complete file. Never allow a copy fallback.
        if (!MoveFileExW(
                temporaryPath.c_str(),
                path.c_str(),
                MOVEFILE_WRITE_THROUGH |
                    (replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0)))
        {
            const DWORD error = GetLastError();

            if (!replaceExisting && (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS))
            {
                std::error_code cleanupError;
                std::filesystem::remove(temporaryPath, cleanupError);

                if (cleanupError)
                {
                    throw std::runtime_error("Destination collision; temporary cleanup failed: " +
                        cleanupError.message());
                }

                Fail(errorMessage, "Destination scene file already exists.");
                return CreateSceneDocumentResult::Collision;
            }

            throw std::runtime_error(
                "Failed to publish scene file (Windows error " + std::to_string(error) + ").");
        }

        temporaryPath.clear();

        if (errorMessage)
        {
            errorMessage->clear();
        }

        return CreateSceneDocumentResult::Created;
    }
    catch (const std::exception& exception)
    {
        if (!temporaryPath.empty())
        {
            std::error_code cleanupError;
            std::filesystem::remove(temporaryPath, cleanupError);
        }

        Fail(errorMessage, exception.what());
        return CreateSceneDocumentResult::Error;
    }
}

bool SaveSceneDocument(
    const SceneDocument& document,
    const std::filesystem::path& path,
    std::string* errorMessage)
{
    return WriteSceneDocument(document, path, errorMessage, true) == CreateSceneDocumentResult::Created;
}

CreateSceneDocumentResult CreateSceneDocument(
    const SceneDocument& document,
    const std::filesystem::path& path,
    std::string* errorMessage)
{
    return WriteSceneDocument(document, path, errorMessage, false);
}

bool LoadSceneDocument(
    const std::filesystem::path& path,
    SceneDocument& document,
    std::string* errorMessage)
{
    try
    {
        std::ifstream file(path, std::ios::binary);

        if (!file)
        {
            return Fail(errorMessage, "Failed to open scene file.");
        }

        Json json = Json::parse(file);

        SceneDocument loaded = FromJson(json);

        std::string validationError;

        if (!ValidateSceneDocument(loaded, &validationError))
        {
            return Fail(errorMessage, validationError);
        }

        document = std::move(loaded);

        if (errorMessage)
        {
            errorMessage->clear();
        }

        return true;
    }
    catch (const std::exception& exception)
    {
        return Fail(errorMessage, exception.what());
    }
}

bool LoadSceneDocumentForCooking(const std::filesystem::path& path, SceneDocument& document, size_t maxBytes, std::string* error)
{
    try
    {
        const auto size = std::filesystem::file_size(path);
        if(size > maxBytes || size > SIZE_MAX) throw std::runtime_error("Loose scene exceeds cooker admission.");
        std::ifstream stream(path,std::ios::binary);
        std::string bytes(static_cast<size_t>(size),'\0');
        if(!stream.read(bytes.data(),static_cast<std::streamsize>(bytes.size())) || stream.peek()!=std::char_traits<char>::eof())
            throw std::runtime_error("Cannot read stable bounded cooker input.");
        const auto json = Json::parse(bytes);
        const auto keys = [](const Json& value, std::initializer_list<std::string_view> allowed)
        {
            if(!value.is_object()) throw std::runtime_error("Expected scene object fields.");
            for(auto i=value.begin();i!=value.end();++i)
                if(std::find(allowed.begin(),allowed.end(),i.key())==allowed.end())
                    throw std::runtime_error("Unsupported scene element: " + i.key());
        };
        keys(json,{"schemaVersion","id","name","camera","directionalLight","objects","environmentReference"});
        keys(json.at("camera"),{"position","target","verticalFov","aspectRatio","nearPlane","farPlane"});
        keys(json.at("directionalLight"),{"direction","color","intensity","ambientStrength","angularRadius"});
        for(const auto& object:json.at("objects"))
        {
            keys(object,{"id","name","transform","meshReference","materialReference","materialSlotBindings"});
            keys(object.at("transform"),{"position","rotation","scale"});
            if(object.contains("materialSlotBindings"))
                for(const auto& binding:object.at("materialSlotBindings")) keys(binding,{"slotID","materialReference"});
        }
        auto candidate = FromJson(json);
        if(!ValidateSceneDocument(candidate,error)) return false;
        document = std::move(candidate); return true;
    }
    catch(const std::exception& exception) { if(error) *error=exception.what(); return false; }
}
