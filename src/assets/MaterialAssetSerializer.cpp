#include "assets/MaterialAssetSerializer.h"
#include "assets/AssetID.h"
#include "Renderer/OpenPBRMaterialParameters.h"

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

// Material documents.
//
// Schema 2 (current) stores the OpenPBR Surface parameters under OpenPBR's names (base_color, specular_roughness,
// coat_weight, ...), plus Engine's own fields in camelCase (surface detail, preferARM, the path-traced AO extension and
// the texture references).
//
// Schema 1 described one of three material models; it is migrated on load (OpenPBR report, decisions 1 to 3):
// - metallicRoughness: base colour, roughness and metalness as they were, specular_ior 1.5 (the fixed F0 0.04 it used);
// - dielectric: metalness 0 (the model ignored it), specular_ior from ior;
// - conductor: metalness 1 with base_color and specular_color fitted to the complex IOR (n, k) as F82-tint colours.
// Schema 1 coupled the diffuse Oren-Nayar roughness to the specular roughness; schema 2 has OpenPBR's separate
// base_diffuse_roughness, and migration takes OpenPBR's default 0 (Lambertian), accepting that change of look.
namespace
{
    using json = nlohmann::json;

    constexpr int CurrentSchemaVersion = 2;

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

    std::string_view CoatNormalName(bool followsBaseNormal)
    {
        return followsBaseNormal ? "base" : "smooth";
    }

