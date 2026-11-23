#pragma once
#include "Renderer/Passes/EditorColliderPass.h"
#include "Renderer/Passes/EditorGridPass.h"
#include "Renderer/Passes/EditorVoxelPass.h"
#include "Renderer/Passes/EditorAirPass.h"
#include "Renderer/Passes/EditorMistPass.h"
#include "Renderer/VoxelResidencyResources.h"
#include "Renderer/AirField.h"
#include "Renderer/Passes/PsrInPlaceReplacePass.h"
#include "Renderer/PsrPipelinePreparation.h"
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
#include "ReferenceRayCounts.h"
#include "SceneLightBuffer.h"
#include "LightSampling/LightSamplingBuffer.h"
#include "LightSampling/LightSamplingData.h"
#include "LightSampling/ReGIRPresampling.h"
#include "ComputePipeline.h"
#include "DescriptorHeap.h"
#include "GraphicsDevice.h"
#include "GpuTimestampProfiler.h"
#include "RenderObject.h"
#include "RendererStatistics.h"
#include "SceneLightingEpoch.h"
#include "RadianceCache/RadianceCacheD6.h"
#include "Sharc/SharcCache.h"
#include "ResourceUploader.h"
#include "SwapChain.h"
#include "EnvironmentMap.h"
#include "EnvironmentPreparation.h"
#include "RenderTarget.h"
#include "DisplacementGPUPatchBuffer.h"
#include "Displacement/DisplacementUpdateState.h"
#include "Displacement/DisplacementGeometryBinding.h"

#include "Debug/DebugUI.h"
#include "RenderingConfiguration.h"
#include "ConfigurationPreparation.h"
#include "Diagnostics/ConfigurationReadiness.h"

#include "Math/Vector3.h"
#include "Math/Matrix4.h"
#include "Scene/RenderView.h"
#include "Scene/SceneLighting.h"

#include "Shader/ShaderLibrary.h"

#include "RayTracing/RayTracingSceneResources.h"

#include "Passes/ToneMapPass.h"
#include "Passes/PresentationResolvePass.h"
#include "Passes/DlssInputPreparationPass.h"
#include "Passes/RrInputPreparationPass.h"
#include "Passes/ReSTIRPTPass.h"
#include "Passes/AutoExposurePass.h"
#include "Upscaling/DlssBackend.h"
#include "Passes/DeferredLightingPass.h"
#include "Passes/BRDFIntegrationPass.h"
#include "Passes/GGXAverageAlbedoPass.h"
#include "Passes/RayTracedShadowPass.h"
#include "LegacyLightingResources.h"
#include "Passes/RayTracedReflectionPass.h"
#include "Passes/ShadowDenoisePass.h"
#include "Passes/RayTracedReflectionTemporalPass.h"
#include "Passes/RayTracedReflectionDenoisePass.h"
#include "Passes/ReflectionContributionLimitPass.h"
#include "Passes/NrdDenoiser.h"
#include "Passes/NrdPreparePass.h"
#include "Passes/NrdPathTracePreparePass.h"
#include "Passes/NrdHistoryConfidencePass.h"
#include "Passes/NrdReflectionResolvePass.h"
#include "Passes/SelectionMaskPass.h"
#include "Passes/RayTracedGIPass.h"
#include "Passes/RayTracedAOPass.h"
#include "Passes/AOTemporalPass.h"
#include "Passes/AODenoisePass.h"
#include "Passes/AOResolvePass.h"
#include "Passes/NrdDiffuseResolvePass.h"
#include "Passes/PathTraceReferencePass.h"
#include "Passes/PathTraceSpatialPass.h"
#include "Passes/PathTraceTemporalPass.h"
#include "PreviousGBufferHistory.h"
#include "Rtxdi/RtxdiResources.h"
#include "MaterialTables/MaterialTableResources.h"
#include "RendererLightingMode.h"
#include "LightingModeTransition.h"
#include "Passes/GISpatialDenoisePass.h"
#include "Passes/GISignedCorrectionPass.h"
#include "Passes/DisplacementVerticesPass.h"
#include "Passes/DisplacementIndicesPass.h"

#include "Geometry/DisplacementMeshGenerator.h"

#include <d3d12.h>
#include <filesystem>
#include <functional>
#include <future>
#include <span>
#include <string>
#include <vector>
#include <array>
#include <optional>
#include <memory>
#include <cstddef>
#include <limits>

struct IDXGIAdapter3;
#include <climits>
#include "Debug/MeshThumbnailDiagnostics.h"

struct EnvironmentAsset
{
    std::string displayName;

    std::filesystem::path relativePath;
    std::string reference;
};

struct RendererOptions
{
    const EditorColliderOverlay* editorColliderOverlay = nullptr; // Borrowed for this Render call only.
    // VX.4C.2 voxel debug overlay; drawn only while its generation names the published data. Borrowed for this call.
    const EditorVoxelOverlay::Request* editorVoxelOverlay = nullptr;
    // AIR.1: the camera-centred air field; absent or disabled means no air work (the air binding is then invalid).
    std::optional<AirFrameRequest> air;
    // AIR.4A: the Editor's velocity vectors, drawn in Editor and Play frames (never in a packaged runtime).
    std::optional<AirVisualizationRequest> airVisualization;
    // AIR.4F F4.5.1b.1: the Editor's temperature points, drawn like the vectors (before them).
    std::optional<AirTemperatureRequest> airTemperature;
    // AIR.4E.2: the Editor's divergence points, drawn like the temperature points (after them); absent frames release their storage.
    std::optional<AirDivergenceRequest> airDivergence;
    // AIR.4F F3: the Editor's volumetric mist, drawn like the vectors (before them) while the air field holds mist.
    std::optional<AirMistRenderRequest> airMist;
    // AIR.4C: the Editor's air-cell diagnostic (blocked and unknown cells of the obstacle mask), drawn like the vectors.
    std::optional<AirCellRequest> airCells;
    // Editor viewport grid for this frame; absent in Play mode, with the Overlay off and in runtime.
    std::optional<EditorGridRequest> editorGrid;

    std::optional<MaterialPreviewRequest> materialPreview;

    // Borrowed only for this Render call; Editor retains resources through GPU completion.
    std::vector<Texture*> editorTextures;

    size_t selectedObject = (std::numeric_limits<size_t>::max)();
    std::vector<size_t> selectedObjects;

    Vector3 selectionOutlineColor{1.0f, 0.72f, 0.0f};
    unsigned int selectionOutlineThickness = 2;
    // Final display tone mapper only; HDR lighting and accumulation are unaffected, so switching never resets them.
    ToneMapOperator toneMapOperator = ToneMapOperator::Existing;

    bool enableRTGI = true;

    float rtgiContactAOStrength = 1.0f;
    float rtgiContactAORadius = 2.5f;

    // Legacy reflection denoising (PS.2B: formerly read from the Debug panel by the renderer).
    ReflectionDenoiserMode reflectionDenoiser = ReflectionDenoiserMode::Nrd;
    bool contributionLimiter = true;

    // PSR.2 development view; None when Off. Applied only to Path Tracing frames with in-place replacement.
    ToneMapDebugView psrDebugView = ToneMapDebugView::None;

    // RTXPT D.3B.0 path-trace denoiser diagnostic view (Normal: the production presentation).
    PathTraceDenoiserVisualization pathTraceDenoiserVisualization{};

    // Checked view for this frame (BuildFlyRenderView/ResolveMainCameraView). Absent: the scene's
    // Editor fly camera, unchanged. Copied by value; the renderer retains only its identity.
    std::optional<RenderView> view;

    // LIGHT.1 scene lighting captured for this frame (FileSceneRuntime::CaptureSceneLighting).
    // Absent: procedural demos use their explicit sun; object-owned file scenes stay dark. Copied by value.
    std::optional<SceneLightingSnapshot> lighting;

    // M3.5: the same, shared (FileSceneRuntime::CaptureSceneLightingShared). When set it is used instead of `lighting`, and
    // an unchanged snapshot revision lets the renderer reuse its frame lighting and skip republishing the light table.
    std::shared_ptr<const SceneLightingSnapshot> sharedLighting;

    // The frame's snapshot: sharedLighting, else lighting, else none.
    const SceneLightingSnapshot* GetLighting() const noexcept
    {
        return sharedLighting ? sharedLighting.get() : (lighting ? &*lighting : nullptr);
    }

    // RTXPT.1 experimental lighting mode for this frame only. Legacy is the default; the selection is
    // never persisted. An unavailable reference mode renders Legacy and reports why.
    RendererLightingMode lightingMode = RendererLightingMode::Legacy;
    PathTraceReferenceSettings pathTraceReference;

    // SHARC.4C experimental D6 radiance cache, default off. On: its GPU resources exist and their generation follows
    // the scene-lighting epoch on path-traced frames. No rendering stage reads or writes the cache yet, so the image is
    // unchanged. Off: no allocation and no dispatch. Unsupported in the packaged runtime.
    bool experimentalD6RadianceCache = false;

    // SHARC.4E experimental cached-radiance rendering, default off (it also enables the D6 cache). Off: the reference
    // pipeline, with no SHARC dispatch, lookup or allocation.
    SharcSettings sharc;

    // SHARC.4E.1: the path-tracing denoiser (RTXPT D.1B signal extraction, D.2B temporal reconstruction, D.3B spatial
    // filtering) on Path Traced Raster Primary frames (with ReSTIR DI too since SHARC.5B), with or without SHARC. The denoised result is presented in place
    // of the progressive mean; the accumulation itself is unchanged. Off here; the Editor selects Engine by default.
    // NRD.2: a mode (PathTraceDenoiserMode); every value is implemented since HYBRID.5.
    PathTraceDenoiserMode pathTraceDenoiserMode = PathTraceDenoiserMode::Off;
    // DENOISE.1 diagnostics: which denoiser stages run (defaults: the production pipeline). Applied when it changes.
    PathTraceDenoiserStages pathTraceDenoiserStages;
    // UPSCALING.2B diagnostics: sub-pixel camera jitter on Path Traced Raster Primary frames (with ReSTIR DI too). Off
    // by default: the unjittered path, unchanged. Not saved.
    bool pathTraceCameraJitter = false;
    // UPSCALING.3B diagnostics: prepare the DLSS inputs (DLSS motion vectors with sky motion, exposure texture, the
    // DLSS jitter phase count, the per-frame upscaler input contract) without evaluating DLSS. Off by default: no pass,
    // no allocation, unchanged rendering. Not saved.
    bool dlssInputPreparation = false;
    // Seconds since the previous frame, for exposure adaptation (0 holds the adapted exposure).
    float frameDeltaSeconds = 0.0f;
    // Snap automatic exposure to its target this frame (scene activation); camera cuts and reference-exposure
    // changes snap on their own.
    bool exposureReset = false;
};

// UPSCALING.3B: Engine's per-frame upscaler input contract, filled on frames that prepare DLSS inputs. Everything a
// temporal upscaler (DLSS now, Engine's own later) consumes at the render extent. The resources are borrowed from the
// renderer and valid until the next scene resize or shutdown; nothing here is DLSS-specific except dlssMotionVectors
// and exposure, which DlssInputPreparationPass derives.
struct UpscalerFrameInputs
{
    // Whether this frame recorded the preparation (all fields below describe that frame).
    bool prepared = false;
    unsigned int renderWidth = 0;
    unsigned int renderHeight = 0;
    unsigned int outputWidth = 0;
    unsigned int outputHeight = 0;
    // Linear HDR scene colour at the render extent (RGBA16F, before tone mapping; alpha unused).
    RenderTarget* color = nullptr;
    // Whether color is this frame's per-frame denoised path-traced image (temporal + spatial denoiser ran, samples were
    // traced this frame), the source the vendor upscaler must use. False for held, undenoised or Legacy frames.
    bool colorIsPerFrameDenoised = false;
    // Raster depth at the render extent (D32, near 0 / far 1, sky 1; jittered like the G-buffer).
    DepthBuffer* depth = nullptr;
    // Engine's motion (RGBA16F: xy currentUV - previousUV, unjittered; z view-depth change; w material AO).
    RenderTarget* engineMotion = nullptr;
    // DLSS motion (RG16F: previous - current pixel position, render pixels, unjittered, sky included).
    Texture* dlssMotionVectors = nullptr;
    // 1x1 R16F holding the tone mapper's exposure multiplier, written on the GPU from the exposure state.
    Texture* exposure = nullptr;
    // This frame's jitter (render pixels; CameraJitterStatus convention).
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    std::uint32_t jitterPhaseCount = 0;
    // A history discontinuity since the previous prepared frame (scene activation, material revision, HDRI change,
    // camera cut, resize, mode or option change): DLSS InReset. Camera motion alone never sets it.
    bool reset = false;
};

// UPSCALING.2B: the camera jitter of the most recently recorded frame. The offset is where the frame sampled the scene
// relative to each pixel centre, in render pixels (x right, y down), each component in [-0.5, 0.5).
struct CameraJitterStatus
{
    bool active = false;
    // Index into the Halton(2, 3) sequence, in [0, phaseCount).
    std::uint32_t phase = 0;
    // Renderer::CameraJitterPhaseCount, or the DLSS phase count while DLSS input preparation is on (UPSCALING.3B).
    std::uint32_t phaseCount = 0;
    float x = 0.0f;
    float y = 0.0f;
};

// Evidence of the reference path tracer for the Editor and diagnostics. Values only.
struct PathTraceReferenceStatus
{
    PathTraceReferenceAvailability availability = PathTraceReferenceAvailability::NotRequested;
    std::string unavailableReason;

    // True when the most recent recorded frame rendered with the reference mode.
    bool activeLastFrame = false;
    bool secondaryShadowRayQueryReady = false;
    bool secondaryShadowRayQueryActive = false;
    std::uint64_t visibilityTimingEpoch = 0;
    std::uint32_t accumulatedSamples = 0;
    std::uint64_t accumulationResets = 0;
    std::uint64_t resourceBytes = 0;
};

// RTXPT.2 R4: the camera of the frame a previous G-buffer slot was copied from. The inverse is the
// camera-relative one the raster primary uses (the view without its translation), so reconstruction
// stays precise far from the world origin; world position = cameraPosition + reconstructed offset.
struct PreviousGBufferCamera
{
    Matrix4 view;
    Matrix4 projection;
    Matrix4 inverseCameraRelativeViewProjection;
    Vector3 position;
};

