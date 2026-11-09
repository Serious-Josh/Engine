#include "Audio/AudioSystemInternal.h"

#if defined(ENGINE_HAS_STEAMAUDIO)
#include "Audio/SteamAudioCoordinates.h"
#include "Audio/SteamAudioSnapshotSource.h"
#include "Core/Log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <system_error>
#include <utility>

namespace Audio::Internal
{
namespace
{
    // How often source poses are simulated. Simulation only needs to follow gameplay, not the mixer.
    constexpr std::chrono::milliseconds SimulationInterval{50};

    // The simulator's occlusion sample limit; only used once simulation has a scene.
    constexpr int MaximumOcclusionSamples = 16;

    // M6.4: how often reflections simulate. Reflections change slowly and cost far more than direct simulation.
    constexpr std::chrono::milliseconds ReflectionInterval{150};

    // Direct work takes priority, but continual scene changes must not stop reflections for good.
    constexpr std::chrono::milliseconds ReflectionStarvationLimit{1000};

    // How often a deferred reflection frame looks again; a newer frame may have replaced it by then.
    constexpr std::chrono::milliseconds ReflectionDeferralPoll{5};

    // The settings M6.2 validated against the pinned plugin. Capacity is the SDK's maxNumSources, reported when exceeded.
    constexpr IPLReflectionEffectType ReflectionType = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    constexpr int ReflectionMaxRays = 4096;
    constexpr int ReflectionDiffuseSamples = 32;
    constexpr int ReflectionBounces = 8;
    constexpr float ReflectionMaxDuration = SteamAudioReflectionMaxDuration;
    constexpr int ReflectionMaxOrder = SteamAudioReflectionMaxOrder;
    constexpr int ReflectionMaxSources = 16;
    // M6.7: the local parameter an event authors to be able to use the reverb. 0 silent, 1 full.
    constexpr const char* ReverbSendParameter = "SteamAudioReverbSend";
    constexpr const char* ReflectionReturnBus = "bus:/SteamAudioReverb";
    // Steam Audio's own pool for tracing rays; the reflection worker only waits for it. Embree's device has a separate pool
    // for scene builds, which this does not change.
    constexpr int ReflectionThreads = 2;

    // M9.3: the pathing inputs M9.0 measured with. The order is the plugin's simulation order, the one it renders paths at.
    constexpr int PathingVisSamples = 4;
    constexpr float PathingVisRadius = 0.5f;
    constexpr float PathingVisThreshold = 0.1f;
    constexpr float PathingVisRange = 50.0f;
    constexpr int PathingOrder = ReflectionMaxOrder;
    // M9.5: how fast the pathing gain follows the direct path's visibility. Twice the 150 ms simulation cadence: no step between runs is heard.
    constexpr double PathingGainSeconds = 0.3;

    bool InsideAnyProbe(const std::vector<std::array<float, 4>>& spheres, const IPLVector3& point) noexcept
    {
        return std::any_of(spheres.begin(), spheres.end(), [&](const std::array<float, 4>& sphere)
        {
            const float x = point.x - sphere[0];
            const float y = point.y - sphere[1];
            const float z = point.z - sphere[2];
            return x * x + y * y + z * z <= sphere[3] * sphere[3];
        });
    }

    // Enough for the SDK to see a listener change, far too little to hear: restarts accumulation after a root is replaced.
    constexpr float AccumulationResetOffset = 0.001f;

    using Clock = std::chrono::steady_clock;

    IPLVector3 ToIPL(const SteamAudio::Vector& vector) noexcept
    {
        return IPLVector3{vector[0], vector[1], vector[2]};
    }

    // The one conversion path to Steam Audio space (SteamAudioCoordinates.h), from FMOD's values.
    IPLCoordinateSpace3 ToIPLCoordinateSpace(const FMOD_3D_ATTRIBUTES& attributes) noexcept
    {
        Attributes3D engine;
        engine.position = {attributes.position.x, attributes.position.y, attributes.position.z};
        engine.forward = {attributes.forward.x, attributes.forward.y, attributes.forward.z};
        engine.up = {attributes.up.x, attributes.up.y, attributes.up.z};

        const SteamAudio::CoordinateSpace space = SteamAudio::ToSteamAudioCoordinateSpace(engine);
        return IPLCoordinateSpace3{ToIPL(space.right), ToIPL(space.up), ToIPL(space.ahead), ToIPL(space.origin)};
    }

    double Microseconds(Clock::time_point start, Clock::time_point end)
    {
        return std::chrono::duration<double, std::micro>(end - start).count();
    }

    IPLSimulationInputs MakeInputs(const SteamAudioSimulationTestControls& controls) noexcept
    {
        IPLSimulationInputs inputs{};
        inputs.flags = IPL_SIMULATIONFLAGS_DIRECT;
        inputs.directFlags = static_cast<IPLDirectSimulationFlags>(controls.directFlags);
        inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_INVERSEDISTANCE;
        inputs.distanceAttenuationModel.minDistance = 1.0f;
        inputs.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
        inputs.occlusionType = controls.volumetricOcclusion ? IPL_OCCLUSIONTYPE_VOLUMETRIC : IPL_OCCLUSIONTYPE_RAYCAST;
        inputs.occlusionRadius = 1.0f;
        inputs.numOcclusionSamples = MaximumOcclusionSamples;
        inputs.numTransmissionRays = 8;
        return inputs;
    }

    // iplEmbreeDeviceCreate does not check rtcNewDevice, so its success proves nothing. A throwaway simulator traces one
    // instanced triangle, the way production uses Embree, and must get the known answer. Nothing here outlives the call.
    bool EmbreeGivesKnownAnswer(IPLContext context, IPLEmbreeDevice device, int sampleRate, int frameSize) noexcept
    {
        IPLSceneSettings sceneSettings{};
        sceneSettings.type = IPL_SCENETYPE_EMBREE;
        sceneSettings.embreeDevice = device;

        IPLSimulationSettings simulationSettings{};
        simulationSettings.flags = IPL_SIMULATIONFLAGS_DIRECT;
        simulationSettings.sceneType = IPL_SCENETYPE_EMBREE;
        simulationSettings.maxNumOcclusionSamples = MaximumOcclusionSamples;
        simulationSettings.samplingRate = sampleRate;
        simulationSettings.frameSize = frameSize;

        IPLVector3 vertices[3] = {{-4.0f, -4.0f, -2.5f}, {4.0f, -4.0f, -2.5f}, {0.0f, 4.0f, -2.5f}};
        IPLTriangle triangle{};
        triangle.indices[0] = 0;
        triangle.indices[1] = 1;
        triangle.indices[2] = 2;
        IPLint32 materialIndex = 0;
        IPLMaterial material{};
        material.transmission[0] = 0.5f;
        material.transmission[1] = 0.5f;
        material.transmission[2] = 0.5f;

        IPLStaticMeshSettings meshSettings{};
        meshSettings.numVertices = 3;
        meshSettings.numTriangles = 1;
        meshSettings.numMaterials = 1;
        meshSettings.vertices = vertices;
        meshSettings.triangles = &triangle;
        meshSettings.materialIndices = &materialIndex;
        meshSettings.materials = &material;

        IPLScene root = nullptr;
        IPLScene subScene = nullptr;
        IPLStaticMesh mesh = nullptr;
        IPLInstancedMesh instance = nullptr;
        IPLSimulator simulator = nullptr;
        IPLSource source = nullptr;
        bool correct = false;

        IPLSourceSettings sourceSettings{};
        sourceSettings.flags = IPL_SIMULATIONFLAGS_DIRECT;

        if(iplSimulatorCreate(context, &simulationSettings, &simulator) == IPL_STATUS_SUCCESS &&
            iplSceneCreate(context, &sceneSettings, &root) == IPL_STATUS_SUCCESS &&
            iplSceneCreate(context, &sceneSettings, &subScene) == IPL_STATUS_SUCCESS &&
            iplStaticMeshCreate(subScene, &meshSettings, &mesh) == IPL_STATUS_SUCCESS)
        {
            iplStaticMeshAdd(mesh, subScene);
            iplSceneCommit(subScene);

            IPLInstancedMeshSettings instanceSettings{};
            instanceSettings.subScene = subScene;
            instanceSettings.transform.elements[0][0] = 1.0f;
            instanceSettings.transform.elements[1][1] = 1.0f;
            instanceSettings.transform.elements[2][2] = 1.0f;
            instanceSettings.transform.elements[3][3] = 1.0f;

            if(iplInstancedMeshCreate(root, &instanceSettings, &instance) == IPL_STATUS_SUCCESS &&
                iplSourceCreate(simulator, &sourceSettings, &source) == IPL_STATUS_SUCCESS)
            {
                iplInstancedMeshAdd(instance, root);
                iplSceneCommit(root);
                iplSimulatorSetScene(simulator, root);
                iplSourceAdd(source, simulator);
                iplSimulatorCommit(simulator);

                IPLSimulationInputs inputs = MakeInputs(SteamAudioSimulationTestControls{});
                inputs.directFlags = static_cast<IPLDirectSimulationFlags>(
                    IPL_DIRECTSIMULATIONFLAGS_OCCLUSION | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
                inputs.source.origin = {0.0f, 0.0f, -5.0f};
                inputs.source.ahead = {0.0f, 0.0f, 1.0f};
                inputs.source.up = {0.0f, 1.0f, 0.0f};
                inputs.source.right = {-1.0f, 0.0f, 0.0f};
                iplSourceSetInputs(source, IPL_SIMULATIONFLAGS_DIRECT, &inputs);

                IPLSimulationSharedInputs shared{};
                shared.listener.origin = {0.0f, 0.0f, 0.0f};
                shared.listener.ahead = {0.0f, 0.0f, -1.0f};
                shared.listener.up = {0.0f, 1.0f, 0.0f};
                shared.listener.right = {1.0f, 0.0f, 0.0f};
                iplSimulatorSetSharedInputs(simulator, IPL_SIMULATIONFLAGS_DIRECT, &shared);
                iplSimulatorRunDirect(simulator);

                IPLSimulationOutputs outputs{};
                iplSourceGetOutputs(source, IPL_SIMULATIONFLAGS_DIRECT, &outputs);
                correct = outputs.direct.occlusion == 0.0f && std::abs(outputs.direct.transmission[1] - 0.5f) < 1e-5f;

                iplSourceRemove(source, simulator);
                iplSimulatorSetScene(simulator, nullptr);
                iplSimulatorCommit(simulator);
                iplInstancedMeshRemove(instance, root);
                iplSceneCommit(root);
            }

            iplStaticMeshRemove(mesh, subScene);
            iplSceneCommit(subScene);
        }

        iplSourceRelease(&source);
        iplInstancedMeshRelease(&instance);
        iplStaticMeshRelease(&mesh);
        iplSceneRelease(&subScene);
        iplSceneRelease(&root);
        iplSimulatorRelease(&simulator);
        return correct;
    }

    IPLMatrix4x4 ToIPLMatrix(const AcousticTransform& world) noexcept
    {
        const AcousticTransform converted = SteamAudio::ToSteamAudioTransform(world);
        IPLMatrix4x4 matrix{};

        for(int row = 0; row < 4; ++row)
        {
            for(int column = 0; column < 4; ++column)
            {
                matrix.elements[row][column] = converted[row][column];
            }
        }

        return matrix;
    }

    void InjectSimulationDelay(int milliseconds) noexcept
    {
        if(milliseconds > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        }
    }
}

SteamAudioSimulation::SteamAudioSimulation(const SteamAudioRuntime& runtime, FMOD::Studio::System& studio,
    FMOD::System& core) noexcept :
    m_runtime(runtime), m_studio(studio), m_core(core)
{
}

SteamAudioSimulation::~SteamAudioSimulation()
{
    Shutdown();
}

void SteamAudioSimulation::Start(const SteamAudioOptions& options)
{
    m_injectedFailure = options.injectedFailure;
    m_injectedFailuresRemaining = options.injectedFailureCount;

    // The undocumented path must be proven before any SnapshotSource can reach a production DSP. No bank is loaded yet, so
    // no DSP exists under either choice, and the choice then holds for the whole session. It comes first because the
    // ray tracer depends on it: the DSP lock path has no geometry, so it never brings Embree up.
    SnapshotCompatibility compatibility;

    if(options.injectedFailure == SteamAudioInjectedFailure::SimulatorCreation)
    {
        compatibility.detail = "the simulator could not be created";
    }
    else if(options.forceDspLockSynchronization)
    {
        compatibility.detail = "requested by the host";
    }
    else
    {
        compatibility = CheckSnapshotCompatibility(m_runtime, m_studio, m_core, options.injectedFailure);
    }

    m_stats.quarantinedSources += compatibility.quarantined;
    const bool startWorker = compatibility.accepted && options.injectedFailure != SteamAudioInjectedFailure::WorkerStart;
    SelectRayTracer(options, startWorker);

    IPLSimulationSettings settings{};
    settings.flags = IPL_SIMULATIONFLAGS_DIRECT;
    settings.sceneType = m_embreeDevice ? IPL_SCENETYPE_EMBREE : IPL_SCENETYPE_DEFAULT;
    settings.maxNumOcclusionSamples = MaximumOcclusionSamples;
    settings.samplingRate = m_runtime.GetSampleRate();
    settings.frameSize = m_runtime.GetBlockLength();

    // Off in production: nothing about reflections exists, in the simulator or the plugin, unless a verification session asks.
    bool routingWanted = options.reflectionRouting;
    m_reflectionRoutingRequested = options.reflectionRouting;
    m_reflectionBounces.store(options.reflectionBounces, std::memory_order_relaxed);

    if(routingWanted)
    {
        // Only stereo has been validated: other layouts crash (mono) or give unverified wet levels (5.1, 7.1). One condition, so wider support is one
        // change once it is validated. Direct audio is unaffected.
        int sampleRate = 0;
        int rawSpeakers = 0;
        FMOD_SPEAKERMODE speakerMode = FMOD_SPEAKERMODE_DEFAULT;

        if(m_core.getSoftwareFormat(&sampleRate, &speakerMode, &rawSpeakers) != FMOD_OK || speakerMode != FMOD_SPEAKERMODE_STEREO)
        {
            routingWanted = false;
            m_stats.reflectionInfrastructureDetail = "reflections support only a stereo mixer";
            Log::Warning("Audio: Steam Audio reflections are unavailable: the mixer is not stereo. Direct audio continues.");
        }
    }

    bool reflectionsWanted = routingWanted || options.reflectionInfrastructureForTesting || options.environmentalReverbForTesting;

    if(reflectionsWanted && !startWorker)
    {
        reflectionsWanted = false;
        routingWanted = false;
        m_stats.reflectionInfrastructureDetail = "reflections need the snapshot path";

        if(options.reflectionRouting)
        {
            Log::Warning("Audio: Steam Audio reflections are unavailable: they need the snapshot path. Direct audio continues.");
        }
    }

    m_productionRouting = routingWanted;

    const IPLSimulationSettings directSettings = settings;

    if(reflectionsWanted)
    {
        settings.flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS);
        settings.reflectionType = ReflectionType;
        settings.maxNumRays = ReflectionMaxRays;
        settings.numDiffuseSamples = ReflectionDiffuseSamples;
        settings.maxDuration = ReflectionMaxDuration;
        settings.maxOrder = ReflectionMaxOrder;
        // One more for reverb. Events still get ReflectionMaxSources.
        settings.maxNumSources = ReflectionMaxSources + (options.environmentalReverbForTesting || m_productionRouting ? 1 : 0);
        settings.numThreads = ReflectionThreads;
    }

    IPLerror status = IPL_STATUS_FAILURE;

    if(options.injectedFailure != SteamAudioInjectedFailure::SimulatorCreation)
    {
        status = iplSimulatorCreate(m_runtime.GetContext(), &settings, &m_simulator);
    }

    if(reflectionsWanted && status == IPL_STATUS_SUCCESS &&
        (options.injectedFailure == SteamAudioInjectedFailure::ReflectionCompatibility || !ReflectionDimensionsMatch(settings)))
    {
        // Never run on with disagreeing buffer sizes: direct simulation carries on with a simulator that has no reflections.
        reflectionsWanted = false;
        m_productionRouting = false;
        m_stats.reflectionInfrastructureDetail = "simulator and plugin reflection dimensions disagree";
        iplSimulatorRelease(&m_simulator);
        settings = directSettings;
        status = iplSimulatorCreate(m_runtime.GetContext(), &settings, &m_simulator);
    }

    if(status != IPL_STATUS_SUCCESS)
    {
        m_simulator = nullptr;
        ReleaseEmbree();
        m_stats.activeRayTracer = SteamAudioRayTracer::Default;
        m_stats.synchronizationDetail = "Steam Audio simulator creation failed (IPLerror " +
            std::to_string(static_cast<int>(status)) + ")";
        Log::Warning("Audio: " + m_stats.synchronizationDetail + "; Steam Audio effects run as direct HRTF.");
        return;
    }

    m_stats.simulatorActive = true;
    m_synchronization = SteamAudioSynchronization::DspLock;
    m_stats.synchronizationDetail = compatibility.detail;

    if(startWorker)
    {
        try
        {
            m_worker = std::thread(&SteamAudioSimulation::WorkerMain, this);
            m_synchronization = SteamAudioSynchronization::Snapshot;
        }
        catch(const std::system_error& error)
        {
            m_stats.synchronizationDetail = std::string("the simulation worker could not start: ") + error.what();
            // The DSP lock path has no geometry, so no scene of this simulator's type will ever be created.
            ReleaseEmbree();
            m_stats.activeRayTracer = SteamAudioRayTracer::Default;
            m_stats.rayTracerDetail = "the DSP lock path has no acoustic geometry";
            Log::Warning("Audio: Steam Audio ray tracer: requested Embree, none in use; " + m_stats.rayTracerDetail + ".");
        }
    }
    else if(compatibility.accepted)
    {
        m_stats.synchronizationDetail = "injected worker start failure";
    }

