#include "Audio/AudioSystemInternal.h"

#if defined(ENGINE_HAS_STEAMAUDIO)
#include "Audio/SteamAudioSnapshotSource.h"

#include <atomic>
#include <filesystem>
#include <string_view>
#endif

namespace Audio::Internal
{
#if defined(ENGINE_HAS_STEAMAUDIO)
namespace
{
    // FMOD_DSP_DESCRIPTION as laid out by FMOD's plugin SDK 110 (FMOD 2.03 and earlier), which Steam Audio 4.8.1's
    // plugin is built against, up to the members identification reads. FMOD 2.04 (plugin SDK 204) changed the
    // layout and registers such legacy descriptions as converted copies, so the exported original is read through
    // this mirror and never through FMOD's own type.
    struct LegacyDspDescription
    {
        unsigned int pluginsdkversion;
        char name[32];
        unsigned int version;
        int numinputbuffers;
        int numoutputbuffers;
        void* create;
        void* release;
        void* reset;
        void* read;
        void* process;
        void* setposition;
        int numparameters;
        FMOD_DSP_PARAMETER_DESC** paramdesc;
    };

    constexpr unsigned int LegacyPluginSdkVersion = 110;
    // Spatializer, Mixer Return, Reverb.
    constexpr int PluginEffectCount = 3;
    constexpr int SpatializerParameterCount = 36;
    // IPL_SPATIALIZE_SIMULATION_OUTPUTS_HANDLE: where later milestones bind simulation results. Checked by name and
    // type here so a reordered parameter list is refused rather than written to.
    constexpr int SimulationOutputsHandleParameter = 33;
    constexpr std::string_view SimulationOutputsHandleName = "SimOutHandle";
    // IPL_SPATIALIZE_APPLY_DISTANCEATTENUATION: Off (0), Physics-Based (1) or Curve-Driven (2), as authored.
    constexpr int DistanceAttenuationParameter = 2;
    constexpr std::string_view DistanceAttenuationName = "ApplyDA";

    // phonon.dll stays loaded for the process once loaded: the delay-load thunks keep pointing into it.
    std::atomic<bool> phononLoaded{false};

    std::filesystem::path ExecutableDirectory()
    {
        std::wstring path(MAX_PATH, L'\0');

        for(;;)
        {
            const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));

            if(length == 0)
            {
                return {};
            }

            if(length < path.size())
            {
                path.resize(length);
                return std::filesystem::path(path).parent_path();
            }

            path.resize(path.size() * 2);
        }
    }

    std::string DescribeLastError()
    {
        return "Windows error " + std::to_string(GetLastError());
    }

    std::string DescribeStatus(IPLerror status)
    {
        switch(status)
        {
        case IPL_STATUS_SUCCESS:
            return "success";
        case IPL_STATUS_FAILURE:
            return "failure";
        case IPL_STATUS_OUTOFMEMORY:
            return "out of memory";
        case IPL_STATUS_INITIALIZATION:
            return "initialization error";
        }

        return "IPLerror " + std::to_string(static_cast<int>(status));
    }

    template<typename Function>
    Function Resolve(HMODULE module, const char* name)
    {
        return reinterpret_cast<Function>(GetProcAddress(module, name));
    }

    bool SameParameter(const FMOD_DSP_PARAMETER_DESC* parameter, std::string_view name, FMOD_DSP_PARAMETER_TYPE type)
    {
        return parameter && parameter->type == type && std::string_view(parameter->name) == name;
    }
}

SteamAudioRuntime::~SteamAudioRuntime()
{
    // AudioSystem terminates explicitly after releasing FMOD; this only covers an owner that did not.
    Terminate();
}

bool SteamAudioRuntime::Start(FMOD::System& core, SteamAudioInjectedFailure injectedFailure, std::string& error)
{
    if(m_started)
    {
        return true;
    }

    if(!LoadRuntime(injectedFailure, error) || !CreateContextAndHRTF(core, injectedFailure, error) ||
        !RegisterPlugin(core, injectedFailure, error))
    {
        ReleaseUnstarted(core);
        return false;
    }

    // From here on every Steam Audio DSP FMOD creates uses Engine's context and HRTF.
    m_initialize(m_context);
    m_setHRTF(m_hrtf);
    m_started = true;
    return true;
}

