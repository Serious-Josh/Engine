#pragma once

#include "Audio/AudioSystem.h"
#include "Engine/AcousticGeometry.h"
#include "Engine/ProbeVolumes.h"
#include "Scene/SceneDocument.h"
#include "assets/BakedReflectionsAsset.h"
#include "assets/ProbeBatchAsset.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

// M8.4: which Probe Volumes' baked data the listener-centric reverb may use at Play. Pure: reads assets through the host's loaders
// and decides with M8.2's ReflectionBakes::Evaluate against the scene as it is now. Only a fully Valid volume qualifies: every layer
// it requires (REVERB and each static source's) is present and current. That is also what keeps overlapping volumes safe, since the
// SDK misbehaves where registered batches overlap and one lacks a requested layer (M8.1). Nothing is written back.
namespace BakedReverbRuntime
{
struct Loaders
{
    std::function<std::optional<ProbeBatchAssets::Data>(const std::string& reference, std::string* error)> probes;
    std::function<BakedReflectionsAssets::Resolution(const std::string& reference)> baked;
};

struct VolumeReport
{
    std::string name;
    bool eligible = false;
    // Why not, when not.
    std::string reason;
};

// M8.5: a static Audio Source whose baked layer every selected volume holds under the same identifier, bit for bit.
struct BakedSource
{
    std::string objectId;
    // As stored in the assets: Steam Audio space, the exact bits the SDK will match.
    Audio::ReflectionLayerIdentifier identifier;
    // Engine space, where it was baked; the session compares the live position against it.
    std::array<float, 3> position{};
};

struct SourceReport
{
    std::string objectId;
    bool eligible = false;
    std::string reason;
};

struct Selection
{
    std::vector<Audio::AudioSystem::BakedReverbVolume> volumes;
    std::vector<VolumeReport> reports;
    std::vector<BakedSource> sources;
    std::vector<SourceReport> sourceReports;
};

// The volumes, then the static sources they can play. A source qualifies only when every selected volume (every batch that will be
// registered) stores its layer with identical identifier bits: one that differed would have no data in a batch the simulator holds,
// which is the overlapping-batch defect M8.1 found. Volumes are never dropped for it, so their REVERB still plays.
Selection Select(const SceneDocument& document, const ProbeVolumes::SceneAccess& access, const AcousticGeometry::MaterialResolver& material,
    const Loaders& loaders);
}
