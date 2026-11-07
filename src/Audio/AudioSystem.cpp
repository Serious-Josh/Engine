#include "Audio/AudioSystemInternal.h"
#include "Core/Log.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

namespace Audio
{
#if defined(ENGINE_HAS_FMOD)
std::string Internal::DescribeResult(FMOD_RESULT result)
{
    return std::string(FMOD_ErrorString(result)) + " (FMOD_RESULT " + std::to_string(static_cast<int>(result)) + ")";
}
#endif

std::string_view ToString(Status status)
{
    switch(status)
    {
    case Status::NotStarted:
        return "not started";
    case Status::Active:
        return "active";
    case Status::NoOutput:
        return "running without output";
    case Status::Disabled:
        return "disabled";
    case Status::Unavailable:
        return "unavailable";
    }

    return "unknown";
}

std::string_view ToString(SteamAudioStatus status)
{
    switch(status)
    {
    case SteamAudioStatus::NotRequested:
        return "not requested";
    case SteamAudioStatus::Active:
        return "active";
    case SteamAudioStatus::NotCompiled:
        return "not compiled";
    case SteamAudioStatus::Unavailable:
        return "unavailable";
    }

    return "unknown";
}

namespace
{
    // One FMOD Studio system per process: set by the AudioSystem holding it, cleared when it is released.
    std::atomic<bool> processStudioSystemClaimed{false};
}

#if defined(ENGINE_HAS_FMOD)
namespace
{
    using Internal::DescribeResult;
    using Internal::Output;

    // Virtual voices; real voices are bounded separately by FMOD's software channel count.
    constexpr int MaximumChannels = 512;

    bool IsOutputFailure(FMOD_RESULT result)
    {
        switch(result)
        {
        case FMOD_ERR_OUTPUT_ALLOCATED:
        case FMOD_ERR_OUTPUT_CREATEBUFFER:
        case FMOD_ERR_OUTPUT_DRIVERCALL:
        case FMOD_ERR_OUTPUT_FORMAT:
        case FMOD_ERR_OUTPUT_INIT:
        case FMOD_ERR_OUTPUT_NODRIVERS:
            return true;
        default:
            return false;
        }
    }

    // 0xaaaabbcc -> "a.bb.cc", matching FMOD's own release names (2.04.00).
    std::string FormatVersion(unsigned int version)
    {
        char text[32] = {};
        std::snprintf(text, sizeof(text), "%x.%02x.%02x", version >> 16, (version >> 8) & 0xff, version & 0xff);
        return text;
    }

    std::atomic<bool> runtimeLibrariesLoaded{false};

    // The FMOD DLLs this executable delay-loads, read from its own delay-import table: cmake/FMOD.cmake links the
    // Editor's Debug build against the logging DLLs (fmodL, fmodstudioL) and everything else against the production
    // DLLs, while EngineAudio itself is one library shared by both. Core comes first: Studio imports it.
    std::vector<std::string> FindRuntimeLibraries()
    {
        const auto* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        std::vector<std::string> libraries;

        if(directory.VirtualAddress == 0)
        {
            return libraries;
        }

        for(const auto* descriptor = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + directory.VirtualAddress);
            descriptor->DllNameRVA != 0; ++descriptor)
        {
            const std::string name = reinterpret_cast<const char*>(base + descriptor->DllNameRVA);

            if(_strnicmp(name.c_str(), "fmod", 4) == 0)
            {
                libraries.push_back(name);
            }
        }

        std::stable_partition(libraries.begin(), libraries.end(), [](const std::string& name)
        {
            return _strnicmp(name.c_str(), "fmodstudio", 10) != 0;
        });

        return libraries;
    }

