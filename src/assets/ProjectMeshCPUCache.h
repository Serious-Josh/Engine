#pragma once

#include "assets/ImportedMesh.h"
#include "assets/MeshPreparationPolicy.h"
#include "assets/MeshImportUnits.h"

#include <atomic>
#include <list>
#include <memory>
#include <string>
#include <tuple>
#include <filesystem>
#include <utility>

// Engine-owned, main-thread CPU retention only. FBX callers validate registry,
// containment and locked source hashes; cooked callers verify typed provider and
// payload evidence. A cache lookup never grants source/session authorization.
class ProjectMeshCPUCache
{
public:
    // First five fields retain FBX semantics. Cooked: provider namespace, AssetID,
    // payload SHA-256, compatibility, unused PreserveSource. Final tag prevents
    // collisions; publication/session tokens deliberately do not prevent reuse.
    using KeyFields = std::tuple<std::filesystem::path, std::string, std::string, std::uint32_t,
        MeshUVOrientation, MeshPreparationSourceKind, MeshImportUnits>;

    struct Key : KeyFields
    {
        Key() = default;
        // Preserve existing five-field FBX construction and std::get access.
        Key(std::filesystem::path project, std::string asset, std::string digest, std::uint32_t compatibility,
            MeshUVOrientation uvPolicy, MeshPreparationSourceKind kind = MeshPreparationSourceKind::FBX,
            MeshImportUnits units = {})
            : KeyFields(std::move(project), std::move(asset), std::move(digest), compatibility, uvPolicy, kind, units)
        {
        }

        bool operator==(const Key&) const = default;
        auto operator<=>(const Key&) const = default;
    };

    struct Limits
    {
        size_t retainedBytes = 512 * 1024 * 1024;
        size_t entryBytes = 128 * 1024 * 1024;
    };

    struct Statistics
    {
        size_t hits = 0;
        size_t misses = 0;
        size_t evictions = 0;
        size_t bypasses = 0;

        size_t retainedBytes = 0;
        size_t pinnedOutsideCacheBytes = 0;
    };

    ProjectMeshCPUCache();
    explicit ProjectMeshCPUCache(Limits limits);
    ProjectMeshCPUCache(const ProjectMeshCPUCache&) = delete;
    ProjectMeshCPUCache& operator=(const ProjectMeshCPUCache&) = delete;

    std::shared_ptr<const ImportedMesh> FindVerified(const Key& key);
    // Complete validated Engine geometry only, after source/payload verification.
    // Moves buffers; oversized entries return an uncached, accounted consumer lease.
    std::shared_ptr<const ImportedMesh> StoreVerified(const Key& key, ImportedMesh geometry);
    // Main-thread adoption of immutable worker output; no geometry copy.
    std::shared_ptr<const ImportedMesh> StorePreparedVerified(const Key& key,
        std::shared_ptr<const ImportedMesh> geometry);
    static size_t GeometryBytes(const ImportedMesh& geometry);
    static size_t MetadataBytes(const MeshMetadata& metadata);
    Statistics GetStatistics() const;
    void Clear(); // Consumer leases remain valid, including after cache destruction.

private:
    struct Allocation;
    struct Entry
    {
        Key key;
        std::shared_ptr<const ImportedMesh> geometry;

        size_t geometryBytes = 0;
        size_t retainedBytes = 0;
    };

    std::shared_ptr<const ImportedMesh> RetainVerified(const Key& key,
        std::shared_ptr<const ImportedMesh> geometry, size_t geometryBytes);
    static size_t EntryBytes(const Key& key);

    Limits m_limits;
    Statistics m_statistics;

    std::shared_ptr<std::atomic_size_t> m_liveBytes;
    std::list<Entry> m_entries; // Most recently used first.

    size_t m_cachedGeometryBytes = 0;
};
