#pragma once

#include "Audio/AudioSystem.h"

#include <string>

// SA.2B: what the Project Settings Audio category reports about Steam Audio. Pure, so every combination of saved
// preference and runtime state is testable without the Editor.
//
// Steam Audio is requested at most once per audio session, when the project opens and before its banks load
// (Engine.cpp, AudioSystem::EnableSteamAudio), and stays as it is until the process ends. The runtime state therefore
// tells whether this session asked for Steam Audio: any status except NotRequested means it did. A saved preference
// that differs from that takes effect only when the project is reloaded.
struct SteamAudioSettingsStatus
{
    enum class State
    {
        // This session runs Steam Audio.
        Active,
        // This session did not request it (the project had it off when it opened).
        Disabled,
        // Requested, but it could not start; FMOD runs without it.
        Unavailable,
        // This Engine build has no Steam Audio.
        NotCompiled,
        // No audio system is attached (no information).
        Unknown
    };

    State state = State::Unknown;
    // One line for the status row.
    std::string summary;
    // Why, when the audio system says (the context/HRTF description, or the failure).
    std::string detail;
    // The saved preference differs from what this session runs: reload the project to apply it.
    bool reloadRequired = false;
};

inline SteamAudioSettingsStatus DescribeSteamAudioSettings(bool savedEnabled, const Audio::AudioSystem* audio)
{
    using State = SteamAudioSettingsStatus::State;
    SteamAudioSettingsStatus result;

    if(!audio)
    {
        result.summary = "Unknown (no audio system)";
        return result;
    }

    const Audio::SteamAudioStatus status = audio->GetSteamAudioStatus();
    result.detail = audio->GetSteamAudioStatusDetail();
    const bool requestedThisSession = status != Audio::SteamAudioStatus::NotRequested;

    switch(status)
    {
    case Audio::SteamAudioStatus::Active:
        result.state = State::Active;
        result.summary = "Active";
        break;
    case Audio::SteamAudioStatus::Unavailable:
        result.state = State::Unavailable;
        result.summary = "Unavailable - FMOD runs without Steam Audio; banks that use Steam Audio effects do not load";
        break;
    case Audio::SteamAudioStatus::NotCompiled:
        result.state = State::NotCompiled;
        result.summary = "Not compiled into this Engine build";
        break;
    case Audio::SteamAudioStatus::NotRequested:
        if(Audio::AudioSystem::IsSteamAudioCompiled())
        {
            result.state = State::Disabled;
            result.summary = "Disabled for this session";
        }
        else
        {
            result.state = State::NotCompiled;
            result.summary = "Not compiled into this Engine build";
        }
        break;
    }

    result.reloadRequired = savedEnabled != requestedThisSession;
    return result;
}
