#pragma once

// Private to the audio module: the Steam Audio simulation sources of the running FMOD system. Include only from the audio
// module's .cpp files; it brings in FMOD and Steam Audio headers. Full contract: SteamAudioIntegration.md, sections 16 and 18.
//
// One source per Steam Audio Spatializer DSP, for that DSP's lifetime, bound exactly once in PLUGIN_CREATED on FMOD's Studio
// thread. The mixer was measured not to process a DSP while that callback runs (section 17.3; undocumented). The plugin's
// short-lived handle is removed before the bind returns, and nothing ever rebinds. Events with no spatializer have no source.
//
// Two synchronizations, chosen once at Start, before any bank can create a DSP, and never mixed:
//
// * Snapshot (Option B, the default). The plugin holds a SnapshotSource and reads wait-free copies of finished results. The
//   simulator's own sources are private and unreachable by FMOD, so simulation can take any length of time.
// * DspLock (Option A, the fallback when CheckSnapshotCompatibility refuses the binaries). The plugin holds the real source,
//   and the main thread runs RunDirect under System::lockDSP, which excludes the plugin's reads.
//
// Thread ownership under Snapshot:
//
// * Main thread: FMOD. Applies lifecycle changes, reads poses, hands the worker immutable frames, destroys SnapshotSources.
// * Studio thread: PLUGIN_CREATED and PLUGIN_DESTROYED. Creates and binds SnapshotSources, and reports to the main thread.
// * Worker: between thread start and join it alone calls the simulator and private sources, and writes the channels. It
//   never calls FMOD.
// * Mixer: SnapshotSource::getOutputs only. It takes no lock and never waits for anyone.
//
// M4 acoustic scene (Snapshot only; never under the DSP lock): the worker owns the Steam Audio root scene, one sub-scene per
// distinct AcousticMeshData, and one instance per object. The main thread hands it immutable geometry by shared pointer. Every
// scene change and commit runs on the worker between simulations, so none can overlap RunDirect, however long a build takes.
//
// m_mutex guards handoffs only. It is never held across FMOD, simulation or join; under DspLock the order is lockDSP, then
// m_mutex, because the Studio thread may create real sources.

#include "Audio/AudioSystem.h"
#include "Audio/SteamAudioRuntime.h"

#if defined(ENGINE_HAS_STEAMAUDIO)
#include <fmod_studio.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#endif

namespace Audio::Internal
{
#if defined(ENGINE_HAS_STEAMAUDIO)
class SnapshotSource;

class SteamAudioSimulation final
{
public:
    SteamAudioSimulation(const SteamAudioRuntime& runtime, FMOD::Studio::System& studio, FMOD::System& core) noexcept;
    ~SteamAudioSimulation();
    SteamAudioSimulation(const SteamAudioSimulation&) = delete;
    SteamAudioSimulation& operator=(const SteamAudioSimulation&) = delete;

    // Creates the simulator, then selects the synchronization. On simulator failure no source is ever created and
    // spatializers run as direct HRTF only. Before any bank is loaded.
    void Start(const SteamAudioOptions& options);

    // Only after the Studio system is released (no callback remains). Joins the worker. Idempotent.
    void Shutdown() noexcept;

    // Main thread, before the instance starts: its Steam Audio DSPs will be bound as FMOD creates them.
    void AttachToInstance(FMOD::Studio::EventInstance& instance);

    // Main thread, once per audio frame: applies lifecycle changes, and hands over or runs a simulation when one is due.
    void Update() noexcept;

    SteamAudioSimulationStats GetStats() const;

    // Verification: an immediate tick that returns after its results are published.
    void RunNow() noexcept;
    std::vector<SteamAudioSourceSnapshot> GetSnapshots() const;
    void SetTestControls(const SteamAudioSimulationTestControls& controls);
    void* GetWorkerThreadForTesting() noexcept;

    bool IsAcousticSimulationAvailable() const noexcept
    {
        return m_synchronization == SteamAudioSynchronization::Snapshot;
    }

