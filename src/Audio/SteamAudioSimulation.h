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
// * Main thread: FMOD. Applies lifecycle changes, reads poses, hands the worker immutable frames, destroys SnapshotSources
//   and releases the private sources the worker has handed back.
// * Studio thread: PLUGIN_CREATED and PLUGIN_DESTROYED. Creates and binds SnapshotSources, and reports to the main thread.
// * Worker: between thread start and join it alone calls the simulator and the private sources it holds, and writes the
//   channels. A private source leaves it only after being removed and committed out. It never calls FMOD.
// * Mixer: SnapshotSource::getOutputs only. It takes no lock and never waits for anyone.
//
// M4 acoustic scene (Snapshot only; never under the DSP lock): the worker owns the Steam Audio root scene, one sub-scene per
// distinct AcousticMeshData, and one instance per object. The main thread hands it immutable geometry by shared pointer. Every
// scene change and commit runs on the worker between simulations, so none can overlap RunDirect, however long a build takes.
//
// m_mutex guards handoffs only. It is never held across FMOD, simulation or join; under DspLock the order is lockDSP, then
// m_mutex, because the Studio thread may create real sources.
//
// M7 probe batches are not the simulator's: AudioSystem holds them, and only M8 may hand them to iplSimulatorAddProbeBatch.
//
// M6.4 reflection infrastructure (Snapshot only, verification sessions only; off in production). A second worker runs
// iplSimulatorRunReflections on its own cadence and never publishes to the plugin. m_simulatorGate is what makes that safe:
// the direct worker holds it exclusively around every commit and every scene or source change, and the reflection worker
// shares it across its run. RunDirect takes no gate: only its own thread changes the simulator, and the SDK allows direct and
// reflection runs to overlap. Lock order: m_simulatorGate, then m_mutex. Neither is ever taken by the mixer, the Studio thread
// or the main thread, and no thread waits for the gate while holding m_mutex, so shutdown can join both workers.
//
// Direct-first scheduling: std::shared_mutex does not hold new readers back for a waiting writer, so the gate alone can let
// consecutive reflection runs delay a change (section 25). So the direct worker only tries the gate: if a run holds it, the
// worker keeps its changes, still simulates and publishes on the current scene, and retries. Meanwhile its request stops a
// new run from starting, and a run in progress is never interrupted, so a change waits for at most that one run. Overdue
// reflection frames are replaced, never queued. If direct work never lets up, one run is let through per
// ReflectionStarvationLimit, and the limit keeps those from being consecutive.

#include "Audio/AudioSystem.h"
#include "Audio/SteamAudioRuntime.h"

