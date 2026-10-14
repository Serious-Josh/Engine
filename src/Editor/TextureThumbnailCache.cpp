#include "TextureThumbnailCache.h"
#include "Renderer/ImageLoader.h"
#include "Renderer/DDSLoader.h"
#include "Renderer/DescriptorHeap.h"
#include "Project/ProjectPaths.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace
{
    // CPU-only helper, used exclusively by the bounded preparation worker.
    static TextureThumbnailDiskCache::Image PreparePixels(const TextureThumbnailDiskCache::Source& diskSource)
    {
        TextureThumbnailDiskCache::Image pixels;

        if(TextureThumbnailDiskCache::Load(diskSource, pixels))
        {
            return pixels;
        }

        std::string extension = diskSource.path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value)
        {
            return static_cast<char>(std::tolower(value));
        });
        DirectX::ScratchImage decoded;
        ImageData standard;
        DirectX::Image source{};

        if(extension == ".dds")
        {
            DirectX::ScratchImage compressed;

            if(!DDSLoader::Load(diskSource.path, compressed))
            {
                throw std::runtime_error("Cannot decode DDS thumbnail.");
            }

            const auto format = compressed.GetMetadata().format;

            if(format != DXGI_FORMAT_BC1_UNORM && format != DXGI_FORMAT_BC1_UNORM_SRGB &&
               format != DXGI_FORMAT_BC4_UNORM && format != DXGI_FORMAT_BC5_UNORM)
            {
                throw std::runtime_error("Unsupported DDS thumbnail format.");
            }

            if(FAILED(DirectX::Decompress(*compressed.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM, decoded)))
            {
                throw std::runtime_error("Cannot decompress DDS thumbnail.");
            }

            source = *decoded.GetImage(0, 0, 0);
        }
        else if(extension == ".hdr")
        {
            HDRImageData hdr;

            if(!ImageLoader::LoadHDR(diskSource.path, hdr))
            {
                throw std::runtime_error("Cannot decode HDR thumbnail.");
            }

            standard.width = hdr.width;
            standard.height = hdr.height;
            standard.pixels.resize(static_cast<size_t>(hdr.width) * hdr.height * 4);

            for(size_t index = 0; index < standard.pixels.size(); ++index)
            {
                float value = hdr.pixels[index];
                value = std::isfinite(value) ? (std::max)(0.0f, value) : 0.0f;
                standard.pixels[index] = index % 4 == 3 ? 255 :
                    static_cast<unsigned char>(255.0f * std::pow(value / (1.0f + value), 1.0f / 2.2f));
            }
        }
        else if(!ImageLoader::Load(diskSource.path, standard))
        {
            throw std::runtime_error("Cannot decode image thumbnail.");
        }

        if(extension != ".dds")
        {
            source = {standard.width, standard.height, DXGI_FORMAT_R8G8B8A8_UNORM,
                static_cast<size_t>(standard.width) * 4, standard.pixels.size(), standard.pixels.data()};
        }

        const double scale = (std::min)(1.0, 96.0 / static_cast<double>((std::max)(source.width, source.height)));
        pixels.width = static_cast<unsigned int>((std::max)(1.0, std::round(source.width * scale)));
        pixels.height = static_cast<unsigned int>((std::max)(1.0, std::round(source.height * scale)));
        DirectX::ScratchImage resized;

        if(FAILED(DirectX::Resize(source, pixels.width, pixels.height, DirectX::TEX_FILTER_DEFAULT, resized)))
        {
            throw std::runtime_error("Cannot resize thumbnail.");
        }

        const auto* image = resized.GetImage(0, 0, 0);
        pixels.pixels.resize(static_cast<size_t>(pixels.width) * pixels.height * 4);

        for(size_t row = 0; row < pixels.height; ++row)
        {
            memcpy(pixels.pixels.data() + row * pixels.width * 4,
                image->pixels + row * image->rowPitch, static_cast<size_t>(pixels.width) * 4);
        }

        TextureThumbnailDiskCache::Store(diskSource, pixels);
        return pixels;
    }
}

