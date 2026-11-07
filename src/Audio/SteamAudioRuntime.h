#pragma once

// Private to the audio module (SA.1): Steam Audio's FMOD integration as AudioSystem owns it. Include only from the
// audio module's .cpp files; it brings in FMOD (and, when compiled, Steam Audio) headers.
//
// Ownership and lifetime contract (verified against Steam Audio 4.8.1 and FMOD 2.04.00):
//
// * Two DLLs, one Steam Audio. phonon_fmod.dll (the FMOD plugin) never links phonon.dll: in a game it uses only the
//   IPLContext handed to iplFMODInitialize, through that object's interfaces. Engine loads phonon.dll by full path
//   from the plugin's directory (the executable's), which is the same file the plugin itself would load, so the
//   process holds one phonon.dll and the plugin works on Engine's context, HRTF and (later) sources.
// * The plugin keeps process-global state (context, HRTF, source registry) that iplFMODInitialize fills and
//   iplFMODTerminate clears. Initialize asserts it is empty, so it runs once per Start/Terminate pair; one
//   AudioSystem per process (AudioSystem's own rule) makes that one pair at a time.
// * Order at start: phonon.dll, context, HRTF, plugin registration (FMOD::System::loadPlugin) and its validation,
//   iplFMODInitialize, iplFMODSetHRTF -- all before any bank loads. A bank whose events use Steam Audio effects
//   fails to load without the plugin (FMOD_ERR_PLUGIN_MISSING), and a Steam Audio DSP that processes before
//   iplFMODInitialize outputs silence in a game (the plugin creates its own context only inside FMOD Studio).
// * Order at shutdown: every Steam Audio DSP must be gone before iplFMODTerminate, and FMOD destroys them in
//   Studio::System::release. FMOD also unloads phonon_fmod.dll there, so Engine pins the module with its own
//   reference: Terminate runs after the Studio system is released, then releases the HRTF and context, then the pin.
// * The HRTF is created for FMOD's software mixer format (sample rate and DSP block size), fixed when the Studio
//   system initializes and unchanged by output device changes, which FMOD handles internally.
//
// DSP identification and binding (binding is used by SteamAudioSimulation, SA.3): FMOD 2.04 registers the plugin's
// legacy (plugin SDK 110) descriptions as copies, so a Steam Audio Spatializer instance is recognised by identity
// with FMOD's registered copy, never by name: the registered description is the one whose create callback is the
// spatializer's own exported code, and an instance belongs to it when every parameter descriptor it reports is that
// description's descriptor object.

#include "Audio/AudioSystem.h"

#include <cstdint>
#include <string>

#if defined(ENGINE_HAS_FMOD)
#include <fmod.hpp>
#endif

#if defined(ENGINE_HAS_STEAMAUDIO)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <phonon.h>
#endif

namespace Audio::Internal
{
#if defined(ENGINE_HAS_STEAMAUDIO)
inline constexpr int SteamAudioReflectionMaxOrder = 1;
inline constexpr float SteamAudioReflectionMaxDuration = 1.0f;

class SteamAudioRuntime final
{
public:
    SteamAudioRuntime() = default;
    ~SteamAudioRuntime();
    SteamAudioRuntime(const SteamAudioRuntime&) = delete;
    SteamAudioRuntime& operator=(const SteamAudioRuntime&) = delete;

    // Brings Steam Audio up inside `core`'s running system. On failure everything created so far is released and
    // FMOD is left exactly as it was (no plugin registered). Main thread; no bank may be loaded yet.
    bool Start(FMOD::System& core, SteamAudioInjectedFailure injectedFailure, std::string& error);

    // Only after the Studio system that owned `core` has been released (no Steam Audio DSP may exist). Idempotent.
    void Terminate() noexcept;

    bool IsStarted() const noexcept { return m_started; }
    // "Steam Audio 4.8.1, HRTF at 48000 Hz / 512 frames" for logs and diagnostics.
    const std::string& GetDescription() const noexcept { return m_description; }

    // True for a Steam Audio Spatializer instance of the registered plugin (see the identification rule above).
    bool IsSpatializer(FMOD::DSP& dsp) const;
    // SA.2: whether a spatializer (IsSpatializer) applies distance attenuation as authored. FMOD Studio stores the
    // effect's UI default, Off, unless the author chooses Curve-Driven or Physics-Based. Unreadable counts as on.
    bool HasDistanceAttenuation(FMOD::DSP& spatializer) const;
    // M6.5: read only, the effect's Apply Reflections as authored in FMOD Studio. Engine never writes it. Unreadable counts as off.
    bool AppliesReflections(FMOD::DSP& spatializer) const;

