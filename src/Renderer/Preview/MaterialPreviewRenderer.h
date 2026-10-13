#pragma once

#include "Renderer/GBuffer.h"
#include "Renderer/Mesh.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/Passes/ToneMapPass.h"
#include "MaterialPreviewRequest.h"
#include "Renderer/ProjectTextureLoader.h"
#include "assets/AssetRecord.h"
#include "MaterialThumbnailDiskCache.h"
#include "MaterialThumbnailValidation.h"
#include "assets/ProjectAssetRegistry.h"

#include <optional>
#include <map>
#include <tuple>
#include <vector>
#include <deque>
#include <chrono>

class DescriptorHeap;
class ResourceUploader;
class ShaderLibrary;
class ProjectAssetRegistry;
class ProjectPaths;

// Renderer-owned resources, independent of the active scene and its material tables.
class MaterialPreviewRenderer
{
    public:
    static constexpr unsigned int kExtent = 256;
    static constexpr size_t kThumbnailCapacity = 16;

    // Recording borrows this context for the duration of a call.
    // They must not begin, close, submit, wait for, or abort command recording.
    struct RecordingContext
    {
        ID3D12GraphicsCommandList* commandList = nullptr;
        unsigned int frameSlot = 0;
        unsigned int brdfLUTIndex = 0;
        unsigned int averageAlbedoLUTIndex = 0;
    };

    MaterialPreviewRenderer() = default;
    MaterialPreviewRenderer(const MaterialPreviewRenderer&) = delete;
    MaterialPreviewRenderer& operator=(const MaterialPreviewRenderer&) = delete;

    bool Initialize(
        ID3D12Device* device,
        ShaderLibrary& shaders,
        ResourceUploader& uploader,
        DescriptorHeap& rtvHeap,
        DescriptorHeap& dsvHeap,
        DescriptorHeap& resourceHeap);

    // Caller supplies an acquired frame slot and binds the shared resource heap.
    bool Record(const RecordingContext& context);
    void SetRequest(const std::optional<MaterialPreviewRequest>& request);
    // Called before Renderer recording. Only changed dependencies can upload.
    void PrepareTextures(const std::optional<MaterialPreviewRequest>& request, const ProjectAssetRegistry& registry);
    void ConfirmSubmission(std::uint64_t submissionFence);
    std::optional<MaterialPreviewImage> GetImage(const DescriptorHeap& heap) const;

    // Bounded asynchronous request acceptance. Preparation runs at the pre-recording poll.
    bool RequestSavedThumbnail(
        const ProjectPaths& paths,
        const ProjectAssetRegistry& registry,
        const AssetID& id,
        std::uint64_t completedFence,
        std::string& error);
    // Calling this marks a current-frame display use; the descriptor remains Renderer-owned.
    std::optional<MaterialPreviewImage> GetThumbnailImage(const DescriptorHeap& heap);
    std::optional<MaterialPreviewImage> GetThumbnailImage(
        const DescriptorHeap& heap, const ProjectPaths& paths, const AssetID& id);
    // Stops queued work only. Recorded/submitted jobs and images retain lifetime protection.
    void DiscardThumbnailRequests();
    // UI begins one enumeration; the first 16 distinct requests form the pinned working set.
    void BeginThumbnailWorkingSet();
    // Explicit Refresh can invalidate immediately, without waiting for throttled revalidation.
    void InvalidateThumbnailImages();
    void InvalidateThumbnailSource(const std::filesystem::path& path);
    void PollThumbnailCapture(std::uint64_t completedFence);

    // Caller must prove GPU completion before teardown once preview recording exists.
    // Descriptor heaps must remain alive until this returns. No destructor cleanup.
    void Shutdown();

    private:
    bool InitializePipelines(ID3D12Device* device, ShaderLibrary& shaders);

    struct TextureDependencies
    {
        std::array<Texture*, 7> textures{};
        std::optional<MaterialPreviewRequest> request;
        std::vector<AssetRecord> registrySnapshot;
        bool ready = false;
    };

    struct InspectorState
    {
        // The owned request includes the Inspector's independent yaw/pitch and revision.
        std::optional<MaterialPreviewRequest> request;
        bool outputValid = false;
        bool recorded = false;

        TextureDependencies dependencies;
        RenderTarget displayColor;
    };

    struct ThumbnailImage
    {
        RenderTarget color;
        std::filesystem::path projectRoot;
        std::string materialIdentity;
        std::string fingerprint;
        std::uint64_t revision = 0;
        std::uint64_t generationFence = 0;
        std::uint64_t displayFence = 0;
        std::uint64_t lastUse = 0;
        bool ready = false;
        bool stale = false;
        bool recorded = false;
        bool awaitingDisplaySubmission = false;
        std::chrono::steady_clock::time_point nextValidation{};
    };