struct TextureThumbnailCache::Worker
{
    struct Job
    {
        ProjectPaths paths;
        AssetRecord record;
        std::string identity;
        std::shared_ptr<std::atomic_bool> cancelled;
        Ticket ticket = 0;
        std::uint64_t epoch = 0;
    };
    struct Result
    {
        TextureThumbnailDiskCache::Image pixels;
        std::string identity;
        std::string error;
        Ticket ticket = 0;
        std::uint64_t epoch = 0;
    };

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    std::optional<Result> result;
    bool stop = false;

    static bool Current(const TextureThumbnailDiskCache::Source& source)
    {
        return !source.identity.empty() && std::filesystem::file_size(source.path) == source.size &&
            std::filesystem::last_write_time(source.path) == source.modified;
    }

    void Run()
    {
        for(;;)
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [this] { return stop || (!jobs.empty() && !result); });

            if(stop)
            {
                return;
            }

            auto job = std::move(jobs.front());
            jobs.pop_front();
            lock.unlock();

            if(job.cancelled->load())
            {
                continue;
            }

            Result prepared;
            prepared.ticket = job.ticket;
            prepared.epoch = job.epoch;

            try
            {
                ProjectAssetRegistry registry;
                registry.Register(job.record);
                const auto resolved = ProjectAssetResolver::Resolve(job.paths, registry, job.record.id);

                if(job.record.type != AssetType::Texture || resolved.status != AssetAvailability::Available)
                {
                    throw std::runtime_error("Texture unavailable. Refresh after restoring the file.");
                }

                const auto source = TextureThumbnailDiskCache::Describe(job.paths, job.record, resolved.resolvedPath);

                if(!Current(source))
                {
                    throw std::runtime_error("Texture source revision unavailable.");
                }

                prepared.identity = source.identity;

                if(prepared.identity != job.identity)
                {
                    prepared.pixels = PreparePixels(source);
                }

                if(!Current(source))
                {
                    throw std::runtime_error("Texture changed during thumbnail preparation.");
                }
            }
            catch(const std::exception& exception)
            {
                prepared.pixels = {};
                prepared.error = exception.what();
            }

            lock.lock();

            if(!stop && !job.cancelled->load())
            {
                result = std::move(prepared);
            }
        }
    }
};

TextureThumbnailCache::TextureThumbnailCache() : m_worker(std::make_shared<Worker>())
{
    // Detached CPU ownership lets cancellation/clear return without waiting for disk or decode.
    // The worker's shared state outlives the cache and contains no device or descriptor pointers.
    std::thread([worker = m_worker] { worker->Run(); }).detach();
}

TextureThumbnailCache::~TextureThumbnailCache()
{
    std::lock_guard lock(m_worker->mutex);
    m_worker->stop = true;
    m_worker->jobs.clear();
    m_worker->wake.notify_one();
}

void TextureThumbnailCache::Configure(ID3D12Device* device, DescriptorHeap& heap)
{
    m_device = device;
    m_heap = &heap;
}

bool TextureThumbnailCache::Retirable(const Entry& entry) const
{
    return !m_quarantined && !entry.uploadGeneration && !entry.displayGeneration &&
        !entry.preview.awaitingSubmission && entry.uploadFence <= m_completedFence &&
        entry.preview.lastSubmissionFence <= m_completedFence;
}

