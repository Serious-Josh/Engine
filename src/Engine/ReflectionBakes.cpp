#include "Engine/ReflectionBakes.h"

#include "Audio/SteamAudioCoordinates.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace ReflectionBakes
{
namespace
{
    using BakedReflectionsAssets::Layer;
    using BakedReflectionsAssets::LayerKind;

    class Hasher
    {
    public:
        void Add(std::uint64_t value) noexcept
        {
            m_hash = (m_hash ^ value) * 0x100000001b3ull;
            m_hash ^= m_hash >> 31;
        }

        // Canonical: -0 and 0 are the same input.
        void Add(float value) noexcept
        {
            AddBits(value == 0.0f ? 0.0f : value);
        }

        // Exact bits, for identifiers, which the SDK compares bit for bit.
        void AddBits(float value) noexcept
        {
            Add(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(value)));
        }

        void Add(std::string_view text) noexcept
        {
            Add(static_cast<std::uint64_t>(text.size()));

            for(const char character : text)
            {
                Add(static_cast<std::uint64_t>(static_cast<unsigned char>(character)));
            }
        }

        std::uint64_t Value() const noexcept
        {
            return m_hash;
        }

    private:
        std::uint64_t m_hash = 0xcbf29ce484222325ull;
    };

    void AddMaterial(Hasher& hash, const Audio::AcousticMaterialValues& material) noexcept
    {
        for(const float value : material.absorption)
        {
            hash.Add(value);
        }

        hash.Add(material.scattering);

        for(const float value : material.transmission)
        {
            hash.Add(value);
        }
    }

    std::pair<std::uint64_t, std::uint64_t> HashObject(const AcousticObject& object) noexcept
    {
        const Audio::AcousticMeshData& mesh = *object.mesh;
        Hasher geometry;
        geometry.Add(static_cast<std::uint64_t>(mesh.positions.size()));

        for(const auto& position : mesh.positions)
        {
            geometry.Add(position[0]);
            geometry.Add(position[1]);
            geometry.Add(position[2]);
        }

        geometry.Add(static_cast<std::uint64_t>(mesh.triangles.size()));

        for(const auto& triangle : mesh.triangles)
        {
            geometry.Add(static_cast<std::uint64_t>(triangle[0]));
            geometry.Add(static_cast<std::uint64_t>(triangle[1]));
            geometry.Add(static_cast<std::uint64_t>(triangle[2]));
        }

        for(const auto& row : object.world)
        {
            for(const float value : row)
            {
                geometry.Add(value);
            }
        }

        // Each triangle's resolved values, not its index into the material list: the same surfaces described in a different
        // list order are the same acoustics.
        Hasher materials;

        for(std::size_t triangle = 0; triangle < mesh.triangles.size(); ++triangle)
        {
            const std::int32_t index = triangle < mesh.triangleMaterials.size() ? mesh.triangleMaterials[triangle] : -1;

            if(index >= 0 && static_cast<std::size_t>(index) < mesh.materials.size())
            {
                AddMaterial(materials, mesh.materials[static_cast<std::size_t>(index)]);
            }
            else
            {
                materials.Add(std::uint64_t{0xffffffffffffffffull});
            }
        }

        return {geometry.Value(), materials.Value()};
    }

    bool SameBits(float first, float second) noexcept
    {
        return std::bit_cast<std::uint32_t>(first) == std::bit_cast<std::uint32_t>(second);
    }

    float Distance(const std::array<float, 3>& first, const std::array<float, 3>& second) noexcept
    {
        const float x = first[0] - second[0];
        const float y = first[1] - second[1];
        const float z = first[2] - second[2];
        return std::sqrt(x * x + y * y + z * z);
    }

    const Layer* FindLayer(const BakedReflectionsAssets::Data& data, const RequiredLayer& required, std::size_t& index)
    {
        for(index = 0; index < data.layers.size(); ++index)
        {
            const Layer& layer = data.layers[index];

            if(layer.kind == required.kind && layer.sourceObjectId == required.sourceObjectId)
            {
                return &layer;
            }
        }

        return nullptr;
    }

    StaleReason AssetProblem(const BakedReflectionsAssets::Resolution& resolution) noexcept
    {
        if(resolution.missing)
        {
            return StaleReason::AssetMissing;
        }

        switch(resolution.decode.error)
        {
        case BakedReflectionsAssets::DecodeError::UnsupportedVersion:
        case BakedReflectionsAssets::DecodeError::UnsupportedSteamAudioVersion:
            return StaleReason::UnsupportedAsset;
        default:
            return StaleReason::CorruptAsset;
        }
    }

    // Why a stored layer no longer matches its requirement; None when it does.
    StaleReason Compare(const BakedReflectionsAssets::Data& data, const Layer& stored, const RequiredLayer& required)
    {
        const AcousticSceneHashes storedScene{stored.geometryHash, stored.materialsHash};
        const std::uint64_t recomputed = LayerFingerprint(stored.kind, stored.centre, stored.radius, stored.sourceObjectId,
            stored.probesHash, storedScene, stored.settingsHash);

        if(recomputed != stored.fingerprint || stored.probesHash != data.sourceProbesHash)
        {
            return StaleReason::CorruptAsset;
        }

        if(stored.probesHash != required.probesHash)
        {
            return StaleReason::ProbeLayoutChanged;
        }

        if(stored.geometryHash != required.scene.geometry)
        {
            return StaleReason::AcousticGeometryChanged;
        }

        if(stored.materialsHash != required.scene.materials)
        {
            return StaleReason::AcousticMaterialChanged;
        }

        if(stored.settingsHash != required.settingsHash || !SameBits(stored.radius, required.radius))
        {
            return StaleReason::BakeSettingsChanged;
        }

        if(stored.kind == LayerKind::StaticSource && Distance(stored.centre, required.centre) > StaticSourceTolerance)
        {
            return StaleReason::StaticSourceMoved;
        }

        return StaleReason::None;
    }
}

