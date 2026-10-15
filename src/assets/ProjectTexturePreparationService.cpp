#include "assets/CookedSurface.h"
#include "assets/ProjectTexturePreparationService.h"

#include "Renderer/Preview/MaterialThumbnailValidation.h"
#include <DirectXTex.h>
#include "stb_image.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <utility>
#include <type_traits>
#include <Windows.h>

namespace
{
    std::string Digest(const std::filesystem::path& path)
    {
        MaterialThumbnailValidation validation; // Fresh hasher; never trust timestamps alone.
        return validation.Digest(path);
    }

    struct SourceLock
    {
        HANDLE handle = INVALID_HANDLE_VALUE;

        explicit SourceLock(const std::filesystem::path& path)
        {
            handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);

            if(handle == INVALID_HANDLE_VALUE)
            {
                throw std::runtime_error("Cannot pin texture source against modification.");
            }
        }

        ~SourceLock()
        {
            if(handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(handle);
            }
        }
        SourceLock(const SourceLock&) = delete;
        SourceLock& operator=(const SourceLock&) = delete;
        HANDLE Release() noexcept { return std::exchange(handle, INVALID_HANDLE_VALUE); }
    };

    bool PinnedPathMatches(HANDLE source, const std::filesystem::path& expected)
    {
        std::array<wchar_t, 32768> buffer{};
        const auto length = GetFinalPathNameByHandleW(source, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
        if(!length || length >= buffer.size()) return false;
        std::wstring path(buffer.data(), length);
        if(path.starts_with(L"\\\\?\\UNC\\")) path = L"\\\\" + path.substr(8);
        else if(path.starts_with(L"\\\\?\\")) path.erase(0, 4);
        return ProjectAssetRegistry::PathsEqual(path, expected);
    }

    size_t DDSPreparationBytes(const std::filesystem::path& path, size_t sourceBytes, size_t limit)
    {
        DirectX::TexMetadata metadata{};

        if(FAILED(DirectX::GetMetadataFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, metadata)) ||
            metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || metadata.arraySize != 1 || metadata.depth != 1 ||
            metadata.width == 0 || metadata.height == 0 || metadata.width > 16384 || metadata.height > 16384 ||
            metadata.mipLevels == 0 || metadata.mipLevels > Texture::CalculateMipLevels(
                static_cast<unsigned int>(metadata.width), static_cast<unsigned int>(metadata.height)))
        {
            throw std::runtime_error("Unsupported DDS header or mip layout.");
        }

        switch(metadata.format)
        {
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC5_UNORM:
            break;
        default:
            throw std::runtime_error("Unsupported DDS preparation format; expected BC1, BC4, or BC5 UNORM.");
        }

        size_t mipBytes = 0;
        size_t width = metadata.width;
        size_t height = metadata.height;

        for(size_t mip = 0; mip < metadata.mipLevels; ++mip)
        {
            size_t rowPitch = 0;
            size_t slicePitch = 0;

            if(FAILED(DirectX::ComputePitch(metadata.format, width, height, rowPitch, slicePitch)))
            {
                throw std::runtime_error("Cannot calculate DDS mip storage.");
            }

            mipBytes += slicePitch;
            width = (std::max)(size_t(1), width / 2);
            height = (std::max)(size_t(1), height / 2);
        }

        // DDS stays block-compressed: no expanded RGBA image or generated mip chain.
        // Charge all supplied mips once for ScratchImage and up to three times for
        // prepared-vector capacity plus its old allocation during growth.
        const std::uint64_t required = static_cast<std::uint64_t>(sourceBytes) * 2 +
            static_cast<std::uint64_t>(mipBytes) * 4;

        if(required > limit)
        {
            throw std::runtime_error("DDS preparation requires " + std::to_string(required) +
                " reserved bytes; configured limit is " + std::to_string(limit) + " bytes.");
        }

        return static_cast<size_t>(required);
    }

    // Conservative admission estimate, including source bytes, decoder copies,
    // generated mips and prepared-vector growth. Decoder allocator overhead is additional.
    size_t PreparationBytes(const std::filesystem::path& path, size_t sourceLimit, size_t limit)
    {
        const auto source = std::filesystem::file_size(path);

        if(source > sourceLimit || source > limit / 2)
        {
            throw std::runtime_error("Texture source exceeds preparation budget.");
        }

        auto extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c)
        {
            return static_cast<char>(std::tolower(c));
        });

        if(extension == ".dds")
        {
            return DDSPreparationBytes(path, static_cast<size_t>(source), limit);
        }

        int w = 0;
        int h = 0;
        int channels = 0;

        if(!stbi_info(path.string().c_str(), &w, &h, &channels) || w <= 0 || h <= 0 || w > 16384 || h > 16384)
        {
            throw std::runtime_error("Cannot inspect texture or unsupported dimensions.");
        }

        const size_t width = static_cast<size_t>(w);
        const size_t height = static_cast<size_t>(h);
        const size_t bytesPerPixel = extension == ".hdr" ? 64 : 32;
        // LDR includes the expanded RGBA image, complete generated mip chain,
        // and prepared-vector growth. Keep the existing conservative allowance.
        const std::uint64_t required = source * 2 + static_cast<std::uint64_t>(width) * height * bytesPerPixel;

        if(required > limit)
        {
            throw std::runtime_error("Decoded texture preparation requires " + std::to_string(required) +
                " reserved bytes; configured limit is " + std::to_string(limit) + " bytes.");
        }

        return static_cast<size_t>(required);
    }
}