    bool SetAcousticObject(std::uint64_t id, std::shared_ptr<const AcousticMeshData> mesh, const AcousticTransform& world);
    bool SetAcousticObjectTransform(std::uint64_t id, const AcousticTransform& world);
    bool RemoveAcousticObject(std::uint64_t id);
    bool ClearAcousticScene();

private:
    using SourceId = std::uint64_t;

    // What the Studio thread reports to the main thread, in order: FMOD may reuse a DSP's address.
    struct LifecycleChange
    {
        enum class Kind
        {
            Created,
            Destroyed,
            // A SnapshotSource whose bind failed: never in the simulator, only waiting to be destroyed.
            BindFailed
        };

        Kind kind = Kind::Created;
        FMOD::DSP* dsp = nullptr;
        FMOD::Studio::EventInstance* instance = nullptr;
        SnapshotSource* snapshot = nullptr;
        // DspLock only: the real source the DSP is bound to.
        IPLSource realSource = nullptr;
    };

    // Main-thread record of a bound source.
    struct ActiveSource
    {
        FMOD::Studio::EventInstance* instance = nullptr;
        SourceId id = 0;
        SnapshotSource* snapshot = nullptr;
        IPLSource realSource = nullptr;
        IPLCoordinateSpace3 pose{};
    };

    // Main-thread record of a SnapshotSource on its way out: destroyed when the worker has let go and the plugin has.
    struct RetiringSource
    {
        SnapshotSource* snapshot = nullptr;
        SourceId id = 0;
        bool workerDone = false;
    };

    // Worker's request to change its sources or scene.
    struct WorkerCommand
    {
        enum class Kind
        {
            AddSource,
            RemoveSource,
            SetScene,
            SetAcousticObject,
            RemoveAcousticObject,
            ClearAcousticScene
        };

        Kind kind = Kind::AddSource;
        SourceId id = 0;
        SnapshotSource* snapshot = nullptr;
        void* scene = nullptr;
        std::shared_ptr<const AcousticMeshData> mesh;
        AcousticTransform world{};
    };

    // Worker-owned: one Steam Audio sub-scene per distinct geometry, shared by its instances.
    struct AcousticSubScene
    {
        std::shared_ptr<const AcousticMeshData> mesh;
        IPLScene scene = nullptr;
        IPLStaticMesh staticMesh = nullptr;
        int references = 0;
    };

    struct AcousticObject
    {
        const AcousticMeshData* mesh = nullptr;
        IPLInstancedMesh instance = nullptr;
        AcousticTransform world{};
    };

    struct FramePose
    {
        SourceId id = 0;
        IPLCoordinateSpace3 pose{};
    };

    // One immutable simulation request. A newer frame replaces an unconsumed one: a slow simulation never builds a backlog.
    struct InputFrame
    {
        bool valid = false;
        std::uint64_t ticket = 0;
        IPLCoordinateSpace3 listener{};
        std::vector<FramePose> poses;
        SteamAudioSimulationTestControls controls;
    };

    // Worker-owned, between thread start and join.
    struct PrivateSource
    {
        SnapshotSource* snapshot = nullptr;
        // Null when creation failed: the snapshot then stays at its last, neutral value.
        IPLSource realSource = nullptr;
        IPLCoordinateSpace3 pose{};
    };

    struct PublishedRecord
    {
        SteamAudioDirectSnapshot snapshot;
        IPLCoordinateSpace3 pose{};
    };

    static FMOD_RESULT F_CALL OnEventCallback(FMOD_STUDIO_EVENT_CALLBACK_TYPE type, FMOD_STUDIO_EVENTINSTANCE* event,
        void* parameters);
    void OnPluginCreated(FMOD::Studio::EventInstance& instance, FMOD::DSP& dsp);
    void OnPluginDestroyed(FMOD::DSP& dsp);
    bool ConsumeInjectedFailure(SteamAudioInjectedFailure failure);
    void BindSnapshotSource(FMOD::Studio::EventInstance& instance, FMOD::DSP& dsp);
    void BindRealSource(FMOD::Studio::EventInstance& instance, FMOD::DSP& dsp);

    void ApplyLifecycleChanges() noexcept;
    void ReclaimRetiredSources() noexcept;
    bool ReadListener(IPLCoordinateSpace3& listener) noexcept;
    void PostFrame(std::uint64_t ticket) noexcept;
    void SimulateUnderLock() noexcept;