    // Loads the FMOD DLLs from the normal DLL search path (the executable's directory first) before any FMOD call:
    // a delay-loaded call into a missing DLL would raise a structured exception instead of returning an error.
    // Loaded libraries stay loaded for the process, because the delay-load thunks keep pointing into them.
    bool LoadRuntimeLibraries(std::string& missingLibrary)
    {
        if(runtimeLibrariesLoaded.load())
        {
            return true;
        }

        const std::vector<std::string> libraries = FindRuntimeLibraries();

        if(libraries.size() != 2)
        {
            missingLibrary = "fmod.dll/fmodstudio.dll (the executable does not delay-load exactly two FMOD DLLs)";
            return false;
        }

        for(const std::string& library : libraries)
        {
            if(!LoadLibraryA(library.c_str()))
            {
                missingLibrary = library;
                return false;
            }
        }

        runtimeLibrariesLoaded.store(true);
        return true;
    }

    void InitializeFMODDebugOutput()
    {
#if defined(_DEBUG)
        // Debug links the logging libraries. FMOD's warnings and errors go to the debugger output and console from
        // FMOD's own threads, so they are not routed through Log. Process-wide; configured before the first system.
        static const bool configured = []()
        {
            FMOD::Debug_Initialize(FMOD_DEBUG_LEVEL_WARNING, FMOD_DEBUG_MODE_TTY);
            return true;
        }();
        static_cast<void>(configured);
#endif
    }
}
#endif

#if defined(ENGINE_HAS_FMOD)
// Creates and initializes one Studio system with the requested output. On failure nothing remains allocated.
FMOD_RESULT AudioSystem::Impl::Start(Output output, InjectedFailure injectedFailure)
{
    if(injectedFailure == InjectedFailure::StudioCreation)
    {
        return FMOD_ERR_MEMORY;
    }

    FMOD_RESULT result = FMOD::Studio::System::create(&studio);

    if(result != FMOD_OK)
    {
        studio = nullptr;
        return result;
    }

    result = studio->getCoreSystem(&core);

    if(result == FMOD_OK && softwareSpeakerModeForTesting != 0)
    {
        result = core->setSoftwareFormat(0, static_cast<FMOD_SPEAKERMODE>(softwareSpeakerModeForTesting), 0);
    }

    if(result == FMOD_OK && output == Output::NoSound)
    {
        result = core->setOutput(FMOD_OUTPUTTYPE_NOSOUND);
    }

    if(result == FMOD_OK && output == Output::Offline)
    {
        result = core->setOutput(FMOD_OUTPUTTYPE_NOSOUND_NRT);
    }

    // FMOD reports a missing device only at initialize; checking first gives the clearer reason.
    if(result == FMOD_OK && output == Output::Device)
    {
        int driverCount = 0;
        result = core->getNumDrivers(&driverCount);

        if(result == FMOD_OK && driverCount == 0)
        {
            result = FMOD_ERR_OUTPUT_NODRIVERS;
        }
    }

    if(result == FMOD_OK)
    {
        // NoSoundInitialization models a machine where both outputs fail, so it fails the device attempt as well.
        const bool injectDeviceFailure = injectedFailure == InjectedFailure::OutputInitialization ||
            injectedFailure == InjectedFailure::NoSoundInitialization;
        const bool injectOutputFailure =
            (output == Output::Device && injectDeviceFailure) ||
            (output == Output::NoSound && injectedFailure == InjectedFailure::NoSoundInitialization);

        if(injectOutputFailure)
        {
            result = FMOD_ERR_OUTPUT_INIT;
        }
        else if(injectedFailure == InjectedFailure::StudioInitialization)
        {
            result = FMOD_ERR_INTERNAL;
        }
        else
        {
            // Studio's default asynchronous mode: update() only queues commands for FMOD's Studio thread, so a
            // stalled Engine frame never stalls the mixer. Live Update is not enabled (Audio.5, Editor only).
            const FMOD_STUDIO_INITFLAGS studioFlags =
                output == Output::Offline ? FMOD_STUDIO_INIT_SYNCHRONOUS_UPDATE : FMOD_STUDIO_INIT_NORMAL;
            result = studio->initialize(MaximumChannels, studioFlags, FMOD_INIT_NORMAL, nullptr);
        }
    }

    if(result != FMOD_OK)
    {
        ReleaseStudioSystem();
    }

    return result;
}

