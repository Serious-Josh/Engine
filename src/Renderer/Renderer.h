#pragma once
#include "Renderer/Passes/EditorColliderPass.h"
#include "Renderer/Passes/EditorGridPass.h"
#include "Debug/CpuFrameTiming.h"
#include "Editor/TextureThumbnailCache.h"
#include "Diagnostics/FrameHitchDiagnostics.h"

#include "GBuffer.h"
#include "Preview/MaterialPreviewRenderer.h"
#include "UploadBuffer.h"
#include "StructuredBuffer.h"
#include "MaterialTablePublication.h"
#include "MaterialDependencyRetention.h"
#include "MaterialAdmission.h"
#include "ProjectTextureLoader.h"
#include "CommandContext.h"
#include "DescriptorHeap.h"
#include "GraphicsDevice.h"
#include "GpuTimestampProfiler.h"
#include "RenderObject.h"
#include "RendererStatistics.h"
#include "ResourceUploader.h"
#include "SwapChain.h"
#include "EnvironmentMap.h"
#include "RenderTarget.h"
#include "DisplacementGPUPatchBuffer.h"
#include "Displacement/DisplacementUpdateState.h"
#include "Displacement/DisplacementGeometryBinding.h"

#include "Debug/DebugUI.h"

#include "Math/Vector3.h"
#include "Math/Matrix4.h"
#include "Scene/RenderView.h"
#include "Scene/SceneLighting.h"

#include "Shader/ShaderLibrary.h"

#include "RayTracing/RayTracingSceneResources.h"

#include "Passes/ToneMapPass.h"
#include "Passes/DeferredLightingPass.h"
#include "Passes/BRDFIntegrationPass.h"
#include "Passes/GGXAverageAlbedoPass.h"
#include "Passes/RayTracedShadowPass.h"
#include "Passes/RayTracedReflectionPass.h"
#include "Passes/ShadowDenoisePass.h"
#include "Passes/RayTracedReflectionTemporalPass.h"
#include "Passes/RayTracedReflectionDenoisePass.h"
#include "Passes/ReflectionContributionLimitPass.h"
#include "Passes/NrdDenoiser.h"
#include "Passes/NrdPreparePass.h"
#include "Passes/NrdReflectionResolvePass.h"
#include "Passes/SelectionMaskPass.h"
#include "Passes/RayTracedGIPass.h"
#include "Passes/RayTracedAOPass.h"
#include "Passes/AOTemporalPass.h"
#include "Passes/AODenoisePass.h"
#include "Passes/AOResolvePass.h"
#include "Passes/NrdDiffuseResolvePass.h"
#include "Passes/PathTraceReferencePass.h"
#include "MaterialTables/MaterialTableResources.h"
#include "RendererLightingMode.h"
#include "Passes/GISpatialDenoisePass.h"
#include "Passes/GISignedCorrectionPass.h"
#include "Passes/DisplacementVerticesPass.h"
#include "Passes/DisplacementIndicesPass.h"

#include "Geometry/DisplacementMeshGenerator.h"

#include <d3d12.h>
#include <filesystem>
#include <string>
#include <vector>
#include <array>
#include <optional>
#include <memory>
#include <cstddef>
#include <limits>
#include <climits>

struct EnvironmentAsset
{
    std::string displayName;

    std::filesystem::path relativePath;
    std::string reference;
};

struct RendererOptions
{
    const EditorColliderOverlay* editorColliderOverlay = nullptr; // Borrowed for this Render call only.
    // Editor viewport grid for this frame; absent in Play mode, with the Overlay off and in runtime.
    std::optional<EditorGridRequest> editorGrid;

    std::optional<MaterialPreviewRequest> materialPreview;

    // Borrowed only for this Render call; Editor retains resources through GPU completion.
    std::vector<Texture*> editorTextures;

    size_t selectedObject = (std::numeric_limits<size_t>::max)();
    std::vector<size_t> selectedObjects;

    Vector3 selectionOutlineColor{1.0f, 0.72f, 0.0f};
    unsigned int selectionOutlineThickness = 2;

    bool enableRTGI = true;

    float rtgiContactAOStrength = 1.0f;
    float rtgiContactAORadius = 2.5f;

    // Checked view for this frame (BuildFlyRenderView/ResolveMainCameraView). Absent: the scene's
    // Editor fly camera, unchanged. Copied by value; the renderer retains only its identity.
    std::optional<RenderView> view;

    // LIGHT.1 scene lighting captured for this frame (FileSceneRuntime::CaptureSceneLighting).
    // Absent: the scene's legacy DirectionalLight only, exactly as before LIGHT.1. Copied by value.
    std::optional<SceneLightingSnapshot> lighting;

    // RTXPT.1 experimental lighting mode for this frame only. Legacy is the default; the selection is
    // never persisted. An unavailable reference mode renders Legacy and reports why.
    RendererLightingMode lightingMode = RendererLightingMode::Legacy;
    PathTraceReferenceSettings pathTraceReference;
};

// Evidence of the reference path tracer for the Editor and diagnostics. Values only.
struct PathTraceReferenceStatus
{
    PathTraceReferenceAvailability availability = PathTraceReferenceAvailability::NotRequested;
    std::string unavailableReason;

    // True when the most recent recorded frame rendered with the reference mode.
    bool activeLastFrame = false;
    std::uint32_t accumulatedSamples = 0;
    std::uint64_t accumulationResets = 0;
    std::uint64_t resourceBytes = 0;
};

// Evidence of the RTXPT.2 raster-primary mode for the Editor and diagnostics. Values only.
struct PathTraceRasterPrimaryStatus
{
    PathTraceRasterPrimaryAvailability availability = PathTraceRasterPrimaryAvailability::NotRequested;
    std::string reason;
};

class Scene;
struct FrameLighting;
class FileSceneAssetCatalog;
class FileSceneRuntime;
class MirrorTestScene;
struct Material;

class Renderer
{
    public:
    ~Renderer();

    bool Initialize(
        void* windowHandle,
        unsigned int width,
        unsigned int height,
        const std::filesystem::path& assetRoot, std::span<const std::byte> packagedShaders = {},
        const HDRImageData* environment = nullptr, std::string environmentIdentity = {},
        bool deferEditorResources = false,
        // Packaged runtime only: the archive's material table bootstrap entry (empty for archives that predate
        // it). Development loads assets/MaterialTables instead. Absence never fails initialization.
        std::span<const std::byte> packagedMaterialTables = {});
    // First project selection only for interactive startup; legacy initialization calls this
    // eagerly. Idempotent, with no project asset requests or scene recording.
    bool InitializeEditorResources();
    using FrameRenderResult = RendererDiagnostics::FrameRenderResult;

    enum class DisplacementCompletionStatus
    {
        NoPendingUpdate,
        AwaitingSubmission,
        OriginalSubmissionPending,
        LaterCoveragePending,
        Finalized,
        Error
    };

    enum class DisplacementRequestResult
    {
        Accepted,
        Deferred,
        Error
    };

    static constexpr bool IsAsyncDisplacementEnabled()
    {
        return kEnableFrameIntegratedDisplacement;
    }

    struct DisplacementCompletionResult
    {
        const Scene* scene = nullptr;
        std::uint64_t sceneResourceLifetime = 0;
        std::uint64_t sceneRevision = 0;
        std::uint64_t requestRevision = 0;
        // Diagnostic origin only; acknowledgment remains revision-based.
        CpuTiming::Clock::time_point acceptedAt{};
    };

