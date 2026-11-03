#include "Audio/AudioSystemInternal.h"
#include "Core/Log.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
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

    if(result == FMOD_OK && output == Output::NoSound)
    {
        result = core->setOutput(FMOD_OUTPUTTYPE_NOSOUND);
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
            result = studio->initialize(MaximumChannels, FMOD_STUDIO_INIT_NORMAL, FMOD_INIT_NORMAL, nullptr);
        }
    }

    if(result != FMOD_OK)
    {
        ReleaseStudioSystem();
    }

    return result;
}

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

    if(output == Output::NoSound)
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
}
