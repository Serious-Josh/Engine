#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
    // M9.5: the sound also reaches the listener along baked paths around obstacles, where the direct path is blocked. Needs a Probe Volume
    // whose pathing is baked and current, covering both source and listener; otherwise the source sounds exactly as it would without.
    bool pathing = false;

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

// M8.2: how one Probe Volume bakes its reflections. Each value is passed to the Steam Audio baker as it is; there are no presets.
// Ambisonic order and duration are not here: they follow the simulator (SteamAudioReflectionMaxOrder and MaxDuration), so a bake
// always matches what plays it. M8.1 measured every field below changing the baked output, so each is in the bake fingerprint.
struct ReflectionBakeSettings
{
    // The SDK accepts zero rays and bakes a silent layer without complaint (M8.1), so zero is refused here. The upper limits
    // guard hostile files and runaway values; they are not quality advice.
    static constexpr std::uint32_t MinimumRays = 1;
    static constexpr std::uint32_t MaximumRays = 1u << 20;
    static constexpr std::uint32_t MinimumDiffuseSamples = 1;
    static constexpr std::uint32_t MaximumDiffuseSamples = 1u << 16;
    static constexpr std::uint32_t MinimumBounces = 1;
    static constexpr std::uint32_t MaximumBounces = 256;
    // Metres. Engine's realtime reflections use 1.0, as the SDK's own baking example does.
    static constexpr float MinimumIrradianceDistance = 0.01f;
    static constexpr float MaximumIrradianceDistance = 100.0f;

    std::uint32_t rays = 16384;
    std::uint32_t diffuseSamples = 1024;
    std::uint32_t bounces = 16;
    float irradianceMinimumDistance = 1.0f;

    bool operator==(const ReflectionBakeSettings&) const = default;
};

bool ValidateReflectionBakeSettings(const ReflectionBakeSettings& settings, std::string* error = nullptr);

// M9.4: how one Probe Volume bakes pathing, passed to iplPathBakerBake as it is. Pathing is source-independent: one layer per volume.
// The defaults are the values M9.0 and M9.3 measured with. Materials never change the bake (M9.4 E7), so nothing about them is here.
struct PathingBakeSettings
{
    // numSamples * numSamples rays test whether two probes see each other.
    static constexpr std::uint32_t MinimumSamples = 1;
    static constexpr std::uint32_t MaximumSamples = 64;
    // Metres. The sphere each probe's samples are taken in, and the farthest two probes may be to see each other or to be joined by a path.
    static constexpr float MinimumRadius = 0.01f;
    static constexpr float MaximumRadius = 10.0f;
    static constexpr float MaximumRange = 10000.0f;

    std::uint32_t samples = 4;
    float radius = 0.5f;
    // The fraction of unoccluded rays above which two probes see each other.
    float threshold = 0.1f;
    float visibilityRange = 50.0f;
    float pathRange = 100.0f;

    bool operator==(const PathingBakeSettings&) const = default;
};

// Inline, because the audio module's baker checks requests with it and does not link the scene module.
inline bool ValidatePathingBakeSettings(const PathingBakeSettings& settings, std::string* error = nullptr)
{
    using Limits = PathingBakeSettings;
    const char* reason = nullptr;
    const auto inRange = [](float value, float minimum, float maximum)
    {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    };

    if(settings.samples < Limits::MinimumSamples || settings.samples > Limits::MaximumSamples)
    {
        reason = "Pathing samples must be between 1 and 64.";
    }
    else if(!inRange(settings.radius, Limits::MinimumRadius, Limits::MaximumRadius))
    {
        reason = "Pathing radius must be between 0.01 and 10 metres.";
    }
    else if(!inRange(settings.threshold, 0.0f, 1.0f))
    {
        reason = "Pathing threshold must be between 0 and 1.";
    }
    else if(!inRange(settings.visibilityRange, Limits::MinimumRadius, Limits::MaximumRange) ||
        !inRange(settings.pathRange, Limits::MinimumRadius, Limits::MaximumRange))
    {
        reason = "Pathing visibility and path ranges must be between 0.01 and 10000 metres.";
    }

    if(reason && error)
    {
        *error = reason;
    }

    return reason == nullptr;
}

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
    // M8.2: what the next bake uses.
    ReflectionBakeSettings bakeSettings;
    // "asset:<uuid>" of the last published BakedReflections; empty until baked. Kept while stale, until a replacement is published.
    std::string bakedReflections;
    // M9.5: whether this volume's baked pathing is used at Play. Off keeps the asset but registers nothing.
    bool pathingEnabled = true;
    // M9.4: what the next pathing bake uses.
    PathingBakeSettings pathingSettings;
    // "asset:<uuid>" of the last published BakedPathing; empty until baked. Independent of bakedReflections in every way.
    std::string bakedPathing;

    bool operator==(const ProbeVolumeComponent&) const = default;
};

bool ValidateProbeVolume(const ProbeVolumeComponent& volume, std::string* error = nullptr);

// Lexical contract shared by authoring and both codecs. Whether the event exists is decided by the loaded banks at
// Play, never by scene loading.
bool ValidateAudioSourceComponent(const AudioSourceComponent& source, std::string* error = nullptr);