void TextureThumbnailCache::Poll()
{
    std::optional<Worker::Result> result;

    {
        std::lock_guard lock(m_worker->mutex);
        result.swap(m_worker->result);
        m_worker->wake.notify_one();
    }

    if(!result)
    {
        return;
    }

    for(auto& [key, entry] : m_entries)
    {
        if(entry.ticket != result->ticket || entry.epoch != result->epoch)
        {
            continue;
        }

        entry.preparing = false;
        entry.nextValidation = std::chrono::steady_clock::now() + std::chrono::seconds(1);

        if(!entry.consumers || m_quarantined)
        {
            return;
        }

        if(!result->error.empty())
        {
            entry.preview.error = std::move(result->error);
            entry.ready = false;
            entry.preview.invalidated = true;
            return;
        }

        if(entry.identity == result->identity && !entry.preview.invalidated)
        {
            if(entry.needsValidation && entry.uploadFence && entry.uploadFence <= m_completedFence)
            {
                entry.ready = true;
                entry.needsValidation = false;
            }

            return;
        }

        entry.ready = false;
        entry.preview.invalidated = true;

        entry.identity = std::move(result->identity);
        entry.pixels = std::move(result->pixels);
        entry.preview.error.clear();
        return;
    }
}

bool TextureThumbnailCache::Queue(Entry& entry)
{
    std::lock_guard lock(m_worker->mutex);

    if(entry.preparing || m_quarantined || m_worker->jobs.size() >= kQueueCapacity)
    {
        return false;
    }

    entry.cancelled = std::make_shared<std::atomic_bool>(false);
    ++entry.epoch;

    m_worker->jobs.push_back({entry.project, entry.record,
        entry.preview.invalidated ? std::string{} : entry.identity,
        entry.cancelled, entry.ticket, entry.epoch});
    entry.preparing = true;
    m_worker->wake.notify_one();
    return true;
}

TextureThumbnailCache::Ticket TextureThumbnailCache::Request(const ProjectPaths& paths,
    const ProjectAssetRegistry& registry, const AssetRecord& record, Consumer consumer)
{
    Poll();
    const auto* registered = registry.Find(record.id);

    if(!registered || registered->type != AssetType::Texture || record.type != registered->type ||
        !ProjectAssetRegistry::PathsEqual(registered->relativePath, record.relativePath))
    {
        return 0;
    }

    const Key key{paths.GetRoot().lexically_normal(),
        record.id.ToReference() + ":" + record.relativePath.lexically_normal().generic_string()};
    auto found = m_entries.find(key);

    if(found == m_entries.end())
    {
        if(m_entries.size() >= kCapacity)
        {
            auto victim = m_entries.end();

            for(auto candidate = m_entries.begin(); candidate != m_entries.end(); ++candidate)
            {
                if(candidate->second.consumers || !Retirable(candidate->second))
                {
                    continue;
                }

                if(victim == m_entries.end() || candidate->second.preview.lastUse < victim->second.preview.lastUse)
                {
                    victim = candidate;
                }
            }

            if(victim == m_entries.end())
            {
                return 0;
            }

            if(victim->second.cancelled)
            {
                victim->second.cancelled->store(true);
            }

            victim->second.preview.texture.Shutdown();
            m_entries.erase(victim);
        }

        found = m_entries.try_emplace(key, paths, record).first;
        found->second.ticket = ++m_nextTicket;
        found->second.preview.sourcePath = (paths.GetRoot() / record.relativePath).lexically_normal();
    }

    auto& entry = found->second;
    entry.consumers |= static_cast<unsigned int>(consumer);
    entry.browserUsed |= consumer == Consumer::AssetBrowser;
    entry.preview.lastUse = ++m_useSerial;

    if(!entry.preparing && !entry.uploadGeneration && entry.uploadFence <= m_completedFence &&
        entry.pixels.pixels.empty() && std::chrono::steady_clock::now() >= entry.nextValidation)
    {
        Queue(entry);
    }

    return entry.ticket;
}

const TextureThumbnailCache::Preview* TextureThumbnailCache::Query(Ticket ticket)
{
    for(auto& [key, entry] : m_entries)
    {
        if(entry.ticket != ticket || !entry.consumers || m_quarantined)
        {
            continue;
        }

        if(!entry.ready || entry.preview.invalidated)
        {
            return entry.preview.error.empty() ? nullptr : &entry.preview;
        }

        entry.preview.awaitingSubmission = true;
        return &entry.preview;
    }

    return nullptr;
}