    [[nodiscard]] FrameRenderResult Render(const Scene& scene, const RendererOptions& options);
    // Project Browser only: no scene, previews, asset uploads or compute recording.
    [[nodiscard]] bool RenderUIOnly();
    // Nonblocking and outside any frame recording/exit scope. Finalized stays
    // observable until its exact payload is explicitly taken; no acknowledgment.
    [[nodiscard]] DisplacementCompletionStatus ProcessDisplacementCompletion();
    // Owner-aware delivery validates the current registered resource lifetime
    // before transferring a payload. A newer requested revision is permitted.
    [[nodiscard]] bool TakeDisplacementCompletion(MirrorTestScene& owner, DisplacementCompletionResult& result);
    [[nodiscard]] DisplacementRequestResult RequestDisplacementUpdate(MirrorTestScene& owner,
                                                                      std::uint64_t requestRevision);
    void Shutdown();

    bool Resize(
        unsigned int width,
        unsigned int height);

    struct FinalSceneImage
    {
        D3D12_GPU_DESCRIPTOR_HANDLE srv{};

        unsigned int width = 0;
        unsigned int height = 0;
    };

    // Borrowed descriptor: valid until checked renderer resize or shutdown, never owned by ImGui.
    [[nodiscard]] bool TryGetFinalSceneImage(FinalSceneImage& image) const;

    // Copied evidence of the most recently recorded frame view (identity and matrices) and the
    // number of camera cuts applied. Read-only; selection stays with the caller's RendererOptions.
    const std::optional<RenderViewIdentity>& GetRecordedViewIdentity() const noexcept { return m_renderedViewIdentity; }
    const Matrix4& GetRecordedView() const noexcept { return m_recordedView; }
    const Matrix4& GetRecordedProjection() const noexcept { return m_recordedProjection; }
    std::uint64_t GetCameraCutCount() const noexcept { return m_cameraCutCount; }
    // LIGHT.1 evidence: the sun and the deferred light count of the last recorded frame.
    const DirectionalLight& GetRecordedSun() const noexcept { return m_recordedSun; }
    std::uint32_t GetRecordedAdditionalLightCount() const noexcept { return m_recordedAdditionalLightCount; }
    const PathTraceReferenceStatus& GetPathTraceReferenceStatus() const noexcept { return m_pathTraceStatus; }
    const PathTraceRasterPrimaryStatus& GetPathTraceRasterPrimaryStatus() const noexcept { return m_rasterPrimaryStatus; }

    std::optional<MaterialPreviewImage> GetMaterialPreviewImage() const
    {
        return m_materialPreviewRenderer.GetImage(m_resourceDescriptorHeap);
    }

    // Enqueue between frames, before command recording; true means accepted, not image-ready.
    bool RequestSavedMaterialThumbnail(
        const ProjectPaths& paths,
        const ProjectAssetRegistry& registry,
        const AssetID& id,
        std::string& error)
    {
        return m_materialPreviewRenderer.RequestSavedThumbnail(
            paths, registry, id, m_commandContext.GetCompletedFenceValue(), error);
    }

    std::optional<MaterialPreviewImage> GetSavedMaterialThumbnailImage()
    {
        return m_materialPreviewRenderer.GetThumbnailImage(m_resourceDescriptorHeap);
    }

    std::optional<MaterialPreviewImage> GetSavedMaterialThumbnailImage(
        const ProjectPaths& paths, const AssetID& id)
    {
        return m_materialPreviewRenderer.GetThumbnailImage(m_resourceDescriptorHeap, paths, id);
    }

    void BeginSavedMaterialThumbnailWorkingSet()
    {
        m_materialPreviewRenderer.BeginThumbnailWorkingSet();
    }

    // Mesh thumbnail integration points; callers poll between frames using the live project registry.
    bool RequestMeshThumbnail(const ProjectPaths& paths, const ProjectAssetRegistry& registry,
        const AssetID& id, std::uint64_t& ticket, std::string& error,
        MaterialPreviewRenderer::MeshThumbnailConsumer consumer = MaterialPreviewRenderer::MeshThumbnailConsumer::AssetBrowser)
    {
        return m_materialPreviewRenderer.RequestMeshThumbnail(paths, registry, id, ticket, error, consumer);
    }

    void PollMeshThumbnails(const ProjectPaths& paths, const ProjectAssetRegistry& registry)
    {
        m_materialPreviewRenderer.PollMeshThumbnailGPUCompletion(m_commandContext.GetCompletedFenceValue());
        m_materialPreviewRenderer.PollMeshThumbnailPreparation(paths, registry);
    }

    std::optional<MaterialPreviewRenderer::MeshThumbnailImage> GetMeshThumbnailImage(std::uint64_t ticket)
    {
        return m_materialPreviewRenderer.GetMeshThumbnailImage(m_resourceDescriptorHeap, ticket);
    }

    std::string GetMeshThumbnailError(std::uint64_t ticket) const
    {
        return m_materialPreviewRenderer.GetMeshThumbnailError(ticket);
    }

    bool IsMeshThumbnailRequestCurrent(std::uint64_t ticket) const
    {
        const auto* job = m_materialPreviewRenderer.GetMeshThumbnailPreparation(ticket);
        return job && job->state != MeshThumbnailPreparation::State::Cancelled &&
            job->state != MeshThumbnailPreparation::State::Failed &&
            m_materialPreviewRenderer.GetMeshThumbnailError(ticket).empty();
    }

    // True when preparation itself failed (unreadable, unsupported or invalid source), as opposed
    // to a temporary GPU-side failure reported through GetMeshThumbnailError.
    bool HasMeshThumbnailPreparationFailed(std::uint64_t ticket) const
    {
        const auto* job = m_materialPreviewRenderer.GetMeshThumbnailPreparation(ticket);
        return job && job->state == MeshThumbnailPreparation::State::Failed;
    }

    void DiscardMeshThumbnail(std::uint64_t ticket,
        MaterialPreviewRenderer::MeshThumbnailConsumer consumer = MaterialPreviewRenderer::MeshThumbnailConsumer::AssetBrowser)
    {
        m_materialPreviewRenderer.DiscardMeshThumbnailPreparation(ticket, consumer);
    }

    void DiscardSavedMaterialThumbnailRequests()
    {
        m_materialPreviewRenderer.DiscardThumbnailRequests();
    }

    void InvalidateSavedMaterialThumbnailImages()
    {
        m_materialPreviewRenderer.InvalidateThumbnailImages();
    }

    void InvalidateSavedMaterialThumbnailSource(const std::filesystem::path& path)
    {
        m_materialPreviewRenderer.InvalidateThumbnailSource(path);
    }

    void PrepareMaterialPreviewTextures(
        const std::optional<MaterialPreviewRequest>& request,
        const ProjectAssetRegistry& registry)
    {
        m_materialPreviewRenderer.PollThumbnailCapture(m_commandContext.GetCompletedFenceValue());
        m_materialPreviewRenderer.PrepareTextures(request, registry);
    }

    bool ResizeScene(unsigned int width, unsigned int height);

    ID3D12Device* GetDevice();
    ResourceUploader& GetResourceUploader();
    DescriptorHeap& GetResourceDescriptorHeap();
    ShaderLibrary& GetShaderLibrary();
    DebugUI& GetDebugUI();

