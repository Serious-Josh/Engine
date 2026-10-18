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
    using PreparationStatus = ProjectMeshPreparationService::Status;

    PreparationStatus MapStatus(CookedMeshSource::Status status)
    {
        using S = CookedMeshSource::Status;
        using P = PreparationStatus;

        switch(status)
        {
            case S::Success: return P::Available;
            case S::Missing: return P::Missing;
            case S::Stale: return P::Stale;
            case S::Corrupt: return P::Corrupt;
            case S::Unsupported: return P::Unsupported;
            case S::IncompatibleCook: return P::IncompatibleCook;
            case S::IOFailure: return P::IOFailure;
            case S::AdmissionLimit: return P::AdmissionLimit;
            case S::AllocationFailure: return P::AllocationFailure;
            case S::IdentityFailure: return P::IdentityFailure;
        }

        return P::Failed;
    }

    PreparationStatus MapStatus(CookedMesh::Status status)
    {
        using S = CookedMesh::Status;
        using P = PreparationStatus;

        switch(status)
        {
            case S::Success: return P::Available;
            case S::Corrupt: return P::Corrupt;
            case S::Unsupported: return P::Unsupported;
            case S::IncompatibleCook: return P::IncompatibleCook;
            case S::AdmissionLimit: return P::AdmissionLimit;
            case S::AllocationFailure: return P::AllocationFailure;
            case S::IdentityFailure: return P::IdentityFailure;
        }

        return P::Failed;
    }

    PreparationStatus CheckCooked(const CookedMeshSource::Provider& provider,
        const CookedMeshSource::AssetEvidence& evidence, std::string& error)
    {
        if(provider.Mapping() != evidence.mapping)
        {
            error = "Cooked completion belongs to a different provider mapping.";
            return PreparationStatus::Stale;
        }

        const auto current = provider.CheckCurrent();

        if(current.status != CookedMeshSource::Status::Success)
        {
            error = current.message;
            return current.status == CookedMeshSource::Status::Missing ? PreparationStatus::Stale : MapStatus(current.status);
        }

        const auto lookup = provider.Lookup(evidence.assetID);

        if(lookup.status != CookedMeshSource::Status::Success || *lookup.evidence != evidence)
        {
            error = "Cooked AssetID, compatibility, payload or recipe mapping changed.";
            return PreparationStatus::Stale;
        }

        return PreparationStatus::Available;
    }

    std::string CookedDigest(const CookedMesh::Evidence& evidence)
    {
        std::string result;
        constexpr char hex[] = "0123456789abcdef";

        for(auto byte : evidence.payloadSHA256)
        {
            result += hex[byte >> 4];
            result += hex[byte & 15];
        }

        return result;
    }

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

    struct CookedCPULease
    {
        std::shared_ptr<Reservation> reservation;
        std::shared_ptr<const ImportedMesh> geometry;
    };

    struct CookedMetadataLease
    {
        std::shared_ptr<Reservation> reservation;
        std::shared_ptr<const CookedMesh::MetadataSnapshot> metadata;
    };
}

struct ProjectMeshPreparationService::State
{
    enum class Stage { Hash, HashReady, Import, Verify, Cooked, Done };

    struct Job
    {
        Identity identity;
        Stage stage = Stage::Hash;
        Status status = Status::Pending;
        bool cacheHit = false;
        std::shared_ptr<const ImportedMesh> geometry;
        std::shared_ptr<const CookedMesh::MetadataSnapshot> metadata;
        std::shared_ptr<const CookedMeshSource::Provider> provider;
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

    RequestResult Submit(Identity identity, std::uint64_t generation,
        std::shared_ptr<const CookedMeshSource::Provider> provider = {},
        std::shared_ptr<const ImportedMesh> cached = {})
    {
        std::lock_guard lock(mutex);
        std::erase_if(tokens, [](const auto& entry) { return entry.second.expired(); });
        std::erase_if(jobs, [](const auto& weak) { return weak.expired(); });

        if (tokens.size() >= limits.requests || nextTicket == UINT64_MAX)
        {
            return {Status::Retry, 0, "Mesh subscriber capacity exhausted."};
        }

        std::shared_ptr<Job> job;
        size_t pending = 0;

        for (const auto& weak : jobs)
        {
            if (const auto existing = weak.lock())
            {
                if (existing->stage != Stage::Done)
                {
                    ++pending;
                }

                auto comparable = existing->identity;
                comparable.digest.clear();

                if (comparable == identity && Wanted(existing))
                {
                    job = existing;
                }
            }
        }

        bool created = false;

        if (!job)
        {
            if (pending >= limits.pending || Completed() >= limits.completed ||
                bytes->load() > limits.retainedBytes - limits.preparationBytes)
            {
                return {Status::Retry, 0, "Mesh pending/completed capacity or consumer-held memory budget exhausted."};
            }

            job = std::make_shared<Job>();
            job->identity = std::move(identity);
            job->provider = std::move(provider);
            job->geometry = std::move(cached);
            job->cacheHit = bool(job->geometry);
            job->stage = job->provider ? Stage::Cooked : Stage::Hash;
            job->reservation = std::make_shared<Reservation>();
            job->reservation->accounting = bytes;
            job->reservation->bytes = limits.preparationBytes;
            bytes->fetch_add(limits.preparationBytes);
            jobs.push_back(job);
            created = true;
        }

        const auto ticket = nextTicket++;
        auto current = std::make_shared<std::atomic_bool>(true);
        tokens.emplace(ticket, current);

        try
        {
            subscribers.emplace(ticket, Subscriber{job, current, generation});

            if (created)
            {
                queue.push_back(job);
            }
        }
        catch (...)
        {
            tokens.erase(ticket);
            subscribers.erase(ticket);
            throw;
        }

        wake.notify_one();
        return {job->stage == Stage::Done && (job->geometry || job->metadata) ? Status::Available : Status::Accepted, ticket, {}};
    }

