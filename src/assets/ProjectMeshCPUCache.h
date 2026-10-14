#pragma once

#include "assets/ModelImporter.h"

#include <atomic>
#include <list>
#include <memory>
#include <string>
#include <tuple>

// Engine-owned, main-thread CPU retention only. Callers must validate current
// registry/type/containment and hash a locked source before using a verified key.
class ProjectMeshCPUCache
{
public:
    using Key = std::tuple<std::filesystem::path, std::string, std::string, std::uint32_t, MeshUVOrientation>;

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
    // Complete importer output only, after post-import digest verification.
    // Moves buffers; oversized entries return an uncached, accounted consumer lease.
    std::shared_ptr<const ImportedMesh> StoreVerified(const Key& key, ImportedMesh geometry);
    // Main-thread adoption of immutable worker output; no geometry copy.
    std::shared_ptr<const ImportedMesh> StorePreparedVerified(const Key& key,
        std::shared_ptr<const ImportedMesh> geometry);
    static size_t GeometryBytes(const ImportedMesh& geometry);
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