    // Shared shader-visible CBV/SRV/UAV heap accounting for diagnostics; never logs.
    DescriptorHeap::Statistics GetResourceDescriptorStatistics() const noexcept
    {
        return m_resourceDescriptorHeap.GetStatistics();
    }

    // Main-thread reclaim boundary for descriptor exhaustion. Outside any recording, asks the
    // Editor texture-thumbnail and mesh-thumbnail caches to release idle, fence-retired SRVs.
    // Scene, DXR, material and in-flight descriptors are never touched. Returns the count freed.
    size_t ReclaimIdleEditorDescriptors();

    // Preparation only. No scene mesh binding or active DXR snapshot publication.
    std::optional<RayTracingSceneResources::ReplacementCandidate> PrepareDXRMembershipCandidate(
        const FileSceneRuntime& runtime, FileSceneAssetCatalog& catalog,
        const std::vector<RayTracingSceneResources::ResidencyProposal>& proposals,
        unsigned int frameSlot, std::string& error, const FileSceneRuntime* membershipSource = nullptr,
        RayTracingSceneResources::CandidateFailure* failure = nullptr);
    RayTracingSceneResources::CandidateStatus PollDXRMembershipCandidate(
        const RayTracingSceneResources::ReplacementCandidate& candidate, const FileSceneRuntime& runtime,
        FileSceneAssetCatalog& catalog, std::string& error) const;
    bool RefreshDXRMembershipCandidateTransforms(const RayTracingSceneResources::ReplacementCandidate& candidate,
        const FileSceneRuntime& runtime, FileSceneAssetCatalog& catalog, unsigned int frameSlot, std::string& error);

    bool InitializeSceneResources(
        const Scene& scene,
        const std::vector<Material*>& materials);

    // Failed-attempt recovery only; may drain the queue. Candidate assets must
    // remain alive until this succeeds. Does not resolve async displacement.
    [[nodiscard]] bool CleanupFailedSceneInitialization(const Scene& candidate);

    [[nodiscard]] bool ReleaseSceneResources();
    // Checked teardown only: performs the caller's existing queue drain, then
    // resolves exact successful submissions or cancels an unrecorded request.
    // Output is empty when nothing finalized; no scene acknowledgment here.
    [[nodiscard]] bool DrainAndResolveSceneDisplacement(const Scene& scene, DisplacementCompletionResult& result);
    void ResetSceneHistory();

    // Nonblocking editor resource retirement; valid only within this Renderer lifetime.
    std::uint64_t GetEditorCompletedFence() const;
    std::uint64_t GetEditorSubmissionFence() const;

    bool WaitForGPU();

    bool ReplaceMeshGeometry(
        Mesh& mesh,
        const void* vertices,
        size_t vertexCount,
        const unsigned int* indices,
        size_t indexCount);

    // After InitializeSceneResources succeeds; checked scene release is the
    // only unregistration path. The owner must outlive this registration.
    bool RegisterDisplacementHeightmap(MirrorTestScene& owner);
    bool GenerateDisplacementMeshGPU(
        MirrorTestScene& owner,
        const DisplacementMeshSettings& settings);

    const std::vector<EnvironmentAsset>& GetAvailableEnvironments() const
    {
        return m_availableEnvironments;
    }

    size_t GetCurrentEnvironmentIndex() const
    {
        return m_currentEnvironmentIndex;
    }

    bool SetEnvironment(size_t index);
    bool SetEnvironmentImage(const HDRImageData*, std::string identity, std::string displayName, std::string reference = {});
    const std::string& GetEnvironmentIdentity() const noexcept { return m_environmentIdentity; }

    const RendererStatistics& GetStatistics() const;

    const CpuTiming::FrameSample& GetCpuFrameTiming() const
    {
        return m_cpuFrameTiming;
    }

    // CPU-only diagnostics, independent of asynchronous activation.
    static constexpr bool IsFrameHitchDiagnosticsEnabled()
    {
        return kEnableFrameHitchDiagnostics;
    }

    // Reversible logging-isolation experiment; independent of frame timing diagnostics.
    static constexpr bool kSuppressDisplacementLifecycleLoggingForDiagnostics = true;

    using FrameHitchRegion = RendererDiagnostics::FrameHitchRegion;

    // Main-thread, between frames only. Identity must be reacquired after scene initialization.
    // Fixed record order/count; texture descriptors cannot change in this checkpoint.
    std::uint64_t GetMaterialTableIdentity() const noexcept
    {
        return m_sceneResourceLifetime;
    }

    std::uint64_t GetMaterialTableRevision() const noexcept
    {
        return m_materialPublication.revision;
    }

    bool HasPublishedMaterial(const Material& material) const noexcept;
    // Engine thread, pre-Render. Repeated calls advance one bounded admission.
    MaterialAdmissionResult AdvanceMaterialAdmission(ProjectMaterialPreparationService& preparation,
        std::shared_ptr<const ProjectMaterialPreparationService::Candidate> candidate,
        FileSceneAssetCatalog& catalog, const ProjectPaths& paths, const ProjectAssetRegistry& registry,
        FileSceneRuntime& runtime);
    MaterialAdmissionResult AdvanceArchiveMaterialAdmission(ProjectMaterialPreparationService& preparation,
        std::shared_ptr<const ProjectMaterialPreparationService::Candidate> candidate,
        FileSceneAssetCatalog& catalog, FileSceneRuntime& runtime);
    void CancelMaterialAdmission();
    static constexpr size_t MaterialAdmissionLimit = 4096;

    // Shared shader-visible CBV/SRV/UAV heap. Measured in B.1: 119 descriptors after renderer
    // initialization; Editor preview caches can hold up to ~137 more (64 texture thumbnails,
    // 9 mesh thumbnail jobs, 32 material images, 32 preview textures); each unique resident mesh
    // needs 2 and each texture 1. The previous 256 could be exhausted by the Editor alone.
    // 4096 matches MaterialAdmissionLimit and costs ~128 KiB. Exhaustion still recovers (B.1).
    static constexpr unsigned int ResourceDescriptorCapacity = 4096;
    bool CanEditPendingMaterialBindings(const Scene& scene) const noexcept;
    struct SelectionPublicationIdentity
    {
        std::uint64_t rendererLifetime = 0;
        std::uint64_t bindingEpoch = 0;
    };
    SelectionPublicationIdentity GetSelectionPublicationIdentity(const Scene& scene) const noexcept;
    bool IsCommandRecordingActive() const noexcept
    {
        return m_activeRecordingGeneration != 0 || m_displacementExitScopeGeneration != 0;
    }
    bool CopyMaterialTableSnapshot(std::uint64_t identity, std::vector<GPUMaterial>& records,
        std::string* errorMessage = nullptr) const;
    bool SubmitMaterialTableSnapshot(std::uint64_t identity, const std::vector<GPUMaterial>& records,
        std::string* errorMessage = nullptr);

