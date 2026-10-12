#include "assets/MaterialAssetSerializer.h"
#include "assets/AssetID.h"

#include <Windows.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{
    using json = nlohmann::json;

    std::string_view ModelName(MaterialModel model)
    {
        switch (model){
            case MaterialModel::MetallicRoughness:
                return "metallicRoughness";
            case MaterialModel::Dielectric:
                return "dielectric";
            case MaterialModel::Conductor:
                return "conductor";
            default:
                throw std::runtime_error("Invalid material model");
        }
    }

    MaterialModel ParseModel(const std::string& value)
    {
        if(value == "metallicRoughness"){
            return MaterialModel::MetallicRoughness;
        }

        if(value == "dielectric"){
            return MaterialModel::Dielectric;
        }

        if(value == "conductor"){
            return MaterialModel::Conductor;
        }

        throw std::runtime_error("Invalid material model");
    }

    std::string_view DetailModeName(SurfaceDetailMode mode)
    {
        switch (mode){
            case SurfaceDetailMode::NormalOnly:
                return "normalOnly";
            case SurfaceDetailMode::ParallaxOcclusion:
                return "parallaxOcclusion";
            case SurfaceDetailMode::GeometryWithParallaxFallback:
                return "geometryWithParallaxFallback";
            default:
                throw std::runtime_error("Invalid surface detail mode");
        }
    }

    SurfaceDetailMode ParseDetailMode(const std::string& value)
    {
        if(value == "normalOnly"){
            return SurfaceDetailMode::NormalOnly;
        }

        if(value == "parallaxOcclusion"){
            return SurfaceDetailMode::ParallaxOcclusion;
        }

        if(value == "geometryWithParallaxFallback"){
            return SurfaceDetailMode::GeometryWithParallaxFallback;
        }

        throw std::runtime_error("Invalid surface detail mode");
    }

    float ReadFiniteFloat(const json& object, const char* key)
    {
        const json& value = object.at(key);

        if(!value.is_number()){
            throw std::runtime_error(std::string(key) + " must be numeric");
        }

        const float result = value.get<float>();

        if(!std::isfinite(result)){
            throw std::runtime_error(std::string(key) + " must be finite");
        }

        return result;
    }

    std::array<float, 3> ReadFloat3(
        const json& object,
        const char* key)
    {
        const json& value = object.at(key);

        if(!value.is_array() || value.size() != 3){
            throw std::runtime_error(std::string(key) + " must contain 3 numbers");
        }

        std::array<float, 3> result{};

        for(std::size_t i = 0; i < 3; ++i)
        {
            if (!value[i].is_number()){
                throw std::runtime_error(std::string(key) + " contains a non-number");
            }

            result[i] = value[i].get<float>();

            if(!std::isfinite(result[i])){
                throw std::runtime_error(std::string(key) + " contains a non-finite value");
            }
        }

        return result;
    }

    void ValidateFloat(float value, const char* name)
    {
        if(!std::isfinite(value)){
            throw std::runtime_error(std::string(name) + " must be finite");
        }
    }

    void ValidateUnitFloat(float value, const char* name)
    {
        ValidateFloat(value, name);

        if(value < 0.0f || value > 1.0f){
            throw std::runtime_error(std::string(name) + " must be between 0 and 1");
        }
    }

    void ValidateMaterial(const MaterialAssetData& material)
    {
        ModelName(material.model);
        DetailModeName(material.surfaceDetailMode);

        for(float value : material.baseColor){
            ValidateUnitFloat(value, "baseColor");
        }

        ValidateUnitFloat(material.roughness, "roughness");
        ValidateUnitFloat(material.metallic, "metallic");

        ValidateFloat(material.ior, "ior");

        if(material.ior <= 0.0f){
            throw std::runtime_error("ior must be positive");
        }

        for(float value : material.conductorN)
        {
            ValidateFloat(value, "conductorN");
            if(value < 0.0f){
                throw std::runtime_error("conductorN must be nonnegative");
            }
        }

        for(float value : material.conductorK)
        {
            ValidateFloat(value, "conductorK");
            if(value < 0.0f){
                throw std::runtime_error("conductorK must be nonnegative");
            }
        }

        ValidateFloat(material.heightScale, "heightScale");
        ValidateFloat(material.heightBias, "heightBias");

        const std::optional<std::string>* textures[] =
        {
            &material.baseColorTexture,
            &material.normalTexture,
            &material.roughnessTexture,
            &material.metallicTexture,
            &material.armTexture,
            &material.aoTexture,
            &material.heightTexture
        };

        for (const auto* texture : textures)
        {
            if(texture->has_value() && !AssetID::ParseReference(**texture).has_value())
            {
                throw std::runtime_error("Invalid texture asset reference");
            }
        }
    }

    std::optional<std::string> ReadTextureReference(
        const json& textures,
        const char* key)
    {
        const json& value = textures.at(key);

        if(value.is_null()){
            return std::nullopt;
        }

        if(!value.is_string()){
            throw std::runtime_error(std::string(key) + " must be null or a string");
        }

        const std::string reference = value.get<std::string>();

        if(!AssetID::ParseReference(reference).has_value()){
            throw std::runtime_error(std::string(key) + " has an invalid asset ID");
        }

        return reference;
    }
}

