#pragma once

#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <string>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

// UPSCALING.3A: what the NVIDIA DLSS Super Resolution backend can do on this machine. Ordered by how far initialization
// got; Available means a feature may be created, not that reconstruction is active.
enum class DlssAvailability
{
    // Built without ENGINE_ENABLE_DLSS.
    NotCompiled,
    // Initialize has not run (or Shutdown ran).
    NotInitialized,
    // nvngx_dlss.dll is not next to the executable, where NGX loads it from.
    RuntimeMissing,
    // NVSDK_NGX_D3D12_Init_with_ProjectID or the capability query failed (see the NGX result codes).
    InitializationFailed,
    // The driver is older than DLSS needs (see the minimum driver version).
    DriverOutdated,
    // NGX reports DLSS Super Resolution unavailable on this GPU/driver, or denied for this application.
    Unsupported,
    // NGX reports DLSS Super Resolution available; feature/evaluation state is tracked separately.
    Available
};

const char* DlssAvailabilityName(DlssAvailability availability) noexcept;

// RR.1A: what DLSS Ray Reconstruction (NGX feature DLSS-D) can do on this machine, queried with Super Resolution on the
// same NGX instance. Available means an RR feature may be created; nothing renders with it yet (RR.1C).
enum class DlssRayReconstructionAvailability
{
    // Built without ENGINE_ENABLE_DLSS_RR (or without the Super Resolution backend it needs).
    NotCompiled,
    // The process turned Ray Reconstruction off before Initialize (SetRayReconstructionPermitted). Both the Editor and
    // packaged games permit it since the cooked-game port.
    NotPermitted,
    // Initialize has not run (or Shutdown ran).
    NotInitialized,
    // nvngx_dlssd.dll is not next to the executable.
    RuntimeMissing,
    // NGX itself was not initialized (its init failed, or the Super Resolution runtime it is gated on is missing).
    NgxInitializationFailed,
    // The NGX capability parameters could not be read, or SuperSamplingDenoising.Available was absent.
    CapabilityQueryFailed,
    // SuperSamplingDenoising.NeedsUpdatedDriver (see the minimum driver version).
    DriverOutdated,
    // SuperSamplingDenoising.Available is 0 on this GPU/driver.
    Unsupported,
    // SuperSamplingDenoising.FeatureInitResult is a failure (RR denied or failed to initialize for this application).
    FeatureInitFailed,
    Available
};

const char* DlssRayReconstructionAvailabilityName(DlssRayReconstructionAvailability availability) noexcept;

// FG.1: what DLSS Frame Generation (NGX feature DLSS-G) can do on this machine, queried with Super Resolution on the same NGX
// instance. Available means a Frame Generation feature may be created; nothing renders with it yet (FG.3).
enum class DlssFrameGenerationAvailability
{
    // Built without ENGINE_ENABLE_DLSS_FG (or without the Super Resolution backend it needs).
    NotCompiled,
    // The process turned Frame Generation off before Initialize (SetFrameGenerationPermitted). Both the Editor and
    // packaged games permit it since the cooked-game port.
    NotPermitted,
    // Initialize has not run (or Shutdown ran).
    NotInitialized,
    // nvngx_dlssg.dll is not next to the executable.
    RuntimeMissing,
    // NGX itself was not initialized (its init failed, or the Super Resolution runtime it is gated on is missing).
    NgxInitializationFailed,
    // The NGX capability parameters could not be read, or FrameGeneration.Available was absent.
    CapabilityQueryFailed,
    // FrameGeneration.NeedsUpdatedDriver (see the minimum driver version).
    DriverOutdated,
    // FrameGeneration.Available is 0: an RTX 40-series GPU or newer is required.
    Unsupported,
    // FrameGeneration.FeatureInitResult is a failure (denied, or Windows' hardware-accelerated GPU scheduling is off).
    FeatureInitFailed,
    Available
};

const char* DlssFrameGenerationAvailabilityName(DlssFrameGenerationAvailability availability) noexcept;

// UPSCALING.3D: the NGX Super Resolution quality modes Engine offers, whose render extents come from the NGX
// optimal-settings query (never from fixed scale factors).
enum class DlssQualityPreset
{
    Quality,
    Balanced,
    Performance
};

const char* DlssQualityPresetName(DlssQualityPreset preset) noexcept;