bool SteamAudioRuntime::LoadRuntime(SteamAudioInjectedFailure injectedFailure, std::string& error)
{
    const std::filesystem::path directory = ExecutableDirectory();
    const std::filesystem::path phonon = directory / "phonon.dll";
    const std::filesystem::path plugin = directory / "phonon_fmod.dll";
    std::error_code ignored;

    if(injectedFailure == SteamAudioInjectedFailure::MissingRuntime || !std::filesystem::is_regular_file(phonon, ignored) ||
        !std::filesystem::is_regular_file(plugin, ignored))
    {
        error = "phonon.dll and phonon_fmod.dll were not both found beside the executable";
        return false;
    }

    constexpr DWORD searchFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;

    // By full path, before any Steam Audio call: the delay-loaded imports then bind to this module, the one the
    // plugin also resolves beside itself.
    if(!phononLoaded.load())
    {
        if(!LoadLibraryExW(phonon.c_str(), nullptr, searchFlags))
        {
            error = "cannot load phonon.dll (" + DescribeLastError() + ")";
            return false;
        }

        phononLoaded.store(true);
    }

    m_plugin = LoadLibraryExW(plugin.c_str(), nullptr, searchFlags);

    if(!m_plugin)
    {
        error = "cannot load phonon_fmod.dll (" + DescribeLastError() + ")";
        return false;
    }

    using GetVersionFunction = void(F_CALL*)(unsigned int*, unsigned int*, unsigned int*);
    const auto getVersion = Resolve<GetVersionFunction>(m_plugin, "iplFMODGetVersion");
    m_initialize = Resolve<InitializeFunction>(m_plugin, "iplFMODInitialize");
    m_terminate = Resolve<TerminateFunction>(m_plugin, "iplFMODTerminate");
    m_setHRTF = Resolve<SetHRTFFunction>(m_plugin, "iplFMODSetHRTF");
    m_addSource = Resolve<AddSourceFunction>(m_plugin, "iplFMODAddSource");
    m_removeSource = Resolve<RemoveSourceFunction>(m_plugin, "iplFMODRemoveSource");

    if(!getVersion || !m_initialize || !m_terminate || !m_setHRTF || !m_addSource || !m_removeSource)
    {
        error = "phonon_fmod.dll lacks the Steam Audio FMOD integration API";
        return false;
    }

    unsigned int major = 0;
    unsigned int minor = 0;
    unsigned int patch = 0;
    getVersion(&major, &minor, &patch);

    if(major != STEAMAUDIO_VERSION_MAJOR || minor != STEAMAUDIO_VERSION_MINOR || patch != STEAMAUDIO_VERSION_PATCH)
    {
        error = "phonon_fmod.dll is Steam Audio " + std::to_string(major) + "." + std::to_string(minor) + "." +
            std::to_string(patch) + ", but Engine was built for " + std::to_string(STEAMAUDIO_VERSION_MAJOR) + "." +
            std::to_string(STEAMAUDIO_VERSION_MINOR) + "." + std::to_string(STEAMAUDIO_VERSION_PATCH);
        return false;
    }

    return true;
}

bool SteamAudioRuntime::CreateContextAndHRTF(FMOD::System& core, SteamAudioInjectedFailure injectedFailure,
    std::string& error)
{
    IPLContextSettings contextSettings{};
    // Context creation fails when phonon.dll does not implement this API version.
    contextSettings.version = STEAMAUDIO_VERSION;
    // The level the FMOD plugin itself selects; higher levels can lower clocks for little gain.
    contextSettings.simdLevel = IPL_SIMDLEVEL_AVX2;

    IPLerror status = IPL_STATUS_INITIALIZATION;

    if(injectedFailure != SteamAudioInjectedFailure::ContextCreation)
    {
        status = iplContextCreate(&contextSettings, &m_context);
    }

    if(status != IPL_STATUS_SUCCESS)
    {
        m_context = nullptr;
        error = "Steam Audio context creation failed (" + DescribeStatus(status) + ")";
        return false;
    }

    // The HRTF matches FMOD's mixer, which every DSP processes at: its sample rate and block size are fixed for the
    // life of the Studio system.
    int sampleRate = 0;
    FMOD_SPEAKERMODE speakerMode = FMOD_SPEAKERMODE_DEFAULT;
    int rawSpeakers = 0;
    unsigned int blockLength = 0;
    int blockCount = 0;
    FMOD_RESULT result = core.getSoftwareFormat(&sampleRate, &speakerMode, &rawSpeakers);

    if(result == FMOD_OK)
    {
        result = core.getDSPBufferSize(&blockLength, &blockCount);
    }

    if(result != FMOD_OK)
    {
        error = "cannot read FMOD's mixer format: " + DescribeResult(result);
        return false;
    }

    IPLAudioSettings audioSettings{sampleRate, static_cast<IPLint32>(blockLength)};
    IPLHRTFSettings hrtfSettings{};
    hrtfSettings.type = IPL_HRTFTYPE_DEFAULT;
    hrtfSettings.volume = 1.0f;
    status = IPL_STATUS_FAILURE;

    if(injectedFailure != SteamAudioInjectedFailure::HRTFCreation)
    {
        status = iplHRTFCreate(m_context, &audioSettings, &hrtfSettings, &m_hrtf);
    }

    if(status != IPL_STATUS_SUCCESS)
    {
        m_hrtf = nullptr;
        error = "Steam Audio HRTF creation failed (" + DescribeStatus(status) + ")";
        return false;
    }

    m_sampleRate = sampleRate;
    m_blockLength = static_cast<int>(blockLength);
    m_description = "Steam Audio " + std::to_string(STEAMAUDIO_VERSION_MAJOR) + "." +
        std::to_string(STEAMAUDIO_VERSION_MINOR) + "." + std::to_string(STEAMAUDIO_VERSION_PATCH) + ", HRTF at " +
        std::to_string(sampleRate) + " Hz / " + std::to_string(blockLength) + " frames";
    return true;
}

