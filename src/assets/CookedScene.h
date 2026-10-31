#pragma once
#include "Scene/SceneDocument.h"
#include "assets/ArchiveCookedMeshProvider.h"
#include "assets/ProjectAssetRegistry.h"
#include <optional>

namespace CookedScene
{
    inline constexpr std::uint32_t LegacyVersion = 1;
    inline constexpr std::uint32_t BoxPhysicsVersion = 2;
    inline constexpr std::uint32_t SpherePhysicsVersion = 3;
    inline constexpr std::uint32_t CapsulePhysicsVersion = 4;
    inline constexpr std::uint32_t CapsuleRotationVersion = 5;
    inline constexpr std::uint32_t ContactEventsVersion = 6;
    inline constexpr std::uint32_t TriggerColliderVersion = 7;
    inline constexpr std::uint32_t CharacterControllerVersion = 8;
    inline constexpr std::uint32_t HierarchyVersion = 9;
    inline constexpr std::uint32_t CameraVersion = 10;
    inline constexpr std::uint32_t LightVersion = 11;
    inline constexpr std::uint32_t EnvironmentNoneVersion = 12;
    inline constexpr std::uint32_t ObjectLightingVersion = 13;
    // Physical light units (PhotometricUnits.h): no range; scene calibration replaces ambient strength.
    inline constexpr std::uint32_t PhysicalLightingVersion = 14;
    // Photographic exposure on every Camera component and on the Editor viewport camera.
    inline constexpr std::uint32_t CameraExposureVersion = 15;
    inline constexpr std::uint32_t Version = CameraExposureVersion;

    constexpr bool IsSupportedVersion(std::uint32_t version)
    {
        return version == LegacyVersion ||
            version == BoxPhysicsVersion ||
            version == SpherePhysicsVersion ||
            version == CapsulePhysicsVersion ||
            version == CapsuleRotationVersion ||
            version == ContactEventsVersion ||
            version == TriggerColliderVersion ||
            version == CharacterControllerVersion ||
            version == HierarchyVersion ||
            version == CameraVersion ||
            version == LightVersion ||
            version == EnvironmentNoneVersion ||
            version == ObjectLightingVersion ||
            version == PhysicalLightingVersion ||
            version == CameraExposureVersion;
    }
    struct Policy
    {
        size_t maxBytes = 16 * 1024 * 1024, maxObjects = 8193, maxStringBytes = 4096, maxSlots = 4096;
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
        // Retained to verify archive entry metadata against the decoded payload header.
        std::uint32_t payloadVersion = Version;
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