    // SA.3: binds `source` to a new spatializer, once, before it processes. The plugin's handle exists only inside
    // this call: its registry recycles handles, and the DSP keeps its own reference to the source. Thread-safe.
    bool BindSimulationSource(FMOD::DSP& spatializer, IPLSource source) const;

    // M6.4: process-wide in the plugin until Terminate, and every spatializer reads it. Before any bank; never changed while
    // DSPs exist. False when the plugin lacks the entry point.
    bool SetPluginSimulationSettings(const IPLSimulationSettings& settings) const;
    // M6.7. False when the plugin lacks the export.
    bool SetPluginReverbSource(IPLSource source) const;
    // One of the plugin's effects, unconnected. The caller owns it. Null when the plugin has no effect of that name.
    FMOD::DSP* CreateEffectByName(FMOD::System& core, std::string_view name) const;
    void SetAppliesReflections(FMOD::DSP& spatializer, bool applies) const;

    // Verification only, before Start.
    void SetContextAllocatorForTesting(void* (*allocate)(std::size_t, std::size_t), void (*release)(void*), bool useSdkAllocator) noexcept
    {
        m_testAllocate = allocate;
        m_testFree = release;
        m_useSdkAllocator = useSdkAllocator;
    }

    // M6.6: sizes the mixer pool from what this session's effects really allocate at the mixer's format. Main thread, before any bank.
    // On failure Steam Audio simply stays on the heap, as a warning says.
    void ArmMixerPool(FMOD::System& core, bool reflectionsActive, const SteamAudioOptions& options);
    // Main thread, every update: hands back heap blocks freed during a mix, and reports exhaustion once per occurrence.
    void UpdateMixerPool() noexcept;
    SteamAudioMixerPoolStats GetMixerPoolStats() const noexcept;

    // D9 handshake only: a real spatializer connected to nothing, which no mixer block can reach; and the bind above,
    // returning the source's reference count as the plugin leaves it, for a source whose owner holds one reference.
    FMOD::DSP* CreateUnconnectedSpatializer(FMOD::System& core) const;
    int BindSimulationSourceForHandshake(FMOD::DSP& spatializer, IPLSource source) const;

    IPLContext GetContext() const noexcept { return m_context; }
    int GetSampleRate() const noexcept { return m_sampleRate; }
    int GetBlockLength() const noexcept { return m_blockLength; }

private:
    bool LoadRuntime(SteamAudioInjectedFailure injectedFailure, std::string& error);
    bool CreateContextAndHRTF(FMOD::System& core, SteamAudioInjectedFailure injectedFailure, std::string& error);
    bool RegisterPlugin(FMOD::System& core, SteamAudioInjectedFailure injectedFailure, std::string& error);
    void ReleaseUnstarted(FMOD::System& core) noexcept;

    // The pin on phonon_fmod.dll; held from Start until after iplFMODTerminate.
    HMODULE m_plugin = nullptr;
    unsigned int m_pluginHandle = 0;
    bool m_pluginRegistered = false;
    // FMOD's registered copy of the spatializer description; valid while the plugin stays registered.
    const FMOD_DSP_DESCRIPTION* m_spatializer = nullptr;
    IPLContext m_context = nullptr;
    IPLHRTF m_hrtf = nullptr;
    bool m_started = false;
    std::string m_description;

    using InitializeFunction = void(F_CALL*)(IPLContext);
    using TerminateFunction = void(F_CALL*)();
    using SetHRTFFunction = void(F_CALL*)(IPLHRTF);
    InitializeFunction m_initialize = nullptr;
    TerminateFunction m_terminate = nullptr;
    SetHRTFFunction m_setHRTF = nullptr;

    using AddSourceFunction = IPLint32(F_CALL*)(IPLSource);
    using RemoveSourceFunction = void(F_CALL*)(IPLint32);
    AddSourceFunction m_addSource = nullptr;
    RemoveSourceFunction m_removeSource = nullptr;
    using SetSimulationSettingsFunction = void(F_CALL*)(IPLSimulationSettings);
    SetSimulationSettingsFunction m_setSimulationSettings = nullptr;
    using SetReverbSourceFunction = void(F_CALL*)(IPLSource);
    SetReverbSourceFunction m_setReverbSource = nullptr;
    void* (*m_testAllocate)(std::size_t, std::size_t) = nullptr;
    void (*m_testFree)(void*) = nullptr;
    bool m_useSdkAllocator = false;
    bool m_usesMixerAllocator = false;
    std::uint64_t m_reportedMixerHeapAllocations = 0;
    std::uint64_t m_reportedBadFrees = 0;
    bool m_reportedLargerClass = false;
    int m_sampleRate = 0;
    int m_blockLength = 0;
};
#endif
}
