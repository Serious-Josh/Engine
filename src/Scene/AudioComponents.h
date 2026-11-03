#pragma once

#include <cstddef>
#include <optional>
#include <string>

// Audio.2A authored audio components. Like every scene component they hold intent only: no event instance, handle
// or FMOD state. Playback belongs to the gameplay audio session of one Play (GameAudioSession).
//
// FMOD Studio owns the sound design: whether an event is 3D, loops, how it attenuates and spatializes are properties
// of the event in its bank, so none of them is duplicated here.

// Plays one FMOD Studio event at its object's world position during gameplay.
struct AudioSourceComponent
{
    static constexpr std::size_t MaximumEventPathBytes = 512;

    bool enabled = true;
    // The event's FMOD Studio GUID, "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" (lowercase or uppercase hex); empty until
    // assigned. The stable reference: it survives renames and moves in FMOD Studio and needs no strings bank.
    std::string eventGuid;
    // "event:/..." as last seen, for display and diagnostics only; playback never reads it.
    std::string eventPath;
    // Starts when gameplay starts (after every script's OnStart).
    bool autoplay = true;

    bool operator==(const AudioSourceComponent&) const = default;
};

// Hears the scene from its object's world transform during gameplay. The first enabled listener in scene order is
// the active one; without any, the active gameplay camera hears.
struct AudioListenerComponent
{
    bool enabled = true;

    bool operator==(const AudioListenerComponent&) const = default;
};

// Audio.2B: both audio components of one object, edited and recorded in history as one value.
struct AudioComponentState
{
    std::optional<AudioSourceComponent> source;
    std::optional<AudioListenerComponent> listener;

    bool operator==(const AudioComponentState&) const = default;
};

// Lexical contract shared by authoring and both codecs. Whether the event exists is decided by the loaded banks at
// Play, never by scene loading.
bool ValidateAudioSourceComponent(const AudioSourceComponent& source, std::string* error = nullptr);