    void PrepareCooked(const std::shared_ptr<Job>& job, Status& status, std::string& error,
        std::shared_ptr<const ImportedMesh>& geometry, std::shared_ptr<const CookedMesh::MetadataSnapshot>& metadata,
        size_t& retained)
    {
        status = CheckCooked(*job->provider, *job->identity.cooked, error);
        if(status != Status::Available) { return; }
        CookedMesh::Policy policy;
        policy.maxDecodedBytes = limits.geometryBytes;
        auto acquired = job->provider->Acquire(job->identity.cooked->assetID, policy);

        if(acquired.status != CookedMeshSource::Status::Success)
        {
            status = MapStatus(acquired.status);
            error = acquired.message;
            return;
        }

        if(acquired.lease->Evidence() != *job->identity.cooked)
        {
            status = Status::Stale;
            error = "Acquired payload evidence differs from the requested cooked mapping.";
            return;
        }

        auto lease = std::make_shared<CookedCPULease>();
        lease->reservation = job->reservation;
        const auto snapshot = acquired.lease->Metadata();

        if(job->identity.mode == PreparationMode::Geometry)
        {
            if(job->cacheHit)
            {
                lease->geometry = job->geometry;
            }
            else
            {
                const auto decoded = CookedMesh::Decode(acquired.lease->Bytes(), policy);

                if(decoded.status != CookedMesh::Status::Success)
                {
                    status = MapStatus(decoded.status);
                    error = decoded.message;
                    return;
                }

                if(decoded.evidence != job->identity.cooked->payload)
                {
                    status = Status::Corrupt;
                    error = "Decoded cooked identity differs from requested payload evidence.";
                    return;
                }

                lease->geometry = decoded.geometry;
            }

            if(lease->geometry->vertices.size() != snapshot->vertexCount || lease->geometry->indices.size() != snapshot->indexCount ||
                lease->geometry->metadata != snapshot->metadata ||
                lease->geometry->boundsMin.x != snapshot->boundsMin.x ||
                lease->geometry->boundsMin.y != snapshot->boundsMin.y ||
                lease->geometry->boundsMin.z != snapshot->boundsMin.z ||
                lease->geometry->boundsMax.x != snapshot->boundsMax.x ||
                lease->geometry->boundsMax.y != snapshot->boundsMax.y ||
                lease->geometry->boundsMax.z != snapshot->boundsMax.z)
            {
                status = Status::Corrupt;
                error = "Cached/decoded cooked geometry differs from exact payload metadata.";
                return;
            }
        }

        retained = sizeof(CookedCPULease) + sizeof(CookedMetadataLease) + sizeof(CookedMesh::MetadataSnapshot) + 256 +
            ProjectMeshCPUCache::MetadataBytes(snapshot->metadata);

        if(retained > limits.preparationBytes)
        {
            status = Status::AdmissionLimit;
            error = "Cooked retained metadata exceeds service allocation limits.";
            return;
        }

        if(lease->geometry)
        {
            const auto geometryBytes = ProjectMeshCPUCache::GeometryBytes(*lease->geometry);

            if(geometryBytes > limits.geometryBytes || geometryBytes > limits.preparationBytes - retained)
            {
                status = Status::AdmissionLimit;
                error = "Cooked retained geometry exceeds service allocation limits.";
                return;
            }

            retained += geometryBytes;
        }

        acquired.lease.reset(); // Do not retain encoded bytes with completion/cache output.
        status = CheckCooked(*job->provider, *job->identity.cooked, error);
        if(status != Status::Available) { return; }
        auto metadataLease = std::make_shared<CookedMetadataLease>();
        metadataLease->reservation = job->reservation;
        metadataLease->metadata = snapshot;
        metadata = {metadataLease, snapshot.get()};
        if(lease->geometry) { geometry = {lease, lease->geometry.get()}; }
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
            std::shared_ptr<const CookedMesh::MetadataSnapshot> metadata;
            size_t retainedBytes = 0;

            try
            {
                if(job->identity.sourceKind == MeshPreparationSourceKind::Cooked)
                {
                    PrepareCooked(job, status, error, geometry, metadata, retainedBytes);
                }
                else
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
                        lease->imported = ModelImporter::LoadFlattened(job->identity.path, job->identity.uvPolicy,
                            limits.geometryBytes, job->identity.units);

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
            }
            catch (const std::bad_alloc&)
            {
                status = job->identity.sourceKind == MeshPreparationSourceKind::Cooked ? Status::AllocationFailure : Status::Failed;
                error = "Allocation"; // SSO diagnostic remains available on allocation failure.
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
                if (geometry || metadata)
                {
                    job->reservation->Reduce(retainedBytes);
                }

                job->geometry = std::move(geometry);
                job->metadata = std::move(metadata);
                job->stage = Stage::Done;
            }
        }
    }
};