void TextureThumbnailCache::Release(Ticket ticket, Consumer consumer)
{
    for(auto& [key, entry] : m_entries)
    {
        if(entry.ticket != ticket)
        {
            continue;
        }

        entry.consumers &= ~static_cast<unsigned int>(consumer);

        if(!entry.consumers)
        {
            if(entry.cancelled)
            {
                entry.cancelled->store(true);
            }

            ++entry.epoch;
            entry.preparing = false;
            entry.pixels = {};
        }
    }
}

void TextureThumbnailCache::ReleaseConsumer(Consumer consumer, bool invalidate)
{
    for(auto& [key, entry] : m_entries)
    {
        if(invalidate && (entry.consumers & static_cast<unsigned int>(consumer)))
        {
            Invalidate(entry.preview.sourcePath);
        }

        Release(entry.ticket, consumer);
    }
}

void TextureThumbnailCache::BeginFrame()
{
    Poll();

    for(auto& [key, entry] : m_entries)
    {
        if(!entry.browserUsed)
        {
            Release(entry.ticket, Consumer::AssetBrowser);
        }

        entry.browserUsed = false;
    }
}

const TextureThumbnailCache::Preview* TextureThumbnailCache::FindOrCreate(const ProjectPaths& paths,
    const ProjectAssetRegistry& registry, const AssetRecord& record)
{
    return Query(Request(paths, registry, record, Consumer::AssetBrowser));
}

void TextureThumbnailCache::Invalidate(const std::filesystem::path& path)
{
    for(auto& [key, entry] : m_entries)
    {
        if(entry.preview.sourcePath != path.lexically_normal())
        {
            continue;
        }

        if(entry.cancelled)
        {
            entry.cancelled->store(true);
        }

        ++entry.epoch;
        entry.preparing = false;
        entry.ready = false;
        entry.preview.invalidated = true;

        entry.pixels = {};
        entry.nextValidation = {};
    }
}

void TextureThumbnailCache::SetCompletedFence(std::uint64_t fence)
{
    m_completedFence = fence == UINT64_MAX ? 0 : fence;
    Poll();

    for(auto& [key, entry] : m_entries)
    {
        if(entry.uploadFence && entry.uploadFence <= m_completedFence && !m_quarantined)
        {
            entry.staging.Reset();
        }

        if(entry.consumers && !entry.preparing && !entry.uploadGeneration &&
            entry.uploadFence <= m_completedFence && entry.pixels.pixels.empty() &&
            std::chrono::steady_clock::now() >= entry.nextValidation)
        {
            Queue(entry);
        }
    }
}

D3D12_GPU_DESCRIPTOR_HANDLE TextureThumbnailCache::GetHandle(const Preview& preview) const
{
    return m_heap->GetGPUHandle(preview.texture.GetDescriptorIndex());
}

bool TextureThumbnailCache::Upload(ID3D12GraphicsCommandList* commandList, Entry& entry,
    std::uint64_t generation)
{
    if(!m_device || !m_heap || !Retirable(entry))
    {
        return false;
    }

    auto& preview = entry.preview;
    preview.texture.Shutdown();
    entry.staging.Reset();
    entry.uploadFence = 0;
    preview.width = entry.pixels.width;
    preview.height = entry.pixels.height;

    if(!preview.texture.Initialize(m_device, preview.width, preview.height, 1, 1,
        DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST) ||
        !preview.texture.CreateSRV(m_device, *m_heap))
    {
        preview.texture.Shutdown();
        preview.error = "Cannot allocate texture thumbnail.";
        entry.pixels = {};
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    const auto description = preview.texture.GetResource()->GetDesc();
    m_device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes;

    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;

    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if(FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&entry.staging))))
    {
        preview.texture.Shutdown();
        preview.error = "Cannot allocate thumbnail staging.";
        entry.pixels = {};
        return false;
    }

    void* mapped = nullptr;
    const D3D12_RANGE noReads{0, 0};

    if(FAILED(entry.staging->Map(0, &noReads, &mapped)))
    {
        preview.texture.Shutdown();
        entry.staging.Reset();
        preview.error = "Cannot map thumbnail staging.";
        entry.pixels = {};
        return false;
    }

    for(size_t row = 0; row < preview.height; ++row)
    {
        memcpy(static_cast<unsigned char*>(mapped) + footprint.Offset + row * footprint.Footprint.RowPitch,
            entry.pixels.pixels.data() + row * preview.width * 4, static_cast<size_t>(preview.width) * 4);
    }

    entry.staging->Unmap(0, nullptr);
    entry.savedStates = preview.texture.GetSubresourceStates();
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = preview.texture.GetResource();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = entry.staging.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;

    commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    preview.texture.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    entry.uploadGeneration = generation;

    entry.ready = false;
    entry.needsValidation = true;
    preview.invalidated = false;

    entry.pixels = {};
    return true;
}