    m_stats.synchronization = m_synchronization;

    if(reflectionsWanted && m_synchronization == SteamAudioSynchronization::Snapshot)
    {
        StartReflectionInfrastructure(settings, options);
    }

    if(options.reflectionInfrastructureForTesting && !m_reflectionsActive)
    {
        Log::Warning("Audio: Steam Audio reflection infrastructure stays off: " + m_stats.reflectionInfrastructureDetail + ".");
    }

    // M9.3: before any bank, so before any source or command can reach it. The workers read it only through work posted after Start
    // returns, under m_mutex, which orders this write before them.
    if(options.pathing && m_reflectionsActive)
    {
        IPLSimulationSettings pathingSettings{};
        pathingSettings.flags = IPL_SIMULATIONFLAGS_PATHING;
        pathingSettings.sceneType = settings.sceneType;
        pathingSettings.maxOrder = PathingOrder;
        pathingSettings.numVisSamples = PathingVisSamples;
        pathingSettings.samplingRate = settings.samplingRate;
        pathingSettings.frameSize = settings.frameSize;

        if(iplSimulatorCreate(m_runtime.GetContext(), &pathingSettings, &m_pathingSimulator) != IPL_STATUS_SUCCESS)
        {
            m_pathingSimulator = nullptr;
            Log::Warning("Audio: the Steam Audio pathing simulator could not be created; pathing stays off.");
        }
        else
        {
            m_pathingValidation = options.pathingValidationForTesting;
            m_pathingSourceLimit = static_cast<std::size_t>(std::max(options.pathingSourceLimit, 0));
            m_pathingOcclusionSamples = std::clamp(options.pathingOcclusionSamples, 1, MaximumOcclusionSamples);
            m_stats.pathingActive = true;
            Log::Info("Audio: Steam Audio pathing is available for up to " + std::to_string(m_pathingSourceLimit) + " sources.");
        }
    }

    if(m_synchronization == SteamAudioSynchronization::Snapshot)
    {
        Log::Info("Audio: Steam Audio simulation uses snapshots; " + m_stats.synchronizationDetail + ".");
    }
    else
    {
        Log::Warning("Audio: Steam Audio simulation falls back to the DSP lock: " + m_stats.synchronizationDetail + ".");
    }
}

// What the plugin would compute for its convolution effect, against what a real source of this simulator reports. A throwaway
// source, never added to the simulator.
bool SteamAudioSimulation::ReflectionDimensionsMatch(const IPLSimulationSettings& settings) const noexcept
{
    IPLSourceSettings sourceSettings{};
    sourceSettings.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
    IPLSource source = nullptr;

    if(iplSourceCreate(m_simulator, &sourceSettings, &source) != IPL_STATUS_SUCCESS)
    {
        return false;
    }

    IPLSimulationOutputs outputs{};
    iplSourceGetOutputs(source, IPL_SIMULATIONFLAGS_REFLECTIONS, &outputs);
    iplSourceRelease(&source);

    const int channels = (settings.maxOrder + 1) * (settings.maxOrder + 1);
    const int samples = static_cast<int>(std::ceil(settings.maxDuration * static_cast<float>(settings.samplingRate)));
    return outputs.reflections.numChannels == channels && outputs.reflections.irSize == samples &&
        settings.samplingRate == m_runtime.GetSampleRate() && settings.frameSize == m_runtime.GetBlockLength();
}

// Before any bank, so before any DSP. The plugin call comes last and cannot fail after it: once made it holds until Terminate.
void SteamAudioSimulation::StartReflectionInfrastructure(const IPLSimulationSettings& settings, const SteamAudioOptions& options)
{
    m_assumeReflectionConsumers = options.assumeReflectionConsumersForTesting;
    m_withholdReflectionIr = options.withholdReflectionIrForTesting;

    try
    {
        if(options.injectedFailure == SteamAudioInjectedFailure::ReflectionWorkerStart)
        {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        }

        m_reflectionWorker = std::thread(&SteamAudioSimulation::ReflectionWorkerMain, this);
    }
    catch(const std::system_error& error)
    {
        m_stats.reflectionInfrastructureDetail = std::string("the reflection worker could not start: ") + error.what();
        return;
    }

    if(!m_runtime.SetPluginSimulationSettings(settings))
    {
        m_productionRouting = false;
        // The worker idles until shutdown: no frame is posted while reflections are inactive.
        m_stats.reflectionInfrastructureDetail = "the plugin has no iplFMODSetSimulationSettings";
        return;
    }

    m_reflectionSettings = settings;
    m_reflectionsActive = true;
    m_stats.reflectionInfrastructureActive = true;
    m_stats.reflectionSourceCapacity = ReflectionMaxSources;
    m_reverbWanted = options.environmentalReverbForTesting || m_productionRouting;
    m_reverbRouting = m_productionRouting ? SteamAudioReverbRoutingForTesting::Combined : options.reverbRoutingForTesting;
    m_holdReverbReturn = options.holdReverbReturnForTesting;

    if(m_productionRouting)
    {
        m_stats.reflectionRoutingActive = true;
        m_stats.reflectionInfrastructureDetail = "listener-centric reverb, with source-centric reflections on request";
        Log::Info("Audio: Steam Audio reflections are active.");
        return;
    }

    m_stats.reflectionInfrastructureDetail = m_withholdReflectionIr ? "verification session; the plugin gets a null IR" :
        "verification session; events authored with Apply Reflections are audible";
    Log::Warning("Audio: Steam Audio reflection infrastructure is active for verification.");
}

void SteamAudioSimulation::SelectRayTracer(const SteamAudioOptions& options, bool geometryAllowed)
{
    m_stats.requestedRayTracer = options.rayTracer;
    m_stats.activeRayTracer = SteamAudioRayTracer::Default;

    const auto report = [&](std::string detail, bool warn)
    {
        m_stats.rayTracerDetail = std::move(detail);
        const std::string line = std::string("Audio: Steam Audio ray tracer: requested ") + ToString(m_stats.requestedRayTracer) +
            ", active " + ToString(m_stats.activeRayTracer) + "; " + m_stats.rayTracerDetail + ".";

        if(warn)
        {
            Log::Warning(line);
        }
        else
        {
            Log::Info(line);
        }
    };

    if(options.rayTracer == SteamAudioRayTracer::Default)
    {
        report("Embree not requested", false);
        return;
    }

    if(!geometryAllowed)
    {
        report("Embree not started, because the DSP lock path has no acoustic geometry", true);
        return;
    }

    if(options.injectedFailure == SteamAudioInjectedFailure::SimulatorCreation)
    {
        report("Embree not started, because the simulator could not be created", true);
        return;
    }

    IPLEmbreeDeviceSettings deviceSettings{};
    IPLerror status = IPL_STATUS_INITIALIZATION;

    if(options.injectedFailure != SteamAudioInjectedFailure::EmbreeDevice)
    {
        status = iplEmbreeDeviceCreate(m_runtime.GetContext(), &deviceSettings, &m_embreeDevice);
    }

    if(status != IPL_STATUS_SUCCESS || !m_embreeDevice)
    {
        m_embreeDevice = nullptr;
        report("Embree device creation failed (IPLerror " + std::to_string(static_cast<int>(status)) + "); falling back for this session", true);
        return;
    }

    if(options.injectedFailure == SteamAudioInjectedFailure::EmbreeProbe ||
        !EmbreeGivesKnownAnswer(m_runtime.GetContext(), m_embreeDevice, m_runtime.GetSampleRate(), m_runtime.GetBlockLength()))
    {
        ReleaseEmbree();
        report("Embree gave a wrong answer on its startup test scene; falling back for this session", true);
        return;
    }

    m_stats.activeRayTracer = SteamAudioRayTracer::Embree;
    report("Embree initialized", false);
}

void SteamAudioSimulation::ReleaseEmbree() noexcept
{
    if(m_embreeDevice)
    {
        iplEmbreeDeviceRelease(&m_embreeDevice);
    }

    m_embreeDevice = nullptr;
}

IPLSceneSettings SteamAudioSimulation::MakeSceneSettings() const noexcept
{
    IPLSceneSettings settings{};
    settings.type = m_embreeDevice ? IPL_SCENETYPE_EMBREE : IPL_SCENETYPE_DEFAULT;
    settings.embreeDevice = m_embreeDevice;
    return settings;
}

void SteamAudioSimulation::Shutdown() noexcept
{
    if(!m_simulator)
    {
        return;
    }

    if(m_synchronization == SteamAudioSynchronization::Snapshot)
    {
        ShutdownSnapshotPath();
    }
    else
    {
        ShutdownLockPath();
    }

    iplSimulatorRelease(&m_simulator);
    // Last: every scene that uses it is gone with the simulator.
    ReleaseEmbree();
    m_stats.simulatorActive = false;
    m_stats.activeSources = 0;

    try
    {
        if(m_stats.reflectionRoutingActive)
        {
            Log::Info("Audio: Steam Audio reflections stopped; " + std::to_string(m_stats.reflectionSessions) + " sessions, " +
                std::to_string(m_stats.reverbSends) + " sends opened, reverb IR reads " + std::to_string(m_stats.reverbIrReads) +
                ", source-centric IR reads " + std::to_string(m_stats.reflectionIrReads - m_stats.reverbIrReads) + ", denied " + std::to_string(m_stats.sourceCentricDenied) +
                ", binding lost " + std::to_string(m_stats.reverbBindingLost) + ".");
        }

        Log::Info("Audio: Steam Audio simulation stopped; " + std::to_string(m_stats.sourcesCreated) +
            " sources created, " + std::to_string(m_stats.sourcesDestroyed) + " destroyed; private: " +
            std::to_string(m_stats.privateSourcesCreated) + " created, " + std::to_string(m_stats.privateSourcesRemoved) +
            " removed, " + std::to_string(m_stats.privateSourcesReleased) + " released.");

        if(m_stats.quarantinedSources > 0)
        {
            Log::Warning("Audio: " + std::to_string(m_stats.quarantinedSources) + " Steam Audio sources were deliberately "
                "leaked because the plugin never released them.");
        }
    }
    catch(...)
    {
        // Logging allocation failure only.
    }
}

void SteamAudioSimulation::StartEnvironmentalReverb() noexcept
{
    if(!m_reverbWanted || !m_reflectionsActive)
    {
        m_reverbWanted = false;
        return;
    }

    auto* const snapshot = new(std::nothrow) SnapshotSource();

    if(!snapshot || !m_runtime.SetPluginReverbSource(snapshot->AsIPLSource()))
    {
        delete snapshot;
        m_reverbWanted = false;
        Log::Warning("Audio: the Steam Audio plugin has no reverb source call; reverb stays off.");
        return;
    }

    m_reverbSnapshot = snapshot;

    {
        const std::lock_guard lock(m_mutex);
        WorkerCommand command;
        command.kind = WorkerCommand::Kind::AddReverbSource;
        command.snapshot = snapshot;
        m_pendingCommands.push_back(std::move(command));
        m_stats.environmentalReverbActive = true;
        ++m_stats.sourcesCreated;
    }

    m_workerWake.notify_one();

    if(!m_productionRouting)
    {
        Log::Warning("Audio: Steam Audio environmental reverb is active for verification.");
    }
}

namespace
{
    bool GroupHoldsEffect(FMOD::ChannelGroup& group, std::string_view effect)
    {
        int count = 0;
        group.getNumDSPs(&count);

        for(int index = 0; index < count; ++index)
        {
            FMOD::DSP* dsp = nullptr;
            char name[64]{};

            if(group.getDSP(index, &dsp) == FMOD_OK && dsp->getInfo(name, nullptr, nullptr, nullptr) == FMOD_OK && effect == name)
            {
                return true;
            }
        }

        return false;
    }