#if defined(ENGINE_HAS_STEAMAUDIO)
void AudioSystem::Impl::StopProbeGeneration() noexcept
{
    if(probeJob)
    {
        probeJob->Cancel();
    }

    if(probeThread.joinable())
    {
        probeThread.join();
    }

    probeJob.reset();
}
#endif

void AudioSystem::Impl::ReleaseStudioSystem() noexcept
{
    if(!studio)
    {
        return;
    }

    // Releases the Studio system's Core system as well; Core is never released on its own.
    const FMOD_RESULT result = studio->release();

    if(result != FMOD_OK)
    {
        Log::Warning("Audio: FMOD Studio system release reported " + DescribeResult(result) + ".");
    }

    studio = nullptr;
    core = nullptr;

#if defined(ENGINE_HAS_STEAMAUDIO)
    // SA.1: only now, with every Steam Audio DSP destroyed by the release above, may the plugin's state go. SA.3:
    // sources and their simulator first, while Steam Audio's context still exists.
    StopProbeGeneration();
    steamAudioSimulation.reset();
    probeBatches.reset();

    if(steamAudio)
    {
        steamAudio->Terminate();
        steamAudio.reset();
        Log::Info("Audio: Steam Audio stopped.");
    }
#endif
}

ProbeGenerationResult AudioSystem::GenerateProbes(const ProbeGenerationRequest& request) const
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    if(m_impl->steamAudio && m_impl->steamAudioStatus == SteamAudioStatus::Active)
    {
        return Internal::GenerateProbeBatch(m_impl->steamAudio->GetContext(), request);
    }
#else
    static_cast<void>(request);
#endif

    ProbeGenerationResult result;
    result.error = "Steam Audio is not active in this project";
    return result;
}

std::shared_ptr<ProbeGenerationJob> AudioSystem::StartProbeGeneration(ProbeGenerationRequest request)
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    if(!m_impl->steamAudio || m_impl->steamAudioStatus != SteamAudioStatus::Active || (m_impl->probeJob && !m_impl->probeJob->IsFinished()))
    {
        return nullptr;
    }

    m_impl->StopProbeGeneration();
    auto job = std::make_shared<ProbeGenerationJob>();
    // The context outlives the thread: StopProbeGeneration joins it before Steam Audio stops.
    m_impl->probeThread = std::thread([job, context = m_impl->steamAudio->GetContext(), request = std::move(request)]
    {
        job->Finish(Internal::GenerateProbeBatch(context, request, job.get()));
    });
    m_impl->probeJob = job;
    return job;
#else
    static_cast<void>(request);
    return nullptr;
#endif
}

bool AudioSystem::LoadProbeBatch(std::span<const std::byte> batch, std::size_t expectedProbes, std::string& error)
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    if(m_impl->steamAudio && m_impl->steamAudioStatus == SteamAudioStatus::Active)
    {
        if(!m_impl->probeBatches)
        {
            m_impl->probeBatches = std::make_unique<Internal::SteamAudioProbeBatches>();
        }

        return m_impl->probeBatches->Load(m_impl->steamAudio->GetContext(), batch, expectedProbes, error);
    }
#else
    static_cast<void>(batch);
    static_cast<void>(expectedProbes);
#endif

    error = "Steam Audio is not active";
    return false;
}

void AudioSystem::ClearProbeBatches() noexcept
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    if(m_impl->probeBatches)
    {
        m_impl->probeBatches->Clear();
    }
#endif
}

ProbeBatchStats AudioSystem::GetProbeBatchStats() const noexcept
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    if(m_impl->probeBatches)
    {
        return m_impl->probeBatches->GetStats();
    }
#endif

    return {};
}

