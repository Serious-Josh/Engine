#include "assets/ProjectMeshCPUCache.h"

#include <algorithm>
#include <utility>

struct ProjectMeshCPUCache::Allocation
{
    ImportedMesh geometry;

    std::shared_ptr<std::atomic_size_t> accounting;
    size_t bytes = 0;

    Allocation(ImportedMesh value, std::shared_ptr<std::atomic_size_t> owner, size_t size)
        : geometry(std::move(value)), accounting(std::move(owner)), bytes(size)
    {
        accounting->fetch_add(bytes);
    }

    ~Allocation()
    {
        accounting->fetch_sub(bytes);
    }
};

ProjectMeshCPUCache::ProjectMeshCPUCache() : ProjectMeshCPUCache(Limits{}) {}

ProjectMeshCPUCache::ProjectMeshCPUCache(Limits limits)
    : m_limits(limits), m_liveBytes(std::make_shared<std::atomic_size_t>(0))
{
}

size_t ProjectMeshCPUCache::GeometryBytes(const ImportedMesh& geometry)
{
    // Include vector capacity, metadata structures and owned strings. Counting
    // inline string storage again is conservative. Allocator bookkeeping is estimated.
    size_t bytes = sizeof(Allocation) + 32;
    bytes += geometry.vertices.capacity() * sizeof(Vertex);
    bytes += geometry.indices.capacity() * sizeof(std::uint32_t);

    return bytes + MetadataBytes(geometry.metadata);
}

size_t ProjectMeshCPUCache::MetadataBytes(const MeshMetadata& metadata)
{
    size_t bytes = metadata.submeshes.capacity() * sizeof(MeshSubmesh);
    bytes += metadata.materialSlots.capacity() * sizeof(MeshMaterialSlot);

    for(const auto& submesh : metadata.submeshes)
    {
        bytes += submesh.name.capacity() + 1;
    }

    for(const auto& slot : metadata.materialSlots)
    {
        bytes += slot.id.capacity() + 1;
        bytes += slot.name.capacity() + 1;
    }

    return bytes;
}

size_t ProjectMeshCPUCache::EntryBytes(const Key& key)
{
    size_t bytes = sizeof(Entry) + 2 * sizeof(void*);
    bytes += (std::get<0>(key).native().capacity() + 1) * sizeof(std::filesystem::path::value_type);
    bytes += std::get<1>(key).capacity() + 1;
    bytes += std::get<2>(key).capacity() + 1;
    return bytes;
}

std::shared_ptr<const ImportedMesh> ProjectMeshCPUCache::FindVerified(const Key& key)
{
    const auto found = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& entry)
    {
        return entry.key == key;
    });

    if(found == m_entries.end())
    {
        ++m_statistics.misses;
        return {};
    }

    ++m_statistics.hits;
    m_entries.splice(m_entries.begin(), m_entries, found);
    return m_entries.front().geometry;
}

std::shared_ptr<const ImportedMesh> ProjectMeshCPUCache::StoreVerified(const Key& key, ImportedMesh geometry)
{
    const auto existing = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& entry)
    {
        return entry.key == key;
    });

    if(existing != m_entries.end())
    {
        m_entries.splice(m_entries.begin(), m_entries, existing);
        return m_entries.front().geometry;
    }

    const size_t geometryBytes = GeometryBytes(geometry);
    auto allocation = std::make_shared<Allocation>(std::move(geometry), m_liveBytes, geometryBytes);
    std::shared_ptr<const ImportedMesh> result(allocation, &allocation->geometry);

    return RetainVerified(key, std::move(result), geometryBytes);
}

std::shared_ptr<const ImportedMesh> ProjectMeshCPUCache::StorePreparedVerified(const Key& key,
    std::shared_ptr<const ImportedMesh> geometry)
{
    if (!geometry)
    {
        return {};
    }

    const auto existing = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& entry)
    {
        return entry.key == key;
    });

    if (existing != m_entries.end())
    {
        m_entries.splice(m_entries.begin(), m_entries, existing);
        return m_entries.front().geometry;
    }

    const auto bytes = GeometryBytes(*geometry);
    auto accounting = m_liveBytes;
    const auto* value = geometry.get();
    accounting->fetch_add(bytes);
    // Constructor failure invokes the deleter as well, balancing accounting.
    std::shared_ptr<const ImportedMesh> result(value,
        [geometry = std::move(geometry), accounting, bytes](const ImportedMesh*) mutable
        {
            geometry.reset();
            accounting->fetch_sub(bytes);
        });
    return RetainVerified(key, std::move(result), bytes);
}

std::shared_ptr<const ImportedMesh> ProjectMeshCPUCache::RetainVerified(const Key& key,
    std::shared_ptr<const ImportedMesh> result, size_t geometryBytes)
{
    // Charge the actual retained key capacities, which can differ from the caller's.
    Entry entry{key, result, geometryBytes, 0};
    entry.retainedBytes = geometryBytes + EntryBytes(entry.key);

    if(entry.retainedBytes > m_limits.entryBytes || entry.retainedBytes > m_limits.retainedBytes)
    {
        ++m_statistics.bypasses;
        return result;
    }

    // Allocate the list node before evicting anything. The new entry is not yet
    // included in accounting; prior entries remain valid through shared leases.
    m_entries.push_front(std::move(entry));
    const auto required = m_entries.front().retainedBytes;

    while(m_statistics.retainedBytes > m_limits.retainedBytes - required)
    {
        const auto& victim = m_entries.back();
        m_statistics.retainedBytes -= victim.retainedBytes;
        m_cachedGeometryBytes -= victim.geometryBytes;
        m_entries.pop_back();
        ++m_statistics.evictions;
    }

    m_statistics.retainedBytes += required;
    m_cachedGeometryBytes += geometryBytes;
    return result;
}

ProjectMeshCPUCache::Statistics ProjectMeshCPUCache::GetStatistics() const
{
    auto result = m_statistics;
    result.pinnedOutsideCacheBytes = m_liveBytes->load() - m_cachedGeometryBytes;
    return result;
}

void ProjectMeshCPUCache::Clear()
{
    m_entries.clear();
    m_statistics.retainedBytes = 0;
    m_cachedGeometryBytes = 0;
}
