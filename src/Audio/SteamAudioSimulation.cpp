#include "Audio/AudioSystemInternal.h"

#if defined(ENGINE_HAS_STEAMAUDIO)
#include "Audio/SteamAudioCoordinates.h"
#include "Audio/SteamAudioSnapshotSource.h"
#include "Core/Log.h"

#include <algorithm>
#include <array>
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

    IPLerror status = IPL_STATUS_FAILURE;

    if(options.injectedFailure != SteamAudioInjectedFailure::SimulatorCreation)
    {
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

    if(m_synchronization == SteamAudioSynchronization::Snapshot)
    {
        Log::Info("Audio: Steam Audio simulation uses snapshots; " + m_stats.synchronizationDetail + ".");
    }
    else
    {
        Log::Warning("Audio: Steam Audio simulation falls back to the DSP lock: " + m_stats.synchronizationDetail + ".");
    }
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
        Log::Info("Audio: Steam Audio simulation stopped; " + std::to_string(m_stats.sourcesCreated) +
            " sources created, " + std::to_string(m_stats.sourcesDestroyed) + " destroyed.");

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
    m_worker.join();

    // The join hands the private sources back to this thread.
    for(auto& [id, privateSource] : m_privateSources)
    {
        if(privateSource.realSource)
        {
            iplSourceRemove(privateSource.realSource, m_simulator);
            iplSourceRelease(&privateSource.realSource);
        }
    }

    m_privateSources.clear();
    ReleaseAcousticScene();
    iplSimulatorCommit(m_simulator);

    for(auto& [dsp, active] : m_activeSources)
    {
        m_retiringSources.push_back({active.snapshot, active.id, true});
    }

    m_activeSources.clear();

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

    // Deliberately leaked: freeing one could leave the plugin a dangling vtable.
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
    if(!m_simulator)
    {
        return;
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

    const bool bound = !ConsumeInjectedFailure(SteamAudioInjectedFailure::SourceBinding) &&
        m_runtime.BindSimulationSource(dsp, source->AsIPLSource());

    const std::lock_guard lock(m_mutex);
    ++m_stats.sourcesCreated;

    if(bound)
    {
        ++m_stats.bindings;
        m_lifecycleChanges.push_back({LifecycleChange::Kind::Created, &dsp, &instance, source, nullptr});
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
                m_retiringSources.push_back({change.snapshot, 0, true});
                continue;
            }

            if(change.kind == LifecycleChange::Kind::Created)
            {
                ActiveSource active;
                active.instance = change.instance;
                active.id = m_nextSourceId++;
                active.snapshot = change.snapshot;
                active.realSource = change.realSource;

                if(active.snapshot)
                {
                    WorkerCommand command;
                    command.kind = WorkerCommand::Kind::AddSource;
                    command.id = active.id;
                    command.snapshot = active.snapshot;
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
                m_retiringSources.push_back({active.snapshot, active.id, false});
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

        for(const SourceId id : m_workerRemoved)
        {
            for(RetiringSource& retiring : m_retiringSources)
            {
                if(retiring.id == id)
                {
                    retiring.workerDone = true;
                }
            }

            m_published.erase(id);
        }

        m_workerRemoved.clear();
        m_stats.activeSources = m_activeSources.size();
        m_stats.retiringSources = m_retiringSources.size();
    }

    m_workerWake.notify_one();
    ReclaimRetiredSources();

    SteamAudioSimulationStats stats = GetStats();

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

void SteamAudioSimulation::ReclaimRetiredSources() noexcept
{
    std::size_t destroyed = 0;
    std::int64_t anomalies = 0;

    for(auto retiring = m_retiringSources.begin(); retiring != m_retiringSources.end();)
    {
        if(retiring->workerDone && retiring->snapshot->References() == 1)
        {
            anomalies += retiring->snapshot->Anomalies();
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

    {
        const std::lock_guard lock(m_mutex);

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
    std::vector<SourceId> removed;
    std::unordered_map<SourceId, AcousticTransform> transforms;

    for(;;)
    {
        {
            std::unique_lock lock(m_mutex);
            m_workerWake.wait(lock, [&]
            {
                return m_workerStop || m_pendingFrame.valid || !m_pendingCommands.empty() ||
                    !m_pendingAcousticTransforms.empty();
            });

            // Queued work is dropped at stop: Shutdown cleans up whatever this thread still owns.
            if(m_workerStop)
            {
                return;
            }

            commands.swap(m_pendingCommands);
            frame.valid = m_pendingFrame.valid;

            if(frame.valid)
            {
                frame.ticket = m_pendingFrame.ticket;
                frame.listener = m_pendingFrame.listener;
                frame.controls = m_pendingFrame.controls;
                frame.poses.swap(m_pendingFrame.poses);
                m_pendingFrame.valid = false;
            }

            transforms.swap(m_pendingAcousticTransforms);
        }

        removed.clear();
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

        {
            const std::lock_guard lock(m_mutex);
            m_workerRemoved.insert(m_workerRemoved.end(), removed.begin(), removed.end());
        }

        if(frame.valid)
        {
            SimulateAndPublish(frame);
        }
    }
}

void SteamAudioSimulation::ApplyWorkerCommands(const std::vector<WorkerCommand>& commands,
    std::vector<SourceId>& removed) noexcept
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
        else if(command.kind == WorkerCommand::Kind::AddSource)
        {
            PrivateSource privateSource;
            privateSource.snapshot = command.snapshot;
            IPLSourceSettings settings{};
            settings.flags = IPL_SIMULATIONFLAGS_DIRECT;

            if(!ConsumeInjectedFailure(SteamAudioInjectedFailure::PrivateSourceCreation) &&
                iplSourceCreate(m_simulator, &settings, &privateSource.realSource) == IPL_STATUS_SUCCESS)
            {
                iplSourceAdd(privateSource.realSource, m_simulator);
                commit = true;
            }
            else
            {
                // Stays bound, with its neutral snapshot: the same sound as a source in a scene-less simulation.
                privateSource.realSource = nullptr;
                const std::lock_guard lock(m_mutex);
                ++m_stats.privateSourceFailures;
            }

            m_privateSources[command.id] = privateSource;
        }
        else
        {
            const auto found = m_privateSources.find(command.id);

            if(found != m_privateSources.end())
            {
                if(found->second.realSource)
                {
                    iplSourceRemove(found->second.realSource, m_simulator);
                    iplSourceRelease(&found->second.realSource);
                    commit = true;
                }

                m_privateSources.erase(found);
            }

            // After this the worker holds no pointer to the SnapshotSource.
            removed.push_back(command.id);
        }
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
