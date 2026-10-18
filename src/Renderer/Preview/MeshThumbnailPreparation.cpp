#include "Renderer/Preview/MeshThumbnailPreparation.h"
#include "assets/MeshImportUnitsJson.h"

#include "Renderer/Preview/MaterialThumbnailValidation.h"
#include "assets/ModelImporter.h"
#include "assets/ProjectAssetResolver.h"
#include "Project/ProjectPaths.h"

#include <Windows.h>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <optional>

namespace
{
    // Reuse the checked pixel-cache codec in a separate mesh namespace. Existing
    // material identities, entry bytes, and formats remain completely unchanged.
    MaterialThumbnailDiskCache::Source DescribeMeshCache(const ProjectPaths& paths,
        const AssetRecord& record, const std::filesystem::path& path, const std::string& fingerprint)
    {
        auto source = MaterialThumbnailDiskCache::Describe(paths, record, path, fingerprint);

        if(source.entry.empty())
        {
            return {};
        }

        source.directory = source.directory.parent_path().parent_path() / "MeshThumbnails" / source.directory.filename();
        source.identity = "mesh-thumbnail-cache-v2\n256x256\nR8G8B8A8_UNORM-display-encoded\n" + source.identity;
        std::uint64_t hash = 14695981039346656037ull;

        for(unsigned char byte : source.identity)
        {
            hash = (hash ^ byte) * 1099511628211ull;
        }

        source.entry = source.directory / (std::to_string(hash) + ".thumb");
        return source;
    }

    size_t GeometryBytes(const ImportedMesh& geometry)
    {
        size_t bytes = geometry.vertices.capacity() * sizeof(Vertex) + geometry.indices.capacity() * sizeof(uint32_t);
        bytes += geometry.metadata.submeshes.capacity() * sizeof(MeshSubmesh);
        bytes += geometry.metadata.materialSlots.capacity() * sizeof(MeshMaterialSlot);

        for(const auto& range : geometry.metadata.submeshes)
        {
            bytes += range.name.capacity();
        }

        for(const auto& slot : geometry.metadata.materialSlots)
        {
            bytes += slot.id.capacity() + slot.name.capacity();
        }

        return bytes;
    }

    void ValidatePreparedGeometry(const ImportedMesh& geometry)
    {
        std::string error;

        if(geometry.vertices.empty() || !ValidateMeshMetadata(geometry.metadata, geometry.indices.size(), error))
        {
            throw std::runtime_error("Invalid prepared thumbnail geometry: " + error);
        }

        for(uint32_t index : geometry.indices)
        {
            if(index >= geometry.vertices.size())
            {
                throw std::runtime_error("Prepared thumbnail geometry contains an out-of-range index.");
            }
        }

        for(const auto& vertex : geometry.vertices)
        {
            for(float component : vertex.position)
            {
                if(!std::isfinite(component))
                {
                    throw std::runtime_error("Prepared thumbnail geometry contains nonfinite positions.");
                }
            }
        }

        const double extentX = double(geometry.boundsMax.x) - geometry.boundsMin.x;
        const double extentY = double(geometry.boundsMax.y) - geometry.boundsMin.y;
        const double extentZ = double(geometry.boundsMax.z) - geometry.boundsMin.z;
        const double radius = std::hypot(extentX, extentY, extentZ);

        if(extentX < 0 || extentY < 0 || extentZ < 0 || !std::isfinite(radius) || radius <= 0)
        {
            throw std::runtime_error("Prepared thumbnail geometry has invalid or degenerate bounds.");
        }
    }
}

MeshThumbnailPreparation::~MeshThumbnailPreparation()
{
    Shutdown();
}