nlohmann::json MaterialAssetSerializer::Serialize(
    const MaterialAssetData& material)
{
    ValidateMaterial(material);

    return {
        { "schemaVersion", 1 },
        { "model", ModelName(material.model) },
        { "baseColor", material.baseColor },
        { "roughness", material.roughness },
        { "metallic", material.metallic },
        { "ior", material.ior },
        { "conductorN", material.conductorN },
        { "conductorK", material.conductorK },
        { "surfaceDetailMode", DetailModeName(material.surfaceDetailMode) },
        { "heightScale", material.heightScale },
        { "heightBias", material.heightBias },
        { "preferARM", material.preferARM },
        { "textures", {
            { "baseColor", material.baseColorTexture },
            { "normal", material.normalTexture },
            { "roughness", material.roughnessTexture },
            { "metallic", material.metallicTexture },
            { "arm", material.armTexture },
            { "ao", material.aoTexture },
            { "height", material.heightTexture }
        }}
    };
}

bool MaterialAssetSerializer::Deserialize(
    const nlohmann::json& document,
    MaterialAssetData& material,
    std::string* errorMessage)
{
    try
    {
        if(!document.is_object()){
            throw std::runtime_error("Material document must be an object");
        }
    

        if (!document.contains("schemaVersion") ||
            !document.at("schemaVersion").is_number_integer() ||
            document.at("schemaVersion").get<int>() != 1)
        {
            throw std::runtime_error("Unsupported material schema version");
        }

        MaterialAssetData staged{};

        staged.model = ParseModel(document.at("model").get<std::string>());
        staged.baseColor = ReadFloat3(document, "baseColor");
        staged.roughness = ReadFiniteFloat(document, "roughness");
        staged.metallic = ReadFiniteFloat(document, "metallic");
        staged.ior = ReadFiniteFloat(document, "ior");

        staged.conductorN = ReadFloat3(document, "conductorN");
        staged.conductorK = ReadFloat3(document, "conductorK");

        staged.surfaceDetailMode = ParseDetailMode(document.at("surfaceDetailMode").get<std::string>());

        staged.heightScale = ReadFiniteFloat(document, "heightScale");
        staged.heightBias = ReadFiniteFloat(document, "heightBias");
        staged.preferARM = document.at("preferARM").get<bool>();

        const json& textures = document.at("textures");

        if(!textures.is_object()){
            throw std::runtime_error("textures must be an object");
        }

        staged.baseColorTexture = ReadTextureReference(textures, "baseColor");
        staged.normalTexture = ReadTextureReference(textures, "normal");
        staged.roughnessTexture = ReadTextureReference(textures, "roughness");
        staged.metallicTexture = ReadTextureReference(textures, "metallic");
        staged.armTexture = ReadTextureReference(textures, "arm");
        staged.aoTexture = ReadTextureReference(textures, "ao");
        staged.heightTexture = ReadTextureReference(textures, "height");

        ValidateMaterial(staged);

        material = std::move(staged);

        if(errorMessage)
            errorMessage->clear();

        return true;
    }
    catch (const std::exception& exception)
    {
        if(errorMessage){
            *errorMessage = exception.what();
        }

        return false;
    }
}


bool MaterialAssetSerializer::SaveToFile(
    const MaterialAssetData& material,
    const std::filesystem::path& path,
    std::string* errorMessage)
{
    namespace fs = std::filesystem;

    fs::path temporaryPath;

    try
    {
        if(path.empty() ||
           !path.has_filename() ||
           path.extension() != L".material")
        {
            throw std::runtime_error("Material file must have a .material extension.");
        }

        // Validate and serialize before touching the filesystem.
        const std::string serialized = Serialize(material).dump(4) + '\n';

        const fs::path directory = path.has_parent_path() ? path.parent_path() : fs::path(".");

        wchar_t temporaryName[MAX_PATH + 1]{};

        if(GetTempFileNameW(directory.c_str(), L"MAT", 0, temporaryName) == 0)
        {
            throw std::runtime_error("Failed to create temporary material file. Windows error " + std::to_string(GetLastError()));
        }

        temporaryPath = temporaryName;

        {
            std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);

            if(!file)
            {
                throw std::runtime_error("Failed to open temporary material file.");
            }

            file.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));

            file.flush();

            if(!file)
            {
                throw std::runtime_error("Failed to write temporary material file.");
            }

            file.close();

            if(!file)
            {
                throw std::runtime_error("Failed to close temporary material file.");
            }
        }

        if(!MoveFileExW(temporaryPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            throw std::runtime_error("Failed to publish material file. Windows error " + std::to_string(GetLastError()));
        }

        temporaryPath.clear();

        if(errorMessage)
        {
            errorMessage->clear();
        }

        return true;
    }
    catch (const std::exception& exception)
    {
        if(!temporaryPath.empty())
        {
            std::error_code cleanupError;
            fs::remove(temporaryPath, cleanupError);
        }

        if(errorMessage)
        {
            *errorMessage = exception.what();
        }

        return false;
    }
}


bool MaterialAssetSerializer::LoadFromFile(
    const std::filesystem::path& path,
    MaterialAssetData& material,
    std::string* errorMessage)
{
    try
    {
        if(path.empty() ||
           !path.has_filename() ||
           path.extension() != L".material")
        {
            throw std::runtime_error("Material file must have a .material extension.");
        }

        std::ifstream file(path, std::ios::binary);

        if(!file)
        {
            throw std::runtime_error("Failed to open material file.");
        }

        const nlohmann::json document = nlohmann::json::parse(file);

        // Deserialize stages changes and commits only
        // after the entire document passes validation.
        return Deserialize(document, material, errorMessage);
    }
    catch (const std::exception& exception)
    {
        if(errorMessage)
        {
            *errorMessage = exception.what();
        }

        return false;
    }
}