#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

class ProjectPaths;
class ProjectAssetRegistry;

// M4: how a surface treats sound, independent of how it looks. Coefficients are in [0, 1], for three bands in Steam Audio's
// order (low, mid, high). Absorption and scattering are stored now, though only reflections (M6) will use them.
struct AcousticMaterialData
{
    std::array<float, 3> absorption{0.1f, 0.2f, 0.3f};
    float scattering = 0.05f;
    std::array<float, 3> transmission{0.1f, 0.05f, 0.03f};

    bool operator==(const AcousticMaterialData&) const = default;
};

namespace AcousticMaterials
{
// Project asset file extension, and the directory below the project root such assets live in.
inline constexpr std::wstring_view FileExtension = L".acousticmaterial";
inline constexpr std::string_view BuiltinPrefix = "builtin:acoustic/";

// Every coefficient finite and within [0, 1].
bool Validate(const AcousticMaterialData& material, std::string* error = nullptr);

nlohmann::json Serialize(const AcousticMaterialData& material);
bool Deserialize(const nlohmann::json& document, AcousticMaterialData& material, std::string* error = nullptr);
bool SaveToFile(const AcousticMaterialData& material, const std::filesystem::path& path, std::string* error = nullptr);
bool LoadFromFile(const std::filesystem::path& path, AcousticMaterialData& material, std::string* error = nullptr);

// Valve's Steam Audio presets, with their values: "builtin:acoustic/concrete" and so on.
std::optional<AcousticMaterialData> FindBuiltin(std::string_view reference);
std::vector<std::string> BuiltinReferences();
// What a surface with no usable assignment uses: Steam Audio's own default material.
const AcousticMaterialData& DefaultMaterial() noexcept;

// A built-in, or an AcousticMaterial asset of the project ("asset:<uuid>"). nullopt, with the reason, otherwise.
std::optional<AcousticMaterialData> Resolve(std::string_view reference, const ProjectPaths* paths,
    const ProjectAssetRegistry* registry, std::string* error = nullptr);

// Creates the file at a project-relative path and registers it, persisting the registry, and returns its reference. Nothing is
// changed on failure; an existing file is never replaced.
std::optional<std::string> CreateAsset(const ProjectPaths& paths, ProjectAssetRegistry& registry,
    const std::filesystem::path& relativePath, const AcousticMaterialData& material, std::string* error = nullptr);

// "Concrete" for "builtin:acoustic/concrete".
std::string BuiltinDisplayName(std::string_view reference);

// What a reference field shows. No reference means the default material. A missing or wrong reference stays visible as such,
// and is never redirected to another asset.
struct ReferenceDescription
{
    std::string display;
    std::string detail;
    bool missing = false;
};

ReferenceDescription Describe(std::string_view reference, const ProjectPaths* paths, const ProjectAssetRegistry* registry);
}
