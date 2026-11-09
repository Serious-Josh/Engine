#include "Engine/ReflectionBakeJobs.h"

#include <stdexcept>
#include <utility>

namespace ReflectionBakeJobs
{
using BakedReflectionsAssets::LayerKind;
using ReflectionBakes::LayerAction;

Audio::ReflectionLayerIdentifier IdentifierFor(LayerKind kind, const std::array<float, 3>& centre, float radius) noexcept
{
    Audio::ReflectionLayerIdentifier identifier;
    identifier.kind = kind == LayerKind::Reverb ? Audio::ReflectionLayerKind::Reverb : Audio::ReflectionLayerKind::StaticSource;

    if(kind == LayerKind::StaticSource)
    {
        identifier.centre = centre;
        identifier.radius = radius;
    }

    return identifier;
}

ReflectionBakes::VolumeContext ContextFor(const VolumeInputs& inputs) noexcept
{
    ReflectionBakes::VolumeContext context;
    context.volume = inputs.volume;
    context.probes = inputs.probes;
    context.scene = inputs.sceneHashes;
    context.sources = inputs.sources;
    context.baked = inputs.baked;
    return context;
}

Preparation Prepare(const VolumeInputs& inputs, bool force)
{
    Preparation preparation;
    preparation.status = ReflectionBakes::Evaluate(ContextFor(inputs));
    preparation.plan = ReflectionBakes::Plan(preparation.status, force);
    const ReflectionBakes::VolumePlan& plan = preparation.plan;

    if(preparation.status.state == ReflectionBakes::VolumeState::NotRequired)
    {
        preparation.outcome = PreparationOutcome::NotRequired;
        return preparation;
    }

    if(plan.needsProbeGeneration || !inputs.probes)
    {
        preparation.outcome = PreparationOutcome::NeedsProbes;
        return preparation;
    }

    if(plan.skip)
    {
        preparation.outcome = PreparationOutcome::UpToDate;
        return preparation;
    }

    preparation.outcome = PreparationOutcome::Bake;
    const ProbeVolumeComponent& volume = *inputs.volume;
    Audio::ReflectionBakeRequest& request = preparation.request;
    Expectation& expected = preparation.expected;
    preparation.reused = plan.CountActions(LayerAction::Reuse);
    // Evaluate only reuses layers of an asset that decoded, and all of an asset's layers share its probes.
    const BakedReflectionsAssets::Data* existing = preparation.reused > 0 && inputs.baked && inputs.baked->data ? &*inputs.baked->data : nullptr;

    if(existing)
    {
        request.batch = existing->batch;
        request.expectedProbes = existing->probes.size();

        for(const std::size_t index : plan.remove)
        {
            const BakedReflectionsAssets::Layer& layer = existing->layers[index];
            request.remove.push_back(IdentifierFor(layer.kind, layer.centre, layer.radius));
        }
    }
    else
    {
        // The reuse count must agree with where the batch comes from.
        preparation.reused = 0;
        request.batch = inputs.probes->batch;
        request.expectedProbes = inputs.probes->probes.size();
    }

    for(const ReflectionBakes::LayerPlan& layer : plan.layers)
    {
        const bool keep = existing && layer.action == LayerAction::Reuse;
        Expectation::Source source;
        source.baked = !keep;

        if(keep)
        {
            const BakedReflectionsAssets::Layer& stored = existing->layers[*layer.existing];
            source.index = request.keep.size();
            // The stored identifier, bit for bit: it is the one the batch holds.
            request.keep.push_back(IdentifierFor(stored.kind, stored.centre, stored.radius));
        }
        else
        {
            if(existing && layer.existing)
            {
                // A stale layer, perhaps under an old identifier (a moved source): it goes before its replacement is baked.
                const BakedReflectionsAssets::Layer& stored = existing->layers[*layer.existing];
                request.remove.push_back(IdentifierFor(stored.kind, stored.centre, stored.radius));
            }

            source.index = request.bake.size();
            request.bake.push_back(IdentifierFor(layer.layer.kind, layer.layer.centre, layer.layer.radius));
        }

        expected.layers.push_back(layer.layer);
        expected.sources.push_back(source);
    }

    expected.parameters = ReflectionBakes::ParametersFor(volume.bakeSettings);
    request.rays = expected.parameters.rays;
    request.diffuseSamples = expected.parameters.diffuseSamples;
    request.bounces = expected.parameters.bounces;
    request.irradianceMinimumDistance = expected.parameters.irradianceMinimumDistance;
    request.order = expected.parameters.order;
    request.duration = expected.parameters.duration;
    request.geometry.reserve(inputs.scene.size());

    for(const ReflectionBakes::AcousticObject& object : inputs.scene)
    {
        if(object.mesh)
        {
            request.geometry.push_back({object.mesh, object.world});
        }
    }

    expected.objectId = inputs.objectId;
    expected.volume = volume;
    expected.probeFingerprint = inputs.probes->fingerprint;
    expected.probes = inputs.probes->probes;
    return preparation;
}

Assembly Assemble(const Expectation& expected, const Audio::ReflectionBakeResult& result)
{
    Assembly assembly;

    if(!result.success)
    {
        assembly.error = result.error.empty() ? "the bake failed" : result.error;
        return assembly;
    }

    BakedReflectionsAssets::Data data;
    data.parameters = expected.parameters;
    data.sourceProbeBatch = expected.volume.probeBatch;
    data.sourceProbeFingerprint = expected.probeFingerprint;
    data.probes = expected.probes;
    data.batch = result.batch;

    if(expected.layers.empty() || expected.layers.size() != expected.sources.size())
    {
        assembly.error = "the bake has no layers";
        return assembly;
    }

    data.sourceProbesHash = expected.layers.front().probesHash;

    for(std::size_t index = 0; index < expected.layers.size(); ++index)
    {
        const Expectation::Source& source = expected.sources[index];
        const std::vector<std::uint64_t>& sizes = source.baked ? result.bakedBytes : result.keptBytes;

        if(source.index >= sizes.size() || sizes[source.index] == 0)
        {
            assembly.error = "a required layer is missing from the bake";
            return assembly;
        }

        data.layers.push_back(ReflectionBakes::ToStoredLayer(expected.layers[index], sizes[source.index]));
    }

    try
    {
        assembly.encoded = BakedReflectionsAssets::Encode(data);
    }
    catch(const std::length_error& error)
    {
        assembly.error = std::string("the bake is too large to store: ") + error.what();
        return assembly;
    }

    BakedReflectionsAssets::Data decoded;
    const BakedReflectionsAssets::DecodeResult check = BakedReflectionsAssets::Decode(assembly.encoded, decoded);

    if(!check || !(decoded == data))
    {
        assembly.encoded.clear();
        assembly.error = "the baked reflections asset does not decode: " + check.message;
        return assembly;
    }

    assembly.success = true;
    return assembly;
}

bool IsStillCurrent(const Expectation& expected, const ProbeVolumeComponent& current, const ReflectionBakes::VolumeStatus& status,
    std::string* why)
{
    const auto fail = [&](const char* reason)
    {
        if(why)
        {
            *why = reason;
        }

        return false;
    };

    // Any edit to the volume: its settings, its probes or its published bake (an undo, a Clear Bake, another publication).
    if(!(current == expected.volume))
    {
        return fail("the Probe Volume changed during baking");
    }

    if(!status.probesReady)
    {
        return fail("the probes became stale during baking");
    }

    if(status.layers.size() != expected.layers.size())
    {
        return fail("the static sources changed during baking");
    }

    for(std::size_t index = 0; index < expected.layers.size(); ++index)
    {
        if(status.layers[index].required.fingerprint != expected.layers[index].fingerprint)
        {
            return fail("the acoustic scene or a static source changed during baking");
        }
    }

    return true;
}
}