std::string AudioSystem::Impl::DescribeRunningOutput(Output output) const
{
    unsigned int version = 0;
    unsigned int build = 0;
    std::string description;

    if(core->getVersion(&version, &build) == FMOD_OK)
    {
        description = "FMOD " + FormatVersion(version) + " (build " + std::to_string(build) + ")";
    }

    int sampleRate = 0;
    FMOD_SPEAKERMODE speakerMode = FMOD_SPEAKERMODE_DEFAULT;
    int rawSpeakers = 0;

    if(core->getSoftwareFormat(&sampleRate, &speakerMode, &rawSpeakers) == FMOD_OK)
    {
        description += ", " + std::to_string(sampleRate) + " Hz";
    }

    if(output != Output::Device)
    {
        return description + ", no-sound output";
    }

    int driver = 0;
    char driverName[256] = {};

    if(core->getDriver(&driver) == FMOD_OK &&
        core->getDriverInfo(driver, driverName, sizeof(driverName), nullptr, nullptr, nullptr, nullptr) == FMOD_OK)
    {
        description += ", output device '" + std::string(driverName) + "'";
    }

    return description;
}
#endif

AudioSystem::AudioSystem() : m_impl(std::make_unique<Impl>())
{
}

AudioSystem::~AudioSystem()
{
    Shutdown();
}

bool AudioSystem::IsBackendCompiled() noexcept
{
#if defined(ENGINE_HAS_FMOD)
    return true;
#else
    return false;
#endif
}

std::int64_t AudioSystem::GetBackendAllocatedBytes() noexcept
{
#if defined(ENGINE_HAS_FMOD)
    if(!runtimeLibrariesLoaded.load())
    {
        return -1;
    }

    int currentBytes = 0;
    int peakBytes = 0;

    if(FMOD::Memory_GetStats(&currentBytes, &peakBytes, true) != FMOD_OK)
    {
        return -1;
    }

    return currentBytes;
#else
    return -1;
#endif
}

Status AudioSystem::Initialize(const InitializeOptions& options)
{
    if(m_impl->status == Status::Active || m_impl->status == Status::NoOutput)
    {
        return m_impl->status;
    }

    m_impl->updateCount = 0;
    m_impl->updateFailureReported = false;

#if !defined(ENGINE_HAS_FMOD)
    static_cast<void>(options);
    m_impl->status = Status::Disabled;
    m_impl->statusDetail = "this build has no FMOD backend (ENGINE_ENABLE_FMOD=OFF or the SDK was not found)";
    Log::Info("Audio: disabled; " + m_impl->statusDetail + ".");
    return m_impl->status;
#else
    bool expectedUnclaimed = false;

    if(!processStudioSystemClaimed.compare_exchange_strong(expectedUnclaimed, true))
    {
        m_impl->status = Status::Unavailable;
        m_impl->statusDetail = "another AudioSystem already owns this process's FMOD Studio system";
        Log::Error("Audio: unavailable; " + m_impl->statusDetail + ".");
        return m_impl->status;
    }

    m_impl->claimedProcessSystem = true;

    if(std::string missingLibrary; !LoadRuntimeLibraries(missingLibrary))
    {
        processStudioSystemClaimed.store(false);
        m_impl->claimedProcessSystem = false;
        m_impl->status = Status::Unavailable;
        m_impl->statusDetail = "the FMOD runtime library " + missingLibrary + " was not found beside the executable";
        Log::Warning("Audio: unavailable; " + m_impl->statusDetail + ". Engine continues without audio.");
        return m_impl->status;
    }

    InitializeFMODDebugOutput();

    // Device output first, unless the host asked for none. Only an output failure falls back to no-sound output.
    Output output = options.noSoundOutput ? Output::NoSound : Output::Device;

    if(options.offlineRendering)
    {
        output = Output::Offline;
    }

    m_impl->softwareSpeakerModeForTesting = options.softwareSpeakerModeForTesting;
    FMOD_RESULT result = m_impl->Start(output, options.injectedFailure);
    std::string deviceFailure;

    if(output == Output::Device && IsOutputFailure(result))
    {
        deviceFailure = DescribeResult(result);
        Log::Warning("Audio: no usable output device (" + deviceFailure + "); retrying with FMOD's no-sound output.");
        output = Output::NoSound;
        result = m_impl->Start(output, options.injectedFailure);
    }

    if(result != FMOD_OK)
    {
        processStudioSystemClaimed.store(false);
        m_impl->claimedProcessSystem = false;
        m_impl->status = Status::Unavailable;
        m_impl->statusDetail = "FMOD initialization failed: " + DescribeResult(result);
        Log::Error("Audio: unavailable; " + m_impl->statusDetail + ". Engine continues without audio.");
        return m_impl->status;
    }

    m_impl->statusDetail = m_impl->DescribeRunningOutput(output);

    if(output == Output::Device)
    {
        m_impl->status = Status::Active;
        Log::Info("Audio: active; " + m_impl->statusDetail + ".");
    }
    else
    {
        m_impl->status = Status::NoOutput;

        if(!deviceFailure.empty())
        {
            m_impl->statusDetail += "; output device failed: " + deviceFailure;
        }

        Log::Warning("Audio: running without output; " + m_impl->statusDetail + ".");
    }

    return m_impl->status;
#endif
}

