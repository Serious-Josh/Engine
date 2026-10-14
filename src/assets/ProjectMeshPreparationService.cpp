#include "assets/ProjectMeshPreparationService.h"
#include "Renderer/Preview/MaterialThumbnailValidation.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <Windows.h>

namespace
{
    struct SourcePin
    {
        HANDLE handle = INVALID_HANDLE_VALUE;

        explicit SourcePin(const std::filesystem::path& path)
        {
            handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

            if (handle == INVALID_HANDLE_VALUE)
            {
                throw std::runtime_error("Cannot pin mesh source against writes/deletion.");
            }
        }

        ~SourcePin() { CloseHandle(handle); }
        SourcePin(const SourcePin&) = delete;
        SourcePin& operator=(const SourcePin&) = delete;

        std::string Identity(size_t sourceLimit) const
        {
            BY_HANDLE_FILE_INFORMATION info{};

            if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                throw std::runtime_error("Cannot inspect mesh source identity.");
            }

            const auto size = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;

            if (!size || size > sourceLimit)
            {
                throw std::runtime_error("Mesh source exceeds the configured source-file limit or is empty.");
            }

            return std::to_string(info.dwVolumeSerialNumber) + ":" + std::to_string(info.nFileIndexHigh) + ":" +
                std::to_string(info.nFileIndexLow) + ":" + std::to_string(size) + ":" +
                std::to_string(info.ftLastWriteTime.dwHighDateTime) + ":" +
                std::to_string(info.ftLastWriteTime.dwLowDateTime);
        }
    };

    std::string Digest(const std::filesystem::path& path)
    {
        MaterialThumbnailValidation validation;
        return validation.Digest(path);
    }

    struct Reservation
    {
        std::shared_ptr<std::atomic_size_t> accounting;
        size_t bytes = 0;
        ~Reservation()
        {
            if (accounting)
            {
                accounting->fetch_sub(bytes);
            }
        }

        void Reduce(size_t retained)
        {
            accounting->fetch_sub(bytes - retained);
            bytes = retained;
        }
    };

    struct GeometryLease
    {
        // Main-thread cache hits borrow immutable storage; imports own their buffers.
        std::shared_ptr<Reservation> reservation;
        std::shared_ptr<const ImportedMesh> cached;
        ImportedMesh imported;
    };
}

struct ProjectMeshPreparationService::State
{
    enum class Stage { Hash, HashReady, Import, Verify, Done };

    struct Job
    {
        Identity identity;
        Stage stage = Stage::Hash;
        Status status = Status::Pending;
        bool cacheHit = false;
        std::shared_ptr<const ImportedMesh> geometry;
        std::shared_ptr<Reservation> reservation;
        std::string error;
    };

    struct Subscriber
    {
        std::shared_ptr<Job> job;
        std::shared_ptr<std::atomic_bool> current;
        std::uint64_t generation;
    };

    Limits limits;
    const std::thread::id owner = std::this_thread::get_id();
    std::shared_ptr<std::atomic_size_t> bytes = std::make_shared<std::atomic_size_t>(0);
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    std::deque<std::shared_ptr<Job>> queue;
    std::vector<std::weak_ptr<Job>> jobs;
    std::map<std::uint64_t, Subscriber> subscribers;
    std::map<std::uint64_t, std::weak_ptr<std::atomic_bool>> tokens;

    std::uint64_t generation = 1;
    std::uint64_t nextTicket = 1;
    bool accepting = false;
    bool stop = false;

    bool OnOwner() const { return owner == std::this_thread::get_id(); }

    bool Wanted(const std::shared_ptr<Job>& job) const
    {
        return std::any_of(subscribers.begin(), subscribers.end(), [&](const auto& entry)
        {
            return entry.second.job == job;
        });
    }

    size_t Completed() const
    {
        size_t count = 0;

        for (const auto& weak : jobs)
        {
            if (const auto job = weak.lock(); job &&
                (job->stage == Stage::HashReady || job->stage == Stage::Done))
            {
                ++count;
            }
        }

        return count;
    }