// RTXPT.2 R4: the previous frame's G-buffer as path-traced consumers may read it. Only handed out while
// valid; the SRVs are in the resource descriptor heap and in PIXEL|NON_PIXEL_SHADER_RESOURCE state.
struct PreviousGBufferView
{
    unsigned int depthIndex = 0;
    unsigned int surfaceIndex = 0;
    unsigned int geometricNormalIndex = 0;
    unsigned int materialIndex = 0;
    PreviousGBufferCamera camera;
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
        std::span<const std::byte> packagedMaterialTables = {}, const RenderingConfiguration* initialConfiguration = nullptr,
        // M0c.5 Stage 5.1: the Project Browser initializes only the core (device, shader library, heaps, ImGui, swap
        // chain); EnsureSceneRenderer initializes the rest when a project is selected.
        bool deferSceneRenderer = false,
        // M0c.5 Stages 5.2/5.3: a Project Browser workflow start (a Browser selection or a command-line project). Its renderer
        // bootstraps with the 16x8 neutral environment instead of the first .hdr, and creates AIR, its views, the mist pass and
        // the Legacy passes on first use. Packaged, developer and verification starts create them eagerly, as before.
        bool projectWorkflowStart = false);
    // M0c.5 Stage 5.1: initializes the scene renderer deferred by Initialize, between frames. Idempotent once it
    // succeeds; a failure is final (Shutdown releases the partial group). Required before any scene, configuration
    // preparation or Editor resources.
    bool EnsureSceneRenderer();
    bool IsSceneRendererInitialized() const noexcept { return m_sceneRendererState == SceneRendererState::Initialized; }
    // M0c.5 Stage 5.1: initializes NGX once, on the main thread between frames, when DLSS is requested. Called at project
    // initialization (before the first frame sizes the scene) and by SetRenderScale. Failure is reported, never fatal.
    void EnsureDlssBackend(bool dlssRequested);
    // M0c.5 Stage 5.5: starts NGX's initialization on its own thread (DlssBackend::BeginInitialize) when DLSS is requested and
    // NGX is neither initialized nor initializing. EnsureDlssBackend (the first frame that sizes the scene for DLSS) and
    // configuration preparation (PollDlssBackend) complete it; nothing calls NGX meanwhile.
    void BeginDlssBackend(bool dlssRequested);
    // Adopts a finished initialization without waiting. True when none is pending.
    bool PollDlssBackend();
    // M0c.5 Stage 5.5: between InitializeProject's phases, polls the pending configuration's preparation as a frame would
    // (accepts finished libraries, queues their DXR pipelines), without presenting or switching anything.
    void AdvanceConfigurationPreparation();
    // First project selection only for interactive startup; legacy initialization calls this
    // eagerly. Idempotent, with no project asset requests or scene recording. Requires the scene renderer.
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
    // Its extent is the output extent.
    [[nodiscard]] bool TryGetFinalSceneImage(FinalSceneImage& image) const;

    // UPSCALING.2A: the scene's two extents. Output is the extent ResizeScene requests and the final scene image has;
    // render is the extent every scene pass, history and dispatch is sized from. They are equal until an upscaler
    // exists. All zero before initialization and while a scene resize is incomplete or failed.
    struct SceneResolution
    {
        unsigned int renderWidth = 0;
        unsigned int renderHeight = 0;
        unsigned int outputWidth = 0;
        unsigned int outputHeight = 0;
    };

    SceneResolution GetSceneResolution() const noexcept
    {
        return {m_sceneWidth, m_sceneHeight, m_outputWidth, m_outputHeight};
    }

    // UPSCALING.2C experimental render scale. Diagnostic75 renders the scene at 75% of the output extent per axis and
    // presents it through the diagnostic bilinear resolve (not an upscaler). UPSCALING.3C Dlss75: the same extents,
    // reconstructed by DLSS Super Resolution on raster-primary path-traced frames whenever DLSS can run (otherwise the
    // diagnostic resolve, reported as inactive). UPSCALING.3D DlssQuality/Balanced/Performance: the same, at the render
    // extent NGX reports optimal for that preset and the output extent (75% when NGX reports none; DLSS then inactive).
    enum class RenderScale
    {
        Native,
        Diagnostic75,
        Dlss75,
        DlssQuality,
        DlssBalanced,
        DlssPerformance
    };

    static constexpr bool IsDlssRenderScale(RenderScale scale) noexcept
    {
        return scale == RenderScale::Dlss75 || scale == RenderScale::DlssQuality || scale == RenderScale::DlssBalanced ||
            scale == RenderScale::DlssPerformance;
    }

    // UPSCALING.3D: the NGX preset of a preset scale (none for the diagnostic Dlss75 and the non-DLSS scales).
    static std::optional<DlssQualityPreset> GetDlssPreset(RenderScale scale) noexcept;

    // Takes effect at the next ResizeScene, which recreates the extent-sized resources only when the render extent
    // changes. Set it before ResizeScene, outside any frame recording.
    void SetRenderScale(RenderScale scale, bool eligible = true, bool retry = false, bool normalRequest = false);
    // RR.1C: a latched Ray Reconstruction failure is a pending fallback too.
    bool HasPendingDlssFallback() const noexcept
    {
        return (!m_dlssFailure.empty() && IsDlssRenderScale(m_renderScale)) ||
            (!m_rrFailure.empty() && m_rayReconstructionEffective);
    }
    RenderScale GetRenderScale() const noexcept { return m_renderScale; }

    // UPSCALING.2B: length of the camera jitter sequence (Halton(2, 3), indices 1..8).
    static constexpr std::uint32_t CameraJitterPhaseCount = 8;

    // UPSCALING.3B: the DLSS jitter phase count for a render/output extent pair: max(16, round(8 * output pixels /
    // render pixels)) (DLSS Programming Guide 3.7.1.1 formula and table; 2.2 asks for at least 16).
    static std::uint32_t DlssJitterPhaseCount(unsigned int renderWidth, unsigned int renderHeight,
        unsigned int outputWidth, unsigned int outputHeight) noexcept;

    // UPSCALING.3B: the last prepared frame's upscaler inputs (prepared = false unless preparation is on).
    const UpscalerFrameInputs& GetUpscalerInputs() const noexcept { return m_upscalerInputs; }

    // UPSCALING.3B/3C: how the last frame was presented.
    const char* GetPresentationModeName() const noexcept
    {
        // RR.1C: the experimental Ray Reconstruction path, at DLAA or a preset.
        if(m_rayReconstructionEffective)
        {
            if(!m_rrEvaluatedLastFrame)
            {
                return "DLSS Ray Reconstruction (inactive)";
            }

            return m_renderScale == RenderScale::Native ? "DLSS Ray Reconstruction DLAA" : "DLSS Ray Reconstruction";
        }

        if(!PresentationResolveActive())
        {
            return "Native";
        }

        switch(m_renderScale)
        {
        case RenderScale::Dlss75:
            return m_dlssActiveLastFrame ? "DLSS 75% (diagnostic)" : "Diagnostic Bilinear (DLSS inactive)";
        case RenderScale::DlssQuality:
            return m_dlssActiveLastFrame ? "DLSS Quality" : "Diagnostic Bilinear (DLSS Quality inactive)";
        case RenderScale::DlssBalanced:
            return m_dlssActiveLastFrame ? "DLSS Balanced" : "Diagnostic Bilinear (DLSS Balanced inactive)";
        case RenderScale::DlssPerformance:
            return m_dlssActiveLastFrame ? "DLSS Performance" : "Diagnostic Bilinear (DLSS Performance inactive)";
        default:
            return "Diagnostic Bilinear";
        }
    }

    // UPSCALING.3C: whether the last frame was reconstructed by DLSS, and why not when DLSS was requested.
    bool IsDlssActiveLastFrame() const noexcept { return m_dlssActiveLastFrame; }
    const char* GetDlssInactiveReason() const noexcept { return m_dlssInactiveReason; }
    // PS.5E: the scheduled Native fallback's stage and detail; empty while DLSS has not failed.
    const std::string& GetDlssFailure() const noexcept { return m_dlssFailure; }
    const DlssFeatureStatus& GetDlssFeatureStatus() const noexcept { return m_dlssBackend.GetFeatureStatus(); }

    // UPSCALING.3C: NGX InJitterOffset = DlssJitterSign * CameraJitterStatus (per axis). Verified by the jitter-sign
    // test (build/upscaling-3c/REPORT.md section 4).
    static constexpr float DlssJitterSignX = -1.0f;
    static constexpr float DlssJitterSignY = -1.0f;

    const CameraJitterStatus& GetCameraJitterStatus() const noexcept { return m_cameraJitterStatus; }

    // UPSCALING.3A: what the DLSS Super Resolution backend found at initialization (informational).
    const DlssStatus& GetDlssStatus() const noexcept { return m_dlssBackend.GetStatus(); }

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
    // PS.5C: the last recorded frame's PSR: whether it was requested, the replacement chain that actually ran (0: none)
    // and why the requested PSR lighting is unavailable (empty while it runs). The same values DebugUI shows.
    struct PsrStatus
    {
        bool requested = false;
        std::uint32_t activeChain = 0;
        std::string unavailable;
    };
    const PsrStatus& GetPsrStatus() const noexcept { return m_psrStatus; }
    // SHARC.4E: the cache's statistics for the Editor (two frames old; valid only with SharcSettings::statistics).
    const SharcStatus& GetSharcStatus() const noexcept { return m_sharcStatus; }
    // The shared raster-primary prerequisites (PathTracedRasterPrimary), ReSTIR DI's effective availability, and both.
    const PathTraceModeAvailability& GetPathTraceRasterPrimaryStatus() const noexcept
    {
        return m_rasterPrimaryStatus.rasterPrimary;
    }
    PathTraceModeAvailability GetPathTraceReSTIRDIStatus() const { return GetReSTIRDIAvailability(m_rasterPrimaryStatus); }
    const PathTraceRasterPrimaryStatus& GetPathTraceRasterPrimaryCapability() const noexcept
    {
        return m_rasterPrimaryStatus;
    }
    // ER.1 Phase 2: why the selected path-tracing denoiser is not active yet; empty otherwise.
    const std::string& GetDenoiserPreparation() const noexcept { return m_denoiserPreparation; }
    // PS.5B: the effective path-tracing denoiser, and why it cannot denoise (empty while it does): a creation failure
    // of the Engine denoiser's passes, which every denoiser presents through or falls back to, or the NRD fallback.
    PathTraceDenoiserMode GetPathTraceDenoiserMode() const noexcept { return m_pathTraceDenoiserMode; }
    std::string GetPathTraceDenoiserFailure() const;
    // Async DXR preparation: requested versus active lighting mode and the real preparation stage.
    const LightingModeTransitionStatus& GetLightingModeTransitionStatus() const noexcept
    {
        return m_lightingModeTransition.GetStatus();
    }

    // M0c.4 Stage 4.0 (diagnostics, RendererReadiness.cpp): whether the complete requested configuration is ready at this
    // output extent, observed without creating, compiling, allocating, waiting or changing any state; and what the last
    // recorded frame rendered.
    ConfigurationReadiness EvaluateConfigurationReadiness(const RenderingConfiguration& configuration, unsigned int outputWidth,
        unsigned int outputHeight) const;
    RenderedConfiguration GetRenderedConfiguration() const;

    // M0c.4 Stage 4.1/4.2 (RendererConfigurationPreparation.cpp): the configuration to present this frame. Called once per
    // frame before the render scale is applied. While the request's identity (ConfigurationIdentity) differs from the
    // presented one, the presented configuration is returned unchanged and the request is prepared in the background;
    // once everything it needs is ready, the request is returned and becomes presented. Never blocks.
    const RenderingConfiguration& SelectPresentedConfiguration(const RenderingConfiguration& requested);
    // A later request in the same frame (the Editor's settings window draws after the scale is applied): returned when its
    // identity is the one selected this frame (its per-frame values apply now), otherwise the selected configuration
    // (the new identity is prepared from the next frame on).
    RenderingConfiguration PresentConfiguration(const RenderingConfiguration& latest) const;
    // DLSS eligibility of the presented configuration: Path Tracing with a denoiser, prepared and switching or active; while
    // the first request prepares at startup, the request's own eligibility (its render extent is set up once).
    bool IsPresentedDlssEligible() const;
    // Stage 4.4: starts preparing a configuration as soon as it is known, before the first frame (packaged preflight, a
    // command-line project, a Project Browser selection). Equivalent to the first frame's SelectPresentedConfiguration.
    void BeginConfigurationPreparation(const RenderingConfiguration& requested);
    // Stage 4.3: the first request is still preparing: frames record no lighting (no Ray Tracing, no path tracing) and
    // present the preparation screen; uploads, residency, acceleration structures and AIR continue.
    bool IsLightingSuppressed() const noexcept;
    const ConfigurationPreparationStatus& GetConfigurationPreparationStatus() const noexcept
    {
        return m_configurationPreparation.GetStatus();
    }

    // Whether any configuration has been selected yet (BeginConfigurationPreparation or a frame).
    bool HasPresentedConfiguration() const noexcept
    {
        return m_configurationPreparation.HasPresented();
    }

    // BT.3, read-only: the DXR workers' activity (zero when a worker does not exist), and what the last Render's
    // preparation gates concluded. Render thread only.
    void GetDxrWorkerActivity(AsyncWorkerActivity& libraries, AsyncWorkerActivity& pipelines) const;

    const DxrPreparationFrameReport& GetDxrPreparationFrameReport() const noexcept
    {
        return m_dxrPreparationReport;
    }

    // RTXPT.2 R4: false, leaving the view untouched, unless the previous PT frame's copy was submitted and
    // no invalidation has happened since.
    bool TryGetPreviousGBuffer(PreviousGBufferView& view) const;

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

    // O(1) display of a resident saved-thumbnail image by its cache handle; see MaterialPreviewRenderer.
    MaterialPreviewRenderer::ResidentThumbnail PinResidentMaterialThumbnail(size_t slot, std::uint64_t serial,
        D3D12_GPU_DESCRIPTOR_HANDLE& srv)
    {
        return m_materialPreviewRenderer.PinResidentThumbnail(m_resourceDescriptorHeap, slot, serial, srv);
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
        auto& diagnostics = MeshThumbnailDiagnostics::Get();
        MeshThumbnailDiagnostics::Timer pollTimer(diagnostics.pollMicroseconds);
        MeshThumbnailDiagnostics::Add(diagnostics.polls);
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
        MeshThumbnailDiagnostics::Add(MeshThumbnailDiagnostics::Get().browserDiscards);
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


    // W6.4: whether the stable BLAS registry already holds this mesh (a residency batch builds a BLAS for each mesh it lacks).
    bool HasStableBLAS(const Mesh* mesh) const
    {
        return m_rayTracingScene.m_blasRegistry.meshToBottomLevel.contains(mesh);
    }

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
    // SHARC.4B: records a radiance-affecting change for the next scene-lighting latch. History-only resets
    // (camera cuts, leaving path tracing) must not call this.
    void InvalidateSceneLighting(SceneLightingInvalidation reason) noexcept;
    // The scene-lighting epoch of the latest path-traced frame recording (SceneLightingEpoch.h).
    const SceneLightingEpoch::Status& GetSceneLightingEpoch() const noexcept;

    // Nonblocking editor resource retirement; valid only within this Renderer lifetime.
    std::uint64_t GetEditorCompletedFence() const;
    std::uint64_t GetEditorSubmissionFence() const;

    // VX.4C.2: replaces the Editor voxel overlay data (synchronous upload). Call between frames, never while recording.
    // Returns the new generation for EditorVoxelOverlay::Request, or 0 on failure (the previous data stays current).
    std::uint64_t PublishEditorVoxelOverlay(const EditorVoxelOverlay::Data& data, std::string* error);
    // Drops the overlay data; its buffers are released once the GPU completes every frame that read them.
    void ClearEditorVoxelOverlay();
    std::size_t GetEditorVoxelRetiredBufferCount() const noexcept
    {
        return m_editorVoxelPass.GetRetiredCount();
    }

    // VX.5 static voxel obstacle residency (VoxelResidencyResources.h). Main thread, between frames; Render records the uploads
    // at the start of its command list and publishes the table once every region it names is uploaded.
    bool ConfigureVoxelResidency(std::uint32_t regionBricks, int baseExponent, std::uint32_t window, std::string* error);
    void ResetVoxelResidency();
    std::optional<std::uint32_t> AddVoxelResidencyRegion(const Voxel::Residency::PackedRegion& region);
    void RemoveVoxelResidencyRegion(std::uint32_t slot);
    void SetVoxelResidencyTable(std::vector<Voxel::Residency::TableEntry> table);
    std::vector<std::uint32_t> TakeUploadedVoxelResidencySlots();
    // The binding this frame's consumers read; `valid` is 0 until a table is published (every query then answers unknown).
    const VoxelWorldBinding& GetVoxelWorldBinding() const noexcept
    {
        return m_voxelResidency.GetBinding();
    }

    // R1.4c diagnostics: chunk, page, slot, retirement and upload counters of the residency storage (read-only).
    VoxelResidencyResources::Statistics GetVoxelResidencyStatistics() const
    {
        return m_voxelResidency.GetStatistics();
    }

    // AIR.1 air field (AirField.h). Reset re-initializes the whole field on the next enabled frame (scene change); Release frees
    // its storage once the GPU is done with it.
    void ResetAirField() noexcept
    {
        m_airField.ResetField();
    }

    void ReleaseAirField();

    // R1.5b (decisions D4, D5): a packaged game whose cooked scene requests AIR initializes it here, before its first AIR frame: the air
    // field's pipelines, the pressure solver's (eagerly), the mist noise volumes from their archive bytes and the mist's Original
    // Raymarch stages. Main thread, after Initialize, between frames. Allocates no field storage (that waits for the first enabled
    // frame). Idempotent; false when a pipeline cannot be created (the field then stays uninitialized and records nothing).
    struct PackagedAirInitialization
    {
        double milliseconds = 0.0;
        int noiseVolumes = 0;
        bool pressurePipelines = false;
        bool mistPipelines = false;
    };

    bool InitializePackagedAir(std::span<const std::byte> worley, std::span<const std::byte> curl, std::span<const std::byte> filaments,
        PackagedAirInitialization* report = nullptr);
    bool IsAirFieldInitialized() const noexcept
    {
        return m_airField.IsInitialized();
    }

    // AIR.4F: zeroes all mist on the next frame with mist.
    void ClearAirMist() noexcept
    {
        m_airField.ClearMist();
    }
    // Valid after an enabled frame recorded its update; consumers in that frame read it.
    const AirFieldBinding& GetAirFieldBinding() const noexcept
    {
        return m_airField.GetBinding();
    }

    // AIR.4D D2: the latest Air Motor emission status, by AirMotorInstance::tag (a few frames old; empty while air is off).
    // AIR.4E.5b.1: the fixed air steps the latest frame recorded.
    std::uint32_t GetAirStepCount() const noexcept
    {
        return m_airField.GetLastStepCount();
    }

    const std::map<std::uint64_t, AirMotorEmissionStatus>& GetAirMotorEmission() const noexcept
    {
        return m_airField.GetEmissionStatus();
    }

    // AIR.4F F2: the latest Mist Emitter status, by MistEmitterInstance::tag (a few frames old; empty while mist is off).
    const std::map<std::uint64_t, MistEmitterStatus>& GetMistEmitterStatus() const noexcept
    {
        return m_airField.GetMistEmitterStatus();
    }

    // AIR.4F.2b: the latest frame's motors (by tag) that reached the air window but found every motor slot taken.
    const std::vector<std::uint64_t>& GetAirMotorsWithoutCapacity() const noexcept
    {
        return m_airField.GetMotorsWithoutCapacity();
    }

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

    // radianceScale: EnvironmentRadianceMultiplier of the scene the environment lights. It is part of the
    // environment's identity, so a calibration change rebuilds the environment.
    bool SetEnvironment(size_t index, float radianceScale);
    bool SetEnvironmentImage(const HDRImageData*, std::string identity, std::string displayName, std::string reference = {},
        float radianceScale = 1.0f);
    const std::string& GetEnvironmentIdentity() const noexcept { return m_environmentIdentity; }

    // S0.5 asynchronous environment replacement (see EnvironmentPreparation.h). Rendering keeps the published environment
    // until the requested one is fully prepared on the GPU; it then becomes active at the start of a frame. Only the newest
    // request can publish: every request supersedes the previous one, and synchronous SetEnvironment/SetEnvironmentImage
    // and CancelEnvironmentPreparation cancel any in flight. Returns the request's ticket (never 0).
    std::uint64_t RequestEnvironment(EnvironmentPreparationSource source);
    enum class EnvironmentPreparationState
    {
        Unknown,
        Pending,
        Published,
        Failed,
        Superseded,
    };
    EnvironmentPreparationState QueryEnvironmentPreparation(std::uint64_t ticket, std::string* error = nullptr) const;
    void CancelEnvironmentPreparation();
    // Also true while a replaced environment or staging buffer waits for its fence, so a settled scene's memory is final.
    bool IsEnvironmentPreparationPending() const noexcept
    {
        return m_environmentState == EnvironmentPreparationState::Pending ||
            std::any_of(m_environmentRetirements.begin(), m_environmentRetirements.end(),
                [](const auto& retirement) { return retirement.fence != UINT64_MAX; });
    }
    // S0.8B asynchronous geometry uploads (RendererGeometryUploads.cpp). A batch of staged buffer copies (collected by
    // ResourceUploader::SetDeferredBufferBatch) is recorded at the end of a later frame recording on the frame queue, at
    // most 32 MiB per frame, so every later BLAS build and draw on that queue sees the data. TakeGeometryUploadState
    // reports Submitted once that frame's submission is confirmed (the destination may then be published) and forgets
    // the ticket. The destinations are borrowed: their owner keeps them alive while the ticket is Queued and, once
    // recorded, until a GPU drain (scene resource release drains and calls ReleaseGeometryUploads). Queue returns 0 when
    // the bounded queue (128 MiB of staging) is full; the caller then uploads synchronously as before.
    enum class GeometryUploadState
    {
        Unknown,
        Queued,
        Submitted,
        Uncertain,
    };
    bool CanQueueGeometryUploads(size_t bytes) const noexcept;
    std::uint64_t QueueGeometryUploads(DeferredBufferUploadBatch&& batch);
    GeometryUploadState TakeGeometryUploadState(std::uint64_t ticket);
    void CancelGeometryUpload(std::uint64_t ticket) noexcept;

    // Sources for the built-in neutral (no assignment) and black (None) environments, as SetEnvironmentImage selects them.
    static EnvironmentPreparationSource NeutralEnvironmentSource(float radianceScale);
    static EnvironmentPreparationSource NoneEnvironmentSource(float radianceScale);
    // The camera exposure state as last read back from the GPU (a few frames old; display only).
    const AutoExposurePass::Readback& GetExposureReadback() const noexcept { return m_autoExposurePass.GetReadback(); }
    // The identity SetEnvironmentImage records for a source identity at a radiance scale.
    static std::string CalibratedEnvironmentIdentity(const std::string& identity, float radianceScale);

    const RendererStatistics& GetStatistics() const;

    // M4.1 shared light sampling: the latest build, its CPU cost and the publication counters. Diagnostics only.
    struct LightSamplingReport
    {
        bool enabled = false;
        bool available = false;
        std::uint64_t revision = 0;
        std::uint64_t builds = 0;
        std::uint64_t copies = 0;
        std::uint64_t failures = 0;
        double lastBuildMilliseconds = 0.0;
        double maximumBuildMilliseconds = 0.0;
        std::uint64_t lastCopyBytes = 0;
        std::uint64_t capacityBytes = 0;
        LightSamplingStatistics statistics;
        // M4.1.1: incremental updates, patch copies, background rebuilds adopted and discarded, and the last
        // incremental update's CPU time.
        std::uint64_t incrementalUpdates = 0;
        std::uint64_t patches = 0;
        std::uint64_t rebuildsAdopted = 0;
        std::uint64_t rebuildsDiscarded = 0;
        // M4.2C: background rebuilds started, whole copies, bytes uploaded in total, and the largest incremental update's
        // CPU time.
        std::uint64_t rebuildsLaunched = 0;
        std::uint64_t fullCopies = 0;
        std::uint64_t uploadBytes = 0;
        double maximumIncrementalMilliseconds = 0.0;
        double lastIncrementalMilliseconds = 0.0;
        bool lastCopyWasPatch = false;
    };

    LightSamplingReport GetLightSamplingReport() const;

    // M4.1: whether the shared light-sampling structures may be built and published. M4.3.5: on by default for ReGIR, which
    // builds them only on its own frames; ENGINE_LIGHT_SAMPLING=1 and sampled NEE build them every frame. Tests turn it off
    // to make the publication unavailable.
    void SetLightSamplingEnabled(bool enabled) noexcept
    {
        m_lightSamplingEnabled = enabled;
    }

    // M4.2A experimental sampled indirect NEE: only with ENGINE_SAMPLED_NEE=1 (the M42A twin libraries), whose estimator
    // these settings select per frame. Sampled NEE runs only when the shared light sampling is published for the frame's
    // table and the table has more than `threshold` lights; otherwise the experimental libraries' exhaustive loop runs.
    struct SampledIndirectNeeSettings
    {
        // false: the exhaustive loop (the in-library reference for A/B comparisons).
        bool sampled = true;
        // K, the RIS candidates per indirect vertex (2..8). M4.2C: 8, the best quality per millisecond in M4.2B.
        std::uint32_t candidates = 8;
        // Sampled only above this many table lights (M4.0: 64). Tests may lower it.
        std::uint32_t threshold = 64;
        // Test only: no indirect luminance clamp on next-event estimates, or on any indirect contribution.
        bool noNeeClamp = false;
        bool noClamp = false;
        // M4.2B: how the indirect luminance clamp treats the sampled finite-light estimate: 0 joint (with the vertex's sun,
        // HDRI and directional estimates, as exhaustive NEE), 1 exempt, 2 separate (clamped on its own).
        std::uint32_t clampPolicy = 1;
        // M4.2B: RIS target p_hat^gamma: 0 gamma 1, 1 gamma 0.75, 2 gamma 0.5.
        std::uint32_t targetTemper = 0;
        // M4.2C: NVIDIA SHARC training's next-event estimation: false keeps its one-light reservoir over every table light
        // (the comparison path). Above the same threshold, with the same K and tempering.
        bool trainingSampled = true;
        // M4.2C: the clamp policy of SHARC training's sampled estimate (as clampPolicy).
        std::uint32_t trainingClampPolicy = 1;
    };

    struct SampledIndirectNeeReport
    {
        bool librariesRequested = false;
        // The frame selected the sampled estimator for its image paths (mode set, RayQuery twins active).
        bool sampledLastFrame = false;
        // M4.2C: the frame selected sampled SHARC training.
        bool sampledTrainingLastFrame = false;
        std::uint64_t sampledTrainingFrames = 0;
        // M4.2C: frames rendered without table lights because sampling the guard had trusted was unavailable and the
        // exhaustive fallback would have exceeded the workload guard.
        std::uint64_t unsafeFallbackFrames = 0;
        std::uint64_t sampledFrames = 0;
        std::uint64_t exhaustiveFrames = 0;
        // Frames that requested sampling above the threshold but had no light-sampling structures for their table.
        std::uint64_t fallbackFrames = 0;
    };

    void SetSampledIndirectNeeSettings(const SampledIndirectNeeSettings& settings) noexcept
    {
        m_sampledNeeSettings = settings;
    }

    const SampledIndirectNeeSettings& GetSampledIndirectNeeSettings() const noexcept
    {
        return m_sampledNeeSettings;
    }

    const SampledIndirectNeeReport& GetSampledIndirectNeeReport() const noexcept
    {
        return m_sampledNeeReport;
    }

    // The frame's LightSamplingConstants address (0 when not published); valid during the frame's recording.
    D3D12_GPU_VIRTUAL_ADDRESS GetFrameLightSamplingConstants() const noexcept
    {
        return m_frameLightSamplingConstants;
    }

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
    // W6 finalization: one fixed heap sized for Engine's documented maxima, never grown or replaced (so no in-flight GPU work
    // can see a heap change): 8,192 unique resident meshes x 2 (16,384), up to MaterialAdmissionLimit materials with their
    // textures, Editor previews, and per-membership tables (active, candidate, up to four retired) with room to spare.
    // 65,536 descriptors cost ~2 MiB (32-byte descriptors), far below D3D12's 1,000,000 shader-visible limit. Exhaustion
    // still fails each transaction gracefully and recovers (B.1).
    static constexpr unsigned int ResourceDescriptorCapacity = 65536;
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
    // S0.8A: the same, sharing the caller's immutable prepared data instead of copying it.
    bool QueuePreparedTextureUpload(std::shared_ptr<const PreparedProjectTexture> prepared, std::uint64_t& ticket,
        std::string& error);
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

    // Diagnostics only: whether ENGINE_FRAME_HITCH_LOGGING=1 enabled the report, and this frame's Editor phases for it.
    bool IsFrameHitchLoggingEnabled() const noexcept { return m_frameHitchDiagnostics.loggingEnabled; }
    void RecordEditorPhaseTiming(const EditorPhaseTiming::Frame& phases) noexcept
    {
        if constexpr (kEnableFrameHitchDiagnostics)
        {
            m_frameHitchDiagnostics.current.editorPhases = phases;
        }
    }

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
    friend class VoxelOverlayVerification;
    friend class VoxelResidencyVerification;
    friend class VoxelQueryVerification;
    friend class VoxelQueryRig;
    friend class VoxelStreamingVerification;
    friend class AirFieldVerification;
    friend class AirMotorEditorVerification;
    friend class BackgroundTasksVerification;
    friend class ProjectBrowserVerification;
#endif
    friend class SceneMembershipPublication;
#if defined(ENGINE_HAS_VERIFICATION)
    friend class SceneDuplicationVerification;
    friend class SceneMembershipVerification;
    friend class CompositePlacementStress;
    friend class ResidencyBatchVerification;
    friend class RenderingVerification;
    friend class GamePlayGPUVerification;
    friend class AirPackagedParityVerification;
    friend class MemoryRegistryVerification;
    friend class PackagedVolumetricsVerification;
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
    // M3.5: the frame's lighting, reused while the snapshot revision (non-zero), sun and settings are unchanged; `revision` is
    // set to the content's revision (0 when it must not be reused). Never modified once returned.
    // M3.5: whether a path-traced mode's estimated exhaustive light work for this frame is safely below the GPU timeout
    // (with hysteresis); otherwise `failure` says why and the frame renders Legacy. Logged once per change.
    bool IsPathTraceWorkloadSafe(std::uint32_t tableLights, const PathTraceReferenceSettings& settings,
        const SharcSettings& sharc, std::string& failure);

    public:
    // M4.2C: the path-tracing workload estimate, a pure function of its inputs (unit-tested without a renderer).
    // Far below the 2 s Windows GPU timeout; guarded above the first, released below the second (no flicker at the edge).
    static constexpr double PathTraceWorkloadGuardMilliseconds = 250.0;
    static constexpr double PathTraceWorkloadReleaseMilliseconds = 200.0;

    struct PathTraceWorkloadInputs
    {
        std::uint32_t tableLights = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t samplesPerFrame = 1;
        std::uint32_t maxPathDepth = 1;
        // The experimental sampled-NEE permutations are requested (otherwise the production model applies).
        bool estimatorAware = false;
        bool sampledRequested = false;
        bool trainingSampledRequested = false;
        std::uint32_t threshold = 64;
        std::uint32_t candidates = 8;
        std::uint32_t directionalLights = 0;
        // This table's sampling structures are built and were published, and RayQuery visibility is active.
        bool structuresReady = false;
        bool rayQueryActive = false;
        bool sharcEnabled = false;
        std::uint32_t trainingTile = 8;
        std::uint32_t trainingMaxDepth = 8;
    };

    // What an estimate priced. totalMilliseconds is what the guard compares; the exhaustive parts are what the frame would
    // cost if sampling turned out to be unavailable after all (the failed-publication fail-safe).
    struct PathTraceWorkloadModel
    {
        bool estimatorAware = false;
        bool sampledImage = false;
        bool sampledTraining = false;
        double imageMilliseconds = 0.0;
        double trainingMilliseconds = 0.0;
        double exhaustiveImageMilliseconds = 0.0;
        double exhaustiveTrainingMilliseconds = 0.0;
        double totalMilliseconds = 0.0;
        // The recording the estimate was made for (0: none).
        std::uint64_t recordingGeneration = 0;
    };

    static PathTraceWorkloadModel EstimatePathTraceWorkload(const PathTraceWorkloadInputs& inputs) noexcept;

    // M5.1: whether this frame's sampled light structures can be trusted by the guard (its structuresReady input). The
    // structures a frame samples are rebuilt or updated and published for exactly that frame's table, so sampling is
    // available unless publication fails; the last recorded publication is the evidence. It must have succeeded for a table
    // of the same light count (a changed count is a new table: its first frame is priced exhaustively, as before). The
    // lighting revision does not matter: revision-0 content is rebuilt every frame and published just the same. If
    // publication fails after all, the existing per-frame fail-safe drops the table lights rather than run exhaustively.
    static bool SampledLightStructuresTrusted(bool lastPublicationAvailable, std::uint32_t lastPublishedLightCount,
        std::uint32_t tableLights) noexcept
    {
        return lastPublicationAvailable && tableLights != 0 && lastPublishedLightCount == tableLights;
    }

    private:
    PathTraceWorkloadModel m_pathTraceWorkloadModel;
    bool m_pathTraceWorkloadGuarded = false;
    std::uint64_t m_pathTraceWorkloadGuardChanges = 0;
    double m_pathTraceWorkloadEstimateMilliseconds = 0.0;
    std::shared_ptr<const FrameLighting> AcquireFrameLighting(const DirectionalLight& legacySun,
        const SceneLightingSnapshot* snapshot, const SceneLightingSettings& proceduralSettings, std::uint64_t& revision);
    // M4.1: builds (or reuses, for the same non-zero revision) the shared light-sampling structures of the frame's published
    // light table and records their publication; returns the frame's LightSamplingConstants address (0 when disabled).
    D3D12_GPU_VIRTUAL_ADDRESS RecordLightSampling(ID3D12GraphicsCommandList* commandList, const FrameLighting& lighting,
        std::uint64_t lightingRevision, bool regirFrame, bool restirPtFrame);
    bool m_lightSamplingEnabled = false;
    // M4.3.5: built every frame (ENGINE_LIGHT_SAMPLING=1, sampled NEE), not only on ReGIR frames.
    bool m_lightSamplingAlways = false;
    // M4.3.5: the frame's RTXDI local lights while ReGIR is requested (0 otherwise), for ReGIR's threshold.
    std::uint32_t m_frameReGIRLocalLights = 0;
    LightSamplingSettings m_lightSamplingSettings;
    LightSamplingBuffer m_lightSamplingBuffer;
    // M4.1.1: the structures sampling m_lightSamplingCacheTable (lighting revision m_lightSamplingCacheRevision), updated
    // in place by incremental updates. Owned by the render thread; the background rebuild works on its own copy.
    std::shared_ptr<LightSamplingData> m_lightSamplingCache;
    std::uint64_t m_lightSamplingCacheRevision = 0;
    SceneLightTable m_lightSamplingCacheTable;
    // M4.1.1: identifies m_lightSamplingCache's contents for the buffer (every build, update or adoption is new), since an
    // adopted rebuild holds different words for the same lighting revision.
    std::uint64_t m_lightSamplingPublication = 0;
    // M4.1.1: a full build of m_lightSamplingRebuildTable on a worker, started while the structures hold lights an
    // incremental update moved to the wide pool. Adopted (brought up to date incrementally) when it completes.
    std::future<std::shared_ptr<LightSamplingData>> m_lightSamplingRebuild;
    SceneLightTable m_lightSamplingRebuildTable;
    D3D12_GPU_VIRTUAL_ADDRESS m_frameLightSamplingConstants = 0;
    // M4.3 ReGIR: the frame's scene light buffer SRV (for the presampling pass), the presampling pipeline and its slot
    // buffer (allocated on first use for the fixed settings, released at shutdown), and a frame counter for its seeds.
    std::uint32_t m_frameSceneLightBufferIndex = UINT_MAX;
    ComputePipeline m_regirPresamplePipeline;
    bool m_regirPresampleAttempted = false;
    Buffer m_regirSlots;
    std::uint32_t m_regirFrame = 0;
    // Records this frame's ReGIR presampling when ReGIR can be used; returns the slot buffer's UAV index or UINT_MAX.
    std::uint32_t PrepareReGIRFrame(ID3D12GraphicsCommandList* commandList, const FrameLighting& frameLighting,
        const PathTraceReferenceConstants& constants);
    // The descriptor index of the buffer published this frame (valid with m_lightSamplingReport.available).
    std::uint32_t m_frameLightSamplingBufferIndex = 0;
    // M4.2A: the mode word and buffer index the path tracer reads from SceneLightConstants.padding.
    void ApplySampledIndirectNee(const FrameLighting& lighting, SceneLightConstants& constants);
    SampledIndirectNeeSettings m_sampledNeeSettings;
    SampledIndirectNeeReport m_sampledNeeReport;
    bool m_sampledNeeFallbackLogged = false;
    LightSamplingReport m_lightSamplingReport;
    // M5.1 guard fix: the light count of the table the last recorded publication sampled (0 when that publication failed or
    // nothing was published). Unlike m_lightSamplingCacheTable it is kept for every lighting revision, including revision 0
    // (content not identified), whose structures are rebuilt and published for exactly that frame's table.
    std::uint32_t m_lightSamplingPublishedLightCount = 0;
    bool m_lightSamplingFailing = false;
    std::shared_ptr<const FrameLighting> m_frameLightingCache;
    std::uint64_t m_frameLightingCacheRevision = 0;
    DirectionalLight m_frameLightingCacheSun;
    SceneLightingSettings m_frameLightingCacheSettings;
    // M3.5 ENGINE_DIAG_LIGHT_PUBLICATION_VERIFY (Debug): reused frame lightings checked and differences found.
    std::uint64_t m_frameLightingVerifications = 0;
    std::uint64_t m_frameLightingMismatches = 0;

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
    // M0c.5 Stage 5.1: everything the Project Browser does not use: scene passes, Editor overlays and AIR views, AIR,
    // mist, LUTs, material tables, Legacy passes, the bootstrap environment and the scene-extent targets.
    bool InitializeSceneRenderer(const HDRImageData* initialEnvironment, std::string environmentIdentity,
        std::span<const std::byte> packagedMaterialTables);

    enum class SceneRendererState
    {
        NotInitialized,
        Initialized,
        Failed
    };

    SceneRendererState m_sceneRendererState = SceneRendererState::NotInitialized;
    bool m_projectWorkflowStart = false;

    // M0c.5 Stage 5.3: the Legacy passes, created at the first Legacy frame of a project workflow start (otherwise eagerly).
    bool InitializeLegacyLighting();
    bool m_legacyLightingInitialized = false;
    bool m_legacyLightingFailed = false;

    // M0c.5 Stage 5.3: a project workflow start's AIR field, AIR views and mist pass (haze included), prepared when a frame first
    // requests any of them. The pipeline worker compiles their shaders into a private library (DXC only, plus throwaway pipelines that warm
    // the driver cache); the render thread adopts the shaders and creates the real objects between frames. Until Ready, frames
    // withhold those requests, and before the first lit frame they stay unlit (no partial lighting). Failure is final for the
    // session: AIR, mist and haze stay off, as an uninitialized field already behaves.
    enum class VolumetricState
    {
        NotRequested,
        Preparing,
        Ready,
        Failed
    };

    static bool RequestsVolumetrics(const RendererOptions& options) noexcept;
    bool PrepareVolumetrics();
    bool InitializeVolumetrics();
    VolumetricState m_volumetricState = VolumetricState::NotRequested;
    std::shared_ptr<ShaderLibrary> m_volumetricShaders;
    // M0c.5 Stage 5.5: the volumetric job's own worker, started by the first request (absent until then).
    std::unique_ptr<RayTracingPipelineWorker> m_volumetricWorker;
    // M0c.6: when a frame last requested enabled AIR (or the field held no storage); after ModeResourceGracePeriod without one the
    // storage is retired (RecordRenderFrame).
    std::chrono::steady_clock::time_point m_airLastRequested = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point m_airVectorsLastRequested = std::chrono::steady_clock::now();
    std::uint64_t m_airStorageRetirements = 0;

public:
    std::uint64_t GetAirStorageRetirements() const noexcept
    {
        return m_airStorageRetirements;
    }

private:
    std::chrono::steady_clock::time_point m_volumetricRequested;
    bool m_litFramePresented = false;
    bool m_volumetricHoldThisFrame = false;

    // SER S6B: chooses the path tracer's DXR library variant once, before any library is prepared
    // (SelectPathTraceLibraryVariant: ENGINE_PATH_TRACE_LIBRARIES and the device capabilities), and logs it.
    void SelectPathTraceLibraries();

    // UPSCALING.2A/2C: the render extent for a requested output extent. Native: the output extent itself. Diagnostic75
    // and Dlss75: each axis round(0.75 * output) (half up), at least 1. UPSCALING.3D presets: the NGX optimal extent;
    // presetSupported is false (and the extent 75%) when NGX reports none. RR.1C rayReconstruction: a preset's extent is
    // Ray Reconstruction's optimal one instead of Super Resolution's.
    SceneResolution ResolveSceneResolution(unsigned int outputWidth, unsigned int outputHeight, RenderScale scale,
        bool& presetSupported, bool rayReconstruction = false);

    // Private resize stages: called only after Resize() confirms the queue is idle.
    bool ResizeApplicationResources(
        unsigned int width,
        unsigned int height);
    bool ResizeSceneResources(const SceneResolution& resolution);

    void Draw(
        ID3D12GraphicsCommandList* commandList,
        const RenderObject& object,
        std::size_t renderObjectIndex,
        D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress);
    // depthPrepass: the Editor overlay depth prepass variant (UPSCALING.3C.4) instead of the selection mask pipeline.
    void DrawSelectionMask(
        ID3D12GraphicsCommandList* commandList,
        const RenderObject& object,
        D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress,
        bool depthPrepass = false);
    void DrawMaterialRange(ID3D12GraphicsCommandList* commandList, const RenderObject& object,
        Material& material, unsigned int firstIndex, unsigned int indexCount,
        D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress, std::size_t renderObjectIndex);
    bool ValidateSceneMaterialSlots(const Scene& scene, const std::vector<Material*>& materials) const;
    void BeginGeometryPass(ID3D12GraphicsCommandList* commandList);
    void EndGeometryPass(ID3D12GraphicsCommandList* commandList);
    bool InitializeMaterials(const std::vector<Material*>& materials);
    bool InitializeRayTracingScene(const Scene& scene);
    bool DiscoverEnvironments();
    bool PreprocessEnvironment(EnvironmentMap& environment);

    // S0.5 asynchronous environment preparation (RendererEnvironmentPreparation.cpp). m_environmentJob is the newest
    // request, the only one that may publish. Superseded jobs whose worker is still running drain in
    // m_environmentDrainingJobs (at most one: a newer request then waits in m_environmentQueuedSource, latest wins).
    // Retirements hold GPU-referenced owners until their fence completes (UINT64_MAX: until shutdown's drain).
    struct EnvironmentRetirement
    {
        std::uint64_t fence = 0;
        std::unique_ptr<EnvironmentPreparationJob> job;
        // A job's staging buffer, retired on its own once the copies reading it were submitted.
        Microsoft::WRL::ComPtr<ID3D12Resource> staging;
    };
    std::unique_ptr<EnvironmentPreparationJob> m_environmentJob;
    std::vector<std::unique_ptr<EnvironmentPreparationJob>> m_environmentDrainingJobs;
    std::optional<EnvironmentPreparationSource> m_environmentQueuedSource;
    std::uint64_t m_environmentQueuedTicket = 0;
    std::vector<EnvironmentRetirement> m_environmentRetirements;
    std::uint64_t m_environmentLatestTicket = 0;
    EnvironmentPreparationState m_environmentState = EnvironmentPreparationState::Unknown;
    std::string m_environmentError;
    // Environments published by RequestEnvironment (diagnostics and verification).
    std::uint64_t m_environmentPublications = 0;
    // Set by a publication whose request snaps exposure; consumed by that frame's exposure pass.
    bool m_exposureSnapAfterEnvironmentPublication = false;
    // Per-frame budgets: staging bytes copied and estimated precompute GPU microseconds (ENGINE_ENVIRONMENT_PREP_COPY_MIB
    // and ENGINE_ENVIRONMENT_PREP_GPU_US override them for measurement).
    std::uint64_t m_environmentCopyBudget = 0;
    std::uint64_t m_environmentWorkBudget = 0;
    void StartEnvironmentJob(EnvironmentPreparationSource source, std::uint64_t ticket);
    void SupersedeEnvironmentJob();
    void RetireEnvironmentJob(std::unique_ptr<EnvironmentPreparationJob> job);
    void CollectEnvironmentPreparation();
    bool BeginEnvironmentGpuStage(EnvironmentPreparationJob& job);
    void RecordEnvironmentPreparation(ID3D12GraphicsCommandList* commandList);
    void CommitEnvironmentPreparationSubmission(bool executed);
    void PublishPreparedEnvironment();
    void FailEnvironmentJob(const std::string& error);
    void ShutdownEnvironmentPreparation();

    struct QueuedGeometryUpload
    {
        std::uint64_t ticket = 0;
        DeferredBufferUploadBatch batch;
        std::uint64_t lifetime = 0;
        bool recordedUncommitted = false;
        bool uncertain = false;
    };
    struct GeometryStagingRetirement
    {
        std::uint64_t fence = 0;
        size_t bytes = 0;
        Microsoft::WRL::ComPtr<ID3D12Resource> staging;
    };
    std::deque<QueuedGeometryUpload> m_geometryUploads;
    std::vector<GeometryStagingRetirement> m_geometryStagingRetirements;
    std::unordered_map<std::uint64_t, GeometryUploadState> m_geometryUploadStates;
    std::uint64_t m_geometryUploadTicket = 0;
    void RecordGeometryUploads(ID3D12GraphicsCommandList* commandList);
    void CommitGeometryUploadSubmission(bool executed);
    void CollectGeometryUploadRetirements();
    void ReleaseGeometryUploads() noexcept;

    GraphicsDevice m_graphicsDevice;

    SwapChain m_swapChain;

    ReferenceRayCounts m_referenceRayCounts;
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
    EditorVoxelPass m_editorVoxelPass;
    EditorAirPass m_editorAirPass;
    EditorMistPass m_editorMistPass;
    // Diagnostics: vectors drawn by the last frame.
    std::uint32_t m_lastAirVectorCount = 0;
    std::uint32_t m_lastAirTemperatureCount = 0;
    std::uint32_t m_lastAirDivergenceCount = 0;
    // Diagnostics: cells examined by the last frame's air-cell view.
    std::uint32_t m_lastAirCellCount = 0;
    // Diagnostics: sub-cells examined by the last frame's fluid-pieces view.
    std::uint32_t m_lastAirSubcellCount = 0;
    VoxelResidencyResources m_voxelResidency;
    AirField m_airField;
    // PSR.2 Stage 2 development prototype (PathTraceReferenceSettings::primarySurfaceReplacementInPlace).
    PsrInPlaceReplacePass m_psrInPlaceReplacePass;
    std::shared_ptr<PsrPipelinePreparation> m_psrPreparedAuxiliary;
    // PS.5C: a packaged game's cooked shader bootstrap, shared with the PSR auxiliary job (null in the Editor).
    std::shared_ptr<const std::vector<std::byte>> m_packagedShaderPayload;
    PsrStatus m_psrStatus;
    bool m_psrAuxiliaryReady = false;
    bool m_psrAuxiliaryFailed = false;
    std::uint32_t m_psrActiveChain = 0;
    ShaderLibraryPreparation PreparePsrAuxiliary();

    // Whether the last recorded frame ran the in-place replacement pass (diagnostics).
    bool m_psrInPlaceReplacedLastFrame = false;
    // Multibounce PSR: this frame's effective chain length (1, or 2 for two-bounce PSR), decided with its lighting.
    std::uint32_t m_psrChainThisFrame = 1;
    // Development verification only: when set (with the pass's diagnostic pipeline initialized), the replacement pass
    // runs its diagnostic variant and writes per-pixel hit records here. Null in normal rendering.
    Buffer* m_psrDiagnosticHits = nullptr;
    bool RecordPsrInPlaceReplacement(ID3D12GraphicsCommandList* commandList, const Scene& scene, const Matrix4& view,
        const Matrix4& renderProjection, const Vector3& cameraPosition, const CameraConstants& cameraConstants);
    // PSR.2 Stage 3A: whether this frame lights replaced pixels (decided before the geometry pass, so replacement runs
    // without a diagnostic view only when it does), and the PSR.2 lighting pair's bindings, set by
    // RecordPsrInPlaceReplacement on such a frame (0 otherwise).
#ifndef NDEBUG
    // Verification only: withholds the combined PSR + SHARC path, as a failed preparation does.
    bool m_psrSharcPreparationFailureForTesting = false;
    Buffer* m_psrSharcEnergyReferenceForTesting = nullptr;
#endif
    bool m_psrLightingThisFrame = false;
    static bool PsrLightingSupportsDenoiser(PathTraceDenoiserMode mode);
    // PSR.2 Stage 3B phase 4: an NRD mode's PSR bindings for its preparation pass on a frame whose replaced pixels the path
    // tracer lit, creating the pass's variant on first use; empty otherwise, or when the variant cannot be created
    // (failed reports why).
    NrdPathTracePreparePass::PsrBindings PathTraceNrdPsrBindings(NrdPathTracePreparePass& pass, bool& failed);
    // The NRD material comparison of a PSR frame (nrd::ReblurSettings or nrd::RelaxSettings); unchanged otherwise.
    template<typename Settings>
    void ApplyNrdPsrMaterialSeparation(bool psrFrame, Settings& settings) const;
    D3D12_GPU_VIRTUAL_ADDRESS m_psrGBufferConstantsAddress = 0;
    D3D12_GPU_VIRTUAL_ADDRESS m_psrGBufferTransformsAddress = 0;
    // PSR.2 Stage 3B: the denoisers' PSR variants' constants on such a frame (0 otherwise), and whether the last
    // temporal frame used the PSR variant (a change restarts the history).
    D3D12_GPU_VIRTUAL_ADDRESS m_psrDenoiseConstantsAddress = 0;
    bool m_psrTemporalLastFrame = false;
    // Whether the last path-traced frame recorded the PSR.2 lighting pair.
    bool m_psrLightingLastFrame = false;
    // M5.5 guide-only PSR for ReSTIR PT (configuration B): requested and prepared this frame (eligibility marking in the
    // geometry pass, replacement after lighting and the previous G-buffer copy), and whether it replaced the G-buffer for
    // RR's guides this frame. B's lighting estimator never reads the replaced G-buffer.
    bool m_psrGuideOnlyThisFrame = false;
    // Optimization A5: the Editor's last applied spatial pair count (RecordReSTIRPTFrame applies changes only).
    std::uint32_t m_restirPtUiSpatialPairs = 3;
    // Optimization B1a: the Editor's last applied decorrelation mode (true: stagnancy).
    bool m_restirPtUiStagnancy = false;
    // Optimization B1b: the Editor's last applied firefly replacement setting.
    bool m_restirPtUiFirefly = false;
    bool m_psrGuideOnlyLastFrame = false;

    BRDFIntegrationPass m_brdfIntegrationPass;

    GGXAverageAlbedoPass m_ggxAverageAlbedoPass;

    // M0c.1: the passes only Legacy lighting records (LegacyLightingResources.h).
    LegacyLightingResources m_legacyLighting;
    // Brings the Legacy passes to the render extent when a resize skipped them or they retired; before a Legacy recording
    // reads them.
    bool EnsureLegacyLightingExtent();

    // M0c.2 / M0c.3: the residency policy shared by the Legacy and Path Tracing resource owners (ModeResourceResidency.h).
    // An inactive mode keeps its resources this long after the last frame that used them before they may retire.
    static constexpr std::chrono::seconds ModeResourceGracePeriod{5};

    enum class ModeResourceDecision
    {
        Keep,
        // Due, but the last use has not completed on the GPU; retried by a later recording, never waited for.
        Deferred,
        Retire,
        RetireUnderPressure
    };

    // Whether an inactive mode's resources may retire now: never while a transition is pending or after an unconfirmed
    // submission; otherwise once the grace period ended (or earlier under DXGI memory pressure, or when the caller's own
    // rule makes them due) and the last use completed.
    ModeResourceDecision EvaluateModeResourceRetirement(ModeResourceResidency& residency, bool transitionPending,
        bool dueRegardless = false);
    // At the start of a recording's resource decisions: a use the previous recording never committed makes the residency's
    // last fence unknown.
    static void BeginModeResourceRecording(ModeResourceResidency& residency) noexcept;
    // This recording uses the mode's resources (they are resident): the grace period restarts.
    static void MarkModeResourceUsed(ModeResourceResidency& residency) noexcept;
    // After a confirmed submission: a mode used by the recording was last used at that submission's fence.
    void CommitModeResourceSubmission(ModeResourceResidency& residency);
    // The DXGI local-segment pressure state for retirement, with the hysteresis NRD retention uses.
    bool IsModeResourceMemoryUnderPressure();
    bool m_modeResourceUnderPressure = false;
    // Testing only: a DXGI headroom fraction used instead of the adapter's (negative queries DXGI).
    double m_modeResourceHeadroomOverrideForTesting = -1.0;

    // Retires the inactive Legacy textures (M0c.2). Called on recordings that do not render Legacy.
    void UpdateLegacyLightingRetention();

    // M0c.3 Phase A: residency of Path Tracing's exclusive resources, shared by Reference, Raster Primary and ReSTIR DI:
    // the reference pass's render-extent textures and buffers, the temporal history, the spatial intermediates, the RTXDI
    // reservoirs and the previous-frame G-buffer copy. Their pass objects stay Renderer members; this record owns their
    // lifecycle.
    ModeResourceResidency m_pathTracingResidency;
    // M0c.4 Stage 4.1/4.2: the presented and pending configurations.
    ConfigurationPreparation m_configurationPreparation;
    // The preparation sets found ready for request generation m_preparedStepsGeneration (bits of PrepareConfiguration).
    std::uint32_t m_preparedSteps = 0;
    std::uint64_t m_preparedStepsGeneration = 0;
#ifndef NDEBUG
    // Verification only (M0c.4): a pending path-traced configuration stays Preparing (a slow compile) or fails to prepare.
    bool m_configurationPreparationHoldForTesting = false;
    bool m_configurationPreparationFailureForTesting = false;
    // M0c.4 diagnostics: frames whose PSR replacement records were validated, and the findings (ValidatePsrFrameRecords).
    std::uint64_t m_psrRecordCheckFrames = 0;
    std::uint64_t m_psrRecordFindings = 0;
    void ValidatePsrFrameRecords();
    bool IsValidMaterialTableDescriptor(Texture& table) const;
#endif
    // M0c.4 diagnostics: a named DRED marker in the frame recording (Debug, ENGINE_DIAG_DRED_MARKERS only; no-op otherwise).
    void MarkDredStage(ID3D12GraphicsCommandList* commandList, const wchar_t* stage) const;
    // One non-blocking preparation step for the pending identity: the same preparation functions, with the same
    // arguments, that the gates evaluate in the frame that switches to it, so that frame passes every gate.
    PreparationStep PrepareConfiguration(const ConfigurationIdentity& identity, std::string& detail);
    float EstimatePreparationProgress(const ConfigurationIdentity& identity) const;
    // Stage 4.4: releases the Legacy textures Initialize created when no recording ever used them and the starting
    // configuration does not render Legacy (they are created again by the first Legacy frame, as after retirement).
    void ReleaseUnusedLegacyTextures();
    void RecordPreparationScreen(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE output, unsigned int width,
        unsigned int height, bool drawProgress) const;
    // Stage 4.3/4.5: set by unlit frames, so the first lit one snaps the auto exposure instead of adapting from black.
    bool m_exposureSnapAfterUnlitFrames = false;
    // Whether the last recorded frame lit anything (GetRenderedConfiguration).
    bool m_litLastFrame = true;
    // Stage 4.3: armed by SelectPresentedConfiguration while the first request prepares, consumed by the next recording.
    bool m_lightingSuppressedForNextFrame = false;
    const RenderingConfiguration& Presented();
    // Whether the pending configuration (if any) needs these resources, so retirement keeps them for its switch.
    bool IsPendingConfigurationMode(bool pathTraced) const;
    // The group as its NrdRetentionGroup value (that enum is declared further down this class).
    bool IsPendingConfigurationNrdGroup(int group) const;
    // M0c.4 Stage 4.0: one set of DXR requirements observed through the const paths (libraries first; pipelines once every
    // library is accepted, as PrepareDxrLibraries orders them).
    DependencyReadiness ObserveDxrRequirements(const std::vector<PathTraceLibraryRequirement>& libraries,
        const std::vector<PathTraceReferencePass::PipelineRequirement>& pipelines) const;
    // Retires the inactive Path Tracing resources. Called on recordings that do not render a path-traced mode.
    void UpdatePathTracingRetention();
    // Releases them (the caller has established that no submitted or recorded work references them).
    void ReleasePathTracingResources();
    // Recreates the reference pass's textures at the render extent after a retirement, before a path-traced recording uses
    // them; the lazily created resources follow on first use as after a resize. False fails the recording closed.
    bool EnsurePathTracingResident();
    // Testing only: the next recreation fails as an allocation failure would.
    bool m_pathTracingRecreationFailureForTesting = false;

    // The hybrid lighting mode's REBLUR instances (reflections, half-resolution GI). NRD.2: created on the first frame
    // that denoises with them, released on the first path-traced frame (the only modes that never use them).
    NrdDenoiser m_nrdReflectionDenoiser;
    NrdDenoiser m_nrdGIDenoiser;
    // A creation failure is not retried until the next resize or renderer restart.
    bool m_hybridNrdCreationFailed = false;

    // Creates both hybrid instances at the scene extent when missing. Creation only; the SDK waits for the GPU while
    // building its pipelines.
    bool EnsureHybridNrdDenoisers();
    // Waits for the GPU, then releases both hybrid instances when they exist. Their next use starts new history.
    void ReleaseHybridNrdDenoisers();

    GISignedCorrectionPass m_giSignedCorrectionPass;

    // RTXPT.1 reference path tracer. Created lazily on the first frame that requests it, so Legacy
    // frames and packaged runtimes never allocate it; resized with the scene; released at shutdown.
    PathTraceReferencePass m_pathTraceReferencePass;
    // RTXPT D.2B temporal reconstruction of the extracted signals (diagnostics only, created on first use).
    PathTraceTemporalPass m_pathTraceTemporalPass;
    // RTXPT D.3B spatial filtering of the temporal outputs (diagnostics only, created on first use; no textures).
    PathTraceSpatialPass m_pathTraceSpatialPass;
    // NRD.3 path-traced NRD signal preparation and bypass reconstruction (diagnostics only). Its textures exist only
    // while m_nrdPathTraceDiagnostic is set: created on its first extraction frame, released when it is cleared
    // (after a GPU wait), on resize and at shutdown.
    NrdPathTracePreparePass m_nrdPathTracePreparePass;
    // NRD.RELAX.2 RELAX signal preparation and bypass reconstruction (diagnostics only; RELAX is never dispatched). Its
    // pipelines and textures, and the extraction's raw diffuse hit distance, exist only while m_nrdRelaxPrepareDiagnostic
    // is set: created on its first extraction frame, released when it is cleared (after a GPU wait), on resize and at
    // shutdown. Separate from REBLUR's instance, whose textures and history it never touches.
    NrdPathTracePreparePass m_nrdRelaxPreparePass;
    PathTraceReferenceStatus m_pathTraceStatus;

    // RTXPT.2 R4 previous-frame G-buffer, PT modes only (D4). Allocated on the first frame that renders
    // either PT mode, kept across mode switches and Legacy frames, resized with the scene and released at
    // shutdown, both after the existing WaitForGPU. A failed allocation is non-fatal and is not retried
    // until the next scene resize.
    PreviousGBufferHistory m_previousGBufferHistory;
    bool m_previousGBufferHistoryFailed = false;
    void EnsurePreviousGBufferHistory();

    // Each PT frame records a copy of its final G-buffer into the slot that is not being read. The copy
    // becomes the read slot only after the frame's command list was submitted; a frame that fails before
    // that, or a later Render failure, invalidates instead. The single queue orders the next frame's
    // overwrite of a slot after every earlier frame's reads of it.
    unsigned int m_previousGBufferReadSlot = 0;
    bool m_previousGBufferValid = false;
    PreviousGBufferCamera m_previousGBufferCameras[PreviousGBufferHistory::SlotCount];
    std::optional<PathTracePrimary> m_previousGBufferPrimary;

    struct PendingPreviousGBufferCopy
    {
        unsigned int slot = 0;
        PreviousGBufferCamera camera;
    };

    std::optional<PendingPreviousGBufferCopy> m_pendingPreviousGBufferCopy;
    std::uint64_t m_previousGBufferCopiesRecorded = 0;
    std::uint64_t m_previousGBufferCopiesSubmitted = 0;
    // Whether this recording's PT consumers could read history (after its own invalidations). Evidence only.
    bool m_previousGBufferAvailableThisFrame = false;
    void InvalidatePreviousGBufferHistory();
    void RecordPreviousGBufferCopy(ID3D12GraphicsCommandList* commandList, const PreviousGBufferCamera& camera);
    void CommitPreviousGBufferCopy();

    // RTXPT.3 R3.3 ReSTIR DI resources, raster-primary ReSTIR DI variant only. Same lifetime as the R4 history:
    // allocated lazily by that variant's first frame (EnsureRtxdiResources), resized with the scene and
    // released at shutdown after the existing WaitForGPU; a failed allocation is not retried until the next
    // scene resize. Reservoir history is invalidated at the same boundaries as the R4 history and is
    // committed only after a successful submission. No frame uses it yet.
    RtxdiResources m_rtxdiResources;
    bool m_rtxdiResourcesFailed = false;
    void EnsureRtxdiResources();

    // M5.1 ReSTIR PT Enhanced (configuration B; Editor-only, experimental). The pass is created on the mode's first request
    // (EnsureReSTIRPT: pipelines, light tiles, counters) and released at shutdown; its reservoirs follow the render extent
    // and are released with the path-tracing textures. m_restirPtSettings are the published defaults, overridden only by
    // the ENGINE_RESTIR_PT_* diagnostics (Initialize) and the M5.1 harnesses. m_restirPtFrameLastFrame: the last recorded
    // path-traced frame ran configuration B.
    ReSTIRPTPass m_restirPtPass;
    ReSTIRPTSettings m_restirPtSettings;
    PathTraceModeAvailability m_restirPtAvailability;
    std::uint32_t m_restirPtFrame = 0;
    bool m_restirPtFrameLastFrame = false;
    // M5.3: whether the last configuration B recording ran spatial reuse.
    bool m_restirPtSpatialLastFrame = false;
    bool m_restirPtFrameThisFrame = false;
    bool EnsureReSTIRPT();
    bool m_restirPtSamplingFailureLogged = false;
    // M5.1-4 (M5.2 prerequisite): this frame's per-instance transforms (by TLAS InstanceID = render-object index) and the
    // original reconnection criteria's minimum distance (2% of the smallest dimension of the scene's world bounds), from
    // the scene's render objects. Only on configuration B frames.
    void PrepareReSTIRPTTransforms(const Scene& scene);
    std::vector<ReSTIRPTInstanceTransform> m_restirPtTransforms;
    float m_restirPtReconnectionMinDistance = 0.0f;
    void RecordReSTIRPTFrame(ID3D12GraphicsCommandList* commandList, unsigned int frameIndex,
        D3D12_GPU_VIRTUAL_ADDRESS constantsAddress, D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstantsAddress,
        const FrameLighting& frameLighting, const PathTraceReferenceSettings& settings, const Matrix4& view,
        const Matrix4& projection, bool extract, bool rawExtraction);
    // M5.2: the signature the B frame's reservoirs are computed against (temporal history reuse requires a match).
    ReSTIRPTHistorySignature MakeReSTIRPTHistorySignature(const PathTraceReferenceSettings& settings) const;
    // M5.2: why the most recent B frame did or did not read history (diagnostics and the harness).
    ReSTIRPTHistoryDecision m_restirPtHistoryDecision = ReSTIRPTHistoryDecision::NoCommittedFrame;

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
        SceneLightTable lights{};
        D3D12_GPU_VIRTUAL_ADDRESS topLevelAddress = 0;
        std::uint64_t topLevelRecording = 0;
        std::uint64_t materialRevision = 0;
        std::uint64_t sceneResourceLifetime = 0;
        unsigned int environmentTextureIndex = UINT_MAX;
        unsigned int environmentConditionalCDFIndex = UINT_MAX;
        float environmentTotalWeight = 0.0f;
    };

    std::optional<PathTraceAccumulationKey> m_pathTraceAccumulationKey;

    // SHARC.4B: the radiance-affecting inputs of a path-traced frame. The accumulation key with its camera-only
    // terms neutralized (view, projection, size, samples per frame, maximum accumulated samples), plus what the key
    // lacks: ray-tracing membership and per-object material bindings, the library variant and the cache's identity.
    struct SceneLightingInputs
    {
        PathTraceAccumulationKey key;
        std::uint64_t materialBindingEpoch = 0;
        PathTraceLibraryVariant libraryVariant = PathTraceLibraryVariant::SM66;
        RadianceCacheConfigurationIdentity cache;
    };

    static SceneLightingInputs MakeSceneLightingInputs(const PathTraceAccumulationKey& key,
        std::uint64_t materialBindingEpoch, PathTraceLibraryVariant libraryVariant,
        const RadianceCacheConfigurationIdentity& cache);
    static bool SameSceneLightingInputs(const SceneLightingInputs& left, const SceneLightingInputs& right);
    // Latches the frame's epoch from its key. Called once per path-traced recording, in RecordPathTraceReference,
    // after the frame's material table, TLAS update and environment are recorded.
    void LatchSceneLighting(const PathTraceAccumulationKey& key);
    SceneLightingEpoch m_sceneLightingEpoch;
    std::optional<SceneLightingInputs> m_sceneLightingInputs;
    // The D6 cache's identity while it has resources, otherwise all zero (UpdateRadianceCacheD6).
    RadianceCacheConfigurationIdentity m_radianceCacheConfiguration;

    // SHARC.4C experimental D6 radiance cache (RendererOptions::experimentalD6RadianceCache): resources and generation
    // lifecycle only. UpdateRadianceCacheD6 runs at the start of each scene-frame recording, before the latch.
    RadianceCacheD6 m_radianceCacheD6;
    RadianceCacheD6::Settings m_radianceCacheD6Settings;
    // A failed allocation is not retried every frame; turning the option off clears it.
    bool m_radianceCacheD6Failed = false;
    // Diagnostics fault injection: the next allocation fails.
    bool m_radianceCacheD6FailAllocationForTesting = false;
    void UpdateRadianceCacheD6(bool enabled);
    // SHARC.4D learning: the cache library's pipeline and the frame's insertion binding (0: this frame does not learn).
    // SHARC.4D-Q: deferred insertion also needs a record per path (pixels x samplesPerFrame).
    D3D12_GPU_VIRTUAL_ADDRESS PrepareRadianceCacheLearning(ID3D12GraphicsCommandList* commandList,
        std::uint32_t samplesPerFrame);
    // SHARC.4E: applies the frame's SharcSettings to the cache (before UpdateRadianceCacheD6's generation work);
    // estimator-changing settings restart accumulation. m_sharcStatus is read from the frame slot's readback.
    void ApplySharcSettings(const SharcSettings& requested, bool rasterPrimary);
    // SHARC.4E.1: sets the denoiser's stage flags when the effective denoiser (RendererOptions::pathTraceDenoiserMode, the
    // raster-primary modes only) changes, so diagnostics that drive the flags directly are left alone while it is
    // unchanged. NRD.4B: REBLUR keeps the Engine stage flags (its fallback); RELAX and Hybrid are presented as Engine.
    void ApplyPathTraceDenoiser(PathTraceDenoiserMode requested, bool rasterPrimary);
    bool m_pathTraceDenoiserOption = false;
    // NRD.2: the mode this frame denoises with (Off outside the raster-primary modes).
    PathTraceDenoiserMode m_pathTraceDenoiserMode = PathTraceDenoiserMode::Off;

    // UPSCALING.2B camera jitter (RendererOptions::pathTraceCameraJitter). Applies only to raster-primary path-traced
    // frames. The phase advances once per jittered recording and wraps; switching the option, a scene resize and
    // ResetSceneHistory return it to 0. Switching the option also discards every path-tracing history.
    void ApplyCameraJitter(bool requested);
    // This recording's jitter (0, 0 and inactive unless jitter applies) for a sequence of phaseCount phases; advances
    // the phase. A different phaseCount than the previous jittered frame's restarts the sequence at phase 0.
    CameraJitterStatus NextCameraJitter(bool applies, std::uint32_t phaseCount);
    std::uint32_t m_cameraJitterPhaseCount = CameraJitterPhaseCount;
    bool m_cameraJitterOption = false;
    std::uint32_t m_cameraJitterPhase = 0;
    CameraJitterStatus m_cameraJitterStatus;
    // Diagnostics only: this offset (render pixels) on every jittered frame in place of the sequence.
    std::optional<std::array<float, 2>> m_cameraJitterForTesting;
    // AIR.4F F4.4.1: the last recorded frame's mist request (none: no mist requested), for checking what the Engine passed.
    std::optional<AirMistRenderRequest> m_lastAirMistRequestForTesting;

    void UpdateSharcStatus(unsigned int frameSlot);

    // NVIDIA SHARC Checkpoint A (Documentation/Rendering/NVIDIA_SHARC_Integration_A.md): the cache, its allocation state
    // and its validity rules. Only one of the two caches has resources at a time.
    struct NvidiaSharcSemantics
    {
        std::uint64_t sceneResourceLifetime = 0;
        unsigned int environmentTextureIndex = UINT_MAX;
        unsigned int environmentConditionalCDFIndex = UINT_MAX;
        std::uint32_t maxPathDepth = 0;
        std::uint32_t russianRouletteDepth = 0;
        PathTraceRadianceCacheImplementation implementation = PathTraceRadianceCacheImplementation::Nvidia;

        bool operator==(const NvidiaSharcSemantics&) const = default;
    };

    bool NvidiaSharcSelected() const noexcept
    {
        return m_sharcSettings.enabled && m_sharcSettings.implementation == SharcImplementation::Nvidia;
    }

    // Why NVIDIA SHARC cannot run here (capabilities, deployed headers), checked once; empty when it can.
    const std::string& NvidiaSharcCapabilityFailure();
    void UpdateNvidiaSharc(bool enabled);
    // After the scene-lighting latch: clears the cache when what its radiance means changed (scene lifetime, environment
    // replacement, path estimator). Moving objects and lights, light and material edits and residency changes do not clear
    // it: NVIDIA's accumulation window and stale-entry eviction adapt to them.
    void ApplyNvidiaSharcValidity(const PathTraceAccumulationKey& key);
    SharcCache::Constants MakeNvidiaSharcConstants(const Vector3& cameraPosition, const PathTraceReferenceSettings& settings);

    SharcCache m_sharcNvidia;
    bool m_sharcNvidiaFailed = false;
    std::optional<std::string> m_sharcNvidiaCapabilityFailure;
    std::optional<NvidiaSharcSemantics> m_sharcNvidiaSemantics;
    std::uint64_t m_sharcNvidiaEpochAdvances = 0;
    std::optional<Vector3> m_sharcNvidiaPreviousCamera;
    std::uint32_t m_sharcNvidiaFrameIndex = 0;
    // Diagnostics: the next allocation fails; a forced capability failure; precision overrides.
    bool m_sharcNvidiaFailAllocationForTesting = false;
    std::optional<std::string> m_sharcNvidiaCapabilityFailureForTesting;
    std::optional<float> m_sharcNvidiaRadianceScaleForTesting;
    float m_sharcNvidiaDepositLuminanceLimitForTesting = 0.0f;
    std::uint32_t m_sharcNvidiaQueryVertexForTesting = 2;
    std::uint32_t m_sharcNvidiaTrainingMaxDepthForTesting = 8;
    // The query-side library the frame's settings select (Query, QueryTermination or Render). NRD.RELAX.2B:
    // rawDiffuseHitDistance selects the render library's raw-distance build for an extraction frame.
    // The render library SHARC substitutes cached radiance with (SHARC.4E, 4E.1, 5B; NRD.RELAX.2B raw distance).
    static PathTraceRadianceCacheLibrary SharcRenderLibrary(bool extract, bool splitReSTIRDI, bool rawDiffuseHitDistance);
    PathTraceRadianceCacheLibrary RadianceCacheQueryLibrary(bool extract, bool splitReSTIRDI,
        bool rawDiffuseHitDistance = false) const;
    SharcSettings m_sharcSettings;
    SharcStatus m_sharcStatus;
    // Diagnostics only (SHARC.5B): the render library's queryMode in place of 1 (2: the cached part zeroed, 3: also the
    // miss term, 4: the cached part unclamped), to compare a terminated path's terms one by one. 0 in production.
    std::uint32_t m_sharcDiagnosticQueryMode = 0;
    // RTXDI.1 diagnostics: initial RTXDI candidate counts in place of RtxdiInitialLocalLightSamples and
    // RtxdiInitialInfiniteLightSamples (0 = the production constant).
    std::uint32_t m_rtxdiLocalLightSamplesForTesting = 0;
    // WNP.2: shadow-test the selected initial ReSTIR DI sample at the primary vertex and discard it when occluded, before
    // reuse (RTXDI enableInitialVisibility). Costs one shadow ray per ReSTIR DI sample. False restores the previous
    // visibility-blind initial sampling exactly.
    bool m_rtxdiInitialVisibility = true;
    std::uint32_t m_rtxdiInfiniteLightSamplesForTesting = 0;
    // PT.2A: keep ReSTIR DI history through transform-only TLAS updates (moving objects). Reused samples are still shaded
    // with current visibility; bias-correction visibility is traced from the previous surfaces through the current TLAS
    // (RtxdiReuseBridge.hlsli). False restores the pre-PT.2A policy, which discarded all history whenever any transform
    // changed.
    bool m_rtxdiPreserveHistoryThroughTransforms = true;
    // PT.2A: the final shading ray reuses the initial-visibility result when it is provably the same ray (same surface,
    // bit-identical direction and distance, same TLAS within the dispatch). False always traces it.
    bool m_rtxdiReuseInitialVisibility = true;
    // PT.2A diagnostics: where the final ray is reused, trace it anyway and count disagreements (RTXDI diagnostic counter
    // finalReuseMismatches); shading then uses the traced result.
    bool m_rtxdiVerifyInitialVisibilityReuseForTesting = false;
    // PT.2A diagnostics: a RWByteAddressBuffer UAV of the RTXDI_DIAGNOSTIC_* words (PathTraceReference.hlsl) the ReSTIR DI
    // direct pass adds to. UINT_MAX (production) counts nothing.
    std::uint32_t m_rtxdiDiagnosticCountersIndexForTesting = UINT_MAX;
    // M4.3 ReGIR: whether ReSTIR DI uses ReGIR candidates where they are available (M4.3.5: on by default above
    // ReGIRSettings::localLightThreshold local lights; ENGINE_REGIR=0 disables it; the harness toggles it), and whether the
    // last recorded frame's direct pass used them.
    bool m_regirRequestedForTesting = PathTraceReferencePass::ReGIRLibrariesRequested();
    bool m_regirUsedLastFrameForTesting = false;
    // M4.3 harness: render requested ReSTIR DI frames in the Raster Primary mode without ReSTIR DI (the exhaustive reference).
    bool m_rasterPrimaryInsteadOfReSTIRForTesting = false;
    // M4.3.5 harness: the next frame whose direct pass records ReGIR (presampling recorded) is discarded through
    // AbortTrackedRecording before submission, as a recording failure would be; Render then reports it Deferred (nothing
    // executed or presented) instead of Error, so the Editor keeps running. Cleared when it fires; m_abortedFrameForTesting
    // records whether the discard was confirmed.
    bool m_abortFrameAfterReGIRForTesting = false;
    // The control case: false fires on the next path-traced frame whether or not it records ReGIR.
    bool m_abortFrameRequiresReGIRForTesting = true;
    bool m_abortedFrameForTesting = false;
    std::uint32_t m_abortedFramesForTesting = 0;
    // R.1 harness: the next frame with an armed displacement exit scope fails after its commands are recorded (before the
    // end-of-frame staging); the scope discards it. Render reports it Deferred when the discard is confirmed.
    bool m_failDisplacementFrameAfterRecordingForTesting = false;
    bool m_injectedFrameFailureForTesting = false;
    // M4.3.5 timing: the last frame's PrepareReGIRFrame CPU time (eligibility, constants, presampling dispatch record).
    double m_regirPrepareMillisecondsForTesting = 0.0;
    // M4.3: ReGIR settings (window, slots, candidates; ENGINE_REGIR_* overrides at startup) and the frame's presampling.
    ReGIRSettings m_regirSettings;
    // PT.2B: a light that moved or changed keeps its ReSTIR DI samples (translated by stable object ID; bias correction
    // evaluates them with the light's previous record). False restores the pre-PT.2B rule: any change of a light's
    // parameters drops its samples.
    bool m_rtxdiPreserveHistoryThroughLightChanges = true;
    // PT.2B diagnostics: the RTXDI light layout of another environment strategy, for a build whose path tracer libraries
    // were compiled with that strategy (RtxdiEnvironmentConfig.hlsli swapped in the build's assets). Unset in production,
    // which always uses CompiledRtxdiEnvironmentStrategy().
    std::optional<RtxdiEnvironmentStrategy> m_rtxdiEnvironmentStrategyForTesting;
    // PT.2B diagnostics, Reservoir strategy: environment and BSDF candidate counts in place of
    // RtxdiInitialEnvironmentSamples and RtxdiInitialBrdfSamples (UINT_MAX = the production constant).
    std::uint32_t m_rtxdiEnvironmentSamplesForTesting = UINT_MAX;
    std::uint32_t m_rtxdiBrdfSamplesForTesting = UINT_MAX;
    // Whether the last path-traced frame rendered with the cached-radiance library.
    bool m_sharcRenderedLastFrame = false;
    // SHARC.6A: this frame's render dispatch writes the history prior the temporal pass reads.
    bool m_sharcHistoryPriorWritten = false;
    // Its query binding's constants, which the temporal pass reads to look the prior up.
    D3D12_GPU_VIRTUAL_ADDRESS m_sharcHistoryPriorBinding = 0;
    // SHARC.4D-R (diagnostics): the query library's binding for the frame (0: render with the reference library).
    D3D12_GPU_VIRTUAL_ADDRESS PrepareRadianceCacheQuery(ID3D12GraphicsCommandList* commandList, bool extract,
        bool splitReSTIRDI, bool rawDiffuseHitDistance = false);
    // The learning library of the frame PrepareRadianceCacheLearning prepared.
    PathTraceRadianceCacheLibrary m_radianceCacheLearningLibrary = PathTraceRadianceCacheLibrary::Training;
    // Diagnostics (SHARC.4D-Q): the library Insertion::Deferred frames learn with (Deferred or DeferredLate).
    PathTraceRadianceCacheLibrary m_radianceCacheDeferredLibrary = PathTraceRadianceCacheLibrary::Deferred;
    // Diagnostics fault injection: the next record-buffer allocation fails.
    bool m_radianceCacheRecordsFailAllocationForTesting = false;
    // Whether the last path-traced frame recorded learning (the cache library and a resolve).
    bool m_radianceCacheLearnedLastFrame = false;
    bool m_radianceCacheLearningFailureLogged = false;
    // RTXPT D.1A: a fixed seed for every accumulation run (diagnostics only). Empty in production, where each run
    // takes PathTraceSequenceSeed of its first frame's serial relative to m_pathTraceSequenceOrigin, so a moving
    // camera, which restarts accumulation every frame, no longer repeats one random sequence.
    std::optional<std::uint32_t> m_pathTraceRandomSeed;
    // The seed of the current accumulation run, latched when accumulation restarts.
    std::uint32_t m_pathTraceRunSeed = 0;
    // m_gpuProfilerFrameSerial at the last ResetSceneHistory: the first run after a reset uses seed 0.
    std::uint64_t m_pathTraceSequenceOrigin = 0;
    // RTXPT D.1B.2 diffuse/specular signal extraction: requested by diagnostics only (never by default or by any
    // setting). Raster Primary and split ReSTIR DI frames then record the extraction exports, which accumulate
    // exactly as the production ones and also fill the pass's diffuse and specular outputs.
    bool m_pathTraceSignalExtraction = false;
    // Whether the last recorded path-tracing frame ran the extraction exports.
    bool m_pathTraceExtractedLastFrame = false;
    // PSR.1a: whether the last path-traced frame recorded primary surface replacement, and the last reported reason it
    // could not (reported once per distinct reason).
    bool m_primarySurfaceReplacementLastFrame = false;
    std::string m_primarySurfaceReplacementFailure;
    // PSR.1a-P: whether any published material can produce a vertex-0 roughness at or below the delta threshold
    // (MaterialCanBeDeltaMirror), cached per scene-resource lifetime and material revision.
    std::pair<std::uint64_t, std::uint64_t> m_primarySurfaceReplacementMaterials{UINT64_MAX, UINT64_MAX};
    bool m_primarySurfaceReplacementPossible = true;
    // PSR.1b, captured by RecordRenderFrame when guides are requested: the unjittered previous camera raster motion uses
    // (before this frame overwrites it) and each render object's current-to-previous world transform, indexed by its
    // TLAS InstanceID (the render object index).
    Matrix4 m_psrGuidePreviousView;
    Matrix4 m_psrGuidePreviousViewProjection;
    bool m_psrGuidePreviousCameraValid = false;
    std::vector<PathTracePsrTransform> m_psrGuideTransforms;
    bool m_primarySurfaceReplacementGuidesLastFrame = false;
    std::string m_primarySurfaceReplacementGuideFailure;
    // NRD.3: requested by diagnostics only (never by default, any setting or the Editor). Each extraction frame then
    // also prepares REBLUR's inputs from the raw extraction and reconstructs the raw signals through an NRD bypass. It
    // reads the frame's signals and writes only its own textures: the image is unchanged. Frames without extraction
    // (denoiser Off) do not run it, since turning extraction on would itself change the accumulation's rounding.
    bool m_nrdPathTraceDiagnostic = false;
    // Whether the last recorded path-tracing frame ran it.
    bool m_nrdPathTraceDiagnosticRanLastFrame = false;
    // NRD.RELAX.2: requested by diagnostics only, like m_nrdPathTraceDiagnostic. Each extraction frame then also writes the
    // raw diffuse hit distance and prepares RELAX's inputs from it and the raw extraction, with their bypass
    // reconstruction. Reads the frame's signals and writes only its own textures: the image and REBLUR are unchanged.
    bool m_nrdRelaxPrepareDiagnostic = false;
    // Whether the last recorded path-tracing frame wrote the raw diffuse hit distance (NRD.RELAX.2B: through a
    // raw-distance extraction pipeline, PathTraceReferencePass::WroteRawDiffuseHitDistance), and ran the RELAX
    // preparation.
    bool m_pathTraceRawDiffuseHitDistanceLastFrame = false;
    // NRD.RELAX.4: the last reported raw-distance extraction failure (logged once per distinct reason).
    std::string m_rawDiffuseHitDistanceFailure;
    bool m_nrdRelaxPrepareDiagnosticRanLastFrame = false;
    // NRD.4A: the diagnostic's fixed radiance scale (NrdPathTracePrepareConstants::radianceScale). Diagnostics only; a
    // denoiser history would need a restart whenever it changed.
    float m_nrdPathTraceRadianceScale = 64.0f;
    // RTXPT D.2B temporal reconstruction, requested by diagnostics only and only effective with extraction: each
    // extraction frame then reprojects and accumulates the diffuse and specular signals into temporal history.
    // m_pathTraceTemporalPresentation additionally presents that history (diffuse + specular on surface pixels) in
    // place of the progressive mean; the accumulation itself is never touched.
    bool m_pathTraceTemporal = false;
    bool m_pathTraceTemporalPresentation = false;
    // Whether the last recorded path-tracing frame ran the temporal pass.
    bool m_pathTraceTemporalRanLastFrame = false;
    // RTXPT D.3B spatial filtering, requested by diagnostics only and only effective on frames that ran the temporal
    // pass: it filters this frame's temporal outputs and presents the result (remodulated diffuse + specular on
    // surface pixels) instead of the temporal presentation. Without temporal reconstruction it does nothing. It never
    // writes temporal history.
    bool m_pathTraceSpatial = false;
    // Whether the current recording intends to run the spatial pass (decided before the temporal pass, whose own
    // presentation it replaces), and whether the last recorded path-tracing frame ran it.
    bool m_pathTraceSpatialRequested = false;
    bool m_pathTraceSpatialRanLastFrame = false;
    // RTXPT D.3B.0 diagnostic view of the frame being recorded (from the debug UI), and whether the last recorded
    // path-tracing frame presented a scalar view (shown without tone mapping). A view other than Normal runs the
    // spatial dispatch for display only, on frames that ran the temporal pass; it changes no history.
    PathTraceDenoiserVisualization m_pathTraceVisualization{};
    // PS.2B: this frame's requested view (RendererOptions::pathTraceDenoiserVisualization).
    PathTraceDenoiserVisualization m_requestedPathTraceVisualization{};
    bool m_pathTraceScalarViewLastFrame = false;
