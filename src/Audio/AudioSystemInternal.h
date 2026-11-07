#pragma once

// Private to the audio module (AudioSystem.cpp, AudioBanks.cpp): the only headers that include FMOD.

#include "Audio/AudioSystem.h"
#include "Audio/SteamAudioRuntime.h"
#include "Audio/SteamAudioProbes.h"
#include "Audio/SteamAudioSimulation.h"

#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(ENGINE_HAS_FMOD)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fmod.hpp>
#include <fmod_errors.h>
#include <fmod_studio.hpp>
#endif

namespace Audio
{
#if defined(ENGINE_HAS_FMOD)
namespace Internal
{
    enum class Output
    {
        Device,
        NoSound,
        // Verification: FMOD's offline output, with each Studio update mixing synchronously.
        Offline
    };

    // "<FMOD's message> (FMOD_RESULT n)".
    std::string DescribeResult(FMOD_RESULT result);
}
#endif

struct AudioSystem::Impl
{
    Status status = Status::NotStarted;
    std::string statusDetail;
    std::uint64_t updateCount = 0;
    bool claimedProcessSystem = false;
    bool updateFailureReported = false;
    SteamAudioStatus steamAudioStatus = SteamAudioStatus::NotRequested;
    std::string steamAudioDetail;

    bool IsRunning() const noexcept
    {
        return status == Status::Active || status == Status::NoOutput;
    }

#if defined(ENGINE_HAS_FMOD)
    // Owned. Its Core system is borrowed from it and released by Studio::System::release, never directly.
    FMOD::Studio::System* studio = nullptr;
    FMOD::System* core = nullptr;
#if defined(ENGINE_HAS_STEAMAUDIO)
    // SA.1: present while Steam Audio is Active. ReleaseStudioSystem terminates it after FMOD has destroyed every
    // Steam Audio effect (SteamAudioRuntime.h).
    std::unique_ptr<Internal::SteamAudioRuntime> steamAudio;
    // SA.3: the simulation sources and D9's worker; present whenever steamAudio is. It borrows steamAudio, so it goes first.
    std::unique_ptr<Internal::SteamAudioSimulation> steamAudioSimulation;
    // M7: the gameplay session's loaded probes. Gone before the context.
    std::unique_ptr<Internal::SteamAudioProbeBatches> probeBatches;
    // M7.1: at most one generation. Joined before the context goes.
    std::shared_ptr<ProbeGenerationJob> probeJob;
    std::thread probeThread;
    void StopProbeGeneration() noexcept;
#endif

    FMOD_RESULT Start(Internal::Output output, InjectedFailure injectedFailure);
    int softwareSpeakerModeForTesting = 0;
    void ReleaseStudioSystem() noexcept;
    std::string DescribeRunningOutput(Internal::Output output) const;

    // Audio.1B banks: one entry per loaded key. The Bank handle is FMOD's; Engine owns the right to unload it.
    struct BankEntry
    {
        std::string key;
        FMOD::Studio::Bank* bank = nullptr;
        std::uint32_t references = 0;
    };

    // Audio.1B event instances created through AudioSystem. FMOD's handles are validated by FMOD itself
    // (EventInstance::isValid), so an instance destroyed by a bank unload is detected rather than dereferenced.
    struct EventSlot
    {
        FMOD::Studio::EventInstance* instance = nullptr;
        std::uint32_t generation = 0;
    };

    std::vector<BankEntry> banks;
    std::vector<EventSlot> eventSlots;
    std::vector<std::uint32_t> freeEventSlots;

    BankEntry* FindBank(std::string_view key);
    const BankEntry* FindBank(std::string_view key) const;
    bool AddLoadedBank(std::string_view key, FMOD_RESULT result, FMOD::Studio::Bank* bank, std::string& error);
    FMOD::Studio::EventInstance* ResolveEvent(EventHandle handle) const;
    void FreeEventSlot(std::uint32_t index) noexcept;
    // Drops slots whose instance FMOD has destroyed (bank unloaded, or released and finished).
    void ReapEventSlots() noexcept;
    void ReleaseAllEvents() noexcept;
    void UnloadAllBanks() noexcept;
#endif
};
}