    // Complete table + owning leases. Every referenced texture descriptor must be covered.
    bool SubmitMaterialTableRevision(std::uint64_t identity, const std::vector<GPUMaterial>& records,
        std::vector<Texture::Dependency> dependencies, std::string* errorMessage = nullptr);
    // Main-thread requests; no material bindings change. Caller cancels rejected/unused tickets.
    bool QueuePreparedTextureUpload(const PreparedProjectTexture& prepared, std::uint64_t& ticket, std::string& error);
    std::optional<PreparedTextureUpload::State> GetPreparedTextureUploadState(std::uint64_t ticket, std::string* error = nullptr) const;
    bool TakeCompletedPreparedTextureUpload(std::uint64_t ticket, Texture& destination);
    void CancelPreparedTextureUpload(std::uint64_t ticket);

    class FrameHitchProbe
    {
        public:
        FrameHitchProbe(
          Renderer& renderer,
          FrameHitchRegion region,
          bool measure = true);
        ~FrameHitchProbe();
        FrameHitchProbe(const FrameHitchProbe&) = delete;
        FrameHitchProbe& operator=(const FrameHitchProbe&) = delete;

        private:
        double* m_total = nullptr;

        CpuTiming::Clock::time_point m_start{};
    };

    void RecordFrameHitchRegion(
        FrameHitchRegion region,
        double milliseconds,
        std::uint64_t cameraRevision);
    // Shared nonblocking texture service. Owned by Renderer and available to all editor consumers.
    TextureThumbnailCache& GetTextureThumbnailCache() { return m_textureThumbnails; }

    void BeginFrameHitchDiagnostics(std::uint64_t engineFrameSerial);
    void RecordFrameHitchDiagnostics(
        CpuTiming::Clock::time_point frameStart,
        double frameMilliseconds,
        std::optional<FrameRenderResult> outcome);

    void ResetCpuSubmissionCadence()
    {
        m_hasPreviousRenderExecute = false;
    }

    private:
    friend class MeshResidencyPublication;
    friend class FileSceneRuntime;
#if defined(ENGINE_HAS_VERIFICATION)
    friend class PresentationTransformVerification;
    friend class ScenePhysicsDataVerification;
    friend class SceneHierarchyVerification;
    friend class DescriptorRecoveryVerification;
    friend class ThumbnailSchedulingVerification;
    friend class NormalMapVerification;
    friend class EditorGridVerification;
    friend class ProjectBrowserVerification;
#endif
    friend class SceneMembershipPublication;
#if defined(ENGINE_HAS_VERIFICATION)
    friend class SceneDuplicationVerification;
    friend class SceneMembershipVerification;
#endif
    friend class SceneMaterialBindingPublication;
#if defined(ENGINE_HAS_VERIFICATION)
    friend class SceneMaterialBindingVerification;
    friend class MaterialAdmissionVerification;
#endif
    friend class ArchiveSurfaceAdmission;
    MaterialAdmissionResult AdvanceMaterialAdmissionInternal(ProjectMaterialPreparationService&,
        std::shared_ptr<const ProjectMaterialPreparationService::Candidate>, FileSceneAssetCatalog&,
        const ProjectPaths*, const ProjectAssetRegistry*, FileSceneRuntime&);
#if defined(ENGINE_HAS_VERIFICATION)
    friend class ViewportMaterialDropVerification;
#endif
    static constexpr bool kEnableFrameHitchDiagnostics = true;
    static constexpr double kCpuHitchThresholdMilliseconds = 25.0;

    using HitchSampleWindow = RendererDiagnostics::HitchSampleWindow;

    // A fresh snapshot is opened by Engine before Frame(). Optional timings
    // distinguish an operation that did not run from a measured zero duration.
    using FrameHitchSnapshot = RendererDiagnostics::FrameHitchSnapshot;

    // CPU-only accumulation for helpers with multiple early returns.
    struct HitchTimingScope
    {
        explicit HitchTimingScope(double& accumulatedMilliseconds)
            : total(accumulatedMilliseconds), start(CpuTiming::Clock::now())
        {
        }

        ~HitchTimingScope()
        {
            total += CpuTiming::MillisecondsSince(start);
        }

        double& total;

        CpuTiming::Clock::time_point start;
    };

    const char* GetDisplacementDiagnosticPhase() const;

    using FrameHitchDiagnostics = RendererDiagnostics::FrameHitchDiagnostics;

    FrameHitchDiagnostics m_frameHitchDiagnostics{};
    void ReportFrameHitchDiagnostics(CpuTiming::Clock::time_point now) const;

    struct DisplacementHeightmapRegistration
    {
        MirrorTestScene* owner = nullptr;
        const Scene* scene = nullptr;
        Mesh* mesh = nullptr;

        Texture* texture = nullptr;

        ID3D12Resource* resource = nullptr;

        DescriptorHeap* descriptorHeap = nullptr;

        ID3D12DescriptorHeap* heapResource = nullptr;
        unsigned int descriptor = UINT_MAX;
        std::uint64_t sceneLifetime = 0;

        bool useUnresolved = false;
    };

    DisplacementHeightmapRegistration m_heightmapRegistration;
    bool ValidateDisplacementHeightmapRegistration() const;
    bool GenerateDisplacementMeshGPU(
        Mesh& mesh,
        Texture& heightmap,
        const DisplacementMeshSettings& settings);

    using DisplacementUpdateStatus = RendererDisplacement::DisplacementUpdateStatus;
    using DisplacementHistoryPreparation = RendererDisplacement::DisplacementHistoryPreparation;
    using DisplacementTLASPreparation = RendererDisplacement::DisplacementTLASPreparation;
    using DisplacementRecordingStage = RendererDisplacement::DisplacementRecordingStage;

    using DisplacementReservationOwnership = RendererDisplacement::DisplacementReservationOwnership;
    using DisplacementTLASUse = RendererDisplacement::DisplacementTLASUse;
    using DisplacementFrameExitState = RendererDisplacement::DisplacementFrameExitState;
    using DisplacementFrameRecovery = RendererDisplacement::DisplacementFrameRecovery;

    using PendingDisplacementUpdate = RendererDisplacement::PendingDisplacementUpdate;
    using DisplacementRecordingOutcome = RendererDisplacement::DisplacementRecordingOutcome;
    using DisplacementCoverageReceipt = RendererDisplacement::DisplacementCoverageReceipt;
    using DisplacementSubmissionCoverage = RendererDisplacement::DisplacementSubmissionCoverage;

    // UNUSED. Future scheduling must fence the recorded slot's allocator,
    // UploadBuffer region and TLAS instance upload before CPU reuse. Keep
    // the scene-owned mesh/heightmap and their descriptors alive/unchanged;
    // keep Renderer-owned patch storage, heaps, AS and retired BLAS resources
    // alive until resolution. Scene release/reinitialization/shutdown must
    // drain and resolve this record before destroying any dependency.
    // This record neither owns these resources nor proves safe draw publication.
    PendingDisplacementUpdate m_pendingDisplacementUpdate;