// RR.1A: the NGX execution modes Ray Reconstruction can be created for (RR guide 3.2). DLAA renders at the output extent.
enum class DlssRayReconstructionQuality
{
    Dlaa,
    Quality,
    Balanced,
    Performance
};

const char* DlssRayReconstructionQualityName(DlssRayReconstructionQuality quality) noexcept;

// RR.1A: Ray Reconstruction's capability, read from the same capability parameters as Super Resolution's.
struct DlssRayReconstructionStatus
{
    DlssRayReconstructionAvailability availability = DlssRayReconstructionAvailability::NotInitialized;
    // nvngx_dlssd.dll beside the executable (checked whenever RR is compiled and permitted).
    bool runtimePresent = false;
    // SuperSamplingDenoising.* capability parameters, valid once the capability query succeeded.
    bool available = false;
    bool needsUpdatedDriver = false;
    std::uint32_t minDriverVersionMajor = 0;
    std::uint32_t minDriverVersionMinor = 0;
    std::int32_t featureInitResult = 0;
    std::string detail;
};

// FG.1: Frame Generation's capability, read from the same capability parameters as Super Resolution's.
struct DlssFrameGenerationStatus
{
    DlssFrameGenerationAvailability availability = DlssFrameGenerationAvailability::NotInitialized;
    // nvngx_dlssg.dll beside the executable (checked whenever Frame Generation is compiled and permitted).
    bool runtimePresent = false;
    // FrameGeneration.* capability parameters, valid once the capability query succeeded.
    bool available = false;
    bool needsUpdatedDriver = false;
    std::uint32_t minDriverVersionMajor = 0;
    std::uint32_t minDriverVersionMinor = 0;
    std::int32_t featureInitResult = 0;
    // DLSSG.MultiFrameCountMax: generated frames per rendered pair the GPU allows (1 or unset: 2x only). Engine uses 2x.
    std::uint32_t multiFrameCountMax = 0;
    std::string detail;
};

struct DlssStatus
{
    DlssAvailability availability = DlssAvailability::NotInitialized;
    // Raw NVSDK_NGX_Result values (0: the call was not made).
    std::uint32_t initResult = 0;
    std::uint32_t capabilityResult = 0;
    // SuperSampling.* capability parameters, valid once the capability query succeeded.
    bool superSamplingAvailable = false;
    bool needsUpdatedDriver = false;
    std::uint32_t minDriverVersionMajor = 0;
    std::uint32_t minDriverVersionMinor = 0;
    std::int32_t featureInitResult = 0;
    // Wall time of Initialize (NGX init plus the capability query).
    double initializeMilliseconds = 0.0;
    // One line for logs and the Editor readout.
    std::string detail;
    // RR.1A.
    DlssRayReconstructionStatus rayReconstruction;
    // FG.1.
    DlssFrameGenerationStatus frameGeneration;
};

// UPSCALING.3C: the DLSS Super Resolution feature of one render/output extent pair; RR.1A: also the Ray Reconstruction
// feature's (its own instance).
struct DlssFeatureStatus
{
    bool active = false;
    unsigned int renderWidth = 0;
    unsigned int renderHeight = 0;
    unsigned int outputWidth = 0;
    unsigned int outputHeight = 0;
    // NVSDK_NGX_PerfQuality_Value whose dynamic render range contains the render extent (-1: none).
    int perfQuality = -1;
    // UPSCALING.3D: the preset the feature was requested for (-1: none, the first mode whose range holds the extent).
    int requestedPreset = -1;
    // FG.1: the back-buffer DXGI_FORMAT a Frame Generation feature was created for (0 for the other features).
    std::uint32_t backbufferFormat = 0;
    // Raw NVSDK_NGX_Result values of the last creation and evaluation (0: not called).
    std::uint32_t createResult = 0;
    std::uint32_t evaluateResult = 0;
    // RR.1A: evaluations in submitted recordings since the feature was created. A recorded evaluation counts only once its
    // recording is submitted (ConfirmRecordingSubmitted); a discarded one never does.
    std::uint64_t evaluations = 0;
    const char* perfQualityName = "";
    // The dynamic render ranges NGX reports per quality mode for this output extent.
    std::string supportedRanges;
    // Why the feature is or is not active.
    std::string detail;
};

