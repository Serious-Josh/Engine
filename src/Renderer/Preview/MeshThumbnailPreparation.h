#pragma once

#include "assets/ImportedMesh.h"
#include "assets/ModelImporter.h"
#include "assets/ProjectAssetRegistry.h"
#include "Renderer/Preview/MaterialThumbnailDiskCache.h"

#include <deque>
#include <future>
#include <map>
#include <memory>

class ProjectPaths;

// CPU-only service. Main-thread request/poll/access; one owned asynchronous task.
class MeshThumbnailPreparation
{
public:
    enum class State { Pending, Preparing, CacheHit, Prepared, Failed, Cancelled };
    struct Identity
    {
        std::filesystem::path project;
        std::string assetReference;
        std::string sourceFingerprint;
        std::string renderVersion = "mesh-v3-camera-yaw180-neutral-studio3-normalized-bounds-zoom1.30-margin0.90-agx-256";
        std::uint32_t importRevision = ModelImporter::kImportRevision;
        std::string uvPolicy = "FlipV";
        MeshImportUnits units;
    };
    struct Job
    {
        Identity identity;
        State state = State::Pending;
        std::string error;
        ImportedMesh geometry;
        MaterialThumbnailDiskCache::Image cachedPixels;
        MaterialThumbnailDiskCache::Source diskSource;
    };

    // Eight browser entries plus one independent Scene Object Inspector consumer.
    static constexpr size_t kJobCapacity = 9;
    static constexpr size_t kMaximumJobBytes = 64 * 1024 * 1024;
    static constexpr size_t kMaximumRetainedBytes = 128 * 1024 * 1024;

    MeshThumbnailPreparation() = default;
    ~MeshThumbnailPreparation();
    bool Request(const ProjectPaths& paths, const ProjectAssetRegistry& registry,
        const AssetID& id, std::uint64_t& ticket, std::string& error);
    // Main thread, outside recording. Only polls ready futures; never waits for import.
    void Poll(const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    const Job* Find(std::uint64_t ticket) const noexcept;
    // Transfers prepared geometry without duplicating the bounded CPU allocation.
    bool TakeGeometry(std::uint64_t ticket, ImportedMesh& geometry);
    bool TakeCachedPixels(std::uint64_t ticket, MaterialThumbnailDiskCache::Image& pixels);
    void Cancel(std::uint64_t ticket);
    void Discard(std::uint64_t ticket);
    void Shutdown();

private:
    struct Source
    {
        std::filesystem::path project;
        std::filesystem::path path;
        AssetRecord record;
        std::uintmax_t size = 0;
        std::filesystem::file_time_type modified{};
        std::string previousFingerprint;
    };
    struct Entry
    {
        Job job;
        Source source;
        size_t bytes = 0;
    };
    struct Completion
    {
        Job job;
        Source source;
        size_t bytes = 0;
        bool reuse = false;
        // Pins source bytes until main-thread identity/registration revalidation.
        std::shared_ptr<void> sourceLock;
    };
    static Completion Prepare(Source source);
    void StartNext();
    void Release(Entry& entry);

    std::map<std::uint64_t, Entry> m_jobs;
    std::deque<std::uint64_t> m_pending;
    std::future<Completion> m_worker;

    std::uint64_t m_activeTicket = 0;
    std::uint64_t m_nextTicket = 1;
    size_t m_retainedBytes = 0;
};