// This is the existing worker SourceLock with an extended, revocable lifetime.
// No source bytes are accessed after worker preparation. A denied-write/delete
// handle plus file identity keeps the worker's content digest valid at acceptance.
class TextureSourceEvidence
{
public:
    ProjectTexturePreparationService::Identity identity;
    std::uintmax_t bytes = 0;
    std::filesystem::file_time_type modified;
    BY_HANDLE_FILE_INFORMATION file{};
    std::atomic<HANDLE> handle{INVALID_HANDLE_VALUE};
    std::atomic_bool valid{true};
    ~TextureSourceEvidence() { Revoke(); }
    void Revoke() noexcept
    {
        valid = false;
        const auto pinned = handle.exchange(INVALID_HANDLE_VALUE);
        if(pinned != INVALID_HANDLE_VALUE) CloseHandle(pinned);
    }
    bool MatchesPath(const std::filesystem::path& path) const
    {
        if(handle.load() == INVALID_HANDLE_VALUE) return false;
        const auto current = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if(current == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION information{};
        const bool same = GetFileInformationByHandle(current, &information) &&
            !(information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
            file.dwVolumeSerialNumber == information.dwVolumeSerialNumber &&
            file.nFileIndexHigh == information.nFileIndexHigh && file.nFileIndexLow == information.nFileIndexLow;
        CloseHandle(current);
        return same;
    }
};

struct ProjectTexturePreparationService::State
{
    struct Job
    {
        Identity identity;
        bool done = false;
        std::shared_ptr<const PreparedProjectTexture> prepared;
        std::shared_ptr<TextureSourceEvidence> source;
        std::string error;
    };

    struct Subscriber
    {
        std::shared_ptr<Job> job;
        std::uint64_t generation = 0;
        std::shared_ptr<std::atomic_bool> current;
    };

    Limits limits;
    std::shared_ptr<std::atomic_size_t> bytes = std::make_shared<std::atomic_size_t>(0);
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    std::deque<std::shared_ptr<Job>> queue;
    std::map<std::uint64_t, Subscriber> subscribers;
    struct Token
    {
        std::weak_ptr<std::atomic_bool> current;
        std::weak_ptr<TextureSourceEvidence> source;
    };
    std::map<std::uint64_t, Token> tokens;
    struct CacheEntry
    {
        Identity identity;
        std::weak_ptr<const PreparedProjectTexture> prepared;
    };
    std::deque<CacheEntry> cache; // Weak CPU cache, bounded by request capacity; never pins source files.
    std::atomic_uint64_t hashes{0}, decodes{0}, cacheHits{0}, deduplicated{0};
#ifdef ENGINE_TEXTURE_PREPARATION_TESTS
    std::function<void(TestStage)> testHook;
    void Observe(TestStage stage)
    {
        std::function<void(TestStage)> hook;
        { std::lock_guard lock(mutex); hook = testHook; }
        if(hook) hook(stage);
    }
#endif
    std::uint64_t generation = 1;
    std::uint64_t nextTicket = 1;
    bool accepting = false;
    bool stop = false;

    void RevokeIfUnused(const std::shared_ptr<TextureSourceEvidence>& source)
    {
        if(!source) return;
        const bool wanted = std::any_of(tokens.begin(), tokens.end(), [&](const auto& entry)
        {
            const auto token = entry.second.current.lock();
            return token && token->load() && entry.second.source.lock() == source;
        });
        if(!wanted) source->Revoke();
    }

    void PruneTokens()
    {
        for(auto entry = tokens.begin(); entry != tokens.end();)
        {
            if(entry->second.current.expired())
            {
                const auto source = entry->second.source.lock();
                entry = tokens.erase(entry);
                RevokeIfUnused(source);
            }
            else ++entry;
        }
    }

    bool Wanted(const std::shared_ptr<Job>& job) const
    {
        return std::any_of(subscribers.begin(), subscribers.end(), [&](const auto& entry)
        {
            return entry.second.job == job;
        });
    }

    size_t Completed() const
    {
        size_t completed = 0;
        for(auto entry = subscribers.begin(); entry != subscribers.end(); ++entry)
        {
            if(entry->second.job->done && std::none_of(subscribers.begin(), entry, [&](const auto& previous)
                { return previous.second.job == entry->second.job; })) ++completed;
        }
        return completed;
    }

    void Run()
    {
        for(;;)
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return stop || (!queue.empty() && Completed() < limits.completed); });

            if(stop)
            {
                return;
            }

            auto job = queue.front();
            queue.pop_front();

            if(!Wanted(job))
            {
                continue;
            }

            lock.unlock();
            std::shared_ptr<const PreparedProjectTexture> prepared;
            std::shared_ptr<TextureSourceEvidence> evidence;
            Identity completedIdentity;
            std::string error;
            size_t reservation = 0;

            try
            {
                if(job->identity.archive)
                {
                    const auto& identity = job->identity;
                    const auto id = *AssetID::Parse(identity.assetID);
                    const auto* entry = identity.archive->ArchiveEntry(id);
                    if(!entry || entry->storedBytes > limits.archiveBytes || entry->storedBytes > limits.preparationBytes / 2)
                        throw std::runtime_error("Archive texture exceeds worker reservation.");
                    reservation = static_cast<size_t>(entry->storedBytes) * 2;
                    const auto previous = bytes->fetch_add(reservation);
                    if(previous > limits.retainedBytes - reservation)
                    { bytes->fetch_sub(reservation); reservation = 0; throw std::runtime_error("Deferred: retained texture memory exhausted."); }
                    auto raw = identity.archive->ReadEntry(id,EngineAssetPak::Type::Texture,CookedSurface::Version,limits.archiveBytes);
                    auto image = std::make_unique<PreparedProjectTexture>(CookedSurface::DecodeTexture(raw,id,identity.contentGeneration,identity.interpretation));
                    ++decodes;
                    const auto retained = reservation; auto accounting = bytes; reservation = 0;
                    prepared = std::shared_ptr<const PreparedProjectTexture>(image.release(),[accounting,retained](const PreparedProjectTexture* p){ delete p; accounting->fetch_sub(retained); });
                    evidence = std::make_shared<TextureSourceEvidence>(); evidence->identity = identity; evidence->bytes = entry->storedBytes;
                    completedIdentity = identity;
                }
                else
                {
                SourceLock source(job->identity.path);
                auto identity = job->identity;
                evidence = std::make_shared<TextureSourceEvidence>();
                if(!GetFileInformationByHandle(source.handle, &evidence->file) ||
                    (evidence->file.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                    !(evidence->file.nFileIndexHigh || evidence->file.nFileIndexLow) ||
                    !PinnedPathMatches(source.handle, identity.path))
                    throw std::runtime_error("Cannot identify a regular pinned texture source.");
                evidence->bytes = std::filesystem::file_size(identity.path);
                evidence->modified = std::filesystem::last_write_time(identity.path);
                if(evidence->bytes > limits.sourceBytes)
                    throw std::runtime_error("Texture source exceeds preparation budget.");
#ifdef ENGINE_TEXTURE_PREPARATION_TESTS
                Observe(TestStage::Pinned);
#endif
                ++hashes;
                identity.digest = Digest(identity.path);
                {
                    std::lock_guard cacheLock(mutex);
                    if(stop || !Wanted(job)) throw std::runtime_error("Texture preparation canceled.");
                    for(const auto& cached : cache)
                        if(cached.identity == identity && (prepared = cached.prepared.lock()))
                        { ++cacheHits; break; }
                }
#ifdef ENGINE_TEXTURE_PREPARATION_TESTS
                Observe(TestStage::Hashed);
#endif
                if(!prepared)
                {
                    reservation = PreparationBytes(identity.path, limits.sourceBytes, limits.preparationBytes);
                    const auto previous = bytes->fetch_add(reservation);
                    if(previous > limits.retainedBytes - reservation)
                    {
                        bytes->fetch_sub(reservation);
                        reservation = 0;
                        throw std::runtime_error("Deferred: consumer-held texture memory exhausts budget; retry later.");
                    }
                    auto image = std::make_unique<PreparedProjectTexture>();
                    ++decodes;
                    if(!PrepareProjectTexture(job->identity.path, job->identity.interpretation, *image, &error))
                        throw std::runtime_error(error);
                    if(image->pixels.capacity() > reservation)
                        throw std::runtime_error("Prepared data exceeds reservation.");
                    const auto retained = reservation;
                    auto accounting = bytes;
                    reservation = 0; // The deleter owns accounting even if shared-pointer allocation throws.
                    prepared = std::shared_ptr<const PreparedProjectTexture>(image.release(),
                        [accounting, retained](const PreparedProjectTexture* value)
                        {
                            delete value;
                            accounting->fetch_sub(retained);
                        });
                    reservation = 0;
                }
                ++hashes;
                if(Digest(identity.path) != identity.digest)
                    throw std::runtime_error("Texture content changed during preparation.");
                evidence->identity = std::move(identity);
                completedIdentity = evidence->identity; // Fallible copy before the completion critical section.
                evidence->handle = source.Release();
#ifdef ENGINE_TEXTURE_PREPARATION_TESTS
                Observe(TestStage::Prepared);
#endif
                }
            }
            catch(const std::exception& exception)
            {
                error = exception.what();
                prepared.reset();
                evidence.reset();
            }

            if(reservation)
            {
                bytes->fetch_sub(reservation);
            }

            lock.lock();
            // Reserve a completed slot even when subscribers deduplicate the same job.
            wake.wait(lock, [&] { return stop || !Wanted(job) || Completed() < limits.completed; });

            if(!stop && Wanted(job))
            {
                if(prepared && evidence)
                {
                    static_assert(std::is_nothrow_move_assignable_v<Identity>);
                    job->identity = std::move(completedIdentity);
                    job->source = std::move(evidence);
                    for(const auto& [ticket, subscriber] : subscribers)
                        if(subscriber.job == job) tokens.at(ticket).source = job->source;
                    // Caching is optional; allocation failure cannot discard a valid completion.
                    try
                    {
                        std::erase_if(cache, [](const auto& cached) { return cached.prepared.expired(); });
                        if(cache.size() >= limits.requests) cache.pop_front();
                        // Archive jobs deduplicate while active; avoid a weak-data
                        // cache entry retaining the source archive after consumers close.
                        if(!job->identity.archive) cache.push_back({job->identity, prepared});
                    }
                    catch(const std::bad_alloc&) {}
                }
                job->prepared = std::move(prepared);
                job->error = std::move(error);
                job->done = true;
            }
        }
    }
};

