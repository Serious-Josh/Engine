#include "Engine/ReflectionBakeStatus.h"

#include <algorithm>
#include <bit>

namespace ReflectionBakeStatus
{
namespace
{
    class KeyHasher
    {
    public:
        void Add(std::uint64_t value) noexcept
        {
            m_hash = (m_hash ^ value) * 0x100000001b3ull;
            m_hash ^= m_hash >> 29;
        }

        void Add(float value) noexcept
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

        void Add(const std::optional<FileStamp>& stamp) noexcept
        {
            Add(static_cast<std::uint64_t>(stamp.has_value()));

            if(stamp)
            {
                Add(static_cast<std::uint64_t>(stamp->time.time_since_epoch().count()));
                Add(static_cast<std::uint64_t>(stamp->size));
            }
        }

        void Add(const std::optional<Audio::AcousticTransform>& world) noexcept
        {
            Add(static_cast<std::uint64_t>(world.has_value()));

            if(world)
            {
                for(const auto& row : *world)
                {
                    for(const float value : row)
                    {
                        Add(value);
                    }
                }
            }
        }

        std::uint64_t Value() const noexcept
        {
            return m_hash;
        }

    private:
        std::uint64_t m_hash = 0xcbf29ce484222325ull;
    };

    std::optional<FileStamp> Stamp(const Host& host, std::string_view reference)
    {
        return reference.empty() || !host.stamp ? std::nullopt : host.stamp(reference);
    }

    // Everything CollectScene reads, without building or hashing a mesh. A mesh is identified by its address and size: meshes are
    // immutable once loaded, and a reload makes a new one.
    std::uint64_t SceneKey(const SceneDocument& document, const Host& host)
    {
        KeyHasher key;
        key.Add(host.geometryRevision ? host.geometryRevision() : std::uint64_t{0});

        for(const SceneObjectDocument& object : document.objects)
        {
            const bool acoustic = object.acoustics && object.acoustics->enabled;
            const bool source = object.audioSource && object.audioSource->enabled && object.audioSource->sourceCentricReflections &&
                object.audioSource->bakedReflections;

            if(!acoustic && !source)
            {
                continue;
            }

            key.Add(std::string_view(object.id));
            key.Add(static_cast<std::uint64_t>(acoustic) | static_cast<std::uint64_t>(source) << 1);
            key.Add(host.access.world ? host.access.world(object) : std::nullopt);

            if(acoustic)
            {
                const ImportedMesh* mesh = host.access.mesh ? host.access.mesh(object) : nullptr;
                key.Add(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(mesh)));
                key.Add(static_cast<std::uint64_t>(mesh ? mesh->vertices.size() : 0));
                key.Add(static_cast<std::uint64_t>(mesh ? mesh->indices.size() : 0));

                for(const auto& [submesh, reference] : object.acoustics->submeshMaterials)
                {
                    key.Add(std::string_view(submesh));
                    key.Add(std::string_view(reference));
                    key.Add(Stamp(host, reference));
                }
            }
        }

        return key.Value();
    }
}

AcousticScene CollectScene(const SceneDocument& document, const Host& host)
{
    AcousticScene scene;
    scene.objects = ReflectionBakes::CollectAcousticScene(document, host.access, host.resolveMaterial, &scene.skipped);
    scene.hashes = ReflectionBakes::HashAcousticScene(scene.objects);
    scene.sources = ReflectionBakes::CollectStaticSources(document, host.access, &scene.unplaced);
    return scene;
}

namespace
{
    // Both assets as stored now. Probes that are missing or do not decode come back empty; ProbeStatus says which.
    void LoadAssets(const ProbeVolumeComponent& volume, const Host& host, std::optional<ProbeBatchAssets::Data>& probes,
        std::optional<BakedReflectionsAssets::Resolution>& baked)
    {
        probes.reset();
        baked.reset();

        if(!volume.probeBatch.empty() && Stamp(host, volume.probeBatch) && host.loadProbes)
        {
            probes = host.loadProbes(volume.probeBatch);
        }

        if(!volume.bakedReflections.empty() && host.loadBaked)
        {
            baked = host.loadBaked(volume.bakedReflections);
        }
    }

    // Whether loaded probes still match the volume. The fingerprint reads the live scene, so it is never cached.
    ProbeVolumes::Status ProbeStatus(const SceneDocument& document, const SceneObjectDocument& object, const Host& host,
        const std::optional<ProbeBatchAssets::Data>& probes)
    {
        const ProbeVolumeComponent& volume = *object.probeVolume;

        if(volume.probeBatch.empty())
        {
            return ProbeVolumes::Status::NotGenerated;
        }

        if(!Stamp(host, volume.probeBatch))
        {
            return ProbeVolumes::Status::Missing;
        }

        if(!probes)
        {
            return ProbeVolumes::Status::Invalid;
        }

        if(probes->fingerprint != ProbeVolumes::Fingerprint(document, object, host.access))
        {
            return ProbeVolumes::Status::Stale;
        }

        return ProbeVolumes::Status::Valid;
    }

