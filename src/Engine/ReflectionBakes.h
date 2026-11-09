#pragma once

#include "Audio/AudioSystem.h"
#include "Engine/AcousticGeometry.h"
#include "Engine/ProbeVolumes.h"
#include "Scene/SceneDocument.h"
#include "assets/BakedReflectionsAsset.h"
#include "assets/ProbeBatchAsset.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// M8.2: whether a Probe Volume's baked reflections still describe the scene, and what a bake would have to do about it.
// Pure: it bakes nothing, reads no files and touches no Steam Audio object. Validity comes from fingerprints of the inputs,
// never from baked bytes, which M8.1 found are not reproducible.
//
// Two separate questions, never mixed:
// * ProbeVolumes::Fingerprint: do the probes still match the volume and, for floor placement, the geometry? (M7)
// * Here: do the baked layers still match the probes, the acoustic scene, the settings and each static source?
//
// The acoustic scene (every acoustics-enabled object's geometry, transform and resolved materials) is one input to every layer
// of every volume: reflections reach beyond a volume's box, so any acoustic change invalidates all of them.
namespace ReflectionBakes
{
// Tied to the simulator (see BakedReflectionsAssets::SimulatorOrder).
inline constexpr std::uint32_t BakeOrder = BakedReflectionsAssets::SimulatorOrder;
inline constexpr float BakeDuration = BakedReflectionsAssets::SimulatorDuration;

// Steam Audio space. Large enough to hold every probe of every volume, so a static source's layer has data at all of them:
// probes outside the sphere still take interpolation weight and darken the result (M8.1).
inline constexpr float StaticSourceInfluenceRadius = 1.0e6f;

// How far a static source may be from where it was baked, in metres, before its layer is stale.
inline constexpr float StaticSourceTolerance = 0.01f;

struct AcousticObject
{
    std::shared_ptr<const Audio::AcousticMeshData> mesh;
    Audio::AcousticTransform world{};
};

// Order-independent, so reordering objects in the document changes nothing. Object ids and names are not part of it: two scenes
// with the same acoustic content share their bakes.
struct AcousticSceneHashes
{
    // Vertex positions, triangles and world transforms.
    std::uint64_t geometry = 0;
    // Which material each triangle uses, and those materials' resolved values.
    std::uint64_t materials = 0;