    bool ParseCoatNormal(const std::string& value)
    {
        if(value == "smooth")
        {
            return false;
        }

        if(value == "base")
        {
            return true;
        }

        throw std::runtime_error("Invalid coat_normal");
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

    void ValidateUnitColor(const std::array<float, 3>& color, const char* name)
    {
        for(float value : color)
        {
            ValidateUnitFloat(value, name);
        }
    }

    void ValidatePositiveFloat(float value, const char* name)
    {
        ValidateFloat(value, name);

        if(value <= 0.0f)
        {
            throw std::runtime_error(std::string(name) + " must be positive");
        }
    }

    void ValidateNonnegativeFloat(float value, const char* name)
    {
        ValidateFloat(value, name);

        if(value < 0.0f)
        {
            throw std::runtime_error(std::string(name) + " must be nonnegative");
        }
    }

    void ValidateMaterial(const MaterialAssetData& material)
    {
        DetailModeName(material.surfaceDetailMode);

        ValidateUnitColor(material.baseColor, "base_color");
        ValidateUnitFloat(material.baseWeight, "base_weight");
        ValidateUnitFloat(material.metallic, "base_metalness");
        ValidateUnitFloat(material.diffuseRoughness, "base_diffuse_roughness");

        ValidateUnitFloat(material.roughness, "specular_roughness");
        ValidatePositiveFloat(material.ior, "specular_ior");
        ValidateNonnegativeFloat(material.specularWeight, "specular_weight");
        ValidateUnitColor(material.specularColor, "specular_color");
        ValidateUnitFloat(material.anisotropy, "specular_roughness_anisotropy");
        ValidateFloat(material.anisotropyRotationDegrees, "specularAnisotropyRotationDegrees");

        ValidateUnitFloat(material.coatWeight, "coat_weight");
        ValidateUnitColor(material.coatColor, "coat_color");
        ValidateUnitFloat(material.coatRoughness, "coat_roughness");
        ValidatePositiveFloat(material.coatIor, "coat_ior");
        ValidateUnitFloat(material.coatDarkening, "coat_darkening");

        ValidateUnitFloat(material.fuzzWeight, "fuzz_weight");
        ValidateUnitColor(material.fuzzColor, "fuzz_color");
        ValidateUnitFloat(material.fuzzRoughness, "fuzz_roughness");

        ValidateUnitFloat(material.thinFilmWeight, "thin_film_weight");
        ValidateNonnegativeFloat(material.thinFilmThickness, "thin_film_thickness");
        ValidatePositiveFloat(material.thinFilmIor, "thin_film_ior");

        ValidateUnitFloat(material.aoDiffuse, "pathTracedAODiffuse");
        ValidateUnitFloat(material.aoSpecularWeight, "pathTracedAOSpecularWeight");

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

    // Every top-level key Deserialize reads from a schema 1 document: schemaVersion, then ReadSchemaOne's model fields and
    // ReadSharedFields' fields. It must match the keys those functions read (each with its own type, so they cannot read
    // from this list): Build Game rejects a schema 1 source with any key outside it (SchemaOneDocumentKeys).
    constexpr std::array<std::string_view, 13> SchemaOneDocumentKeyList =
    {
        "schemaVersion",
        "model",
        "baseColor",
        "roughness",
        "metallic",
        "ior",
        "conductorN",
        "conductorK",
        "surfaceDetailMode",
        "heightScale",
        "heightBias",
        "preferARM",
        "textures"
    };

    // The "textures" keys ReadSharedFields reads (schema 1 and schema 2 alike); it must match them (SchemaOneTextureKeys).
    constexpr std::array<std::string_view, 7> SchemaOneTextureKeyList =
    {
        "baseColor",
        "normal",
        "roughness",
        "metallic",
        "arm",
        "ao",
        "height"
    };

    // The fields schema 1 and schema 2 share under the same names.
    void ReadSharedFields(const json& document, MaterialAssetData& staged)
    {
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
    }

    // Schema 1: one of three material models (see the file comment for the migration).
    void ReadSchemaOne(const json& document, MaterialAssetData& staged)
    {
        const std::string model = document.at("model").get<std::string>();
        staged.baseColor = ReadFloat3(document, "baseColor");
        staged.roughness = ReadFiniteFloat(document, "roughness");
        const float metallic = ReadFiniteFloat(document, "metallic");
        const float ior = ReadFiniteFloat(document, "ior");
        const std::array<float, 3> conductorN = ReadFloat3(document, "conductorN");
        const std::array<float, 3> conductorK = ReadFloat3(document, "conductorK");

        if(model == "metallicRoughness")
        {
            staged.metallic = metallic;
            staged.ior = 1.5f;
        }
        else if(model == "dielectric")
        {
            staged.metallic = 0.0f;
            staged.ior = ior;
        }
        else if(model == "conductor")
        {
            for(size_t channel = 0; channel < 3; ++channel)
            {
                ValidateNonnegativeFloat(conductorN[channel], "conductorN");
                ValidateNonnegativeFloat(conductorK[channel], "conductorK");
            }

            const OpenPBRMaterialParameters::MetalColors colors =
                OpenPBRMaterialParameters::MetalColorsFromComplexIor(conductorN, conductorK);
            staged.metallic = 1.0f;
            staged.ior = 1.5f;
            staged.baseColor = colors.baseColor;
            staged.specularColor = colors.edgeTint;
        }
        else
        {
            throw std::runtime_error("Invalid material model");
        }

        ReadSharedFields(document, staged);

        // A coat over parallax relief follows the base normal by default (the smooth normal would read as a flat sheet
        // of glass over it; POM report decision 4).
        staged.coatFollowsBaseNormal = staged.surfaceDetailMode != SurfaceDetailMode::NormalOnly;
    }

    void ReadSchemaTwo(const json& document, MaterialAssetData& staged)
    {
        staged.baseColor = ReadFloat3(document, "base_color");
        staged.baseWeight = ReadFiniteFloat(document, "base_weight");
        staged.metallic = ReadFiniteFloat(document, "base_metalness");
        staged.diffuseRoughness = ReadFiniteFloat(document, "base_diffuse_roughness");

        staged.roughness = ReadFiniteFloat(document, "specular_roughness");
        staged.ior = ReadFiniteFloat(document, "specular_ior");
        staged.specularWeight = ReadFiniteFloat(document, "specular_weight");
        staged.specularColor = ReadFloat3(document, "specular_color");
        staged.anisotropy = ReadFiniteFloat(document, "specular_roughness_anisotropy");
        staged.anisotropyRotationDegrees = ReadFiniteFloat(document, "specularAnisotropyRotationDegrees");

        staged.coatWeight = ReadFiniteFloat(document, "coat_weight");
        staged.coatColor = ReadFloat3(document, "coat_color");
        staged.coatRoughness = ReadFiniteFloat(document, "coat_roughness");
        staged.coatIor = ReadFiniteFloat(document, "coat_ior");
        staged.coatDarkening = ReadFiniteFloat(document, "coat_darkening");
        staged.coatFollowsBaseNormal = ParseCoatNormal(document.at("coatNormal").get<std::string>());

        staged.fuzzWeight = ReadFiniteFloat(document, "fuzz_weight");
        staged.fuzzColor = ReadFloat3(document, "fuzz_color");
        staged.fuzzRoughness = ReadFiniteFloat(document, "fuzz_roughness");

        staged.thinFilmWeight = ReadFiniteFloat(document, "thin_film_weight");
        staged.thinFilmThickness = ReadFiniteFloat(document, "thin_film_thickness");
        staged.thinFilmIor = ReadFiniteFloat(document, "thin_film_ior");

        staged.aoDiffuse = ReadFiniteFloat(document, "pathTracedAODiffuse");
        staged.aoSpecularWeight = ReadFiniteFloat(document, "pathTracedAOSpecularWeight");

        ReadSharedFields(document, staged);
    }
}

std::span<const std::string_view> MaterialAssetSerializer::SchemaOneDocumentKeys()
{
    return SchemaOneDocumentKeyList;
}

std::span<const std::string_view> MaterialAssetSerializer::SchemaOneTextureKeys()
{
    return SchemaOneTextureKeyList;
}

nlohmann::json MaterialAssetSerializer::Serialize(
    const MaterialAssetData& material)
{
    ValidateMaterial(material);

    return {
        { "schemaVersion", CurrentSchemaVersion },
        { "base_color", material.baseColor },
        { "base_weight", material.baseWeight },
        { "base_metalness", material.metallic },
        { "base_diffuse_roughness", material.diffuseRoughness },
        { "specular_roughness", material.roughness },
        { "specular_ior", material.ior },
        { "specular_weight", material.specularWeight },
        { "specular_color", material.specularColor },
        { "specular_roughness_anisotropy", material.anisotropy },
        { "specularAnisotropyRotationDegrees", material.anisotropyRotationDegrees },
        { "coat_weight", material.coatWeight },
        { "coat_color", material.coatColor },
        { "coat_roughness", material.coatRoughness },
        { "coat_ior", material.coatIor },
        { "coat_darkening", material.coatDarkening },
        { "coatNormal", CoatNormalName(material.coatFollowsBaseNormal) },
        { "fuzz_weight", material.fuzzWeight },
        { "fuzz_color", material.fuzzColor },
        { "fuzz_roughness", material.fuzzRoughness },
        { "thin_film_weight", material.thinFilmWeight },
        { "thin_film_thickness", material.thinFilmThickness },
        { "thin_film_ior", material.thinFilmIor },
        { "pathTracedAODiffuse", material.aoDiffuse },
        { "pathTracedAOSpecularWeight", material.aoSpecularWeight },
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

        if(!document.contains("schemaVersion") || !document.at("schemaVersion").is_number_integer())
        {
            throw std::runtime_error("Unsupported material schema version");
        }

        const int schemaVersion = document.at("schemaVersion").get<int>();
        MaterialAssetData staged{};

        if(schemaVersion == 1)
        {
            ReadSchemaOne(document, staged);
        }
        else if(schemaVersion == CurrentSchemaVersion)
        {
            ReadSchemaTwo(document, staged);
        }
        else
        {
            throw std::runtime_error("Unsupported material schema version");
        }

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