MeshThumbnailPreparation::Completion MeshThumbnailPreparation::Prepare(Source source)
{
    Completion result{Job{}, std::move(source)};
    result.job.identity.project = result.source.project;
    result.job.identity.assetReference = result.source.record.id.ToReference();
    result.job.identity.units = result.source.record.meshImportUnits;

    try
    {
        HANDLE input = CreateFileW(result.source.path.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

        if(input == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error("Cannot lock mesh thumbnail source (Windows error " + std::to_string(GetLastError()) + ").");
        }

        result.sourceLock = std::shared_ptr<void>(input, [](void* handle) { CloseHandle(handle); });
        result.source.size = std::filesystem::file_size(result.source.path);
        result.source.modified = std::filesystem::last_write_time(result.source.path);
        MaterialThumbnailValidation validation;
        auto& identity = result.job.identity;
        identity.sourceFingerprint = validation.Digest(result.source.path);

        if(!result.source.previousFingerprint.empty())
        {
            result.reuse = identity.sourceFingerprint == result.source.previousFingerprint;
            result.job.state = result.reuse ? State::Prepared : State::Cancelled;
            result.job.error = result.reuse ? "" : "Mesh source revision changed; request a fresh thumbnail.";
            return result;
        }

        const std::string fingerprint = identity.assetReference + "\n" + identity.sourceFingerprint + "\n" +
            std::to_string(identity.importRevision) + "\n" + identity.uvPolicy + "\n" + identity.renderVersion +
            "\n" + SerializeMeshImportUnits(identity.units).dump();
        result.job.diskSource = DescribeMeshCache(ProjectPaths(result.source.project), result.source.record,
            result.source.path, fingerprint);

        if(MaterialThumbnailDiskCache::Load(result.job.diskSource, result.job.cachedPixels) &&
            result.job.cachedPixels.width == 256 && result.job.cachedPixels.height == 256)
        {
            result.job.state = State::CacheHit;
            result.bytes = result.job.cachedPixels.pixels.capacity();
            return result; // No Assimp work or CPU geometry allocation on a cache hit.
        }

        result.job.cachedPixels = {};
        result.job.geometry = ModelImporter::LoadFlattened(result.source.path, MeshUVOrientation::FlipV,
            kMaximumJobBytes, result.source.record.meshImportUnits);
        ValidatePreparedGeometry(result.job.geometry);
        result.bytes = GeometryBytes(result.job.geometry);

        if(result.bytes > kMaximumJobBytes)
        {
            throw std::runtime_error("Mesh thumbnail geometry exceeds the 64 MiB per-job retention limit.");
        }

        result.job.state = State::Prepared;
    }
    catch(const std::exception& error)
    {
        result.job.geometry = {};
        result.job.cachedPixels = {};
        result.bytes = 0;
        result.job.state = State::Failed;
        result.job.error = error.what();
    }
    catch(...)
    {
        result.job.geometry = {};
        result.job.cachedPixels = {};
        result.bytes = 0;
        result.job.state = State::Failed;
        result.job.error = "Unknown mesh thumbnail preparation failure.";
    }

    return result;
}

bool MeshThumbnailPreparation::Request(const ProjectPaths& paths, const ProjectAssetRegistry& registry,
    const AssetID& id, std::uint64_t& ticket, std::string& error)
{
    error.clear();
    ticket = 0;

    try
    {
        const auto project = std::filesystem::canonical(paths.GetRoot());
        const auto* record = registry.Find(id);
        const auto resolved = ProjectAssetResolver::Resolve(paths, registry, id);

        if(!record || record->type != AssetType::Mesh || resolved.status != AssetAvailability::Available ||
            _wcsicmp(resolved.resolvedPath.extension().c_str(), L".fbx") != 0)
        {
            throw std::runtime_error("Mesh thumbnail requires an available registered FBX mesh asset.");
        }

        for(auto& [existingTicket, entry] : m_jobs)
        {
            if(entry.source.project != project || entry.source.record.id != id ||
                entry.source.record.meshImportUnits != record->meshImportUnits ||
                entry.job.state == State::Failed || entry.job.state == State::Cancelled)
            {
                continue;
            }

            if(entry.job.state == State::Prepared || entry.job.state == State::CacheHit)
            {
                // Rehash off-thread before reusing retained content, including
                // changes whose size/timestamp are indistinguishable.
                entry.source.previousFingerprint = entry.job.identity.sourceFingerprint;
                m_pending.push_back(existingTicket);
                entry.job.state = State::Pending;
            }

            ticket = existingTicket;
            StartNext();
            return true;
        }

        if(m_jobs.size() >= kJobCapacity || m_nextTicket == 0)
        {
            throw std::runtime_error("Mesh thumbnail job capacity reached; release completed jobs before retrying.");
        }

        Source source{project, resolved.resolvedPath, *record};
        const auto next = m_nextTicket++;
        Entry entry{Job{}, source};
        entry.job.identity.project = project;
        entry.job.identity.assetReference = id.ToReference();
        entry.job.identity.units = record->meshImportUnits;
        m_jobs.emplace(next, std::move(entry));

        try
        {
            m_pending.push_back(next);
        }
        catch(...)
        {
            m_jobs.erase(next);
            throw;
        }

        ticket = next;
        StartNext();
        return true;
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

void MeshThumbnailPreparation::StartNext()
{
    if(m_worker.valid())
    {
        return;
    }

    const auto attempts = m_pending.size();

    for(size_t attempt = 0; attempt < attempts; ++attempt)
    {
        const auto ticket = m_pending.front();
        m_pending.pop_front();
        auto found = m_jobs.find(ticket);

        if(found == m_jobs.end() || found->second.job.state != State::Pending)
        {
            continue;
        }

        if(found->second.source.previousFingerprint.empty() &&
            m_retainedBytes > kMaximumRetainedBytes - kMaximumJobBytes)
        {
            m_pending.push_back(ticket);
            continue;
        }

        try
        {
            m_worker = std::async(std::launch::async, &MeshThumbnailPreparation::Prepare, found->second.source);
            m_activeTicket = ticket;
            found->second.job.state = State::Preparing;
        }
        catch(const std::exception& error)
        {
            found->second.job.state = State::Failed;
            found->second.job.error = error.what();
        }

        return;
    }
}

void MeshThumbnailPreparation::Poll(const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    std::error_code projectError;
    const auto activeProject = std::filesystem::canonical(paths.GetRoot(), projectError);

    for(auto& [ticket, entry] : m_jobs)
    {
        const auto* record = registry.Find(entry.source.record.id);
        const auto resolved = ProjectAssetResolver::Resolve(paths, registry, entry.source.record.id);
        bool current = !projectError && activeProject == entry.source.project && record &&
            record->type == AssetType::Mesh && record->relativePath == entry.source.record.relativePath &&
            record->meshImportUnits == entry.source.record.meshImportUnits;

        current = current && resolved.status == AssetAvailability::Available && resolved.resolvedPath == entry.source.path;

        if(current && (entry.job.state == State::Prepared || entry.job.state == State::CacheHit))
        {
            std::error_code sizeError;
            std::error_code timeError;
            const auto size = std::filesystem::file_size(entry.source.path, sizeError);
            const auto modified = std::filesystem::last_write_time(entry.source.path, timeError);
            current = !sizeError && !timeError && size == entry.source.size && modified == entry.source.modified;
        }

        if(!current && entry.job.state != State::Cancelled)
        {
            Release(entry);
            entry.job.state = State::Cancelled;
            entry.job.error = "Mesh thumbnail project or registration changed.";
        }
    }

    if(m_worker.valid() && m_worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        std::optional<Completion> completion;

        try
        {
            completion = m_worker.get();
        }
        catch(...)
        {
            const auto failed = m_jobs.find(m_activeTicket);

            if(failed != m_jobs.end() && failed->second.job.state != State::Cancelled)
            {
                Release(failed->second);
                failed->second.job.state = State::Failed;
                failed->second.job.error = "Mesh thumbnail worker could not return its result.";
            }

            m_activeTicket = 0;
            StartNext();
            return;
        }

        auto completed = std::move(*completion);
        const auto found = m_jobs.find(m_activeTicket);
        m_activeTicket = 0;

        if(found != m_jobs.end() && found->second.job.state != State::Cancelled)
        {
            std::error_code error;
            const auto project = std::filesystem::canonical(paths.GetRoot(), error);
            const auto* record = registry.Find(completed.source.record.id);
            const auto resolved = ProjectAssetResolver::Resolve(paths, registry, completed.source.record.id);
            const bool current = !error && project == completed.source.project && record &&
                record->type == AssetType::Mesh && record->relativePath == completed.source.record.relativePath &&
                record->meshImportUnits == completed.source.record.meshImportUnits &&
                resolved.status == AssetAvailability::Available && resolved.resolvedPath == completed.source.path;

            if(!current)
            {
                Release(found->second);
                found->second.job.state = State::Cancelled;
                found->second.job.error = "Mesh thumbnail became stale after project, registration, or source replacement.";
            }
            else if(completed.reuse)
            {
                // Pixel ownership may already have transferred to a GPU job.
                found->second.job.state = found->second.job.cachedPixels.width == 256 ? State::CacheHit : State::Prepared;
                found->second.source = std::move(completed.source);
                found->second.source.previousFingerprint.clear();
            }
            else
            {
                Release(found->second);
                found->second.job = std::move(completed.job);
                found->second.source = std::move(completed.source);
                found->second.bytes = completed.bytes;
                m_retainedBytes += completed.bytes;
            }
        }
    }

    StartNext();
}

const MeshThumbnailPreparation::Job* MeshThumbnailPreparation::Find(std::uint64_t ticket) const noexcept
{
    const auto found = m_jobs.find(ticket);
    return found == m_jobs.end() ? nullptr : &found->second.job;
}

void MeshThumbnailPreparation::Release(Entry& entry)
{
    m_retainedBytes -= entry.bytes;
    entry.bytes = 0;
    entry.job.geometry = {};
    entry.job.cachedPixels = {};
}

bool MeshThumbnailPreparation::TakeGeometry(std::uint64_t ticket, ImportedMesh& geometry)
{
    const auto found = m_jobs.find(ticket);

    if(found == m_jobs.end() || found->second.job.state != State::Prepared ||
        found->second.job.geometry.vertices.empty())
    {
        return false;
    }

    auto& entry = found->second;
    geometry = std::move(entry.job.geometry);
    entry.job.geometry = {};
    m_retainedBytes -= entry.bytes;
    entry.bytes = 0;
    return true;
}

bool MeshThumbnailPreparation::TakeCachedPixels(std::uint64_t ticket, MaterialThumbnailDiskCache::Image& pixels)
{
    const auto found = m_jobs.find(ticket);

    if(found == m_jobs.end() || found->second.job.state != State::CacheHit ||
        found->second.job.cachedPixels.pixels.size() != 256 * 256 * 4)
    {
        return false;
    }

    auto& entry = found->second;
    pixels = std::move(entry.job.cachedPixels);
    entry.job.cachedPixels = {256, 256, {}}; // Preserve provenance for revision-only revalidation.
    m_retainedBytes -= entry.bytes;
    entry.bytes = 0;
    return true;
}

void MeshThumbnailPreparation::Cancel(std::uint64_t ticket)
{
    // Scrolling can cancel many requests while one Assimp task is still running.
    // Remove their queue nodes immediately rather than waiting for that task.
    std::erase(m_pending, ticket);
    const auto found = m_jobs.find(ticket);

    if(found != m_jobs.end())
    {
        Release(found->second);
        found->second.job.state = State::Cancelled;
    }
}

void MeshThumbnailPreparation::Shutdown()
{
    m_pending.clear();

    if(m_worker.valid())
    {
        m_worker.wait(); // Shutdown only; Assimp calls are not interruptible.
        try
        {
            m_worker.get();
        }
        catch(...)
        {
            // No worker borrows Renderer resources; shutdown still completes.
        }
    }

    m_jobs.clear();
    m_activeTicket = 0;
    m_retainedBytes = 0;
}

void MeshThumbnailPreparation::Discard(std::uint64_t ticket)
{
    Cancel(ticket);
    m_jobs.erase(ticket);
}