void AudioSystem::Disable(std::string_view reason)
{
    if(m_impl->status == Status::Active || m_impl->status == Status::NoOutput)
    {
        return;
    }

    m_impl->status = Status::Disabled;
    m_impl->statusDetail = reason;
    Log::Info("Audio: disabled; " + m_impl->statusDetail + ".");
}

void AudioSystem::Update() noexcept
{
#if defined(ENGINE_HAS_FMOD)
    if(!m_impl->studio)
    {
        return;
    }

#if defined(ENGINE_HAS_STEAMAUDIO)
    if(m_impl->steamAudio)
    {
        m_impl->steamAudio->UpdateMixerPool();
    }

    if(m_impl->steamAudioSimulation)
    {
        m_impl->steamAudioSimulation->Update();
    }
#endif

    const FMOD_RESULT result = m_impl->studio->update();

    if(result == FMOD_OK)
    {
        ++m_impl->updateCount;
        // Instances FMOD destroyed since the last frame (released and finished, or their bank unloaded).
        m_impl->ReapEventSlots();
        return;
    }

    // Reported once per session: a persistent failure must not flood the log every frame.
    if(!m_impl->updateFailureReported)
    {
        m_impl->updateFailureReported = true;
        Log::Error("Audio: FMOD Studio update failed: " + DescribeResult(result) + ".");
    }
#endif
}

void AudioSystem::Shutdown() noexcept
{
#if defined(ENGINE_HAS_FMOD)
    const bool released = m_impl->studio != nullptr;

    // Explicit, ordered teardown of everything Engine created: instances, then banks, then the system. Releasing the
    // system alone would free them too, but this keeps the order independent of FMOD's internal release sequence.
    if(released)
    {
#if defined(ENGINE_HAS_STEAMAUDIO)
        if(m_impl->steamAudioSimulation)
        {
            m_impl->steamAudioSimulation->ReleaseReflectionReturn();
        }
#endif
        m_impl->ReleaseAllEvents();
        m_impl->UnloadAllBanks();
    }

    m_impl->ReleaseStudioSystem();

    if(released)
    {
        Log::Info("Audio: FMOD Studio system released after " + std::to_string(m_impl->updateCount) + " updates.");
    }
#endif

    if(m_impl->claimedProcessSystem)
    {
        processStudioSystemClaimed.store(false);
        m_impl->claimedProcessSystem = false;
    }

    m_impl->status = Status::NotStarted;
    m_impl->statusDetail.clear();
    m_impl->steamAudioStatus = SteamAudioStatus::NotRequested;
    m_impl->steamAudioDetail.clear();
}

Status AudioSystem::GetStatus() const noexcept
{
    return m_impl->status;
}

