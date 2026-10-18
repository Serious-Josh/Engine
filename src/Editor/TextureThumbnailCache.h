#pragma once

#include "Renderer/Texture.h"
#include "assets/ProjectAssetResolver.h"
#include "TextureThumbnailDiskCache.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <wrl/client.h>

class DescriptorHeap;

// All public operations run on the editor/Renderer thread. The worker owns CPU data only.
class TextureThumbnailCache
{
public:
    enum class Consumer : unsigned int { AssetBrowser = 1, MaterialInspector = 2 };
    enum class Failure { None, Preparation, DescriptorCapacity, TextureResource, UploadResource, UploadMap, DeviceLost };
    using Ticket = std::uint64_t;
    static constexpr size_t kCapacity = 64;
    static constexpr size_t kQueueCapacity = 32;
    static constexpr size_t kMaximumPixelBytes = (kCapacity + 2) * 96 * 96 * 4;
    static constexpr size_t kUploadsPerFrame = 2;

    struct Preview
    {
        Texture texture;
        unsigned int width = 0;
        unsigned int height = 0;
        std::string error;
        Failure failure = Failure::None;
        bool retryable = false;

        std::uint64_t lastUse = 0;
        std::uint64_t lastSubmissionFence = 0;
        bool awaitingSubmission = false;
        bool invalidated = false;
        std::filesystem::path sourcePath;
    };

    TextureThumbnailCache();
    ~TextureThumbnailCache();
    TextureThumbnailCache(const TextureThumbnailCache&) = delete;
    TextureThumbnailCache& operator=(const TextureThumbnailCache&) = delete;

    void Configure(ID3D12Device* device, DescriptorHeap& heap);
    void BeginFrame();
    void Invalidate(const std::filesystem::path& path);
    void SetCompletedFence(std::uint64_t completedFence);
    const Preview* FindOrCreate(const ProjectPaths& paths, const ProjectAssetRegistry& registry, const AssetRecord& record);
    Ticket Request(const ProjectPaths& paths, const ProjectAssetRegistry& registry,
        const AssetRecord& record, Consumer consumer);
    // Borrow until the next cache call; a returned error has invalidated=true.
    // Ready queries pin the descriptor through the next Renderer submission.
    const Preview* Query(Ticket ticket);
    void Release(Ticket ticket, Consumer consumer);

    // Descriptor-pressure reclaim hook. Releases SRVs held by idle entries only: no consumer
    // pin, no upload or display generation, nothing awaiting submission, and every fence
    // retired. Visible tiles re-request their previews later. Returns the number released.
    size_t ReleaseIdleDescriptors();
    void ReleaseConsumer(Consumer consumer, bool invalidate = false);
    D3D12_GPU_DESCRIPTOR_HANDLE GetHandle(const Preview& preview) const;

    void BeginRecording(std::uint64_t generation);
    bool Record(ID3D12GraphicsCommandList* commandList, std::uint64_t generation);
    void Submitted(std::uint64_t generation, std::uint64_t fence);
    void Discarded(std::uint64_t generation);
    void Uncertain(std::uint64_t generation);
    void ResolveAfterGPUDrain();
    void ClearAfterGPUDrain();

    struct Diagnostics
    {
        size_t entries = 0;
        size_t resident = 0;
        size_t pendingRetirement = 0;
        size_t pixelBytes = 0;
        size_t retryable = 0;
        size_t terminal = 0;
        std::uint64_t evictions = 0;
        std::uint64_t uploadAttempts = 0;
        std::uint64_t preparationJobs = 0;
        std::uint64_t pixelPreparations = 0;
    };
    Diagnostics GetDiagnostics() const;

private:
    struct Worker;
    struct Entry
    {
        Entry(const ProjectPaths& paths, const AssetRecord& asset) : project(paths), record(asset) {}

        Preview preview;
        ProjectPaths project;
        AssetRecord record;
        TextureThumbnailDiskCache::Image pixels;
        Microsoft::WRL::ComPtr<ID3D12Resource> staging;
        std::vector<D3D12_RESOURCE_STATES> savedStates;
        std::string identity;
        std::shared_ptr<std::atomic_bool> cancelled;

        Ticket ticket = 0;
        std::uint64_t epoch = 0;
        std::uint64_t uploadGeneration = 0;
        std::uint64_t displayGeneration = 0;
        std::uint64_t uploadFence = 0;
        unsigned int consumers = 0;
        bool browserUsed = false;
        bool preparing = false;
        bool ready = false;
        bool needsValidation = false;
        std::chrono::steady_clock::time_point nextValidation{};
        std::chrono::steady_clock::time_point nextUploadAttempt{};
    };
    using Key = std::pair<std::filesystem::path, std::string>;
    bool Queue(Entry& entry);
    void Poll();
    bool Upload(ID3D12GraphicsCommandList* commandList, Entry& entry, std::uint64_t generation);
    bool Retirable(const Entry& entry) const;
    bool EvictUnused(Entry* exclude, bool requireDescriptor);
    bool FailUpload(Entry& entry, Failure failure, std::string error);

    ID3D12Device* m_device = nullptr;
    DescriptorHeap* m_heap = nullptr;
    std::shared_ptr<Worker> m_worker;
    std::map<Key, Entry> m_entries;

    Ticket m_nextTicket = 0;
    std::uint64_t m_useSerial = 0;
    std::uint64_t m_completedFence = 0;
    bool m_quarantined = false;
    std::uint64_t m_evictions = 0;
    std::uint64_t m_uploadAttempts = 0;
    std::uint64_t m_preparationJobs = 0;
    std::uint64_t m_pixelPreparations = 0;
};