void TextureThumbnailCache::BeginRecording(std::uint64_t generation)
{
    for(auto& [key, entry] : m_entries)
    {
        if(entry.preview.awaitingSubmission && !entry.displayGeneration)
        {
            entry.displayGeneration = generation;
        }
    }
}

bool TextureThumbnailCache::Record(ID3D12GraphicsCommandList* commandList, std::uint64_t generation)
{
    if(m_quarantined || !generation || !commandList)
    {
        return false;
    }

    size_t uploaded = 0;

    for(auto& [key, entry] : m_entries)
    {
        if(entry.consumers && !entry.pixels.pixels.empty() && uploaded < kUploadsPerFrame && Retirable(entry))
        {
            ++uploaded;
            Upload(commandList, entry, generation);
        }
    }

    return true;
}

void TextureThumbnailCache::Submitted(std::uint64_t generation, std::uint64_t fence)
{
    if(!generation || !fence || fence == UINT64_MAX || m_quarantined)
    {
        return;
    }

    for(auto& [key, entry] : m_entries)
    {
        if(entry.uploadGeneration == generation)
        {
            entry.uploadGeneration = 0;
            entry.uploadFence = fence;
            entry.nextValidation = {};
        }

        if(entry.displayGeneration == generation)
        {
            entry.displayGeneration = 0;
            entry.preview.lastSubmissionFence = fence;
            entry.preview.awaitingSubmission = false;
        }
    }
}

void TextureThumbnailCache::Discarded(std::uint64_t generation)
{
    if(!generation || m_quarantined)
    {
        return;
    }

    for(auto& [key, entry] : m_entries)
    {
        if(entry.uploadGeneration == generation)
        {
            entry.preview.texture.RestoreStatesAfterAbort(entry.savedStates);
            entry.preview.texture.Shutdown();
            entry.staging.Reset();
            entry.uploadGeneration = 0;
            entry.preview.invalidated = true;
            entry.nextValidation = {};
        }

        if(entry.displayGeneration == generation)
        {
            entry.displayGeneration = 0;
            entry.preview.awaitingSubmission = false;
        }
    }
}

void TextureThumbnailCache::Uncertain(std::uint64_t generation)
{
    for(const auto& [key, entry] : m_entries)
    {
        if(generation && (entry.uploadGeneration == generation || entry.displayGeneration == generation))
        {
            m_quarantined = true;
            return;
        }
    }
}

void TextureThumbnailCache::ResolveAfterGPUDrain()
{
    if(m_quarantined)
    {
        ClearAfterGPUDrain();
    }
}

void TextureThumbnailCache::ClearAfterGPUDrain()
{
    for(auto& [key, entry] : m_entries)
    {
        if(entry.cancelled)
        {
            entry.cancelled->store(true);
        }

        entry.preview.texture.Shutdown();
    }

    m_entries.clear();
    m_completedFence = 0;
    m_quarantined = false;

    std::lock_guard lock(m_worker->mutex);
    m_worker->jobs.clear();
    m_worker->result.reset();
    m_worker->wake.notify_one();
}