    // The values the validated listening banks authored. From Final Out: the Mixer Return adds its input to its output and assumes they match.
    void ConfigureReturnEffect(FMOD::DSP& dsp)
    {
        int count = 0;
        dsp.getNumParameters(&count);

        for(int index = 0; index < count; ++index)
        {
            FMOD_DSP_PARAMETER_DESC* description = nullptr;

            if(dsp.getParameterInfo(index, &description) != FMOD_OK)
            {
                continue;
            }

            if(std::string_view(description->name) == "OutputFormat")
            {
                dsp.setParameterInt(index, 1);
            }
            else if(std::string_view(description->name) == "Binaural")
            {
                dsp.setParameterBool(index, true);
            }
        }
    }
}

// The bus is locked first: an idle return has no channel group, and FMOD frees it, and any effect on it, when nothing feeds it.
void SteamAudioSimulation::BeginReflectionSession() noexcept
{
    if(!m_productionRouting || m_returnBus)
    {
        return;
    }

    {
        const std::lock_guard lock(m_mutex);
        ++m_stats.reflectionSessions;
    }

    FMOD::Studio::Bus* bus = nullptr;

    if(m_studio.getBus(ReflectionReturnBus, &bus) != FMOD_OK)
    {
        Log::Warning("Audio: the banks have no bus:/SteamAudioReverb; Steam Audio reflections are silent and direct audio continues.");
        return;
    }

    if(bus->lockChannelGroup() != FMOD_OK)
    {
        Log::Warning("Audio: the Steam Audio reflection return cannot be held; reflections are silent and direct audio continues.");
        return;
    }

    m_returnBus = bus;
    m_studio.flushCommands();

    if(m_returnBus->getChannelGroup(&m_returnGroup) != FMOD_OK)
    {
        Log::Warning("Audio: the Steam Audio reflection return has no channel group; reflections are silent and direct audio continues.");
        EndReflectionSession();
        return;
    }

    // Banks that author these effects, as the listening fixtures do, keep them: a second set would process twice.
    const bool authored = GroupHoldsEffect(*m_returnGroup, "Steam Audio Reverb") || GroupHoldsEffect(*m_returnGroup, "Steam Audio Mixer Return");

    if(!authored)
    {
        if(!m_returnReverb)
        {
            m_returnReverb = m_runtime.CreateEffectByName(m_core, "Steam Audio Reverb");
        }

        if(!m_returnMixer)
        {
            m_returnMixer = m_runtime.CreateEffectByName(m_core, "Steam Audio Mixer Return");
        }

        if(!m_returnReverb || !m_returnMixer)
        {
            Log::Warning("Audio: the Steam Audio return effects cannot be created; reflections are silent and direct audio continues.");
            EndReflectionSession();
            return;
        }

        ConfigureReturnEffect(*m_returnReverb);
        ConfigureReturnEffect(*m_returnMixer);

        // The group's last DSP is the bus input, and the effects must be downstream of it: anything past it hears nothing, and the send leaks dry.
        int count = 0;
        m_returnGroup->getNumDSPs(&count);
        const int inputIndex = std::max(count - 1, 0);

        if(m_returnGroup->addDSP(inputIndex, m_returnMixer) != FMOD_OK || m_returnGroup->addDSP(inputIndex + 1, m_returnReverb) != FMOD_OK)
        {
            Log::Warning("Audio: the Steam Audio return effects cannot be installed; reflections are silent and direct audio continues.");
            m_returnDspsAdded = true;
            EndReflectionSession();
            return;
        }

        m_returnDspsAdded = true;
    }

    m_returnInstalled = true;
    m_returnBus->setMute(false);
    Log::Info("Audio: Steam Audio reflections use " + std::to_string(m_reflectionBounces.load(std::memory_order_relaxed)) + " bounces.");
    m_studio.flushCommands();
    const std::lock_guard lock(m_mutex);
    m_stats.reflectionReturnInstalled = true;
}

void SteamAudioSimulation::SetReflectionBounces(int bounces) noexcept
{
    m_reflectionBounces.store(bounces, std::memory_order_relaxed);
}

void SteamAudioSimulation::MuteReturnUntilInstalled() noexcept
{
    FMOD::Studio::Bus* bus = nullptr;

    // Asked for each new event: a reloaded bank brings a new, unmuted bus.
    if(m_reflectionRoutingRequested && !m_returnInstalled && m_studio.getBus(ReflectionReturnBus, &bus) == FMOD_OK)
    {
        bus->setMute(true);
    }
}

void SteamAudioSimulation::EndReflectionSession() noexcept
{
    if(!m_returnBus)
    {
        return;
    }

    // Before the effects leave, so a send never reaches an empty return.
    if(m_returnBus->isValid())
    {
        m_returnBus->setMute(true);
        m_studio.flushCommands();
    }

    if(m_returnDspsAdded && m_returnGroup)
    {
        m_returnGroup->removeDSP(m_returnReverb);
        m_returnGroup->removeDSP(m_returnMixer);
    }

    m_returnDspsAdded = false;
    m_returnInstalled = false;
    m_returnGroup = nullptr;

    if(m_returnBus->isValid())
    {
        m_returnBus->unlockChannelGroup();
        m_studio.flushCommands();
    }

    m_returnBus = nullptr;
    SetListenerReflectionsWithheld(false);
    const std::lock_guard lock(m_mutex);
    m_stats.reflectionReturnInstalled = false;
}

void SteamAudioSimulation::ReleaseReflectionReturn() noexcept
{
    EndReflectionSession();

    if(m_returnReverb)
    {
        m_returnReverb->release();
        m_returnReverb = nullptr;
    }

    if(m_returnMixer)
    {
        m_returnMixer->release();
        m_returnMixer = nullptr;
    }
}

void SteamAudioSimulation::SetListenerReflectionsWithheld(bool withheld) noexcept
{
    {
        const std::lock_guard lock(m_withholdMutex);
        m_listenerWithheld = withheld;
    }

    ApplyReverbWithhold();
}

void SteamAudioSimulation::SetBakedUncovered(bool uncovered) noexcept
{
    {
        const std::lock_guard lock(m_withholdMutex);

        if(m_bakedUncovered == uncovered)
        {
            return;
        }

        m_bakedUncovered = uncovered;
    }

    ApplyReverbWithhold();
}

// Either thread. The snapshot outlives both workers (it is retired only after they are joined).
void SteamAudioSimulation::ApplyReverbWithhold() noexcept
{
    const std::lock_guard lock(m_withholdMutex);
    SnapshotSource* const snapshot = m_reverbSnapshot ? m_reverbSnapshot : m_workerReverbSnapshot;

    if(snapshot)
    {
        snapshot->SetIrWithheld(m_listenerWithheld || m_bakedUncovered);
    }
}

bool SteamAudioSimulation::SetBakedReverb(std::vector<BakedReverbBatch> batches, bool listener) noexcept
{
    if(!m_reflectionsActive || !m_reverbSnapshot || !m_worker.joinable())
    {
        ReleaseBakedBatches(batches);
        return false;
    }

    std::shared_ptr<std::vector<BakedReverbBatch>> payload;

    try
    {
        payload = std::make_shared<std::vector<BakedReverbBatch>>(std::move(batches));
        const bool baked = !payload->empty() && listener;

        // Silent from now until a baked run has covered the listener, so no earlier realtime IR is heard as baked.
        if(baked)
        {
            SetBakedUncovered(true);
        }

        {
            const std::lock_guard lock(m_mutex);
            WorkerCommand command;
            command.kind = WorkerCommand::Kind::SetBakedReverb;
            command.baked = std::move(payload);
            command.bakedListener = listener;
            m_pendingCommands.push_back(std::move(command));
        }

        m_workerWake.notify_one();
        return true;
    }
    catch(...)
    {
        ReleaseBakedBatches(payload ? *payload : batches);
        return false;
    }
}

void SteamAudioSimulation::ReleaseBakedBatches(std::vector<BakedReverbBatch>& batches) noexcept
{
    for(BakedReverbBatch& entry : batches)
    {
        if(entry.batch)
        {
            iplProbeBatchRelease(&entry.batch);
            const std::lock_guard lock(m_mutex);
            ++m_stats.bakedReverbBatchesReleased;
        }
    }

    batches.clear();
}

void SteamAudioSimulation::SetPathingBatches(std::vector<BakedReverbBatch> batches) noexcept
{
    const auto release = [](std::vector<BakedReverbBatch>& entries)
    {
        for(BakedReverbBatch& entry : entries)
        {
            if(entry.batch)
            {
                iplProbeBatchRelease(&entry.batch);
            }
        }

        entries.clear();
    };

    if(!m_pathingSimulator || !m_worker.joinable())
    {
        release(batches);
        return;
    }

    std::shared_ptr<std::vector<BakedReverbBatch>> payload;

    try
    {
        payload = std::make_shared<std::vector<BakedReverbBatch>>(std::move(batches));
        const std::lock_guard lock(m_mutex);
        WorkerCommand command;
        command.kind = WorkerCommand::Kind::SetPathingBatch;
        command.pathingBatches = payload;
        m_pendingCommands.push_back(std::move(command));
    }
    catch(...)
    {
        release(payload ? *payload : batches);
        return;
    }

    m_workerWake.notify_one();
}

// Under the exclusive gate, so no pathing run is using the old set. The pathing simulator alone: a batch without the REVERB layer next to
// one with it crashes reflection runs (M9.0). Removal and the commit come before any release. Sources choose their batch per run, by index,
// so every selection is reset here: none may outlive the set it indexed.
void SteamAudioSimulation::WorkerSetPathingBatches(std::vector<BakedReverbBatch>& batches) noexcept
{
    std::vector<BakedReverbBatch> previous = std::move(m_pathingBatches);
    m_pathingBatches.clear();

    for(BakedReverbBatch& entry : previous)
    {
        iplSimulatorRemoveProbeBatch(m_pathingSimulator, entry.batch);
    }

    for(BakedReverbBatch& entry : batches)
    {
        iplSimulatorAddProbeBatch(m_pathingSimulator, entry.batch);
    }

    iplSimulatorCommit(m_pathingSimulator);

    for(BakedReverbBatch& entry : previous)
    {
        iplProbeBatchRelease(&entry.batch);
    }

    m_pathingBatches = std::move(batches);

    for(auto& [id, source] : m_privateSources)
    {
        source.pathingBatch = -1;
        source.pathingGain = 0.0f;
    }

    const std::lock_guard lock(m_mutex);
    m_stats.pathingBatchRegistered = !m_pathingBatches.empty();
    m_stats.pathingBatches = m_pathingBatches.size();
}

// Direct worker, exclusive gate. The pathing source starts with no batch and no gain, so its first result fades in from silence.
bool SteamAudioSimulation::CreatePathingSource(PrivateSource& source) noexcept
{
    IPLSourceSettings settings{};
    settings.flags = IPL_SIMULATIONFLAGS_PATHING;

    if(iplSourceCreate(m_pathingSimulator, &settings, &source.pathingSource) != IPL_STATUS_SUCCESS)
    {
        source.pathingSource = nullptr;
        return false;
    }

    iplSourceAdd(source.pathingSource, m_pathingSimulator);
    source.snapshot->SetPathingCapacity(SnapshotSource::PathingCapacity::Simulating);
    source.pathingBatch = -1;
    source.pathingGain = 0.0f;
    source.pathingGainAt = {};
    ++m_pathingSourceCount;
    const std::lock_guard lock(m_mutex);
    ++m_stats.pathingSourcesCreated;
    return true;
}

// Direct worker, exclusive gate: freed capacity goes to the longest-waiting opted-in source still alive. No source is ever preempted, so
// nothing churns: a source keeps its pathing from creation until it retires.
void SteamAudioSimulation::ServePathingQueue(bool& commit) noexcept
{
    while(m_pathingSourceCount < m_pathingSourceLimit && !m_pathingQueue.empty())
    {
        const SourceId id = m_pathingQueue.front();
        m_pathingQueue.erase(m_pathingQueue.begin());
        const auto found = m_privateSources.find(id);

        if(found != m_privateSources.end() && !found->second.pathingSource && CreatePathingSource(found->second))
        {
            commit = true;
        }
    }

    const std::lock_guard lock(m_mutex);
    m_stats.pathingSources = m_pathingSourceCount;
    m_stats.pathingSourcesWaiting = m_pathingQueue.size();
}

// Under the exclusive gate: no reflection run can be using the old set. Removal and the commit come before any release.
void SteamAudioSimulation::WorkerSetBakedReverb(std::vector<BakedReverbBatch>& batches, bool& commit) noexcept
{
    std::vector<BakedReverbBatch> previous = std::move(m_bakedReverb);
    m_bakedReverb.clear();

    for(BakedReverbBatch& entry : previous)
    {
        iplSimulatorRemoveProbeBatch(m_simulator, entry.batch);
    }

    for(BakedReverbBatch& entry : batches)
    {
        iplSimulatorAddProbeBatch(m_simulator, entry.batch);
    }

    iplSimulatorCommit(m_simulator);
    static_cast<void>(commit);
    const std::size_t removedCount = previous.size();
    ReleaseBakedBatches(previous);
    const bool baked = !batches.empty();
    m_bakedReverb = std::move(batches);

    {
        const std::lock_guard lock(m_mutex);
        m_stats.bakedReverbBatchesRemoved += removedCount;
        m_stats.bakedReverbBatches = m_bakedReverb.size();
        m_stats.bakedReverbBatchesAdded += m_bakedReverb.size();

        if(!baked)
        {
            m_stats.bakedReverbCovered = false;
        }
    }

    // Back to realtime: the reverb source's next realtime run starts its accumulation afresh.
    if(!baked || !m_bakedListener)
    {
        m_reflectionAccumulationStale.store(true, std::memory_order_relaxed);
        SetBakedUncovered(false);
    }
}

bool SteamAudioSimulation::SetSourceCentricRequest(FMOD::Studio::EventInstance& instance, bool enabled, const IPLBakedDataIdentifier* baked)
{
    if(!m_productionRouting)
    {
        return false;
    }

    const std::lock_guard lock(m_mutex);
    std::erase(m_sourceCentricRequests, &instance);
    std::erase_if(m_bakedRequests, [&](const auto& request)
    {
        return request.first == &instance;
    });

    if(enabled)
    {
        m_sourceCentricRequests.push_back(&instance);

        if(baked)
        {
            m_bakedRequests.emplace_back(&instance, *baked);
        }
    }

    return true;
}

bool SteamAudioSimulation::SetPathingRequest(FMOD::Studio::EventInstance& instance, bool enabled)
{
    if(!m_pathingSimulator)
    {
        return false;
    }

    const std::lock_guard lock(m_mutex);
    std::erase(m_pathingRequests, &instance);

    if(enabled)
    {
        m_pathingRequests.push_back(&instance);
    }

    return true;
}

bool SteamAudioSimulation::DisableBakedReflections(FMOD::Studio::EventInstance& instance) noexcept
{
    try
    {
        const std::lock_guard lock(m_mutex);

        for(auto& [dsp, active] : m_activeSources)
        {
            if(active.instance == &instance && active.baked)
            {
                WorkerCommand command;
                command.kind = WorkerCommand::Kind::DisableBakedSource;
                command.id = active.id;
                m_pendingCommands.push_back(std::move(command));
                active.baked.reset();
                ++m_stats.bakedSourcesDisabled;
                m_workerWake.notify_one();
                return true;
            }
        }

        // Not bound yet: withdraw the request whole. Without its baked layer it must not become realtime.
        const bool requested = std::erase_if(m_bakedRequests, [&](const auto& request)
        {
            return request.first == &instance;
        }) > 0;

        if(requested)
        {
            std::erase(m_sourceCentricRequests, &instance);
            ++m_stats.bakedSourcesDisabled;
        }

        return requested;
    }
    catch(...)
    {
        return false;
    }
}

void* SteamAudioSimulation::GetReverbGroupForTesting() noexcept
{
    FMOD::Studio::Bus* bus = nullptr;
    FMOD::ChannelGroup* group = nullptr;

    if(!m_reverbSnapshot || m_studio.getBus("bus:/SteamAudioReverb", &bus) != FMOD_OK)
    {
        return nullptr;
    }

    if(!m_reverbGroupLocked)
    {
        m_reverbGroupLocked = bus->lockChannelGroup() == FMOD_OK;
        m_studio.flushCommands();
    }

    return bus->getChannelGroup(&group) == FMOD_OK ? group : nullptr;
}

// Worker. Gives free slots to waiting events, oldest first, and reports the rest once, so they fall back. A slot is a new source
// because flags are fixed at creation; the gate is held exclusively here, so no reflection run sees the change half made.
void SteamAudioSimulation::ServeReflectionSlots(bool& commit) noexcept
{
    std::vector<std::pair<SourceId, bool>> reports;
    auto waiting = m_slotQueue.begin();

    while(waiting != m_slotQueue.end())
    {
        const auto found = m_privateSources.find(waiting->first);

        if(found == m_privateSources.end())
        {
            waiting = m_slotQueue.erase(waiting);
            continue;
        }

        if(m_reflectionSourceCount >= static_cast<std::size_t>(ReflectionMaxSources))
        {
            if(!waiting->second)
            {
                waiting->second = true;
                reports.emplace_back(waiting->first, false);
            }

            ++waiting;
            continue;
        }

        IPLSourceSettings settings{};
        settings.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
        IPLSource source = nullptr;

        if(iplSourceCreate(m_simulator, &settings, &source) != IPL_STATUS_SUCCESS)
        {
            if(!waiting->second)
            {
                waiting->second = true;
                reports.emplace_back(waiting->first, false);
            }

            ++waiting;
            continue;
        }

        iplSourceAdd(source, m_simulator);
        commit = true;
        IPLSimulationOutputs outputs{};
        iplSourceGetOutputs(source, IPL_SIMULATIONFLAGS_REFLECTIONS, &outputs);

        // A baked slot is silent until a baked run covers the listener (RunReflections decides it after each run).
        if(found->second.baked)
        {
            found->second.snapshot->SetIrWithheld(true);
            const std::lock_guard lock(m_mutex);
            ++m_stats.bakedSourceSlots;
        }

        found->second.snapshot->PublishReflectionIr(outputs.reflections.ir, outputs.reflections.numChannels, outputs.reflections.irSize);
        found->second.reflectionSource = source;
        found->second.reflectionCapable = true;
        ++m_reflectionSourceCount;
        reports.emplace_back(waiting->first, true);
        waiting = m_slotQueue.erase(waiting);
    }

    if(!reports.empty())
    {
        const std::lock_guard lock(m_mutex);
        m_slotReports.insert(m_slotReports.end(), reports.begin(), reports.end());
        m_stats.reflectionSources = m_reflectionSourceCount;
    }
}

namespace
{
    bool GroupHoldsSend(FMOD::ChannelGroup& group) noexcept
    {
        int count = 0;
        group.getNumDSPs(&count);

        for(int index = 0; index < count; ++index)
        {
            FMOD::DSP* dsp = nullptr;
            FMOD_DSP_TYPE type = FMOD_DSP_TYPE_UNKNOWN;

            if(group.getDSP(index, &dsp) == FMOD_OK && dsp->getType(&type) == FMOD_OK && type == FMOD_DSP_TYPE_SEND)
            {
                return true;
            }
        }

        int groups = 0;
        group.getNumGroups(&groups);

        for(int index = 0; index < groups; ++index)
        {
            FMOD::ChannelGroup* child = nullptr;

            if(group.getGroup(index, &child) == FMOD_OK && child && GroupHoldsSend(*child))
            {
                return true;
            }
        }

        return false;
    }
}

// Main thread. The send closes as reflections open, and the reverse, with RampReverbSends making it gradual.
void SteamAudioSimulation::ApplySlotReports() noexcept
{
    std::vector<std::pair<SourceId, bool>> reports;

    {
        const std::lock_guard lock(m_mutex);
        reports.swap(m_slotReports);
    }

    for(const auto& [id, assigned] : reports)
    {
        for(auto& [dsp, active] : m_activeSources)
        {
            if(active.id != id)
            {
                continue;
            }

            active.slotAssigned = assigned;

            if(m_productionRouting && !assigned && !active.slotDenied)
            {
                active.slotDenied = true;
                const std::lock_guard lock(m_mutex);
                ++m_stats.sourceCentricDenied;
                Log::Warning("Audio: source-centric reflections are unavailable for an event: all " + std::to_string(ReflectionMaxSources) +
                    " slots are in use. It keeps listener-centric reflections, and gets a slot if one frees.");
            }

            if(assigned)
            {
                active.slotDenied = false;
                m_runtime.SetAppliesReflections(*dsp, true);
                const std::lock_guard lock(m_mutex);
                m_stats.reflectionSlotPromotions += active.sendOpened ? 1 : 0;
            }

            active.reverbSendTarget = assigned && m_reverbRouting != SteamAudioReverbRoutingForTesting::Combined ? 0.0f : 1.0f;
            break;
        }
    }
}

// Main thread, every Update. Only for events authored with the optional SteamAudioReverbSend parameter, from before fixed sends were
// supported: moves it towards its target. An ordinary send needs none of this, as the return's mute covers its one unsafe state. Writing
// the Send DSP's own level would not work: it reads back as set and changes nothing.
void SteamAudioSimulation::RampReverbSends() noexcept
{
    constexpr float RampSeconds = 0.15f;
    const auto now = Clock::now();
    const float step = std::chrono::duration<float>(now - m_lastReverbRamp).count() / RampSeconds;
    m_lastReverbRamp = now;
    std::size_t slots = 0;
    std::size_t fallback = 0;
    std::size_t denied = 0;
    std::uint64_t opened = 0;
    std::uint64_t withoutSend = 0;

    for(auto& [dsp, active] : m_activeSources)
    {
        slots += active.slotAssigned ? 1 : 0;
        denied += active.slotDenied ? 1 : 0;

        if(!active.sendSearchReported && active.instance->isValid())
        {
            active.sendSearchReported = true;
            FMOD_STUDIO_PARAMETER_DESCRIPTION description{};
            FMOD::Studio::EventDescription* eventDescription = nullptr;
            active.hasReverbSend = active.instance->getDescription(&eventDescription) == FMOD_OK &&
                eventDescription->getParameterDescriptionByName(ReverbSendParameter, &description) == FMOD_OK;
            withoutSend += active.hasReverbSend ? 0 : 1;
        }

        if(!active.hasReverbSend)
        {
            continue;
        }

        // An empty return passes what it receives, so a send may open only once the effects are on it.
        const float target = m_productionRouting && !m_returnInstalled ? 0.0f : active.reverbSendTarget;
        fallback += target > 0.0f ? 1 : 0;

        if(active.reverbSendLevel != target && active.instance->isValid())
        {
            const float distance = target - active.reverbSendLevel;
            active.reverbSendLevel += std::clamp(distance, -step, step);
            active.instance->setParameterByName(ReverbSendParameter, active.reverbSendLevel, true);

            if(target > 0.0f && !active.sendOpened)
            {
                active.sendOpened = true;
                ++opened;
            }
        }
    }

    const std::lock_guard lock(m_mutex);

    if(m_stats.sourceCentricWithoutSend > 0 && !m_warnedSourceCentricWithoutSend)
    {
        m_warnedSourceCentricWithoutSend = true;
        Log::Warning("Audio: Source-Centric Reflections is on for an event with no Send, so it gets no reflections. Add a Send to SteamAudioReverb in FMOD Studio.");
    }

    m_stats.reverbFallbackEvents = fallback;
    m_stats.reflectionSlotEvents = slots;
    m_stats.sourceCentricUnavailableEvents = denied;
    m_stats.reverbSends += opened;
    m_stats.reverbEventsWithoutSend += withoutSend;
}

void SteamAudioSimulation::ShutdownSnapshotPath() noexcept
{
    // No FMOD thread remains. Report what the Studio thread left, so every source is on the retiring list.
    ApplyLifecycleChanges();

    {
        const std::lock_guard lock(m_mutex);
        m_workerStop = true;
    }

    // Without m_mutex: the worker needs it to finish. Cancellation waits for at most the RunDirect in progress.
    m_workerWake.notify_all();
    m_reflectionWake.notify_all();
    m_worker.join();

    if(m_reflectionWorker.joinable())
    {
        m_reflectionWorker.join();
    }

    m_reflectionsActive = false;
    m_stats.reflectionInfrastructureActive = false;
    m_stats.reflectionSources = 0;

    // M9.3: both workers are joined. Pathing sources and the batches leave the pathing simulator, which is committed, before any release;
    // then the simulator itself, before the scene it holds is released below. Batches still in a command were never registered.
    if(m_pathingSimulator)
    {
        for(auto& [id, privateSource] : m_privateSources)
        {
            if(privateSource.pathingSource)
            {
                iplSourceRemove(privateSource.pathingSource, m_pathingSimulator);
            }
        }

        for(BakedReverbBatch& entry : m_pathingBatches)
        {
            iplSimulatorRemoveProbeBatch(m_pathingSimulator, entry.batch);
        }

        iplSimulatorCommit(m_pathingSimulator);

        for(auto& [id, privateSource] : m_privateSources)
        {
            if(privateSource.pathingSource)
            {
                iplSourceRelease(&privateSource.pathingSource);
                ++m_stats.pathingSourcesReleased;
            }
        }

        for(BakedReverbBatch& entry : m_pathingBatches)
        {
            iplProbeBatchRelease(&entry.batch);
        }

        m_pathingBatches.clear();

        for(WorkerCommand& command : m_pendingCommands)
        {
            if(command.kind == WorkerCommand::Kind::SetPathingBatch && command.pathingBatches)
            {
                for(BakedReverbBatch& entry : *command.pathingBatches)
                {
                    iplProbeBatchRelease(&entry.batch);
                }

                command.pathingBatches->clear();
            }
        }

        iplSimulatorRelease(&m_pathingSimulator);
        m_pathingSourceCount = 0;
        m_pathingQueue.clear();
        m_stats.pathingActive = false;
        m_stats.pathingBatchRegistered = false;
        m_stats.pathingBatches = 0;
        m_stats.pathingSources = 0;
        m_stats.pathingSourcesWaiting = 0;
    }

    // M8.4: both workers are joined. Registered batches leave the simulator before their release; unapplied ones were never in it.
    for(BakedReverbBatch& entry : m_bakedReverb)
    {
        iplSimulatorRemoveProbeBatch(m_simulator, entry.batch);
        ++m_stats.bakedReverbBatchesRemoved;
    }

    if(!m_bakedReverb.empty())
    {
        iplSimulatorCommit(m_simulator);
    }

    ReleaseBakedBatches(m_bakedReverb);
    m_stats.bakedReverbBatches = 0;

    for(WorkerCommand& command : m_pendingCommands)
    {
        if(command.kind == WorkerCommand::Kind::SetBakedReverb && command.baked)
        {
            ReleaseBakedBatches(*command.baked);
        }
    }

    m_workerReverbSnapshot = nullptr;

    for(auto& [dsp, active] : m_activeSources)
    {
        m_retiringSources.push_back({active.snapshot, active.id, true, nullptr});
    }

    m_activeSources.clear();

    // The join hands everything the worker held to this thread: acknowledgements it sent, and sources it never got to remove.
    AcceptWorkerRemovals();

    for(auto& [id, privateSource] : m_privateSources)
    {
        if(!privateSource.realSource)
        {
            continue;
        }

        iplSourceRemove(privateSource.realSource, m_simulator);
        ++m_stats.privateSourcesRemoved;

        if(privateSource.reflectionSource)
        {
            iplSourceRemove(privateSource.reflectionSource, m_simulator);
        }

        for(RetiringSource& retiring : m_retiringSources)
        {
            if(retiring.id == id)
            {
                retiring.privateSource = privateSource.realSource;
                retiring.privateReflectionSource = privateSource.reflectionSource;
            }
        }
    }

    m_privateSources.clear();

    // The DSP went with the graph; the plugin's own reference goes now, and the usual retirement does the rest.
    if(m_reverbSnapshot)
    {
        m_runtime.SetPluginReverbSource(nullptr);

        if(m_reverbRealSource)
        {
            iplSourceRemove(m_reverbRealSource, m_simulator);
            ++m_stats.privateSourcesRemoved;
        }

        m_stats.reverbIrReads = static_cast<std::uint64_t>(m_reverbSnapshot->IrReads());
        m_retiringSources.push_back({m_reverbSnapshot, 0, true, m_reverbRealSource});
        m_reverbSnapshot = nullptr;
        m_reverbRealSource = nullptr;
    }

    m_stats.environmentalReverbActive = false;
    ReleaseAcousticScene();
    iplSimulatorCommit(m_simulator);

    for(RetiringSource& retiring : m_retiringSources)
    {
        retiring.workerDone = true;
    }

    // FMOD's release has returned, so its references are normally gone already; a short wait covers deferred work.
    const auto deadline = Clock::now() + std::chrono::milliseconds(500);

    for(;;)
    {
        ReclaimRetiredSources();

        if(m_retiringSources.empty() || Clock::now() >= deadline)
        {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Deliberately leaked, each with its private source: freeing either could leave the plugin a dangling pointer.
    m_stats.quarantinedSources += m_retiringSources.size();
    m_retiringSources.clear();
    m_boundDsps.clear();
}

void SteamAudioSimulation::ShutdownLockPath() noexcept
{
    ApplyLifecycleChanges();
    m_boundDsps.clear();

    for(auto& [dsp, active] : m_activeSources)
    {
        iplSourceRemove(active.realSource, m_simulator);
        iplSourceRelease(&active.realSource);
        ++m_stats.sourcesDestroyed;
    }

    m_activeSources.clear();
    iplSimulatorCommit(m_simulator);
}

void SteamAudioSimulation::AttachToInstance(FMOD::Studio::EventInstance& instance)
{
    MuteReturnUntilInstalled();

    if(!m_simulator)
    {
        return;
    }

    {
        // A new instance can reuse a released one's address.
        const std::lock_guard lock(m_mutex);
        std::erase(m_sourceCentricRequests, &instance);
        std::erase(m_pathingRequests, &instance);
        std::erase_if(m_bakedRequests, [&](const auto& request)
        {
            return request.first == &instance;
        });
    }

    // Set before start(), so no DSP of this instance can be created unobserved. A failure leaves the event as direct
    // HRTF, which is what it would be without simulation.
    const FMOD_RESULT userData = instance.setUserData(this);
    const FMOD_RESULT callback = instance.setCallback(&SteamAudioSimulation::OnEventCallback,
        FMOD_STUDIO_EVENT_CALLBACK_PLUGIN_CREATED | FMOD_STUDIO_EVENT_CALLBACK_PLUGIN_DESTROYED);

    if(userData != FMOD_OK || callback != FMOD_OK)
    {
        Log::Warning("Audio: cannot observe an event instance's Steam Audio effects: " +
            DescribeResult(userData != FMOD_OK ? userData : callback) + ".");
    }
}

// FMOD's Studio thread.
FMOD_RESULT F_CALL SteamAudioSimulation::OnEventCallback(FMOD_STUDIO_EVENT_CALLBACK_TYPE type,
    FMOD_STUDIO_EVENTINSTANCE* event, void* parameters)
{
    auto* instance = reinterpret_cast<FMOD::Studio::EventInstance*>(event);
    void* userData = nullptr;

    if(instance->getUserData(&userData) != FMOD_OK || !userData)
    {
        return FMOD_OK;
    }

    auto* simulation = static_cast<SteamAudioSimulation*>(userData);
    const auto* properties = static_cast<const FMOD_STUDIO_PLUGIN_INSTANCE_PROPERTIES*>(parameters);
    auto* dsp = reinterpret_cast<FMOD::DSP*>(properties->dsp);

    if(type == FMOD_STUDIO_EVENT_CALLBACK_PLUGIN_CREATED)
    {
        simulation->OnPluginCreated(*instance, *dsp);
    }
    else if(type == FMOD_STUDIO_EVENT_CALLBACK_PLUGIN_DESTROYED)
    {
        simulation->OnPluginDestroyed(*dsp);
    }

    return FMOD_OK;
}

bool SteamAudioSimulation::ConsumeInjectedFailure(SteamAudioInjectedFailure failure)
{
    const std::lock_guard lock(m_mutex);

    if(m_injectedFailure != failure || m_injectedFailuresRemaining <= 0)
    {
        return false;
    }

    --m_injectedFailuresRemaining;
    return true;
}

// FMOD's Studio thread.
void SteamAudioSimulation::OnPluginCreated(FMOD::Studio::EventInstance& instance, FMOD::DSP& dsp)
{
    if(!m_runtime.IsSpatializer(dsp))
    {
        return;
    }

    // M9.1: Engine publishes no pathing output, and the plugin's path effect crashes on the null coefficients a source would
    // otherwise give it (M9.0). Off before the first block, whatever was authored and whether or not this DSP gets a source.
    // SnapshotSource still answers a pathing read with silent coefficients, in case automation turns it back on.
    const bool pathingAuthored = m_runtime.AppliesPathing(dsp);

    if(pathingAuthored)
    {
        m_runtime.SetAppliesPathing(dsp, false);
    }

    {
        const std::lock_guard lock(m_mutex);
        m_stats.pathingDisabledAtBind += pathingAuthored ? 1 : 0;

        // Binding twice could be dropped or stuck (section 3.3), so a repeated report is never bound again.
        if(!m_boundDsps.insert(&dsp).second)
        {
            ++m_stats.repeatedBindsRefused;
            return;
        }
    }

    if(m_synchronization == SteamAudioSynchronization::Snapshot)
    {
        BindSnapshotSource(instance, dsp, pathingAuthored);
    }
    else
    {
        BindRealSource(instance, dsp);
    }
}

void SteamAudioSimulation::BindSnapshotSource(FMOD::Studio::EventInstance& instance, FMOD::DSP& dsp, bool pathingAuthored)
{
    if(ConsumeInjectedFailure(SteamAudioInjectedFailure::SourceCreation))
    {
        const std::lock_guard lock(m_mutex);
        ++m_stats.creationFailures;
        m_boundDsps.erase(&dsp);
        return;
    }

    auto* source = new(std::nothrow) SnapshotSource();

    if(!source)
    {
        const std::lock_guard lock(m_mutex);
        ++m_stats.creationFailures;
        m_boundDsps.erase(&dsp);
        return;
    }

    // FMOD Studio is the authority: this reads what was authored, and never enables anything. Reflections need the authored
    // value at creation; a later automation of it finds no IR and adds nothing.
    bool reflectionConsumer = m_reflectionsActive && (m_assumeReflectionConsumers || m_runtime.AppliesReflections(dsp));
    std::optional<IPLBakedDataIdentifier> baked;

    // On the production route the Audio Source decides, whatever the event authored.
    if(m_productionRouting)
    {
        const std::lock_guard lock(m_mutex);
        const auto request = std::find(m_sourceCentricRequests.begin(), m_sourceCentricRequests.end(), &instance);
        reflectionConsumer = request != m_sourceCentricRequests.end();
        const auto bakedRequest = std::find_if(m_bakedRequests.begin(), m_bakedRequests.end(), [&](const auto& entry)
        {
            return entry.first == &instance;
        });

        if(reflectionConsumer && bakedRequest != m_bakedRequests.end())
        {
            baked = bakedRequest->second;

            // Baked layers are looked up only through a reverb-route slot; the legacy path would trace this source in real time.
            if(!m_reverbWanted)
            {
                reflectionConsumer = false;
                baked.reset();
            }
        }
    }

    // A slot is only worth having for an event with a Send. FMOD can say that one exists but not where it goes: every Studio send reports
    // the same FMOD_DSP_SEND_RETURNID, so an event with only unrelated sends still gets a slot. Asked here, before the slot, as a later check
    // would delay reflections past the start of a short event. If the groups can't be read yet, the slot is granted. See section 35.
    if(m_productionRouting && reflectionConsumer)
    {
        FMOD::ChannelGroup* group = nullptr;
        const FMOD_RESULT readable = instance.getChannelGroup(&group);
        const std::lock_guard lock(m_mutex);
        ++m_stats.sourceCentricSendChecks;

        if(readable != FMOD_OK || !group)
        {
            ++m_stats.sourceCentricSendChecksUnreadable;
        }
        else if(!GroupHoldsSend(*group))
        {
            reflectionConsumer = false;
            ++m_stats.sourceCentricWithoutSend;
        }
    }

    const bool bound = !ConsumeInjectedFailure(SteamAudioInjectedFailure::SourceBinding) &&
        m_runtime.BindSimulationSource(dsp, source->AsIPLSource());

    // With reverb, a reflection effect exists only for an event with a slot: the plugin would otherwise create it for every event authored
    // with reflections, and it is what would exhaust the pool. Before the first mix, so none is ever made here.
    if(m_reverbWanted && (reflectionConsumer || m_productionRouting))
    {
        m_runtime.SetAppliesReflections(dsp, false);
    }

    // M9.5: how much of the direct path this spatializer takes away, as authored; pathing restores only that part.
    const bool occlusionApplied = m_runtime.AppliesSimulatedOcclusion(dsp);
    const bool transmissionApplied = m_runtime.AppliesSimulatedTransmission(dsp);
    const std::lock_guard lock(m_mutex);
    ++m_stats.sourcesCreated;

    if(bound)
    {
        ++m_stats.bindings;
        LifecycleChange change{LifecycleChange::Kind::Created, &dsp, &instance, source, nullptr, reflectionConsumer,
            reflectionConsumer ? baked : std::nullopt};
        change.pathing = m_pathingSimulator && std::find(m_pathingRequests.begin(), m_pathingRequests.end(), &instance) != m_pathingRequests.end();
        change.occlusionApplied = occlusionApplied;
        change.transmissionApplied = transmissionApplied;
        change.pathingAuthored = pathingAuthored;
        m_lifecycleChanges.push_back(std::move(change));
        return;
    }

    // The DSP keeps running as direct HRTF. The main thread destroys the source once it sees no reference but its own.
    ++m_stats.bindingFailures;
    m_boundDsps.erase(&dsp);
    m_lifecycleChanges.push_back({LifecycleChange::Kind::BindFailed, &dsp, &instance, source, nullptr});
}

void SteamAudioSimulation::BindRealSource(FMOD::Studio::EventInstance& instance, FMOD::DSP& dsp)
{
    IPLSource source = nullptr;

    if(!ConsumeInjectedFailure(SteamAudioInjectedFailure::SourceCreation))
    {
        IPLSourceSettings settings{};
        settings.flags = IPL_SIMULATIONFLAGS_DIRECT;

        // Under m_mutex, so creation never overlaps the main thread's use of the simulator.
        const std::lock_guard lock(m_mutex);

        if(iplSourceCreate(m_simulator, &settings, &source) != IPL_STATUS_SUCCESS)
        {
            source = nullptr;
        }
    }

    if(!source)
    {
        const std::lock_guard lock(m_mutex);
        ++m_stats.creationFailures;
        m_boundDsps.erase(&dsp);
        return;
    }

    const bool bound = !ConsumeInjectedFailure(SteamAudioInjectedFailure::SourceBinding) &&
        m_runtime.BindSimulationSource(dsp, source);

    const std::lock_guard lock(m_mutex);
    ++m_stats.sourcesCreated;

    if(bound)
    {
        ++m_stats.bindings;
        m_lifecycleChanges.push_back({LifecycleChange::Kind::Created, &dsp, &instance, nullptr, source});
        return;
    }

    // The DSP holds no reference, so this release destroys the source.
    ++m_stats.bindingFailures;
    m_boundDsps.erase(&dsp);
    iplSourceRelease(&source);
    ++m_stats.sourcesDestroyed;
}

// FMOD's Studio thread. The DSP is about to be destroyed; its own reference keeps its source valid until then.
void SteamAudioSimulation::OnPluginDestroyed(FMOD::DSP& dsp)
{
    const std::lock_guard lock(m_mutex);

    if(m_boundDsps.erase(&dsp) > 0)
    {
        m_lifecycleChanges.push_back({LifecycleChange::Kind::Destroyed, &dsp, nullptr, nullptr, nullptr});
    }
}

void SteamAudioSimulation::ApplyLifecycleChanges() noexcept
{
    {
        const std::lock_guard lock(m_mutex);
        bool commit = false;

        for(const LifecycleChange& change : m_lifecycleChanges)
        {
            if(change.kind == LifecycleChange::Kind::BindFailed)
            {
                m_retiringSources.push_back({change.snapshot, 0, true, nullptr});
                continue;
            }

            if(change.kind == LifecycleChange::Kind::Created)
            {
                ActiveSource active;
                active.instance = change.instance;
                active.id = m_nextSourceId++;
                active.snapshot = change.snapshot;
                active.realSource = change.realSource;
                active.reflectionConsumer = change.reflectionConsumer;
                active.baked = change.baked;
                active.pathingRequested = change.pathing;
                active.pathingAuthored = change.pathingAuthored;
                active.occlusionApplied = change.occlusionApplied;
                active.transmissionApplied = change.transmissionApplied;
                // No reflections authored: this event has only the reverb.
                active.reverbSendTarget = m_reverbRouting == SteamAudioReverbRoutingForTesting::SourceOnly ||
                    (m_reverbWanted && (!change.reflectionConsumer || m_reverbRouting != SteamAudioReverbRoutingForTesting::Exclusive)) ? 1.0f : 0.0f;

                if(active.snapshot)
                {
                    WorkerCommand command;
                    command.kind = WorkerCommand::Kind::AddSource;
                    command.id = active.id;
                    command.snapshot = active.snapshot;
                    command.reflectionConsumer = change.reflectionConsumer;
                    command.bakedSource = change.baked;
                    command.pathing = change.pathing;
                    command.occlusionApplied = change.occlusionApplied;
                    command.transmissionApplied = change.transmissionApplied;
                    m_pendingCommands.push_back(std::move(command));
                }
                else
                {
                    iplSourceAdd(active.realSource, m_simulator);
                    commit = true;
                }

                m_activeSources[change.dsp] = active;
                continue;
            }

            const auto found = m_activeSources.find(change.dsp);

            if(found == m_activeSources.end())
            {
                continue;
            }

            ActiveSource& active = found->second;

            if(active.snapshot)
            {
                // The worker may be using it right now; the record waits for the worker's acknowledgement.
                WorkerCommand command;
                command.kind = WorkerCommand::Kind::RemoveSource;
                command.id = active.id;
                m_pendingCommands.push_back(std::move(command));
                m_retiringSources.push_back({active.snapshot, active.id, false, nullptr});
            }
            else
            {
                iplSourceRemove(active.realSource, m_simulator);
                iplSourceRelease(&active.realSource);
                ++m_stats.sourcesDestroyed;
                commit = true;
            }

            m_activeSources.erase(found);
        }

        m_lifecycleChanges.clear();

        // DspLock: same thread as RunDirect, so this can never overlap a simulation.
        if(commit)
        {
            iplSimulatorCommit(m_simulator);
        }

        AcceptWorkerRemovals();
        m_stats.activeSources = m_activeSources.size();
        m_stats.retiringSources = m_retiringSources.size();
    }

    m_workerWake.notify_one();
    ReclaimRetiredSources();

    SteamAudioSimulationStats stats = GetStats();

    if(stats.reflectionCapacityExhausted > 0 && !m_capacityReported)
    {
        m_capacityReported = true;

        try
        {
            Log::Warning("Audio: more Steam Audio sources than the reflection capacity of " +
                std::to_string(stats.reflectionSourceCapacity) + "; the extra sources simulate direct only.");
        }
        catch(...)
        {
            // Logging allocation failure only.
        }
    }

    if(stats.pathingSourcesWaiting > 0 && !m_pathingCapacityReported)
    {
        m_pathingCapacityReported = true;

        try
        {
            Log::Warning("Audio: more Audio Sources use pathing than the limit of " + std::to_string(m_pathingSourceLimit) +
                "; the extra sources wait, with direct sound only, until a place is free.");
        }
        catch(...)
        {
            // Logging allocation failure only.
        }
    }

    if((stats.creationFailures > 0 || stats.bindingFailures > 0) && !m_failuresReported)
    {
        m_failuresReported = true;

        try
        {
            Log::Warning("Audio: a Steam Audio simulation source could not be created or bound; that effect runs as "
                "direct HRTF.");
        }
        catch(...)
        {
            // Logging allocation failure only.
        }
    }
}

void SteamAudioSimulation::AcceptWorkerRemovals() noexcept
{
    for(const RemovedPrivateSource& removed : m_workerRemoved)
    {
        for(RetiringSource& retiring : m_retiringSources)
        {
            if(retiring.id == removed.id)
            {
                retiring.workerDone = true;
                retiring.privateSource = removed.realSource;
                retiring.privateReflectionSource = removed.reflectionSource;
            }
        }

        m_published.erase(removed.id);
    }

    m_workerRemoved.clear();
}

// Stage B. Main thread, every Update and at shutdown, so it progresses whether or not anything else plays.
//
// References() == 1 means the only reference left is the owner's: bind took the plugin's before the DSP was reported, and the
// plugin returns it when the DSP is finally destroyed, after its last block. The private source is out of the simulator's run
// list (committed before workerDone), and iplSourceRelease does not touch the simulator, so this may overlap a simulation.
void SteamAudioSimulation::ReclaimRetiredSources() noexcept
{
    std::size_t destroyed = 0;
    std::size_t released = 0;
    std::int64_t anomalies = 0;
    std::int64_t irReads = 0;
    std::int64_t pathingReads = 0;
    std::int64_t validPathingReads = 0;

    for(auto retiring = m_retiringSources.begin(); retiring != m_retiringSources.end();)
    {
        if(retiring->workerDone && retiring->snapshot->References() == 1)
        {
            retiring->snapshot->MarkIrReleased();

            if(retiring->privateReflectionSource)
            {
                iplSourceRelease(&retiring->privateReflectionSource);
            }

            if(retiring->privateSource)
            {
                iplSourceRelease(&retiring->privateSource);
                ++released;
            }

            anomalies += retiring->snapshot->Anomalies();
            irReads += retiring->snapshot->IrReads();
            pathingReads += retiring->snapshot->NeutralPathingReads();
            validPathingReads += retiring->snapshot->ValidPathingReads();
            delete retiring->snapshot;
            ++destroyed;
            retiring = m_retiringSources.erase(retiring);
            continue;
        }

        ++retiring;
    }

    if(destroyed > 0 || anomalies > 0 || pathingReads > 0)
    {
        const std::lock_guard lock(m_mutex);
        m_stats.sourcesDestroyed += destroyed;
        m_stats.privateSourcesReleased += released;
        m_stats.reflectionIrReads += static_cast<std::uint64_t>(irReads);
        m_stats.pathingNeutralReads += static_cast<std::uint64_t>(pathingReads);
        m_stats.pathingValidReads += static_cast<std::uint64_t>(validPathingReads);
        m_stats.sourceAnomalies += static_cast<std::uint64_t>(anomalies);
        m_stats.retiringSources = m_retiringSources.size();
    }
}

bool SteamAudioSimulation::ReadListener(IPLCoordinateSpace3& listener) noexcept
{
    FMOD_3D_ATTRIBUTES attributes{};

    if(m_studio.getListenerAttributes(0, &attributes) != FMOD_OK)
    {
        return false;
    }

    listener = ToIPLCoordinateSpace(attributes);
    return true;
}

// Reads FMOD on this thread and hands the worker a copy. Replaces an unconsumed frame, so nothing accumulates.
void SteamAudioSimulation::PostFrame(std::uint64_t ticket) noexcept
{
    IPLCoordinateSpace3 listener{};
    const bool hasListener = ReadListener(listener);
    m_framePoses.clear();
    const auto now = Clock::now();

    for(auto& [dsp, active] : m_activeSources)
    {
        FMOD_3D_ATTRIBUTES attributes{};

        // A finished, released instance keeps its last pose until FMOD reports its DSP destroyed.
        if(active.instance->isValid() && active.instance->get3DAttributes(&attributes) == FMOD_OK)
        {
            active.pose = ToIPLCoordinateSpace(attributes);
        }

        m_framePoses.push_back({active.id, active.pose});
    }

    // Nothing reflects without geometry, so an empty scene costs the reflection worker nothing.
    m_reflectionSceneUsed |= !m_acousticObjectIds.empty();
    const std::int64_t reverbIrReads = m_reverbSnapshot ? m_reverbSnapshot->IrReads() : 0;
    const bool reflectionDue = m_reflectionsActive && hasListener && m_reflectionSceneUsed &&
        now >= m_nextReflectionFrame;

    if(reflectionDue)
    {
        // Frames are only posted on simulation ticks, so one due frame is late by up to a tick. Counting the next from when this one
        // was due keeps the mean at the interval whatever the frame rate; counting from now would add that lateness every time.
        // After a stall it restarts rather than catching up.
        const auto fromDue = m_nextReflectionFrame + ReflectionInterval;
        m_nextReflectionFrame = m_testControls.legacyReflectionCadence || fromDue <= now ? now + ReflectionInterval : fromDue;
    }

    {
        const std::lock_guard lock(m_mutex);
        m_stats.reverbIrReads = static_cast<std::uint64_t>(reverbIrReads);

        if(reflectionDue)
        {
            m_stats.reflectionFramesCoalesced += m_pendingReflectionFrame.valid ? 1 : 0;
            m_pendingReflectionFrame.valid = true;
            m_pendingReflectionFrame.listener = listener;
            m_pendingReflectionFrame.poses = m_framePoses;
            m_pendingReflectionFrame.controls = m_testControls;
            m_pendingReflectionFrame.postedAt = now;
        }

        if(m_pendingFrame.valid)
        {
            ++m_stats.coalescedFrames;
        }

        m_pendingFrame.valid = true;
        m_pendingFrame.ticket = ticket;

        if(hasListener)
        {
            m_pendingFrame.listener = listener;
        }

        m_pendingFrame.poses.swap(m_framePoses);
        m_pendingFrame.controls = m_testControls;

        // With geometry in the scene every source simulates against it; without any, outputs stay at their neutral values.
        if(!m_acousticObjectIds.empty())
        {
            m_pendingFrame.controls.directFlags |= IPL_DIRECTSIMULATIONFLAGS_OCCLUSION | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION;
        }
    }

    m_workerWake.notify_one();

    if(reflectionDue)
    {
        m_reflectionWake.notify_one();
    }
}

// Option A: outputs are published in place, so the plugin's reads are excluded for the whole of RunDirect.
void SteamAudioSimulation::SimulateUnderLock() noexcept
{
    IPLCoordinateSpace3 listener{};

    if(ReadListener(listener))
    {
        IPLSimulationSharedInputs shared{};
        shared.listener = listener;
        iplSimulatorSetSharedInputs(m_simulator, IPL_SIMULATIONFLAGS_DIRECT, &shared);
    }

    IPLSimulationInputs inputs = MakeInputs(m_testControls);

    for(auto& [dsp, active] : m_activeSources)
    {
        FMOD_3D_ATTRIBUTES attributes{};

        if(!active.instance->isValid() || active.instance->get3DAttributes(&attributes) != FMOD_OK)
        {
            continue;
        }

        active.pose = ToIPLCoordinateSpace(attributes);
        inputs.source = active.pose;
        iplSourceSetInputs(active.realSource, IPL_SIMULATIONFLAGS_DIRECT, &inputs);
    }

    const auto lockRequested = Clock::now();

    if(m_core.lockDSP() != FMOD_OK)
    {
        // Never simulate unprotected; the previous outputs stay in place.
        const std::lock_guard lock(m_mutex);
        ++m_stats.lockFailures;
        return;
    }

    const auto lockAcquired = Clock::now();

    {
        const std::lock_guard lock(m_mutex);
        iplSimulatorRunDirect(m_simulator);
    }

    InjectSimulationDelay(m_testControls.injectedSimulationMilliseconds);
    const auto simulated = Clock::now();
    m_core.unlockDSP();

    const std::lock_guard lock(m_mutex);
    const double lockWait = Microseconds(lockRequested, lockAcquired);
    const double hold = Microseconds(lockAcquired, simulated);
    ++m_stats.simulationRuns;
    m_stats.lastSimulationMicroseconds = hold;
    m_stats.maximumSimulationMicroseconds = std::max(m_stats.maximumSimulationMicroseconds, hold);
    m_stats.lastLockWaitMicroseconds = lockWait;
    m_stats.maximumLockWaitMicroseconds = std::max(m_stats.maximumLockWaitMicroseconds, lockWait);
    m_stats.lastLockHoldMicroseconds = hold;
    m_stats.maximumLockHoldMicroseconds = std::max(m_stats.maximumLockHoldMicroseconds, hold);
}

// M9.3, main thread. Apply Pathing is Engine's to set, never the authored value's: on only while the source's snapshot holds a valid result.
// Either order of the two is safe, because a read without a valid result gets silent coefficients (M9.1, M9.2); this keeps the path effect
// from running, and costing, while there is nothing for it to render.
void SteamAudioSimulation::UpdatePathingActivation() noexcept
{
    std::uint64_t activations = 0;
    std::uint64_t deactivations = 0;

    for(auto& [dsp, active] : m_activeSources)
    {
        if(!active.snapshot)
        {
            continue;
        }

        const bool wanted = active.snapshot->PathingAvailable();

        if(wanted != active.pathingApplied)
        {
            m_runtime.SetAppliesPathing(*dsp, wanted);
            active.pathingApplied = wanted;
            (wanted ? activations : deactivations) += 1;
        }
    }

    if(activations > 0 || deactivations > 0)
    {
        const std::lock_guard lock(m_mutex);
        m_stats.pathingActivations += activations;
        m_stats.pathingDeactivations += deactivations;
    }
}

void SteamAudioSimulation::Update() noexcept
{
    if(!m_simulator)
    {
        return;
    }

    ApplyLifecycleChanges();

    // Retried until the bank holding the bus has loaded.
    if(m_holdReverbReturn && !m_reverbGroupLocked)
    {
        FMOD::Studio::Bus* bus = nullptr;
        m_reverbGroupLocked = m_studio.getBus("bus:/SteamAudioReverb", &bus) == FMOD_OK && bus->lockChannelGroup() == FMOD_OK;
    }

    if(m_productionRouting && m_reverbSnapshot && m_reverbSnapshot->References() == 1 && !m_bindingLossReported)
    {
        // Rebinding here would race the plugin's release, which FMOD's DSP lock does not exclude. Reported instead: it means something released a Reverb.
        m_bindingLossReported = true;
        Log::Error("Audio: the Steam Audio plugin lost the reverb source; listener-centric reflections are silent until restart.");
        const std::lock_guard lock(m_mutex);
        ++m_stats.reverbBindingLost;
    }

    if(m_reverbWanted || m_reverbRouting == SteamAudioReverbRoutingForTesting::SourceOnly)
    {
        ApplySlotReports();
        RampReverbSends();
    }

    if(m_pathingSimulator)
    {
        UpdatePathingActivation();
    }

    const auto now = Clock::now();

    if(now - m_lastSimulation < SimulationInterval || m_activeSources.empty())
    {
        return;
    }

    m_lastSimulation = now;

    if(m_synchronization == SteamAudioSynchronization::Snapshot)
    {
        PostFrame(0);
    }
    else
    {
        SimulateUnderLock();
    }
}

void SteamAudioSimulation::RunNow() noexcept
{
    if(!m_simulator)
    {
        return;
    }

    ApplyLifecycleChanges();

    if(m_activeSources.empty())
    {
        return;
    }

    if(m_synchronization != SteamAudioSynchronization::Snapshot)
    {
        SimulateUnderLock();
        return;
    }

    const std::uint64_t ticket = m_nextTicket++;
    PostFrame(ticket);

    std::unique_lock lock(m_mutex);
    m_workerDone.wait_for(lock, std::chrono::seconds(30), [&] { return m_completedTicket >= ticket; });
}

// The simulation worker: the only code using the simulator and private sources between thread start and join.
void SteamAudioSimulation::WorkerMain() noexcept
{
    std::vector<WorkerCommand> commands;
    InputFrame frame;
    std::vector<RemovedPrivateSource> removed;
    std::unordered_map<SourceId, AcousticTransform> transforms;
    // Set while changes wait for a reflection run to end, from the first attempt at the gate.
    bool gateRequested = false;
    auto gateRequestTime = Clock::now();
    std::uint64_t runsBeforeWait = 0;

    for(;;)
    {
        bool blockAtGate = false;

        {
            std::unique_lock lock(m_mutex);
            const auto hasWork = [&]
            {
                return m_workerStop || m_pendingFrame.valid || !m_pendingCommands.empty() ||
                    !m_pendingAcousticTransforms.empty();
            };

            // Changes it is holding need a retry, whether or not anything new arrives.
            if(gateRequested)
            {
                m_workerWake.wait_for(lock, ReflectionDeferralPoll, hasWork);
            }
            else
            {
                m_workerWake.wait(lock, hasWork);
            }

            // Queued work is dropped at stop: Shutdown cleans up whatever this thread still owns.
            if(m_workerStop)
            {
                return;
            }

            blockAtGate = m_blockAtGateForTesting;

            // After changes held back from earlier passes, in order. A newer object command overrides a held transform, which
            // would otherwise be applied after it.
            for(WorkerCommand& command : m_pendingCommands)
            {
                if(command.kind == WorkerCommand::Kind::ClearAcousticScene)
                {
                    transforms.clear();
                }
                else if(command.kind == WorkerCommand::Kind::SetAcousticObject ||
                    command.kind == WorkerCommand::Kind::RemoveAcousticObject)
                {
                    transforms.erase(command.id);
                }

                commands.push_back(std::move(command));
            }

            m_pendingCommands.clear();
            frame.valid = m_pendingFrame.valid;

            if(frame.valid)
            {
                frame.ticket = m_pendingFrame.ticket;
                frame.listener = m_pendingFrame.listener;
                frame.controls = m_pendingFrame.controls;
                frame.poses.swap(m_pendingFrame.poses);
                m_pendingFrame.valid = false;
            }

            for(const auto& [id, world] : m_pendingAcousticTransforms)
            {
                transforms[id] = world;
            }

            m_pendingAcousticTransforms.clear();
        }

        removed.clear();

        // Taken only for actual changes, so a frame with none never touches the gate. Released before the acknowledgement
        // below: by then the reflection worker can no longer reach a removed source. Without the gate the changes stay here and
        // this pass still simulates: RunDirect needs no gate, because only this thread changes the simulator.
        if(!commands.empty() || !transforms.empty() || !m_retiredEmbreeInstances.empty())
        {
            std::unique_lock gate(m_simulatorGate, std::defer_lock);

            if(!gate.try_lock() && !gateRequested)
            {
                gateRequested = true;
                gateRequestTime = Clock::now();
                m_directGateRequests.fetch_add(1, std::memory_order_seq_cst);
                const std::lock_guard lock(m_mutex);
                runsBeforeWait = m_stats.reflectionRuns;
            }

            if(!gate.owns_lock() && blockAtGate)
            {
                gate.lock();
            }

            if(gate.owns_lock())
            {
                if(gateRequested)
                {
                    gateRequested = false;
                    m_directGateRequests.fetch_sub(1, std::memory_order_seq_cst);
                    const double gateWait = Microseconds(gateRequestTime, Clock::now());
                    const std::lock_guard lock(m_mutex);
                    m_stats.maximumSimulatorGateWaitMicroseconds = std::max(m_stats.maximumSimulatorGateWaitMicroseconds, gateWait);
                    m_stats.maximumReflectionRunsDuringGateWait = std::max(m_stats.maximumReflectionRunsDuringGateWait,
                        m_stats.reflectionRuns - runsBeforeWait);
                }

                ApplyWorkerCommands(commands, removed);
                commands.clear();

                if(!transforms.empty())
                {
                    bool changed = false;
                    WorkerApplyTransforms(transforms, changed);
                    transforms.clear();

                    if(changed)
                    {
                        CommitAcousticScene();
                    }
                }
            }
        }

        {
            const std::lock_guard lock(m_mutex);
            m_workerRemoved.insert(m_workerRemoved.end(), removed.begin(), removed.end());

            for(const RemovedPrivateSource& source : removed)
            {
                m_stats.privateSourcesRemoved += source.realSource ? 1 : 0;
            }
        }

        if(frame.valid)
        {
            SimulateAndPublish(frame);
        }
    }
}

// The reflection worker: never calls FMOD, never publishes, and sees the simulator only while sharing m_simulatorGate.
void SteamAudioSimulation::ReflectionWorkerMain() noexcept
{
    InputFrame frame;
    auto lastRun = Clock::now();

    for(;;)
    {
        bool yieldToDirect = false;

        {
            std::unique_lock lock(m_mutex);
            m_reflectionWake.wait(lock, [&]
            {
                return m_workerStop || m_pendingReflectionFrame.valid;
            });

            if(m_workerStop)
            {
                return;
            }

            const bool directFirst = !m_pendingReflectionFrame.controls.disableDirectFirstScheduling;
            const bool directPending = !m_pendingCommands.empty() || !m_pendingAcousticTransforms.empty() ||
                m_pendingFrame.valid || m_directGateRequests.load(std::memory_order_seq_cst) > 0 ||
                m_pendingReflectionFrame.controls.reportDirectWorkAlwaysPending;
            const bool overdue = Clock::now() - lastRun >= ReflectionStarvationLimit;

            if(directFirst && directPending && !overdue)
            {
                // The frame stays pending, where the main thread replaces it: overdue runs are skipped, not queued.
                ++m_stats.reflectionDeferrals;
                m_reflectionWake.wait_for(lock, ReflectionDeferralPoll, [&]
                {
                    return m_workerStop;
                });
                continue;
            }

            if(directFirst && directPending)
            {
                ++m_stats.reflectionForcedRuns;
            }

            // A run let through for starvation must not give way below, or continual changes would stop it after all.
            yieldToDirect = directFirst && !overdue;
            frame.listener = m_pendingReflectionFrame.listener;
            frame.controls = m_pendingReflectionFrame.controls;
            frame.postedAt = m_pendingReflectionFrame.postedAt;
            frame.poses.swap(m_pendingReflectionFrame.poses);
            m_pendingReflectionFrame.valid = false;
        }

        if(RunReflections(frame, yieldToDirect))
        {
            lastRun = Clock::now();
        }
    }
}

bool SteamAudioSimulation::RunReflections(const InputFrame& frame, bool yieldToDirect) noexcept
{
    const auto start = Clock::now();

    {
        const std::shared_lock gate(m_simulatorGate);

        // The direct worker may have asked between the check above and this lock. It counts first and then waits, so with
        // this reader in place either it is seen here, or it arrives after this point and waits for this one run only.
        if(yieldToDirect && m_directGateRequests.load(std::memory_order_seq_cst) > 0)
        {
            const std::lock_guard lock(m_mutex);
            ++m_stats.reflectionDeferrals;
            return false;
        }

        if(!m_acousticRoot)
        {
            return false;
        }

        IPLSimulationSharedInputs shared{};
        shared.listener = frame.listener;
        // Before the accumulation nudge below: coverage is judged at the listener itself.
        const IPLVector3 listenerAt = frame.listener.origin;

        if(m_reflectionAccumulationStale.exchange(false, std::memory_order_relaxed))
        {
            shared.listener.origin.y += AccumulationResetOffset;
        }

        shared.numRays = m_reflectionSettings.maxNumRays;
        shared.numBounces = m_reflectionBounces.load(std::memory_order_relaxed);
        shared.duration = m_reflectionSettings.maxDuration;
        shared.order = m_reflectionSettings.maxOrder;
        shared.irradianceMinDistance = 1.0f;
        iplSimulatorSetSharedInputs(m_simulator, IPL_SIMULATIONFLAGS_REFLECTIONS, &shared);

        IPLSimulationInputs inputs = MakeInputs(frame.controls);
        inputs.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
        inputs.reverbScale[0] = 1.0f;
        inputs.reverbScale[1] = 1.0f;
        inputs.reverbScale[2] = 1.0f;

        std::size_t realtimeSources = 0;
        std::size_t bakedSources = 0;

        for(const FramePose& framePose : frame.poses)
        {
            // A source retired since the frame was made is gone from the map, and cannot return: ids are never reused.
            const auto found = m_privateSources.find(framePose.id);

            if(found != m_privateSources.end() && found->second.reflectionCapable)
            {
                // M8.5: a baked source looks its own layer up and is never traced, whatever else this run traces.
                IPLSimulationInputs sourceInputs = inputs;
                sourceInputs.source = framePose.pose;

                if(found->second.baked)
                {
                    sourceInputs.baked = IPL_TRUE;
                    sourceInputs.bakedDataIdentifier = *found->second.baked;
                    ++bakedSources;
                }
                else
                {
                    ++realtimeSources;
                }

                iplSourceSetInputs(found->second.reflectionSource ? found->second.reflectionSource : found->second.realSource, IPL_SIMULATIONFLAGS_REFLECTIONS,
                    &sourceInputs);
            }
        }

        // Always at the listener: this one source is the whole cost of the reverb, however many events send to it. M8.4: with baked
        // data registered for the listener it looks the REVERB layer up instead of tracing, so realtime and baked never both feed it.
        const bool baked = m_reverbRealSource && m_bakedListener && !m_bakedReverb.empty();

        if(m_reverbRealSource)
        {
            IPLSimulationInputs reverbInputs = inputs;
            reverbInputs.source = frame.listener;

            if(baked)
            {
                reverbInputs.baked = IPL_TRUE;
                reverbInputs.bakedDataIdentifier.type = IPL_BAKEDDATATYPE_REFLECTIONS;
                reverbInputs.bakedDataIdentifier.variation = IPL_BAKEDDATAVARIATION_REVERB;
                // The REVERB identifier's sphere is all zeros; the SDK compares it even for this variation (M8.1).
                reverbInputs.bakedDataIdentifier.endpointInfluence = IPLSphere{};
            }

            iplSourceSetInputs(m_reverbRealSource, IPL_SIMULATIONFLAGS_REFLECTIONS, &reverbInputs);
        }

        iplSimulatorRunReflections(m_simulator);

        // Outside every probe the SDK keeps the last output (M8.1), so baked output is withheld there rather than trusted to clear: the
        // reverb's (M8.4) and each baked source's (M8.5), all judged by the same listener position.
        if(!m_bakedReverb.empty())
        {
            bool covered = false;

            for(const BakedReverbBatch& entry : m_bakedReverb)
            {
                for(const std::array<float, 4>& sphere : entry.spheres)
                {
                    const float x = listenerAt.x - sphere[0];
                    const float y = listenerAt.y - sphere[1];
                    const float z = listenerAt.z - sphere[2];

                    if(x * x + y * y + z * z <= sphere[3] * sphere[3])
                    {
                        covered = true;
                        break;
                    }
                }

                if(covered)
                {
                    break;
                }
            }

            if(baked)
            {
                SetBakedUncovered(!covered);
            }

            for(const FramePose& framePose : frame.poses)
            {
                const auto found = m_privateSources.find(framePose.id);

                if(found != m_privateSources.end() && found->second.reflectionCapable && found->second.baked && found->second.snapshot)
                {
                    found->second.snapshot->SetIrWithheld(!covered || found->second.bakedDisabled);
                }
            }

            const std::lock_guard lock(m_mutex);
            ++m_stats.bakedReverbRuns;
            m_stats.bakedReverbUncoveredRuns += covered ? 0 : 1;
            m_stats.bakedReverbCovered = covered;
        }
        else
        {
            // No baked data registered: a baked slot has nothing to look up, and stays silent rather than keeping old output.
            for(const FramePose& framePose : frame.poses)
            {
                const auto found = m_privateSources.find(framePose.id);

                if(found != m_privateSources.end() && found->second.reflectionCapable && found->second.baked && found->second.snapshot)
                {
                    found->second.snapshot->SetIrWithheld(true);
                }
            }
        }

        {
            const std::lock_guard lock(m_mutex);
            m_stats.lastRunRealtimeSources = realtimeSources;
            m_stats.lastRunBakedSources = bakedSources;
        }

        // M9.3: sequentially, still under the shared gate, so no scene, batch or source change can overlap it.
        if(m_pathingSimulator)
        {
            RunPathing(frame);
        }

        InjectSimulationDelay(frame.controls.injectedReflectionMilliseconds);
    }

    const double elapsed = Microseconds(start, Clock::now());
    const std::lock_guard lock(m_mutex);
    ++m_stats.reflectionRuns;
    m_stats.reflectionRunMicrosecondsSum += elapsed;
    m_stats.maximumReflectionRunMicroseconds = std::max(m_stats.maximumReflectionRunMicroseconds, elapsed);
    const double wake = Microseconds(frame.postedAt, start);
    m_stats.reflectionWakeMicrosecondsSum += wake;
    m_stats.maximumReflectionWakeMicroseconds = std::max(m_stats.maximumReflectionWakeMicroseconds, wake);

    if(m_lastReflectionStart != Clock::time_point{})
    {
        const double interval = Microseconds(m_lastReflectionStart, start);
        m_stats.reflectionIntervalMicrosecondsSum += interval;
        m_stats.maximumReflectionIntervalMicroseconds = std::max(m_stats.maximumReflectionIntervalMicroseconds, interval);
        ++m_stats.reflectionIntervals;
    }

    m_lastReflectionStart = start;
    return true;
}

// M9.5, reflection worker, shared gate held.
//
// Coverage and batch: a source is simulated only when it and the listener are both inside one registered batch's probes, because paths
// never cross from one batch to another and outside a batch the SDK leaves the previous output in place (M9.0). Where several batches
// cover both, the one the source already uses is kept for as long as it still does, so overlapping volumes never flip a source back and
// forth; otherwise the first in priority order is chosen. Without one the source is disabled (never given a null batch, which crashes
// the SDK) and published invalid.
//
// Mixing (M9.5 option A): pathing makes up only what the direct path loses. Its Ambisonic coefficients are scaled by
// 1 - visibility, where visibility is what the plugin lets through of the direct path (SetDirectVisibility). In clear sight that is
// nothing, so the authored direct sound is heard alone; fully blocked, pathing plays in full, beside whatever the direct path's own
// transmission lets through. The gain moves towards its target with a time constant, so a 150 ms simulation cadence never steps it, and
// starts from silence whenever the source's batch changes or it regains coverage. The EQ is left as simulated.
void SteamAudioSimulation::RunPathing(const InputFrame& frame) noexcept
{
    const auto now = Clock::now();
    std::vector<bool> listenerInside(m_pathingBatches.size());

    for(std::size_t index = 0; index < m_pathingBatches.size(); ++index)
    {
        listenerInside[index] = InsideAnyProbe(m_pathingBatches[index].spheres, frame.listener.origin);
    }

    std::vector<PrivateSource*> simulated;
    simulated.reserve(frame.poses.size());
    std::uint64_t switches = 0;
    bool anyCovered = false;

    for(const FramePose& framePose : frame.poses)
    {
        const auto found = m_privateSources.find(framePose.id);

        if(found == m_privateSources.end() || !found->second.pathingSource)
        {
            continue;
        }

        PrivateSource& source = found->second;
        const auto covers = [&](int index)
        {
            return index >= 0 && static_cast<std::size_t>(index) < m_pathingBatches.size() && listenerInside[static_cast<std::size_t>(index)] &&
                InsideAnyProbe(m_pathingBatches[static_cast<std::size_t>(index)].spheres, framePose.pose.origin);
        };
        int chosen = covers(source.pathingBatch) ? source.pathingBatch : -1;
        // M9.6, display only: each end inside some batch, not necessarily the same one.
        source.pathingSourceInside = false;

        for(const BakedReverbBatch& batch : m_pathingBatches)
        {
            source.pathingSourceInside = source.pathingSourceInside || InsideAnyProbe(batch.spheres, framePose.pose.origin);
        }

        for(int index = 0; chosen < 0 && static_cast<std::size_t>(index) < m_pathingBatches.size(); ++index)
        {
            chosen = covers(index) ? index : -1;
        }

        if(chosen != source.pathingBatch)
        {
            switches += source.pathingBatch >= 0 && chosen >= 0 ? 1 : 0;
            source.pathingBatch = chosen;
            source.pathingGain = 0.0f;
            source.pathingGainAt = {};
        }

        IPLSimulationInputs inputs{};
        inputs.flags = chosen >= 0 ? IPL_SIMULATIONFLAGS_PATHING : static_cast<IPLSimulationFlags>(0);
        inputs.source = framePose.pose;
        inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_INVERSEDISTANCE;
        inputs.distanceAttenuationModel.minDistance = 1.0f;
        // Never null while enabled: the SDK dereferences the batch's path simulator without a check (M9.0).
        inputs.pathingProbes = chosen >= 0 ? m_pathingBatches[static_cast<std::size_t>(chosen)].batch : nullptr;
        inputs.visRadius = PathingVisRadius;
        inputs.visThreshold = PathingVisThreshold;
        inputs.visRange = PathingVisRange;
        inputs.pathingOrder = PathingOrder;
        inputs.enableValidation = m_pathingValidation ? IPL_TRUE : IPL_FALSE;
        inputs.findAlternatePaths = m_pathingValidation ? IPL_TRUE : IPL_FALSE;
        iplSourceSetInputs(source.pathingSource, IPL_SIMULATIONFLAGS_PATHING, &inputs);
        simulated.push_back(&source);
        anyCovered |= chosen >= 0;
    }

    const bool visualize = m_pathingVisualize.load(std::memory_order_relaxed);
    const bool listenerInsideAny = std::find(listenerInside.begin(), listenerInside.end(), true) != listenerInside.end();
    // The SDK does not document which thread reports segments, so the capture is locked. Only while visualizing.
    struct SegmentCapture
    {
        std::mutex mutex;
        std::vector<AudioSystem::PathingSegment> segments;
    } capture;

    if(anyCovered)
    {
        IPLSimulationSharedInputs shared{};
        shared.listener = frame.listener;

        // M9.6: the SDK's own report of the probe-to-probe segments it walked, only while the Editor asks for them.
        if(visualize)
        {
            shared.pathingVisCallback = [](IPLVector3 from, IPLVector3 to, IPLbool occluded, void* userData)
            {
                try
                {
                    auto& target = *static_cast<SegmentCapture*>(userData);
                    const std::lock_guard segmentLock(target.mutex);
                    target.segments.push_back({{from.x, from.y, from.z}, {to.x, to.y, to.z}, occluded == IPL_TRUE});
                }
                catch(...)
                {
                }
            };
            shared.pathingUserData = &capture;
        }

        iplSimulatorSetSharedInputs(m_pathingSimulator, IPL_SIMULATIONFLAGS_PATHING, &shared);
        iplSimulatorRunPathing(m_pathingSimulator);
    }

    std::size_t covered = 0;

    for(PrivateSource* source : simulated)
    {
        SteamAudioPathingSnapshot result;
        result.order = PathingOrder;

        if(source->pathingBatch >= 0)
        {
            IPLSimulationOutputs outputs{};
            iplSourceGetOutputs(source->pathingSource, IPL_SIMULATIONFLAGS_PATHING, &outputs);
            result.valid = outputs.pathing.shCoeffs != nullptr;
            const float target = std::clamp(1.0f - source->snapshot->DirectVisibility(), 0.0f, 1.0f);
            const double elapsed = source->pathingGainAt == Clock::time_point{} ? std::chrono::duration<double>(ReflectionInterval).count() :
                std::chrono::duration<double>(now - source->pathingGainAt).count();
            source->pathingGain += (target - source->pathingGain) * static_cast<float>(1.0 - std::exp(-elapsed / PathingGainSeconds));
            source->pathingGainAt = now;

            for(int band = 0; band < IPL_NUM_BANDS; ++band)
            {
                result.eq[band] = outputs.pathing.eqCoeffs[band];
            }

            for(int index = 0; result.valid && index < (PathingOrder + 1) * (PathingOrder + 1); ++index)
            {
                result.sh[index] = outputs.pathing.shCoeffs[index] * source->pathingGain;
            }

            covered += result.valid ? 1 : 0;
        }

        source->snapshot->PublishPathing(result);
        source->snapshot->SetPathingRun(source->pathingBatch, source->pathingSourceInside, listenerInsideAny,
            source->pathingBatch >= 0 ? source->pathingGain : 0.0f);
    }

    const double elapsed = Microseconds(now, Clock::now());
    const std::lock_guard lock(m_mutex);

    if(visualize || !m_pathingSegments.empty())
    {
        m_pathingSegments = std::move(capture.segments);
    }

    ++m_stats.pathingRuns;
    m_stats.lastPathingRunMicroseconds = elapsed;
    m_stats.pathingRunMicrosecondsSum += elapsed;
    m_stats.maximumPathingRunMicroseconds = std::max(m_stats.maximumPathingRunMicroseconds, elapsed);
    m_stats.lastPathingCoveredSources = covered;
    m_stats.lastPathingUncoveredSources = simulated.size() - covered;
    m_stats.pathingSnapshotsPublished += simulated.size();
    m_stats.pathingBatchSwitches += switches;
}

void SteamAudioSimulation::ApplyWorkerCommands(const std::vector<WorkerCommand>& commands,
    std::vector<RemovedPrivateSource>& removed) noexcept
{
    bool commit = false;
    bool acousticChanged = false;
    // M9.3: pathing sources leave the pathing simulator, which is committed, before they are released.
    bool pathingCommit = false;
    std::vector<IPLSource> pathingRemoved;

    for(const WorkerCommand& command : commands)
    {
        if(command.kind == WorkerCommand::Kind::SetPathingBatch)
        {
            WorkerSetPathingBatches(*command.pathingBatches);
        }
        else if(command.kind == WorkerCommand::Kind::SetAcousticObject)
        {
            acousticChanged |= WorkerSetAcousticObject(command.id, command.mesh, command.world);
        }
        else if(command.kind == WorkerCommand::Kind::RemoveAcousticObject)
        {
            acousticChanged |= WorkerRemoveAcousticObject(command.id);
        }
        else if(command.kind == WorkerCommand::Kind::ClearAcousticScene)
        {
            std::vector<SourceId> ids;

            for(const auto& [id, object] : m_acousticObjects)
            {
                ids.push_back(id);
            }

            for(const SourceId id : ids)
            {
                acousticChanged |= WorkerRemoveAcousticObject(id);
            }
        }
        else if(command.kind == WorkerCommand::Kind::SetScene)
        {
            iplSimulatorSetScene(m_simulator, static_cast<IPLScene>(command.scene));
            commit = true;
        }
        else if(command.kind == WorkerCommand::Kind::AddReverbSource)
        {
            IPLSourceSettings settings{};
            settings.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;

            if(iplSourceCreate(m_simulator, &settings, &m_reverbRealSource) == IPL_STATUS_SUCCESS)
            {
                iplSourceAdd(m_reverbRealSource, m_simulator);
                m_workerReverbSnapshot = command.snapshot;
                commit = true;
                IPLSimulationOutputs outputs{};
                iplSourceGetOutputs(m_reverbRealSource, IPL_SIMULATIONFLAGS_REFLECTIONS, &outputs);
                command.snapshot->PublishReflectionIr(outputs.reflections.ir, outputs.reflections.numChannels, outputs.reflections.irSize);
                const std::lock_guard lock(m_mutex);
                m_stats.reverbIrPublished = true;
                ++m_stats.privateSourcesCreated;
            }
            else
            {
                m_reverbRealSource = nullptr;
                Log::Warning("Audio: the Steam Audio reverb source could not be created; the reverb stays silent.");
            }
        }
        else if(command.kind == WorkerCommand::Kind::SetBakedReverb)
        {
            m_bakedListener = command.bakedListener;
            WorkerSetBakedReverb(*command.baked, commit);
        }
        else if(command.kind == WorkerCommand::Kind::DisableBakedSource)
        {
            const auto found = m_privateSources.find(command.id);

            if(found != m_privateSources.end())
            {
                found->second.bakedDisabled = true;

                if(found->second.snapshot)
                {
                    found->second.snapshot->SetIrWithheld(true);
                }
            }
        }
        else if(command.kind == WorkerCommand::Kind::AddSource)
        {
            PrivateSource privateSource;
            privateSource.snapshot = command.snapshot;
            privateSource.baked = command.bakedSource;
            IPLSourceSettings settings{};
            settings.flags = IPL_SIMULATIONFLAGS_DIRECT;


            // With reverb, slots are given and reported by ServeReflectionSlots.
            if(m_reverbWanted && command.reflectionConsumer)
            {
                if(m_reverbRouting != SteamAudioReverbRoutingForTesting::ListenerOnly)
                {
                    m_slotQueue.emplace_back(command.id, false);
                }
            }
            else if(m_reflectionsActive && command.reflectionConsumer)
            {
                if(m_reflectionSourceCount < static_cast<std::size_t>(ReflectionMaxSources))
                {
                    settings.flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS);
                    privateSource.reflectionCapable = true;
                }
                else
                {
                    const std::lock_guard lock(m_mutex);
                    ++m_stats.reflectionCapacityExhausted;
                }
            }

            if(!ConsumeInjectedFailure(SteamAudioInjectedFailure::PrivateSourceCreation) &&
                iplSourceCreate(m_simulator, &settings, &privateSource.realSource) == IPL_STATUS_SUCCESS)
            {
                iplSourceAdd(privateSource.realSource, m_simulator);
                commit = true;
                m_reflectionSourceCount += privateSource.reflectionCapable ? 1 : 0;
                const bool publishIr = privateSource.reflectionCapable && !m_withholdReflectionIr;

                if(publishIr)
                {
                    IPLSimulationOutputs outputs{};
                    iplSourceGetOutputs(privateSource.realSource, IPL_SIMULATIONFLAGS_REFLECTIONS, &outputs);
                    privateSource.snapshot->PublishReflectionIr(outputs.reflections.ir, outputs.reflections.numChannels,
                        outputs.reflections.irSize);
                }

                const std::lock_guard lock(m_mutex);
                ++m_stats.privateSourcesCreated;
                m_stats.reflectionIrPublished += publishIr ? 1 : 0;
                m_stats.reflectionSources = m_reflectionSourceCount;
            }
            else
            {
                // Stays bound, with its neutral snapshot: the same sound as a source in a scene-less simulation.
                privateSource.realSource = nullptr;
                privateSource.reflectionCapable = false;
                const std::lock_guard lock(m_mutex);
                ++m_stats.privateSourceFailures;
            }

            // M9.5: only for a source that asked, and only within capacity; otherwise it waits, keeping M9.1's silent pathing answer and its
            // usual sound. Without a pathing source Apply Pathing stays off.
            privateSource.pathingRequested = command.pathing && m_pathingSimulator;
            privateSource.occlusionApplied = command.occlusionApplied;
            privateSource.transmissionApplied = command.transmissionApplied;

            if(privateSource.pathingRequested)
            {
                if(m_pathingSourceCount < m_pathingSourceLimit)
                {
                    pathingCommit |= CreatePathingSource(privateSource);
                }
                else
                {
                    m_pathingQueue.push_back(command.id);
                    privateSource.snapshot->SetPathingCapacity(SnapshotSource::PathingCapacity::Waiting);
                }
            }

            m_privateSources[command.id] = privateSource;
        }
        else
        {
            // Stage A. Erased here, and ids are never reused, so no later frame or command can reach this source again.
            IPLSource realSource = nullptr;
            IPLSource reflectionSource = nullptr;
            const auto found = m_privateSources.find(command.id);

            if(found != m_privateSources.end())
            {
                realSource = found->second.realSource;
                reflectionSource = found->second.reflectionSource;

                if(realSource)
                {
                    iplSourceRemove(realSource, m_simulator);
                    commit = true;
                }

                if(reflectionSource)
                {
                    iplSourceRemove(reflectionSource, m_simulator);
                    commit = true;
                }

                if(found->second.pathingSource)
                {
                    iplSourceRemove(found->second.pathingSource, m_pathingSimulator);
                    pathingRemoved.push_back(found->second.pathingSource);
                    pathingCommit = true;
                    --m_pathingSourceCount;
                }

                std::erase(m_pathingQueue, command.id);

                m_reflectionSourceCount -= found->second.reflectionCapable ? 1 : 0;
                m_privateSources.erase(found);
                const std::lock_guard lock(m_mutex);
                m_stats.reflectionSources = m_reflectionSourceCount;
            }

            // Sent after this function's commit. After this the worker holds no pointer to either source.
            removed.push_back({command.id, realSource, reflectionSource});
        }
    }

    if(m_reverbWanted)
    {
        ServeReflectionSlots(commit);
    }

    if(commit)
    {
        iplSimulatorCommit(m_simulator);
    }

    if(m_pathingSimulator)
    {
        ServePathingQueue(pathingCommit);
    }

    if(pathingCommit)
    {
        iplSimulatorCommit(m_pathingSimulator);
    }

    // Out of the pathing simulator's run list since the commit, and never reachable by the plugin: nothing else holds them.
    for(IPLSource& source : pathingRemoved)
    {
        iplSourceRelease(&source);
    }

    if(!pathingRemoved.empty())
    {
        const std::lock_guard lock(m_mutex);
        m_stats.pathingSourcesReleased += pathingRemoved.size();
    }

    if(!m_retiredEmbreeInstances.empty())
    {
        RebuildEmbreeRoot();
        acousticChanged = true;
    }

    if(acousticChanged)
    {
        CommitAcousticScene();
    }
}

// Steam Audio 4.8.1's Embree scene never detaches a removed instance, yet it frees the instance's geometry ID. The next
// instance gets that ID, Embree refuses to attach it without any error reaching the API, and it silently contributes
// nothing. So a removed instance stays referenced, which keeps its ID taken, until a batch ends. Then a new root, whose IDs
// start over, takes the survivors. Removals are rare; moving an object uses a transform update and never comes here.
void SteamAudioSimulation::RebuildEmbreeRoot() noexcept
{
    IPLSceneSettings settings = MakeSceneSettings();
    IPLScene root = nullptr;

    if(iplSceneCreate(m_runtime.GetContext(), &settings, &root) != IPL_STATUS_SUCCESS)
    {
        // The old root still works and its IDs stay taken. The next removal tries again.
        const std::lock_guard lock(m_mutex);
        ++m_stats.acousticGeometryFailures;
        return;
    }

    std::vector<IPLInstancedMesh> replaced;
    std::vector<SourceId> failed;

    for(auto& [id, object] : m_acousticObjects)
    {
        IPLInstancedMeshSettings instanceSettings{};
        instanceSettings.subScene = m_acousticSubScenes.at(object.mesh).scene;
        instanceSettings.transform = ToIPLMatrix(object.world);
        IPLInstancedMesh instance = nullptr;
        replaced.push_back(object.instance);

        if(iplInstancedMeshCreate(root, &instanceSettings, &instance) == IPL_STATUS_SUCCESS)
        {
            iplInstancedMeshAdd(instance, root);
            object.instance = instance;
        }
        else
        {
            failed.push_back(id);
        }
    }

    iplSceneCommit(root);
    SetSimulatorScenes(root);
    m_reflectionAccumulationStale.store(true, std::memory_order_relaxed);

    for(const SourceId id : failed)
    {
        ReleaseSubScene(m_acousticObjects.at(id).mesh);
        m_acousticObjects.erase(id);
    }

    // An Embree scene released with removals it never committed leaks what it still lists, and the sub-scenes behind them.
    for(IPLInstancedMesh& instance : replaced)
    {
        iplInstancedMeshRemove(instance, m_acousticRoot);
        iplInstancedMeshRelease(&instance);
    }

    if(m_acousticRoot)
    {
        iplSceneCommit(m_acousticRoot);
    }

    ReleaseRetiredEmbreeInstances();

    if(m_acousticRoot)
    {
        iplSceneRelease(&m_acousticRoot);
    }

    m_acousticRoot = root;

    const std::lock_guard lock(m_mutex);
    ++m_stats.embreeRootRebuilds;
    m_stats.acousticGeometryFailures += failed.size();
}

void SteamAudioSimulation::ReleaseRetiredEmbreeInstances() noexcept
{
    for(IPLInstancedMesh& instance : m_retiredEmbreeInstances)
    {
        iplInstancedMeshRelease(&instance);
    }

    m_retiredEmbreeInstances.clear();
}

bool SteamAudioSimulation::EnsureRootAcousticScene() noexcept
{
    if(m_acousticRoot)
    {
        return true;
    }

    IPLSceneSettings settings = MakeSceneSettings();

    if(iplSceneCreate(m_runtime.GetContext(), &settings, &m_acousticRoot) != IPL_STATUS_SUCCESS)
    {
        m_acousticRoot = nullptr;
        return false;
    }

    SetSimulatorScenes(m_acousticRoot);
    return true;
}

// M9.3: one scene, both simulators (M9.0 E4: shared on Default and Embree, each simulator committed after the scene). Exclusive gate.
void SteamAudioSimulation::SetSimulatorScenes(IPLScene scene) noexcept
{
    iplSimulatorSetScene(m_simulator, scene);
    iplSimulatorCommit(m_simulator);

    if(m_pathingSimulator)
    {
        iplSimulatorSetScene(m_pathingSimulator, scene);
        iplSimulatorCommit(m_pathingSimulator);
    }
}

// One sub-scene per distinct geometry: vertices through the single coordinate helper, with triangles wound the other way,
// because negating Z mirrors them. It holds the mesh's own materials, per triangle.
SteamAudioSimulation::AcousticSubScene* SteamAudioSimulation::AcquireSubScene(
    const std::shared_ptr<const AcousticMeshData>& mesh) noexcept
{
    const auto found = m_acousticSubScenes.find(mesh.get());

    if(found != m_acousticSubScenes.end())
    {
        ++found->second.references;
        return &found->second;
    }

    const auto buildStart = Clock::now();
    std::vector<IPLVector3> vertices;
    std::vector<IPLTriangle> triangles;
    std::vector<IPLMaterial> materials;
    vertices.reserve(mesh->positions.size());
    triangles.reserve(mesh->triangles.size());
    materials.reserve(mesh->materials.size());

    for(const std::array<float, 3>& position : mesh->positions)
    {
        vertices.push_back(ToIPL(SteamAudio::ToSteamAudio(position)));
    }

    for(const std::array<std::uint32_t, 3>& triangle : mesh->triangles)
    {
        IPLTriangle converted{};
        converted.indices[0] = static_cast<IPLint32>(triangle[0]);
        converted.indices[1] = static_cast<IPLint32>(triangle[2]);
        converted.indices[2] = static_cast<IPLint32>(triangle[1]);
        triangles.push_back(converted);
    }

    for(const AcousticMaterialValues& values : mesh->materials)
    {
        IPLMaterial material{};

        for(int band = 0; band < 3; ++band)
        {
            material.absorption[band] = values.absorption[band];
            material.transmission[band] = values.transmission[band];
        }

        material.scattering = values.scattering;
        materials.push_back(material);
    }

    AcousticSubScene entry;
    entry.mesh = mesh;
    entry.references = 1;

    IPLSceneSettings sceneSettings = MakeSceneSettings();

    IPLStaticMeshSettings meshSettings{};
    meshSettings.numVertices = static_cast<IPLint32>(vertices.size());
    meshSettings.numTriangles = static_cast<IPLint32>(triangles.size());
    meshSettings.numMaterials = static_cast<IPLint32>(materials.size());
    meshSettings.vertices = vertices.data();
    meshSettings.triangles = triangles.data();
    meshSettings.materialIndices = const_cast<IPLint32*>(mesh->triangleMaterials.data());
    meshSettings.materials = materials.data();

    if(ConsumeInjectedFailure(SteamAudioInjectedFailure::AcousticGeometry) ||
        iplSceneCreate(m_runtime.GetContext(), &sceneSettings, &entry.scene) != IPL_STATUS_SUCCESS)
    {
        return nullptr;
    }

    if(iplStaticMeshCreate(entry.scene, &meshSettings, &entry.staticMesh) != IPL_STATUS_SUCCESS)
    {
        iplSceneRelease(&entry.scene);
        return nullptr;
    }

    iplStaticMeshAdd(entry.staticMesh, entry.scene);
    iplSceneCommit(entry.scene);

    const double build = Microseconds(buildStart, Clock::now());

    {
        const std::lock_guard lock(m_mutex);
        m_stats.maximumSubSceneBuildMicroseconds = std::max(m_stats.maximumSubSceneBuildMicroseconds, build);
    }

    const auto [inserted, added] = m_acousticSubScenes.emplace(mesh.get(), std::move(entry));
    return &inserted->second;
}

void SteamAudioSimulation::ReleaseSubScene(const AcousticMeshData* mesh) noexcept
{
    const auto found = m_acousticSubScenes.find(mesh);

    if(found == m_acousticSubScenes.end() || --found->second.references > 0)
    {
        return;
    }

    iplStaticMeshRemove(found->second.staticMesh, found->second.scene);
    iplSceneCommit(found->second.scene);
    iplStaticMeshRelease(&found->second.staticMesh);
    iplSceneRelease(&found->second.scene);
    m_acousticSubScenes.erase(found);
}

// Replaces any object with this id. False when nothing changed. A failure leaves the object out of the scene.
bool SteamAudioSimulation::WorkerSetAcousticObject(SourceId id, const std::shared_ptr<const AcousticMeshData>& mesh,
    const AcousticTransform& world) noexcept
{
    bool changed = WorkerRemoveAcousticObject(id);
    AcousticSubScene* subScene = EnsureRootAcousticScene() ? AcquireSubScene(mesh) : nullptr;
    IPLInstancedMesh instance = nullptr;

    if(subScene)
    {
        IPLInstancedMeshSettings settings{};
        settings.subScene = subScene->scene;
        settings.transform = ToIPLMatrix(world);

        if(iplInstancedMeshCreate(m_acousticRoot, &settings, &instance) != IPL_STATUS_SUCCESS)
        {
            instance = nullptr;
            ReleaseSubScene(mesh.get());
        }
    }

    if(!instance)
    {
        const std::lock_guard lock(m_mutex);
        ++m_stats.acousticGeometryFailures;
        return changed;
    }

    iplInstancedMeshAdd(instance, m_acousticRoot);
    m_acousticObjects[id] = AcousticObject{mesh.get(), instance, world};
    return true;
}

bool SteamAudioSimulation::WorkerRemoveAcousticObject(SourceId id) noexcept
{
    const auto found = m_acousticObjects.find(id);

    if(found == m_acousticObjects.end())
    {
        return false;
    }

    iplInstancedMeshRemove(found->second.instance, m_acousticRoot);

    if(m_embreeDevice)
    {
        m_retiredEmbreeInstances.push_back(found->second.instance);
    }
    else
    {
        iplInstancedMeshRelease(&found->second.instance);
    }

    ReleaseSubScene(found->second.mesh);
    m_acousticObjects.erase(found);
    return true;
}

void SteamAudioSimulation::WorkerApplyTransforms(const std::unordered_map<SourceId, AcousticTransform>& transforms,
    bool& changed) noexcept
{
    std::uint64_t applied = 0;

    for(const auto& [id, world] : transforms)
    {
        const auto found = m_acousticObjects.find(id);

        // An object removed since, or whose build failed.
        if(found != m_acousticObjects.end())
        {
            iplInstancedMeshUpdateTransform(found->second.instance, m_acousticRoot, ToIPLMatrix(world));
            found->second.world = world;
            changed = true;
            ++applied;
        }
    }

    const std::lock_guard lock(m_mutex);
    m_stats.acousticTransformUpdates += applied;
}

void SteamAudioSimulation::CommitAcousticScene() noexcept
{
    const auto start = Clock::now();

    if(m_acousticRoot)
    {
        iplSceneCommit(m_acousticRoot);
    }

    // M9.3: as M9.0 verified the shared scene, the pathing simulator is committed after the scene it reads.
    if(m_pathingSimulator)
    {
        iplSimulatorCommit(m_pathingSimulator);
    }

    const double commit = Microseconds(start, Clock::now());
    std::uint64_t triangles = 0;

    for(const auto& [id, object] : m_acousticObjects)
    {
        triangles += object.mesh->triangles.size();
    }

    const std::lock_guard lock(m_mutex);
    m_stats.acousticObjects = m_acousticObjects.size();
    m_stats.acousticSubScenes = m_acousticSubScenes.size();
    m_stats.acousticTriangles = triangles;
    m_stats.lastSceneCommitMicroseconds = commit;
    m_stats.maximumSceneCommitMicroseconds = std::max(m_stats.maximumSceneCommitMicroseconds, commit);
    m_workerDone.notify_all();
}

// After the join, when this thread owns everything the worker did.
void SteamAudioSimulation::ReleaseAcousticScene() noexcept
{
    std::vector<SourceId> ids;

    for(const auto& [id, object] : m_acousticObjects)
    {
        ids.push_back(id);
    }

    for(const SourceId id : ids)
    {
        WorkerRemoveAcousticObject(id);
    }

    if(m_acousticRoot)
    {
        iplSceneCommit(m_acousticRoot);
        iplSceneRelease(&m_acousticRoot);
    }

    ReleaseRetiredEmbreeInstances();
}

bool SteamAudioSimulation::SetAcousticObject(std::uint64_t id, std::shared_ptr<const AcousticMeshData> mesh,
    const AcousticTransform& world)
{
    if(!mesh || !IsAcousticSimulationAvailable())
    {
        return false;
    }

    m_acousticObjectIds.insert(id);

    {
        const std::lock_guard lock(m_mutex);
        WorkerCommand command;
        command.kind = WorkerCommand::Kind::SetAcousticObject;
        command.id = id;
        command.mesh = std::move(mesh);
        command.world = world;
        m_pendingCommands.push_back(std::move(command));
        m_pendingAcousticTransforms.erase(id);
    }

    m_workerWake.notify_one();
    return true;
}

bool SteamAudioSimulation::SetAcousticObjectTransform(std::uint64_t id, const AcousticTransform& world)
{
    if(!m_acousticObjectIds.contains(id))
    {
        return false;
    }

    {
        const std::lock_guard lock(m_mutex);
        m_pendingAcousticTransforms[id] = world;
    }

    m_workerWake.notify_one();
    return true;
}

bool SteamAudioSimulation::RemoveAcousticObject(std::uint64_t id)
{
    if(m_acousticObjectIds.erase(id) == 0)
    {
        return false;
    }

    {
        const std::lock_guard lock(m_mutex);
        WorkerCommand command;
        command.kind = WorkerCommand::Kind::RemoveAcousticObject;
        command.id = id;
        m_pendingCommands.push_back(std::move(command));
        m_pendingAcousticTransforms.erase(id);
    }

    m_workerWake.notify_one();
    return true;
}

bool SteamAudioSimulation::ClearAcousticScene()
{
    if(!IsAcousticSimulationAvailable())
    {
        return false;
    }

    m_acousticObjectIds.clear();

    {
        const std::lock_guard lock(m_mutex);
        WorkerCommand command;
        command.kind = WorkerCommand::Kind::ClearAcousticScene;
        m_pendingCommands.push_back(std::move(command));
        m_pendingAcousticTransforms.clear();
    }

    m_workerWake.notify_one();
    return true;
}

void SteamAudioSimulation::SimulateAndPublish(const InputFrame& frame) noexcept
{
    IPLSimulationSharedInputs shared{};
    shared.listener = frame.listener;
    iplSimulatorSetSharedInputs(m_simulator, IPL_SIMULATIONFLAGS_DIRECT, &shared);

    IPLSimulationInputs inputs = MakeInputs(frame.controls);

    for(const FramePose& framePose : frame.poses)
    {
        // A source removed since the frame was made is simply no longer here.
        const auto found = m_privateSources.find(framePose.id);

        if(found == m_privateSources.end())
        {
            continue;
        }

        found->second.pose = framePose.pose;

        if(found->second.realSource)
        {
            // M9.5: a source simulating pathing traces volumetric occlusion, so the share pathing makes up changes gradually across an
            // edge instead of flipping (M9.5 E9). Everyone else keeps the single ray.
            IPLSimulationInputs sourceInputs = inputs;
            sourceInputs.source = framePose.pose;

            if(found->second.pathingSource && !frame.controls.volumetricOcclusion)
            {
                sourceInputs.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;
                sourceInputs.numOcclusionSamples = m_pathingOcclusionSamples;
            }

            iplSourceSetInputs(found->second.realSource, IPL_SIMULATIONFLAGS_DIRECT, &sourceInputs);
        }
    }

    const auto simulationStart = Clock::now();
    iplSimulatorRunDirect(m_simulator);
    InjectSimulationDelay(frame.controls.injectedSimulationMilliseconds);
    const auto publicationStart = Clock::now();

    ++m_generation;
    std::uint64_t published = 0;
    std::uint64_t skipped = 0;

    for(auto& [id, privateSource] : m_privateSources)
    {
        if(!privateSource.realSource)
        {
            continue;
        }

        IPLSimulationOutputs outputs{};
        iplSourceGetOutputs(privateSource.realSource, IPL_SIMULATIONFLAGS_DIRECT, &outputs);

        // The slot is the writer's alone until Publish, so the mixer cannot see any of this.
        SteamAudioDirectSnapshot& slot = privateSource.snapshot->Channel().WriteSlot();
        slot.generation = m_generation;
        slot.tag = static_cast<std::uint32_t>(id);
        slot.distanceAttenuation = outputs.direct.distanceAttenuation;
        slot.directivity = outputs.direct.directivity;
        slot.occlusion = outputs.direct.occlusion;

        for(int band = 0; band < IPL_NUM_BANDS; ++band)
        {
            slot.airAbsorption[band] = outputs.direct.airAbsorption[band];
            slot.transmission[band] = outputs.direct.transmission[band];
        }

        // M9.5: what the plugin will let through of this direct path, by Steam Audio's own rule (direct_effect.cpp): occlusion alone, or
        // occlusion + (1 - occlusion) * transmission, transmission averaged over the bands as the frequency-independent mode does and as the
        // frequency-dependent one does on average. A spatializer that doesn't apply the simulation's occlusion lets everything through.
        if(privateSource.pathingSource)
        {
            float visibility = 1.0f;

            if(privateSource.occlusionApplied)
            {
                const float occlusion = std::clamp(outputs.direct.occlusion, 0.0f, 1.0f);
                const float transmission = privateSource.transmissionApplied ? std::clamp((outputs.direct.transmission[0] + outputs.direct.transmission[1] +
                    outputs.direct.transmission[2]) / 3.0f, 0.0f, 1.0f) : 0.0f;
                visibility = occlusion + (1.0f - occlusion) * transmission;
            }

            privateSource.snapshot->SetDirectVisibility(std::isfinite(visibility) ? visibility : 1.0f);
        }

        if(frame.controls.publishFilter)
        {
            frame.controls.publishFilter(slot);
        }

        // A failed publication abandons the slot; the mixer keeps the last complete snapshot, and the next tick refills it.
        if(frame.controls.skipPublication)
        {
            ++skipped;
            continue;
        }

        privateSource.snapshot->Channel().Publish();
        ++published;
    }

    // M9.2, verification only until a pathing simulator exists: synthetic results through the production pathing channel, as a
    // pathing run on this worker will publish them.
    std::uint64_t pathingPublished = 0;
    std::uint64_t pathingAltered = 0;

    // Never to a source with a pathing source: pathing runs publish to that one on the reflection worker, and each channel has one writer.
    if(frame.controls.syntheticPathing)
    {
        SteamAudioPathingSnapshot synthetic = *frame.controls.syntheticPathing;

        for(int repeat = 0; repeat < std::max(frame.controls.syntheticPathingRepeats, 1); ++repeat)
        {
            for(auto& [id, privateSource] : m_privateSources)
            {
                if(privateSource.pathingSource)
                {
                    continue;
                }

                synthetic.generation = m_generation * 1000u + static_cast<std::uint32_t>(repeat);
                pathingAltered += privateSource.snapshot->PublishPathing(synthetic) ? 0 : 1;
                ++pathingPublished;
            }
        }
    }

    const auto end = Clock::now();
    const double simulation = Microseconds(simulationStart, publicationStart);
    const double publication = Microseconds(publicationStart, end);

    const std::lock_guard lock(m_mutex);
    ++m_stats.simulationRuns;
    m_stats.snapshotsPublished += published;
    m_stats.pathingSnapshotsPublished += pathingPublished;
    m_stats.pathingSnapshotsAltered += pathingAltered;
    m_stats.publicationsSkipped += skipped;
    m_stats.lastSimulationMicroseconds = simulation;
    m_stats.maximumSimulationMicroseconds = std::max(m_stats.maximumSimulationMicroseconds, simulation);
    m_stats.lastPublicationMicroseconds = publication;
    m_stats.maximumPublicationMicroseconds = std::max(m_stats.maximumPublicationMicroseconds, publication);

    if(m_diagnosticsWanted)
    {
        for(auto& [id, privateSource] : m_privateSources)
        {
            PublishedRecord& record = m_published[id];
            record.pose = privateSource.pose;

            if(privateSource.realSource)
            {
                IPLSimulationOutputs outputs{};
                iplSourceGetOutputs(privateSource.realSource, IPL_SIMULATIONFLAGS_DIRECT, &outputs);
                record.snapshot.distanceAttenuation = outputs.direct.distanceAttenuation;
                record.snapshot.occlusion = outputs.direct.occlusion;

                for(int band = 0; band < IPL_NUM_BANDS; ++band)
                {
                    record.snapshot.transmission[band] = outputs.direct.transmission[band];
                }
            }
        }
    }

    m_completedTicket = std::max(m_completedTicket, frame.ticket);
    m_workerDone.notify_all();
}

SteamAudioSimulationStats SteamAudioSimulation::GetStats() const
{
    const std::lock_guard lock(m_mutex);
    return m_stats;
}

std::optional<AudioSystem::PathingEventStatus> SteamAudioSimulation::GetPathingStatus(const FMOD::Studio::EventInstance& instance) const
{
    const std::lock_guard lock(m_mutex);

    for(const auto& [dsp, active] : m_activeSources)
    {
        if(active.instance != &instance || !active.snapshot)
        {
            continue;
        }

        AudioSystem::PathingEventStatus status;
        status.available = m_pathingSimulator != nullptr;
        status.requested = active.pathingRequested;

        switch(active.snapshot->GetPathingCapacity())
        {
        case SnapshotSource::PathingCapacity::Simulating:
            status.capacity = AudioSystem::PathingEventStatus::Capacity::Simulating;
            break;
        case SnapshotSource::PathingCapacity::Waiting:
            status.capacity = AudioSystem::PathingEventStatus::Capacity::Waiting;
            break;
        case SnapshotSource::PathingCapacity::NotRequested:
            status.capacity = AudioSystem::PathingEventStatus::Capacity::NotRequested;
            break;
        }

        status.batch = active.snapshot->PathingBatchShown();
        status.sourceCovered = active.snapshot->PathingSourceCovered();
        status.listenerCovered = active.snapshot->PathingListenerCovered();
        status.gain = active.snapshot->PathingGainShown();
        status.applyPathing = active.pathingApplied;
        status.authoredApplyPathing = active.pathingAuthored;
        status.occlusionApplied = active.occlusionApplied;
        status.transmissionApplied = active.transmissionApplied;
        return status;
    }

    return std::nullopt;
}

std::vector<AudioSystem::PathingSegment> SteamAudioSimulation::GetPathingSegments() const
{
    const std::lock_guard lock(m_mutex);
    return m_pathingSegments;
}

std::vector<SteamAudioSourceSnapshot> SteamAudioSimulation::GetSnapshots() const
{
    std::vector<SteamAudioSourceSnapshot> snapshots;
    snapshots.reserve(m_activeSources.size());

    const std::lock_guard lock(m_mutex);
    const_cast<SteamAudioSimulation*>(this)->m_diagnosticsWanted = true;

    for(const auto& [dsp, active] : m_activeSources)
    {
        SteamAudioSourceSnapshot snapshot;
        snapshot.nativeInstance = active.instance;
        snapshot.nativeDsp = dsp;
        snapshot.bindCount = 1;
        snapshot.position = {active.pose.origin.x, active.pose.origin.y, active.pose.origin.z};

        if(active.snapshot)
        {
            snapshot.nativeSource = active.snapshot;
            snapshot.tag = static_cast<std::uint32_t>(active.id);
            const auto published = m_published.find(active.id);

            if(published != m_published.end())
            {
                const IPLVector3& origin = published->second.pose.origin;
                snapshot.position = {origin.x, origin.y, origin.z};
                snapshot.distanceAttenuation = published->second.snapshot.distanceAttenuation;
                snapshot.occlusion = published->second.snapshot.occlusion;

                for(int band = 0; band < IPL_NUM_BANDS; ++band)
                {
                    snapshot.transmission[static_cast<std::size_t>(band)] = published->second.snapshot.transmission[band];
                }
            }
        }
        else
        {
            IPLSimulationOutputs outputs{};
            // Option A: the main thread is the only writer of these outputs, so no lock is needed to read them here.
            iplSourceGetOutputs(active.realSource, IPL_SIMULATIONFLAGS_DIRECT, &outputs);
            snapshot.nativeSource = active.realSource;
            snapshot.distanceAttenuation = outputs.direct.distanceAttenuation;
            snapshot.occlusion = outputs.direct.occlusion;
        }

        snapshots.push_back(snapshot);
    }

    return snapshots;
}

void SteamAudioSimulation::SetTestControls(const SteamAudioSimulationTestControls& controls)
{
    m_testControls = controls;

    {
        const std::lock_guard lock(m_mutex);
        m_blockAtGateForTesting = controls.disableDirectFirstScheduling;
    }

    SetSnapshotReadObserver(controls.readObserver);
    SetPathingReadObserver(controls.pathingReadObserver);

    if(!m_simulator || !controls.scene)
    {
        return;
    }

    if(m_synchronization == SteamAudioSynchronization::Snapshot)
    {
        {
            const std::lock_guard lock(m_mutex);
            WorkerCommand command;
            command.kind = WorkerCommand::Kind::SetScene;
            command.scene = controls.scene;
            m_pendingCommands.push_back(std::move(command));
        }

        m_workerWake.notify_one();
    }
    else
    {
        iplSimulatorSetScene(m_simulator, static_cast<IPLScene>(controls.scene));
        iplSimulatorCommit(m_simulator);
    }
}

void* SteamAudioSimulation::GetWorkerThreadForTesting() noexcept
{
    return m_worker.joinable() ? m_worker.native_handle() : nullptr;
}
}
#endif