    void WorkerMain() noexcept;
    void ApplyWorkerCommands(const std::vector<WorkerCommand>& commands, std::vector<SourceId>& removed) noexcept;
    void SimulateAndPublish(const InputFrame& frame) noexcept;
    // Chooses the session's ray tracer, once, before the simulator exists. Any failure leaves the default one.
    void SelectRayTracer(const SteamAudioOptions& options, bool geometryAllowed);
    void ReleaseEmbree() noexcept;
    void RebuildEmbreeRoot() noexcept;
    void ReleaseRetiredEmbreeInstances() noexcept;
    IPLSceneSettings MakeSceneSettings() const noexcept;
    bool EnsureRootAcousticScene() noexcept;
    AcousticSubScene* AcquireSubScene(const std::shared_ptr<const AcousticMeshData>& mesh) noexcept;
    void ReleaseSubScene(const AcousticMeshData* mesh) noexcept;
    bool WorkerSetAcousticObject(SourceId id, const std::shared_ptr<const AcousticMeshData>& mesh, const AcousticTransform& world) noexcept;
    bool WorkerRemoveAcousticObject(SourceId id) noexcept;
    void WorkerApplyTransforms(const std::unordered_map<SourceId, AcousticTransform>& transforms, bool& changed) noexcept;
    void CommitAcousticScene() noexcept;
    void ReleaseAcousticScene() noexcept;

    void ShutdownSnapshotPath() noexcept;
    void ShutdownLockPath() noexcept;

    const SteamAudioRuntime& m_runtime;
    FMOD::Studio::System& m_studio;
    FMOD::System& m_core;
    IPLSimulator m_simulator = nullptr;
    // Set by Start before the worker exists, then read-only until Shutdown. Null with the default ray tracer.
    IPLEmbreeDevice m_embreeDevice = nullptr;
    SteamAudioSynchronization m_synchronization = SteamAudioSynchronization::None;

    // Guards the members up to m_diagnosticsWanted: handoffs between the Studio, main and worker threads.
    mutable std::mutex m_mutex;
    std::unordered_set<FMOD::DSP*> m_boundDsps;
    std::vector<LifecycleChange> m_lifecycleChanges;
    SteamAudioInjectedFailure m_injectedFailure = SteamAudioInjectedFailure::None;
    int m_injectedFailuresRemaining = 0;
    SteamAudioSimulationStats m_stats;
    std::condition_variable m_workerWake;
    std::condition_variable m_workerDone;
    bool m_workerStop = false;
    InputFrame m_pendingFrame;
    std::vector<WorkerCommand> m_pendingCommands;
    std::vector<SourceId> m_workerRemoved;
    std::unordered_map<SourceId, AcousticTransform> m_pendingAcousticTransforms;
    std::uint64_t m_completedTicket = 0;
    std::unordered_map<SourceId, PublishedRecord> m_published;
    bool m_diagnosticsWanted = false;

    // Main thread only.
    std::unordered_map<FMOD::DSP*, ActiveSource> m_activeSources;
    std::vector<RetiringSource> m_retiringSources;
    SteamAudioSimulationTestControls m_testControls;
    std::chrono::steady_clock::time_point m_lastSimulation{};
    SourceId m_nextSourceId = 1;
    std::uint64_t m_nextTicket = 1;
    bool m_failuresReported = false;
    std::vector<FramePose> m_framePoses;
    std::unordered_set<SourceId> m_acousticObjectIds;
    std::thread m_worker;

    // Worker only, between thread start and join.
    std::unordered_map<SourceId, PrivateSource> m_privateSources;
    std::uint32_t m_generation = 0;
    IPLScene m_acousticRoot = nullptr;
    // Worker: removed instances, held until the root that contains them is gone (see RebuildEmbreeRoot).
    std::vector<IPLInstancedMesh> m_retiredEmbreeInstances;
    std::unordered_map<const AcousticMeshData*, AcousticSubScene> m_acousticSubScenes;
    std::unordered_map<SourceId, AcousticObject> m_acousticObjects;
};
#endif
}