    DisplacementSubmissionCoverage m_displacementCoverage;
    // UNUSED activation. Must precede the original displacement submission.
    // Once enabled every Renderer Begin/End/abort/Execute is covered or rejected.
    // Teardown stays blocked until async finalization resolves and clears coverage.
    bool BeginPendingDisplacementCoverage();
    bool ValidatePendingDisplacementCoverage() const;
    // UNUSED retirement prerequisite, not GPU completion or permission to release.
    bool ValidatePendingDisplacementRetirementCoverage() const;
    // UNUSED. Freeze only outside recording, after exact completion and complete
    // reference coverage. Failure never drops receipts or submission ownership.
    bool ValidatePendingDisplacementCoverageResolution() const;
    bool BeginPendingDisplacementCoverageResolution();
    bool ValidateCoveredDisplacementRecording() const;
    bool ValidateCoveredDisplacementGeometry() const;
    ReservedSubmissionResult ExecuteCoveredDisplacementRecording(
        unsigned int frameSlot,
        SubmissionFenceReservation reservation,
        SubmissionFenceToken& token);
    // UNUSED later-frame ownership. Acquire before any submission attempt.
    // Cleanup requires original completion and full replacement-only coverage.
    // Later tokens may still be pending; releasing provenance neither completes
    // GPU work nor releases resources. Historical receipts remain until success.
    bool ReserveLaterDisplacementSubmission();
    bool CleanupLaterDisplacementSubmissions();
    bool ResolveLaterDisplacementFrameExit();

    bool HasUnresolvedDisplacementUpdate() const;
    bool ValidatePendingDisplacementIdentity() const;
    // UNUSED. Acquire directly from CommandContext and adopt exclusively;
    // arbitrary externally supplied reservation identities are not accepted.
    bool ReservePendingDisplacementSubmission();
    bool ValidatePendingDisplacementReservation() const;
    bool CancelPendingDisplacementReservation();
    // Future caller must check End success, arm immediately before its
    // ExecuteReserved call, then immediately forward that exact enum/token.
    // These helpers do not execute, wait, publish, or resolve patch tracking.
    bool ArmPendingDisplacementSubmission();
    bool ResolvePendingDisplacementSubmission(
        ReservedSubmissionResult result,
        SubmissionFenceToken token);
    // UNUSED. Call immediately after a checked TLAS build result, using the
    // identity captured before that call. For no-build later frames, reuse
    // only an already submitted receipt for the still-current shared TLAS.
    bool CapturePendingDisplacementTLASUse(
        unsigned int frameSlot,
        bool rebuilt,
        bool recordingSucceeded,
        std::uint64_t previousTLASRecording);
    // Future caller forwards the exact successful ExecuteReserved result
    // immediately after End/Execute, before any other submission in this slot.
    // Never retains/releases tokens; the future submission owner does that.
    bool AssociatePendingDisplacementTLASSubmission(
        unsigned int frameSlot,
        std::uint64_t generation,
        ReservedSubmissionResult result,
        SubmissionFenceToken token);
    void InvalidateRecordingEligibility();
    bool BeginTrackedRecording(unsigned int frameSlot);
    bool EndTrackedRecording();
    bool AbortTrackedRecording();
    bool ResolvePreviewAfterGPUDrain();
    bool ExecuteTrackedRecording(unsigned int frameSlot);

    // Archive startup progress only; caller owns slot rotation, cancellation and polling.
    // Failure retains existing recording/submission ownership for checked teardown.
    void SubmitArchiveMaterialProgress(unsigned int frameSlot);

    // Associate a successfully executed recording, not GPU completion. The caller supplies
    // the captured fence and retains its own dependency-confirmation failure policy.
    bool ConfirmRecordedMaterialSubmission(
        unsigned int frameSlot,
        std::uint64_t recordingGeneration,
        std::uint64_t recordedRevision,
        std::uint64_t submissionFence);

    struct RecordingEligibilityScope
    {
        Renderer& renderer;

        explicit RecordingEligibilityScope(Renderer& owner) : renderer(owner)
        {
            renderer.InvalidateRecordingEligibility();
        }

        ~RecordingEligibilityScope()
        {
            renderer.InvalidateRecordingEligibility();
        }
    };

    std::uint64_t m_activeRecordingGeneration = 0;
    // Persists through Close; eligibility's active generation is cleared at End.
    std::uint64_t m_commandRecordingGeneration = 0;
    bool m_commandSubmissionAttempted = false;
    unsigned int m_activeRecordingSlot = UINT_MAX;

    using FrameDisplacementPreparation = RendererDisplacement::FrameDisplacementPreparation;

    struct DiagnosticLODBindingIdentity
    {
        ID3D12Resource* resource = nullptr; // Borrowed, never dereferenced.

        D3D12_GPU_VIRTUAL_ADDRESS address = 0;

        size_t bytes = 0;
        unsigned int descriptor = UINT_MAX;
    };

    FrameDisplacementPreparation m_frameDisplacementPreparation;
    DiagnosticLODBindingIdentity m_diagnosticLODBindings[2]{};
    // Observation only in current paths; no async record is populated.
    // Reset is void, but Renderer initializes exactly two upload regions;
    // for a live resource and slot < 2 its implementation always resets.
    void ObserveFrameUploadReset(unsigned int frameSlot);
    void ObserveDiagnosticLODBinding(
        unsigned int frameSlot,
        D3D12_GPU_VIRTUAL_ADDRESS address,
        size_t bytes,
        bool descriptorCreated);
    // UNUSED eligibility for future displacement integration. Does not
    // attest submission/GPU completion or unrelated pass success.
    bool ValidateFrameDisplacementPreparation(unsigned int frameSlot) const;
    // UNUSED for async activation. Identity/order only: no guarantee about
    // unrelated void passes, successful submission, or GPU completion.
    bool ValidatePendingDisplacementRecording(unsigned int frameSlot) const;
    // Future caller must immediately supply the checked result of the
    // corresponding compute/BLAS/merged-TLAS recording API, in order.
    // Compute success means both vertex and index passes succeeded.
    // A false result or invalid transition invalidates recording eligibility.
    bool AdvancePendingDisplacementRecording(
        unsigned int frameSlot,
        DisplacementRecordingStage stage,
        bool recordingSucceeded);
    // UNUSED. Caller must have successfully begun this frame slot and reset
    // its upload region exactly once. Call before camera/previous-frame
    // constants or temporal consumers are recorded. Recording only, before
    // Execute; caller arms the failure gate before accepting AS mutation.
    // Requests TLAS work only; the future merged transform/reference build
    // must explicitly advance each slot to Recorded after recording succeeds.
    bool PreparePendingDisplacementForFrame(unsigned int frameSlot);
    enum class FrameDisplacementRecordingResult
    {
        Rejected, // No commands added; does not resolve any preexisting uncertain submission.
        Recorded,
        Discarded, // Entire unsubmitted frame closed; AS ownership may still be mutated.
        Unresolved // Discard not confirmed. Never resolve patch tracking or reuse staging.
    };

