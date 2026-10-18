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
#include "MeshThumbnailPreparation.h"
#include "assets/ProjectAssetRegistry.h"

#include <optional>
#include <map>
#include <tuple>
#include <vector>
#include <deque>
#include <chrono>
#include <span>
#include <future>
#include <wrl/client.h>

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
    static constexpr size_t kMeshThumbnailCapacity = 8;
    enum class MeshThumbnailConsumer : unsigned int { AssetBrowser = 1, SceneInspector = 2 };

    // Recording borrows this context for the duration of a call.
    // They must not begin, close, submit, wait for, or abort command recording.
    struct RecordingContext
    {
        ID3D12GraphicsCommandList* commandList = nullptr;
        unsigned int frameSlot = 0;
        unsigned int brdfLUTIndex = 0;
        unsigned int averageAlbedoLUTIndex = 0;
        std::uint64_t generation = 0;
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
    bool BeginRecording(std::uint64_t generation);
    bool ConfirmSubmission(std::uint64_t generation, std::uint64_t submissionFence);
    bool DiscardRecording(std::uint64_t generation);
    void MarkSubmissionUncertain(std::uint64_t generation);
    bool IsQuarantined() const noexcept
    {
        return m_quarantined;
    }
    std::optional<MaterialPreviewImage> GetImage(const DescriptorHeap& heap) const;

    // Bounded asynchronous request acceptance. Preparation runs at the pre-recording poll.
    bool RequestSavedThumbnail(
        const ProjectPaths& paths,
        const ProjectAssetRegistry& registry,
        const AssetID& id,
        std::uint64_t completedFence,
        std::string& error);

    // Main-thread mesh requests/polling, outside command recording.
    bool RequestMeshThumbnail(const ProjectPaths& paths, const ProjectAssetRegistry& registry,
        const AssetID& id, std::uint64_t& ticket, std::string& error,
        MeshThumbnailConsumer consumer = MeshThumbnailConsumer::AssetBrowser);
    void PollMeshThumbnailPreparation(const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    void PollMeshThumbnailGPUCompletion(std::uint64_t completedFence)
    {
        PollMeshGPUFences(completedFence);
    }
    // Main-thread borrow valid until the next poll, discard, or shutdown.
    // Prepared geometry becomes empty after transfer to the preview-owned GPU job.
    const MeshThumbnailPreparation::Job* GetMeshThumbnailPreparation(std::uint64_t ticket) const noexcept;
    void CancelMeshThumbnailPreparation(std::uint64_t ticket);
    void DiscardMeshThumbnailPreparation(std::uint64_t ticket,
        MeshThumbnailConsumer consumer = MeshThumbnailConsumer::AssetBrowser);
    struct MeshThumbnailImage
    {
        D3D12_GPU_DESCRIPTOR_HANDLE srv{};
        MeshThumbnailPreparation::Identity identity;
    };
    std::optional<MeshThumbnailImage> GetMeshThumbnailImage(const DescriptorHeap& heap, std::uint64_t ticket);

    // Descriptor-pressure reclaim hook. Releases the image SRV/RTV of mesh thumbnail jobs that
    // are idle under the same rule as capacity eviction: not recorded, not awaiting display
    // submission, both fences retired and no geometry in flight. Requesters re-request later.
    // Main thread, outside preview recording. Returns the number of jobs released.
    size_t ReleaseIdleMeshThumbnailImages();
    std::string GetMeshThumbnailError(std::uint64_t ticket) const;
    // Calling this marks a current-frame display use; the descriptor remains Renderer-owned.
    std::optional<MaterialPreviewImage> GetThumbnailImage(const DescriptorHeap& heap);
    std::optional<MaterialPreviewImage> GetThumbnailImage(
        const DescriptorHeap& heap, const ProjectPaths& paths, const AssetID& id);
    // Stops queued work only. Recorded/submitted jobs and images retain lifetime protection.
    void DiscardThumbnailRequests();
    // UI begins one enumeration; bounded Browser and Inspector requests share the pinned working set.
    void BeginThumbnailWorkingSet();
    // Explicit Refresh can invalidate immediately, without waiting for throttled revalidation.
    void InvalidateThumbnailImages();
    void InvalidateThumbnailSource(const std::filesystem::path& path);
    void PollThumbnailCapture(std::uint64_t completedFence);

    // Caller must prove GPU completion before teardown once preview recording exists.
    // Descriptor heaps must remain alive until this returns. No destructor cleanup.
    void Shutdown();

    private:
    // Browser admits 16 entries; remaining bounded capacity serves visible Inspector slots.
    static constexpr size_t kThumbnailImageCapacity = 32;
    MeshThumbnailPreparation m_meshPreparation;
    // Recording-only borrows. Caller retains buffers/ranges through submission
    // and GPU completion; describing geometry never allocates GPU resources.
    struct PreviewGeometry
    {
        D3D12_VERTEX_BUFFER_VIEW vertices{};
        D3D12_INDEX_BUFFER_VIEW indices{};
        std::span<const MeshSubmesh> ranges;

        MeshBounds bounds;
        Matrix4 model = Matrix4::Identity();

        unsigned int indexCount = 0;
    };

    struct PreviewFraming
    {
        Matrix4 viewProjection = Matrix4::Identity();
        Vector3 eye;
    };

    // One active geometry allocation (64 MiB), one staging slice (8 MiB), nine images.
    static constexpr size_t kMeshImageCapacity = kMeshThumbnailCapacity + 1;
    static constexpr size_t kMeshUploadBytes = 8 * 1024 * 1024;
    struct MeshGPUJob
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> vertices;
        Microsoft::WRL::ComPtr<ID3D12Resource> indices;
        Microsoft::WRL::ComPtr<ID3D12Resource> staging;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        ImportedMesh geometry;
        MaterialThumbnailDiskCache::Image cachedPixels;
        MaterialThumbnailDiskCache::Source diskSource;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        std::uint64_t imageBytes = 0;
        RenderTarget color;
        PreviewFraming framing;
        Matrix4 model = Matrix4::Identity();
        MeshThumbnailPreparation::Identity identity;
        std::string error;

        size_t copiedBytes = 0;
        size_t recordedBytes = 0;
        std::uint64_t ticket = 0;
        std::uint64_t fence = 0;
        std::uint64_t displayFence = 0;
        bool recorded = false;
        bool rendered = false;
        bool renderRecorded = false;
        bool ready = false;
        bool stale = false;
        bool validating = false;
        bool awaitingDisplaySubmission = false;
        bool diskHit = false;
        bool persistenceAttempted = false;
    };
    std::deque<MeshGPUJob> m_meshJobs;
    std::vector<std::uint64_t> m_meshTickets;
    std::map<std::uint64_t, unsigned int> m_meshConsumers;
    std::uint64_t m_meshCompletedFence = 0;
    bool m_meshTurn = false;
    std::future<void> m_meshWriter;

    bool RecordMeshJob(const RecordingContext& context);
    bool HasMeshWork() const;
    void PollMeshGPUFences(std::uint64_t completedFence);
    void PrepareMeshGPUJob(std::uint64_t ticket);
    bool RecordCachedMeshImage(const RecordingContext& context, MeshGPUJob& job);
    void RecordMeshReadback(const RecordingContext& context, MeshGPUJob& job);
    void PersistMeshImage(MeshGPUJob& job);

    static bool DescribeGeometry(const Mesh& mesh, const Matrix4& model,
        PreviewGeometry& geometry, std::string& error);
    static bool FrameMeshBounds(const MeshBounds& bounds, PreviewFraming& framing,
        Matrix4& model, std::string& error);
    bool RecordGeometryJob(const RecordingContext& context, GPUMaterial material,
        const std::array<Texture*, 7>& textures, RenderTarget& displayColor,
        const PreviewGeometry& geometry, const PreviewFraming& framing);

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

    struct TargetState
    {
        RenderTarget* target;
        D3D12_RESOURCE_STATES state;
    };
    struct TextureState
    {
        CachedTexture* entry;
        std::vector<D3D12_RESOURCE_STATES> states;
        bool awaitingSubmission;
    };
    struct ImageState
    {
        ThumbnailImage* image;
        bool ready;
        bool recorded;
    };
    struct RecordingSnapshot
    {
        std::vector<TargetState> targets;
        std::vector<TextureState> textures;
        std::vector<ImageState> images;

        D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        std::uint64_t generation = 0;

        bool inspectorValid = false;
        bool thumbnailValid = false;
        bool capturePending = false;
    };
    std::optional<RecordingSnapshot> m_recording;
    bool m_quarantined = false;
    std::uint64_t m_completedTextureFence = 0;
    std::uint64_t m_textureUseSerial = 0;
};
