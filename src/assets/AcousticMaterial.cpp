#include "assets/AcousticMaterial.h"

#include "assets/AssetID.h"
#include "assets/AssetRecord.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/ProjectAssetRegistry.h"
#include "assets/ProjectAssetResolver.h"
#include "Project/ProjectPaths.h"

#include <cctype>
#include <cmath>
#include <fstream>
#include <utility>

namespace AcousticMaterials
{
namespace
{
    constexpr int FormatVersion = 1;

    struct BuiltinMaterial
    {
        std::string_view name;
        AcousticMaterialData data;
    };

    // Steam Audio 4.8.1's material assets (unity/.../Resources/Materials/*.asset at tag v4.8.1), value for value.
    const std::array<BuiltinMaterial, 11> Builtins{{
        {"generic", {{0.10f, 0.20f, 0.30f}, 0.05f, {0.100f, 0.050f, 0.030f}}},
        {"brick", {{0.03f, 0.04f, 0.07f}, 0.05f, {0.015f, 0.015f, 0.015f}}},
        {"concrete", {{0.05f, 0.07f, 0.08f}, 0.05f, {0.015f, 0.002f, 0.001f}}},
        {"ceramic", {{0.01f, 0.01f, 0.02f}, 0.05f, {0.060f, 0.044f, 0.011f}}},
        {"gravel", {{0.60f, 0.70f, 0.80f}, 0.05f, {0.031f, 0.012f, 0.008f}}},
        {"carpet", {{0.24f, 0.69f, 0.73f}, 0.05f, {0.020f, 0.005f, 0.003f}}},
        {"glass", {{0.06f, 0.03f, 0.02f}, 0.05f, {0.060f, 0.044f, 0.011f}}},
        {"plaster", {{0.12f, 0.06f, 0.04f}, 0.05f, {0.056f, 0.056f, 0.004f}}},
        {"wood", {{0.11f, 0.07f, 0.06f}, 0.05f, {0.070f, 0.014f, 0.005f}}},
        {"metal", {{0.20f, 0.07f, 0.06f}, 0.05f, {0.200f, 0.025f, 0.010f}}},
        {"rock", {{0.13f, 0.20f, 0.24f}, 0.05f, {0.015f, 0.002f, 0.001f}}},
    }};

    bool InRange(float value)
    {
        return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
    }

    bool Fail(std::string* error, std::string message)
    {
        if(error)
        {
            *error = std::move(message);
        }

        return false;
    }

    bool ReadBands(const nlohmann::json& document, const char* key, std::array<float, 3>& bands)
    {
        if(!document.contains(key) || !document.at(key).is_object())
        {
            return false;
        }

        const nlohmann::json& value = document.at(key);
        const char* names[3] = {"low", "mid", "high"};

        for(std::size_t band = 0; band < 3; ++band)
        {
            if(!value.contains(names[band]) || !value.at(names[band]).is_number())
            {
                return false;
            }

            bands[band] = value.at(names[band]).get<float>();
        }

        return true;
    }

    nlohmann::json WriteBands(const std::array<float, 3>& bands)
    {
        return {{"low", bands[0]}, {"mid", bands[1]}, {"high", bands[2]}};
    }
}

bool Validate(const AcousticMaterialData& material, std::string* error)
{
    for(std::size_t band = 0; band < 3; ++band)
    {
        if(!InRange(material.absorption[band]) || !InRange(material.transmission[band]))
        {
            return Fail(error, "Acoustic material absorption and transmission must be finite and within 0 to 1.");
        }
    }

    if(!InRange(material.scattering))
    {
        return Fail(error, "Acoustic material scattering must be finite and within 0 to 1.");
    }

    return true;
}

nlohmann::json Serialize(const AcousticMaterialData& material)
{
    return {{"version", FormatVersion}, {"absorption", WriteBands(material.absorption)}, {"scattering", material.scattering},
        {"transmission", WriteBands(material.transmission)}};
}

bool Deserialize(const nlohmann::json& document, AcousticMaterialData& material, std::string* error)
{
    if(!document.is_object() || !document.contains("version") || !document.at("version").is_number_integer() ||
        document.at("version").get<int>() != FormatVersion)
    {
        return Fail(error, "Unsupported acoustic material format.");
    }

    AcousticMaterialData candidate;

    if(!ReadBands(document, "absorption", candidate.absorption) || !ReadBands(document, "transmission", candidate.transmission) ||
        !document.contains("scattering") || !document.at("scattering").is_number())
    {
        return Fail(error, "An acoustic material needs absorption, scattering and transmission values.");
    }

    candidate.scattering = document.at("scattering").get<float>();

    if(!Validate(candidate, error))
    {
        return false;
    }

    material = candidate;
    return true;
}

bool SaveToFile(const AcousticMaterialData& material, const std::filesystem::path& path, std::string* error)
{
    if(!Validate(material, error))
    {
        return false;
    }

    // Written beside the file, then renamed over it, so a failed write can't damage an existing material.
    std::filesystem::path temporary = path;
    temporary += L".saving";

    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << Serialize(material).dump(4) << '\n';
        file.flush();

        if(!file.good())
        {
            return Fail(error, "Cannot write acoustic material file.");
        }
    }

    std::error_code filesystemError;
    std::filesystem::rename(temporary, path, filesystemError);

    if(filesystemError)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return Fail(error, "Cannot replace acoustic material file: " + filesystemError.message());
    }

    return true;
}