    // Default-off repeated camera LOD updates, with at most one prepared/in-flight
    // request. All async handoff, recovery, binding and submission gates derive
    // from this switch. Startup/switch/resource-rebuild generation stays synchronous.
    static constexpr bool kEnableAsyncCameraDisplacementUpdates = true;
    static constexpr bool kEnableFrameIntegratedDisplacement = kEnableAsyncCameraDisplacementUpdates;
    static constexpr bool kEnableFrameIntegratedDisplacementRecording = kEnableFrameIntegratedDisplacement;
    static constexpr bool kEnableFrameDisplacementRequestPreparation = kEnableFrameIntegratedDisplacement;
    // DISABLED, before Begin/Render. Insufficient persistent storage or an
    // overlapping request returns false without consuming/acknowledging revision.
    // The caller retains newer requests; no scene snapshot or GPU work is created.
    bool PrepareFrameDisplacementRequest(
        const Scene& scene,
        Mesh& mesh,
        std::uint64_t requestRevision,
        const DisplacementMeshSettings& settings);
    // UNUSED handoff after successful frame Begin, before diagnostic allocations.
    // Reservation is already owned; coverage starts in this exact recording.
    bool ActivatePreparedDisplacementRequest(unsigned int frameSlot);
    bool CancelPreparedDisplacementRequest(); // UNUSED; only before any recording handoff.
    bool BuildDisplacementRequestLayout(
        const Mesh& mesh,
        const DisplacementMeshSettings& settings,
        DisplacementGPUIndexLayout& layout,
        RayTracingGeometryCounts& counts) const;
    // DISABLED. Call after diagnostic LOD allocation, before ordinary TLAS work
    // and temporal constants. Caller must separately prepare persistent metadata
    // capacity, an owned LOD/count request, reservation and coverage; growth is
    // forbidden here. Heightmap/settings must remain valid during recording,
    // and all referenced GPU resources must survive submission resolution.
    // Failure aborts the whole frame when possible; never continue rendering.
    FrameDisplacementRecordingResult RecordPendingDisplacementForFrame(unsigned int frameSlot);
    // UNUSED whole-frame scope: future Render integration must construct it
    // before displacement recording (or later-frame reservation), keeping it
    // alive through End, reserved submission and every later return, including
    // Present failure.
    // Eligibility invalidation alone never resolves this ownership.
    struct DisplacementFrameExitScope
    {
        Renderer& renderer;

        bool armed = false;

        explicit DisplacementFrameExitScope(Renderer& owner);
        DisplacementFrameExitScope(const DisplacementFrameExitScope&) = delete;
        DisplacementFrameExitScope& operator=(const DisplacementFrameExitScope&) = delete;
        ~DisplacementFrameExitScope();
    };

    static constexpr bool kEnableDisplacementFrameExitResolution = kEnableFrameIntegratedDisplacement;
    std::uint64_t m_displacementExitScopeGeneration = 0;
    bool CaptureDisplacementFrameRecovery(unsigned int frameSlot);
    bool ResolveDisplacementFrameExit();
    enum class DisplacementCompletionObservation
    {
        NoUpdate,
        Pending,
        Complete,
        Error
    };
    // UNUSED, observation only. Complete does not mean finalized. Error
    // preserves the unresolved record and existing lifecycle guards; a
    // future caller must stop rather than publish/release/reset on Error.
    DisplacementCompletionObservation ObservePendingDisplacementCompletion() const;

    DisplacementCompletionResult m_completedDisplacementUpdate{};

    // Per-request diagnostics reset only after a new request is accepted.
    std::uint64_t m_displacementDeferredFrames = 0;

    bool m_displacementCompletionObserved = false;
    bool m_displacementTerminalErrorLogged = false;
    // Fixed-size CPU diagnostics. No GPU timestamps or per-frame log allocation.
    // The measurement window runs from acceptance through successful finalization.
    struct DisplacementWallClockTiming
    {
        double diagnosticComputeMilliseconds = 0.0;
        double diagnosticBLASMilliseconds = 0.0;
        double diagnosticTLASMilliseconds = 0.0;

        CpuTiming::Clock::time_point accepted{};
        CpuTiming::Clock::time_point recordingStart{};
        CpuTiming::Clock::time_point computeEnd{};
        CpuTiming::Clock::time_point recordingEnd{};

        CpuTiming::Clock::time_point submissionReturned{};
        CpuTiming::Clock::time_point originalCompletionObserved{};
        CpuTiming::Clock::time_point firstDeferred{};
        CpuTiming::Clock::time_point finalized{};

        std::uint64_t acquiredFrames = 0;
        std::uint64_t blockingSlotWaits = 0;

        double acquisitionMilliseconds = 0.0;
        double fenceWaitMilliseconds = 0.0;
        double maxFenceWaitMilliseconds = 0.0;
    };

    DisplacementWallClockTiming m_displacementWallClock{};
    void LogDisplacementWallClockSummary(
        std::uint64_t revision,
        size_t laterSubmissions);
    void LogAsyncDisplacementError();
    // Pure value assembly only; upload and history mutation remain in RecordRenderFrame.
    // Pre-CAM.4 fly-camera matrices for frames without an explicit view.
    static RenderView DefaultFlyRenderView(const Camera& camera);
    // Identity of the last recorded view; a different identity on the next recording is a cut.
    std::optional<RenderViewIdentity> m_renderedViewIdentity;
    std::uint64_t m_cameraCutCount = 0;
    DirectionalLight m_recordedSun;
    std::uint32_t m_recordedAdditionalLightCount = 0;
    // Last recorded view and projection, reported through GetRecordedView/Projection.
    Matrix4 m_recordedView;
    Matrix4 m_recordedProjection;
    static CameraConstants BuildFrameCameraConstants(
        const Matrix4& currentView,
        const Matrix4& currentViewProjection,
        const Matrix4& previousView,
        const Matrix4& previousViewProjection,
        bool hasPreviousViewProjection);
    // Record against the acquired slot, between geometry drawing and EndGeometryPass.
    void RecordSceneSelectionMask(
        ID3D12GraphicsCommandList* commandList,
        const Scene& scene,
        const RendererOptions& options,
        D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress);
    // Upload failure is the only propagated AO error; pass-local skips remain local.
    bool RecordAmbientOcclusionPasses(
        ID3D12GraphicsCommandList* commandList,
        unsigned int frameSlot,
        const AOConstants& constants,
        const ShadowConstants& shadowConstants,
        unsigned int width,
        unsigned int height);
    bool RecordRenderFrame(
        const Scene& scene,
        const RendererOptions& options);

    // UNUSED. Single-threaded, outside command recording. Submitted must
    // describe this update's successful combined DIRECT submission, with
    // the failure gate armed before AS mutation. Output changes only on
    // success; caller owns future acknowledgment, never Renderer.
    // Future async integration must skip history reset/TLAS flag scheduling
    // already performed by submission-visible preparation. Unchanged for now.
    bool FinalizePendingDisplacementUpdate(DisplacementCompletionResult& result);
    // UNUSED. Selective retirement for covered frame-integrated updates only.
    // Later covered frames may remain queued: they consume the replacement.
    // No active/unsubmitted recording is permitted during this one-shot operation.
    bool FinalizeCoveredDisplacementUpdate(DisplacementCompletionResult& result);

    using SubmittedDisplacementBinding = RendererDisplacement::GeometryBinding;

    // UNUSED. Transient, borrowed binding for the exact pending Mesh only.
    // Valid only for command lists guaranteed to execute after the successful
    // update submission on this same DIRECT queue. Do not cache across record
    // resolution or scene lifetime changes. Completion-confirmed Mesh counts
    // and views remain untouched; this does not authorize pending rendering.
    // Output is unchanged on failure.
    bool TryGetSubmittedDisplacementBinding(
        const Mesh* mesh,
        SubmittedDisplacementBinding& binding) const;
    // UNUSED. Only for dependent commands after successful compute, BLAS,
    // and merged TLAS recording in this same active generation/slot. End,
    // abort, Execute and reset invalidate eligibility. This proves recording
    // order only, not successful submission or GPU completion. Mesh state
    // remains unchanged; output is unchanged on failure.
    bool TryGetRecordingDisplacementBinding(
        const Mesh* mesh,
        unsigned int frameSlot,
        SubmittedDisplacementBinding& binding) const;
    // Shared geometry validation/view construction only; callers separately
    // establish Recording or successfully Submitted provenance.
    bool TryBuildPendingDisplacementBinding(
        const Mesh* mesh,
        SubmittedDisplacementBinding& binding) const;