// UPSCALING.3C: one evaluation. All resources at the states DLSS requires (Programming Guide 3.4): inputs
// NON_PIXEL_SHADER_RESOURCE, output UNORDERED_ACCESS; DLSS returns them to those states.
struct DlssEvaluation
{
    ID3D12Resource* color = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motionVectors = nullptr;
    ID3D12Resource* exposure = nullptr;
    // NGX InJitterOffsetX/Y (render pixels); the caller converts from Engine's convention.
    float jitterOffsetX = 0.0f;
    float jitterOffsetY = 0.0f;
    unsigned int renderWidth = 0;
    unsigned int renderHeight = 0;
    // The caller's history discontinuity. Evaluate adds its own (EvaluationNeedsReset).
    bool reset = false;
};

// RR.1C: one Ray Reconstruction evaluation (RR guide 3.4; NVSDK_NGX_D3D12_DLSSD_Eval_Params). Inputs at the render
// extent in NON_PIXEL_SHADER_RESOURCE, output at the output extent in UNORDERED_ACCESS, as for Super Resolution.
struct DlssRayReconstructionEvaluation
{
    // Noisy linear HDR radiance of this frame (RGBA16F).
    ID3D12Resource* color = nullptr;
    ID3D12Resource* output = nullptr;
    // Hardware depth (the feature is created with NVSDK_NGX_DLSS_Depth_Type_HW).
    ID3D12Resource* depth = nullptr;
    // Ordinary motion vectors (RG16F, render pixels, previous = current + vector, jitter excluded).
    ID3D12Resource* motionVectors = nullptr;
    // pInMotionVectorsReflections (NVSDK_NGX_Parameter_GBuffer_SpecularMvec): the same convention.
    ID3D12Resource* specularMotionVectors = nullptr;
    // Linear diffuse and specular albedo (RGBA8_UNORM).
    ID3D12Resource* diffuseAlbedo = nullptr;
    ID3D12Resource* specularAlbedo = nullptr;
    // World-space shading normal, linear (perceptual) roughness in alpha (NVSDK_NGX_DLSS_Roughness_Mode_Packed).
    ID3D12Resource* normalRoughness = nullptr;
    // Optional exposure (1x1); RR guide 3.7 calls exposure unsupported, RR.1C measures it (null: not passed).
    ID3D12Resource* exposure = nullptr;
    // NGX InPreExposure (0: the helper's default 1).
    float preExposure = 0.0f;
    float jitterOffsetX = 0.0f;
    float jitterOffsetY = 0.0f;
    unsigned int renderWidth = 0;
    unsigned int renderHeight = 0;
    float frameTimeDeltaMilliseconds = 0.0f;
    // The caller's history discontinuity. EvaluateRayReconstruction adds its own (EvaluationNeedsReset).
    bool reset = false;
};

// FG.3: one 2x Frame Generation evaluation (FG guide 6; NVSDK_NGX_D3D12_DLSSG_Eval_Params). Inputs in
// NON_PIXEL_SHADER_RESOURCE, the generated frame in UNORDERED_ACCESS, as for the other features.
struct DlssFrameGenerationEvaluation
{
    // The real frame's final image: display-ready SDR at the feature's output extent and back-buffer format.
    ID3D12Resource* backbuffer = nullptr;
    // Hardware depth and DLSS motion vectors at the render extent (the inputs Super Resolution and Ray Reconstruction read).
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motionVectors = nullptr;
    // Optional: the image before the Editor overlays, so Frame Generation treats the overlays as UI (null: none drawn).
    ID3D12Resource* hudless = nullptr;
    // The generated frame: same extent and format as backbuffer.
    ID3D12Resource* output = nullptr;
    unsigned int renderWidth = 0;
    unsigned int renderHeight = 0;
    // NGX's camera matrices: row-major storage for row vectors (v * M), without jitter. Engine's column-vector matrices
    // are passed transposed (as NRD's are).
    float cameraViewToClip[4][4]{};
    float clipToCameraView[4][4]{};
    float clipToPrevClip[4][4]{};
    float prevClipToClip[4][4]{};
    float cameraPosition[3]{};
    float cameraUp[3]{};
    float cameraRight[3]{};
    float cameraForward[3]{};
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    // Vertical field of view in radians; aspect = width / height.
    float cameraFieldOfView = 0.0f;
    float cameraAspectRatio = 0.0f;
    // The caller's history discontinuity. EvaluateFrameGeneration adds its own (EvaluationNeedsReset).
    bool reset = false;
};