AcousticSceneHashes HashAcousticScene(std::span<const AcousticObject> objects)
{
    std::vector<std::pair<std::uint64_t, std::uint64_t>> perObject;
    perObject.reserve(objects.size());

    for(const AcousticObject& object : objects)
    {
        if(object.mesh)
        {
            perObject.push_back(HashObject(object));
        }
    }

    // Sorting by content makes the result independent of document order.
    std::sort(perObject.begin(), perObject.end());
    Hasher geometry;
    Hasher materials;
    geometry.Add(static_cast<std::uint64_t>(perObject.size()));
    materials.Add(static_cast<std::uint64_t>(perObject.size()));

    for(const auto& [geometryHash, materialsHash] : perObject)
    {
        geometry.Add(geometryHash);
        // Paired with its geometry, so a material moving to another object is a change.
        materials.Add(geometryHash);
        materials.Add(materialsHash);
    }

    return {geometry.Value(), materials.Value()};
}

BakedReflectionsAssets::BakeParameters ParametersFor(const ReflectionBakeSettings& settings) noexcept
{
    BakedReflectionsAssets::BakeParameters parameters;
    parameters.rays = settings.rays;
    parameters.diffuseSamples = settings.diffuseSamples;
    parameters.bounces = settings.bounces;
    parameters.irradianceMinimumDistance = settings.irradianceMinimumDistance;
    parameters.order = BakeOrder;
    parameters.duration = BakeDuration;
    return parameters;
}

std::uint64_t HashParameters(const BakedReflectionsAssets::BakeParameters& parameters) noexcept
{
    Hasher hash;
    hash.Add(static_cast<std::uint64_t>(parameters.rays));
    hash.Add(static_cast<std::uint64_t>(parameters.diffuseSamples));
    hash.Add(static_cast<std::uint64_t>(parameters.bounces));
    hash.Add(parameters.irradianceMinimumDistance);
    hash.Add(static_cast<std::uint64_t>(parameters.order));
    hash.Add(parameters.duration);
    return hash.Value();
}