    bool operator==(const AcousticSceneHashes&) const = default;
};

AcousticSceneHashes HashAcousticScene(std::span<const AcousticObject> objects);

// The bake inputs as the baker receives them, from a volume's settings plus the simulator's order and duration.
BakedReflectionsAssets::BakeParameters ParametersFor(const ReflectionBakeSettings& settings) noexcept;

// Everything that changes the baked result. Thread count does not, and is not here.
std::uint64_t HashParameters(const BakedReflectionsAssets::BakeParameters& parameters) noexcept;

// The probe asset's fingerprint and positions, together.
std::uint64_t HashProbes(const ProbeBatchAssets::Data& probes) noexcept;

// An enabled Audio Source asking for baked source-centric reflections, at its current position.
struct StaticSource
{
    std::string objectId;
    // Steam Audio space, as the identifier stores it.
    std::array<float, 3> position{};
};

// Sorted by object id. A source whose world transform cannot be read is left out, and listed in `unplaced`.
std::vector<StaticSource> CollectStaticSources(const SceneDocument& document, const ProbeVolumes::SceneAccess& scene,
    std::vector<std::string>* unplaced = nullptr);

// Every acoustics-enabled object the scene can build, as GameRuntime publishes it. Objects that cannot contribute are listed in
// `skipped` and left out, so the fingerprint follows what the simulator would actually get. Main thread: reads meshes.
std::vector<AcousticObject> CollectAcousticScene(const SceneDocument& document, const ProbeVolumes::SceneAccess& scene,
    const AcousticGeometry::MaterialResolver& resolveMaterial, std::vector<std::string>* skipped = nullptr);

// One layer a volume needs: its identifier, exactly as it would be baked, and what it would be baked from.
struct RequiredLayer
{
    BakedReflectionsAssets::LayerKind kind = BakedReflectionsAssets::LayerKind::Reverb;
    std::string sourceObjectId;
    std::array<float, 3> centre{};
    float radius = 0.0f;
    std::uint64_t probesHash = 0;
    AcousticSceneHashes scene;
    std::uint64_t settingsHash = 0;
    std::uint64_t fingerprint = 0;
};

// Combines a layer's identifier, its inputs and the format and SDK versions. Recomputed from a stored layer's own fields to check it.
std::uint64_t LayerFingerprint(BakedReflectionsAssets::LayerKind kind, const std::array<float, 3>& centre, float radius,
    const std::string& sourceObjectId, std::uint64_t probesHash, const AcousticSceneHashes& scene, std::uint64_t settingsHash) noexcept;

// Fills the stored-layer fields a bake must record, so the asset matches what Evaluate will later require.
BakedReflectionsAssets::Layer ToStoredLayer(const RequiredLayer& required, std::uint64_t dataBytes);

// Most important first: the first that applies is reported.
enum class StaleReason
{
    None,
    // The volume has no current probes: generate them before baking.
    ProbesNotReady,
    ProbeLayoutChanged,
    AcousticGeometryChanged,
    AcousticMaterialChanged,
    BakeSettingsChanged,
    StaticSourceMoved,
    // A layer required now that the asset does not hold.
    LayerMissing,
    AssetMissing,
    UnsupportedAsset,
    CorruptAsset
};

const char* ToString(StaleReason reason) noexcept;

enum class LayerState
{
    Valid,
    // Never baked.
    Missing,
    // Baked from inputs that have changed.
    Stale
};

struct LayerStatus
{
    RequiredLayer required;
    LayerState state = LayerState::Missing;
    StaleReason reason = StaleReason::None;
    // Index into the asset's layers, when it holds one for this requirement.
    std::optional<std::size_t> existing;
};

// Persistent validity only. A bake in progress or a failed attempt is transient job state; see Present.
enum class VolumeState
{
    // Disabled: nothing is required of it.
    NotRequired,
    Valid,
    // Some required layers are valid, others missing or stale.
    Partial,
    // It was baked, and nothing baked is valid any more.
    Stale,
    // Never baked. Not a warning.
    Missing
};

const char* ToString(VolumeState state) noexcept;

// What one volume's evaluation sees. Pointers are borrowed for the call only.
struct VolumeContext
{
    const ProbeVolumeComponent* volume = nullptr;
    // The volume's current probes, when they decode and ProbeVolumes reports them Valid; otherwise null.
    const ProbeBatchAssets::Data* probes = nullptr;
    AcousticSceneHashes scene;
    std::span<const StaticSource> sources;
    // The referenced asset as resolved, or null when the volume references none.
    const BakedReflectionsAssets::Resolution* baked = nullptr;
};

struct VolumeStatus
{
    VolumeState state = VolumeState::Missing;
    // Why the asset as a whole is unusable, when it is.
    StaleReason assetProblem = StaleReason::None;
    bool probesReady = false;
    std::vector<LayerStatus> layers;
    // Asset layers for sources that no longer ask for baked reflections. They do not affect validity.
    std::vector<std::size_t> unused;
};

// The layers a volume requires: one listener-centric layer, and one per static source in the scene (every volume bakes every static
// source, so that every registered batch holds every layer a source may request; M8.1).
std::vector<RequiredLayer> RequiredLayers(const VolumeContext& context);

VolumeStatus Evaluate(const VolumeContext& context);

// Transient job state, kept outside the asset: a failed attempt never changes what was last published.
enum class JobState
{
    None,
    Baking,
    Failed
};

enum class DisplayState
{
    NotRequired,
    Valid,
    Partial,
    Stale,
    Missing,
    Baking,
    Failed
};

DisplayState Present(const VolumeStatus& status, JobState job) noexcept;

// Whether the Scene Hierarchy shows the stale warning: there is baked data, and it no longer fully matches.
bool ShowsStaleWarning(const VolumeStatus& status) noexcept;

enum class LayerAction
{
    Reuse,
    // Missing: bake it for the first time.
    Add,
    // Stale, or forced: bake it again.
    Rebake
};

struct LayerPlan
{
    RequiredLayer layer;
    LayerAction action = LayerAction::Reuse;
    StaleReason reason = StaleReason::None;
    std::optional<std::size_t> existing;
};

struct VolumePlan
{
    // Nothing to bake and nothing to remove.
    bool skip = true;
    // The probes must be regenerated first; no layer can be baked against them.
    bool needsProbeGeneration = false;
    std::vector<LayerPlan> layers;
    // Existing layers to leave out of the new asset.
    std::vector<std::size_t> remove;

    std::size_t CountActions(LayerAction action) const noexcept;
};

// The work an incremental bake of this volume needs. Valid layers are reused unless `forceRebake`.
VolumePlan Plan(const VolumeStatus& status, bool forceRebake = false);
}
