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
#include "Scene/PhysicalSkySettings.h"
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

// M3.5: the most objects one scene (or one composite World, its level nodes included) may hold. Every object counts: Lights,
// meshes, Cameras and empties alike. Admission (document validation, the hierarchy, composite Worlds, cooked scenes and the
// renderer's membership candidates) enforces exactly this bound; storage is sized to the actual count, never to the bound.
inline constexpr std::size_t MaximumSceneObjects = 65536;

// M3.5: the largest scene source file (JSON) any reader admits: authoring loads, the document cache, the project scan, asset
// transactions and the cooker. A bound on untrusted input, checked against the file size before reading; nothing allocates it.
inline constexpr std::uintmax_t MaximumSceneSourceBytes = 1ull << 30;

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

    // Runtime only, never serialized: in a composite World (W4/W5) this Local Level root entity is parented to its level's
    // synthetic placement node, which stands in for "no parent". The root-object rule for Physics, Trigger Collider and Character
    // Controller treats it as a root. Always false in a scene document read from or written to disk.
    bool parentIsLevelNode = false;
};

// W1.1: a .scene document is either a World or a Local Level. Historical documents (schema < 28) are Local Levels.
enum class SceneKind : std::uint8_t
{
    Local,
    World
};

// W2: where a World places one of its Local Levels: translation and full 3D rotation (Euler radians, the Transform convention).
// There is no scale. Entity transforms inside the Local Level stay in the level's own frame.
struct ScenePlacement
{
    Vector3 position{0.0f, 0.0f, 0.0f};
    Vector3 rotation{0.0f, 0.0f, 0.0f};

    bool operator==(const ScenePlacement& other) const
    {
        return position.x == other.position.x && position.y == other.position.y && position.z == other.position.z &&
            rotation.x == other.rotation.x && rotation.y == other.rotation.y && rotation.z == other.rotation.z;
    }
};

// W2: one Local Level of a World, by the Local Level's asset ID (its document id, a canonical GUID).
struct SceneMembership
{
    std::string level;
    ScenePlacement placement;

    bool operator==(const SceneMembership&) const = default;
};

// W3: a Local Level's environment override switches, one per group. An enabled group uses the Local Level's own stored values; a
// disabled group inherits its World's values, or the Engine scene defaults when the Local Level has no (valid) World. Stored values
// are kept whichever way a switch is set. Worlds have none. Documents from before schema 30 (and documents built in code) have every
// group enabled, which is how Local Levels behaved before W3; new Local Levels start with every group disabled
// (SceneEnvironmentOverrides::None()).
struct SceneEnvironmentOverrides
{
    // skyType, physicalSky, environmentReference and lightingSettings.environmentLuminanceScale.
    bool sky = true;
    // lightingSettings.referenceEV100.
    bool exposure = true;
    // environmentalHaze.
    bool haze = true;
    // mistRendering.
    bool mist = true;
    // airEnvironment.
    bool airEnvironment = true;
    // airSimulation.ambientVelocity only; the rest of the AIR simulation configuration is World-owned.
    bool wind = true;

    static SceneEnvironmentOverrides None() { return {false, false, false, false, false, false}; }
    bool operator==(const SceneEnvironmentOverrides&) const = default;
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
    // W1.1: the explicit scene kind (required) and a canonical GUID id (required when written). Older documents load as Local
    // Levels with their id unchanged, and are written again only after migration.
    static constexpr std::uint32_t kSceneKindSchemaVersion = 28;
    // W2: World/Local membership. A Local Level states its owning World ("owningWorld": GUID or null, required); a World lists
    // its Local Levels with their placements ("members", required). Earlier documents are unassigned Local Levels (or, schema
    // 28 only, Worlds without members, which cannot be written).
    static constexpr std::uint32_t kMembershipSchemaVersion = 29;
    // W3: a Local Level's environment override switches ("environmentOverrides", required; Worlds have none). Earlier Local Levels
    // read with every switch enabled.
    static constexpr std::uint32_t kEnvironmentOverridesSchemaVersion = 30;
    // Physical sky S0: the optional "sky" block (type, and the physical sky's settings). Earlier documents, and later ones without
    // the block, derive the sky type from environmentReference (DeriveSkyType).
    static constexpr std::uint32_t kPhysicalSkySchemaVersion = 31;
    // Physical sky S2: a Directional Light's "affectsAtmosphere" (written only when false). Earlier documents read true.
    static constexpr std::uint32_t kLightAtmosphereSchemaVersion = 32;
    // Physical sky S4: lightingSettings' "environmentRotationDegrees", the HDRI sky's yaw (written only when not 0). Earlier
    // documents read 0.
    static constexpr std::uint32_t kHdriRotationSchemaVersion = 33;
    // Physical sky S5: the physical sky's "night" and "clouds" (each written only when not the default). Earlier documents read
    // the defaults (no clouds).
    static constexpr std::uint32_t kCloudSchemaVersion = 34;
    static constexpr std::uint32_t kSchemaVersion = kCloudSchemaVersion;

