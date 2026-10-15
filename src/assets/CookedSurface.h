#pragma once
#include "assets/CookedScene.h"
#include "assets/MaterialAssetData.h"
#include "Renderer/ProjectTextureLoader.h"

class ProjectPaths;

namespace CookedSurface
{
    inline constexpr std::uint32_t Version = 1;
    inline constexpr size_t MaterialLimit = 1024 * 1024, TextureLimit = 128 * 1024 * 1024;
    struct Dependency { std::uint64_t generation; EngineAssetPak::Digest sha256; bool operator==(const Dependency&) const = default; };
    struct Material
    {
        AssetID id;
        std::uint64_t generation;
        MaterialAssetData parameters;
        std::array<std::optional<Dependency>,7> dependencies;
    };
    struct Asset
    {
        AssetID id;
        EngineAssetPak::Type type;
        std::uint64_t generation;
        std::vector<std::byte> bytes;
    };
    std::array<std::optional<std::string>,7> References(const MaterialAssetData&);
    ProjectTextureInterpretation Interpretation(size_t slot);
    EngineAssetPak::Digest Hash(std::span<const std::byte>);
    std::string Identity(const CookedMeshSource::ArchiveProvider&, const AssetID&);
    bool Matches(const CookedMeshSource::ArchiveProvider&, const AssetID&, EngineAssetPak::Type, std::uint64_t generation, std::string_view identity);
    std::vector<std::byte> EncodeMaterial(const Material&);
    Material DecodeMaterial(std::span<const std::byte>);
    std::vector<std::byte> EncodeTexture(const AssetID&, std::uint64_t, std::span<const PreparedProjectTexture> variants);
    PreparedProjectTexture DecodeTexture(std::span<const std::byte>, const AssetID&, std::uint64_t, ProjectTextureInterpretation);
    // Development only. Each texture AssetID appears once, with required interpretation variants.
    std::vector<Asset> CookDependencies(const CookedScene::Metadata&, const ProjectPaths&, const ProjectAssetRegistry&, std::uint64_t generation = 1);
}
