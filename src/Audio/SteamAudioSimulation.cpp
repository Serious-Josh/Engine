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
    if(m_reverbSnapshot)
    {
        m_reverbSnapshot->SetIrWithheld(withheld);
    }
}

bool SteamAudioSimulation::SetSourceCentricRequest(FMOD::Studio::EventInstance& instance, bool enabled)
{
    if(!m_productionRouting)
    {
        return false;
    }

    const std::lock_guard lock(m_mutex);
    std::erase(m_sourceCentricRequests, &instance);

    if(enabled)
    {
        m_sourceCentricRequests.push_back(&instance);
    }

    return true;
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

    {
        const std::lock_guard lock(m_mutex);

        // Binding twice could be dropped or stuck (section 3.3), so a repeated report is never bound again.
        if(!m_boundDsps.insert(&dsp).second)
        {
            ++m_stats.repeatedBindsRefused;
            return;
        }
    }

    if(m_synchronization == SteamAudioSynchronization::Snapshot)
    {
        BindSnapshotSource(instance, dsp);
    }
    else
    {
        BindRealSource(instance, dsp);
    }
}

void SteamAudioSimulation::BindSnapshotSource(FMOD::Studio::EventInstance& instance, FMOD::DSP& dsp)
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

    // On the production route the Audio Source decides, whatever the event authored.
    if(m_productionRouting)
    {
        const std::lock_guard lock(m_mutex);
        const auto request = std::find(m_sourceCentricRequests.begin(), m_sourceCentricRequests.end(), &instance);
        reflectionConsumer = request != m_sourceCentricRequests.end();
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

    const std::lock_guard lock(m_mutex);
    ++m_stats.sourcesCreated;

    if(bound)
    {
        ++m_stats.bindings;
        m_lifecycleChanges.push_back({LifecycleChange::Kind::Created, &dsp, &instance, source, nullptr, reflectionConsumer});
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
            delete retiring->snapshot;
            ++destroyed;
            retiring = m_retiringSources.erase(retiring);
            continue;
        }

        ++retiring;
    }

    if(destroyed > 0 || anomalies > 0)
    {
        const std::lock_guard lock(m_mutex);
        m_stats.sourcesDestroyed += destroyed;
        m_stats.privateSourcesReleased += released;
        m_stats.reflectionIrReads += static_cast<std::uint64_t>(irReads);
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

        for(const FramePose& framePose : frame.poses)
        {
            // A source retired since the frame was made is gone from the map, and cannot return: ids are never reused.
            const auto found = m_privateSources.find(framePose.id);

            if(found != m_privateSources.end() && found->second.reflectionCapable)
            {
                inputs.source = framePose.pose;
                iplSourceSetInputs(found->second.reflectionSource ? found->second.reflectionSource : found->second.realSource, IPL_SIMULATIONFLAGS_REFLECTIONS, &inputs);
            }
        }

        // Always at the listener: this one source is the whole cost of the reverb, however many events send to it.
        if(m_reverbRealSource)
        {
            inputs.source = frame.listener;
            iplSourceSetInputs(m_reverbRealSource, IPL_SIMULATIONFLAGS_REFLECTIONS, &inputs);
        }

        iplSimulatorRunReflections(m_simulator);
        InjectSimulationDelay(frame.controls.injectedReflectionMilliseconds);
    }

    const double elapsed = Microseconds(start, Clock::now());
    const std::lock_guard lock(m_mutex);
    ++m_stats.reflectionRuns;
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

void SteamAudioSimulation::ApplyWorkerCommands(const std::vector<WorkerCommand>& commands,
    std::vector<RemovedPrivateSource>& removed) noexcept
{
    bool commit = false;
    bool acousticChanged = false;

    for(const WorkerCommand& command : commands)
    {
        if(command.kind == WorkerCommand::Kind::SetAcousticObject)
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
        else if(command.kind == WorkerCommand::Kind::AddSource)
        {
            PrivateSource privateSource;
            privateSource.snapshot = command.snapshot;
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
    iplSimulatorSetScene(m_simulator, root);
    iplSimulatorCommit(m_simulator);
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

    iplSimulatorSetScene(m_simulator, m_acousticRoot);
    iplSimulatorCommit(m_simulator);
    return true;
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
            inputs.source = framePose.pose;
            iplSourceSetInputs(found->second.realSource, IPL_SIMULATIONFLAGS_DIRECT, &inputs);
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

    const auto end = Clock::now();
    const double simulation = Microseconds(simulationStart, publicationStart);
    const double publication = Microseconds(publicationStart, end);

    const std::lock_guard lock(m_mutex);
    ++m_stats.simulationRuns;
    m_stats.snapshotsPublished += published;
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