    ReflectionBakes::VolumeStatus EvaluateWith(const SceneObjectDocument& object, const AcousticScene& scene,
        const ProbeBatchAssets::Data* validProbes, const BakedReflectionsAssets::Resolution* baked)
    {
        ReflectionBakes::VolumeContext context;
        context.volume = &*object.probeVolume;
        context.probes = validProbes;
        context.scene = scene.hashes;
        context.sources = scene.sources;
        context.baked = baked;
        return ReflectionBakes::Evaluate(context);
    }
}

FreshVolume EvaluateFresh(const SceneDocument& document, const SceneObjectDocument& object, const Host& host, const AcousticScene& scene)
{
    FreshVolume fresh;

    if(!object.probeVolume)
    {
        fresh.status.state = ReflectionBakes::VolumeState::NotRequired;
        return fresh;
    }

    LoadAssets(*object.probeVolume, host, fresh.probes, fresh.baked);
    fresh.probeStatus = ProbeStatus(document, object, host, fresh.probes);

    // Only valid probes may be baked against.
    if(fresh.probeStatus != ProbeVolumes::Status::Valid)
    {
        fresh.probes.reset();
    }

    fresh.status = EvaluateWith(object, scene, fresh.probes ? &*fresh.probes : nullptr, fresh.baked ? &*fresh.baked : nullptr);
    return fresh;
}

void Cache::Refresh(const SceneDocument& document, const Host& host, bool recollect)
{
    const std::uint64_t key = SceneKey(document, host);

    if(!m_valid || key != m_sceneKey || recollect)
    {
        AcousticScene scene = CollectScene(document, host);
        const bool changed = !m_valid || !(scene.hashes == m_scene.hashes) || scene.sources.size() != m_scene.sources.size() ||
            !std::equal(scene.sources.begin(), scene.sources.end(), m_scene.sources.begin(),
                [](const ReflectionBakes::StaticSource& first, const ReflectionBakes::StaticSource& second)
            {
                return first.objectId == second.objectId && first.position == second.position;
            });
        m_scene = std::move(scene);
        m_sceneKey = key;
        ++m_sceneCollections;
        m_valid = true;

        // Volumes depend on the hashes and sources only, so an unchanged result leaves every volume as it was.
        if(changed)
        {
            ++m_sceneRevision;
        }
    }

    for(auto& [id, volume] : m_volumes)
    {
        volume.present = false;
    }

    for(const SceneObjectDocument& object : document.objects)
    {
        if(!object.probeVolume)
        {
            continue;
        }

        Volume& volume = m_volumes[object.id];
        const auto world = host.access.world ? host.access.world(object) : std::nullopt;
        const auto probeStamp = Stamp(host, object.probeVolume->probeBatch);
        const auto bakedStamp = Stamp(host, object.probeVolume->bakedReflections);
        volume.present = true;

        if(volume.sceneRevision == m_sceneRevision && volume.component == *object.probeVolume && volume.world == world &&
            volume.probeStamp == probeStamp && volume.bakedStamp == bakedStamp)
        {
            continue;
        }

        const bool filesChanged = !volume.assetsLoaded || volume.component.probeBatch != object.probeVolume->probeBatch ||
            volume.component.bakedReflections != object.probeVolume->bakedReflections || volume.probeStamp != probeStamp ||
            volume.bakedStamp != bakedStamp;

        if(filesChanged)
        {
            LoadAssets(*object.probeVolume, host, volume.probes, volume.baked);

            if(volume.probes)
            {
                volume.probes->batch.clear();
                volume.probes->batch.shrink_to_fit();
            }

            if(volume.baked && volume.baked->data)
            {
                volume.baked->data->batch.clear();
                volume.baked->data->batch.shrink_to_fit();
            }

            volume.assetsLoaded = true;
            ++m_assetLoads;
        }

        volume.entry.probeStatus = ProbeStatus(document, object, host, volume.probes);
        const ProbeBatchAssets::Data* validProbes = volume.entry.probeStatus == ProbeVolumes::Status::Valid ? &*volume.probes : nullptr;
        volume.entry.status = EvaluateWith(object, m_scene, validProbes, volume.baked ? &*volume.baked : nullptr);
        volume.component = *object.probeVolume;
        volume.world = world;
        volume.probeStamp = probeStamp;
        volume.bakedStamp = bakedStamp;
        volume.sceneRevision = m_sceneRevision;
        ++m_volumeEvaluations;
    }

    std::erase_if(m_volumes, [](const auto& entry)
    {
        return !entry.second.present;
    });
}

void Cache::Invalidate() noexcept
{
    m_valid = false;
    m_volumes.clear();
}

const Cache::Entry* Cache::Find(std::string_view objectId) const
{
    const auto found = m_volumes.find(objectId);
    return found == m_volumes.end() ? nullptr : &found->second.entry;
}
}
