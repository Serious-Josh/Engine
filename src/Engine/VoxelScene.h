#pragma once

// VX.4B: from an Engine scene to a published voxel bake. See Documentation/Voxel/VoxelArchitecture.md, sections 4 and 6.7.
//
// Main thread: ResolveParticipation decides which objects are static voxel geometry; Capture copies their world-space triangles
// into an immutable GeometrySnapshot, so nothing the scene does afterwards can change what is baked. Worker thread: SceneBakeJob
// builds the acceleration and prepares the world bake (VX.4A), staging assets. Main thread again: Finish publishes through the
// asset store, and the caller switches the scene's manifest reference (ApplyBakeResult), which is what makes the bake current.

#include "Core/BackgroundTasks.h"
#include "Math/Matrix4.h"
#include "Scene/SceneDocument.h"
#include "Voxel/VoxelAssetStore.h"
#include "Voxel/VoxelBake.h"
#include "Voxel/VoxelGeometry.h"
#include "Voxel/VoxelWorldBake.h"

#include <atomic>
#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct ImportedMesh;

namespace VoxelScene
{
enum class ParticipationReason
{
    // Participates: a Static physics body, or explicitly Static without physics.
    StaticBody,
    ExplicitStatic,
    // Does not participate.
    NoMesh,
    Excluded,
    DynamicBody,
    KinematicBody,
    CharacterController,
    // Automatic without physics: only explicit Static includes it.
    NotStatic
};

struct ParticipationDecision
{
    bool participates = false;
    ParticipationReason reason = ParticipationReason::NotStatic;
    // Explicit Static on a moving body: ignored, reported.
    bool warning = false;
    Voxel::ObjectOrientation orientation = Voxel::ObjectOrientation::Automatic;
};

// The only place voxel baking reads Physics: participation policy of VoxelArchitecture.md 4.1.
ParticipationDecision ResolveParticipation(const SceneObjectDocument& object);

const char* ParticipationReasonToString(ParticipationReason reason) noexcept;

// How capture reads the live scene. Borrowed for the Capture call only.
struct SceneAccess
{
    // The object's authored world matrix (hierarchy applied), or nullopt when it has none.
    std::function<std::optional<Matrix4>(const SceneObjectDocument&)> world;
    // The object's CPU mesh, or null when it has none loaded.
    std::function<const ImportedMesh*(const SceneObjectDocument&)> mesh;
};

// The scene's bake settings as the voxel module takes them; defaults when the scene has none.
Voxel::BakeSettings BakeSettingsFor(const SceneDocument& document);

struct CaptureReport
{
    std::size_t objects = 0;
    std::size_t participating = 0;
    std::size_t triangles = 0;
    std::size_t mirrored = 0;
    // Objects whose mesh and world matrix exactly repeat an earlier participant's: captured once.
    std::size_t duplicateInstances = 0;
    std::map<ParticipationReason, std::size_t> byReason;
    std::vector<std::string> warnings;
};

struct CapturedScene
{
    std::shared_ptr<const Voxel::GeometrySnapshot> snapshot;
    Voxel::BakeSettings settings;
    // The scene's manifest when captured; reuse reads it.
    std::string currentManifest;
    CaptureReport report;
};

// Copies every participating object's triangles into world space (double precision, full world matrix; a mirroring matrix
// reverses winding so solids stay solid), in ascending object-ID order, so triangle order and identity do not depend on document
// order. Fails, naming every offending object, for a missing world matrix or mesh, non-finite matrices or positions, or bad indices.
std::expected<CapturedScene, std::string> Capture(const SceneDocument& document, const SceneAccess& access, const Voxel::SnapshotOptions& options = {});

struct BakeJobOptions
{
    unsigned threads = 1;
    std::size_t memoryBudget = std::size_t{1} << 30;
    std::size_t maximumRegions = std::size_t{1} << 20;
    bool reuse = true;
};

struct BakeOutcome
{
    std::string manifestReference;
    Voxel::BakeSettings settings;
    Voxel::WorldBakeStatistics statistics;
    // Hash of the voxel content alone (keys, kinds, input hashes and region bytes), independent of asset references.
    std::uint64_t contentHash = 0;
};

// One scene bake on a background thread. Construct, Poll from the main thread until finished, then Finish exactly once on the main
// thread. Cancel stops it at the next region; destroying an unfinished job cancels and joins it, and nothing it staged survives.
class SceneBakeJob
{
public:
    SceneBakeJob(CapturedScene scene, std::shared_ptr<Voxel::AssetStore> store, BakeJobOptions options, BackgroundTaskManager* tasks = nullptr);
    ~SceneBakeJob();
    SceneBakeJob(const SceneBakeJob&) = delete;
    SceneBakeJob& operator=(const SceneBakeJob&) = delete;