    enum class DrawGeometrySource
    {
        PublishedMesh,
        RecordingDisplacement,
        SubmittedDisplacement
    };

    struct DrawGeometryBinding
    {
        DrawGeometrySource source = DrawGeometrySource::PublishedMesh;
        // Published geometry delegates view binding to Mesh::Bind, preserving
        // its exact views and non-indexed behavior without reconstructing them.
        const Mesh* publishedMesh = nullptr;
        SubmittedDisplacementBinding displacement{};

        size_t vertexCount = 0;
        size_t indexCount = 0;

        bool indexed = false;
    };

    // Integration gate stays disabled until frame-integrated scheduling and
    // pending rendering are explicitly enabled. This never bypasses Render gates.
    static constexpr bool kEnablePendingDisplacementDrawBindings = kEnableFrameIntegratedDisplacement;
    // Pending views apply only to the owned active DIRECT recording. A matching
    // unresolved Mesh must validate its version or reject the draw, never fall
    // back to completion-confirmed counts. Output is unchanged on failure.
    bool TryResolveDrawGeometryBinding(
        ID3D12GraphicsCommandList* commandList,
        const Mesh& mesh,
        DrawGeometryBinding& binding) const;
    void BindDrawGeometry(
        ID3D12GraphicsCommandList* commandList,
        const DrawGeometryBinding& binding) const;
    // Zero/null until successful initialization; unique across Renderer
    // instances and reinitialization. Scene remains caller-owned.
    enum class SceneInitializationCommands
    {
        None,
        BeginAttempted,
        Recording,
        Recorded,
        CloseAttempted,
        Closed,
        SubmissionAttempted,
        Submitted,
        Complete,
        Abandoned
    };

    struct SceneInitializationAttempt
    {
        const Scene* candidate = nullptr;
        SceneInitializationCommands commands = SceneInitializationCommands::None;

        std::uint64_t recordingGeneration = 0;

        bool failed = false;
        bool gpuCompletionProven = false;
    };

    SceneInitializationAttempt m_sceneInitializationAttempt;

    std::uint64_t m_sceneResourceLifetime = 0;
    const Scene* m_sceneResourceScene = nullptr;

    CpuTiming::FrameSample m_cpuFrameTiming;

    CpuTiming::Clock::time_point m_previousRenderExecuteStart;

    bool m_hasPreviousRenderExecute = false;
    bool InitializeDevice();
    bool InitializeSwapChain(void* windowHandle);

    // Private resize stages: called only after Resize() confirms the queue is idle.
    bool ResizeApplicationResources(
        unsigned int width,
        unsigned int height);
    bool ResizeSceneResources(
        unsigned int width,
        unsigned int height);

    void Draw(
        ID3D12GraphicsCommandList* commandList,
        const RenderObject& object,
        D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress);
    void DrawSelectionMask(
        ID3D12GraphicsCommandList* commandList,
        const RenderObject& object,
        D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress);
    void DrawMaterialRange(ID3D12GraphicsCommandList* commandList, const RenderObject& object,
        Material& material, unsigned int firstIndex, unsigned int indexCount,
        D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress);
    bool ValidateSceneMaterialSlots(const Scene& scene, const std::vector<Material*>& materials) const;
    void BeginGeometryPass(ID3D12GraphicsCommandList* commandList);
    void EndGeometryPass(ID3D12GraphicsCommandList* commandList);
    bool InitializeMaterials(const std::vector<Material*>& materials);
    bool InitializeRayTracingScene(const Scene& scene);
    bool DiscoverEnvironments();
    bool PreprocessEnvironment(EnvironmentMap& environment);

    GraphicsDevice m_graphicsDevice;

    SwapChain m_swapChain;

    CommandContext m_commandContext;

    GpuTimestampProfiler m_gpuTimestampProfiler;

    uint64_t m_gpuProfilerFrameSerial = 0;

    ResourceUploader m_resourceUploader;

    DescriptorHeap m_resourceDescriptorHeap;
    DescriptorHeap m_rtvDescriptorHeap;
    DescriptorHeap m_dsvDescriptorHeap;

    DeferredLightingPass m_deferredLightingPass;

    ToneMapPass m_toneMapPass;
    EditorColliderPass m_editorColliderPass;
    EditorGridPass m_editorGridPass;

    BRDFIntegrationPass m_brdfIntegrationPass;

    GGXAverageAlbedoPass m_ggxAverageAlbedoPass;

    RayTracedShadowPass m_rayTracedShadowPass;

    RayTracedReflectionPass m_rayTracedReflectionPass;

    RayTracedReflectionTemporalPass m_rayTracedReflectionTemporalPass;

    RayTracedReflectionDenoisePass m_rayTracedReflectionDenoisePass;

    RayTracedGIPass m_rayTracedGIPass;

    RayTracedAOPass m_rayTracedAOPass;

    AOTemporalPass m_aoTemporalPass;

    AODenoisePass m_aoDenoisePass;

    AOResolvePass m_aoResolvePass;

    ReflectionContributionLimitPass m_reflectionContributionLimitPass;

    NrdDenoiser m_nrdReflectionDenoiser;
    NrdDenoiser m_nrdGIDenoiser;

    GISpatialDenoisePass m_giSpatialDenoisePass;

    GISignedCorrectionPass m_giSignedCorrectionPass;

    // RTXPT.1 reference path tracer. Created lazily on the first frame that requests it, so Legacy
    // frames and packaged runtimes never allocate it; resized with the scene; released at shutdown.
    PathTraceReferencePass m_pathTraceReferencePass;
    PathTraceReferenceStatus m_pathTraceStatus;

    // RTXPT.1A.2 bundled GGX single-scatter complement tables, required by the path tracer only. Loaded and
    // published once during Initialize; immutable afterwards; released in Shutdown after the final drain.
    MaterialTableResources m_materialTables;
    std::string m_materialTableUnavailableReason;
    void LoadMaterialTables(std::span<const std::byte> packagedMaterialTables);
    // Set by ResetSceneHistory (scene activation, material revision, HDRI, camera cut, generation).
    bool m_pathTraceResetPending = true;
    bool m_pathTraceRenderedLastFrame = false;

    // Everything the converged image depends on. Any difference restarts accumulation.
    struct PathTraceAccumulationKey
    {
        // Switching between the reference and raster-primary modes restarts accumulation.
        PathTracePrimary primary = PathTracePrimary::Camera;
        Matrix4 view;
        Matrix4 projection;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        PathTraceReferenceSettings settings;
        DirectionalLight sun;
        SceneLightConstants lights{};
        D3D12_GPU_VIRTUAL_ADDRESS topLevelAddress = 0;
        std::uint64_t topLevelRecording = 0;
        std::uint64_t materialRevision = 0;
        std::uint64_t sceneResourceLifetime = 0;
        unsigned int environmentTextureIndex = UINT_MAX;
        unsigned int environmentConditionalCDFIndex = UINT_MAX;
        float environmentTotalWeight = 0.0f;
    };

