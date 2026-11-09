#pragma once

#include "Audio/AudioSystem.h"
#include "Engine/AcousticGeometry.h"
#include "Engine/ProbeVolumes.h"
#include "Scene/AudioComponents.h"
#include "Scene/SceneDocument.h"
#include "assets/BakedPathingAsset.h"
#include "assets/ProbeBatchAsset.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// M9.4: whether a Probe Volume's baked pathing still describes the scene, and the bake request and asset bytes for it. Pure: it bakes nothing,
// reads no files and touches no Steam Audio object.
//
// Pathing depends on the volume's probes, the acoustic geometry (positions, triangles and transforms; ReflectionBakes::HashAcousticScene's
// geometry hash) and its own bake settings, and nothing else. Materials do not change a pathing bake at all (M9.4 E7: byte-identical batches
// from very different absorption, scattering and transmission), and reflection settings are not inputs, so neither ever makes pathing stale.
// It is judged independently of the volume's baked reflections.
namespace PathingBakes
{
// What a bake would read now. `probes` is the volume's current probe asset, and only when ProbeVolumes reports it Valid.
struct Inputs
{
    std::string probeReference;
    const ProbeBatchAssets::Data* probes = nullptr;
    std::uint64_t geometryHash = 0;
    PathingBakeSettings settings;
};

std::uint64_t HashSettings(const PathingBakeSettings& settings) noexcept;
// Combines the probe asset's reference, fingerprint and positions, the geometry, the settings, and the format and SDK versions. Needs probes.
std::uint64_t Fingerprint(const Inputs& inputs) noexcept;

enum class State
{
    // The volume has no current probes: generate them first.
    NoProbes,
    // Nothing referenced. Not a warning.
    NotBaked,
    // Referenced, but no registered file.
    Missing,
    // The file does not decode.
    Corrupt,
    Stale,
    Valid
};

// Most important first: the first that applies is reported.
enum class StaleReason
{
    None,
    // Another format or SDK version.
    Version,
    // Other probes, or another probe asset.
    Probes,
    Geometry,
    Settings
};

struct Status
{
    State state = State::NoProbes;
    StaleReason reason = StaleReason::None;
    std::string message;
};

// `asset` is the volume's referenced asset as resolved, or null when it references none.
Status Evaluate(const Inputs& current, const BakedPathingAssets::Resolution* asset);

// Null-safe: no request without probes.
std::optional<Audio::PathingBakeRequest> MakeRequest(const Inputs& inputs, std::vector<Audio::ProbeGeometryObject> geometry);

struct Assembly
{
    std::vector<std::byte> encoded;
    // Empty on success.
    std::string error;
};

// A successful bake, from exactly `bakedFrom`, as the bytes to store: encoded and decoded once to prove they round-trip.
Assembly Assemble(const Inputs& bakedFrom, const Audio::PathingBakeResult& result);

// M9.5: what a Play registers. Pure: reads assets through the host's loaders.
struct Loaders
{
    std::function<std::optional<ProbeBatchAssets::Data>(const std::string& reference, std::string* error)> probes;
    std::function<BakedPathingAssets::Resolution(const std::string& reference)> pathing;
};

struct VolumeReport
{
    std::string name;
    bool eligible = false;
    // Why not, when not.
    std::string reason;
};

struct Selection
{
    // In scene order, which is also the priority order where volumes overlap.
    std::vector<Audio::AudioSystem::BakedReverbVolume> volumes;
    // Volumes with pathing enabled and a pathing reference, eligible or not.
    std::vector<VolumeReport> reports;
};

// Every enabled Probe Volume with pathing enabled and a baked pathing reference, kept only when Evaluate finds it Valid against the scene as
// it is now (its probes current, the acoustic geometry, its settings). Nothing else is ever registered.
Selection Select(const SceneDocument& document, const ProbeVolumes::SceneAccess& access, const AcousticGeometry::MaterialResolver& material,
    const Loaders& loaders);
}