const std::string& AudioSystem::GetStatusDetail() const noexcept
{
    return m_impl->statusDetail;
}

std::uint64_t AudioSystem::GetUpdateCount() const noexcept
{
    return m_impl->updateCount;
}

bool AudioSystem::IsSteamAudioCompiled() noexcept
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    return true;
#else
    return false;
#endif
}

SteamAudioStatus AudioSystem::EnableSteamAudio(const SteamAudioOptions& options)
{
    if(m_impl->steamAudioStatus != SteamAudioStatus::NotRequested)
    {
        return m_impl->steamAudioStatus;
    }

#if !defined(ENGINE_HAS_STEAMAUDIO)
    static_cast<void>(options);
    m_impl->steamAudioStatus = SteamAudioStatus::NotCompiled;
    m_impl->steamAudioDetail = "this build has no Steam Audio (ENGINE_ENABLE_STEAMAUDIO=OFF, or its SDK or the FMOD "
        "backend was not found)";
    Log::Warning("Audio: the project uses Steam Audio, but " + m_impl->steamAudioDetail + "; using FMOD only.");
    return m_impl->steamAudioStatus;
#else
    const auto refuse = [this](std::string detail)
    {
        m_impl->steamAudioStatus = SteamAudioStatus::Unavailable;
        m_impl->steamAudioDetail = std::move(detail);
        Log::Warning("Audio: Steam Audio unavailable; " + m_impl->steamAudioDetail + ". Using FMOD only.");
        return m_impl->steamAudioStatus;
    };

    if(!m_impl->IsRunning())
    {
        return refuse("audio is " + std::string(ToString(m_impl->status)));
    }

    // A bank loaded without the plugin has already failed (or loaded without its Steam Audio effects resolved).
    if(!m_impl->banks.empty())
    {
        return refuse("it must start before any audio bank is loaded");
    }

    auto runtime = std::make_unique<Internal::SteamAudioRuntime>();

    runtime->SetContextAllocatorForTesting(options.contextAllocateForTesting, options.contextFreeForTesting, options.useSdkAllocatorForTesting);

    if(std::string error; !runtime->Start(*m_impl->core, options.injectedFailure, error))
    {
        return refuse(error);
    }

    m_impl->steamAudioStatus = SteamAudioStatus::Active;
    m_impl->steamAudioDetail = runtime->GetDescription();
    m_impl->steamAudio = std::move(runtime);
    Log::Info("Audio: Steam Audio active; " + m_impl->steamAudioDetail + ".");

    // Direct HRTF does not need the simulator, so a failure here keeps Steam Audio Active.
    m_impl->steamAudioSimulation =
        std::make_unique<Internal::SteamAudioSimulation>(*m_impl->steamAudio, *m_impl->studio, *m_impl->core);
    m_impl->steamAudioSimulation->Start(options);

    // After Start, which decides whether reflections are active, and before any bank, so before any DSP can mix.
    const auto stats = m_impl->steamAudioSimulation->GetStats();
    m_impl->steamAudio->ArmMixerPool(*m_impl->core, stats.reflectionInfrastructureActive, options);
    // Before any bank, so the authored return finds the source when its DSP first runs.
    m_impl->steamAudioSimulation->StartEnvironmentalReverb();

    return m_impl->steamAudioStatus;
#endif
}

SteamAudioStatus AudioSystem::GetSteamAudioStatus() const noexcept
{
    return m_impl->steamAudioStatus;
}

const std::string& AudioSystem::GetSteamAudioStatusDetail() const noexcept
{
    return m_impl->steamAudioDetail;
}

#if defined(ENGINE_HAS_FMOD)
namespace
{
    // Every DSP of a group and its nested groups (an event's master track and its group tracks). Steam Audio
    // spatializers are recognised by plugin identity (SteamAudioRuntime::IsSpatializer); FMOD's own spatializer by
    // its built-in DSP type (FMOD Studio's spatializer and panner effects both run as an FMOD Pan DSP).
    enum class SteamAudioKind
    {
        None,
        WithDistanceAttenuation,
        WithoutDistanceAttenuation
    };