bool SteamAudioRuntime::RegisterPlugin(FMOD::System& core, SteamAudioInjectedFailure injectedFailure,
    std::string& error)
{
    if(injectedFailure == SteamAudioInjectedFailure::PluginLoad)
    {
        error = "FMOD refused the Steam Audio plugin: " + DescribeResult(FMOD_ERR_PLUGIN);
        return false;
    }

    const std::filesystem::path plugin = ExecutableDirectory() / "phonon_fmod.dll";
    const std::u8string path = plugin.u8string();
    const FMOD_RESULT result = core.loadPlugin(reinterpret_cast<const char*>(path.c_str()), &m_pluginHandle);

    if(result != FMOD_OK)
    {
        error = "FMOD refused the Steam Audio plugin: " + DescribeResult(result);
        return false;
    }

    m_pluginRegistered = true;

    // The spatializer exactly as the plugin defines it, to recognise FMOD's registered copy by its code.
    using GetDescriptionFunction = const LegacyDspDescription*(F_CALL*)();
    const auto getDescription = Resolve<GetDescriptionFunction>(m_plugin, "FMOD_SteamAudio_Spatialize_GetDSPDescription");
    const LegacyDspDescription* exported = getDescription ? getDescription() : nullptr;

    const bool exportedValid = exported && exported->pluginsdkversion == LegacyPluginSdkVersion &&
        exported->create && exported->numparameters == SpatializerParameterCount && exported->paramdesc &&
        SameParameter(exported->paramdesc[SimulationOutputsHandleParameter], SimulationOutputsHandleName,
            FMOD_DSP_PARAMETER_TYPE_INT);

    int nestedCount = 0;
    const FMOD_DSP_DESCRIPTION* spatializer = nullptr;
    int matches = 0;

    if(exportedValid && core.getNumNestedPlugins(m_pluginHandle, &nestedCount) == FMOD_OK &&
        nestedCount == PluginEffectCount)
    {
        for(int index = 0; index < nestedCount; ++index)
        {
            unsigned int nested = 0;
            const FMOD_DSP_DESCRIPTION* description = nullptr;

            if(core.getNestedPlugin(m_pluginHandle, index, &nested) == FMOD_OK &&
                core.getDSPInfoByPlugin(nested, &description) == FMOD_OK && description &&
                reinterpret_cast<void*>(description->create) == exported->create)
            {
                spatializer = description;
                ++matches;
            }
        }
    }

    const bool registeredValid = matches == 1 && spatializer->numparameters == SpatializerParameterCount &&
        spatializer->paramdesc &&
        SameParameter(spatializer->paramdesc[SimulationOutputsHandleParameter], SimulationOutputsHandleName,
            FMOD_DSP_PARAMETER_TYPE_INT) &&
        SameParameter(spatializer->paramdesc[DistanceAttenuationParameter], DistanceAttenuationName,
            FMOD_DSP_PARAMETER_TYPE_INT);

    if(injectedFailure == SteamAudioInjectedFailure::PluginIdentity || !registeredValid)
    {
        error = "the Steam Audio plugin loaded, but its spatializer does not match the verified Steam Audio " +
            std::to_string(STEAMAUDIO_VERSION_MAJOR) + "." + std::to_string(STEAMAUDIO_VERSION_MINOR) + "." +
            std::to_string(STEAMAUDIO_VERSION_PATCH) + " plugin layout";
        return false;
    }

    m_spatializer = spatializer;
    return true;
}