std::uint64_t HashProbes(const ProbeBatchAssets::Data& probes) noexcept
{
    Hasher hash;
    hash.Add(probes.fingerprint);
    hash.Add(BakedReflectionsAssets::HashProbes(probes.probes));
    return hash.Value();
}

std::vector<StaticSource> CollectStaticSources(const SceneDocument& document, const ProbeVolumes::SceneAccess& scene,
    std::vector<std::string>* unplaced)
{
    std::vector<StaticSource> sources;

    for(const SceneObjectDocument& object : document.objects)
    {
        const bool wantsBaked = object.audioSource && object.audioSource->enabled && object.audioSource->sourceCentricReflections &&
            object.audioSource->bakedReflections;

        if(!wantsBaked)
        {
            continue;
        }

        const auto world = scene.world ? scene.world(object) : std::nullopt;

        if(!world)
        {
            if(unplaced)
            {
                unplaced->push_back(object.name);
            }

            continue;
        }

        const Audio::SteamAudio::Vector engine{(*world)[0][3], (*world)[1][3], (*world)[2][3]};
        sources.push_back({object.id, Audio::SteamAudio::ToSteamAudio(engine)});
    }

    std::sort(sources.begin(), sources.end(), [](const StaticSource& first, const StaticSource& second)
    {
        return first.objectId < second.objectId;
    });
    return sources;
}

std::vector<AcousticObject> CollectAcousticScene(const SceneDocument& document, const ProbeVolumes::SceneAccess& scene,
    const AcousticGeometry::MaterialResolver& resolveMaterial, std::vector<std::string>* skipped)
{
    std::vector<AcousticObject> objects;
    std::map<std::pair<const ImportedMesh*, std::map<std::string, std::string>>, std::shared_ptr<const Audio::AcousticMeshData>> shared;

    for(const SceneObjectDocument& object : document.objects)
    {
        if(!object.acoustics || !object.acoustics->enabled)
        {
            continue;
        }

        const ImportedMesh* geometry = scene.mesh ? scene.mesh(object) : nullptr;
        const auto world = scene.world ? scene.world(object) : std::nullopt;

        if(!geometry || !world || !resolveMaterial)
        {
            if(skipped)
            {
                skipped->push_back(object.name);
            }

            continue;
        }

        auto& mesh = shared[{geometry, object.acoustics->submeshMaterials}];

        if(!mesh)
        {
            AcousticGeometry::BuildReport report;
            mesh = AcousticGeometry::Build(*geometry, *object.acoustics, resolveMaterial, report);
        }

        if(!mesh)
        {
            if(skipped)
            {
                skipped->push_back(object.name);
            }

            continue;
        }

        objects.push_back({mesh, *world});
    }

    return objects;
}

std::uint64_t LayerFingerprint(BakedReflectionsAssets::LayerKind kind, const std::array<float, 3>& centre, float radius,
    const std::string& sourceObjectId, std::uint64_t probesHash, const AcousticSceneHashes& scene, std::uint64_t settingsHash) noexcept
{
    Hasher hash;
    hash.Add(static_cast<std::uint64_t>(BakedReflectionsAssets::FormatVersion));
    hash.Add(static_cast<std::uint64_t>(BakedReflectionsAssets::SteamAudioVersion));
    hash.Add(static_cast<std::uint64_t>(kind));
    hash.AddBits(centre[0]);
    hash.AddBits(centre[1]);
    hash.AddBits(centre[2]);
    hash.AddBits(radius);
    hash.Add(std::string_view(sourceObjectId));
    hash.Add(probesHash);
    hash.Add(scene.geometry);
    hash.Add(scene.materials);
    hash.Add(settingsHash);
    return hash.Value();
}

