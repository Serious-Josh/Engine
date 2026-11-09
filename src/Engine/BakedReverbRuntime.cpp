#include "Engine/BakedReverbRuntime.h"

#include "Engine/ReflectionBakes.h"

#include "Audio/SteamAudioCoordinates.h"

#include <bit>
#include <map>

namespace BakedReverbRuntime
{
namespace
{
    // The first reason a non-valid volume gives: its asset's, or its first non-valid layer's.
    std::string ReasonOf(const ReflectionBakes::VolumeStatus& status)
    {
        if(status.assetProblem != ReflectionBakes::StaleReason::None)
        {
            return ReflectionBakes::ToString(status.assetProblem);
        }

        for(const ReflectionBakes::LayerStatus& layer : status.layers)
        {
            if(layer.state != ReflectionBakes::LayerState::Valid)
            {
                return std::string(ReflectionBakes::ToString(status.state)) + ": " + ReflectionBakes::ToString(layer.reason);
            }
        }

        return ReflectionBakes::ToString(status.state);
    }
}

namespace
{
    bool SameBits(const BakedReflectionsAssets::Layer& first, const BakedReflectionsAssets::Layer& second) noexcept
    {
        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            if(std::bit_cast<std::uint32_t>(first.centre[axis]) != std::bit_cast<std::uint32_t>(second.centre[axis]))
            {
                return false;
            }
        }

        return std::bit_cast<std::uint32_t>(first.radius) == std::bit_cast<std::uint32_t>(second.radius);
    }

    // Against exactly the selected (to be registered) volumes, with their stored identifiers.
    void SelectSources(Selection& selection, std::span<const ReflectionBakes::StaticSource> sources,
        const std::vector<std::vector<BakedReflectionsAssets::Layer>>& layersByVolume)
    {
        for(const ReflectionBakes::StaticSource& source : sources)
        {
            SourceReport report;
            report.objectId = source.objectId;
            const BakedReflectionsAssets::Layer* first = nullptr;
            bool consistent = !layersByVolume.empty();

            for(const auto& layers : layersByVolume)
            {
                const auto found = std::find_if(layers.begin(), layers.end(), [&](const BakedReflectionsAssets::Layer& layer)
                {
                    return layer.kind == BakedReflectionsAssets::LayerKind::StaticSource && layer.sourceObjectId == source.objectId;
                });

                if(found == layers.end())
                {
                    consistent = false;
                    break;
                }

                if(first && !SameBits(*first, *found))
                {
                    consistent = false;
                    break;
                }

                first = first ? first : &*found;
            }

            if(!consistent || !first)
            {
                report.reason = layersByVolume.empty() ? "no Probe Volume with current baked data" :
                    "its baked layers differ between Probe Volumes; rebake them together (Bake All) so they match";
                selection.sourceReports.push_back(report);
                continue;
            }

            BakedSource baked;
            baked.objectId = source.objectId;
            baked.identifier.kind = Audio::ReflectionLayerKind::StaticSource;
            baked.identifier.centre = first->centre;
            baked.identifier.radius = first->radius;
            baked.position = Audio::SteamAudio::FromSteamAudio(Audio::SteamAudio::Vector{first->centre[0], first->centre[1], first->centre[2]});
            selection.sources.push_back(baked);
            report.eligible = true;
            selection.sourceReports.push_back(report);
        }
    }
}

Selection Select(const SceneDocument& document, const ProbeVolumes::SceneAccess& access, const AcousticGeometry::MaterialResolver& material,
    const Loaders& loaders)
{
    Selection selection;
    // The stored layers of each selected volume, in selection order, for the source check below.
    std::vector<std::vector<BakedReflectionsAssets::Layer>> layersByVolume;
    // One scene fingerprint for every volume, built once per Play.
    const std::vector<ReflectionBakes::AcousticObject> scene = ReflectionBakes::CollectAcousticScene(document, access, material);
    const ReflectionBakes::AcousticSceneHashes hashes = ReflectionBakes::HashAcousticScene(scene);
    const std::vector<ReflectionBakes::StaticSource> sources = ReflectionBakes::CollectStaticSources(document, access);

    for(const SceneObjectDocument& object : document.objects)
    {
        if(!object.probeVolume || !object.probeVolume->enabled)
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

        if(volume.bakedReflections.empty())
        {
            refuse("not baked");
            continue;
        }

        if(!loaders.probes || !loaders.baked)
        {
            refuse("baked data cannot be read in this build");
            continue;
        }

        std::string error;
        const std::optional<ProbeBatchAssets::Data> probes = volume.probeBatch.empty() ? std::nullopt : loaders.probes(volume.probeBatch, &error);
        const bool probesValid = probes && probes->fingerprint == ProbeVolumes::Fingerprint(document, object, access);
        BakedReflectionsAssets::Resolution baked = loaders.baked(volume.bakedReflections);

        ReflectionBakes::VolumeContext context;
        context.volume = &volume;
        context.probes = probesValid ? &*probes : nullptr;
        context.scene = hashes;
        context.sources = sources;
        context.baked = &baked;
        const ReflectionBakes::VolumeStatus status = ReflectionBakes::Evaluate(context);

        if(status.state != ReflectionBakes::VolumeState::Valid || !baked.data)
        {
            refuse(ReasonOf(status));
            continue;
        }

        Audio::AudioSystem::BakedReverbVolume eligible;
        eligible.name = report.name;
        eligible.batch = std::move(baked.data->batch);
        eligible.probes = baked.data->probes;
        selection.volumes.push_back(std::move(eligible));
        layersByVolume.push_back(baked.data->layers);
        report.eligible = true;
        selection.reports.push_back(report);
    }

    SelectSources(selection, sources, layersByVolume);
    return selection;
}
}