#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
    bool m_sharcDiagnosticPresentedThisFrame = false;
#endif
    // RTXPT D.3C.2 diagnostic: the spatial filter's a-trous iterations (0: the D.3B.2 5x5 pass; 1 or 2: 3x3
    // iterations at steps 1 and 2). It only matters while the spatial pass runs; without the a-trous pipelines or
    // intermediate the 5x5 pass is used. The count that ran on the last spatial frame (0 for the 5x5 pass) is kept
    // for diagnostics.
    std::uint32_t m_pathTraceSpatialIterations = 2;
    std::uint32_t m_pathTraceSpatialIterationsLastFrame = 0;
    // DENOISE.1 stage isolation, applied when RendererOptions::pathTraceDenoiserStages changes: the spatial filter
    // sets m_pathTraceSpatialIterations or, when off, m_pathTraceSpatialBypass (the temporal pass presents);
    // m_pathTraceTemporalHistoryDisabled starts new history every frame. Switching either, or a reset request,
    // discards the history.
    void ApplyPathTraceDenoiserStages(const PathTraceDenoiserStages& stages);
    PathTraceDenoiserStages m_pathTraceDenoiserStages{};
    bool m_pathTraceSpatialBypass = false;
    bool m_pathTraceTemporalHistoryDisabled = false;
    // DENOISE.3B extended diffuse history (experimental, PathTraceDenoiserStages::extendedDiffuseHistory): the temporal
    // pass keeps a second diffuse running mean (up to PathTraceExtendedMaximum frames) that restarts whenever the scene
    // or view may have changed, i.e. on an accumulation restart, invalid temporal history or a scene-lighting epoch
    // change. After more than PathTraceExtendedStableFrames frames without a restart it replaces the diffuse history as
    // the presentation's and the spatial pass's input. m_pathTraceExtendedFrames: frames since the last restart (0: not
    // running); m_pathTraceExtendedUsed: whether the frame being recorded uses it.
    static constexpr std::uint32_t PathTraceExtendedStableFrames = 32;
    static constexpr float PathTraceExtendedMaximum = 256.0f;
    bool m_pathTraceExtendedDiffuse = false;
    std::uint32_t m_pathTraceExtendedFrames = 0;
    std::uint64_t m_pathTraceExtendedEpoch = 0;
    bool m_pathTraceExtendedUsed = false;

    // What temporal history depends on beyond geometry: a change of any of it invalidates the history. Lighting is
    // compared exactly; a continuous change cannot be told from an abrupt one here, so both invalidate.
    struct PathTraceTemporalKey
    {
        PathTracePrimary primary = PathTracePrimary::Raster;
        std::uint32_t samplesPerFrame = 0;
        std::uint32_t maxPathDepth = 0;
        std::uint32_t russianRouletteDepth = 0;
        DirectionalLight sun;
        SceneLightTable lights{};
    };

    std::optional<PathTraceTemporalKey> m_pathTraceTemporalKey;
    // The temporal key of a frame and whether two keys describe the same signal (shared by the Engine temporal pass and
    // NRD.4B REBLUR, so both restart on the same changes).
    static PathTraceTemporalKey MakePathTraceTemporalKey(
        PathTracePrimary primary,
        const FrameLighting& frameLighting,
        const PathTraceReferenceSettings& settings);
    static bool SamePathTraceTemporalKey(const PathTraceTemporalKey& previous, const PathTraceTemporalKey& key);

    // NRD.4B path-traced REBLUR (PathTraceDenoiserMode::NrdReblur): one REBLUR_DIFFUSE_SPECULAR instance at the render
    // extent, created on the first REBLUR frame, released (one GPU wait) on the first frame of any other mode, and on
    // resize (recreated with CLEAR_AND_RESTART by the next REBLUR frame). A creation failure is sticky until the next
    // resize or mode change; REBLUR frames then fall back to the Engine denoiser, frame by frame.
    NrdDenoiser m_nrdPathTraceDenoiser;
    bool m_nrdPathTraceCreationFailed = false;
    std::uint64_t m_nrdPathTraceCreations = 0;
    // The reason REBLUR last could not denoise (empty while it can); warned once per distinct reason.
    std::string m_nrdPathTraceFailure;
    // History: valid until any reset source clears it (the Engine temporal pass's sources, a mode change, a frame gap);
    // the instance is new after creation (CLEAR_AND_RESTART). Previous camera and jitter are those of the last submitted
    // REBLUR frame: the pending values are committed with the frame's submission and dropped with an abandoned one.
    bool m_nrdPathTraceHistoryValid = false;
    bool m_nrdPathTraceInstanceNew = false;
    bool m_nrdPathTraceDispatchedThisFrame = false;
    bool m_nrdPathTraceExecutedLastFrame = false;
    std::optional<PathTraceTemporalKey> m_nrdPathTraceKey;
    Matrix4 m_nrdPathTracePreviousView{};
    Matrix4 m_nrdPathTracePreviousProjection{};
    float m_nrdPathTracePreviousJitter[2] = {0.0f, 0.0f};
    Matrix4 m_nrdPathTracePendingView{};
    Matrix4 m_nrdPathTracePendingProjection{};
    float m_nrdPathTracePendingJitter[2] = {0.0f, 0.0f};
    std::uint32_t m_nrdPathTraceFrameIndex = 0;
    nrd::AccumulationMode m_nrdPathTraceAccumulationMode = nrd::AccumulationMode::CLEAR_AND_RESTART;
    // PSR.2 Stage 3B phase 4A: whether this frame's REBLUR runs the PSR prepare/resolve variant (replaced pixels lit by
    // the path tracer), and whether the last submitted REBLUR frame did. A change restarts history: the history of a
    // PSR frame holds replaced pixels as their virtual surfaces without the mirror's throughput.
    bool m_nrdPathTracePsrThisFrame = false;
    bool m_nrdPathTracePsrLastFrame = false;
    // Verification only, false in production: PSR frames of every NRD mode run without NRD's material comparison (every
    // minimum at the SDK default 4), to measure what the separation of replaced and ordinary history prevents.
    bool m_nrdPathTracePsrSeparationDisabledForTesting = false;
    // Phases 4B and 4C: as m_nrdPathTracePsrThisFrame / LastFrame, for RELAX and the hybrid.
    bool m_nrdRelaxPsrThisFrame = false;
    bool m_nrdRelaxPsrLastFrame = false;
    bool m_nrdHybridPsrThisFrame = false;
    bool m_nrdHybridPsrLastFrame = false;
    // Diagnostics, 0 in production: the hybrid's denoised resolve components (as m_nrdPathTraceResolveComponents: 0 both,
    // 1 diffuse only, 2 specular only).
    std::uint32_t m_nrdHybridResolveComponents = 0;
    // Whether the last recorded frame presented REBLUR's output (false after a frame of any other mode).
    bool m_nrdPathTracePresentedLastFrame = false;
    // NRD.4C diagnostics, unset/0 in production: REBLUR settings used in place of PathTraceNrdSettings (the caller
    // restarts history when it changes them), and the denoised resolve's components (NrdPathTracePrepareConstants::
    // resolveComponents: 0 both, 1 diffuse only, 2 specular only).
    std::optional<nrd::ReblurSettings> m_nrdPathTraceSettingsOverride;
    std::uint32_t m_nrdPathTraceResolveComponents = 0;
    // NRD.4C diagnostics: create the instance with fp32 internal textures (takes effect at its next creation).
    bool m_nrdPathTracePromoteFloat16To32 = false;
    // ER.1 Phase 3: the promotion the current (possibly retained) instance was created with.
    bool m_nrdPathTraceCreatedPromoteFloat16To32 = false;
    // NRD.4D diagnostics: NrdPathTracePrepareConstants::specularHitDistanceMode (0 in production).
    std::uint32_t m_nrdPathTraceSpecularHitDistanceMode = 0;
    // Diagnostics: NRD's validation overlay (CommonSettings::enableValidation) into this RGBA16F texture.
    bool m_nrdPathTraceValidation = false;
    Texture m_nrdPathTraceValidationTexture;

    // NRD.RELAX.3: requested by diagnostics only (never by default, a setting or the Editor). Each extraction frame that
    // wrote the raw diffuse hit distance then also dispatches RELAX_DIFFUSE_SPECULAR on the RELAX preparation and
    // resolves its output into m_nrdRelaxResolvedTexture. Nothing of it is presented: the image, REBLUR and the Engine
    // denoiser are unchanged. The instance exists only while requested (created on its first frame, released with a GPU
    // wait when cleared, on resize and at shutdown) and shares nothing with REBLUR's.
    // HYBRID.3 private opt-in, non-presenting diagnostic. HYBRID.5: PathTraceDenoiserMode::NrdHybrid presents the same
    // instance and preparation (owned while either is requested); the diagnostic never runs as well as the selected mode.
    bool m_nrdHybridDispatchDiagnostic = false;
    NrdDenoiser m_nrdHybridDenoiser;
    // A creation failure is sticky until the request is cleared, a mode change, a resize or shutdown.
    bool m_nrdHybridCreationFailed = false;
    std::uint64_t m_nrdHybridCreations = 0;
    // Submitted hybrid frames since startup.
    std::uint64_t m_nrdHybridSubmittedDispatches = 0;
    // HYBRID.5: whether the last recorded frame presented the hybrid's output (PathTraceDenoiserMode::NrdHybrid).
    bool m_nrdHybridPresentedLastFrame = false;
    // The reason the hybrid last could not run (empty while it can); logged once per distinct reason.
    std::string m_nrdHybridFailure;
    // History, as REBLUR's (m_nrdPathTraceHistoryValid): valid until a reset source clears it; the instance is new after
    // creation (CLEAR_AND_RESTART). Previous camera and jitter are those of the last submitted hybrid frame; the pending
    // values are committed with the frame's submission and dropped with an abandoned one.
    bool m_nrdHybridHistoryValid = false;
    bool m_nrdHybridInstanceNew = false;
    bool m_nrdHybridDispatchedThisFrame = false;
    bool m_nrdHybridExecutedLastFrame = false;
    std::optional<PathTraceTemporalKey> m_nrdHybridKey;
    Matrix4 m_nrdHybridPreviousView{};
    Matrix4 m_nrdHybridPreviousProjection{};
    float m_nrdHybridPreviousJitter[2] = {0.0f, 0.0f};
    Matrix4 m_nrdHybridPendingView{};
    Matrix4 m_nrdHybridPendingProjection{};
    float m_nrdHybridPendingJitter[2] = {0.0f, 0.0f};
    std::uint32_t m_nrdHybridFrameIndex = 0;
    nrd::AccumulationMode m_nrdHybridAccumulationMode = nrd::AccumulationMode::CLEAR_AND_RESTART;
    // Diagnostics only: the next creation fails as if the SDK had rejected it (the creation-failure path).
    bool m_nrdHybridFailCreationForTesting = false;
    // A diagnostic gap without destroying the owner; never a production setting.
    bool m_nrdHybridSkipFrame = false;
    bool m_nrdHybridFailSpecularForTesting = false;
    NrdPathTracePreparePass m_nrdHybridPreparePass;

    bool EnsureNrdHybridDenoiser();
    void ReleaseNrdHybridDenoiser();
    void ReportNrdHybridUnavailable(const std::string& reason);
    // As RELAX's (NRD.RELAX.4): Begin does everything fallible before anything is recorded; Record records preparation,
    // both filters and, with presentation, the production resolve. Only a complete pair resolves or commits.
    bool BeginNrdHybridFrame(
        PathTracePrimary primary,
        const Matrix4& view,
        const Matrix4& cameraProjection,
        const FrameLighting& frameLighting,
        const PathTraceReferenceSettings& settings);
    bool RecordNrdHybrid(
        ID3D12GraphicsCommandList* commandList,
        unsigned int frameIndex,
        const Matrix4& view,
        const Matrix4& inverseViewProjection,
        const Vector3& cameraPosition,
        Texture* presentation);
    bool RecordNrdHybridDiagnostic(
        ID3D12GraphicsCommandList* commandList,
        unsigned int frameIndex,
        PathTracePrimary primary,
        const Matrix4& view,
        const Matrix4& cameraProjection,
        const Matrix4& inverseViewProjection,
        const Vector3& cameraPosition,
        const FrameLighting& frameLighting,
        const PathTraceReferenceSettings& settings);

    bool m_nrdRelaxDispatchDiagnostic = false;
    NrdDenoiser m_nrdRelaxDenoiser;
    // RGBA16F at the scene extent: remodulated RELAX radiance on surface pixels (alpha 1), zero elsewhere.
    Texture m_nrdRelaxResolvedTexture;
    // A creation failure is sticky until the request is cleared, a resize or shutdown.
    bool m_nrdRelaxCreationFailed = false;
    std::uint64_t m_nrdRelaxCreations = 0;
    // Submitted RELAX frames since startup.
    std::uint64_t m_nrdRelaxSubmittedDispatches = 0;
    // The reason RELAX last could not run (empty while it can); logged once per distinct reason.
    std::string m_nrdRelaxFailure;
    // History, as REBLUR's (m_nrdPathTraceHistoryValid): valid until a reset source clears it; the instance is new after
    // creation (CLEAR_AND_RESTART). Previous camera and jitter are those of the last submitted RELAX frame; the pending
    // values are committed with the frame's submission and dropped with an abandoned one.
    bool m_nrdRelaxHistoryValid = false;
    bool m_nrdRelaxInstanceNew = false;
    bool m_nrdRelaxDispatchedThisFrame = false;
    bool m_nrdRelaxExecutedLastFrame = false;
    std::optional<PathTraceTemporalKey> m_nrdRelaxKey;
    Matrix4 m_nrdRelaxPreviousView{};
    Matrix4 m_nrdRelaxPreviousProjection{};
    float m_nrdRelaxPreviousJitter[2] = {0.0f, 0.0f};
    Matrix4 m_nrdRelaxPendingView{};
    Matrix4 m_nrdRelaxPendingProjection{};
    float m_nrdRelaxPendingJitter[2] = {0.0f, 0.0f};
    std::uint32_t m_nrdRelaxFrameIndex = 0;
    nrd::AccumulationMode m_nrdRelaxAccumulationMode = nrd::AccumulationMode::CLEAR_AND_RESTART;
    // Diagnostics only: the next creation fails as if the SDK had rejected it (the creation-failure path).
    bool m_nrdRelaxFailCreationForTesting = false;
    // NRD.RELAX.4: whether the last recorded frame presented RELAX's output (PathTraceDenoiserMode::NrdRelax).
    bool m_nrdRelaxPresentedLastFrame = false;
    // NRD.RELAX.4 diagnostics, unset in production: RELAX settings used in place of PathTraceRelaxSettings (the caller
    // restarts history when it changes them).
    std::optional<nrd::RelaxSettings> m_nrdRelaxSettingsOverride;
    // NRD RELAX R.4 diagnostics, 0 in production: ENGINE_NRD_RELAX_SIGNAL_VIEW, read at startup. RELAX still runs; its
    // production resolve is replaced by one diffuse signal (NrdRelaxSignalViewConstants::mode).
    std::uint32_t m_nrdRelaxSignalView = 0;
    // Whether the signal view's fallback to the production resolve has been reported (once per run).
    bool m_nrdRelaxSignalViewFallbackReported = false;
    // NRD RELAX R.9A diagnostics, 0 in production: ENGINE_NRD_RELAX_HISTORY_CONFIDENCE, read at startup. 1 binds a
    // constant diffuse history confidence of 1, 2 of 0.1; the specular confidence is always 1. Regular RELAX only.
    std::uint32_t m_nrdRelaxHistoryConfidence = 0;
    // 1x1 R16F constants for IN_DIFF/SPEC_CONFIDENCE, created on the first confidence frame, refilled by a clear each
    // such frame, released with the RELAX instance. m_nrdRelaxConfidenceOne also serves IN_SPEC_CONFIDENCE.
    RenderTarget m_nrdRelaxConfidenceOne;
    RenderTarget m_nrdRelaxConfidenceLow;
    bool m_nrdRelaxConfidenceReported = false;
    // NRD RELAX R.9B diagnostics, off in production: ENGINE_NRD_RELAX_CONFIDENCE_PROBES=1, read at startup. Generates the
    // paired-visibility confidence on raster ReSTIR DI frames under the NRD RELAX denoiser, for signal views 4, 5 and 6;
    // bound to NRD only under m_nrdRelaxGeneratedConfidence (which also sets this).
    bool m_nrdConfidenceProbesEnabled = false;
    // NRD RELAX R.9C diagnostics, off in production: ENGINE_NRD_RELAX_GENERATED_CONFIDENCE=1, read at startup. Enables
    // the probes and binds their confidence as regular RELAX's IN_DIFF_CONFIDENCE (specular 1). A frame without this
    // frame's probes binds 1. Takes precedence over (and disables) ENGINE_NRD_RELAX_HISTORY_CONFIDENCE.
    bool m_nrdRelaxGeneratedConfidence = false;
    bool m_nrdRelaxGeneratedConfidenceFallbackReported = false;
    // R.9F startup diagnostic: 1 preserves R.9C; 2/4/6 exponentiate its filtered confidence, generated mode only.
    std::uint32_t m_nrdRelaxConfidenceStrength = 1;
    NrdHistoryConfidencePass m_nrdConfidencePass;
    // What the probe records were made under. A record is reused only by the immediately next submitted probe frame
    // under an equal identity: scene resources, lights, sun, environment and render extent. TLAS updates (moving
    // objects) do not change it: detecting their visibility changes is the purpose.
    struct NrdConfidenceProbeIdentity
    {
        std::uint64_t sceneResourceLifetime = 0;
        PathTraceTemporalKey lights;
        std::uint32_t environment[5]{};
        float environmentTotalWeight = 0.0f;
        unsigned int width = 0;
        unsigned int height = 0;
    };
    std::optional<NrdConfidenceProbeIdentity> m_nrdConfidencePendingIdentity;
    std::optional<NrdConfidenceProbeIdentity> m_nrdConfidenceCommittedIdentity;
    float m_nrdConfidencePendingCamera[3]{};
    float m_nrdConfidenceCommittedCamera[3]{};
    bool m_nrdConfidenceDispatchedThisFrame = false;
    bool m_nrdConfidenceExecutedLastFrame = false;
    std::uint32_t m_nrdConfidenceFrameIndex = 0;
    // The last logged history state, so the log notes changes only.
    std::string m_nrdConfidenceHistoryState;
    // NRD.RELAX.4 diagnostics: create the RELAX instance with fp32 internal textures (takes effect at its next creation).
    bool m_nrdRelaxPromoteFloat16To32 = false;
    // ER.1 Phase 3: the promotion the current (possibly retained) instance was created with.
    bool m_nrdRelaxCreatedPromoteFloat16To32 = false;

    // The production REBLUR settings (diagnostics compare overrides against them).
    static nrd::ReblurSettings PathTraceNrdProductionSettings();
    bool EnsurePathTraceNrdDenoiser();
    // Waits for the GPU, then releases the instance and the validation texture when they exist.
    void ReleasePathTraceNrdDenoiser();
    void ReportPathTraceNrdUnavailable(const std::string& reason);
    // Everything that can fail before recording: the instance, the preparation textures and the frame's NRD settings.
    // False: this frame falls back to the Engine denoiser.
    bool BeginPathTraceNrdFrame(
        PathTracePrimary primary,
        const Matrix4& view,
        const Matrix4& cameraProjection,
        const FrameLighting& frameLighting,
        const PathTraceReferenceSettings& settings);
    // Records preparation, REBLUR and the production resolve into the presentation, after BeginPathTraceNrdFrame.
    void RecordPathTraceNrd(
        ID3D12GraphicsCommandList* commandList,
        unsigned int frameIndex,
        const Matrix4& view,
        const Matrix4& inverseViewProjection,
        const Vector3& cameraPosition);
    // The preparation constants shared by REBLUR and the NRD.3 diagnostic; also makes the G-buffer and raw extraction
    // shader-readable.
    NrdPathTracePrepareConstants MakeNrdPathTracePrepareConstants(
        ID3D12GraphicsCommandList* commandList,
        const Matrix4& view,
        const Matrix4& inverseViewProjection,
        const Vector3& cameraPosition);
    // Samples recorded by the current frame; added to the accumulated count only after the frame
    // records and submits successfully (Render's Rendered path).
    std::uint32_t m_pathTracePendingSamples = 0;

    // Returns true when the reference pass exists or was created now; records the failure otherwise.
    bool EnsurePathTraceReference();

    // Async DXR preparation (Gate 2), render thread only.
    PathTracePrimary GetLightingModePrimary(RendererLightingMode mode) const;
    bool IsLightingModeReady(RendererLightingMode mode) const;
    ShaderLibraryPreparation PrepareLightingModeLibraries(RendererLightingMode mode);
    void CreateLightingModeResources(RendererLightingMode mode);

    // The raster-primary modes' capability, evaluated on frames that request them. RTXPT.2: the shared prerequisites
    // (rasterPrimary), which own no resources beyond the Raster Primary pipeline in the reference pass. RTXPT.3: ReSTIR DI
    // re-evaluates them, then its own pipelines and the RTXDI resources (restirDI; allocated on first request).
    PathTraceRasterPrimaryStatus m_rasterPrimaryStatus;
    void EvaluatePathTraceRasterPrimary();
    void EvaluatePathTraceReSTIRDI();
    void EvaluatePathTraceReSTIRPT();

    // Prepares this recording's RTXDI frame (light set, SDK buffer indices, reuse inputs, constants b2) and
    // leaves the reservoir buffer in UNORDERED_ACCESS. The frame stays pending until CommitFrame after
    // submission. view is this frame's camera view.
    bool PrepareRtxdiFrame(ID3D12GraphicsCommandList* commandList, const FrameLighting& frameLighting,
        const PathTraceReferenceConstants& constants, const Matrix4& view,
        D3D12_GPU_VIRTUAL_ADDRESS& rtxdiConstantsAddress);

    // Primary of the reference mode. Camera always, except in diagnostics that measure raster-primary
    // agreement against unjittered PixelCenter rays; the Editor never changes it.
    PathTracePrimary m_pathTraceReferencePrimary = PathTracePrimary::Camera;
    // Async DXR preparation (Gate 2). The worker compiles DXIL only; it is joined in Shutdown before the
    // pass that accepts its results.
    std::unique_ptr<ShaderLibraryCompileWorker> m_shaderCompileWorker;
    bool m_shaderCompileWorkerRunning = false;
    // PS.5B.1: DXR libraries and pipelines are prepared off the render thread (the Editor's compile worker, or a
    // packaged game's pipeline worker over its cooked libraries); lighting modes then activate only once ready.
    bool m_asynchronousDxrPreparation = false;
    // ER.1 Phase 4: DXR pipeline creation off the render thread (state objects and shader tables); joined in Shutdown
    // before the pass and the device.
    std::unique_ptr<RayTracingPipelineWorker> m_pipelineWorker;
    bool m_pipelineWorkerRunning = false;
    // BT.3: reset at the start of each Render and filled by the preparation gates; observation only.
    DxrPreparationFrameReport m_dxrPreparationReport;
    // A failure counts only when the reporting call itself took a worker result: an unchanged failure re-reported on
    // every poll (a sticky SHARC or PSR failure, say) must not fail an unrelated batch.
    void RecordDxrPreparation(const ShaderLibraryPreparation& preparation, bool pipelinePhase, bool tookResult);
    std::uint64_t GetDxrResultsTaken() const;
    LightingModeTransition m_lightingModeTransition;
    // ER.1 Phase 2: DXR libraries the worker has prepared and the pass holds (or, for a RayQuery twin, that failed
    // for their current sources). Gates skip polling them, so a settled configuration polls nothing per frame.
    std::vector<std::filesystem::path> m_preparedDxrLibraries;
    // ER.1 Phase 2: the requested NRD denoiser or SHARC whose libraries are still compiling (or failed); the
    // effective setting stays the previous one meanwhile. Empty when nothing is pending.
    std::string m_denoiserPreparation;
    std::string m_sharcPreparation;
    PathTraceLibraryNeeds GetPathTraceLibraryNeeds(RendererLightingMode mode, PathTraceDenoiserMode denoiser,
        bool sharc) const;
    // Polls the worker for every library of the configuration that is not prepared yet, never blocking. Failed
    // outranks Compiling, which outranks Queued, which outranks Ready; an optional twin's failure counts as Ready.
    ShaderLibraryPreparation PrepareDxrLibraries(const PathTraceLibraryNeeds& needs);
    // The same preparation for every lighting mode a frame may record (the requested one and, while it prepares,
    // the active one); Legacy needs none.
    ShaderLibraryPreparation PrepareDxrLibraries(RendererLightingMode requestedMode, PathTraceDenoiserMode denoiser,
        bool sharc);
    // The effective denoiser and SHARC for this frame: the request once its libraries are prepared, otherwise the
    // previous effective setting.
    PathTraceDenoiserMode GateDenoiserRequest(RendererLightingMode requestedMode, PathTraceDenoiserMode requested);
    bool GateSharcRequest(RendererLightingMode requestedMode, PathTraceDenoiserMode denoiser, bool requested);
    // ER.1 Phase 3.1: inactive NRD resources retained for fast switching, evicted under video-memory pressure. A
    // group is evicted only when it is inactive, its last submitted use has completed and no unsubmitted or uncertain
    // recording referenced it; the eviction never waits for the GPU.
    // M0c.3 Phase B: the groups also retire after the shared grace period, through the residency policy Legacy and Path
    // Tracing use.
    enum class NrdRetentionGroup
    {
        // RELAX instance, its resolved output, confidence inputs and preparation pass.
        Relax,
        // The path tracer's raw diffuse hit distance texture and extraction pipelines, shared by RELAX and the hybrid.
        RawDiffuseHitDistance,
        // Path-traced REBLUR instance and its preparation textures.
        PathTraceReblur,
        // Path-traced hybrid instance (RELAX diffuse + REBLUR specular) and its preparation textures.
        Hybrid,
        // The Engine denoiser: the path tracer's temporal history and spatial intermediates.
        EngineDenoiser,
        // Legacy reflection and GI REBLUR pair.
        Legacy,
        Count
    };

    struct NrdRetentionState
    {
        // Use, last-use fence, grace period and counters (retirements are evictions; deferrals were postponed because the
        // last use had not completed yet). `resident` mirrors whether the group holds anything.
        ModeResourceResidency residency;
        // Least-recently-used order (render frame serial of the last frame the group was active).
        std::uint64_t lastActiveFrame = 0;
    };

    struct NrdRetentionStatus
    {
        bool budgetAvailable = false;
        bool underPressure = false;
        std::uint64_t budgetBytes = 0;
        std::uint64_t usageBytes = 0;
    };

    // Enter pressure below 10% local-budget headroom, leave it above 20% (hysteresis).
    static constexpr double NrdRetentionEvictHeadroom = 0.10;
    static constexpr double NrdRetentionResumeHeadroom = 0.20;
    std::array<NrdRetentionState, static_cast<size_t>(NrdRetentionGroup::Count)> m_nrdRetention{};
    NrdRetentionStatus m_nrdRetentionStatus;
    IDXGIAdapter3* m_videoMemoryAdapter = nullptr;
    bool m_videoMemoryAdapterQueried = false;
    std::uint64_t m_nrdRetentionFrame = 0;
    // Diagnostics only: behave as if DXGI reported no budget (the one-inactive-group fallback). The headroom override is the
    // shared m_modeResourceHeadroomOverrideForTesting.
    bool m_nrdRetentionBudgetUnavailableForTesting = false;
    // NRD's own report (persistent + transient pools) for the instances of the last evicted group.
    double m_nrdRetentionLastEvictedReportedMegabytes = 0.0;
    bool IsNrdRetentionGroupAllocated(NrdRetentionGroup group) const;
    bool IsNrdRetentionGroupActive(NrdRetentionGroup group, bool runPathTrace) const;
    bool IsNrdRetentionGroupTransitionPending(NrdRetentionGroup group, bool runPathTrace) const;
    // Marks this recording's active groups, then evicts at most one inactive group if the policy asks for it.
    void UpdateNrdRetention(bool runPathTrace);
    // After a confirmed submission: the active groups' last use is that submission's fence.
    void CommitNrdRetentionSubmission();
    bool EvictNrdRetentionGroup(NrdRetentionGroup group, bool underPressure);
    bool QueryVideoMemoryHeadroom(double& headroom);
    // Records one progressive iteration (or holds a converged image) and presents it as scene colour.
    // projection is the frame's rendering projection (jittered when UPSCALING.2B jitter is active): every G-buffer
    // reconstruction uses it. cameraProjection is the unjittered one and identifies the camera in the accumulation key.
    bool RecordPathTraceReference(
        ID3D12GraphicsCommandList* commandList,
        unsigned int frameIndex,
        PathTracePrimary primary,
        const Matrix4& view,
        const Matrix4& projection,
        const Matrix4& cameraProjection,
        const Matrix4& inverseViewProjection,
        const Vector3& cameraPosition,
        const FrameLighting& frameLighting,
        D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstantsAddress,
        const PathTraceReferenceSettings& settings);
    static bool SamePathTraceAccumulationKey(const PathTraceAccumulationKey& left, const PathTraceAccumulationKey& right);
    // RTXPT D.2B: records the temporal pass after an extraction frame; false when it could not run (the caller then
    // invalidates temporal history).
    bool RecordPathTraceTemporal(
        ID3D12GraphicsCommandList* commandList,
        PathTracePrimary primary,
        const Matrix4& view,
        const Matrix4& projection,
        const Vector3& cameraPosition,
        const FrameLighting& frameLighting,
        const PathTraceReferenceSettings& settings);
    // NRD.3: records the path-traced NRD preparation and bypass reconstruction after an extraction frame. view and
    // inverseViewProjection are the G-buffer's (render, jittered) camera. False when it could not run.
    bool RecordNrdPathTraceDiagnostic(
        ID3D12GraphicsCommandList* commandList,
        unsigned int frameIndex,
        const Matrix4& view,
        const Matrix4& inverseViewProjection,
        const Vector3& cameraPosition);
    // NRD.3: waits for the GPU, then releases the diagnostic's textures when they exist.
    void ReleaseNrdPathTraceDiagnostic();
    // NRD.RELAX.2: records the RELAX preparation and bypass reconstruction after an extraction frame that wrote the raw
    // diffuse hit distance, with RecordNrdPathTraceDiagnostic's camera. False when it could not run.
    bool RecordNrdRelaxPrepareDiagnostic(
        ID3D12GraphicsCommandList* commandList,
        const Matrix4& view,
        const Matrix4& inverseViewProjection,
        const Vector3& cameraPosition);
    // NRD.RELAX.2: waits for the GPU, then releases the RELAX preparation and the raw diffuse hit distance when they exist.
    void ReleaseNrdRelaxPrepareDiagnostic();
    // NRD.RELAX.3 diagnostic RELAX dispatch. Ensure creates the instance on first use; Release waits for the GPU, then
    // releases it, its resolved texture and its history. Record does everything fallible first (instance, inputs,
    // settings) and records nothing when any of it fails; only a recorded dispatch is committed at submission.
    bool EnsureNrdRelaxDenoiser();
    void ReleaseNrdRelaxDenoiser();
    // NRD RELAX R.9B diagnostics: records the confidence probes and filter for this frame (signal views 4 and 5).
    void RecordNrdConfidenceProbes(
        ID3D12GraphicsCommandList* commandList,
        const PathTraceReferenceConstants& constants,
        const FrameLighting& frameLighting,
        const PathTraceReferenceSettings& settings,
        PathTracePrimary primary,
        D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstants,
        D3D12_GPU_VIRTUAL_ADDRESS rtxdiConstants);
    // Waits for the GPU, then releases the diagnostic resolved texture alone (the selected RELAX keeps its instance).
    void ReleaseNrdRelaxDiagnosticTexture();
    void ReportNrdRelaxUnavailable(const std::string& reason);
    // NRD.RELAX.4 (PathTraceDenoiserMode::NrdRelax, and the RELAX.3 diagnostic): Begin does everything fallible before
    // recording (instance, inputs, settings); false means this frame falls back. Record records preparation, RELAX and
    // the resolves: into presentation's surface pixels when given, and into the diagnostic texture while the dispatch
    // diagnostic is requested. Only a recorded dispatch is committed at submission.
    bool BeginNrdRelaxFrame(
        PathTracePrimary primary,
        const Matrix4& view,
        const Matrix4& cameraProjection,
        const FrameLighting& frameLighting,
        const PathTraceReferenceSettings& settings);
    bool RecordNrdRelax(
        ID3D12GraphicsCommandList* commandList,
        unsigned int frameIndex,
        const Matrix4& view,
        const Matrix4& inverseViewProjection,
        const Vector3& cameraPosition,
        Texture* presentation);
    bool RecordNrdRelaxDispatchDiagnostic(
        ID3D12GraphicsCommandList* commandList,
        unsigned int frameIndex,
        PathTracePrimary primary,
        const Matrix4& view,
        const Matrix4& cameraProjection,
        const Matrix4& inverseViewProjection,
        const Vector3& cameraPosition,
        const FrameLighting& frameLighting,
        const PathTraceReferenceSettings& settings);
    // RTXPT D.3B: records the spatial pass over the temporal outputs the temporal pass recorded this frame.
    void RecordPathTraceSpatial(ID3D12GraphicsCommandList* commandList, const Matrix4& view, const Matrix4& projection,
        const PathTraceDenoiserVisualization& visualization);

    bool m_giSignedCorrectionInitialized = false;
    bool m_signedCorrectionExecutedLastFrame = false;

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
    // Render extent (UPSCALING.2A): every scene pass, history and dispatch.
    unsigned int m_sceneWidth = 0;
    unsigned int m_sceneHeight = 0;
    // Output extent (UPSCALING.2A): m_finalSceneColor and the extent ResizeScene requests.
    unsigned int m_outputWidth = 0;
    unsigned int m_outputHeight = 0;

    // UPSCALING.2C: the requested scale (SetRenderScale) and the one the current extents were built with.
    RenderScale m_requestedRenderScale = RenderScale::Native;
    RenderScale m_renderScale = RenderScale::Native;
    bool m_dlssEligible = true;
    bool m_dlssNormalRequest = false;
    bool m_dlssRetryPending = false;
    std::string m_dlssFailure;
    std::string m_dlssPublishedState;
    std::string m_dlssPublishedDetail;
    // RR.1A: the last Ray Reconstruction readout published (logged on change only).
    std::string m_rayReconstructionPublishedState;
    std::string m_rayReconstructionPublishedDetail;

    // UPSCALING.2C output-extent targets of the diagnostic presentation resolve: HDR colour (tone mapping's input),
    // selection mask (the outline's) and depth (the outline's and the Editor overlays'). Allocated only while the render
    // extent differs from the output extent, released otherwise; Native never has them.
    RenderTarget m_presentationColor;
    RenderTarget m_presentationSelectionMask;
    DepthBuffer m_presentationDepth;
    PresentationResolvePass m_presentationResolvePass;

    // UPSCALING.3A: the NGX instance of m_graphicsDevice (Editor only). Initialized after the device, shut down after the
    // final GPU drain and before the device is released. Features are created on demand.
    DlssBackend m_dlssBackend;

    // UPSCALING.3B DLSS input preparation (RendererOptions::dlssInputPreparation). Switching it releases or creates
    // nothing else and requests an upscaler history reset; the pass's textures follow the render extent.
    void ApplyDlssInputPreparation(bool requested);
    bool m_dlssInputPreparation = false;
    DlssInputPreparationPass m_dlssInputPreparationPass;
    // RR.1B: the Ray Reconstruction inputs (diagnostics only before RR.1C; Editor only). Prepared on path-traced frames
    // that extracted and prepared the DLSS inputs while m_rrInputPreparationForTesting is set; its targets are allocated
    // only then and released on resize and when preparation stops (SetRrInputPreparationForTesting).
    RrInputPreparationPass m_rrInputPreparationPass;
    bool m_rrInputPreparationForTesting = false;
    // Adds the PSR throughput target (RrInputPreparation.hlsl).
    bool m_rrInputDiagnosticsForTesting = false;
    bool m_rrInputsPreparedLastFrame = false;
    std::string m_rrInputPreparationFailure;
    // RR.1C: requested (SetRayReconstructionRequested), applied by ResizeScene, and this frame's RR mode.
    bool m_rayReconstructionRequested = false;
    // ENGINE_EXPERIMENTAL_RR was read (ResizeScene, once per session), and whether it requested RR (ORed with every request).
    bool m_rayReconstructionEnvironmentApplied = false;
    bool m_rayReconstructionEnvironmentRequest = false;
    bool m_rayReconstructionEffective = false;
    bool m_rrModeThisFrame = false;
    bool m_rrEvaluatedLastFrame = false;
    // RR.1C: the latched failure (empty: none); the next ResizeScene falls back.
    std::string m_rrFailure;
    const char* m_rrInactiveReason = "";
    // RR.1C: whether ResizeScene would apply Ray Reconstruction now (requested, permitted, available, not failed, and a
    // scale it supports), and the RR quality of a scale.
    bool RayReconstructionApplicable(RenderScale scale) const;
    static DlssRayReconstructionQuality RayReconstructionQuality(RenderScale scale) noexcept;
    void FailRayReconstruction(const std::string& stage, const std::string& detail);
    // The RR feature for this frame's extents (created on the lifecycle list, Super Resolution's released first).
    bool EnsureRayReconstructionFeature(std::string& failure);
    // Records the RR evaluation of this frame's prepared inputs into m_presentationColor; false leaves the resolve's colour.
    bool RecordRayReconstructionEvaluation(ID3D12GraphicsCommandList* commandList, unsigned int frameIndex);
    // Physical Lighting C2: this frame's camera exposure, read by tone mapping and the DLSS exposure texture.
    AutoExposurePass m_autoExposurePass;
    bool m_exposureInitialized = false;
    RenderViewIdentity m_exposureViewIdentity;
    float m_exposureReferenceEV100 = 0.0f;
    UpscalerFrameInputs m_upscalerInputs;
    // Set by every history discontinuity (ResetSceneHistory, scene resize, option changes); consumed by the next
    // prepared frame as UpscalerFrameInputs::reset.
    bool m_upscalerHistoryResetPending = true;

    // UPSCALING.3C DLSS mode: the applied scale is Dlss75 and the frame is a raster-primary path-traced one. Such a frame
    // jitters with the DLSS phase policy, prepares the DLSS inputs, keeps tracing past the sample target (DLSS needs a
    // changing image) and leaves the extended diffuse history out; Native and Diagnostic75 are unaffected.
    bool m_dlssModeThisFrame = false;
    bool m_dlssActiveLastFrame = false;
    bool m_dlssFrameSubmitted = false;
    // Whether the previous frame evaluated DLSS (a gap resets DLSS history).
    bool m_dlssEvaluatedLastFrame = false;
    const char* m_dlssInactiveReason = "";
    // UPSCALING.3C.4: why DLSS mode falls back to the diagnostic resolve this frame (no jitter forced); "" when it runs.
    const char* m_dlssFallbackReason = "";
    // UPSCALING.3D: NGX reported no render extent for the applied preset at this output extent (set by ResizeScene).
    bool m_dlssPresetUnsupported = false;
    // UPSCALING.3D: the last frame presented DLSS without recording the diagnostic resolve.
    bool m_presentationResolveSkippedLastFrame = false;
    // DLSS frames' material texture mip bias is log2(render / output width) plus this (Programming Guide 3.5); every
    // other frame samples with bias 0. 0, not the guide's -1: on Engine's textured scenes -1 doubled the error against
    // Native and added 35-50% still shimmer, while log2(render / output) halved the error of no bias (UPSCALING.3D).
    static constexpr float DlssTextureMipBiasOffset = 0.0f;

    // Diagnostics only: per-axis multipliers in place of DlssJitterSignX/Y, and a colour input in place of the scene
    // colour (a noise-free G-buffer target for the jitter-sign test).
    std::optional<std::array<float, 2>> m_dlssJitterSignForTesting;
    RenderTarget* m_dlssColorOverrideForTesting = nullptr;
    // Diagnostics only (UPSCALING.3D): when set, DLSS frames use this offset in place of DlssTextureMipBiasOffset
    // (NaN: bias 0).
    std::optional<float> m_dlssMipBiasOffsetForTesting;
    // Diagnostics only (UPSCALING.3D): DLSS evaluation reports failure before recording anything (exercises the
    // fallback resolve of a frame that skipped it).
    bool m_dlssEvaluationFailureForTesting = false;
    // UPSCALING.3C.4 Editor overlay prepass: on jittered Editor frames, scene depth and the selection mask are rasterized
    // again with the UNJITTERED camera at the output extent, so the grid, collider / light / frustum wireframes and the
    // selection outline test exactly what an unjittered frame would show. Never read by DLSS, the denoiser or any
    // history. Created on the first such frame and released with every scene resize; unjittered frames record nothing.
    DepthBuffer m_overlayDepth;
    RenderTarget m_overlaySelectionMask;
    bool RecordEditorOverlayPrepass(ID3D12GraphicsCommandList* commandList, const Scene& scene,
        const RendererOptions& options, D3D12_GPU_VIRTUAL_ADDRESS unjitteredCameraConstantsAddress);

    // Diagnostics only (UPSCALING.3C.2): when set, each frame copies the final scene image as it is before the Editor
    // overlays (grid, colliders) into this target of the same extent and format. Null in production: no commands.
    RenderTarget* m_preOverlayCopyForTesting = nullptr;
    // Diagnostics only (PS.5F): when set, each packaged frame copies its back buffer after tone mapping, before
    // Present, into this target of the application extent and the swap chain's format. Null in production.
    RenderTarget* m_packagedPresentCopyForTesting = nullptr;
    // Diagnostics only (M0b.1 P3): the same for Editor frames, after the UI is drawn. Null in production.
    RenderTarget* m_editorPresentCopyForTesting = nullptr;
    // Records the DLSS evaluation of a prepared Dlss75 frame into m_presentationColor; false leaves the resolve's colour.
    bool RecordDlssEvaluation(ID3D12GraphicsCommandList* commandList, unsigned int frameIndex);
    void PublishDlssReadout();
    void FailDlss(const std::string& stage, const std::string& detail);
    void RestoreDlssGraphicsState(ID3D12GraphicsCommandList* commandList);

    // RR.1A NGX lifecycle (DLSS_Ray_Reconstruction_RR1A.md). The Super Resolution feature for this frame's extents, created
    // when needed on a lifecycle list (CreateDlssFeatureOnLifecycleList) instead of the discardable frame recording; failure
    // explains a failure that the feature status does not.
    bool EnsureDlssFeature(const UpscalerFrameInputs& inputs, std::string& failure);
    // Records create(list) on the lifecycle list and submits it on the frame queue; a creation that did not succeed is
    // abandoned (never executed) and its handle released.
    bool CreateDlssFeatureOnLifecycleList(DlssFeatureKind kind, const std::function<bool(ID3D12GraphicsCommandList*)>& create,
        std::string& failure);
    // Releases kind's feature once its submitted uses completed: at once when the fences show it (or gpuIdle, a drained
    // caller), otherwise after a GPU drain (lifecycle transitions only). False when it cannot be released (an evaluation
    // pending in the open recording, or a failed drain).
    bool ReleaseDlssFeature(DlssFeatureKind kind, bool freeMemory, bool gpuIdle);
    // Submission and discard of a frame recording, as NGX bookkeeping sees them (both submission paths; AbortTrackedRecording).
    void ConfirmNgxRecordingSubmitted(std::uint64_t recording, std::uint64_t fenceValue);
    void DiscardNgxRecordingState();
    // The open recording consumed a pending upscaler history reset (returned to pending if the recording is discarded).
    bool m_upscalerResetConsumedInRecording = false;
    // RR.1A: a pending NGX restart (DlssBackend::RecoveryRequired) runs between frames: drain, Shutdown, Initialize. A
    // failed attempt blocks further attempts (fail closed: the instance is never evaluated) until DLSS Retry.
    void RecoverNgxIfRequired();
    bool m_ngxRecoveryBlocked = false;