    std::uint32_t schemaVersion = kSchemaVersion;

    SceneKind kind = SceneKind::Local;

    // Local Levels only: the owning World's asset ID, or empty when unassigned. Changed only by ProjectSceneMembership.
    std::string owningWorld;
    // Worlds only: the Local Levels, in authored order. Changed only by ProjectSceneMembership.
    std::vector<SceneMembership> members;
    // Local Levels only (W3): which environment groups use this document's own values.
    SceneEnvironmentOverrides environmentOverrides;

    // In memory, any non-empty id (historical documents keep theirs until migrated); written documents require a canonical GUID.
    std::string id;
    std::string name = "Untitled Scene";
    // Empty preserves the historical neutral environment; builtin:none explicitly disables it.
    // Otherwise a canonical project texture asset reference.
    static constexpr const char* NoneEnvironmentReference = "builtin:none";
    std::string environmentReference;

    // Physical sky S0: which sky lights the scene, in the Sky environment group with environmentReference (SceneDocumentSkyType).
    // Absent until a sky type is chosen: the type is then derived from environmentReference, exactly as every earlier scene
    // rendered, so code that only sets the reference keeps its meaning. Hdri uses environmentReference as before; Neutral and
    // Physical keep it stored (the HDRI identity) but do not use it.
    std::optional<SkyType> skyType;
    // The physical sky's settings; absent until first edited, and then PhysicalSkySettings' defaults (Earth) apply. Persisted from
    // schema 31 and cooked from payload 38.
    std::optional<PhysicalSkySettings> physicalSky;

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

// W1.1: World documents serialize and validate, but nothing opens them until World loading exists (W4). Every path that opens a
// scene as an ordinary (Local) scene calls this first, so a World is refused explicitly instead of being loaded as a Local Level.
bool ValidateSceneKindOpenable(const SceneDocument& document, std::string* errorMessage = nullptr);

// Physical sky S0: the document's sky type, its stored one or the one its environment reference derives (ResolveSkyType).
SkyType SceneDocumentSkyType(const SceneDocument& document);

// Physical sky S0: whether the scene's sky reads a project HDRI asset (an Hdri sky with an asset reference). Only such a scene
// needs an environment resource when cooked or packaged; Neutral and Physical skies, empty references and builtin:none need none.
bool SceneSkyUsesEnvironmentAsset(SkyType skyType, const std::string& environmentReference);
bool SceneSkyUsesEnvironmentAsset(const SceneDocument& document);

// R1.1: the AIR simulation configuration a frame runs with, the one path the Engine uses. In order: the Editor's Play or Physics
// Simulation session override, if any (the packaged runtime never has one); the scene's own block; the defaults. `document` is null
// when no scene document is active.
AirSimulationSettings ResolveAirSimulation(const SceneDocument* document, const std::optional<AirSimulationSettings>& sessionOverride);

// Shared authoritative component/coexistence validation for proposed object values.
bool ValidateSceneObjectComponents(const SceneObjectDocument& object, std::string* errorMessage = nullptr);
// Shared output admission; all current codecs preserve the complete authored document.
bool ValidateSceneDocumentPersistence(const SceneDocument& document, std::string* errorMessage = nullptr);
// W1.1: admission for writing a .scene file (schema 28). Beyond ValidateSceneDocumentPersistence, the id must be a canonical GUID,
// so a historical document with a legacy id cannot be written until it is migrated. Runtime preparation and cooking keep using
// ValidateSceneDocumentPersistence, so legacy ids still load and run.
bool ValidateSceneDocumentWrite(const SceneDocument& document, std::string* errorMessage = nullptr);