ProjectTexturePreparationService::ProjectTexturePreparationService() : m_state(std::make_unique<State>()) {}
ProjectTexturePreparationService::~ProjectTexturePreparationService() { Shutdown(); }

bool ProjectTexturePreparationService::Start(const Limits& limits, std::string& error)
{
    error.clear();
    auto& state = *m_state;

    if(state.worker.joinable() || !limits.requests || !limits.completed || !limits.sourceBytes ||
        !limits.preparationBytes || limits.retainedBytes < limits.preparationBytes)
    {
        error = "Invalid texture service limits or service already started.";
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
    catch(const std::exception& exception)
    {
        state.accepting = false;
        error = exception.what();
        return false;
    }
}

void ProjectTexturePreparationService::Shutdown()
{
    auto& state = *m_state;

    {
        std::lock_guard lock(state.mutex);
        state.accepting = false;
        state.stop = true;
        ++state.generation;
        for(const auto& [ticket, token] : state.tokens)
        {
            if(auto current = token.current.lock())
            {
                current->store(false);
            }
            if(auto source = token.source.lock()) source->Revoke();
        }

        state.tokens.clear();
        state.subscribers.clear();
        state.queue.clear();
        state.cache.clear();
    }

    state.wake.notify_all();

    if(state.worker.joinable())
    {
        state.worker.join();
    }
}

std::uint64_t ProjectTexturePreparationService::Generation() const { return m_state->generation; }
size_t ProjectTexturePreparationService::RetainedBytes() const { return m_state->bytes->load(); }
bool ProjectTexturePreparationService::IsCurrent(const Completion& completion) const
{
    std::lock_guard lock(m_state->mutex);
    const auto token = m_state->tokens.find(completion.ticket);
    return m_state->accepting && completion.generation == Generation() &&
        completion.current && completion.current->load() && token != m_state->tokens.end() &&
        token->second.current.lock() == completion.current && token->second.source.lock() == completion.source &&
        completion.source && completion.source->valid.load() &&
        (completion.identity.archive || completion.source->handle.load() != INVALID_HANDLE_VALUE);
}

ProjectTexturePreparationService::Statistics ProjectTexturePreparationService::GetStatistics() const
{
    return {m_state->hashes.load(), m_state->decodes.load(), m_state->cacheHits.load(), m_state->deduplicated.load()};
}
#ifdef ENGINE_TEXTURE_PREPARATION_TESTS
void ProjectTexturePreparationService::SetTestHook(std::function<void(TestStage)> hook)
{
    std::lock_guard lock(m_state->mutex);
    m_state->testHook = std::move(hook);
}
#endif

void ProjectTexturePreparationService::Invalidate()
{
    std::lock_guard lock(m_state->mutex);
    ++m_state->generation;
    for(const auto& [ticket, token] : m_state->tokens)
    {
        if(auto current = token.current.lock())
        {
            current->store(false);
        }
        if(auto source = token.source.lock()) source->Revoke();
    }

    m_state->tokens.clear();
    m_state->subscribers.clear();
    m_state->queue.clear();
    m_state->wake.notify_all();
}

void ProjectTexturePreparationService::Cancel(std::uint64_t ticket)
{
    std::lock_guard lock(m_state->mutex);
    const auto found = m_state->tokens.find(ticket);

    if(found != m_state->tokens.end())
    {
        const auto source = found->second.source.lock();
        if(auto token = found->second.current.lock())
        {
            token->store(false);
        }

        m_state->tokens.erase(found);
        m_state->RevokeIfUnused(source);
    }

    m_state->subscribers.erase(ticket);
    std::erase_if(m_state->queue, [&](const auto& job) { return !m_state->Wanted(job); });
    m_state->wake.notify_all();
}

bool ProjectTexturePreparationService::Request(const ProjectPaths& paths, const ProjectAssetRegistry& registry,
    const AssetID& asset, ProjectTextureInterpretation interpretation, Options options,
    std::uint64_t generation, std::uint64_t& ticket, std::string& error)
{
    if(m_archiveOnly) { ticket=0; error="Loose texture access rejected by packaged policy."; return false; }
    ticket = 0;
    error.clear();
    auto& state = *m_state;

    try
    {
        const auto* record = registry.Find(asset);
        const auto resolved = ProjectAssetResolver::Resolve(paths, registry, asset);

        if(!record || record->type != AssetType::Texture || resolved.status != AssetAvailability::Available ||
            options.policyRevision != 1 || generation != state.generation || !state.accepting ||
            interpretation < ProjectTextureInterpretation::SRGBColor || interpretation > ProjectTextureInterpretation::NormalMap)
        {
            throw std::runtime_error("Invalid texture request, policy, subscriber generation or project source.");
        }

        const auto sourceBytes = std::filesystem::file_size(resolved.resolvedPath);

        if(sourceBytes > state.limits.sourceBytes)
        {
            throw std::runtime_error("Texture source is " + std::to_string(sourceBytes) +
                " bytes; configured source limit is " + std::to_string(state.limits.sourceBytes) + " bytes.");
        }

        Identity identity{std::filesystem::canonical(paths.GetRoot()), resolved.resolvedPath,
            asset.ToString(), {}, interpretation, options};
        return RequestIdentity(std::move(identity),generation,ticket,error);
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
        ticket = 0;
        return false;
    }
}

bool ProjectTexturePreparationService::RequestIdentity(Identity identity,std::uint64_t generation,std::uint64_t& ticket,std::string& error)
{
    auto& state = *m_state;
    try
    {
        std::lock_guard lock(state.mutex);

        state.PruneTokens();

        if(state.tokens.size() >= state.limits.requests || state.nextTicket == UINT64_MAX)
        {
            throw std::runtime_error("Deferred: texture request capacity exhausted.");
        }

        std::shared_ptr<State::Job> job;

        for(const auto& [existing, subscriber] : state.subscribers)
        {
            const auto& existingIdentity = subscriber.job->identity;
            if(existingIdentity.project == identity.project && existingIdentity.path == identity.path &&
                existingIdentity.assetID == identity.assetID && existingIdentity.interpretation == identity.interpretation &&
                existingIdentity.options == identity.options && existingIdentity.archive == identity.archive &&
                existingIdentity.contentGeneration == identity.contentGeneration && (!subscriber.job->done ||
                    (subscriber.job->prepared && subscriber.job->source &&
                        (identity.archive ? subscriber.job->source->valid.load() : subscriber.job->source->MatchesPath(identity.path)))))
            {
                job = subscriber.job;
                ++state.deduplicated;
                break;
            }
        }

        if(!job)
        {
            job = std::make_shared<State::Job>();
            job->identity = std::move(identity);
            state.queue.push_back(job);
        }

        ticket = state.nextTicket++;
        auto current = std::make_shared<std::atomic_bool>(true);
        state.tokens.emplace(ticket, State::Token{current, job->source});
        try { state.subscribers.emplace(ticket, State::Subscriber{job, generation, current}); }
        catch(...)
        {
            state.tokens.erase(ticket);
            state.RevokeIfUnused(job->source);
            throw;
        }
        state.wake.notify_one();
        return true;
    }
    catch(const std::exception& e) { ticket=0; error=e.what(); return false; }
}
bool ProjectTexturePreparationService::RequestArchive(std::shared_ptr<const CookedMeshSource::ArchiveProvider> archive,const AssetID& id,
    ProjectTextureInterpretation interpretation,std::uint64_t generation,std::uint64_t& ticket,std::string& error)
{
    ticket=0;
    try
    {
        const auto* entry = archive ? archive->ArchiveEntry(id) : nullptr;
        if(!m_state->accepting || generation!=Generation() || !entry || entry->type!=EngineAssetPak::Type::Texture || entry->payloadVersion!=CookedSurface::Version ||
            entry->storedBytes>m_state->limits.archiveBytes || interpretation<ProjectTextureInterpretation::SRGBColor || interpretation>ProjectTextureInterpretation::NormalMap)
            throw std::runtime_error("Missing/unsupported archive texture or stale request generation.");
        Identity identity; identity.assetID=id.ToString(); identity.digest=CookedSurface::Identity(*archive,id);
        identity.interpretation=interpretation; identity.contentGeneration=entry->generation; identity.archive=std::move(archive);
        return RequestIdentity(std::move(identity),generation,ticket,error);
    }
    catch(const std::exception& e) { error=e.what(); return false; }
}
std::optional<ProjectTexturePreparationService::Completion> ProjectTexturePreparationService::Poll(std::uint64_t ticket,const ProjectPaths& paths,const ProjectAssetRegistry& registry)
{ return PollInternal(ticket,&paths,&registry); }
std::optional<ProjectTexturePreparationService::Completion> ProjectTexturePreparationService::PollArchive(std::uint64_t ticket)
{ return PollInternal(ticket,nullptr,nullptr); }

std::optional<ProjectTexturePreparationService::Completion> ProjectTexturePreparationService::PollInternal(
    std::uint64_t ticket, const ProjectPaths* paths, const ProjectAssetRegistry* registry)
{
    Completion completion;

    {
        std::lock_guard lock(m_state->mutex);
        const auto found = m_state->subscribers.find(ticket);

        if(found == m_state->subscribers.end() || !found->second.job->done)
        {
            return std::nullopt;
        }

        const auto& subscriber = found->second;
        completion = {subscriber.job->identity, ticket, subscriber.generation,
            subscriber.job->prepared, subscriber.job->error, subscriber.current, subscriber.job->source};
        if(completion.source)
        {
            completion.sourceBytes = completion.source->bytes;
            completion.sourceModified = completion.source->modified;
        }
        m_state->subscribers.erase(found);
    }

    m_state->wake.notify_one();

    if(completion.prepared && !(completion.identity.archive ? ValidateArchive(completion,completion.error) :
        (paths && registry && ValidateSource(completion,*paths,*registry,completion.error))))
    {
        completion.prepared.reset();
        Cancel(ticket);
    }
    return completion;
}

bool ProjectTexturePreparationService::ValidateSource(const Completion& completion,
    const ProjectPaths& paths, const ProjectAssetRegistry& registry, std::string& error) const
{
    error.clear();
    try
    {
        const auto asset = AssetID::Parse(completion.identity.assetID);
        const auto* record = asset ? registry.Find(*asset) : nullptr;
        const auto resolved = asset ? ProjectAssetResolver::Resolve(paths, registry, *asset) : AssetResolution{};

        if(!IsCurrent(completion) || !record || record->type != AssetType::Texture ||
            std::filesystem::canonical(paths.GetRoot()) != completion.identity.project ||
            resolved.status != AssetAvailability::Available || resolved.resolvedPath != completion.identity.path)
        {
            throw std::runtime_error("Texture completion subscriber or safe project source is stale.");
        }

        if(!completion.source || completion.identity != completion.source->identity ||
            completion.sourceBytes != completion.source->bytes || completion.sourceModified != completion.source->modified ||
            !completion.source->MatchesPath(resolved.resolvedPath))
        {
            throw std::runtime_error("Texture completion source content is stale.");
        }
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
    return true;
}

bool ProjectTexturePreparationService::ValidateArchive(const Completion& c,std::string& error) const
{
    try
    {
        const auto id=AssetID::Parse(c.identity.assetID);
        if(!id || !IsCurrent(c) || !c.identity.archive || !c.identity.path.empty() || !c.identity.project.empty() ||
            c.source->identity!=c.identity || !CookedSurface::Matches(*c.identity.archive,*id,EngineAssetPak::Type::Texture,c.identity.contentGeneration,c.identity.digest))
            throw std::runtime_error("Archive texture snapshot/generation/ownership is stale.");
        return true;
    }
    catch(const std::exception& e) { error=e.what(); return false; }
}

size_t ProjectTexturePreparationService::EstimateForCooking(const std::filesystem::path& path)
{ return PreparationBytes(path,Limits{}.sourceBytes,Limits{}.preparationBytes); }