public:
    // RR.1A diagnostics: the Ray Reconstruction feature slot exercised without rendering (no evaluation exists before
    // RR.1C). Create releases Super Resolution's feature first (the ownership policy) and creates Ray Reconstruction's for
    // the current output extent at quality; call between frames.
    bool CreateRayReconstructionFeatureForTesting(DlssRayReconstructionQuality quality, std::string& failure);
    bool ReleaseRayReconstructionFeatureForTesting();
    // RR.1A harness: the next frame that records a DLSS evaluation is discarded right after it (AbortTrackedRecording);
    // Render reports it Deferred, as the M4.3.5 hook does.
    bool m_abortFrameAfterDlssEvaluationForTesting = false;
    // With it: only a frame whose DLSS feature was created (on the lifecycle list) during this recording is discarded.
    bool m_abortRequiresDlssCreationForTesting = false;
    bool m_dlssFeatureCreatedThisRecording = false;
    // RR.1A: NGX feature creations submitted on the lifecycle list, and releases that had to drain the GPU first.
    std::uint64_t m_ngxLifecycleCreationsForTesting = 0;
    std::uint64_t m_ngxReleaseWaitsForTesting = 0;
    // RR.1A: NGX restarts after a discarded evaluation (DlssBackend::RecoveryRequired), with the last one's render-thread
    // cost, and the CPU time of the last lifecycle-list feature creation.
    struct NgxRecoveryTiming
    {
        double drainMilliseconds = 0.0;
        double shutdownMilliseconds = 0.0;
        double initializeMilliseconds = 0.0;
    };

    std::uint64_t m_ngxRecoveriesForTesting = 0;
    NgxRecoveryTiming m_lastNgxRecoveryTiming;
    double m_lastNgxCreationMilliseconds = 0.0;
    bool IsNgxRecoveryBlocked() const noexcept { return m_ngxRecoveryBlocked; }    DlssBackend& GetDlssBackendForTesting() noexcept { return m_dlssBackend; }
    // RR.1B diagnostics: starts or stops the Ray Reconstruction input preparation (between frames). Stopping releases its
    // targets after a GPU drain (no recorded frame reads them afterwards; Texture::Shutdown defers nothing).
    void SetRrInputPreparationForTesting(bool enabled, bool diagnostics = false);

    // RR.1C experimental DLSS Ray Reconstruction (DLSS_Ray_Reconstruction_RR1C.md). RR.1D: requested every frame by the
    // resolved configuration (ApplyRenderScale; the project's "dlssRayReconstruction" denoiser), or for the session by
    // ENGINE_EXPERIMENTAL_RR; never in the packaged runtime (RR is Editor-only). The request takes effect at the
    // next ResizeScene, a drained boundary that recreates the extent-sized resources and releases both NGX features. It is
    // effective while Ray Reconstruction is available, not failed, and the render scale is Native (RR at DLAA) or a DLSS
    // preset (RR at that preset's extent); Dlss75 and Diagnostic75 keep their paths. An RR frame is a raster-primary
    // path-traced NRD Hybrid frame: the path tracer, its extraction and the RR inputs run, NRD Hybrid, the Engine stages
    // and DLSS Super Resolution do not, and RR's output at the output extent goes through the ordinary mist, exposure,
    // tone-mapping and overlay path. A failure latches a reason and falls back (NRD Hybrid, plus SR at a preset) at the
    // next ResizeScene; Retry (SetRenderScale with retry) clears it.
    void SetRayReconstructionRequested(bool requested);
    bool IsRayReconstructionRequested() const noexcept { return m_rayReconstructionRequested; }
    // The applied state (ResizeScene) and whether the last frame presented an RR evaluation.
    bool IsRayReconstructionEffective() const noexcept { return m_rayReconstructionEffective; }
    bool IsRayReconstructionActiveLastFrame() const noexcept { return m_rrEvaluatedLastFrame; }
    const std::string& GetRayReconstructionFailure() const noexcept { return m_rrFailure; }
    // RR's jitter phase floor (RR guide 3.6: "at least 32"); the count is max(this, DlssJitterPhaseCount).
    static constexpr std::uint32_t RayReconstructionMinimumJitterPhases = 32;

    // RR.1C exposure experiment (diagnostics only): how RR's colour is exposed. None passes raw radiance and no exposure
    // input (the production configuration, RR.1C section 7); ExposureTexture also binds the auto-exposure texture;
    // PreExposed scales the noisy colour by m_rrPreExposureForTesting (RrInputPreparation.hlsl colorScale) and passes it as
    // InPreExposure. RR's output then stays scaled (the presented image is brighter by that factor): measurement only.
    enum class RayReconstructionExposureMode
    {
        None,
        ExposureTexture,
        PreExposed
    };

    RayReconstructionExposureMode m_rrExposureModeForTesting = RayReconstructionExposureMode::None;
    float m_rrPreExposureForTesting = 1.0f;
    // RR.1C harness: the next RR evaluation reports failure (DlssBackend::failNextRayReconstructionEvaluationForTesting).
    bool m_rrEvaluationFailureForTesting = false;
    // RR.2 boiling investigation (diagnostics only): RR's specular-motion input is the ordinary (surface) motion vectors
    // instead of RR.1B's reflection-aware ones. False in production.
    bool m_rrSurfaceMotionAsSpecularForTesting = false;
    // RR.1C harness: RR evaluations recorded in submitted frames, and the last one's InReset.
    std::uint64_t GetRayReconstructionEvaluationsForTesting() const noexcept
    {
        return m_dlssBackend.GetRayReconstructionFeatureStatus().evaluations;
    }
    RrInputPreparationPass& GetRrInputPreparationPassForTesting() noexcept { return m_rrInputPreparationPass; }
    RenderTarget& GetPresentationColorForTesting() noexcept { return m_presentationColor; }