#if defined(ENGINE_HAS_STEAMAUDIO)
#include <optional>
#include <fmod_studio.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <atomic>
#include <mutex>
#include <shared_mutex>
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
    // M8.3: the session's validated Embree device, or null when the session runs the default ray tracer (not requested, or
    // fallen back). Main thread. Offline baking shares it, retaining it for as long as a bake runs.
    IPLEmbreeDevice GetEmbreeDevice() const noexcept
    {
        return m_embreeDevice;
    }

    // Verification: an immediate tick that returns after its results are published.
    void RunNow() noexcept;
    std::vector<SteamAudioSourceSnapshot> GetSnapshots() const;
    void SetTestControls(const SteamAudioSimulationTestControls& controls);
    void* GetWorkerThreadForTesting() noexcept;
    void* GetReverbGroupForTesting() noexcept;

    // M6.7, main thread, before any bank: binds the reverb source. The DSP and the sends are authored; see section 28.
    void StartEnvironmentalReverb() noexcept;

    // Production route, main thread. See AudioSystem and section 29.
    void BeginReflectionSession() noexcept;
    void EndReflectionSession() noexcept;
    void ReleaseReflectionReturn() noexcept;
    void SetListenerReflectionsWithheld(bool withheld) noexcept;

    // M8.4: one committed batch holding a valid REVERB layer, and its probes (Steam Audio space: x, y, z, radius) for coverage.
    struct BakedReverbBatch
    {
        IPLProbeBatch batch = nullptr;
        std::vector<std::array<float, 4>> spheres;
    };

    // Main thread. Replaces the registered set; an empty set returns the reverb source to realtime simulation. Takes ownership of
    // every batch: the worker adds them to the simulator, and releases each only after removing it and committing. False when no
    // reverb source runs here (no reflections, or the DSP lock path), in which case the batches are released at once. While baked
    // data is registered, the reverb is withheld until a baked run finds the listener inside a probe, and again whenever one does not.
    // `listener`: the shared reverb uses the batches too (M8.4). Without it they serve baked static sources only (M8.5), and the reverb
    // stays realtime.
    bool SetBakedReverb(std::vector<BakedReverbBatch> batches, bool listener = true) noexcept;
    // M8.5: `baked`, when given, is the static source's exact STATICSOURCE identifier. Its slot then looks that layer up and is never
    // traced in real time; without a slot it falls back to the shared reverb send like any other source-centric request.
    bool SetSourceCentricRequest(FMOD::Studio::EventInstance& instance, bool enabled, const IPLBakedDataIdentifier* baked = nullptr);
    // M8.5, main thread: the source moved from where it was baked. Its baked reflections are withheld for good; nothing else changes.
    bool DisableBakedReflections(FMOD::Studio::EventInstance& instance) noexcept;
    // An empty return passes what it receives, which would double any event's fixed send. So the return bus stays muted whenever the route
    // was asked for and its effects are not installed. It costs no event its direct sound or any other send.
    void MuteReturnUntilInstalled() noexcept;
    void SetReflectionBounces(int bounces) noexcept;

    bool IsAcousticSimulationAvailable() const noexcept
    {
        return m_synchronization == SteamAudioSynchronization::Snapshot;
    }

    // M9.3. Set by Start, then read-only.
    bool IsPathingAvailable() const noexcept
    {
        return m_pathingSimulator != nullptr;
    }

    // M9.5, main thread: the batches pathing searches (their PATHING layers, and their probes for coverage), owned from here on, in priority
    // order; empty clears them. The worker registers them with the pathing simulator only, commits, and releases those they replace after
    // removing them and committing.
    void SetPathingBatches(std::vector<BakedReverbBatch> batches) noexcept;
    // M9.5, main thread, before the instance starts: read on the Studio thread as its spatializers are bound. False without a pathing simulator.
    bool SetPathingRequest(FMOD::Studio::EventInstance& instance, bool enabled);
    // M9.6, main thread, display only.
    std::optional<AudioSystem::PathingEventStatus> GetPathingStatus(const FMOD::Studio::EventInstance& instance) const;
    void SetPathingVisualization(bool enabled) noexcept
    {
        m_pathingVisualize.store(enabled, std::memory_order_relaxed);
    }
    // Steam Audio space.
    std::vector<AudioSystem::PathingSegment> GetPathingSegments() const;

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
        // Authored with Apply Reflections, in a session with reflections.
        bool reflectionConsumer = false;
        // M8.5: the baked layer it asked for, if any.
        std::optional<IPLBakedDataIdentifier> baked;
        // M9.5: its Audio Source asked for pathing; and how its spatializer applies the direct simulation's occlusion and transmission
        // (Simulation-Defined), as authored, which is what pathing makes up for.
        bool pathing = false;
        bool occlusionApplied = false;
        bool transmissionApplied = false;
        // M9.6, display only: authored Apply Pathing, before Engine turned it off.
        bool pathingAuthored = false;
    };

    // Main-thread record of a bound source.
    struct ActiveSource
    {
        FMOD::Studio::EventInstance* instance = nullptr;
        SourceId id = 0;
        SnapshotSource* snapshot = nullptr;
        IPLSource realSource = nullptr;
        IPLCoordinateSpace3 pose{};
        bool reflectionConsumer = false;
        std::optional<IPLBakedDataIdentifier> baked;
        // M6.7.
        bool hasReverbSend = false;
        float reverbSendLevel = 0.0f;
        float reverbSendTarget = 0.0f;
        bool sendOpened = false;
        bool sendSearchReported = false;
        bool slotAssigned = false;
        bool slotDenied = false;
        // M9.3: Apply Pathing as Engine last set it on this DSP: on only while the snapshot holds a valid pathing result.
        bool pathingApplied = false;
        bool pathingRequested = false;
        // M9.6, display only: as authored, read at bind.
        bool pathingAuthored = false;
        bool occlusionApplied = false;
        bool transmissionApplied = false;
    };

    // Main-thread record of a source on its way out. Stage A: the worker removes the private source from the simulator,
    // commits, and hands it here (workerDone). Stage B: once the plugin has also let go of the snapshot, the private source
    // is released and the snapshot destroyed. Anything the plugin reaches through a snapshot therefore outlives the plugin.
    struct RetiringSource
    {
        SnapshotSource* snapshot = nullptr;
        SourceId id = 0;
        bool workerDone = false;
        // Null when private creation failed or was never reached. Out of the simulator; owned here from workerDone on.
        IPLSource privateSource = nullptr;
        // M6.7: a slot given after creation is a second source, because a source's flags are fixed when it is made.
        IPLSource privateReflectionSource = nullptr;
    };

    // The worker's acknowledgement of Stage A, sent only after the commit that took the source out of the run list.
    struct RemovedPrivateSource
    {
        SourceId id = 0;
        IPLSource realSource = nullptr;
        IPLSource reflectionSource = nullptr;
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
            ClearAcousticScene,
            AddReverbSource,
            SetBakedReverb,
            DisableBakedSource,
            SetPathingBatch
        };

        Kind kind = Kind::AddSource;
        SourceId id = 0;
        SnapshotSource* snapshot = nullptr;
        bool reflectionConsumer = false;
        void* scene = nullptr;
        std::shared_ptr<const AcousticMeshData> mesh;
        AcousticTransform world{};
        // SetBakedReverb: the new set, owned by the command until the worker takes it, and whether the listener uses it.
        std::shared_ptr<std::vector<BakedReverbBatch>> baked;
        bool bakedListener = false;
        // AddSource: the baked layer the source asked for.
        std::optional<IPLBakedDataIdentifier> bakedSource;
        // SetPathingBatches: the new set (empty clears), owned by the command until the worker takes it.
        std::shared_ptr<std::vector<BakedReverbBatch>> pathingBatches;
        // AddSource, M9.5: as in LifecycleChange.
        bool pathing = false;
        bool occlusionApplied = false;
        bool transmissionApplied = false;
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
        std::chrono::steady_clock::time_point postedAt{};
    };

    // Worker-owned, between thread start and join.
    struct PrivateSource
    {
        SnapshotSource* snapshot = nullptr;
        // Null when creation failed: the snapshot then stays at its last, neutral value.
        IPLSource realSource = nullptr;
        IPLCoordinateSpace3 pose{};
        // Null unless given a slot after creation; then the reflection worker uses this, and realSource stays direct only.
        IPLSource reflectionSource = nullptr;
        // Changes only with the simulator gate held exclusively.
        bool reflectionCapable = false;
        // M8.5: its slot looks this layer up instead of tracing. Disabled once the source moves: then withheld for good.
        std::optional<IPLBakedDataIdentifier> baked;
        bool bakedDisabled = false;
        // M9.3: its source in the pathing simulator, when pathing runs. Never seen by the plugin: results reach it only as copies, through
        // the snapshot's pathing channel. Released by the worker once removed and committed. M9.5: only for a source that asked for
        // pathing and got capacity; created and removed by the direct worker under the exclusive gate.
        IPLSource pathingSource = nullptr;
        bool pathingRequested = false;
        bool occlusionApplied = false;
        bool transmissionApplied = false;
        // M9.5, reflection worker only: the batch its paths come from (index into m_pathingBatches, -1 for none), its smoothed pathing gain,
        // and when that gain was last advanced. Reset whenever the batch changes, coverage is lost, or the pathing source is (re)created.
        int pathingBatch = -1;
        float pathingGain = 0.0f;
        std::chrono::steady_clock::time_point pathingGainAt{};
        // M9.6, display only: inside some registered batch's probes.
        bool pathingSourceInside = false;
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
    void BindSnapshotSource(FMOD::Studio::EventInstance& instance, FMOD::DSP& dsp, bool pathingAuthored);
    void BindRealSource(FMOD::Studio::EventInstance& instance, FMOD::DSP& dsp);

    void ApplyLifecycleChanges() noexcept;
    void ReclaimRetiredSources() noexcept;
    bool ReadListener(IPLCoordinateSpace3& listener) noexcept;
    void PostFrame(std::uint64_t ticket) noexcept;
    void SimulateUnderLock() noexcept;

    void WorkerMain() noexcept;
    void ApplyWorkerCommands(const std::vector<WorkerCommand>& commands, std::vector<RemovedPrivateSource>& removed) noexcept;
    // Requires m_mutex, or that the worker has been joined.
    void AcceptWorkerRemovals() noexcept;
    void SimulateAndPublish(const InputFrame& frame) noexcept;
    bool ReflectionDimensionsMatch(const IPLSimulationSettings& settings) const noexcept;
    void StartReflectionInfrastructure(const IPLSimulationSettings& settings, const SteamAudioOptions& options);
    void ReflectionWorkerMain() noexcept;
    // False when it gave way to direct work without running.
    bool RunReflections(const InputFrame& frame, bool yieldToDirect) noexcept;
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
    std::vector<RemovedPrivateSource> m_workerRemoved;
    std::unordered_map<SourceId, AcousticTransform> m_pendingAcousticTransforms;
    std::uint64_t m_completedTicket = 0;
    std::condition_variable m_reflectionWake;
    InputFrame m_pendingReflectionFrame;
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
    std::thread m_reflectionWorker;
    std::chrono::steady_clock::time_point m_nextReflectionFrame{};
    // Reflection worker only.
    std::chrono::steady_clock::time_point m_lastReflectionStart{};
    void ServeReflectionSlots(bool& commit) noexcept;
    void ApplySlotReports() noexcept;
    void RampReverbSends() noexcept;
    bool m_reflectionRoutingRequested = false;
    std::atomic<int> m_reflectionBounces{8};
    bool m_warnedSourceCentricWithoutSend = false;
    bool m_capacityReported = false;
    // M10.4: the first time opted-in sources wait for a pathing place, said once, as reflection capacity is.
    bool m_pathingCapacityReported = false;

    // M6.7. The SnapshotSource belongs to the main thread; the real source to the worker until it is joined.
    bool m_reverbWanted = false;
    SteamAudioReverbRoutingForTesting m_reverbRouting = SteamAudioReverbRoutingForTesting::Exclusive;
    SnapshotSource* m_reverbSnapshot = nullptr;
    IPLSource m_reverbRealSource = nullptr;
    // M8.4. The reverb is withheld when either the session asks (main thread) or baked data does not cover the listener (worker,
    // after each baked run, so what is heard always comes from a run that covered it). One lock, so the two writers agree.
    std::mutex m_withholdMutex;
    bool m_listenerWithheld = false;
    bool m_bakedUncovered = false;
    void ApplyReverbWithhold() noexcept;
    void SetBakedUncovered(bool uncovered) noexcept;
    // Worker: the registered set, read by reflection runs under the shared gate and changed only under the exclusive one.
    std::vector<BakedReverbBatch> m_bakedReverb;
    // Worker: the shared reverb uses the registered set (M8.4), as opposed to baked static sources alone.
    bool m_bakedListener = false;
    // Main thread, under m_mutex: the baked layer each requesting instance asked for (M8.5).
    std::vector<std::pair<FMOD::Studio::EventInstance*, IPLBakedDataIdentifier>> m_bakedRequests;
    // Worker: the reverb snapshot it published to, set with the source it belongs to.
    SnapshotSource* m_workerReverbSnapshot = nullptr;
    void WorkerSetBakedReverb(std::vector<BakedReverbBatch>& batches, bool& commit) noexcept;
    void ReleaseBakedBatches(std::vector<BakedReverbBatch>& batches) noexcept;
    // Worker only: authored consumers waiting for a reflection slot, oldest first, each with whether fallback was reported.
    std::vector<std::pair<SourceId, bool>> m_slotQueue;
    // Worker to main thread, under m_mutex: true for a slot given, false for no slot free.
    std::vector<std::pair<SourceId, bool>> m_slotReports;
    std::chrono::steady_clock::time_point m_lastReverbRamp{};
    bool m_reverbGroupLocked = false;
    bool m_holdReverbReturn = false;

    // Production route. The DSPs live until shutdown: releasing a Reverb clears the plugin's global source from an FMOD thread that FMOD's DSP lock
    // does not exclude, so Engine never does that while a source is bound and in use. The group and lock last one gameplay session.
    bool m_productionRouting = false;
    FMOD::Studio::Bus* m_returnBus = nullptr;
    FMOD::ChannelGroup* m_returnGroup = nullptr;
    FMOD::DSP* m_returnReverb = nullptr;
    FMOD::DSP* m_returnMixer = nullptr;
    // Installed means sends may open: either Engine's DSPs are on the group, or the banks authored their own.
    bool m_returnInstalled = false;
    bool m_returnDspsAdded = false;
    bool m_bindingLossReported = false;
    // Under m_mutex: read on the Studio thread as a spatializer is created. Instances that asked; forgotten when the pointer is attached again.
    std::vector<const FMOD::Studio::EventInstance*> m_sourceCentricRequests;

    // Set by Start before any bank, then read-only until Shutdown.
    bool m_reflectionsActive = false;
    bool m_assumeReflectionConsumers = false;
    bool m_withholdReflectionIr = false;
    // Main thread: geometry has existed, so reflection runs continue through an empty scene and its IRs fall to silence.
    bool m_reflectionSceneUsed = false;
    // The SDK decides whether to restart accumulation by comparing scene version numbers only, and a replaced root restarts its
    // numbering, so the old room's energy could survive. A listener change also restarts it: set under the exclusive gate
    // when the root is replaced, and taken by the next reflection run.
    std::atomic<bool> m_reflectionAccumulationStale{false};
    IPLSimulationSettings m_reflectionSettings{};

    // M9.3: the pathing-only simulator, sharing m_acousticRoot. Created by Start before the workers, released at Shutdown after both are
    // joined. Everything that changes it (scene, sources, batch, commit) runs on the direct worker with m_simulatorGate held exclusively,
    // exactly as for m_simulator; its runs follow each reflection run on the reflection worker, under the shared gate. Never concurrently.
    IPLSimulator m_pathingSimulator = nullptr;
    bool m_pathingValidation = false;
    // Set by Start, then read-only.
    std::size_t m_pathingSourceLimit = 0;
    int m_pathingOcclusionSamples = 0;
    // Worker: the registered batches in priority order, read by pathing runs under the shared gate and changed only under the exclusive one.
    std::vector<BakedReverbBatch> m_pathingBatches;
    // Direct worker: pathing sources now, and opted-in sources waiting for capacity, oldest first.
    std::size_t m_pathingSourceCount = 0;
    std::vector<SourceId> m_pathingQueue;
    // Under m_mutex: instances that asked for pathing, read on the Studio thread as a spatializer is bound.
    std::vector<const FMOD::Studio::EventInstance*> m_pathingRequests;
    // M9.6: the Editor wants path segments; and the last run's, under m_mutex (Steam Audio space).
    std::atomic<bool> m_pathingVisualize{false};
    std::vector<AudioSystem::PathingSegment> m_pathingSegments;
    void WorkerSetPathingBatches(std::vector<BakedReverbBatch>& batches) noexcept;
    // Direct worker, exclusive gate: a pathing source for `source`, true when created.
    bool CreatePathingSource(PrivateSource& source) noexcept;
    void ServePathingQueue(bool& commit) noexcept;
    void RunPathing(const InputFrame& frame) noexcept;
    void SetSimulatorScenes(IPLScene scene) noexcept;
    // Main thread: Apply Pathing follows each source's pathing availability.
    void UpdatePathingActivation() noexcept;

    // Exclusive around simulator and scene changes, shared across a reflection run. See the lock order above.
    std::shared_mutex m_simulatorGate;
    // The direct worker is at, or about to reach, the gate. Lets the reflection worker give way before taking it.
    std::atomic<int> m_directGateRequests{0};
    // Guarded by m_mutex. Verification only: the direct worker blocks at the gate, as with the gate alone.
    bool m_blockAtGateForTesting = false;

    // Worker only, between thread start and join.
    // Changed only with m_simulatorGate held exclusively, so the reflection worker may look entries up while sharing it.
    std::unordered_map<SourceId, PrivateSource> m_privateSources;
    std::size_t m_reflectionSourceCount = 0;
    std::uint32_t m_generation = 0;
    // Written with m_simulatorGate held exclusively; the reflection worker reads it while sharing the gate.
    IPLScene m_acousticRoot = nullptr;
    // Worker: removed instances, held until the root that contains them is gone (see RebuildEmbreeRoot).
    std::vector<IPLInstancedMesh> m_retiredEmbreeInstances;
    std::unordered_map<const AcousticMeshData*, AcousticSubScene> m_acousticSubScenes;
    std::unordered_map<SourceId, AcousticObject> m_acousticObjects;
};
#endif
}