    template<typename ClassifySteamAudio>
    void CountSpatializers(FMOD::ChannelGroup& group, const ClassifySteamAudio& classifySteamAudio,
        SpatializerSummary& summary)
    {
        int dspCount = 0;
        group.getNumDSPs(&dspCount);

        for(int index = 0; index < dspCount; ++index)
        {
            FMOD::DSP* dsp = nullptr;

            if(group.getDSP(index, &dsp) != FMOD_OK || !dsp)
            {
                continue;
            }

            FMOD_DSP_TYPE type = FMOD_DSP_TYPE_UNKNOWN;

            if(dsp->getType(&type) == FMOD_OK && type == FMOD_DSP_TYPE_PAN)
            {
                ++summary.fmod;
            }
            else if(const SteamAudioKind kind = classifySteamAudio(*dsp); kind != SteamAudioKind::None)
            {
                ++summary.steamAudio;

                if(kind == SteamAudioKind::WithoutDistanceAttenuation)
                {
                    ++summary.steamAudioWithoutDistanceAttenuation;
                }
            }
        }

        int childCount = 0;
        group.getNumGroups(&childCount);

        for(int index = 0; index < childCount; ++index)
        {
            FMOD::ChannelGroup* child = nullptr;

            if(group.getGroup(index, &child) == FMOD_OK && child)
            {
                CountSpatializers(*child, classifySteamAudio, summary);
            }
        }
    }
}
#endif

std::optional<SpatializerSummary> AudioSystem::DescribeEventSpatializers(EventHandle handle) const
{
#if defined(ENGINE_HAS_FMOD)
    FMOD::Studio::EventInstance* instance = m_impl->ResolveEvent(handle);

    if(!instance)
    {
        return std::nullopt;
    }

    // FMOD creates an instance's DSPs asynchronously, after start() reaches its Studio thread.
    FMOD::ChannelGroup* group = nullptr;

    if(instance->getChannelGroup(&group) != FMOD_OK || !group)
    {
        return std::nullopt;
    }

    const auto classifySteamAudio = [this](FMOD::DSP& dsp)
    {
#if defined(ENGINE_HAS_STEAMAUDIO)
        if(!m_impl->steamAudio || !m_impl->steamAudio->IsSpatializer(dsp))
        {
            return SteamAudioKind::None;
        }

        return m_impl->steamAudio->HasDistanceAttenuation(dsp) ? SteamAudioKind::WithDistanceAttenuation :
            SteamAudioKind::WithoutDistanceAttenuation;
#else
        static_cast<void>(dsp);
        return SteamAudioKind::None;
#endif
    };
    SpatializerSummary summary;
    CountSpatializers(*group, classifySteamAudio, summary);
    return summary;
#else
    static_cast<void>(handle);
    return std::nullopt;
#endif
}

bool AudioSystem::IsAcousticSimulationAvailable() const noexcept
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    return m_impl->steamAudioSimulation && m_impl->steamAudioSimulation->IsAcousticSimulationAvailable();
#else
    return false;
#endif
}

bool AudioSystem::SetAcousticObject(std::uint64_t id, std::shared_ptr<const AcousticMeshData> mesh, const AcousticTransform& world)
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    return IsAcousticSimulationAvailable() && m_impl->steamAudioSimulation->SetAcousticObject(id, std::move(mesh), world);
#else
    static_cast<void>(id);
    static_cast<void>(mesh);
    static_cast<void>(world);
    return false;
#endif
}

bool AudioSystem::SetAcousticObjectTransform(std::uint64_t id, const AcousticTransform& world)
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    return IsAcousticSimulationAvailable() && m_impl->steamAudioSimulation->SetAcousticObjectTransform(id, world);
#else
    static_cast<void>(id);
    static_cast<void>(world);
    return false;
#endif
}