ProjectMeshCPUCache::Key ProjectMeshPreparationService::Identity::CacheKey() const
{
    if(sourceKind == MeshPreparationSourceKind::Cooked)
    {
        if(!cooked) { throw std::invalid_argument("Cooked cache identity requires typed payload evidence."); }
        return {std::filesystem::path(cooked->mapping.providerNamespace), cooked->assetID.ToString(), CookedDigest(cooked->payload),
            cooked->payload.cookCompatibility, MeshUVOrientation::PreserveSource, MeshPreparationSourceKind::Cooked};
    }

    return {project, assetID, digest, importerRevision, uvPolicy, MeshPreparationSourceKind::FBX, units};
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
    if(m_archiveOnly) { return {Status::Invalid,0,"Loose mesh access rejected by packaged policy."}; }
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
        identity.units = record->meshImportUnits;

        return state.Submit(std::move(identity), generation);
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

ProjectMeshPreparationService::RequestResult ProjectMeshPreparationService::RequestCooked(
    const CookedRequest& request, ProjectMeshCPUCache& cache)
{
    if(m_archiveOnly && (!request.provider || !request.provider->Mapping().projectNamespace.empty()))
        return {Status::Invalid,0,"Development cooked providers rejected by packaged policy."};
    auto& state = *m_state;

    if(!state.OnOwner() || !state.accepting || request.generation != state.generation)
    {
        return {Status::Stale, 0, "Cooked request owner or subscriber generation is stale."};
    }

    if(!request.provider) { return {Status::Invalid, 0, "Cooked preparation requires an explicit provider snapshot."}; }
    if(request.mode != PreparationMode::Geometry && request.mode != PreparationMode::MetadataOnly)
    {
        return {Status::Invalid, 0, "Unsupported cooked preparation mode."};
    }

    try
    {
        const auto lookup = request.provider->Lookup(request.asset);

        if(lookup.status != CookedMeshSource::Status::Success)
        {
            return {MapStatus(lookup.status), 0, std::string(lookup.message)};
        }

        // Reserve for provider metadata validation plus full decoding while its
        // payload and metadata are still alive. Check by subtraction, not overflow-
        // prone addition. Provider acquisition is tightened to the service policy.
        const auto payloadBytes = lookup.evidence->payload.payloadBytes;
        constexpr size_t overhead = 1024 * 1024;
        const size_t copies = request.mode == PreparationMode::Geometry ? 2 : 1;

        if(payloadBytes > state.limits.preparationBytes || overhead > state.limits.preparationBytes - payloadBytes ||
            state.limits.geometryBytes > (state.limits.preparationBytes - payloadBytes - overhead) / copies)
        {
            return {Status::AdmissionLimit, 0, "Cooked payload/geometry/metadata peak exceeds service preparation budget."};
        }

        Identity identity;
        identity.sourceKind = MeshPreparationSourceKind::Cooked;
        identity.mode = request.mode;
        identity.cooked = lookup.evidence;
        identity.assetID = request.asset.ToString();

        identity.importerRevision = 0;
        identity.uvPolicy = MeshUVOrientation::PreserveSource;
        std::shared_ptr<const ImportedMesh> cached;

        if(request.mode == PreparationMode::Geometry)
        {
            cached = cache.FindVerified(identity.CacheKey());
            if(cached && ProjectMeshCPUCache::GeometryBytes(*cached) > state.limits.geometryBytes) { cached.reset(); }
        }

        return state.Submit(std::move(identity), request.generation, request.provider, std::move(cached));
    }
    catch(const std::bad_alloc&) { return {Status::AllocationFailure, 0, "Allocation"}; }
    catch(const std::exception& error) { return {Status::Invalid, 0, error.what()}; }
}

ProjectMeshPreparationService::Status ProjectMeshPreparationService::ValidateCookedCompletion(
    const Completion& completion, std::string& error) const try
{
    error.clear();

    if(!m_state->OnOwner() || !m_state->accepting || completion.generation != Generation())
    {
        error = "Cooked completion service/session generation is stale.";
        return Status::Stale;
    }

    if(!completion.current || !completion.current->load())
    {
        error = "Cooked completion was canceled or invalidated.";
        return Status::Canceled;
    }

    if(completion.identity.sourceKind != MeshPreparationSourceKind::Cooked || !completion.identity.cooked || !completion.provider ||
        completion.identity.assetID != completion.identity.cooked->assetID.ToString() ||
        (completion.metadata && completion.metadata->evidence != completion.identity.cooked->payload))
    {
        error = "Cooked completion source/mapping/metadata evidence is incomplete or inconsistent.";
        return Status::Invalid;
    }

    return CheckCooked(*completion.provider, *completion.identity.cooked, error);
}
catch(const std::bad_alloc&)
{
    error.clear();
    return Status::AllocationFailure;
}

#ifndef NDEBUG
ProjectMeshPreparationService::DiagnosticStage ProjectMeshPreparationService::ObserveDiagnosticStage(std::uint64_t ticket) const
{
    auto& state = *m_state;
    if(!state.OnOwner()) { return DiagnosticStage::Missing; }
    std::lock_guard lock(state.mutex);
    const auto found = state.subscribers.find(ticket);
    if(found == state.subscribers.end()) { return DiagnosticStage::Missing; }
    const auto& job = found->second.job;
    if(job->stage == State::Stage::Done) { return DiagnosticStage::Completed; }
    return std::find(state.queue.begin(), state.queue.end(), job) != state.queue.end() ?
        DiagnosticStage::Queued : DiagnosticStage::Running;
}
#endif

ProjectMeshPreparationService::Completion ProjectMeshPreparationService::PollCooked(std::uint64_t ticket) try
{
    auto& state = *m_state;
    Completion result;
    result.ticket = ticket;

    if(!state.OnOwner())
    {
        result.status = Status::Invalid;
        result.error = "Cooked polling requires the owner thread.";
        return result;
    }

    std::shared_ptr<State::Job> job;

    {
        std::lock_guard lock(state.mutex);
        const auto found = state.subscribers.find(ticket);

        if(found == state.subscribers.end())
        {
            result.status = Status::Canceled;
            result.error = "Cooked request canceled, invalidated or already delivered.";
            return result;
        }

        job = found->second.job;

        if(job->identity.sourceKind != MeshPreparationSourceKind::Cooked)
        {
            result.status = Status::Invalid;
            result.error = "FBX requests require the existing Poll API.";
            return result;
        }

        result.generation = found->second.generation;
        result.current = found->second.current;

        if(job->stage != State::Stage::Done) { return result; }
        result.identity = job->identity;
        result.provider = job->provider;
        result.geometry = job->geometry;
        result.metadata = job->metadata;

        result.cacheHit = job->cacheHit;
        result.status = job->status;
        result.error = job->error;
    }

    if(result.status == Status::Available)
    {
        const auto validity = ValidateCookedCompletion(result, result.error);
        if(validity != Status::Available) { result.status = validity; }
    }

    if(result.status != Status::Available)
    {
        result.geometry.reset();
        result.metadata.reset();
    }

    {
        std::lock_guard lock(state.mutex);
        state.subscribers.erase(ticket);
    }

    state.wake.notify_one();
    return result;
}
catch(const std::bad_alloc&)
{
    Cancel(ticket);
    Completion failure;
    failure.ticket = ticket;
    failure.status = Status::AllocationFailure;
    return failure;
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
        if(job->identity.sourceKind != MeshPreparationSourceKind::FBX)
        {
            result.status = Status::Invalid;
            result.error = "Cooked requests require PollCooked.";
            return result;
        }
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
            record->meshImportUnits != result.identity.units ||
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
    const bool current = m_state->OnOwner() && m_state->accepting && completion.generation == Generation() &&
        completion.current && completion.current->load();

    if(!current || completion.identity.sourceKind == MeshPreparationSourceKind::FBX) { return current; }
    std::string error;
    return ValidateCookedCompletion(completion, error) == Status::Available;
}

bool ProjectMeshPreparationService::IntegrateCache(Completion& completion, ProjectMeshCPUCache& cache,
    std::string& error)
{
    error.clear();

    if (!IsCurrent(completion) || completion.status != Status::Available || !completion.geometry ||
        (completion.identity.sourceKind == MeshPreparationSourceKind::FBX && completion.identity.digest.empty()))
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