    std::optional<PathTraceAccumulationKey> m_pathTraceAccumulationKey;
    std::uint32_t m_pathTraceRandomSeed = 0;
    // Samples recorded by the current frame; added to the accumulated count only after the frame
    // records and submits successfully (Render's Rendered path).
    std::uint32_t m_pathTracePendingSamples = 0;

    // Returns true when the reference pass exists or was created now; records the failure otherwise.
    bool EnsurePathTraceReference();

    // RTXPT.2 raster-primary mode: whether its prerequisites hold, evaluated on frames that request it.
    // It owns no resources beyond its pipeline in the reference pass.
    PathTraceRasterPrimaryStatus m_rasterPrimaryStatus;
    void EvaluatePathTraceRasterPrimary();

    // Primary of the reference mode. Camera always, except in diagnostics that measure raster-primary
    // agreement against unjittered PixelCenter rays; the Editor never changes it.
    PathTracePrimary m_pathTraceReferencePrimary = PathTracePrimary::Camera;
    // Records one progressive iteration (or holds a converged image) and presents it as scene colour.
    bool RecordPathTraceReference(
        ID3D12GraphicsCommandList* commandList,
        unsigned int frameIndex,
        PathTracePrimary primary,
        const Matrix4& view,
        const Matrix4& projection,
        const Matrix4& inverseViewProjection,
        const Vector3& cameraPosition,
        const FrameLighting& frameLighting,
        D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstantsAddress,
        const PathTraceReferenceSettings& settings);
    static bool SamePathTraceAccumulationKey(const PathTraceAccumulationKey& left, const PathTraceAccumulationKey& right);

    bool m_giSignedCorrectionInitialized = false;
    bool m_signedCorrectionExecutedLastFrame = false;

    NrdPreparePass m_nrdReflectionPreparePass;
    NrdPreparePass m_nrdGIPreparePass;

    NrdReflectionResolvePass m_nrdReflectionResolvePass;

    NrdDiffuseResolvePass m_nrdDiffuseResolvePass;

    ShadowDenoisePass m_shadowDenoisePass;

    // Never recorded or bound until the future coordinator accepts it.
    struct SelectionMaskPipelineCandidate final : SelectionMaskPass
    {
        ~SelectionMaskPipelineCandidate() { Shutdown(); }
    };

    std::unique_ptr<SelectionMaskPipelineCandidate> m_selectionMaskCandidate;
    std::uint64_t m_selectionMaskCandidateLifetime = 0;
    void AdoptSelectionMaskCandidate() noexcept;

    SelectionMaskPass m_selectionMaskPass;

    DisplacementVerticesPass m_displacementVerticesPass;

    DisplacementIndicesPass m_displacementIndicesPass;

    DisplacementGPUPatchBuffer m_displacementGPUPatchBuffer;

    bool m_previousContributionLimiterEnabled = false;
    bool m_nrdExecutedLastFrame = false;
    bool m_nativeReflectionExecutedLastFrame = false;
    bool m_aoExecutedLastFrame = false;

    float m_previousAORadius = 1.5f;

    ShaderLibrary m_shaderLibrary;

    DebugUI m_debugUI;

    unsigned int m_applicationWidth = 0;
    unsigned int m_applicationHeight = 0;

    // Zero denotes an invalid/incomplete scene resource extent after resize failure.
    unsigned int m_sceneWidth = 0;
    unsigned int m_sceneHeight = 0;

    bool m_resizeFailed = false;

    unsigned int m_shadowSampleIndex = 0;
    unsigned int m_reflectionSampleIndex = 0;
    uint32_t m_nrdFrameIndex = 0;

    GBuffer m_gBuffer;

    TextureThumbnailCache m_textureThumbnails;
    MaterialPreviewRenderer m_materialPreviewRenderer;
    bool m_editorResourcesInitialized = false;

    UploadBuffer m_uploadBuffer;

    std::array<StructuredBuffer, CommandContext::FrameCount> m_materialBuffers;

    std::array<UploadBuffer, CommandContext::FrameCount> m_materialUpload;
    std::array<std::unique_ptr<MaterialTableReplacement>, CommandContext::FrameCount> m_materialReplacements;
    std::shared_ptr<MaterialAdmissionState> m_materialAdmission;
    std::weak_ptr<const ProjectMaterialPreparationService::Candidate> m_invalidatedMaterialCandidate;
    void ValidateMaterialAdmissionLifecycle();
    void InvalidateMaterialAdmission();
    friend struct MaterialAdmissionState;

    std::vector<const Material*> m_publishedMaterialIdentities;
    MaterialTablePublication<CommandContext::FrameCount> m_materialPublication;
    // Retain no more distinct dependencies than the resource heap can describe.
    MaterialDependencyRetention<Texture::Dependency, CommandContext::FrameCount, ResourceDescriptorCapacity>
        m_materialDependencies;
    struct TextureUploadEntry
    {
        std::uint64_t ticket = 0;

        std::unique_ptr<PreparedTextureUpload> upload;

        std::string error;
    };
    static constexpr size_t kTextureUploadCapacity = 8;
    static constexpr size_t kTextureUploadByteCapacity = 64 * 1024 * 1024;
    std::vector<TextureUploadEntry> m_preparedTextureUploads;
    void PollPreparedTextureUploads();
    void RecordPreparedTextureUploads(ID3D12GraphicsCommandList* commands);
    StructuredBuffer& CurrentMaterialBuffer();
    void ShutdownMaterialTables();
    bool RecordMaterialTable(unsigned int slot, ID3D12GraphicsCommandList* commandList);

    static constexpr unsigned int kDisplacementLODFrameCount = 2;

    unsigned int m_displacementLODDescriptorIndices[kDisplacementLODFrameCount] = {UINT_MAX, UINT_MAX};

    EnvironmentMap m_environment;
    std::string m_environmentIdentity;

    RenderTarget m_sceneColor;
    RenderTarget m_finalSceneColor;
    RenderTarget m_selectionMask;

    Texture m_brdfIntegrationLUT;
    Texture m_ggxAverageAlbedoLUT;

    RayTracingSceneResources m_rayTracingScene;

    bool m_geometryUpdateFailed = false;
    bool m_pendingGeometryTLASRefresh[2]{};

    Matrix4 m_previousViewProjection{};
    Matrix4 m_previousView{};

    bool m_hasPreviousViewProjection = false;

    Matrix4 m_previousNrdView{};
    Matrix4 m_previousNrdProjection{};

    bool m_hasPreviousNrdCamera = false;

    Vector3 m_previousShadowLightDirection{};

    bool m_hasPreviousShadowLightDirection = false;

    Vector3 m_previousGILightColor{};

    float m_previousGILightIntensity = 0.0f;

    bool m_hasPreviousGILight = false;

    SceneLightConstants m_previousGILocalLights{};
    bool m_hasPreviousGILocalLights = false;

    bool m_packagedRuntime = false;
    bool m_debugUIInitialized = false;
    bool m_commandContextInitialized = false;
    std::filesystem::path m_assetRoot;

    std::vector<EnvironmentAsset> m_availableEnvironments;

    size_t m_currentEnvironmentIndex = 0;

    RendererStatistics m_statistics{};
    RendererStatistics m_currentFrameStatistics{};

    struct PendingRendererStatistics
    {
        RendererStatistics statistics{};

        bool valid = false;
    };

    PendingRendererStatistics m_pendingStatistics[GpuTimestampProfiler::FrameCount]{};
};