// RR.1A: the NGX features Engine can own. Super Resolution and Ray Reconstruction never hold GPU resources at the same time
// (the caller releases the other before creating one; FeatureCreationNeeded / ReleaseFeature). FG.1: Frame Generation runs
// after either of them on the final image, so its feature lives beside whichever is active.
enum class DlssFeatureKind
{
    SuperResolution,
    RayReconstruction,
    FrameGeneration
};

// UPSCALING.3A: owns the NGX SDK instance for one D3D12 device: initialization, the DLSS Super Resolution capability
// query and shutdown; UPSCALING.3C: the DLSS Super Resolution feature (creation, evaluation, release); RR.1A: Ray
// Reconstruction's capability and feature slot; FG.1: Frame Generation's. Every NGX call in Engine goes through this class.
// Not thread-safe (NGX is not); the render thread owns it.
//
// RR.1A NGX lifecycle (Documentation/Rendering/DLSS_Ray_Reconstruction_RR1A.md):
//  - Creation records on a lifecycle command list that the caller submits on the frame queue right away
//    (CommandContext::SubmitLifecycleCommands), never on the discardable frame recording. The caller reports the outcome
//    with ConfirmFeatureCreation or AbandonFeatureCreation; Evaluate refuses a feature whose creation was not submitted.
//  - Evaluation records on the frame recording. It counts (evaluations) only when ConfirmRecordingSubmitted reports that
//    recording submitted. A discarded (R.1 ResourceStateTransaction) or unconfirmed one forces InReset on the next
//    evaluation of that feature: NGX's internal temporal state cannot be rolled back.
//  - Release requires that no evaluation is pending in an open recording and that the GPU has completed every submitted
//    use (completedFenceValue at or past the last confirmed one, or gpuIdle).
class DlssBackend
{
public:
    DlssBackend() = default;
    DlssBackend(const DlssBackend&) = delete;
    DlssBackend& operator=(const DlssBackend&) = delete;
    ~DlssBackend();

    // RR.1A: before Initialize. False reports Ray Reconstruction NotPermitted without querying it.
    void SetRayReconstructionPermitted(bool permitted) noexcept { m_rayReconstructionPermitted = permitted; }
    // FG.1: likewise for Frame Generation.
    void SetFrameGenerationPermitted(bool permitted) noexcept { m_frameGenerationPermitted = permitted; }

    // Once per device, after it is created. Never fails the caller: every outcome is reported through GetStatus. A
    // second call without Shutdown is ignored. M0c.5 Stage 5.5: completes (waits for) an initialization BeginInitialize
    // started, rather than starting another.
    void Initialize(ID3D12Device* device);

    // M0c.5 Stage 5.5: the same initialization on a thread of its own, so it overlaps the caller's work. The NGX API is not
    // thread safe but has no thread affinity (DLSS Programming Guide 5.2.4): while it runs, nothing else calls NGX (the
    // status stays NotInitialized and every other method finds no NGX instance), and its outcome is adopted only by
    // CompleteInitialize, after the thread has finished. Ignored when initialized or already initializing.
    void BeginInitialize(ID3D12Device* device);

    // Adopts a finished BeginInitialize, logging its outcome as Initialize does; with wait, waits for an unfinished one.
    // True when no initialization is pending afterwards.
    bool CompleteInitialize(bool wait);

    bool IsInitializing() const noexcept
    {
        return m_pending.valid();
    }

    // Releases both features, their parameter maps and the NGX instance. Only after GPU completion (the caller drains),
    // before releasing the device. Idempotent.
    void Shutdown();

    const DlssStatus& GetStatus() const noexcept { return m_status; }

    // UPSCALING.3D: NGX's optimal render extent for preset at this output extent (Programming Guide 5.2.8). False when
    // DLSS is unavailable or NGX reports no extent for the combination.
    bool QueryOptimalRenderExtent(DlssQualityPreset preset, unsigned int outputWidth, unsigned int outputHeight,
        unsigned int& renderWidth, unsigned int& renderHeight);

