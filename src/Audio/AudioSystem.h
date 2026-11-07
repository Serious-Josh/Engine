#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Audio
{
// FMOD Studio's 128-bit identifier of an event, bank, bus or VCA (FMOD_GUID's layout). Event GUIDs are the stable
// references Engine stores: they survive renames and moves in FMOD Studio.
struct Guid
{
    std::uint32_t data1 = 0;
    std::uint16_t data2 = 0;
    std::uint16_t data3 = 0;
    std::array<std::uint8_t, 8> data4{};

    bool operator==(const Guid&) const = default;
};

// FMOD's text form, "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" (as exported by FMOD Studio's GUIDs.txt); lowercase out.
std::optional<Guid> ParseGuid(std::string_view text);
std::string ToString(const Guid& guid);

struct EventInfo
{
    Guid id;
    // "event:/..." for display and debugging; empty unless a strings bank naming the event is loaded.
    std::string path;
};

// An event instance created by AudioSystem. A value: it does not own the instance and becomes stale (every query
// answers "invalid") once the instance is released, its bank is unloaded or audio shuts down.
struct EventHandle
{
    std::uint32_t index = 0;
    // 0 is never issued, so a default handle is always invalid.
    std::uint32_t generation = 0;

    bool operator==(const EventHandle&) const = default;
};

// Audio.2A world-space 3D attributes, in Engine's and FMOD's shared convention: left-handed, +X right, +Y up,
// +Z forward, meters and meters per second. FMOD is initialized without FMOD_INIT_3D_RIGHTHANDED, so values pass
// through unchanged. forward and up must be unit length and perpendicular (FMOD's requirement).
struct Attributes3D
{
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> velocity{0.0f, 0.0f, 0.0f};
    std::array<float, 3> forward{0.0f, 0.0f, 1.0f};
    std::array<float, 3> up{0.0f, 1.0f, 0.0f};

    bool operator==(const Attributes3D&) const = default;
};

enum class PlaybackState
{
    // The handle is stale or was never valid.
    Invalid,
    Starting,
    Playing,
    Sustaining,
    Stopping,
    Stopped
};

enum class Status
{
    // Initialize has not run, or Shutdown has completed.
    NotStarted,
    // FMOD is running and mixing to an output device.
    Active,
    // FMOD is running with its no-sound output because no usable output device was available.
    NoOutput,
    // This build has no FMOD backend (ENGINE_ENABLE_FMOD=OFF or the SDK was not found), or the host chose not to
    // start audio.
    Disabled,
    // FMOD failed for a reason other than the output device; Engine continues without audio.
    Unavailable
};

std::string_view ToString(Status status);

// Controlled failures for verification only: hardware and SDK failures that cannot be produced on demand.
// Production hosts always pass None.
enum class InjectedFailure
{
    None,
    // The device output fails to initialize, as when no usable device exists; the no-sound retry runs.
    OutputInitialization,
    // The no-sound retry fails too, after the device output failed.
    NoSoundInitialization,
    // Studio system creation fails before anything was created.
    StudioCreation,
    // Studio initialization fails for a non-output reason after the Studio system was created.
    StudioInitialization
};

struct InitializeOptions
{
    // Skips the output device and starts with FMOD's no-sound output directly (headless diagnostics).
    bool noSoundOutput = false;
    InjectedFailure injectedFailure = InjectedFailure::None;
    // Verification: FMOD's offline output with synchronous Studio updates, so each update mixes deterministically.
    bool offlineRendering = false;
    // Investigation: FMOD_SPEAKERMODE for setSoftwareFormat; 0 leaves FMOD's choice.
    int softwareSpeakerModeForTesting = 0;
};

// SA.1: Steam Audio, Valve's spatial audio, running inside FMOD as an FMOD plugin. It never replaces FMOD: without it
// (any status but Active) FMOD behaves exactly as it does in a build that has no Steam Audio at all.
enum class SteamAudioStatus
{
    // Not requested since audio started: the project does not use Steam Audio.
    NotRequested,
    // The plugin is registered with FMOD and uses Engine's Steam Audio context and HRTF.
    Active,
    // This build has no Steam Audio (ENGINE_ENABLE_STEAMAUDIO=OFF, or its SDK or the FMOD backend was missing).
    NotCompiled,
    // Requested, but it could not start; nothing of it remains and FMOD continues unchanged.
    Unavailable
};

std::string_view ToString(SteamAudioStatus status);

// Controlled Steam Audio failures for verification only; production hosts always pass None.
enum class SteamAudioInjectedFailure
{
    None,
    // phonon.dll or phonon_fmod.dll is absent beside the executable.
    MissingRuntime,
    // Steam Audio's context cannot be created (e.g. an incompatible phonon.dll).
    ContextCreation,
    // The HRTF cannot be created, after the context was.
    HRTFCreation,
    // FMOD refuses the plugin, after the context and HRTF were created.
    PluginLoad,
    // The plugin loads, but its spatializer cannot be identified (an unverified plugin build).
    PluginIdentity,
    // SA.3 (these leave Steam Audio Active, with effects as direct HRTF): the simulator cannot be created.
    SimulatorCreation,
    // A simulation source cannot be created for a new spatializer.
    SourceCreation,
    // A created source cannot be bound to its spatializer.
    SourceBinding,
    // D9 (these leave Steam Audio Active): a loaded binary is not the validated one, so the DSP lock path is used.
    BinaryIdentity,
    // The startup handshake refuses, so the DSP lock path is used.
    HandshakeRefused,
    // The plugin never returns the handshake's reference: the wait must end in quarantine and the DSP lock path.
    HandshakeNoRelease,
    // The simulation worker cannot start, so the DSP lock path is used.
    WorkerStart,
    // The worker cannot create a private source: the bound source stays neutral.
    PrivateSourceCreation,
    // M4: the worker cannot build an object's acoustic geometry: that object contributes nothing.
    AcousticGeometry,
    // M4 Stage 2 (these fall back to the default ray tracer): iplEmbreeDeviceCreate fails.
    EmbreeDevice,
    // The device was created, but Embree does not give the right answer on a known scene.
    EmbreeProbe,
    // M6.4 (these leave direct simulation running): reflection dimensions disagree, so the infrastructure stays off.
    ReflectionCompatibility,
    // The reflection worker cannot start, so the infrastructure stays off.
    ReflectionWorkerStart
};

// M4 Stage 2: which CPU ray tracer builds the acoustic scene. Independent of SteamAudioSynchronization, and fixed for the
// session: a simulator's scene type cannot change, and every scene in it must be of that type.
// Listening diagnostic only. How events authored with Apply Reflections use the reverb when environmentalReverbForTesting is on.
enum class SteamAudioReverbRoutingForTesting
{
    // The M6.7 behaviour: an event uses its reflection slot or the reverb, never both.
    Exclusive,
    // With reflectionInfrastructureForTesting and no reverb: every send stays open, so a Mixer Return sharing the bus keeps processing. With no
    // reverb source, the Steam Audio Reverb DSP adds nothing. A diagnostic workaround, not a production routing decision.
    SourceOnly,
    // No event gets a reflection slot; every event sends to the reverb.
    ListenerOnly,
    // An event with a slot keeps its send open too. Both responses overlap, deliberately and uncompensated.
    Combined
};

enum class SteamAudioRayTracer
{
    Default,
    Embree
};

constexpr const char* ToString(SteamAudioRayTracer rayTracer) noexcept
{
    return rayTracer == SteamAudioRayTracer::Embree ? "Embree" : "Default";
}

// D9: how simulation results reach the plugin. Chosen once per session, before any Steam Audio DSP can exist.
enum class SteamAudioSynchronization
{
    // No simulator, so no sources: spatializers run as direct HRTF.
    None,
    // Option B: wait-free snapshots written by the simulation worker.
    Snapshot,
    // Option A: the fallback. RunDirect on the main thread under FMOD's DSP lock.
    DspLock
};

struct SteamAudioOptions
{
    SteamAudioInjectedFailure injectedFailure = SteamAudioInjectedFailure::None;
    // How many sources SourceCreation and SourceBinding fail before later ones succeed.
    int injectedFailureCount = 1 << 30;
    // Verification: use the DSP lock path whatever the compatibility gate would say.
    bool forceDspLockSynchronization = false;
    SteamAudioRayTracer rayTracer = SteamAudioRayTracer::Default;
    // M6.4 verification only, and never a user setting. Off: no plugin settings call, no reflection worker, no reflection
    // resources. Fixed before audio initialization for the whole session, because the plugin call is process-wide and every
    // spatializer reads it. No reflected audio is produced either way; see SteamAudioIntegration.md, section 25.
    bool reflectionInfrastructureForTesting = false;
    // M6.7 verification only, and never a user setting: listener-centric reverb through an authored return and sends. Starts the
    // reflection infrastructure if needed. An event uses its reflection slot or the reverb, never both. See SteamAudioIntegration.md, section 28.
    bool environmentalReverbForTesting = false;
    SteamAudioReverbRoutingForTesting reverbRoutingForTesting = SteamAudioReverbRoutingForTesting::Exclusive;
    // Production route: every eligible event feeds the shared listener-centric reverb through its post-Spatializer send, and events that ask
    // also get source-centric reflections. Engine owns the return's Reverb and Mixer Return DSPs. Stereo mixer only. Section 29.
    bool reflectionRouting = false;
    // How many times reflection rays bounce. See SetSteamAudioReflectionBounces.
    int reflectionBounces = 8;
    // Listening diagnostic only: holds bus:/SteamAudioReverb open with lockChannelGroup. Releasing a Steam Audio Reverb DSP clears the plugin's reverb source,
    // and Engine sets it once, so without this the listener-centric path is silent after the bus is freed. Not a production lifecycle.
    bool holdReverbReturnForTesting = false;
    // Verification only: treat every spatializer as authored with Apply Reflections, for tests of the worker and its capacity.
    bool assumeReflectionConsumersForTesting = false;
    // Verification only: reflection sources simulate, but the plugin gets a null IR.
    bool withholdReflectionIrForTesting = false;
    // Verification only: Steam Audio context allocator, for poison-on-free and per-thread allocation counts.
    // M6.6: the mixer pool is on unless a test replaces the allocator or asks for the SDK's own (a before/after baseline).
    bool useSdkAllocatorForTesting = false;
    // Fills freed pool blocks, so anything relying on freed or zeroed memory misbehaves visibly.
    bool fillFreedPoolBlocksForTesting = false;
    // Multiplies the supported DSP counts the pool is sized for; below 1 forces exhaustion.
    float poolCapacityScaleForTesting = 1.0f;
    void* (*contextAllocateForTesting)(std::size_t size, std::size_t alignment) = nullptr;
    void (*contextFreeForTesting)(void* block) = nullptr;
};

// The direct results of one simulation of one source: plain values, as the plugin reads them.
struct SteamAudioDirectSnapshot
{
    std::uint32_t generation = 0;
    std::uint32_t tag = 0;
    float distanceAttenuation = 1.0f;
    float airAbsorption[3] = {1.0f, 1.0f, 1.0f};
    float directivity = 1.0f;
    float occlusion = 1.0f;
    float transmission[3] = {1.0f, 1.0f, 1.0f};
};

// Verification hooks: the first runs on the simulation worker on a slot not yet published, the second on the mixer on the
// snapshot being read.
using SteamAudioSnapshotPublishFilter = void (*)(SteamAudioDirectSnapshot& slot);
using SteamAudioSnapshotReadObserver = void (*)(const SteamAudioDirectSnapshot& snapshot, const void* source);

// SA.3: the simulation sources of the running system, for diagnostics and verification.
// M6.6: what Steam Audio's allocations during a mix did. See SteamAudioMixerAllocator.h.
struct SteamAudioMixerPoolStats
{
    bool armed = false;
    std::size_t capacityBytes = 0;
    std::size_t usedBytes = 0;
    std::size_t peakBytes = 0;
    std::size_t outstandingBlocks = 0;
    std::uint64_t poolAllocations = 0;
    std::uint64_t poolFrees = 0;
    // Pool exhausted or unsupported request: the heap served it, on the mixer. Logged. Zero in the supported workload.
    std::uint64_t mixerHeapAllocations = 0;
    // Heap blocks freed during a mix, handed to the main thread.
    std::uint64_t mixerHeapFrees = 0;
    // A full size class served from a larger one: still no heap, and the warning that the pool is short.
    std::uint64_t largerClassAllocations = 0;
    std::size_t largestHeapRequest = 0;
    std::uint64_t doubleFrees = 0;
    std::uint64_t invalidFrees = 0;
    // Process lifetime: regions kept past their session for late blocks, regions unmapped, and regions held now.
    std::uint64_t regionsRetained = 0;
    std::uint64_t regionsReclaimed = 0;
    int regionsHeld = 0;
};

struct SteamAudioSimulationStats
{
    bool simulatorActive = false;
    SteamAudioSynchronization synchronization = SteamAudioSynchronization::None;
    // Why this synchronization: what the compatibility gate accepted or refused.
    std::string synchronizationDetail;
    // M4 Stage 2: what the project asked for, what runs, and why they differ when they do.
    SteamAudioRayTracer requestedRayTracer = SteamAudioRayTracer::Default;
    SteamAudioRayTracer activeRayTracer = SteamAudioRayTracer::Default;
    std::string rayTracerDetail;
    // Sources in the simulator as of the last Update.
    std::size_t activeSources = 0;
    std::uint64_t sourcesCreated = 0;
    std::uint64_t sourcesDestroyed = 0;
    // Successful binds, at most one per DSP lifetime.
    std::uint64_t bindings = 0;
    // A DSP reported again while already bound; it is never bound a second time.
    std::uint64_t repeatedBindsRefused = 0;
    std::uint64_t creationFailures = 0;
    std::uint64_t bindingFailures = 0;
    std::uint64_t lockFailures = 0;
    std::uint64_t simulationRuns = 0;
    // Option B: sources whose private half failed (they stay neutral), and the worker's and plugin's accounting.
    std::uint64_t privateSourceFailures = 0;
    // Option B private-source lifetime: created by the worker, removed from simulation (Stage A), released (Stage B).
    std::uint64_t privateSourcesCreated = 0;
    std::uint64_t privateSourcesRemoved = 0;
    std::uint64_t privateSourcesReleased = 0;
    // M6.4 reflection infrastructure: off unless a verification session asked for it and it passed its checks.
    bool reflectionInfrastructureActive = false;
    std::string reflectionInfrastructureDetail;
    int reflectionSourceCapacity = 0;
    // Reflection-capable private sources now in the simulator; sources over capacity simulate direct only and are counted.
    std::size_t reflectionSources = 0;
    std::uint64_t reflectionCapacityExhausted = 0;
    // M6.5: sources whose IR the plugin can read, and the plugin's reads of it, counted when each source is reclaimed.
    std::uint64_t reflectionIrPublished = 0;
    // M6.7. Now: events with a reflection slot, and events whose send is open. Ever: sends opened, and waiting events given a slot
    // after they had fallen back. Events with no authored send cannot fall back.
    bool environmentalReverbActive = false;
    std::size_t reflectionSlotEvents = 0;
    std::size_t reverbFallbackEvents = 0;
    std::uint64_t reverbSends = 0;
    std::uint64_t reflectionSlotPromotions = 0;
    std::uint64_t reverbEventsWithoutSend = 0;
    bool reverbIrPublished = false;
    // Production route. Requested source-centric events that hold no slot now, and how often a request found none free.
    bool reflectionRoutingActive = false;
    bool reflectionReturnInstalled = false;
    std::uint64_t reflectionSessions = 0;
    std::size_t sourceCentricUnavailableEvents = 0;
    std::uint64_t sourceCentricDenied = 0;
    // Asked for source-centric reflections on an event with no Send anywhere in it. No slot: nothing of it would reach the return.
    std::uint64_t sourceCentricWithoutSend = 0;
    std::uint64_t sourceCentricSendChecks = 0;
    // The event's groups weren't readable when asked, so the slot was granted.
    std::uint64_t sourceCentricSendChecksUnreadable = 0;
    // The plugin dropped the reverb source. Never expected while Engine owns the DSPs.
    std::uint64_t reverbBindingLost = 0;
    std::uint64_t reverbIrReads = 0;
    std::uint64_t reflectionIrReads = 0;
    std::uint64_t reflectionRuns = 0;
    // Reflection frames replaced before they ran: skipped, never queued.
    std::uint64_t reflectionFramesCoalesced = 0;
    // Direct-first scheduling: times a reflection run gave way, and runs let through to prevent starvation.
    std::uint64_t reflectionDeferrals = 0;
    std::uint64_t reflectionForcedRuns = 0;
    // The most reflection runs that completed during one wait of the direct worker at the gate.
    std::uint64_t maximumReflectionRunsDuringGateWait = 0;
    double maximumReflectionRunMicroseconds = 0.0;
    // Start to start of successive reflection runs, and from a frame being posted to its run starting.
    double reflectionIntervalMicrosecondsSum = 0.0;
    double maximumReflectionIntervalMicroseconds = 0.0;
    std::uint64_t reflectionIntervals = 0;
    double reflectionWakeMicrosecondsSum = 0.0;
    double maximumReflectionWakeMicroseconds = 0.0;
    // Longest a simulator or scene change waited for a reflection run. Direct simulation does not wait with it.
    double maximumSimulatorGateWaitMicroseconds = 0.0;
    std::uint64_t snapshotsPublished = 0;
    std::uint64_t publicationsSkipped = 0;
    // Frames replaced before the worker took them: simulation slower than its cadence.
    std::uint64_t coalescedFrames = 0;
    // Waiting for the worker and the plugin to let go.
    std::size_t retiringSources = 0;
    std::uint64_t sourceAnomalies = 0;
    // Sources the plugin never returned, deliberately leaked rather than freed.
    std::size_t quarantinedSources = 0;
    double lastSimulationMicroseconds = 0.0;
    double maximumSimulationMicroseconds = 0.0;
    double lastPublicationMicroseconds = 0.0;
    double maximumPublicationMicroseconds = 0.0;
    // M4: the acoustic scene, as the worker last left it.
    std::size_t acousticObjects = 0;
    std::size_t acousticSubScenes = 0;
    std::uint64_t acousticTriangles = 0;
    std::uint64_t acousticGeometryFailures = 0;
    std::uint64_t acousticTransformUpdates = 0;
    // Embree: root scenes rebuilt after a removal (see RebuildEmbreeRoot).
    std::uint64_t embreeRootRebuilds = 0;
    double maximumSubSceneBuildMicroseconds = 0.0;
    double lastSceneCommitMicroseconds = 0.0;
    double maximumSceneCommitMicroseconds = 0.0;
    // The main thread's wait for FMOD's DSP lock, and how long the mixer is held out while simulation publishes.
    double lastLockWaitMicroseconds = 0.0;
    double maximumLockWaitMicroseconds = 0.0;
    double lastLockHoldMicroseconds = 0.0;
    double maximumLockHoldMicroseconds = 0.0;
};

// One simulation source (verification only). The native pointers are opaque here and belong to the audio module.
struct SteamAudioSourceSnapshot
{
    void* nativeInstance = nullptr;
    void* nativeDsp = nullptr;
    void* nativeSource = nullptr;
    std::uint32_t bindCount = 0;
    // Option B: the value this source's snapshots carry in SteamAudioDirectSnapshot::tag.
    std::uint32_t tag = 0;
    // Steam Audio space, as last given to the simulator.
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    float distanceAttenuation = 1.0f;
    float occlusion = 1.0f;
    std::array<float, 3> transmission{1.0f, 1.0f, 1.0f};
};

// SA.3 verification only. Production simulates with no flags and no scene, so outputs keep their neutral values.
struct SteamAudioSimulationTestControls
{
    // IPLDirectSimulationFlags.
    int directFlags = 0;
    // An IPLScene made with the test's own geometry; it must outlive the AudioSystem's Shutdown.
    void* scene = nullptr;
    bool volumetricOcclusion = false;
    // Simulation time added inside the simulation span (under the lock, for the DSP lock path).
    int injectedSimulationMilliseconds = 0;
    // Added inside the reflection run, with the simulator gate held.
    int injectedReflectionMilliseconds = 0;
    // Verification only: measures each reflection interval from the last due frame, which rounds up to the frame and simulation ticks.
    bool legacyReflectionCadence = false;
    // Verification only: reflection runs start without giving way to direct work, to compare against direct-first scheduling.
    bool disableDirectFirstScheduling = false;
    // Verification only: reflection runs always see direct work pending, so only the starvation limit lets them through.
    bool reportDirectWorkAlwaysPending = false;
    // Each source's fill is abandoned before Publish.
    bool skipPublication = false;
    SteamAudioSnapshotPublishFilter publishFilter = nullptr;
    SteamAudioSnapshotReadObserver readObserver = nullptr;
};

// SA.2: the spatializers on one event instance. An event authored with both pans and attenuates its sound twice;
// a Steam Audio event must carry only the Steam Audio Spatializer (Documentation/Audio/SteamAudioIntegration.md).
struct SpatializerSummary
{
    // Steam Audio Spatializer effects, recognised by plugin identity (0 while Steam Audio is not Active).
    std::size_t steamAudio = 0;
    // FMOD's own spatializer/panner effects (FMOD Pan DSPs).
    std::size_t fmod = 0;
    // Steam Audio Spatializers authored with Distance Attenuation Off: the sound does not get quieter with distance.
    std::size_t steamAudioWithoutDistanceAttenuation = 0;

    bool IsDoubleSpatialized() const noexcept { return steamAudio > 0 && fmod > 0; }
};

// M4: one mesh's acoustic geometry in Engine space, immutable once published and safe to share between objects. Plain values:
// the audio module converts it to Steam Audio's space.
struct AcousticMaterialValues
{
    std::array<float, 3> absorption{};
    float scattering = 0.0f;
    std::array<float, 3> transmission{};
};

struct AcousticMeshData
{
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<std::uint32_t, 3>> triangles;
    // One per triangle, into materials.
    std::vector<std::int32_t> triangleMaterials;
    std::vector<AcousticMaterialValues> materials;
};

// Engine's convention: row-indexed storage, column-vector math, translation in column 3.
using AcousticTransform = std::array<std::array<float, 4>, 4>;

// M7. UniformFloor and Centroid are Steam Audio's own; Uniform3D is an Engine-computed grid (see SteamAudioProbes).
enum class ProbePlacementMode
{
    UniformFloor,
    Centroid,
    Uniform3D
};

struct ProbeGeometryObject
{
    std::shared_ptr<const AcousticMeshData> mesh;
    AcousticTransform world{};
};

// All values in Engine space.
struct ProbeGenerationRequest
{
    ProbePlacementMode placement = ProbePlacementMode::UniformFloor;
    // The volume's object. The volume is a box of `size`, centred on this transform's origin.
    AcousticTransform world{};
    std::array<float, 3> size{};
    float spacing = 0.0f;
    float height = 0.0f;
    // In the object's local space.
    std::vector<std::array<float, 3>> manualProbes;
    // Read by UniformFloor, and by Uniform3D when filtering.
    std::vector<ProbeGeometryObject> geometry;
    // Drops generated probes inside provably closed geometry. Floor and Uniform 3D; never manual probes.
    bool filterInsideSolids = false;
};

struct ProbeSphere
{
    std::array<float, 3> center{};
    float radius = 0.0f;
};

struct ProbeGenerationResult
{
    bool success = false;
    std::string error;
    std::vector<ProbeSphere> probes;
    // What iplProbeBatchSave wrote.
    std::vector<std::byte> batch;
    // Manual probes dropped for coinciding with another probe.
    std::size_t duplicatesRemoved = 0;
    // Stopped on request. `error` says so too.
    bool cancelled = false;
    // Inside-solid filter: dropped, kept because they could not be classified, and the surfaces it could and could not trust.
    std::size_t rejectedProbes = 0;
    std::size_t ambiguousProbes = 0;
    std::size_t closedPieces = 0;
    std::size_t openPieces = 0;
    // Every generated probe lay inside closed geometry that also holds the whole box, which is more likely a room with outward faces: all were kept.
    bool enclosedByOneShell = false;
};

enum class ProbeGenerationStage
{
    Starting,
    BuildingScene,
    // Inside Steam Audio's generator, which reports no progress and can't be interrupted.
    Placing,
    Filtering,
    Packing,
    Finished
};

// One generation running on its own thread, on data it owns. Cancelling takes effect at the next checkpoint between SDK calls.
class ProbeGenerationJob final
{
public:
    void Cancel() noexcept { m_cancel.store(true, std::memory_order_relaxed); }
    bool IsCancelRequested() const noexcept { return m_cancel.load(std::memory_order_relaxed); }
    bool IsFinished() const noexcept { return m_finished.load(std::memory_order_acquire); }
    ProbeGenerationStage GetStage() const noexcept { return m_stage.load(std::memory_order_relaxed); }
    // Of the current stage; 0 of 0 when it can't tell.
    std::uint64_t GetDone() const noexcept { return m_done.load(std::memory_order_relaxed); }
    std::uint64_t GetTotal() const noexcept { return m_total.load(std::memory_order_relaxed); }
    // Only once finished.
    ProbeGenerationResult& GetResult() noexcept { return m_result; }

    // The generator's side.
    void SetStage(ProbeGenerationStage stage, std::uint64_t total = 0) noexcept
    {
        m_total.store(total, std::memory_order_relaxed);
        m_done.store(0, std::memory_order_relaxed);
        m_stage.store(stage, std::memory_order_relaxed);
    }
    void SetDone(std::uint64_t done) noexcept { m_done.store(done, std::memory_order_relaxed); }
    void Finish(ProbeGenerationResult result) noexcept
    {
        m_result = std::move(result);
        SetStage(ProbeGenerationStage::Finished);
        m_finished.store(true, std::memory_order_release);
    }

private:
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_finished{false};
    std::atomic<ProbeGenerationStage> m_stage{ProbeGenerationStage::Starting};
    std::atomic<std::uint64_t> m_done{0};
    std::atomic<std::uint64_t> m_total{0};
    ProbeGenerationResult m_result;
};

// Batches held for a gameplay session. Nothing consumes them until M8.
struct ProbeBatchStats
{
    std::size_t loadedBatches = 0;
    std::size_t loadedProbes = 0;
    std::uint64_t batchesCreated = 0;
    std::uint64_t batchesReleased = 0;
    std::uint64_t loadFailures = 0;
};

// Engine-owned FMOD Studio system (Audio.1A). Construct, use and destroy on the main thread. FMOD types never appear
// in this interface; the Studio system, and the Core system it owns, stay private to the audio module.
//
// At most one AudioSystem holds an FMOD Studio system per process: a second Initialize while another instance is
// running reports Unavailable without creating anything.
class AudioSystem final
{
public:
    AudioSystem();
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // True when this build was compiled against the FMOD SDK.
    static bool IsBackendCompiled() noexcept;
    // Bytes FMOD currently holds across the process (FMOD::Memory_GetStats), for leak checks; -1 when the backend
    // is not compiled or its runtime libraries were never loaded.
    static std::int64_t GetBackendAllocatedBytes() noexcept;

    // Starts FMOD and returns the resulting status. Only device output failures fall back to the no-sound output;
    // any other failure releases everything created so far and reports Unavailable. Never throws, and never
    // prevents the caller from continuing without audio. Calling it while already started changes nothing.
    Status Initialize(const InitializeOptions& options = {});

    // Leaves the session in its current state: a host that never wants audio records Disabled with a reason.
    void Disable(std::string_view reason);

    // Advances FMOD Studio by one Engine frame: flushes queued Studio commands to FMOD's own threads. Does not wait
    // for the mixer or for any GPU work. A no-op unless Active or NoOutput.
    void Update() noexcept;

    // Stops and releases every tracked event instance, unloads every bank, then releases the Studio system, which
    // releases its Core system. Idempotent; safe after any partial failure.
    void Shutdown() noexcept;

    Status GetStatus() const noexcept;
    // Why the status is what it is, for logs and diagnostics UI ("output device 'Speakers'", the FMOD error, ...).
    const std::string& GetStatusDetail() const noexcept;
    // Successful Studio updates since the last Initialize.
    std::uint64_t GetUpdateCount() const noexcept;

    // --- Steam Audio (SA.1) -------------------------------------------------------------------------------------
    // True when this build was compiled against the Steam Audio SDK (which requires the FMOD backend).
    static bool IsSteamAudioCompiled() noexcept;

    // Starts Steam Audio inside the running FMOD system, for a project that uses it. Must run while audio is running
    // and before any bank is loaded: banks whose events use Steam Audio effects only load with the plugin registered.
    // Runs once per Initialize; later calls change nothing and return the current status. A failure is logged,
    // reported as Unavailable and leaves FMOD unchanged; it never stops audio. Shutdown stops Steam Audio after FMOD
    // has destroyed every Steam Audio effect.
    SteamAudioStatus EnableSteamAudio(const SteamAudioOptions& options = {});
    SteamAudioStatus GetSteamAudioStatus() const noexcept;
    // Why the Steam Audio status is what it is, for logs and diagnostics.
    const std::string& GetSteamAudioStatusDetail() const noexcept;
    // Diagnostics: the Steam Audio Spatializer effects of one event instance, recognised by plugin identity (never by
    // effect name). nullopt for a stale handle or while FMOD has not yet created the instance's DSPs; 0 when Steam
    // Audio is not Active.
    std::optional<std::size_t> CountSteamAudioSpatializers(EventHandle handle) const;
    // SA.2: both kinds of spatializer on one event instance. nullopt for a stale handle or while FMOD has not yet
    // created the instance's DSPs.
    std::optional<SpatializerSummary> DescribeEventSpatializers(EventHandle handle) const;

    // --- M4 acoustic scene: main thread. Everything is handed to the simulation worker, which owns the Steam Audio scene. ---
    // True only under snapshot synchronization: geometry simulation must never run under FMOD's DSP lock.
    bool IsAcousticSimulationAvailable() const noexcept;
    // Each call is a request; a failure while building is counted in the simulation stats. False when unavailable.
    bool SetAcousticObject(std::uint64_t id, std::shared_ptr<const AcousticMeshData> mesh, const AcousticTransform& world);
    // Coalesced: only the newest transform per object reaches the worker.
    bool SetAcousticObjectTransform(std::uint64_t id, const AcousticTransform& world);
    bool RemoveAcousticObject(std::uint64_t id);
    bool ClearAcousticScene();

    // --- M7 probes: main thread. ---
    // Blocks until done. Needs Steam Audio Active, because the SDK objects need its context. A failure changes nothing and says why.
    ProbeGenerationResult GenerateProbes(const ProbeGenerationRequest& request) const;
    // As GenerateProbes, on a worker thread. One at a time: nullptr while another runs or when Steam Audio isn't active. Shutdown cancels
    // the job and waits for it, before the context goes; the returned object stays readable after that.
    std::shared_ptr<ProbeGenerationJob> StartProbeGeneration(ProbeGenerationRequest request);
    // `batch` must already have passed ProbeBatchAssets::Decode. Holds the batch until ClearProbeBatches or Shutdown; never adds it to the simulator.
    bool LoadProbeBatch(std::span<const std::byte> batch, std::size_t expectedProbes, std::string& error);
    void ClearProbeBatches() noexcept;
    ProbeBatchStats GetProbeBatchStats() const noexcept;

    // SA.3 diagnostics: zeros unless Steam Audio is Active.
    SteamAudioSimulationStats GetSteamAudioSimulationStats() const;
    SteamAudioMixerPoolStats GetSteamAudioMixerPoolStats() const;
    // SA.3 verification: Steam Audio's IPLContext (opaque), an immediate simulation tick, the source table and the
    // simulation inputs production does not use yet.
    void* GetSteamAudioContextForTesting() const noexcept;
    void RunSteamAudioSimulationForTesting() noexcept;
    std::vector<SteamAudioSourceSnapshot> GetSteamAudioSourcesForTesting() const;
    void SetSteamAudioTestControls(const SteamAudioSimulationTestControls& controls);
    // The opaque FMOD::System, and the worker's native thread handle (null without a worker).
    void* GetNativeCoreForTesting() const noexcept;
    void* GetSteamAudioWorkerThreadForTesting() const noexcept;
    // The authored reverb return's channel group; null unless the verification reverb is active and its bank is loaded.
    void* GetSteamAudioReverbGroupForTesting() const noexcept;
    // The opaque FMOD instance behind a handle, to match it against a SteamAudioSourceSnapshot; null when stale.
    void* GetNativeEventForTesting(EventHandle handle) const;

    // --- Banks (Audio.1B) ---------------------------------------------------------------------------------------
    // Banks are keyed by the caller's stable identity (the AudioBank asset reference, "asset:<uuid>"). Loading a key
    // that is already loaded adds a reference; each load needs one UnloadBank. Loads are synchronous: on success the
    // bank's metadata (events, buses) is available. Sample data loads on demand when events play. Every function
    // fails with an error while audio is not running (Disabled, Unavailable, NotStarted).

    // Development: FMOD reads the file at `path` (the file must stay in place while the bank is loaded; FMOD
    // streams from it). Fails without loading anything for a missing, unreadable or corrupt file, or when the same
    // bank is already loaded under a different key.
    bool LoadBankFile(std::string_view key, const std::filesystem::path& path, std::string& error);
    // Packaged games: FMOD copies `bytes` before returning (FMOD_STUDIO_LOAD_MEMORY), so the caller may free them at
    // once. Same reference and failure rules as LoadBankFile.
    bool LoadBankMemory(std::string_view key, std::span<const std::byte> bytes, std::string& error);
    // Releases one reference; the last one unloads the bank, which destroys every event instance of its events
    // (their handles become stale). False for a key that is not loaded.
    bool UnloadBank(std::string_view key);
    bool IsBankLoaded(std::string_view key) const;
    std::size_t GetLoadedBankCount() const noexcept;
    // Every event of one loaded bank, in FMOD's order; empty for an unknown key.
    std::vector<EventInfo> GetBankEvents(std::string_view key) const;
    // Audio.2B: every event of every loaded bank, each once, sorted by path (events without a path last, by GUID).
    std::vector<EventInfo> GetLoadedEvents() const;

    // --- Events (Audio.1B: internal; scripts reach them through Audio.1C's session API) -------------------------
    // Resolves "event:/..." to its GUID; needs the strings bank that names it.
    std::optional<Guid> FindEvent(std::string_view path) const;
    // True when a loaded bank defines the event.
    bool HasEvent(const Guid& event) const;
    // The event's path, or empty without a strings bank naming it (or for an unknown event).
    std::string GetEventPath(const Guid& event) const;

    // Creates one instance of a loaded event; AudioSystem tracks it until ReleaseEvent, its bank unloads or Shutdown.
    // The returned handle is the caller's only way to reach it.
    std::optional<EventHandle> CreateEventInstance(const Guid& event, std::string& error);
    // Before StartEvent. Asks for source-centric reflections on this instance's spatializers. False when the handle is stale or the route is off.
    bool SetEventSourceCentricReflections(EventHandle handle, bool enabled);
    // Taken up by the next reflection simulation, with no rebuild: bounces are an input to each run, not part of the simulator. Main thread.
    void SetSteamAudioReflectionBounces(int bounces) noexcept;
    // True once the event's timeline has reached its end and only effect tails keep it alive. A looping or sustaining event never gets
    // there, and an event with no timeline length never reports it.
    bool HasEventTimelineEnded(EventHandle handle) const;
    bool StartEvent(EventHandle handle);
    bool StopEvent(EventHandle handle, bool allowFadeOut);
    // Gives the instance back to FMOD, which destroys it once it has stopped (a looping event must be stopped
    // first). The handle is stale from this call on.
    bool ReleaseEvent(EventHandle handle);
    PlaybackState GetPlaybackState(EventHandle handle) const;
    // Tracked instances whose handles are still valid.
    std::size_t GetEventInstanceCount() const;

    // --- Production reflection route: one gameplay session at a time -------------------------------------------------
    // Holds the return bus and installs Engine's DSPs. Sends open only between these calls, so an empty return never passes dry signal.
    void BeginReflectionSession() noexcept;
    void EndReflectionSession() noexcept;
    // True silences the shared listener-centric response without closing sends, which keep the Mixer Return processing source-centric reflections.
    void SetListenerReflectionsWithheld(bool withheld) noexcept;

    // --- Audio.1C control ----------------------------------------------------------------------------------------
    // Volumes are linear gains in [0, MaximumVolume]; parameter values must be finite (FMOD clamps them to the
    // parameter's authored range). Invalid values and unknown names fail with an error and change nothing.
    static constexpr float MaximumVolume = 10.0f;

    // "{guid}" (FMOD Studio's GUID text) or "event:/path" (needs the strings bank) to an event of a loaded bank.
    std::optional<Guid> ResolveEventReference(std::string_view reference) const;
    bool SetEventParameter(EventHandle handle, std::string_view name, float value, std::string& error);
    bool SetEventVolume(EventHandle handle, float volume, std::string& error);

    // Buses ("bus:/..." or "{guid}"), VCAs ("vca:/..." or "{guid}") and global parameters (by name). Getters answer
    // the value last set through FMOD (the authored default until then); nullopt for an unknown object.
    std::optional<float> GetBusVolume(std::string_view bus) const;
    bool SetBusVolume(std::string_view bus, float volume, std::string& error);
    std::optional<float> GetVCAVolume(std::string_view vca) const;
    bool SetVCAVolume(std::string_view vca, float volume, std::string& error);
    std::optional<float> GetGlobalParameter(std::string_view name) const;
    bool SetGlobalParameter(std::string_view name, float value, std::string& error);

    // --- Audio.2A 3D --------------------------------------------------------------------------------------------
    // Whether a loaded event is spatialized (authored in FMOD Studio); false for unknown events.
    bool IsEvent3D(const Guid& event) const;
    // Fails for non-finite values or a forward/up pair that is not unit length and perpendicular.
    bool SetEventAttributes(EventHandle handle, const Attributes3D& attributes, std::string& error);
    std::optional<Attributes3D> GetEventAttributes(EventHandle handle) const;
    // FMOD listener 0, the only listener Engine uses.
    bool SetListenerAttributes(const Attributes3D& attributes, std::string& error);
    std::optional<Attributes3D> GetListenerAttributes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Runs AudioSystem::Update once when a frame scope ends, on every return path of the frame.
class FrameUpdateScope final
{
public:
    explicit FrameUpdateScope(AudioSystem& audio) noexcept : m_audio(audio) {}
    ~FrameUpdateScope() { m_audio.Update(); }
    FrameUpdateScope(const FrameUpdateScope&) = delete;
    FrameUpdateScope& operator=(const FrameUpdateScope&) = delete;

private:
    AudioSystem& m_audio;
};
}