    void Cancel() noexcept;
    bool IsFinished() const noexcept;

    // Regions finished and planned (zero until planning ends). Also forwards them to the background task, when there is one.
    std::pair<std::size_t, std::size_t> Poll();

    // Publishes (main thread) and returns the outcome; or why nothing was published. Waits for the worker if it is still running.
    std::expected<BakeOutcome, Voxel::WorldBakeError> Finish();

private:
    void Run();

    CapturedScene m_scene;
    std::shared_ptr<Voxel::AssetStore> m_store;
    BakeJobOptions m_options;
    BackgroundTaskHandle m_task;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_finished{false};
    std::atomic<std::size_t> m_total{0};
    std::atomic<std::size_t> m_done{0};
    std::optional<std::expected<Voxel::PreparedWorldBake, Voxel::WorldBakeError>> m_prepared;
    bool m_consumed = false;
    std::thread m_thread;
};

// Records a published bake on the scene: its settings and manifest. The caller decides how the edit is applied (history, save).
void ApplyBakeResult(SceneDocument& document, const BakeOutcome& outcome);

// The scene's voxel settings with `manifest` as its current manifest.
VoxelBakeSettings BakeSettingsWithManifest(const Voxel::BakeSettings& settings, const std::string& manifest);

enum class BakeState
{
    NotBaked,
    // The manifest matches the captured scene: same settings, and every region's inputs unchanged.
    UpToDate,
    // Settings match but geometry or participation changed: a rebake would change `changedRegions` regions.
    Stale,
    // The scene's bake settings differ from the manifest's.
    SettingsChanged,
    // The manifest or one of its regions is not registered or not readable.
    Missing,
    // The manifest or one of its regions does not decode, or records something else.
    Corrupt
};

const char* BakeStateToString(BakeState state) noexcept;

struct BakeStatus
{
    BakeState state = BakeState::NotBaked;
    std::string manifestReference;
    // Voxel::ManifestContentHash of the published bake, when its manifest and every region read and decode.
    std::optional<std::uint64_t> contentHash;
    std::size_t populated = 0;
    std::size_t uniformInside = 0;
    std::size_t changedRegions = 0;
    std::string detail;
};

// Reads the scene's current manifest and every region it references, validating them, recomputes the content hash, and plans the
// captured scene to compare input hashes. Safe on a worker thread with a store whose Read is; stops early when `cancel` is set.
BakeStatus EvaluateBakeStatus(const CapturedScene& scene, const Voxel::AssetStore& store, const std::atomic<bool>* cancel = nullptr);

// EvaluateBakeStatus on a background thread. Destroying it cancels and joins.
class BakeStatusCheck
{
public:
    BakeStatusCheck(CapturedScene scene, std::shared_ptr<const Voxel::AssetStore> store);
    ~BakeStatusCheck();
    BakeStatusCheck(const BakeStatusCheck&) = delete;
    BakeStatusCheck& operator=(const BakeStatusCheck&) = delete;

    bool IsFinished() const noexcept;
    // Waits if still running.
    BakeStatus Take();

private:
    CapturedScene m_scene;
    std::shared_ptr<const Voxel::AssetStore> m_store;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_finished{false};
    BakeStatus m_status;
    std::thread m_thread;
};
}
