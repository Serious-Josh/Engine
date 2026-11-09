#pragma once

#include "Engine/AcousticGeometry.h"
#include "Engine/ProbeVolumes.h"
#include "Engine/ReflectionBakes.h"
#include "assets/BakedReflectionsAsset.h"
#include "assets/ProbeBatchAsset.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// M8.3: every Probe Volume's bake status, from one shared acoustic scene. Main thread.
//
// Collecting the acoustic scene builds each acoustic mesh and hashes every vertex, so it happens once per scene change, never per
// volume or per frame. Whether the scene changed is decided from a shallow key: each acoustic object's identity, mesh, transform
// and material assignments, each referenced material file's time and size, and each static source's position. The cache is for
// display only. A bake and its publication call EvaluateFresh, which trusts nothing cached.
//
// The key also holds the host's geometry revision (the Editor uses the scene's structure and mesh residency revisions, which the
// ray tracing scene already relies on for mesh changes), and the host may force a recollection on a timer, so an edit no key sees
// is still picked up. A recollection that finds the same hashes and sources re-evaluates nothing.
namespace ReflectionBakeStatus
{
struct FileStamp
{
    std::filesystem::file_time_type time{};
    std::uintmax_t size = 0;

    bool operator==(const FileStamp&) const = default;
};

// How the host reads the scene and the project's assets.
struct Host
{
    ProbeVolumes::SceneAccess access;
    AcousticGeometry::MaterialResolver resolveMaterial;
    // The registered file behind an "asset:<uuid>" reference; nullopt when there is none (or for a built-in).
    std::function<std::optional<FileStamp>(std::string_view reference)> stamp;
    std::function<std::optional<ProbeBatchAssets::Data>(std::string_view reference)> loadProbes;
    std::function<BakedReflectionsAssets::Resolution(std::string_view reference)> loadBaked;
    // Changes whenever any mesh the scene presents may have changed. Optional.
    std::function<std::uint64_t()> geometryRevision;
};

struct AcousticScene
{
    std::vector<ReflectionBakes::AcousticObject> objects;
    ReflectionBakes::AcousticSceneHashes hashes;
    std::vector<ReflectionBakes::StaticSource> sources;
    // Acoustic objects that contribute nothing, and static sources without a readable position.
    std::vector<std::string> skipped;
    std::vector<std::string> unplaced;
};

AcousticScene CollectScene(const SceneDocument& document, const Host& host);

// One volume, evaluated from its assets as stored now. The assets keep their batches, ready to bake.
struct FreshVolume
{
    // Present only when the probes decode and still match the volume (ProbeVolumes::Status::Valid).
    std::optional<ProbeBatchAssets::Data> probes;
    ProbeVolumes::Status probeStatus = ProbeVolumes::Status::NotGenerated;
    std::optional<BakedReflectionsAssets::Resolution> baked;
    ReflectionBakes::VolumeStatus status;
};

FreshVolume EvaluateFresh(const SceneDocument& document, const SceneObjectDocument& object, const Host& host, const AcousticScene& scene);

class Cache
{
public:
    struct Entry
    {
        ReflectionBakes::VolumeStatus status;
        ProbeVolumes::Status probeStatus = ProbeVolumes::Status::NotGenerated;
    };

    // Re-evaluates what changed since the last call. Volumes no longer in the document are dropped. `recollect` rebuilds and rehashes
    // the acoustic scene whatever the key says: the bounded fallback for edits no key can see.
    void Refresh(const SceneDocument& document, const Host& host, bool recollect = false);
    // Forgets everything: the next Refresh collects the scene and evaluates every volume again.
    void Invalidate() noexcept;

    const Entry* Find(std::string_view objectId) const;
    const AcousticScene& GetScene() const noexcept
    {
        return m_scene;
    }

    // Verification: how often the expensive steps ran.
    std::uint64_t GetSceneCollections() const noexcept
    {
        return m_sceneCollections;
    }
    std::uint64_t GetVolumeEvaluations() const noexcept
    {
        return m_volumeEvaluations;
    }
    std::uint64_t GetAssetLoads() const noexcept
    {
        return m_assetLoads;
    }

private:
    struct Volume
    {
        Entry entry;
        ProbeVolumeComponent component;
        std::optional<Audio::AcousticTransform> world;
        std::optional<FileStamp> probeStamp;
        std::optional<FileStamp> bakedStamp;
        std::uint64_t sceneRevision = 0;
        bool present = false;
        // Decoded once per file version, without the SDK's batch bytes, which evaluation never reads. Moving or editing the volume
        // re-evaluates it without reading either file again.
        bool assetsLoaded = false;
        std::optional<ProbeBatchAssets::Data> probes;
        std::optional<BakedReflectionsAssets::Resolution> baked;
    };

    bool m_valid = false;
    std::uint64_t m_sceneKey = 0;
    std::uint64_t m_sceneRevision = 0;
    AcousticScene m_scene;
    std::map<std::string, Volume, std::less<>> m_volumes;
    std::uint64_t m_sceneCollections = 0;
    std::uint64_t m_volumeEvaluations = 0;
    std::uint64_t m_assetLoads = 0;
};
}
