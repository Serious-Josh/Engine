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
    // AS.5 Script component per object (enabled, script asset reference, class name).
    inline constexpr std::uint32_t ScriptVersion = 16;
    // Audio.2A Audio Source and Audio Listener components per object.
    inline constexpr std::uint32_t AudioVersion = 17;
    // Same layout as AudioVersion; collider dimensions and offsets are object-local (ColliderScaleMigration.h).
    inline constexpr std::uint32_t ColliderScaleVersion = 18;
    // M4: per-object acoustic settings.
    inline constexpr std::uint32_t AcousticsVersion = 19;
    // M6.9: Audio Source and Audio Listener reflection settings.
    inline constexpr std::uint32_t ReflectionSettingsVersion = 20;
    // M7: Probe Volumes.
    inline constexpr std::uint32_t ProbeVolumeVersion = 21;
    // M7.1: a Probe Volume's inside-solid filter setting.
    inline constexpr std::uint32_t ProbeFilterVersion = 22;
    // M8.2: a Probe Volume's reflection bake settings and baked reflections reference.
    inline constexpr std::uint32_t BakedReflectionsVersion = 23;
    // M9.5: a Probe Volume's pathing enable, pathing bake settings and baked pathing reference; an Audio Source's pathing opt-in.
    inline constexpr std::uint32_t PathingVersion = 24;
    // AIR.3: an Air Motor per object.
    inline constexpr std::uint32_t AirMotorVersion = 25;
    // AIR.4D D2: an Air Motor's emission origin and emission radius.
    inline constexpr std::uint32_t AirMotorEmissionVersion = 26;
    // AIR.4D D2.1: Air Motor emission semi-axes and rotation replace the emission radius.
    inline constexpr std::uint32_t AirMotorShapeVersion = 27;
    // AIR.4F F2: a Mist Emitter per object.
    inline constexpr std::uint32_t MistEmitterVersion = 28;
    // AIR.4F F4.5.1b: a Mist Emitter's temperature.
    inline constexpr std::uint32_t MistEmitterTemperatureVersion = 29;
    // AIR.4F.2b: a Mist Emitter's mode and a counted list of its mode fields (Point; AIR.4F.2c appends Volume's within this version).
    inline constexpr std::uint32_t MistEmitterModeVersion = 30;
    // AIR.4F.2d.3: the scene's environmental haze, after the camera exposure.
    inline constexpr std::uint32_t EnvironmentalHazeVersion = 31;
    // AIR.4F.2d.3a: the haze's ambient contribution, after its near transition (a V31 haze reads 0: the sun alone).
    inline constexpr std::uint32_t HazeAmbientVersion = 32;
    // AIR.4F.2d.3b: the haze's height bounds, after its ambient (V32 and earlier: off). A V31/V32 haze's clearance stays the haze's: the
    // mist appearance, with its own clearance, is cooked only from V35.
    inline constexpr std::uint32_t HazeHeightVersion = 33;
    // R1.1: the scene's air environment and AIR simulation configuration, after the haze. Earlier payloads read neither (the
    // defaults apply through ResolveAirSimulation).
    inline constexpr std::uint32_t AirConfigurationVersion = 34;
    // R1.3: the scene's mist appearance (SceneDocument::mistRendering), after the air configuration. Earlier payloads have none (the
    // defaults apply), as packaged games rendered before.
    inline constexpr std::uint32_t MistRenderingVersion = 35;
    // R1.4a: the scene's voxel bake settings and manifest reference (SceneDocument::voxelBake), after the mist appearance. Earlier
    // payloads have none, so their scenes have no bake, as packaged games ran before.
    inline constexpr std::uint32_t VoxelBakeVersion = 36;
    // R1.5: moving-solid participation: each physics record's airParticipation (u32), and the scene's AIR moving-solid enable (u32)
    // after the voxel bake. Earlier payloads read Automatic and off.
    inline constexpr std::uint32_t MovingSolidParticipationVersion = 37;
    inline constexpr std::uint32_t Version = MovingSolidParticipationVersion;

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
            version == CameraExposureVersion ||
            version == ScriptVersion ||
            version == AudioVersion ||
            version == ColliderScaleVersion ||
            version == AcousticsVersion ||
            version == ReflectionSettingsVersion ||
            version == ProbeVolumeVersion ||
            version == ProbeFilterVersion ||
            version == BakedReflectionsVersion ||
            version == PathingVersion ||
            version == AirMotorVersion ||
            version == AirMotorEmissionVersion ||
            version == AirMotorShapeVersion ||
            version == MistEmitterVersion ||
            version == MistEmitterTemperatureVersion ||
            version == MistEmitterModeVersion ||
            version == EnvironmentalHazeVersion ||
            version == HazeAmbientVersion ||
            version == HazeHeightVersion ||
            version == AirConfigurationVersion ||
            version == MistRenderingVersion ||
            version == VoxelBakeVersion ||
            version == MovingSolidParticipationVersion;
    }
    struct Policy
    {
        // M3.5: the cooked payload bound matches the asset pak's per-entry payload and read limits (EngineAssetPak::Limits),
        // which a larger scene could not be stored in anyway; the source bound is the JSON the cooker reads. Both bound
        // untrusted input and are checked against actual sizes before reading; nothing is allocated to them.
        size_t maxBytes = 256 * 1024 * 1024, maxObjects = MaximumSceneObjects, maxStringBytes = 4096, maxSlots = 4096;
        std::uintmax_t maxSourceBytes = MaximumSceneSourceBytes;
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