BakedReflectionsAssets::Layer ToStoredLayer(const RequiredLayer& required, std::uint64_t dataBytes)
{
    Layer layer;
    layer.kind = required.kind;
    layer.centre = required.centre;
    layer.radius = required.radius;
    layer.sourceObjectId = required.sourceObjectId;
    layer.probesHash = required.probesHash;
    layer.geometryHash = required.scene.geometry;
    layer.materialsHash = required.scene.materials;
    layer.settingsHash = required.settingsHash;
    layer.fingerprint = required.fingerprint;
    layer.dataBytes = dataBytes;
    return layer;
}

const char* ToString(StaleReason reason) noexcept
{
    switch(reason)
    {
    case StaleReason::None:
        return "Up to date";
    case StaleReason::ProbesNotReady:
        return "The probes must be generated or regenerated first";
    case StaleReason::ProbeLayoutChanged:
        return "The probe layout changed";
    case StaleReason::AcousticGeometryChanged:
        return "Acoustic geometry changed";
    case StaleReason::AcousticMaterialChanged:
        return "An acoustic material changed";
    case StaleReason::BakeSettingsChanged:
        return "The bake settings changed";
    case StaleReason::StaticSourceMoved:
        return "The static source moved";
    case StaleReason::LayerMissing:
        return "A required layer has not been baked";
    case StaleReason::AssetMissing:
        return "The baked reflections file is missing";
    case StaleReason::UnsupportedAsset:
        return "The baked reflections file is from an unsupported version";
    case StaleReason::CorruptAsset:
        return "The baked reflections file is corrupt";
    }

    return "Unknown";
}

const char* ToString(VolumeState state) noexcept
{
    switch(state)
    {
    case VolumeState::NotRequired:
        return "Not required";
    case VolumeState::Valid:
        return "Valid";
    case VolumeState::Partial:
        return "Partial";
    case VolumeState::Stale:
        return "Stale";
    case VolumeState::Missing:
        return "Not baked";
    }

    return "Unknown";
}

std::vector<RequiredLayer> RequiredLayers(const VolumeContext& context)
{
    std::vector<RequiredLayer> layers;

    if(!context.volume || !context.volume->enabled)
    {
        return layers;
    }

    const std::uint64_t probesHash = context.probes ? HashProbes(*context.probes) : 0;
    const std::uint64_t settingsHash = HashParameters(ParametersFor(context.volume->bakeSettings));
    const auto make = [&](LayerKind kind, std::string sourceObjectId, std::array<float, 3> centre, float radius)
    {
        RequiredLayer layer;
        layer.kind = kind;
        layer.sourceObjectId = std::move(sourceObjectId);
        layer.centre = centre;
        layer.radius = radius;
        layer.probesHash = probesHash;
        layer.scene = context.scene;
        layer.settingsHash = settingsHash;
        layer.fingerprint = LayerFingerprint(kind, layer.centre, layer.radius, layer.sourceObjectId, probesHash, context.scene,
            settingsHash);
        layers.push_back(std::move(layer));
    };

    // The REVERB identifier's sphere is all zeros, always (M8.1).
    make(LayerKind::Reverb, {}, {0.0f, 0.0f, 0.0f}, 0.0f);

    for(const StaticSource& source : context.sources)
    {
        make(LayerKind::StaticSource, source.objectId, source.position, StaticSourceInfluenceRadius);
    }

    return layers;
}

