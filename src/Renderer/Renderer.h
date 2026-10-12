#pragma once
#include "Debug/CpuFrameTiming.h"
#include "Diagnostics/FrameHitchDiagnostics.h"

#include "GBuffer.h"
#include "UploadBuffer.h"
#include "StructuredBuffer.h"
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
#include <cstddef>
#include <limits>
#include <climits>

struct EnvironmentAsset
{
    std::string displayName;

    std::filesystem::path relativePath;
};

struct RendererOptions
{
    size_t selectedObject = (std::numeric_limits<size_t>::max)();

    Vector3 selectionOutlineColor{1.0f, 0.72f, 0.0f};
    unsigned int selectionOutlineThickness = 2;

    bool enableRTGI = true;

    float rtgiContactAOStrength = 1.0f;
    float rtgiContactAORadius = 2.5f;
};

class Scene;
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
        const std::filesystem::path& assetRoot);
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

    ID3D12Device* GetDevice();
    ResourceUploader& GetResourceUploader();
    DescriptorHeap& GetResourceDescriptorHeap();
    ShaderLibrary& GetShaderLibrary();
    DebugUI& GetDebugUI();

    bool InitializeSceneResources(
        const Scene& scene,
        const std::vector<Material*>& materials);

    [[nodiscard]] bool ReleaseSceneResources();
    // Checked teardown only: performs the caller's existing queue drain, then
    // resolves exact successful submissions or cancels an unrecorded request.
    // Output is empty when nothing finalized; no scene acknowledgment here.
    [[nodiscard]] bool DrainAndResolveSceneDisplacement(const Scene& scene, DisplacementCompletionResult& result);
    void ResetSceneHistory();

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
    bool ExecuteTrackedRecording(unsigned int frameSlot);

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
    std::uint64_t m_sceneResourceLifetime = 0;
    const Scene* m_sceneResourceScene = nullptr;

    CpuTiming::FrameSample m_cpuFrameTiming;

    CpuTiming::Clock::time_point m_previousRenderExecuteStart;

    bool m_hasPreviousRenderExecute = false;
    bool InitializeDevice();
    bool InitializeSwapChain(void* windowHandle);
    void Draw(
        ID3D12GraphicsCommandList* commandList,
        const RenderObject& object,
        D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress);
    void DrawSelectionMask(
        ID3D12GraphicsCommandList* commandList,
        const RenderObject& object,
        D3D12_GPU_VIRTUAL_ADDRESS cameraConstantsAddress);
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

    bool m_giSignedCorrectionInitialized = false;
    bool m_signedCorrectionExecutedLastFrame = false;

    NrdPreparePass m_nrdReflectionPreparePass;
    NrdPreparePass m_nrdGIPreparePass;

    NrdReflectionResolvePass m_nrdReflectionResolvePass;

    NrdDiffuseResolvePass m_nrdDiffuseResolvePass;

    ShadowDenoisePass m_shadowDenoisePass;

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

    unsigned int m_width = 0;
    unsigned int m_height = 0;
    unsigned int m_shadowSampleIndex = 0;
    unsigned int m_reflectionSampleIndex = 0;
    uint32_t m_nrdFrameIndex = 0;

    GBuffer m_gBuffer;

    UploadBuffer m_uploadBuffer;

    StructuredBuffer m_materialBuffer;

    static constexpr unsigned int kDisplacementLODFrameCount = 2;

    unsigned int m_displacementLODDescriptorIndices[kDisplacementLODFrameCount] = {UINT_MAX, UINT_MAX};

    EnvironmentMap m_environment;

    RenderTarget m_sceneColor;
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
