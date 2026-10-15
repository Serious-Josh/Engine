#pragma once
#include "Scene/SceneDocument.h"
#include "assets/ArchiveCookedMeshProvider.h"
#include "assets/ProjectAssetRegistry.h"
#include <optional>

namespace CookedScene
{
    inline constexpr std::uint32_t Version = 1;
    struct Policy
    {
        size_t maxBytes = 16 * 1024 * 1024, maxObjects = 8192, maxStringBytes = 4096, maxSlots = 4096;
    };
    struct MeshInput
    {
        AssetID id;
        std::uint64_t generation;
        std::span<const std::byte> payload;
    };
    struct MeshBinding
    {
        CookedMesh::Evidence payload;
        std::uint64_t generation = 0;
        std::vector<MeshMaterialSlot> slots;
        bool operator==(const MeshBinding&) const = default;
    };
    struct EnvironmentInput { AssetID id; std::uint64_t generation; std::span<const std::byte> payload; };
    struct EnvironmentBinding { std::uint64_t generation; EngineAssetPak::Digest sha256; };
    struct Metadata
    {
        SceneDocument document;
        std::optional<EnvironmentBinding> environment;
        // Same order/count as document objects; no mesh means nullopt.
        std::vector<std::optional<MeshBinding>> meshes;
    };
    struct ArchiveScene
    {
        const AssetID sceneAsset;
        const std::uint64_t generation;
        const std::shared_ptr<const CookedMeshSource::ArchiveProvider> provider;
        const Metadata metadata;
        bool ValidateReferences(std::string* error = nullptr) const;
    private:
        ArchiveScene(AssetID id, std::uint64_t gen, std::shared_ptr<const CookedMeshSource::ArchiveProvider> source, Metadata data)
            : sceneAsset(std::move(id)), generation(gen), provider(std::move(source)), metadata(std::move(data)) {}
        friend std::shared_ptr<const ArchiveScene> OpenArchive(const std::filesystem::path&, const std::filesystem::path&, const AssetID&, const Policy&);
    };
    // Authoring only. Explicit prepared mesh bytes, no path discovery/fallback.
    // Throws diagnostic runtime_error; allocation exceptions propagate.
    std::vector<std::byte> Cook(const SceneDocument&, std::span<const MeshInput>, const ProjectAssetRegistry&, const Policy& = {}, const EnvironmentInput* = nullptr);
    std::vector<std::byte> CookFile(const std::filesystem::path&, std::span<const MeshInput>, const ProjectAssetRegistry&, const Policy& = {});
    Metadata Decode(std::span<const std::byte>, const Policy& = {});
    std::shared_ptr<const ArchiveScene> OpenArchive(const std::filesystem::path& toc, const std::filesystem::path& pak,
        const AssetID& sceneAsset, const Policy& = {});
}