private:
    bool PresentationResolveActive() const noexcept
    {
        return m_sceneWidth != m_outputWidth || m_sceneHeight != m_outputHeight;
    }

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
    float m_previousShadowLightAngularRadius = 0.0f;

    bool m_hasPreviousShadowLightDirection = false;

    Vector3 m_previousGILightColor{};

    float m_previousGILightIntensity = 0.0f;

    bool m_hasPreviousGILight = false;

    SceneLightTable m_previousGILocalLights{};
    // M3.5: the lighting revision m_previousGILocalLights holds (0 = unknown); equal revisions skip the comparison and copy.
    std::uint64_t m_previousGILightingRevision = 0;
    // Many-light M2: the GPU scene light table every scene-light consumer reads.
    SceneLightBuffer m_sceneLightBuffer;
    // Many-light M3: true while frames render without table lights (the scene light buffer or RTXDI's light data cannot hold
    // the table) or without RTXDI temporal reuse (RTXDI's light data cannot also hold the previous frame's lights). Logged
    // once when an episode starts and once when it ends; episodes are counted for tests.
    bool m_sceneLightPublicationFailing = false;
    std::uint32_t m_sceneLightPublicationFailureEpisodes = 0;

    // Verification only: records `count` lights into the scene light buffer in the current command list and returns the
    // address of their SceneLightConstants (0 on failure), for harnesses that drive passes directly.
    D3D12_GPU_VIRTUAL_ADDRESS UploadSceneLightsForTesting(const GPULight* lights, std::uint32_t count);
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