    void Run()
    {
        for (;;)
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return stop || (!queue.empty() && Completed() < limits.completed); });

            if (stop)
            {
                return;
            }

            auto job = queue.front();
            queue.pop_front();

            if (!Wanted(job))
            {
                continue;
            }

            const auto stage = job->stage;
            lock.unlock();
            std::string digest;
            std::string error;
            Status status = Status::Available;
            std::shared_ptr<const ImportedMesh> geometry;
            size_t retainedBytes = 0;

            try
            {
                // Only copied, canonical source information reaches this worker.
                // The live registry, cache, scene, and renderer are never accessed.
                SourcePin source(job->identity.path);

                if (std::filesystem::canonical(job->identity.path) != job->identity.path ||
                    source.Identity(limits.sourceBytes) != job->identity.sourceIdentity)
                {
                    status = Status::Stale;
                    throw std::runtime_error("Mesh source identity changed before preparation.");
                }

                digest = Digest(job->identity.path);

                if (stage == Stage::Verify)
                {
                    if (digest != job->identity.digest)
                    {
                        status = Status::Stale;
                        throw std::runtime_error("Cached mesh source digest changed before completion.");
                    }

                    geometry = job->geometry;
                }

                if (stage == Stage::Import)
                {
                    if (digest != job->identity.digest)
                    {
                        status = Status::Stale;
                        throw std::runtime_error("Mesh source digest changed before import.");
                    }

                    auto lease = std::make_shared<GeometryLease>();
                    lease->reservation = job->reservation;
                    // ModelImporter uses a separate Assimp::Importer per call;
                    // it installs no global logger, IO handler, or shared scene.
                    lease->imported = ModelImporter::LoadFlattened(job->identity.path, job->identity.uvPolicy, limits.geometryBytes);

                    if (ProjectMeshCPUCache::GeometryBytes(lease->imported) > limits.geometryBytes)
                    {
                        throw std::runtime_error("Prepared mesh exceeds the configured geometry limit.");
                    }

                    if (Digest(job->identity.path) != digest ||
                        source.Identity(limits.sourceBytes) != job->identity.sourceIdentity)
                    {
                        status = Status::Stale;
                        throw std::runtime_error("Mesh source changed during import.");
                    }

                    geometry = std::shared_ptr<const ImportedMesh>(lease, &lease->imported);
                }

                if (geometry)
                {
                    retainedBytes = ProjectMeshCPUCache::GeometryBytes(*geometry);
                }
            }
            catch (const std::exception& exception)
            {
                if (status != Status::Stale)
                {
                    status = Status::Failed;
                }

                error = exception.what();
            }
            catch (...)
            {
                status = Status::Failed;
                error = "Unknown mesh preparation failure.";
            }

            lock.lock();

            if (stop || !Wanted(job))
            {
                continue;
            }

            job->identity.digest = std::move(digest);
            job->error = std::move(error);
            job->status = status;

            if (stage == Stage::Hash && status == Status::Available)
            {
                job->stage = Stage::HashReady;
            }
            else
            {
                if (geometry)
                {
                    job->reservation->Reduce(retainedBytes);
                }

                job->geometry = std::move(geometry);
                job->stage = Stage::Done;
            }
        }
    }
};

ProjectMeshCPUCache::Key ProjectMeshPreparationService::Identity::CacheKey() const
{
    return {project, assetID, digest, importerRevision, uvPolicy};
}

ProjectMeshPreparationService::ProjectMeshPreparationService() : m_state(std::make_unique<State>()) {}
ProjectMeshPreparationService::~ProjectMeshPreparationService() { Shutdown(); }

bool ProjectMeshPreparationService::Start(const Limits& limits, std::string& error)
{
    auto& state = *m_state;
    error.clear();

    if (!state.OnOwner() || state.worker.joinable() || !limits.requests || !limits.pending || !limits.completed ||
        !limits.sourceBytes || !limits.geometryBytes || limits.geometryBytes > limits.preparationBytes ||
        limits.sourceBytes > limits.preparationBytes / 2 || limits.preparationBytes > limits.retainedBytes)
    {
        error = "Invalid mesh preparation limits, owner thread, or service already started.";
        return false;
    }

    state.limits = limits;
    state.stop = false;
    state.accepting = true;

    try
    {
        state.worker = std::thread([&state] { state.Run(); });
        return true;
    }
    catch (const std::exception& exception)
    {
        state.accepting = false;
        error = exception.what();
        return false;
    }
}

void ProjectMeshPreparationService::Shutdown()
{
    auto& state = *m_state;

    {
        std::lock_guard lock(state.mutex);
        state.accepting = false;
        state.stop = true;

        for (const auto& [ticket, weak] : state.tokens)
        {
            if (const auto current = weak.lock())
            {
                current->store(false);
            }
        }

        state.subscribers.clear();
        state.tokens.clear();
        state.queue.clear();
    }

    state.wake.notify_all();

    if (state.worker.joinable())
    {
        state.worker.join();
    }

    state.jobs.clear();
}

std::uint64_t ProjectMeshPreparationService::Generation() const { return m_state->generation; }
size_t ProjectMeshPreparationService::RetainedBytes() const { return m_state->bytes->load(); }

