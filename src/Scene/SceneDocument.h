#pragma once

#include "Scene/Transform.h"
#include "Scene/SceneLightingSettings.h"
#include "Scene/CameraComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/ScriptComponent.h"
#include "Scene/AudioComponents.h"
#include "Scene/AirMotorComponent.h"
#include "Scene/MistEmitterComponent.h"
#include "Scene/AirEnvironmentSettings.h"
#include "Scene/AirSimulationSettings.h"
#include "Scene/EnvironmentalHazeSettings.h"
#include "Scene/MistRenderSettings.h"
#include "Scene/VoxelComponents.h"
#include "Physics/PhysicsComponent.h"
#include "Physics/TriggerColliderComponent.h"
#include "Physics/CharacterControllerComponent.h"

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <optional>

struct SceneCameraDocument
{
    Vector3 position{0.0f, 0.0f, -5.0f};
    Vector3 target{0.0f, 0.0f, 0.0f};

    float verticalFov = 1.04719755f;
    float aspectRatio = 16.0f / 9.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    // The Editor viewport camera's own exposure, independent of every scene Camera.
    CameraExposure exposure;
};

struct SceneObjectDocument
{
    std::string id;
    std::string name = "Object";


    Transform transform;

    //persistant asset references
    std::string meshReference;
    std::string materialReference;

    // Canonical material asset references keyed by stable mesh-local slot ID.
    // Missing entries inherit materialReference (or builtin:default when empty).
    std::map<std::string, std::string> materialSlotBindings;

    // Optional authored configuration; no runtime body or transform ownership.
    std::optional<Physics::PhysicsComponent> physics;

    // Optional authored trigger geometry; owns no sensor resources.
    std::optional<Physics::TriggerColliderComponent> triggerCollider;

    // Generic Optional authored configuration; exclusive with PhysicsComponent.
    std::optional<Physics::CharacterControllerComponent> characterController;

    // Optional authored lens; owns no pose, render target or GPU resources.
    std::optional<CameraComponent> camera;

    // Optional authored light; owns no GPU resource. Persisted from schema 11 / cooked payload 11.
    std::optional<LightComponent> light;

    // Optional script attachment; owns no instance. Persisted from schema 15 / cooked payload 16.
    std::optional<ScriptComponent> script;

    // Optional audio intent; owns no event instance. Persisted from schema 16 / cooked payload 17.
    std::optional<AudioSourceComponent> audioSource;
    std::optional<AudioListenerComponent> audioListener;

    // Inherent acoustic settings of a mesh object; absent means disabled. Persisted from schema 18 / cooked payload 19.
    std::optional<AcousticSettings> acoustics;

    // M7: a volume of acoustic probes. Persisted from cooked payload 21; the optional field needs no new scene schema.
    std::optional<ProbeVolumeComponent> probeVolume;

    // VX.4B: voxel participation and orientation; absent means Automatic. Persisted from schema 19; not yet in cooked payloads.
    std::optional<VoxelSettings> voxel;

    // AIR.3: an Air Motor; owns no simulation state. Persisted from schema 20 / cooked payload 25.
    std::optional<AirMotorComponent> airMotor;

    // AIR.4F F2: a Mist Emitter; owns no simulation state. Persisted from schema 23 / cooked payload 28.
    std::optional<MistEmitterComponent> mistEmitter;

    // Empty means root; otherwise the exact ID of another object in this scene. Persisted from
    // schema 9 / cooked payload 9; historical versions load every object as a root.
    std::string parentID;
};