bool LoadFromFile(const std::filesystem::path& path, AcousticMaterialData& material, std::string* error)
{
    std::ifstream file(path, std::ios::binary);

    if(!file)
    {
        return Fail(error, "Cannot read acoustic material file.");
    }

    const nlohmann::json document = nlohmann::json::parse(file, nullptr, false);

    if(document.is_discarded())
    {
        return Fail(error, "Acoustic material file is not valid JSON.");
    }

    return Deserialize(document, material, error);
}

std::optional<AcousticMaterialData> FindBuiltin(std::string_view reference)
{
    if(!reference.starts_with(BuiltinPrefix))
    {
        return std::nullopt;
    }

    const std::string_view name = reference.substr(BuiltinPrefix.size());

    for(const BuiltinMaterial& builtin : Builtins)
    {
        if(builtin.name == name)
        {
            return builtin.data;
        }
    }

    return std::nullopt;
}

std::vector<std::string> BuiltinReferences()
{
    std::vector<std::string> references;

    for(const BuiltinMaterial& builtin : Builtins)
    {
        references.push_back(std::string(BuiltinPrefix) + std::string(builtin.name));
    }

    return references;
}

const AcousticMaterialData& DefaultMaterial() noexcept
{
    return Builtins[0].data;
}

std::optional<AcousticMaterialData> Resolve(std::string_view reference, const ProjectPaths* paths,
    const ProjectAssetRegistry* registry, std::string* error)
{
    if(reference.starts_with(BuiltinPrefix))
    {
        std::optional<AcousticMaterialData> builtin = FindBuiltin(reference);

        if(!builtin)
        {
            Fail(error, "unknown built-in acoustic material");
        }

        return builtin;
    }

    const auto id = AssetID::ParseReference(std::string(reference));

    if(!id || !paths || !registry)
    {
        Fail(error, "not an acoustic material reference");
        return std::nullopt;
    }

    const AssetRecord* record = registry->Find(*id);

    if(!record || record->type != AssetType::AcousticMaterial)
    {
        Fail(error, "not a registered acoustic material");
        return std::nullopt;
    }

    const AssetResolution resolution = ProjectAssetResolver::Resolve(*paths, *registry, *id);

    if(resolution.status != AssetAvailability::Available)
    {
        Fail(error, "the acoustic material file is unavailable");
        return std::nullopt;
    }

    AcousticMaterialData material;

    if(!LoadFromFile(resolution.resolvedPath, material, error))
    {
        return std::nullopt;
    }

    return material;
}

std::optional<std::string> CreateAsset(const ProjectPaths& paths, ProjectAssetRegistry& registry,
    const std::filesystem::path& relativePath, const AcousticMaterialData& material, std::string* error)
{
    if(!Validate(material, error))
    {
        return std::nullopt;
    }

    const auto id = AssetID::Generate();

    if(!id)
    {
        Fail(error, "Cannot generate an asset ID.");
        return std::nullopt;
    }

    const AssetRecord record{*id, AssetType::AcousticMaterial, relativePath};
    ProjectAssetRegistry staged = registry;

    if(!ValidateAssetRecord(record) || !staged.Register(record))
    {
        Fail(error, "Invalid or already registered acoustic material path.");
        return std::nullopt;
    }

    const std::filesystem::path destination = paths.GetRoot() / relativePath;
    std::error_code filesystemError;
    std::filesystem::create_directories(destination.parent_path(), filesystemError);

    if(std::filesystem::exists(destination, filesystemError))
    {
        Fail(error, "A file with this name already exists.");
        return std::nullopt;
    }

    if(!SaveToFile(material, destination, error))
    {
        return std::nullopt;
    }

    std::string registryError;

    if(!AssetRegistrySerializer::SaveToFile(staged, paths.GetAssetRegistryPath(), &registryError))
    {
        std::filesystem::remove(destination, filesystemError);
        Fail(error, "Cannot persist the registration: " + registryError);
        return std::nullopt;
    }

    registry = std::move(staged);
    return id->ToReference();
}

std::string BuiltinDisplayName(std::string_view reference)
{
    std::string name(reference.starts_with(BuiltinPrefix) ? reference.substr(BuiltinPrefix.size()) : reference);

    if(!name.empty())
    {
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    }

    return name;
}

ReferenceDescription Describe(std::string_view reference, const ProjectPaths* paths, const ProjectAssetRegistry* registry)
{
    ReferenceDescription description;

    if(reference.empty())
    {
        description.display = "Default material";
        description.detail = "No assignment: this surface uses Steam Audio's default material.";
        return description;
    }

    if(reference.starts_with(BuiltinPrefix))
    {
        if(FindBuiltin(reference))
        {
            description.display = BuiltinDisplayName(reference) + " (built-in)";
            description.detail = "A read-only Steam Audio preset.";
        }
        else
        {
            description.display = "Missing built-in: " + std::string(reference);
            description.detail = "Unknown preset; this surface uses the default material.";
            description.missing = true;
        }

        return description;
    }

    const auto id = AssetID::ParseReference(std::string(reference));
    const AssetRecord* record = id && registry ? registry->Find(*id) : nullptr;

    if(!record || record->type != AssetType::AcousticMaterial)
    {
        description.display = "Missing acoustic material";
        description.detail = std::string(reference) + ": not a registered Acoustic Material. This surface uses the default material.";
        description.missing = true;
        return description;
    }

    description.display = record->relativePath.stem().string();
    description.detail = record->relativePath.generic_string();

    if(paths && ProjectAssetResolver::Resolve(*paths, *registry, *id).status != AssetAvailability::Available)
    {
        description.display = "Missing: " + description.display;
        description.detail += "\nThe file is unavailable. This surface uses the default material.";
        description.missing = true;
    }

    return description;
}
}
