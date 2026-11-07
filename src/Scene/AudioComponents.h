#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

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
    // Adds this source's own simulated reflections to the shared listener-centric ones.
    bool sourceCentricReflections = false;
    // Source-centric reflections come from baked data. Without that data they are reported unavailable, never simulated in real time instead.
    bool bakedReflections = false;

    bool operator==(const AudioSourceComponent&) const = default;
};

// Hears the scene from its object's world transform during gameplay. The first enabled listener in scene order is
// the active one; without any, the active gameplay camera hears.
struct AudioListenerComponent
{
    bool enabled = true;
    // The shared listener-centric reflections come from baked data. Without that data they are reported unavailable and silent.
    bool bakedReflections = false;

    bool operator==(const AudioListenerComponent&) const = default;
};

// Audio.2B: both audio components of one object, edited and recorded in history as one value.
struct AudioComponentState
{
    std::optional<AudioSourceComponent> source;
    std::optional<AudioListenerComponent> listener;

    bool operator==(const AudioComponentState&) const = default;
};

// M4: a mesh object's inherent acoustic settings. Absent or disabled, it contributes no acoustic geometry. Holds intent only:
// no Steam Audio object, and independent of visual materials.
struct AcousticSettings
{
    static constexpr std::size_t MaximumAssignments = 4096;
    static constexpr std::size_t MaximumTextBytes = 512;

    bool enabled = false;
    // One acoustic material reference per submesh, keyed by AcousticGeometry::SubmeshKey ("<index>:<name>"). A built-in
    // ("builtin:acoustic/concrete") or a project asset ("asset:<uuid>"). Unassigned submeshes use the default material.
    std::map<std::string, std::string> submeshMaterials;

    bool operator==(const AcousticSettings&) const = default;
};

bool ValidateAcousticSettings(const AcousticSettings& settings, std::string* error = nullptr);

// M7. UniformFloor and Centroid are Steam Audio's two placements. Uniform3D is Engine's own grid: the SDK has no such mode, and the grid ignores
// geometry, so probes can land inside solid objects.
enum class ProbePlacement
{
    UniformFloor,
    Centroid,
    Uniform3D
};

// A box of probes centred on its object. Holds intent and a reference only: the probes themselves live in a ProbeBatch asset.
struct ProbeVolumeComponent
{
    // Guards hostile files; far beyond what anyone places by hand.
    static constexpr std::size_t MaximumManualProbes = 1u << 20;
    static constexpr std::size_t MaximumReferenceBytes = 512;

    bool enabled = true;
    ProbePlacement placement = ProbePlacement::UniformFloor;
    // Box edge lengths in the object's local units, so the object's scale scales it too.
    std::array<float, 3> size{10.0f, 3.0f, 10.0f};
    // Metres between neighbouring probes, measured in world space. Floor and Uniform 3D.
    float probeSpacing = 2.0f;
    // Metres above each surface found. Floor only. Steam Audio also skips any surface closer than this to the top of the box.
    float heightAboveFloor = 1.5f;
    // Extra probes in the object's local space, added to whatever the placement produces.
    std::vector<std::array<float, 3>> manualProbes;
    // Drops generated probes that lie inside provably closed geometry. Floor and Uniform 3D; manual probes are always kept.
    bool filterInsideSolids = true;
    // "asset:<uuid>" of the generated ProbeBatch; empty until generated.
    std::string probeBatch;

    bool operator==(const ProbeVolumeComponent&) const = default;
};

bool ValidateProbeVolume(const ProbeVolumeComponent& volume, std::string* error = nullptr);

// Lexical contract shared by authoring and both codecs. Whether the event exists is decided by the loaded banks at
// Play, never by scene loading.
bool ValidateAudioSourceComponent(const AudioSourceComponent& source, std::string* error = nullptr);