struct SceneDocument
{
    static constexpr std::uint32_t kLegacySchemaVersion = 1;
    static constexpr std::uint32_t kBoxPhysicsSchemaVersion = 2;
    static constexpr std::uint32_t kSphereSchemaVersion = 3;
    static constexpr std::uint32_t kCapsuleSchemaVersion = 4;
    static constexpr std::uint32_t kCapsuleRotationSchemaVersion = 5;
    static constexpr std::uint32_t kContactEventsSchemaVersion = 6;
    static constexpr std::uint32_t kTriggerColliderSchemaVersion = 7;
    static constexpr std::uint32_t kCharacterControllerSchemaVersion = 8;
    static constexpr std::uint32_t kHierarchySchemaVersion = 9;
    static constexpr std::uint32_t kCameraSchemaVersion = 10;
    static constexpr std::uint32_t kLightSchemaVersion = 11;
    static constexpr std::uint32_t kEnvironmentNoneSchemaVersion = 12;
    static constexpr std::uint32_t kObjectLightingSchemaVersion = 13;
    // Physical light units (PhotometricUnits.h); earlier versions are converted on load.
    static constexpr std::uint32_t kPhysicalLightingSchemaVersion = 14;
    static constexpr std::uint32_t kScriptSchemaVersion = 15;
    // Audio.2A Audio Source and Audio Listener components.
    static constexpr std::uint32_t kAudioSchemaVersion = 16;
    // Physics and Trigger Collider dimensions and offsets are object-local and follow the object's scale; earlier
    // versions stored absolute meters and are converted on load (ColliderScaleMigration.h).
    static constexpr std::uint32_t kColliderScaleSchemaVersion = 17;
    static constexpr std::uint32_t kAcousticsSchemaVersion = 18;
    // VX.4B: per-object voxel settings and the scene's voxel bake.
    static constexpr std::uint32_t kVoxelSchemaVersion = 19;
    // AIR.3: per-object Air Motors.
    static constexpr std::uint32_t kAirMotorSchemaVersion = 20;
    // AIR.4D D2: an Air Motor's emission origin and emission radius.
    static constexpr std::uint32_t kAirMotorEmissionSchemaVersion = 21;
    // AIR.4D D2.1: Omni and Vortex emission shapes (semi-axes and rotation) replace the emission radius; older motors are migrated
    // on load (MigrateLegacyAirMotorEmission).
    static constexpr std::uint32_t kAirMotorShapeSchemaVersion = 22;
    // AIR.4F F2: per-object Mist Emitters.
    static constexpr std::uint32_t kMistEmitterSchemaVersion = 23;
    // AIR.4F F4.4.1: the scene's mist appearance (mistRendering).
    static constexpr std::uint32_t kMistRenderingSchemaVersion = 24;
    // AIR.4F.2b: a Mist Emitter's mode (Legacy, Point; AIR.4F.2c adds Volume) and its Point fields. Optional: an emitter without
    // them is Legacy. AIR.4F.2c's Volume fields join this version as further optional fields rather than bumping it again.
    static constexpr std::uint32_t kMistEmitterModeSchemaVersion = 25;
    // R1.1: the scene's AIR simulation configuration (airSimulation).
    static constexpr std::uint32_t kAirSimulationSchemaVersion = 26;
    // R1.5: moving-solid participation: airSimulation.movingSolids (optional, false when absent) and each physics component's
    // airParticipation (required from this version; earlier scenes read Automatic).
    static constexpr std::uint32_t kMovingSolidParticipationSchemaVersion = 27;
    static constexpr std::uint32_t kSchemaVersion = kMovingSolidParticipationSchemaVersion;

    std::uint32_t schemaVersion = kSchemaVersion;

    std::string id;
    std::string name = "Untitled Scene";
    // Empty preserves the historical neutral environment; builtin:none explicitly disables it.
    // Otherwise a canonical project texture asset reference.
    static constexpr const char* NoneEnvironmentReference = "builtin:none";
    std::string environmentReference;

    std::vector<SceneObjectDocument> objects;

    SceneCameraDocument camera;
    SceneLightingSettings lightingSettings;

    // VX.4B: the scene's voxel bake settings and current manifest; absent until configured. Persisted from schema 19 and
    // cooked from payload 36 (R1.4a), with the manifest reference unchanged so the archive resolves it by the same asset identity.
    std::optional<VoxelBakeSettings> voxelBake;

    // AIR.4F F4.4.1: the scene's mist appearance; absent until first edited, and then MistRenderSettings' defaults apply. Persisted from
    // schema 24 (sanitized on load) and cooked from payload 35 (R1.3; refused when invalid). The haze's lighting reads it too.
    std::optional<MistRenderSettings> mistRendering;

    // AIR.4F F4.5.1b: the scene's air environment (ambient temperature, thermal relaxation); absent until first edited, and then
    // AirEnvironmentSettings' defaults apply. Persisted in schema 24 files; cooked from payload 34 (R1.1).
    std::optional<AirEnvironmentSettings> airEnvironment;

    // R1.1: the scene's AIR simulation configuration; absent until first edited in a stopped scene, and then AirSimulationSettings'
    // defaults (AIR off) apply. Persisted from schema 26 and cooked from payload 34. Read only through ResolveAirSimulation.
    std::optional<AirSimulationSettings> airSimulation;

    // AIR.4F.2d.3: the scene's environmental haze and near-fog clearance; absent: neither. Persisted in schema 25 files and cooked
    // (payload 31). Rendered by the Editor's fog pass.
    std::optional<EnvironmentalHazeSettings> environmentalHaze;
};

bool ValidateSceneDocument(
    const SceneDocument& document,
    std::string* errorMessage = nullptr);

// R1.1: the AIR simulation configuration a frame runs with, the one path the Engine uses. In order: the Editor's Play or Physics
// Simulation session override, if any (the packaged runtime never has one); the scene's own block; the defaults. `document` is null
// when no scene document is active.
AirSimulationSettings ResolveAirSimulation(const SceneDocument* document, const std::optional<AirSimulationSettings>& sessionOverride);

// Shared authoritative component/coexistence validation for proposed object values.
bool ValidateSceneObjectComponents(const SceneObjectDocument& object, std::string* errorMessage = nullptr);
// Shared output admission; all current codecs preserve the complete authored document.
bool ValidateSceneDocumentPersistence(const SceneDocument& document, std::string* errorMessage = nullptr);