void ProjectMeshPreparationService::Invalidate()
{
    auto& state = *m_state;

    if (!state.OnOwner())
    {
        return;
    }

    std::lock_guard lock(state.mutex);

    if (state.generation == UINT64_MAX)
    {
        state.accepting = false;
    }
    else
    {
        ++state.generation;
    }

    for (const auto& [ticket, weak] : state.tokens)
    {
        if (const auto current = weak.lock())
        {
            current->store(false);
        }
    }

    state.tokens.clear();
    state.subscribers.clear();
    state.queue.clear();
    state.wake.notify_all();
}

void ProjectMeshPreparationService::Cancel(std::uint64_t ticket)
{
    auto& state = *m_state;

    if (!state.OnOwner())
    {
        return;
    }

    std::lock_guard lock(state.mutex);
    const auto found = state.tokens.find(ticket);

    if (found != state.tokens.end())
    {
        if (const auto current = found->second.lock())
        {
            current->store(false);
        }

        state.tokens.erase(found);
    }

    state.subscribers.erase(ticket);
    std::erase_if(state.queue, [&](const auto& job) { return !state.Wanted(job); });
    state.wake.notify_all();
}

ProjectMeshPreparationService::RequestResult ProjectMeshPreparationService::Request(const ProjectPaths& paths,
    const ProjectAssetRegistry& registry, const AssetID& asset, std::uint64_t generation)
{
    auto& state = *m_state;

    if (!state.OnOwner() || !state.accepting || generation != state.generation)
    {
        return {Status::Stale, 0, "Mesh request owner or subscriber generation is stale."};
    }

    try
    {
        const auto* record = registry.Find(asset);
        const auto resolved = ProjectAssetResolver::Resolve(paths, registry, asset);

        if (!record || record->type != AssetType::Mesh || resolved.status != AssetAvailability::Available ||
            _wcsicmp(resolved.resolvedPath.extension().c_str(), L".fbx") != 0)
        {
            return {Status::Invalid, 0, "Mesh request requires an accessible registered FBX within the project."};
        }

        SourcePin source(resolved.resolvedPath);
        Identity identity;
        identity.project = std::filesystem::canonical(paths.GetRoot());
        identity.path = std::filesystem::canonical(resolved.resolvedPath);
        identity.assetID = asset.ToString();
        identity.sourceIdentity = source.Identity(state.limits.sourceBytes);

        std::lock_guard lock(state.mutex);
        std::erase_if(state.tokens, [](const auto& entry) { return entry.second.expired(); });
        std::erase_if(state.jobs, [](const auto& weak) { return weak.expired(); });

        if (state.tokens.size() >= state.limits.requests || state.nextTicket == UINT64_MAX)
        {
            return {Status::Retry, 0, "Mesh subscriber capacity exhausted."};
        }

        std::shared_ptr<State::Job> job;
        size_t pending = 0;

        for (const auto& weak : state.jobs)
        {
            if (const auto existing = weak.lock())
            {
                if (existing->stage != State::Stage::Done)
                {
                    ++pending;
                }

                auto comparable = existing->identity;
                comparable.digest.clear();

                if (comparable == identity && state.Wanted(existing))
                {
                    job = existing;
                }
            }
        }

        bool created = false;

        if (!job)
        {
            if (pending >= state.limits.pending || state.Completed() >= state.limits.completed ||
                state.bytes->load() > state.limits.retainedBytes - state.limits.preparationBytes)
            {
                return {Status::Retry, 0, "Mesh pending/completed capacity or consumer-held memory budget exhausted."};
            }

            job = std::make_shared<State::Job>();
            job->identity = std::move(identity);
            job->reservation = std::make_shared<Reservation>();
            job->reservation->accounting = state.bytes;
            job->reservation->bytes = state.limits.preparationBytes;
            state.bytes->fetch_add(state.limits.preparationBytes);
            state.jobs.push_back(job);
            created = true;
        }

        const auto ticket = state.nextTicket++;
        auto current = std::make_shared<std::atomic_bool>(true);
        state.tokens.emplace(ticket, current);

        try
        {
            state.subscribers.emplace(ticket, State::Subscriber{job, current, generation});

            if (created)
            {
                state.queue.push_back(job);
            }
        }
        catch (...)
        {
            state.tokens.erase(ticket);
            state.subscribers.erase(ticket);
            throw;
        }

        state.wake.notify_one();
        return {job->stage == State::Stage::Done && job->geometry ? Status::Available : Status::Accepted, ticket, {}};
    }
    catch (const std::bad_alloc&)
    {
        return {Status::Retry, 0, "Mesh request storage allocation failed."};
    }
    catch (const std::exception& exception)
    {
        return {Status::Invalid, 0, exception.what()};
    }
}

