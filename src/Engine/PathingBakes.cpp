#include "Engine/PathingBakes.h"

#include "Engine/ReflectionBakes.h"
#include "assets/BakedReflectionsAsset.h"

#include <bit>
#include <stdexcept>

namespace PathingBakes
{
namespace
{
    constexpr std::uint64_t FnvOffset = 14695981039346656037ull;
    constexpr std::uint64_t FnvPrime = 1099511628211ull;

    void Mix(std::uint64_t& hash, std::uint64_t value) noexcept
    {
        for(int shift = 0; shift < 64; shift += 8)
        {
            hash = (hash ^ ((value >> shift) & 0xffu)) * FnvPrime;
        }
    }

    void MixText(std::uint64_t& hash, const std::string& text) noexcept
    {
        Mix(hash, text.size());

        for(const char value : text)
        {
            hash = (hash ^ static_cast<unsigned char>(value)) * FnvPrime;
        }
    }

    std::uint64_t Combine(std::uint32_t formatVersion, std::uint32_t steamAudioVersion, const std::string& probeReference,
        std::uint64_t probeFingerprint, std::uint64_t probesHash, std::uint64_t geometryHash, std::uint64_t settingsHash) noexcept
    {
        std::uint64_t hash = FnvOffset;
        Mix(hash, formatVersion);
        Mix(hash, steamAudioVersion);
        MixText(hash, probeReference);
        Mix(hash, probeFingerprint);
        Mix(hash, probesHash);
        Mix(hash, geometryHash);
        Mix(hash, settingsHash);
        return hash;
    }