    // RR.1A: whether EnsureFeature would record a creation for these extents: no active feature for exactly them, and no
    // memoized failure. Cheap; no NGX call.
    bool FeatureCreationNeeded(unsigned int renderWidth, unsigned int renderHeight, unsigned int outputWidth,
        unsigned int outputHeight, std::optional<DlssQualityPreset> preset = std::nullopt) const;

    // UPSCALING.3C: creates the feature for this extent pair on lifecycleList when none exists or the extents changed.
    // RR.1A: lifecycleList is a lifecycle command list (never the frame recording) that the caller submits immediately and
    // reports with ConfirmFeatureCreation/AbandonFeatureCreation. A feature for other extents must already be released
    // (ReleaseFeature): creation never releases one itself. True when a feature for exactly these extents exists. A failure
    // is recorded in GetFeatureStatus and not retried for the same extents. UPSCALING.3D preset: the feature uses that NGX
    // quality mode, which must hold the render extent; without one, the first mode whose range holds it (the diagnostic
    // 75% mode).
    bool EnsureFeature(ID3D12GraphicsCommandList* lifecycleList, unsigned int renderWidth, unsigned int renderHeight,
        unsigned int outputWidth, unsigned int outputHeight, std::optional<DlssQualityPreset> preset = std::nullopt);

    // Records one DLSS evaluation on commandList (the frame recording). NGX leaves the command list's state undefined
    // (Programming Guide 5.2.5): the caller rebinds descriptor heaps, pipeline, root signature, topology, viewport and
    // targets afterwards. InReset is evaluation.reset or EvaluationNeedsReset.
    bool Evaluate(ID3D12GraphicsCommandList* commandList, const DlssEvaluation& evaluation);

    // RR.1A: whether the next evaluation of kind's feature must reset its history regardless of the caller: no submitted
    // evaluation since creation, or the last recorded one was discarded or never confirmed submitted.
    bool EvaluationNeedsReset(DlssFeatureKind kind) const;

    // RR.1A: the NGX instance must be restarted (drained Shutdown + Initialize) before any further evaluation or creation:
    // an evaluation recorded with one of its features was discarded or never confirmed submitted. Measured: the next
    // submitted evaluation then reports D3D12 #527 on NGX's own nv.ngx.dlss.resource textures (UAV and
    // NON_PIXEL_SHADER_RESOURCE swapped), and the same resources survive feature release with FreeMemOnReleaseFeature.
    // Inferred: NGX records the barrier states of its pooled internal resources while recording an evaluation. Engine
    // never touches NGX's private state; a restarted instance is the recovery. Sticky: Shutdown keeps it, and only
    // MarkRecovered (after a successful Initialize) clears it. Evaluate and the feature creations refuse while it is set.
    bool RecoveryRequired() const;
    void MarkRecovered();
    // RR.1A: the lifecycle submission that recorded kind's pending creation was executed on the frame queue (fenceValue) or
    // could not be (Abandon: the handle, whose creation never ran, is released and the failure memoized with detail).
    void ConfirmFeatureCreation(DlssFeatureKind kind, std::uint64_t fenceValue);
    void AbandonFeatureCreation(DlssFeatureKind kind, const std::string& detail);

    // RR.1A: the frame recording `recording` (ResourceStateTransaction::RecordingFor, read before ExecuteCommandLists) was
    // submitted and will signal fenceValue. Its recorded evaluations become submitted ones.
    void ConfirmRecordingSubmitted(std::uint64_t recording, std::uint64_t fenceValue);

    // RR.1A: whether kind's feature may be released now: nothing pending in an open recording, and every submitted use
    // completed (completedFenceValue at or past it) or gpuIdle (the caller just drained the queue).
    bool CanReleaseFeature(DlssFeatureKind kind, std::uint64_t completedFenceValue, bool gpuIdle) const;

    // Releases kind's feature when CanReleaseFeature allows it (true; also true when there is none). freeMemory sets
    // NVSDK_NGX_Parameter_FreeMemOnReleaseFeature so NGX frees the feature's memory instead of caching it (Programming
    // Guide 3.14). A refused release changes nothing and logs why.
    bool ReleaseFeature(DlssFeatureKind kind, std::uint64_t completedFenceValue, bool gpuIdle, bool freeMemory = false);

    bool HasFeature(DlssFeatureKind kind) const noexcept;

    const DlssFeatureStatus& GetFeatureStatus() const noexcept { return m_superResolution.status; }