ProjectMeshPreparationService::Completion ProjectMeshPreparationService::Poll(std::uint64_t ticket,
    const ProjectPaths& paths, const ProjectAssetRegistry& registry, ProjectMeshCPUCache& cache)
{
    auto& state = *m_state;
    Completion result;
    result.ticket = ticket;

    if (!state.OnOwner())
    {
        result.status = Status::Invalid;
        result.error = "Mesh polling requires the owner thread.";
        return result;
    }

    std::shared_ptr<State::Job> job;

    {
        std::lock_guard lock(state.mutex);
        const auto found = state.subscribers.find(ticket);

        if (found == state.subscribers.end())
        {
            result.status = Status::Canceled;
            result.error = "Mesh request canceled, invalidated or already delivered.";
            return result;
        }

        job = found->second.job;
        result.generation = found->second.generation;
        result.current = found->second.current;
        result.cacheHit = job->cacheHit;

        if (job->stage == State::Stage::Hash || job->stage == State::Stage::Import ||
            job->stage == State::Stage::Verify)
        {
            return result;
        }

        result.identity = job->identity;
    }

    try
    {
        // Registry and cheap file-identity rejection on delivery. Worker digests
        // prove preparation freshness; future residency still rehashes its source.
        result.status = Status::Stale;
        const auto asset = AssetID::Parse(result.identity.assetID);
        const auto* record = asset ? registry.Find(*asset) : nullptr;
        const auto resolved = asset ? ProjectAssetResolver::Resolve(paths, registry, *asset) : AssetResolution{};

        if (!IsCurrent(result) || !record || record->type != AssetType::Mesh ||
            std::filesystem::canonical(paths.GetRoot()) != result.identity.project ||
            resolved.status != AssetAvailability::Available || resolved.resolvedPath != result.identity.path)
        {
            result.status = Status::Stale;
            throw std::runtime_error("Mesh completion project, registry or subscriber is stale.");
        }

        SourcePin source(result.identity.path);

        if (source.Identity(state.limits.sourceBytes) != result.identity.sourceIdentity)
        {
            result.status = Status::Stale;
            throw std::runtime_error("Mesh completion source identity is stale.");
        }

        result.status = Status::Pending;

        if (job->stage == State::Stage::HashReady)
        {
            auto cached = cache.FindVerified(result.identity.CacheKey());
            std::shared_ptr<const ImportedMesh> geometry;

            if (cached && ProjectMeshCPUCache::GeometryBytes(*cached) <= state.limits.geometryBytes)
            {
                auto lease = std::make_shared<GeometryLease>();
                lease->reservation = job->reservation;
                lease->cached = std::move(cached);
                geometry = std::shared_ptr<const ImportedMesh>(lease, lease->cached.get());
            }

            std::lock_guard lock(state.mutex);

            // Even cache hits get a fresh worker-side digest check. Never hash
            // on the main thread and never hold the source pin across frames.
            state.queue.push_back(job);

            if (geometry)
            {
                job->geometry = std::move(geometry);
                job->cacheHit = true;
                job->stage = State::Stage::Verify;
            }
            else
            {
                job->stage = State::Stage::Import;
            }

            result.error = job->cacheHit ? "CPU mesh cache hit; worker freshness verification scheduled." :
                "CPU mesh cache miss; background FBX import scheduled.";
            state.wake.notify_one();
            return result;
        }

        result.status = job->status;
        result.geometry = job->geometry;
        result.error = job->error;
    }
    catch (const std::bad_alloc&)
    {
        result.status = Status::Retry;
        result.error = "Mesh completion storage allocation failed; poll again.";
        return result;
    }
    catch (const std::exception& exception)
    {
        if (result.status != Status::Stale)
        {
            result.status = Status::Failed;
        }

        result.error = exception.what();
        result.geometry.reset();
    }

    {
        std::lock_guard lock(state.mutex);
        state.subscribers.erase(ticket);
    }

    state.wake.notify_one();
    return result;
}

bool ProjectMeshPreparationService::IsCurrent(const Completion& completion) const
{
    return m_state->OnOwner() && m_state->accepting && completion.generation == Generation() &&
        completion.current && completion.current->load();
}

bool ProjectMeshPreparationService::IntegrateCache(Completion& completion, ProjectMeshCPUCache& cache,
    std::string& error)
{
    error.clear();

    if (!IsCurrent(completion) || completion.status != Status::Available || !completion.geometry ||
        completion.identity.digest.empty())
    {
        error = "Only a current, validated mesh completion can enter the CPU cache.";
        return false;
    }

    try
    {
        completion.geometry = cache.StorePreparedVerified(completion.identity.CacheKey(), completion.geometry);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

std::string ProjectMeshPreparationService::InspectSourceIdentity(const std::filesystem::path& path)
{
    SourcePin source(path);
    return source.Identity((std::numeric_limits<size_t>::max)());
}