VolumeStatus Evaluate(const VolumeContext& context)
{
    VolumeStatus status;

    if(!context.volume || !context.volume->enabled)
    {
        status.state = VolumeState::NotRequired;
        return status;
    }

    status.probesReady = context.probes != nullptr;
    const BakedReflectionsAssets::Data* data = context.baked && context.baked->data ? &*context.baked->data : nullptr;

    if(context.baked && !data)
    {
        status.assetProblem = AssetProblem(*context.baked);
    }

    std::size_t valid = 0;
    std::size_t stale = 0;
    std::set<std::string> requiredSources;

    for(RequiredLayer& required : RequiredLayers(context))
    {
        LayerStatus layer;
        requiredSources.insert(required.sourceObjectId);
        layer.required = std::move(required);
        std::size_t index = 0;
        const Layer* stored = data ? FindLayer(*data, layer.required, index) : nullptr;

        if(context.baked && !data)
        {
            layer.state = LayerState::Stale;
            layer.reason = status.assetProblem;
        }
        else if(!stored)
        {
            layer.state = LayerState::Missing;
            layer.reason = status.probesReady ? StaleReason::LayerMissing : StaleReason::ProbesNotReady;
        }
        else
        {
            layer.existing = index;
            layer.reason = status.probesReady ? Compare(*data, *stored, layer.required) : StaleReason::ProbesNotReady;
            layer.state = layer.reason == StaleReason::None ? LayerState::Valid : LayerState::Stale;
        }

        valid += layer.state == LayerState::Valid ? 1 : 0;
        stale += layer.state == LayerState::Stale ? 1 : 0;
        status.layers.push_back(std::move(layer));
    }

    if(data)
    {
        for(std::size_t index = 0; index < data->layers.size(); ++index)
        {
            const Layer& stored = data->layers[index];

            if(stored.kind == LayerKind::StaticSource && !requiredSources.contains(stored.sourceObjectId))
            {
                status.unused.push_back(index);
            }
        }
    }

    if(valid == status.layers.size())
    {
        status.state = VolumeState::Valid;
    }
    else if(valid > 0)
    {
        status.state = VolumeState::Partial;
    }
    else if(context.baked || stale > 0)
    {
        status.state = VolumeState::Stale;
    }
    else
    {
        status.state = VolumeState::Missing;
    }

    return status;
}

DisplayState Present(const VolumeStatus& status, JobState job) noexcept
{
    if(job == JobState::Baking)
    {
        return DisplayState::Baking;
    }

    if(job == JobState::Failed)
    {
        return DisplayState::Failed;
    }

    switch(status.state)
    {
    case VolumeState::NotRequired:
        return DisplayState::NotRequired;
    case VolumeState::Valid:
        return DisplayState::Valid;
    case VolumeState::Partial:
        return DisplayState::Partial;
    case VolumeState::Stale:
        return DisplayState::Stale;
    case VolumeState::Missing:
        return DisplayState::Missing;
    }

    return DisplayState::Missing;
}

bool ShowsStaleWarning(const VolumeStatus& status) noexcept
{
    return status.state == VolumeState::Stale || status.state == VolumeState::Partial;
}

std::size_t VolumePlan::CountActions(LayerAction action) const noexcept
{
    return static_cast<std::size_t>(std::count_if(layers.begin(), layers.end(), [action](const LayerPlan& layer)
    {
        return layer.action == action;
    }));
}

VolumePlan Plan(const VolumeStatus& status, bool forceRebake)
{
    VolumePlan plan;

    if(status.state == VolumeState::NotRequired)
    {
        return plan;
    }

    plan.needsProbeGeneration = !status.probesReady;

    for(const LayerStatus& layer : status.layers)
    {
        LayerPlan entry;
        entry.layer = layer.required;
        entry.reason = layer.reason;
        entry.existing = layer.existing;

        if(layer.state == LayerState::Missing)
        {
            entry.action = LayerAction::Add;
        }
        else if(layer.state == LayerState::Stale || forceRebake)
        {
            entry.action = LayerAction::Rebake;
        }
        else
        {
            entry.action = LayerAction::Reuse;
        }

        plan.layers.push_back(std::move(entry));
    }

    // Only a usable asset has layers to drop; an unusable one is replaced whole.
    if(status.assetProblem == StaleReason::None)
    {
        plan.remove = status.unused;
    }

    plan.skip = !plan.needsProbeGeneration && plan.remove.empty() && plan.CountActions(LayerAction::Reuse) == plan.layers.size();
    return plan;
}
}