    Status Make(State state, std::string message, StaleReason reason = StaleReason::None)
    {
        return Status{state, reason, std::move(message)};
    }
}

std::uint64_t HashSettings(const PathingBakeSettings& settings) noexcept
{
    std::uint64_t hash = FnvOffset;
    Mix(hash, settings.samples);
    Mix(hash, std::bit_cast<std::uint32_t>(settings.radius));
    Mix(hash, std::bit_cast<std::uint32_t>(settings.threshold));
    Mix(hash, std::bit_cast<std::uint32_t>(settings.visibilityRange));
    Mix(hash, std::bit_cast<std::uint32_t>(settings.pathRange));
    return hash;
}

std::uint64_t Fingerprint(const Inputs& inputs) noexcept
{
    if(!inputs.probes)
    {
        return 0;
    }

    return Combine(BakedPathingAssets::FormatVersion, BakedPathingAssets::SteamAudioVersion, inputs.probeReference, inputs.probes->fingerprint,
        BakedReflectionsAssets::HashProbes(inputs.probes->probes), inputs.geometryHash, HashSettings(inputs.settings));
}

Status Evaluate(const Inputs& current, const BakedPathingAssets::Resolution* asset)
{
    if(!current.probes)
    {
        return Make(State::NoProbes, "generate probes before baking pathing");
    }

    if(!asset)
    {
        return Make(State::NotBaked, "pathing has not been baked");
    }

    if(asset->missing)
    {
        return Make(State::Missing, "the baked pathing file is missing");
    }

    if(!asset->decode || !asset->data)
    {
        const auto error = asset->decode.error;

        if(error == BakedPathingAssets::DecodeError::UnsupportedVersion || error == BakedPathingAssets::DecodeError::UnsupportedSteamAudioVersion)
        {
            return Make(State::Stale, "baked with another format or Steam Audio version: rebake", StaleReason::Version);
        }

        return Make(State::Corrupt, "the baked pathing is unusable: " + asset->decode.message);
    }

    const BakedPathingAssets::Data& baked = *asset->data;
    // The stored fingerprint must be what its own fields give: anything else is a different format's idea of the inputs.
    const std::uint64_t own = Combine(BakedPathingAssets::FormatVersion, baked.steamAudioVersion, baked.sourceProbeBatch,
        baked.sourceProbeFingerprint, baked.sourceProbesHash, baked.geometryHash, baked.settingsHash);

    if(own != baked.fingerprint)
    {
        return Make(State::Stale, "the baked pathing's fingerprint does not match its inputs: rebake", StaleReason::Version);
    }

    if(baked.sourceProbeBatch != current.probeReference || baked.sourceProbeFingerprint != current.probes->fingerprint ||
        baked.sourceProbesHash != BakedReflectionsAssets::HashProbes(current.probes->probes) || baked.probes != current.probes->probes)
    {
        return Make(State::Stale, "the probes changed since pathing was baked", StaleReason::Probes);
    }

    if(baked.geometryHash != current.geometryHash)
    {
        return Make(State::Stale, "the acoustic geometry changed since pathing was baked", StaleReason::Geometry);
    }

    if(baked.settingsHash != HashSettings(current.settings) || baked.settings != current.settings)
    {
        return Make(State::Stale, "the pathing bake settings changed", StaleReason::Settings);
    }

    return Make(State::Valid, "pathing is baked and current");
}

std::optional<Audio::PathingBakeRequest> MakeRequest(const Inputs& inputs, std::vector<Audio::ProbeGeometryObject> geometry)
{
    if(!inputs.probes || inputs.probes->batch.empty() || inputs.probes->probes.empty())
    {
        return std::nullopt;
    }

    Audio::PathingBakeRequest request;
    request.geometry = std::move(geometry);
    request.batch = inputs.probes->batch;
    request.expectedProbes = inputs.probes->probes.size();
    request.samples = inputs.settings.samples;
    request.radius = inputs.settings.radius;
    request.threshold = inputs.settings.threshold;
    request.visibilityRange = inputs.settings.visibilityRange;
    request.pathRange = inputs.settings.pathRange;
    return request;
}

Assembly Assemble(const Inputs& bakedFrom, const Audio::PathingBakeResult& result)
{
    Assembly assembly;

    if(!result.success || result.batch.empty() || result.layerBytes == 0)
    {
        assembly.error = result.error.empty() ? "the bake did not succeed" : result.error;
        return assembly;
    }

    if(!bakedFrom.probes)
    {
        assembly.error = "the bake has no probes to record";
        return assembly;
    }

    BakedPathingAssets::Data data;
    data.settings = bakedFrom.settings;
    data.sourceProbeBatch = bakedFrom.probeReference;
    data.sourceProbeFingerprint = bakedFrom.probes->fingerprint;
    data.sourceProbesHash = BakedReflectionsAssets::HashProbes(bakedFrom.probes->probes);
    data.geometryHash = bakedFrom.geometryHash;
    data.settingsHash = HashSettings(bakedFrom.settings);
    data.fingerprint = Fingerprint(bakedFrom);
    data.probes = bakedFrom.probes->probes;
    data.layerBytes = result.layerBytes;
    data.batch = result.batch;

    try
    {
        assembly.encoded = BakedPathingAssets::Encode(data);
    }
    catch(const std::exception& exception)
    {
        assembly.error = exception.what();
        return assembly;
    }

    BakedPathingAssets::Data check;

    if(const auto decoded = BakedPathingAssets::Decode(assembly.encoded, check); !decoded || check != data)
    {
        assembly.encoded.clear();
        assembly.error = "the baked pathing does not round-trip: " + decoded.message;
    }

    return assembly;
}

Selection Select(const SceneDocument& document, const ProbeVolumes::SceneAccess& access, const AcousticGeometry::MaterialResolver& material,
    const Loaders& loaders)
{
    Selection selection;
    bool sceneHashed = false;
    std::uint64_t geometryHash = 0;

    for(const SceneObjectDocument& object : document.objects)
    {
        if(!object.probeVolume || !object.probeVolume->enabled || !object.probeVolume->pathingEnabled || object.probeVolume->bakedPathing.empty())
        {
            continue;
        }

        const ProbeVolumeComponent& volume = *object.probeVolume;
        VolumeReport report;
        report.name = object.name.empty() ? object.id : object.name;
        const auto refuse = [&](std::string reason)
        {
            report.reason = std::move(reason);
            selection.reports.push_back(report);
        };

        if(!loaders.probes || !loaders.pathing)
        {
            refuse("baked pathing cannot be read in this build");
            continue;
        }

        // Once per Play, and only when some volume needs it: it reads every acoustic mesh.
        if(!sceneHashed)
        {
            geometryHash = ReflectionBakes::HashAcousticScene(ReflectionBakes::CollectAcousticScene(document, access, material)).geometry;
            sceneHashed = true;
        }

        std::string error;
        const std::optional<ProbeBatchAssets::Data> probes = volume.probeBatch.empty() ? std::nullopt : loaders.probes(volume.probeBatch, &error);
        const bool probesValid = probes && probes->fingerprint == ProbeVolumes::Fingerprint(document, object, access);
        BakedPathingAssets::Resolution baked = loaders.pathing(volume.bakedPathing);
        Inputs inputs;
        inputs.probeReference = volume.probeBatch;
        inputs.probes = probesValid ? &*probes : nullptr;
        inputs.geometryHash = geometryHash;
        inputs.settings = volume.pathingSettings;
        const Status status = Evaluate(inputs, &baked);

        if(status.state != State::Valid || !baked.data)
        {
            refuse(status.message);
            continue;
        }

        Audio::AudioSystem::BakedReverbVolume eligible;
        eligible.name = report.name;
        eligible.batch = std::move(baked.data->batch);
        eligible.probes = std::move(baked.data->probes);
        selection.volumes.push_back(std::move(eligible));
        report.eligible = true;
        selection.reports.push_back(report);
    }

    return selection;
}
}