#pragma once

#include "Audio/AudioSystem.h"
#include "Engine/ReflectionBakes.h"
#include "Scene/AudioComponents.h"
#include "assets/BakedReflectionsAsset.h"
#include "assets/ProbeBatchAsset.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// M8.3: between ReflectionBakes' plan and the audio module's baker. Pure: it reads no file and touches no Steam Audio object, so
// the Editor's single-volume bake and Bake All share every decision made here.
//
// Reuse: when some layers stay valid, the bake starts from the published asset's own batch, which already holds them, drops the
// stale and unused layers and bakes the rest. Otherwise (first bake, new probes, a corrupt asset, Force Rebake) it starts from the
// probe asset's batch. Either way the audio module bakes into a private copy, and the published asset is never changed.
namespace ReflectionBakeJobs
{
Audio::ReflectionLayerIdentifier IdentifierFor(BakedReflectionsAssets::LayerKind kind, const std::array<float, 3>& centre, float radius) noexcept;

// Everything one volume's evaluation and bake read, borrowed for the call.
struct VolumeInputs
{
    std::string objectId;
    const ProbeVolumeComponent* volume = nullptr;
    // The volume's probes, with their batch, only when ProbeVolumes reports them Valid.
    const ProbeBatchAssets::Data* probes = nullptr;
    // The referenced asset as resolved, with its batch; null when the volume references none.
    const BakedReflectionsAssets::Resolution* baked = nullptr;
    std::span<const ReflectionBakes::AcousticObject> scene;
    ReflectionBakes::AcousticSceneHashes sceneHashes;
    std::span<const ReflectionBakes::StaticSource> sources;
};

ReflectionBakes::VolumeContext ContextFor(const VolumeInputs& inputs) noexcept;

// What the result must still match at publication, and what the published asset records.
struct Expectation
{
    std::string objectId;
    // The component as it was when the bake started.
    ProbeVolumeComponent volume;
    // Every layer the asset will hold, in its order.
    std::vector<ReflectionBakes::RequiredLayer> layers;
    // Per layer: baked by this job (index into the request's `bake`), or kept from the existing asset (index into `keep`).
    struct Source
    {
        bool baked = false;
        std::size_t index = 0;
    };
    std::vector<Source> sources;
    BakedReflectionsAssets::BakeParameters parameters;
    std::uint64_t probeFingerprint = 0;
    std::vector<std::array<float, 4>> probes;
};

enum class PreparationOutcome
{
    // Every required layer is valid and nothing is to be removed.
    UpToDate,
    // The probes are missing or stale: no layer can be baked against them.
    NeedsProbes,
    Bake,
    // Nothing is required of a disabled volume.
    NotRequired
};

struct Preparation
{
    PreparationOutcome outcome = PreparationOutcome::UpToDate;
    ReflectionBakes::VolumeStatus status;
    ReflectionBakes::VolumePlan plan;
    // Bake only.
    Audio::ReflectionBakeRequest request;
    Expectation expected;
    std::size_t reused = 0;
};

// Evaluates the volume afresh and plans its bake. `force` rebakes every layer from the probe asset.
Preparation Prepare(const VolumeInputs& inputs, bool force);

struct Assembly
{
    bool success = false;
    std::string error;
    std::vector<std::byte> encoded;
};

// Turns a successful bake into the asset bytes to store, encoded and decoded once to prove they round-trip.
Assembly Assemble(const Expectation& expected, const Audio::ReflectionBakeResult& result);

// The main-thread publication gate. `current` is the volume as it is now, evaluated afresh; the result may be published only when
// it still describes exactly the inputs it was baked from, and nobody replaced the reference meanwhile.
bool IsStillCurrent(const Expectation& expected, const ProbeVolumeComponent& current, const ReflectionBakes::VolumeStatus& status,
    std::string* why = nullptr);
}