    // RR.1A Ray Reconstruction feature slot. Evaluation is RR.1C.
    bool QueryRayReconstructionOptimalRenderExtent(DlssRayReconstructionQuality quality, unsigned int outputWidth,
        unsigned int outputHeight, unsigned int& renderWidth, unsigned int& renderHeight);
    bool RayReconstructionCreationNeeded(unsigned int renderWidth, unsigned int renderHeight, unsigned int outputWidth,
        unsigned int outputHeight, DlssRayReconstructionQuality quality) const;
    // As EnsureFeature, for Ray Reconstruction: linear HDR colour, render-extent motion vectors, hardware depth, roughness
    // packed in the normals' alpha (the RR.0 design), NGX's default RR preset.
    bool EnsureRayReconstructionFeature(ID3D12GraphicsCommandList* lifecycleList, unsigned int renderWidth,
        unsigned int renderHeight, unsigned int outputWidth, unsigned int outputHeight, DlssRayReconstructionQuality quality);
    const DlssFeatureStatus& GetRayReconstructionFeatureStatus() const noexcept { return m_rayReconstruction.status; }
    // RR.1C: records one Ray Reconstruction evaluation on commandList (the frame recording), with the same lifecycle
    // bookkeeping as Evaluate: pending until ConfirmRecordingSubmitted, a discarded or unconfirmed one sets
    // RecoveryRequired, and an instance awaiting its restart is never evaluated. NGX leaves the command list's state
    // undefined; the caller restores it.
    bool EvaluateRayReconstruction(ID3D12GraphicsCommandList* commandList, const DlssRayReconstructionEvaluation& evaluation);
    // NGX_DLSSD_GET_STATS: VRAM NGX holds for Ray Reconstruction (every instance, plus its cache). False when unavailable.
    bool QueryRayReconstructionMemory(std::uint64_t& bytes);

    // FG.1 Frame Generation feature slot, with the RR.1A lifecycle (creation on a lifecycle list confirmed or abandoned by the
    // caller, release only after GPU completion). One feature per output extent, render extent and back-buffer format
    // (NVSDK_NGX_DLSSG_Create_Params); a change needs a release and a new creation. Evaluation is FG.3.
    bool FrameGenerationCreationNeeded(unsigned int outputWidth, unsigned int outputHeight, unsigned int renderWidth,
        unsigned int renderHeight, std::uint32_t backbufferFormat) const;
    bool EnsureFrameGenerationFeature(ID3D12GraphicsCommandList* lifecycleList, unsigned int outputWidth,
        unsigned int outputHeight, unsigned int renderWidth, unsigned int renderHeight, std::uint32_t backbufferFormat);
    const DlssFeatureStatus& GetFrameGenerationFeatureStatus() const noexcept { return m_frameGeneration.status; }
    // FG.3: records one generated frame on commandList (the frame recording), with the RR.1A bookkeeping: pending until
    // ConfirmRecordingSubmitted, a discarded or unconfirmed one sets RecoveryRequired. NGX leaves the command list's state
    // undefined; the caller restores it.
    bool EvaluateFrameGeneration(ID3D12GraphicsCommandList* commandList, const DlssFrameGenerationEvaluation& evaluation);

    // The live NGX feature handles (0 after Shutdown), for lifecycle verification.
    unsigned int LiveFeatureCount() const noexcept;

