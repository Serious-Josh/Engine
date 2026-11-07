#pragma once

#include "Audio/AudioSystem.h"
#include "Scene/SceneDocument.h"
#include "assets/ImportedMesh.h"
#include "assets/ProbeBatchAsset.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// M7: from a scene's Probe Volume to Steam Audio probes, and how to tell whether stored probes still describe the scene. Main thread.
namespace ProbeVolumes
{
// How a host reads the live scene: Editor and packaged games keep transforms and meshes differently.
struct SceneAccess
{
    std::function<std::optional<Audio::AcousticTransform>(const SceneObjectDocument&)> world;
    std::function<const ImportedMesh*(const SceneObjectDocument&)> mesh;
};

// Hashes all the volume's settings and its world transform. Floor placement and a filtered Uniform 3D grid depend on geometry, so they
// also hash each acoustics-enabled object's identity, world transform and vertex positions. Acoustic materials are not part of it: they
// don't move probes. The other placements never read geometry, so geometry changes do not make them stale.
std::uint64_t Fingerprint(const SceneDocument& document, const SceneObjectDocument& volume, const SceneAccess& scene);

enum class Status
{
    // No probes generated yet.
    NotGenerated,
    Valid,
    // The scene or the settings changed since generation.
    Stale,
    // The reference points at nothing readable.
    Missing,
    // The file exists but failed validation.
    Invalid
};

const char* ToString(Status status) noexcept;

// From here on, the Inspector asks before generating. A question, never a limit: answering yes generates all of it.
inline constexpr std::uint64_t ConfirmationProbes = 100000;

struct GenerationEstimate
{
    // Including manual probes. For floor, one per grid column: a column gets one per surface it crosses, and only generation can tell.
    std::uint64_t probes = 0;
    // The count is certain: nothing depends on geometry.
    bool exact = false;
    bool needsConfirmation = false;
    bool fitsAsset = true;
};

GenerationEstimate Estimate(const ProbeVolumeComponent& volume, const Audio::AcousticTransform& world) noexcept;

struct GenerationOutcome
{
    bool success = false;
    std::string error;
    ProbeBatchAssets::Data data;
    std::size_t duplicatesRemoved = 0;
    // Objects with acoustics enabled that could not contribute a surface.
    std::vector<std::string> skippedObjects;
    bool cancelled = false;
    std::size_t closedPieces = 0;
    std::size_t openPieces = 0;
    bool enclosedByOneShell = false;
};

// Everything a generation needs, owned by the request and independent of the scene from here on. Reads meshes, so main thread.
std::optional<Audio::ProbeGenerationRequest> BuildRequest(const SceneDocument& document, const SceneObjectDocument& volume, const SceneAccess& scene,
    std::vector<std::string>& skippedObjects, std::string& error);
GenerationOutcome FinishGeneration(Audio::ProbeGenerationResult result, std::uint64_t fingerprint, std::vector<std::string> skippedObjects);

GenerationOutcome Generate(const Audio::AudioSystem& audio, const SceneDocument& document, const SceneObjectDocument& volume, const SceneAccess& scene);
}