void SteamAudioRuntime::ReleaseUnstarted(FMOD::System& core) noexcept
{
    // No bank is loaded yet, so no Steam Audio DSP exists and the registration can simply be withdrawn.
    if(m_pluginRegistered)
    {
        core.unloadPlugin(m_pluginHandle);
        m_pluginRegistered = false;
    }

    m_spatializer = nullptr;

    // Only objects that exist: any Steam Audio call, even a release of nothing, binds the delay-loaded phonon.dll,
    // which raises a structured exception when phonon.dll is the missing piece.
    if(m_hrtf)
    {
        iplHRTFRelease(&m_hrtf);
    }

    if(m_context)
    {
        iplContextRelease(&m_context);
    }

    if(m_plugin)
    {
        FreeLibrary(m_plugin);
        m_plugin = nullptr;
    }

    m_description.clear();
}

void SteamAudioRuntime::Terminate() noexcept
{
    if(!m_started)
    {
        return;
    }

    // The Studio system is gone, and with it every Steam Audio DSP and the plugin registration; the pin kept the
    // plugin's globals alive for this call.
    m_terminate();
    m_started = false;
    m_pluginRegistered = false;
    m_spatializer = nullptr;
    iplHRTFRelease(&m_hrtf);
    iplContextRelease(&m_context);
    FreeLibrary(m_plugin);
    m_plugin = nullptr;
    m_description.clear();
}

bool SteamAudioRuntime::IsSpatializer(FMOD::DSP& dsp) const
{
    if(!m_spatializer)
    {
        return false;
    }

    int parameterCount = 0;

    if(dsp.getNumParameters(&parameterCount) != FMOD_OK || parameterCount != m_spatializer->numparameters)
    {
        return false;
    }

    for(int index = 0; index < parameterCount; ++index)
    {
        FMOD_DSP_PARAMETER_DESC* parameter = nullptr;

        if(dsp.getParameterInfo(index, &parameter) != FMOD_OK || parameter != m_spatializer->paramdesc[index])
        {
            return false;
        }
    }

    return true;
}

bool SteamAudioRuntime::HasDistanceAttenuation(FMOD::DSP& spatializer) const
{
    // Read only (no rebinding or reconfiguration): the effect's value as authored in FMOD Studio.
    int mode = 0;
    return spatializer.getParameterInt(DistanceAttenuationParameter, &mode, nullptr, 0) != FMOD_OK || mode != 0;
}

FMOD::DSP* SteamAudioRuntime::CreateUnconnectedSpatializer(FMOD::System& core) const
{
    int nestedCount = 0;

    if(core.getNumNestedPlugins(m_pluginHandle, &nestedCount) != FMOD_OK)
    {
        return nullptr;
    }

    for(int index = 0; index < nestedCount; ++index)
    {
        unsigned int nested = 0;
        const FMOD_DSP_DESCRIPTION* description = nullptr;

        if(core.getNestedPlugin(m_pluginHandle, index, &nested) == FMOD_OK &&
            core.getDSPInfoByPlugin(nested, &description) == FMOD_OK && description == m_spatializer)
        {
            FMOD::DSP* dsp = nullptr;
            return core.createDSPByPlugin(nested, &dsp) == FMOD_OK ? dsp : nullptr;
        }
    }

    return nullptr;
}

int SteamAudioRuntime::BindSimulationSourceForHandshake(FMOD::DSP& spatializer, IPLSource source) const
{
    BindSimulationSource(spatializer, source);

    // Retain and release give the count: retain returns the object, and the interface has no other way to read it.
    auto* snapshot = reinterpret_cast<SnapshotSource*>(reinterpret_cast<api::ISource*>(source));
    return snapshot->References();
}

bool SteamAudioRuntime::BindSimulationSource(FMOD::DSP& spatializer, IPLSource source) const
{
    const IPLint32 handle = m_addSource(source);

    if(handle < 0)
    {
        return false;
    }

    // The plugin resolves the handle and retains the source before this returns, so the handle can go at once: on
    // every path, so a failure leaves nothing behind for a later source to recycle.
    const FMOD_RESULT result = spatializer.setParameterInt(SimulationOutputsHandleParameter, handle);
    m_removeSource(handle);
    return result == FMOD_OK;
}
#endif
}