    struct ThumbnailRequest
    {
        std::filesystem::path projectRoot;
        ProjectAssetRegistry registry;
        AssetID id;
    };

    struct ThumbnailState
    {
        std::optional<MaterialPreviewRequest> request;
        TextureDependencies dependencies;
        RenderTarget displayColor;

        bool outputValid = false;
        bool recorded = false;

        std::uint64_t generationFence = 0;
        std::uint64_t revision = 0;

        MaterialThumbnailDiskCache::Source diskSource;
        ProjectAssetRegistry registry;
        std::string fingerprint;

        bool diskHit = false;
        bool capturePending = false;
        bool cacheEligible = false;
        ThumbnailImage* destination = nullptr;
    };

    // Shared scratch attachments are reused serially on the caller's command list.
    // This operation never changes consumer requests/readiness or owns its borrowed inputs.
    bool RecordJob(
        const RecordingContext& context,
        GPUMaterial material,
        float yaw,
        float pitch,
        const std::array<Texture*, 7>& textures,
        RenderTarget& displayColor);

    enum class TexturePreparation { Unchanged, Ready, Deferred, Failed };
    TexturePreparation PrepareJobTextures(
        const std::optional<MaterialPreviewRequest>& request,
        const ProjectAssetRegistry& registry,
        TextureDependencies& dependencies,
        bool contentVersions = false);
    bool ReclaimTexture(const std::array<Texture*, 7>& preparing);
    void PollThumbnailReadback(std::uint64_t completedFence);
    void ProcessThumbnailRequest(std::uint64_t completedFence);
    bool PrepareSavedThumbnail(const ThumbnailRequest& queued, std::uint64_t completedFence, std::string& error);
    ThumbnailImage* AcquireThumbnailImage(std::uint64_t completedFence,
        const std::filesystem::path& root, const std::string& identity);
    bool IsThumbnailAdmitted(const std::filesystem::path& root, const std::string& identity) const;
    bool CanAcquireThumbnailImage(std::uint64_t completedFence,
        const std::filesystem::path& root, const std::string& identity) const;
    ThumbnailImage* FindThumbnailImage(const std::filesystem::path& root, const std::string& identity);

    Pipeline m_geometryPipeline;
    Pipeline m_lightingPipeline;
    ToneMapPass m_toneMap;

    Mesh m_sphere;
    GBuffer m_gBuffer;

    RenderTarget m_hdrColor;

    InspectorState m_inspector;
    ThumbnailState m_thumbnail;
    MaterialThumbnailValidation m_thumbnailValidation;
    static constexpr size_t kThumbnailQueueCapacity = 32;
    // Stable nodes: the active job may borrow a cache entry until capture completes.
    std::deque<ThumbnailImage> m_thumbnailImages;
    std::deque<ThumbnailRequest> m_thumbnailRequests;
    DescriptorHeap* m_thumbnailRTVHeap = nullptr;
    std::filesystem::path m_thumbnailProject;
    std::string m_lastThumbnailIdentity;
    std::uint64_t m_thumbnailUseSerial = 0;
    bool m_thumbnailWorkingSetActive = false;
    std::vector<std::pair<std::filesystem::path, std::string>> m_thumbnailWorkingSet;

    // One persistent upload and readback allocation, reused only after their submission completes.
    ID3D12Resource* m_thumbnailReadback = nullptr;
    ID3D12Resource* m_thumbnailUpload = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_thumbnailFootprint{};
    std::uint64_t m_thumbnailBufferSize = 0;

    bool m_initialized = false;

    ID3D12Device* m_device = nullptr;
    ResourceUploader* m_uploader = nullptr;
    DescriptorHeap* m_heap = nullptr;

    using TextureKey = std::tuple<std::filesystem::path, std::string, ProjectTextureInterpretation,
        std::filesystem::path, std::filesystem::file_time_type, std::uintmax_t, std::string>;
    struct CachedTexture
    {
        Texture texture;
        std::uint64_t lastUse = 0;
        std::uint64_t submissionFence = 0;
        bool awaitingSubmission = false;
    };
    static constexpr size_t kTextureCapacity = 32;
    std::map<TextureKey, CachedTexture> m_textureCache;
    std::uint64_t m_completedTextureFence = 0;
    std::uint64_t m_textureUseSerial = 0;
};
