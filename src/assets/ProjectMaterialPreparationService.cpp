#include "assets/ProjectMaterialPreparationService.h"
#include "assets/MaterialAssetSerializer.h"
#include "Renderer/Preview/MaterialThumbnailValidation.h"
#include <Windows.h>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <functional>
#include <stdexcept>

class MaterialSourceLease
{
public:
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::shared_ptr<const CookedMeshSource::ArchiveProvider> archive;
    bool valid = true;
    BY_HANDLE_FILE_INFORMATION identity{};
    ~MaterialSourceLease() { Revoke(); }
    void Revoke() noexcept
    {
        valid = false;
        if(handle != INVALID_HANDLE_VALUE) CloseHandle(std::exchange(handle, INVALID_HANDLE_VALUE));
    }
    bool Matches(const std::filesystem::path& path) const
    {
        if(handle == INVALID_HANDLE_VALUE) return false;
        const auto probe = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if(probe == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION current{};
        const bool valid = GetFileInformationByHandle(probe, &current) &&
            !(current.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
            current.dwVolumeSerialNumber == identity.dwVolumeSerialNumber &&
            current.nFileIndexHigh == identity.nFileIndexHigh && current.nFileIndexLow == identity.nFileIndexLow;
        CloseHandle(probe);
        return valid;
    }
};

bool ProjectMaterialPreparationService::Candidate::HasPinnedSources() const noexcept
{
    if(!source || !source->valid || (!archive && source->handle == INVALID_HANDLE_VALUE)) return false;
    for(const auto& texture : textures)
        if(texture && (!texture->source || !texture->current || !texture->current->load())) return false;
    return true;
}

namespace
{
    using Service = ProjectMaterialPreparationService;
    auto References(const MaterialAssetData& material)
    {
        return std::array{material.baseColorTexture, material.normalTexture, material.roughnessTexture,
            material.metallicTexture, material.armTexture, material.aoTexture, material.heightTexture};
    }
    ProjectTextureInterpretation Interpretation(size_t slot)
    {
        return slot == 0 ? ProjectTextureInterpretation::SRGBColor : slot == 1 ?
            ProjectTextureInterpretation::NormalMap : ProjectTextureInterpretation::LinearData;
    }
    void PrepareMaterial(Service::Candidate& candidate, std::shared_ptr<MaterialSourceLease>& source,
        const std::function<void()>& pinned)
    {
        auto pin = std::make_shared<MaterialSourceLease>();
        if(candidate.archive)
        {
            const auto id = *AssetID::Parse(candidate.assetID);
            auto raw = candidate.archive->ReadEntry(id,EngineAssetPak::Type::Material,CookedSurface::Version,CookedSurface::MaterialLimit);
            auto decoded = CookedSurface::DecodeMaterial(raw);
            if(decoded.id!=id || decoded.generation!=candidate.contentGeneration) throw std::runtime_error("Archive material identity/generation mismatch.");
            candidate.parameters=std::move(decoded.parameters); candidate.archiveDependencies=std::move(decoded.dependencies);
            pin->archive=candidate.archive; source=std::move(pin); return;
        }
        pin->handle = CreateFileW(candidate.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if(pin->handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(pin->handle, &pin->identity) ||
            (pin->identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !(pin->identity.nFileIndexHigh || pin->identity.nFileIndexLow) || pin->identity.nFileSizeHigh ||
            pin->identity.nFileSizeLow > Service::MaterialByteLimit)
            throw std::runtime_error("Cannot pin material source, or material exceeds byte limit.");
        std::array<wchar_t, 32768> name{};
        const auto length = GetFinalPathNameByHandleW(pin->handle, name.data(), static_cast<DWORD>(name.size()), FILE_NAME_NORMALIZED);
        if(!length || length >= name.size()) throw std::runtime_error("Cannot validate pinned material path.");
        std::wstring finalPath(name.data(), length);
        if(finalPath.starts_with(L"\\\\?\\UNC\\")) finalPath = L"\\\\" + finalPath.substr(8);
        else if(finalPath.starts_with(L"\\\\?\\")) finalPath.erase(0, 4);
        if(!ProjectAssetRegistry::PathsEqual(finalPath, candidate.path))
            throw std::runtime_error("Pinned material path changed.");
        if(pinned) pinned();
        MaterialThumbnailValidation hashing;
        candidate.digest = hashing.Digest(candidate.path);
        std::string error;
        if(!MaterialAssetSerializer::LoadFromFile(candidate.path, candidate.parameters, &error))
            throw std::runtime_error(error);
        MaterialThumbnailValidation verification;
        if(verification.Digest(candidate.path) != candidate.digest || !pin->Matches(candidate.path))
            throw std::runtime_error("Material changed during preparation.");
        source = std::move(pin);
    }
}

struct ProjectMaterialPreparationService::State
{
    struct Job
    {
        std::shared_ptr<Candidate> candidate;
        std::shared_ptr<MaterialSourceLease> source;
        std::array<std::uint64_t, 7> tickets{};
        std::atomic_bool canceled = false;
        bool done = false;
        Result result;
    };
    ProjectTexturePreparationService& textures;
    std::map<std::uint64_t, std::shared_ptr<Job>> jobs; // main-thread only
    std::mutex mutex; // protects queue and worker result handoff
    std::condition_variable wake;
    std::deque<std::shared_ptr<Job>> queue;
    bool stopping = false;
    std::uint64_t nextTicket = 1;
    std::shared_ptr<std::atomic_size_t> retained = std::make_shared<std::atomic_size_t>(0);
    std::thread worker;
#ifdef ENGINE_TEXTURE_PREPARATION_TESTS
    std::function<void()> pinnedHook;
#endif
    explicit State(ProjectTexturePreparationService& service) : textures(service), worker([this] { Run(); }) {}
    void Run()
    {
        for(;;)
        {
            std::shared_ptr<Job> job;
            std::function<void()> pinned;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || !queue.empty(); });
                if(stopping) return;
                job = queue.front(); queue.pop_front();
#ifdef ENGINE_TEXTURE_PREPARATION_TESTS
                pinned = pinnedHook;
#endif
            }
            std::string error;
            if(!job->canceled)
            {
                try { PrepareMaterial(*job->candidate, job->source, pinned); }
                catch(const std::exception& exception) { error = exception.what(); }
            }
            std::lock_guard lock(mutex);
            if(job->canceled) job->source.reset();
            job->result = {error.empty() ? Status::Pending : Status::Failed, {}, std::move(error)};
            job->done = true;
        }
    }
};

ProjectMaterialPreparationService::ProjectMaterialPreparationService(ProjectTexturePreparationService& textures)
    : m_state(std::make_unique<State>(textures)) {}
ProjectMaterialPreparationService::~ProjectMaterialPreparationService() { Shutdown(); }
#ifdef ENGINE_TEXTURE_PREPARATION_TESTS
void ProjectMaterialPreparationService::SetPinnedTestHook(std::function<void()> hook)
{
    std::lock_guard lock(m_state->mutex);
    m_state->pinnedHook = std::move(hook);
}
#endif

ProjectMaterialPreparationService::Status ProjectMaterialPreparationService::Request(const ProjectPaths& paths,
    const ProjectAssetRegistry& registry, const AssetID& id, std::uint64_t session,
    std::uint64_t& ticket, std::string& error)
{
    if(m_archiveOnly) { ticket=0; error="Loose material access rejected by packaged policy."; return Status::Unsupported; }
    ticket = 0; error.clear();
    auto& state = *m_state;
    if(state.stopping) { error = "Material preparation stopped."; return Status::Stale; }
    if(state.jobs.size() >= Capacity || state.retained->load() >= Capacity || state.nextTicket == UINT64_MAX)
    { error = "Material request capacity exhausted."; return Status::Retry; }
    try
    {
        const auto* record = registry.Find(id);
        if(!record || record->type != AssetType::Material)
        { error = "A registered material is required."; return Status::Unsupported; }
        const auto resolved = ProjectAssetResolver::Resolve(paths, registry, id);
        if(resolved.status != AssetAvailability::Available) throw std::runtime_error("Material source unavailable.");
        auto job = std::make_shared<State::Job>();
        auto storage = std::make_unique<Candidate>();
        auto retained = state.retained;
        ++*retained;
        job->candidate = std::shared_ptr<Candidate>(storage.release(), [retained](Candidate* value)
        { delete value; --*retained; });
        auto& candidate = *job->candidate;
        candidate.assetID = id.ToString(); candidate.project = std::filesystem::canonical(paths.GetRoot());
        candidate.path = resolved.resolvedPath; candidate.session = session;
        const auto proposed = state.nextTicket++;
        state.jobs.emplace(proposed, job);
        try
        {
            std::lock_guard lock(state.mutex);
            // Canceled jobs are removed before enqueueing; one active worker may remain.
            std::erase_if(state.queue, [](const auto& queued) { return queued->canceled.load(); });
            state.queue.push_back(job);
        }
        catch(...) { state.jobs.erase(proposed); throw; }
        ticket = proposed; state.wake.notify_one(); return Status::Pending;
    }
    catch(const std::exception& exception) { error = exception.what(); return Status::Failed; }
}

ProjectMaterialPreparationService::Status ProjectMaterialPreparationService::RequestArchive(std::shared_ptr<const CookedMeshSource::ArchiveProvider> archive,
    const AssetID& id,std::uint64_t session,std::uint64_t& ticket,std::string& error)
{
    ticket=0; auto& state=*m_state;
    if(state.stopping) return Status::Stale;
    if(state.jobs.size()>=Capacity || state.retained->load()>=Capacity || state.nextTicket==UINT64_MAX) return Status::Retry;
    try
    {
        const auto* entry=archive ? archive->ArchiveEntry(id) : nullptr;
        if(!entry || entry->type!=EngineAssetPak::Type::Material || entry->payloadVersion!=CookedSurface::Version || entry->storedBytes>MaterialByteLimit)
            throw std::runtime_error("Missing/unsupported archive material.");
        auto job=std::make_shared<State::Job>(); auto storage=std::make_unique<Candidate>(); auto retained=state.retained; ++*retained;
        job->candidate=std::shared_ptr<Candidate>(storage.release(),[retained](Candidate* c){ delete c; --*retained; });
        auto& candidate=*job->candidate; candidate.assetID=id.ToString(); candidate.session=session; candidate.contentGeneration=entry->generation;
        candidate.digest=CookedSurface::Identity(*archive,id); candidate.archive=std::move(archive);
        const auto proposed=state.nextTicket++; state.jobs.emplace(proposed,job);
        try { std::lock_guard lock(state.mutex); std::erase_if(state.queue,[](const auto& j){return j->canceled.load();}); state.queue.push_back(job); }
        catch(...) { state.jobs.erase(proposed); throw; }
        ticket=proposed; state.wake.notify_one(); return Status::Pending;
    }
    catch(const std::exception& e) { error=e.what(); return Status::Failed; }
}
ProjectMaterialPreparationService::Result ProjectMaterialPreparationService::Poll(std::uint64_t ticket,const ProjectPaths& paths,const ProjectAssetRegistry& registry,std::uint64_t session)
{ return PollInternal(ticket,&paths,&registry,session); }
ProjectMaterialPreparationService::Result ProjectMaterialPreparationService::PollArchive(std::uint64_t ticket,std::uint64_t session)
{ return PollInternal(ticket,nullptr,nullptr,session); }

ProjectMaterialPreparationService::Result ProjectMaterialPreparationService::PollInternal(std::uint64_t ticket,
    const ProjectPaths* paths, const ProjectAssetRegistry* registry, std::uint64_t session)
{
    auto& state = *m_state;
    const auto found = state.jobs.find(ticket);
    if(found == state.jobs.end()) return {Status::Stale, {}, "Request canceled or invalidated."};
    auto& job = *found->second;
    if(job.candidate->session != session)
    {
        Cancel(ticket);
        return {Status::Stale, {}, "Material preparation session changed."};
    }
    {
        std::lock_guard lock(state.mutex);
        if(!job.done) return {};
    }
    if(job.result.status == Status::Failed || job.result.status == Status::Stale) return job.result;
    auto& candidate = *job.candidate;
    if(!candidate.source) candidate.source = job.source;
    std::string error;
    const auto reject = [&](Status status, std::string message)
    {
        for(auto dependency : job.tickets) if(dependency) state.textures.Cancel(dependency);
        job.source->Revoke();
        job.result = {status, {}, std::move(message)};
        return job.result;
    };
    if(!(candidate.archive ? ValidateArchive(candidate,session,error) : (paths && registry && Validate(candidate,*paths,*registry,session,error)))) return reject(Status::Stale, error);
    if(job.result.status == Status::Ready) return job.result;
    const auto references = References(candidate.parameters);
    for(size_t slot = 0; slot < references.size(); ++slot)
    {
        if(!references[slot] || candidate.textures[slot]) continue;
        const auto id = AssetID::ParseReference(*references[slot]);
        if(!id) return reject(Status::Unsupported, "Texture dependency requires a registered AssetID.");
        if(!job.tickets[slot])
        {
            if(!(candidate.archive ? state.textures.RequestArchive(candidate.archive,*id,Interpretation(slot),state.textures.Generation(),job.tickets[slot],error) :
                state.textures.Request(*paths,*registry,*id,Interpretation(slot),{},state.textures.Generation(),job.tickets[slot],error)))
            {
                if(error.starts_with("Deferred:"))
                {
                    for(auto& dependency : job.tickets)
                    {
                        if(dependency) state.textures.Cancel(dependency);
                        dependency = 0;
                    }
                    candidate.textures = {};
                    return {Status::Retry, {}, error};
                }
                return reject(Status::Failed, error);
            }
        }
        auto completion = candidate.archive ? state.textures.PollArchive(job.tickets[slot]) : state.textures.Poll(job.tickets[slot],*paths,*registry);
        if(!completion) continue;
        if(!completion->prepared) return reject(Status::Failed, completion->error);
        candidate.textures[slot] = std::move(completion);
    }
    for(size_t slot = 0; slot < references.size(); ++slot)
        if(references[slot] && !candidate.textures[slot]) return {};
    if(!(candidate.archive ? ValidateArchive(candidate,session,error) : (paths && registry && Validate(candidate,*paths,*registry,session,error)))) return reject(Status::Stale, error);
    job.result = {Status::Ready, job.candidate, {}};
    return job.result;
}

bool ProjectMaterialPreparationService::Validate(const Candidate& candidate, const ProjectPaths& paths,
    const ProjectAssetRegistry& registry, std::uint64_t session, std::string& error) const
{
    error.clear();
    try
    {
        // Only this service's outstanding request can authorize this exact immutable candidate.
        bool owned = false;
        for(const auto& [ticket, job] : m_state->jobs)
            if(job->candidate.get() == &candidate && !job->canceled) { owned = true; break; }
        const auto id = AssetID::Parse(candidate.assetID);
        const auto* record = id ? registry.Find(*id) : nullptr;
        const auto resolved = id ? ProjectAssetResolver::Resolve(paths, registry, *id) : AssetResolution{};
        if(!owned || candidate.session != session || candidate.project != std::filesystem::canonical(paths.GetRoot()) ||
            !record || record->type != AssetType::Material || resolved.status != AssetAvailability::Available ||
            resolved.resolvedPath != candidate.path || !candidate.source || !candidate.source->Matches(candidate.path))
            throw std::runtime_error("Material candidate source, registration or session is stale.");
        for(const auto& dependency : candidate.textures)
            if(dependency && !m_state->textures.ValidateSource(*dependency, paths, registry, error)) return false;
        return true;
    }
    catch(const std::exception& exception) { error = exception.what(); return false; }
}

void ProjectMaterialPreparationService::Cancel(std::uint64_t ticket)
{
    auto& state = *m_state;
    const auto found = state.jobs.find(ticket);
    if(found == state.jobs.end()) return;
    auto job = found->second;
    job->canceled = true;
    for(auto dependency : job->tickets) if(dependency) state.textures.Cancel(dependency);
    {
        std::lock_guard lock(state.mutex);
        if(job->done && job->source) job->source->Revoke();
    }
    state.jobs.erase(found);
}
void ProjectMaterialPreparationService::Invalidate()
{
    while(!m_state->jobs.empty()) Cancel(m_state->jobs.begin()->first);
}
void ProjectMaterialPreparationService::Shutdown()
{
    if(!m_state || !m_state->worker.joinable()) return;
    Invalidate();
    { std::lock_guard lock(m_state->mutex); m_state->stopping = true; m_state->queue.clear(); }
    m_state->wake.notify_one();
    m_state->worker.join();
}

bool ProjectMaterialPreparationService::ValidateArchive(const Candidate& c,std::uint64_t session,std::string& error) const
{
    try
    {
        bool owned=false; for(const auto& [ticket,job]:m_state->jobs) if(job->candidate.get()==&c && !job->canceled) { owned=true; break; }
        auto id=AssetID::Parse(c.assetID);
        if(!owned || !id || c.session!=session || !c.archive || !c.project.empty() || !c.path.empty() || !c.source || !c.source->valid || c.source->archive!=c.archive ||
            !CookedSurface::Matches(*c.archive,*id,EngineAssetPak::Type::Material,c.contentGeneration,c.digest)) throw std::runtime_error("Archive material snapshot/session/ownership stale.");
        const auto refs=CookedSurface::References(c.parameters);
        for(size_t i=0;i<7;++i)
        {
            if(bool(refs[i])!=bool(c.archiveDependencies[i])) throw std::runtime_error("Incomplete archive material dependency evidence.");
            if(!refs[i]) continue;
            auto texture=AssetID::ParseReference(*refs[i]); auto* entry=texture ? c.archive->ArchiveEntry(*texture) : nullptr;
            if(!entry || entry->type!=EngineAssetPak::Type::Texture || entry->payloadVersion!=CookedSurface::Version || entry->generation!=c.archiveDependencies[i]->generation ||
                entry->sha256!=c.archiveDependencies[i]->sha256) throw std::runtime_error("Missing/stale archive texture dependency.");
            if(c.textures[i] && (c.textures[i]->identity.archive!=c.archive || c.textures[i]->identity.assetID!=texture->ToString() ||
                c.textures[i]->identity.interpretation!=CookedSurface::Interpretation(i) || !m_state->textures.ValidateArchive(*c.textures[i],error))) return false;
        }
        return true;
    }
    catch(const std::exception& e) { error=e.what(); return false; }
}