    // Diagnostics only: the project ID in place of ENGINE_DLSS_PROJECT_ID (an invalid one exercises the failure path).
    std::optional<std::string> projectIdForTesting;
    // Diagnostics only: any value but Available makes EnsureFeature report no feature (simulates an unavailable DLSS)
    // while an existing feature stays untouched.
    std::optional<DlssAvailability> availabilityOverrideForTesting;
    // Diagnostics only (RR.1A): the file name the Ray Reconstruction runtime check looks for beside the executable.
    std::optional<std::string> rayReconstructionRuntimeNameForTesting;
    // Diagnostics only (RR.1A): the next feature creation reports failure after NGX created it (exercises release after a
    // partial initialization: the handle exists, its creation is abandoned).
    bool failNextCreationForTesting = false;
    // Diagnostics only (RR.1A): the InReset of the last recorded Super Resolution evaluation.
    bool lastEvaluationResetForTesting = false;
    // Diagnostics only (RR.1C): the InReset of the last recorded Ray Reconstruction evaluation.
    bool lastRayReconstructionResetForTesting = false;
    // Diagnostics only (FG.3): the reset of the last recorded Frame Generation evaluation.
    bool lastFrameGenerationResetForTesting = false;
    // Diagnostics only (RR.1C): the next Ray Reconstruction evaluation reports failure without calling NGX.
    bool failNextRayReconstructionEvaluationForTesting = false;

private:
    // RR.1A: one NGX feature and its lifecycle bookkeeping.
    struct FeatureSlot
    {
        // NVSDK_NGX_Parameter* (the slot's own capability map) and NVSDK_NGX_Handle*, opaque here so that the header
        // needs no SDK.
        void* parameters = nullptr;
        void* handle = nullptr;
        DlssFeatureStatus status;
        // A creation recorded on a lifecycle list not yet reported submitted (Evaluate refuses the feature meanwhile).
        bool creationPending = false;
        // The frame recording (and its list) holding an evaluation not yet confirmed submitted (0: none). Resolved lazily
        // against R.1's recording state, hence mutable.
        mutable std::uint64_t pendingRecording = 0;
        mutable const void* pendingList = nullptr;
        // The latest fence of a submitted use (creation or evaluation); unknown when a submission's fence is not known.
        std::uint64_t lastUseFence = 0;
        mutable bool lastUseUnknown = false;
        // A pending evaluation was discarded, failed to submit or was never confirmed: the next evaluation resets.
        mutable bool historyInvalid = false;    };

    // Resolves the slot's pending evaluation against R.1's recording state: an open one stays pending; a discarded or
    // otherwise unconfirmed one invalidates the history.
    void ResolvePendingEvaluation(const FeatureSlot& slot) const;
    // RR.1C: the bookkeeping after a successful NGX evaluation call on commandList (shared by both features).
    void RecordPendingEvaluation(FeatureSlot& slot, ID3D12GraphicsCommandList* commandList);
    bool EnsureParameters(FeatureSlot& slot, const char* featureName);
    bool CanRelease(const FeatureSlot& slot, std::uint64_t completedFenceValue, bool gpuIdle, std::string& reason) const;
    void ReleaseSlot(FeatureSlot& slot, bool freeMemory);
    FeatureSlot& Slot(DlssFeatureKind kind) noexcept
    {
        switch(kind)
        {
        case DlssFeatureKind::RayReconstruction:
            return m_rayReconstruction;
        case DlssFeatureKind::FrameGeneration:
            return m_frameGeneration;
        case DlssFeatureKind::SuperResolution:
        default:
            return m_superResolution;
        }
    }
    const FeatureSlot& Slot(DlssFeatureKind kind) const noexcept
    {
        switch(kind)
        {
        case DlssFeatureKind::RayReconstruction:
            return m_rayReconstruction;
        case DlssFeatureKind::FrameGeneration:
            return m_frameGeneration;
        case DlssFeatureKind::SuperResolution:
        default:
            return m_superResolution;
        }
    }

    // M0c.5 Stage 5.5: everything Initialize decides, computed without touching this object (so on any thread).
    struct InitializationOutcome
    {
        DlssStatus status;
        // The NGX instance exists (Shutdown must release it), whatever the capability query found.
        bool initialized = false;
    };

    struct InitializationRequest
    {
        std::string projectId;
        bool rayReconstructionPermitted = true;
        std::string rayReconstructionRuntimeName;
        bool frameGenerationPermitted = true;
    };

    static InitializationOutcome RunInitialize(ID3D12Device* device, const InitializationRequest& request);
    void Adopt(InitializationOutcome outcome, ID3D12Device* device);
    InitializationRequest MakeInitializationRequest() const;

    std::future<InitializationOutcome> m_pending;
    ID3D12Device* m_pendingDevice = nullptr;

    ID3D12Device* m_device = nullptr;
    bool m_initialized = false;
    bool m_rayReconstructionPermitted = true;
    bool m_frameGenerationPermitted = true;
    // RR.1A: RecoveryRequired (set while resolving a pending evaluation, hence mutable).
    mutable bool m_recoveryRequired = false;
    DlssStatus m_status;
    FeatureSlot m_superResolution;
    FeatureSlot m_rayReconstruction;
    FeatureSlot m_frameGeneration;
};