bool AudioSystem::RemoveAcousticObject(std::uint64_t id)
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    return IsAcousticSimulationAvailable() && m_impl->steamAudioSimulation->RemoveAcousticObject(id);
#else
    static_cast<void>(id);
    return false;
#endif
}

bool AudioSystem::ClearAcousticScene()
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    return IsAcousticSimulationAvailable() && m_impl->steamAudioSimulation->ClearAcousticScene();
#else
    return false;
#endif
}

SteamAudioMixerPoolStats AudioSystem::GetSteamAudioMixerPoolStats() const
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    if(m_impl->steamAudio)
    {
        return m_impl->steamAudio->GetMixerPoolStats();
    }
#endif

    return {};
}

SteamAudioSimulationStats AudioSystem::GetSteamAudioSimulationStats() const
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    if(m_impl->steamAudioSimulation)
    {
        return m_impl->steamAudioSimulation->GetStats();
    }
#endif

    return {};
}

void* AudioSystem::GetSteamAudioContextForTesting() const noexcept
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    if(m_impl->steamAudio)
    {
        return m_impl->steamAudio->GetContext();
    }
#endif

    return nullptr;
}

void* AudioSystem::GetNativeCoreForTesting() const noexcept
{
#if defined(ENGINE_HAS_FMOD)
    return m_impl->core;
#else
    return nullptr;
#endif
}

void AudioSystem::BeginReflectionSession() noexcept
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    if(m_impl->steamAudioSimulation)
    {
        m_impl->steamAudioSimulation->BeginReflectionSession();
    }
#endif
}

void AudioSystem::EndReflectionSession() noexcept
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    if(m_impl->steamAudioSimulation)
    {
        m_impl->steamAudioSimulation->EndReflectionSession();
    }
#endif
}

void AudioSystem::SetListenerReflectionsWithheld(bool withheld) noexcept
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    if(m_impl->steamAudioSimulation)
    {
        m_impl->steamAudioSimulation->SetListenerReflectionsWithheld(withheld);
    }
#else
    static_cast<void>(withheld);
#endif
}

void* AudioSystem::GetSteamAudioReverbGroupForTesting() const noexcept
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    if(m_impl->steamAudioSimulation)
    {
        return m_impl->steamAudioSimulation->GetReverbGroupForTesting();
    }
#endif

    return nullptr;
}

void* AudioSystem::GetSteamAudioWorkerThreadForTesting() const noexcept
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    if(m_impl->steamAudioSimulation)
    {
        return m_impl->steamAudioSimulation->GetWorkerThreadForTesting();
    }
#endif

    return nullptr;
}

void AudioSystem::RunSteamAudioSimulationForTesting() noexcept
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    if(m_impl->steamAudioSimulation)
    {
        m_impl->steamAudioSimulation->RunNow();
    }
#endif
}

std::vector<SteamAudioSourceSnapshot> AudioSystem::GetSteamAudioSourcesForTesting() const
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    if(m_impl->steamAudioSimulation)
    {
        return m_impl->steamAudioSimulation->GetSnapshots();
    }
#endif

    return {};
}

void AudioSystem::SetSteamAudioTestControls(const SteamAudioSimulationTestControls& controls)
{
#if defined(ENGINE_HAS_STEAMAUDIO)
    if(m_impl->steamAudioSimulation)
    {
        m_impl->steamAudioSimulation->SetTestControls(controls);
    }
#else
    static_cast<void>(controls);
#endif
}

void* AudioSystem::GetNativeEventForTesting(EventHandle handle) const
{
#if defined(ENGINE_HAS_FMOD)
    return m_impl->ResolveEvent(handle);
#else
    static_cast<void>(handle);
    return nullptr;
#endif
}

std::optional<std::size_t> AudioSystem::CountSteamAudioSpatializers(EventHandle handle) const
{
    const std::optional<SpatializerSummary> summary = DescribeEventSpatializers(handle);

    if(!summary)
    {
        return std::nullopt;
    }

    return summary->steamAudio;
}
}
