#include "assets/ProjectTexturePreparationService.h"

#include "Renderer/Preview/MaterialThumbnailValidation.h"
#include <DirectXTex.h>
#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <stdexcept>
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
                OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

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
    };

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

struct ProjectTexturePreparationService::State
{
    struct Job
    {
        Identity identity;
        bool done = false;
        std::shared_ptr<const PreparedProjectTexture> prepared;
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
    std::map<std::uint64_t, std::weak_ptr<std::atomic_bool>> tokens;
    std::uint64_t generation = 1;
    std::uint64_t nextTicket = 1;
    bool accepting = false;
    bool stop = false;

    bool Wanted(const std::shared_ptr<Job>& job) const
    {
        return std::any_of(subscribers.begin(), subscribers.end(), [&](const auto& entry)
        {
            return entry.second.job == job;
        });
    }

    size_t Completed() const
    {
        std::set<const Job*> completed;

        for(const auto& [ticket, subscriber] : subscribers)
        {
            if(subscriber.job->done)
            {
                completed.insert(subscriber.job.get());
            }
        }

        return completed.size();
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
            std::string error;
            size_t reservation = 0;

            try
            {
                SourceLock source(job->identity.path);

                if(Digest(job->identity.path) != job->identity.digest)
                {
                    throw std::runtime_error("Texture source changed before preparation.");
                }

                reservation = PreparationBytes(job->identity.path, limits.sourceBytes, limits.preparationBytes);
                const auto previous = bytes->fetch_add(reservation);

                if(previous > limits.retainedBytes - reservation)
                {
                    bytes->fetch_sub(reservation);
                    reservation = 0;
                    throw std::runtime_error("Deferred: consumer-held texture memory exhausts budget; retry later.");
                }

                auto image = std::make_unique<PreparedProjectTexture>();

                if(!PrepareProjectTexture(job->identity.path, job->identity.interpretation, *image, &error))
                {
                    throw std::runtime_error(error);
                }

                if(Digest(job->identity.path) != job->identity.digest || image->pixels.capacity() > reservation)
                {
                    throw std::runtime_error("Texture source changed or prepared data exceeds reservation.");
                }

                const auto retained = reservation;
                auto accounting = bytes;
                reservation = 0; // The shared-pointer deleter owns accounting even if allocation throws.
                prepared = std::shared_ptr<const PreparedProjectTexture>(image.release(),
                    [accounting, retained](const PreparedProjectTexture* value)
                    {
                        delete value;
                        accounting->fetch_sub(retained);
                    });
                reservation = 0;
            }
            catch(const std::exception& exception)
            {
                error = exception.what();
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
        for(const auto& [ticket, weak] : state.tokens)
        {
            if(auto token = weak.lock())
            {
                token->store(false);
            }
        }

        state.tokens.clear();
        state.subscribers.clear();
        state.queue.clear();
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
    return m_state->accepting && completion.generation == Generation() &&
        completion.current && completion.current->load();
}

void ProjectTexturePreparationService::Invalidate()
{
    std::lock_guard lock(m_state->mutex);
    ++m_state->generation;
    for(const auto& [ticket, weak] : m_state->tokens)
    {
        if(auto token = weak.lock())
        {
            token->store(false);
        }
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
        if(auto token = found->second.lock())
        {
            token->store(false);
        }

        m_state->tokens.erase(found);
    }

    m_state->subscribers.erase(ticket);
    std::erase_if(m_state->queue, [&](const auto& job) { return !m_state->Wanted(job); });
    m_state->wake.notify_all();
}

bool ProjectTexturePreparationService::Request(const ProjectPaths& paths, const ProjectAssetRegistry& registry,
    const AssetID& asset, ProjectTextureInterpretation interpretation, Options options,
    std::uint64_t generation, std::uint64_t& ticket, std::string& error)
{
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

        SourceLock source(resolved.resolvedPath);

        const auto sourceBytes = std::filesystem::file_size(resolved.resolvedPath);

        if(sourceBytes > state.limits.sourceBytes)
        {
            throw std::runtime_error("Texture source is " + std::to_string(sourceBytes) +
                " bytes; configured source limit is " + std::to_string(state.limits.sourceBytes) + " bytes.");
        }

        Identity identity{std::filesystem::canonical(paths.GetRoot()), resolved.resolvedPath,
            asset.ToString(), Digest(resolved.resolvedPath), interpretation, options};
        std::lock_guard lock(state.mutex);

        std::erase_if(state.tokens, [](const auto& entry) { return entry.second.expired(); });

        if(state.tokens.size() >= state.limits.requests || state.nextTicket == UINT64_MAX)
        {
            throw std::runtime_error("Deferred: texture request capacity exhausted.");
        }

        std::shared_ptr<State::Job> job;

        for(const auto& [existing, subscriber] : state.subscribers)
        {
            if(!subscriber.job->done && subscriber.job->identity == identity)
            {
                job = subscriber.job;
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
        state.subscribers.emplace(ticket, State::Subscriber{job, generation, current});
        state.tokens.emplace(ticket, current);
        state.wake.notify_one();
        return true;
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

std::optional<ProjectTexturePreparationService::Completion> ProjectTexturePreparationService::Poll(
    std::uint64_t ticket, const ProjectPaths& paths, const ProjectAssetRegistry& registry)
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
            subscriber.job->prepared, subscriber.job->error, subscriber.current};
        m_state->subscribers.erase(found);
    }

    m_state->wake.notify_one();

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

        SourceLock source(resolved.resolvedPath);

        if(Digest(resolved.resolvedPath) != completion.identity.digest)
        {
            throw std::runtime_error("Texture completion source content is stale.");
        }
    }
    catch(const std::exception& exception)
    {
        completion.prepared.reset();
        completion.error = exception.what();
    }

    return completion;
}
