#pragma once

#include "Renderer/RendererStatistics.h"
#include "Renderer/ReferenceRayCounts.h"
#include "Renderer/Passes/PathTraceSpatialPass.h"
#include "Renderer/Passes/ToneMapPass.h"
#include "Renderer/RendererLightingMode.h"
#include "Renderer/LightingModeTransition.h"
#include "Renderer/RenderingConfiguration.h"
#include "Project/ProjectSettings.h"
#include "Engine/RenderingOverrides.h"
#include "GpuTimingMonitor.h"
#include "GpuTimingGraph.h"
#include "CpuFrameTiming.h"
#include "FramePacingMonitor.h"
#include "Editor/EditorIcons.h"

#include <climits>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12CommandQueue;
struct HWND__;

using HWND = HWND__*;

class DescriptorHeap;

class DebugUI
{
    // Local diagnostics drive the Editor's render settings directly (UPSCALING.3C.2).
    friend class SceneHierarchyVerification;
    friend class RenderingVerification;

    public:
    bool ReferenceRayCountsEnabled() const { return m_referenceRayCountsEnabled; }
    void SetReferenceRayCounts(const ReferenceRayCountResult& result) { m_referenceRayCounts = result; }
    DebugUI() = default;

    DebugUI(const DebugUI&) = delete;
    DebugUI& operator=(const DebugUI&) = delete;

    bool Initialize(
        HWND windowsHandle,
        ID3D12Device* device,
        ID3D12CommandQueue* commandQueue,
        DescriptorHeap& descriptorHeap);
    void Shutdown();
    const EditorIcons& GetEditorIcons() const
    {
        return m_editorIcons;
    }

    void BeginFrame();
    void DrawMenu();
    void DrawPerformanceWindow(
        const std::vector<std::string>& environmentNames,
        size_t currentEnvironmentIndex,
        const RendererStatistics& statistics);
    void EndFrame(
        ID3D12GraphicsCommandList* commandList,
        bool* textureUploadRequested = nullptr);

    void SetTLASUpdated(bool updated);
    void RecordFramePacingCpuTiming(const CpuTiming::FrameSample& sample)
    {
        m_framePacingMonitor.RecordCpu(sample);
    }
    void RecordFramePacingGaps(const GpuTimestampProfiler::Results& results)
    {
        m_framePacingMonitor.RecordGaps(results);
    }
    void RecordCpuFrameTiming(const CpuTiming::FrameSample& sample);
    // PS.2C: the project's saved rendering settings (Engine supplies them every frame) and this session's
    // Debug panel overrides. The panel's rendering controls show the effective values and edit only the
    // overrides; nothing here writes ProjectSettings.json.
    void SetSavedRenderingSettings(const RenderingSettings& saved)
    {
        m_savedRendering = saved;
    }
    const RenderingSettings& GetSavedRenderingSettings() const
    {
        return m_savedRendering;
    }
    RenderingOverrides& GetRenderingOverrides()
    {
        return m_renderingOverrides;
    }
    const RenderingOverrides& GetRenderingOverrides() const
    {
        return m_renderingOverrides;
    }
    void ClearRenderingOverrides()
    {
        m_renderingOverrides = {};
    }

    // After a successful Project Settings save: the overrides the save decided are cleared, then the saved
    // settings are replaced.
    void OnProjectSettingsSaved(const RenderingSettings& previousSaved, const RenderingSettings& saved);

    // Saved settings with the session overrides applied.
    RenderingSettings GetEffectiveRenderingSettings() const;
    // The panel's diagnostic controls and Debug-only Reference overrides.
    RenderingDiagnostics GetRenderingDiagnostics() const;

    // The effective settings resolved by the shared ResolveRenderingConfiguration.
    RenderingConfiguration GetRenderingConfiguration() const;

    // PS.3: read-only runtime status for other Editor views. Copies of what this panel already displays (renderer
    // publications of the last frame); nothing is read back from the GPU and nothing here is saved.
    struct RenderingStatus
    {
        LightingModeTransitionStatus transition;
        std::uint32_t psrActiveChain = 0;
        bool psrRequested = false;
        std::string psrUnavailable;
        std::string dlssState;
        std::string dlssDetail;
        bool pathTraceActiveLastFrame = false;
        std::uint32_t accumulatedSamples = 0;
        std::uint64_t accumulationBytes = 0;
        std::string denoiserPreparation;
        SharcStatus sharc;
    };

    RenderingStatus GetRenderingStatus() const
    {
        RenderingStatus status;
        status.transition = m_lightingTransition;
        status.psrActiveChain = m_psrActiveMode;
        status.psrRequested = m_psrLightingRequested;
        status.psrUnavailable = m_psrLightingUnavailable;
        status.dlssState = m_dlssState;
        status.dlssDetail = m_dlssDetail;
        status.pathTraceActiveLastFrame = m_pathTraceActiveLastFrame;
        status.accumulatedSamples = m_pathTraceAccumulatedSamples;
        status.accumulationBytes = m_pathTraceResourceBytes;
        status.denoiserPreparation = m_pathTraceDenoiserPreparation;
        status.sharc = m_sharcStatus;
        return status;
    }

    ToneMapDebugView GetToneMapDebugView() const
    {
        return static_cast<ToneMapDebugView>(m_toneMapDebugView);
    }
    // PSR.2 Stage 3A: whether this frame requested in-place replacement, and why its reflections are not lit (empty when
    // they are).
    void SetPsrActiveMode(std::uint32_t mode) { m_psrActiveMode = mode; }
    void SetPsrLightingStatus(bool requested, const std::string& unavailable)
    {
        m_psrLightingRequested = requested;
        m_psrLightingUnavailable = unavailable;
    }

    // The camera exposure state of the latest completed frame (GPU readback, a few frames old; display only).
    struct ExposureDisplay
    {
        bool valid = false;
        float effectiveEV100 = 0.0f;
        float multiplier = 1.0f;
        float targetEV100 = 0.0f;
        float meteredEV100 = 0.0f;
        std::uint32_t validPixels = 0;
        std::uint32_t invalidPixels = 0;
    };

    void SetExposureDisplay(const ExposureDisplay& display) { m_exposureDisplay = display; }
    const ExposureDisplay& GetExposureDisplay() const { return m_exposureDisplay; }

    // ReSTIR PT Enhanced reuse toggles (Debug UI, Rendering Settings, shown while ReSTIR PT Enhanced is the estimator).
    // The renderer seeds them from its startup settings (ENGINE_RESTIR_PT_TEMPORAL / _SPATIAL) and applies them only
    // when the user changes one, so direct callers that set the renderer's settings themselves are never overridden.
    void SetRestirPtReuse(bool temporal, bool spatial)
    {
        m_restirPtTemporalReuse = temporal;
        m_restirPtSpatialReuse = spatial;
    }

    bool ConsumeRestirPtReuseChange(bool& temporal, bool& spatial)
    {
        temporal = m_restirPtTemporalReuse;
        spatial = m_restirPtSpatialReuse;
        const bool changed = m_restirPtReuseChanged;
        m_restirPtReuseChanged = false;
        return changed;
    }

    // Optimization experiment A5: ReSTIR PT's spatial partner count (1-3), shown under "Spatial reuse".
    void SetRestirPtSpatialPairs(std::uint32_t pairs)
    {
        m_restirPtSpatialPairs = static_cast<int>(pairs);
    }

    std::uint32_t GetRestirPtSpatialPairs() const
    {
        return static_cast<std::uint32_t>(m_restirPtSpatialPairs);
    }

    // Optimization experiment B1a: decorrelation mode (false Uniform, true Stagnancy), shown under the reuse toggles.
    void SetRestirPtStagnancy(bool stagnancy)
    {
        m_restirPtStagnancyMode = stagnancy ? 1 : 0;
    }

    bool GetRestirPtStagnancy() const
    {
        return m_restirPtStagnancyMode != 0;
    }

    // Optimization experiment B1b: firefly replacement, shown under the reuse toggles.
    void SetRestirPtFirefly(bool firefly)
    {
        m_restirPtFirefly = firefly;
    }

    bool GetRestirPtFirefly() const
    {
        return m_restirPtFirefly;
    }

    // M5.6 O0: ReSTIR PT's table-light candidates from the shared ReGIR cells (above 64 local lights).
    void SetRestirPtRegir(bool regir)
    {
        m_restirPtRegir = regir;
    }

    bool GetRestirPtRegir() const
    {
        return m_restirPtRegir;
    }

    // M5.6 O0.1: ReSTIR PT's ReGIR window (cells per axis: 16 or 32) and its spatial pairs on ReGIR frames (0: same).
    void SetRestirPtRegirWindow(std::uint32_t cells)
    {
        m_restirPtRegirWindow32 = cells >= 32u;
    }

    std::uint32_t GetRestirPtRegirWindow() const
    {
        return m_restirPtRegirWindow32 ? 32u : 16u;
    }

    // M5.6 flashing fix: ReSTIR PT's ReGIR slots per cell.
    void SetRestirPtRegirSlots(std::uint32_t slots)
    {
        m_restirPtRegirSlots = static_cast<int>(slots);
    }

    std::uint32_t GetRestirPtRegirSlots() const
    {
        return static_cast<std::uint32_t>(m_restirPtRegirSlots);
    }

    // M5.6 O0.2: ReSTIR PT's ReGIR spot-cone bound and direct candidate fetch.
    void SetRestirPtRegirCone(bool cone)
    {
        m_restirPtRegirCone = cone;
    }

    bool GetRestirPtRegirCone() const
    {
        return m_restirPtRegirCone;
    }

    void SetRestirPtRegirDirect(bool direct)
    {
        m_restirPtRegirDirect = direct;
    }

    bool GetRestirPtRegirDirect() const
    {
        return m_restirPtRegirDirect;
    }

    void SetRestirPtRegirSpatialPairs(std::uint32_t pairs)
    {
        m_restirPtRegirSpatialPairs = static_cast<int>(pairs);
    }

    std::uint32_t GetRestirPtRegirSpatialPairs() const
    {
        return static_cast<std::uint32_t>(m_restirPtRegirSpatialPairs);
    }

    bool IsShadingNormalCorrectionEnabled() const
    {
#ifndef NDEBUG
        return m_shadingNormalCorrectionEnabled;
#else
        return true;
#endif
    }

    // SHARC.4E cached-radiance statistics of the last frame.
    void SetSharcStatus(const SharcStatus& status)
    {
        m_sharcStatus = status;
    }

    // The selector's value: the effective Path Tracing denoiser (the enums share their order up to NrdHybrid). RR.1D: Ray
    // Reconstruction runs the hybrid configuration's path tracer, so its selection reads as NrdHybrid here.
    PathTraceDenoiserMode GetSelectedPathTraceDenoiserMode() const
    {
        const ProjectPathTraceDenoiser denoiser = GetEffectiveRenderingSettings().pathTracing.denoiser;
        return denoiser == ProjectPathTraceDenoiser::DlssRayReconstruction ? PathTraceDenoiserMode::NrdHybrid :
            static_cast<PathTraceDenoiserMode>(denoiser);
    }
    bool IsRayReconstructionSelected() const
    {
        return GetEffectiveRenderingSettings().pathTracing.denoiser == ProjectPathTraceDenoiser::DlssRayReconstruction;
    }

    // DLSS eligibility against the transition and denoiser state last published to the panel (the shared rules).
    bool IsDlssConfigurationEligible() const
    {
        return ::IsDlssConfigurationEligible(GetRenderingConfiguration(), m_lightingTransition.active,
            !m_pathTraceDenoiserPreparation.empty());
    }
    bool IsDlssPreparing() const
    {
        return ::IsDlssPreparing(GetRenderingConfiguration(), m_lightingTransition.active,
            !m_pathTraceDenoiserPreparation.empty());
    }
    bool ConsumeDlssRetry() { return std::exchange(m_dlssRetry, false); }

    // DLSS capability, requested/effective state and retry eligibility, refreshed by the renderer.
    void SetDlssReadout(std::string state, std::string detail, bool canRetry = false)
    {
        m_dlssCanRetry = canRetry;
        m_dlssState = std::move(state);
        m_dlssDetail = std::move(detail);
    }

    // RR.1A: DLSS Ray Reconstruction's capability and feature slot. RR.1D: its requested/effective state and whether a
    // latched failure may be retried (the DLSS Retry request clears it).
    void SetRayReconstructionReadout(std::string state, std::string detail, bool canRetry = false)
    {
        m_rayReconstructionState = std::move(state);
        m_rayReconstructionDetail = std::move(detail);
        m_rayReconstructionCanRetry = canRetry;
    }

    // The renderer's jitter of the last recorded frame (render pixels), shown under the control.
    // NRD.4B: why REBLUR fell back to the Engine denoiser (empty while it denoises).
    void SetPathTraceNrdStatus(const std::string& status)
    {
        m_pathTraceNrdStatus = status;
    }

    // ER.1 Phase 2: why the selected denoiser is not active yet (its libraries compile or failed); empty otherwise.
    void SetPathTraceDenoiserPreparation(const std::string& status)
    {
        m_pathTraceDenoiserPreparation = status;
    }

    void SetPathTraceCameraJitterReadout(bool active, std::uint32_t phase, std::uint32_t phaseCount, float x, float y)
    {
        m_pathTraceCameraJitterActive = active;
        m_pathTraceCameraJitterPhase = phase;
        m_pathTraceCameraJitterPhaseCount = phaseCount;
        m_pathTraceCameraJitterX = x;
        m_pathTraceCameraJitterY = y;
    }

    // Renderer evidence shown next to the mode selector (availability, reason, sample count).
    void SetPathTraceReferenceStatus(
        PathTraceReferenceAvailability availability,
        const std::string& unavailableReason,
        bool activeLastFrame,
        std::uint32_t accumulatedSamples,
        std::uint64_t resourceBytes);

    void SetSecondaryShadowVisibilityStatus(bool ready, bool active, std::uint64_t epoch)
    {
        m_secondaryShadowReady = ready;
        m_secondaryShadowActive = active;
        m_visibilityTimingEpoch = epoch;
    }

    // GPU Timing Monitor: the extents the renderer holds and whether DLSS evaluated the last recorded frame.
    void SetGpuTimingRenderState(unsigned int renderWidth, unsigned int renderHeight, unsigned int outputWidth,
        unsigned int outputHeight, bool dlssActive)
    {
        m_gpuTimingRenderWidth = renderWidth;
        m_gpuTimingRenderHeight = renderHeight;
        m_gpuTimingOutputWidth = outputWidth;
        m_gpuTimingOutputHeight = outputHeight;
        m_gpuTimingDlssActive = dlssActive;
    }

    // The raster-primary capability: the shared prerequisites (the diagnostic exhaustive estimator) and ReSTIR DI's own.
    void SetPathTraceRasterPrimaryStatus(const PathTraceRasterPrimaryStatus& status)
    {
        m_rasterPrimaryStatus = status;
    }

    // Async DXR preparation: the Renderer's transition snapshot from the last recording. Set before the
    // rendering settings are drawn, so the selector and the indicator agree within a UI frame.
    void SetLightingModeTransitionStatus(const LightingModeTransitionStatus& status);

    // The bottom-right preparation indicator, or a dismissible failure notice, inside the scene viewport.
    void DrawLightingModeTransitionIndicator(float viewportX, float viewportY, float viewportWidth,
        float viewportHeight);

    bool HasEnvironmentChangeRequest() const
    {
        return m_requestedEnvironmentIndex >= 0;
    }

    size_t ConsumeEnvironmentChangeRequest()
    {
        const size_t index = static_cast<size_t>(m_requestedEnvironmentIndex);

        m_requestedEnvironmentIndex = -1;

        return index;
    }

    private:
    // The active ReSTIR DI direct pass's primary DI shadow backend, for the status and timing displays.
    const char* PrimaryDiShadowBackendName() const;

    // Views of GetRenderingConfiguration for the panel's own status and timing displays.
    RendererLightingMode GetLightingMode() const
    {
        return GetRenderingConfiguration().lightingMode;
    }
    PathTraceReferenceSettings GetPathTraceReferenceSettings() const
    {
        return GetRenderingConfiguration().pathTrace;
    }
    SharcSettings GetSharcSettings() const
    {
        return GetRenderingConfiguration().sharc;
    }
    PathTraceDenoiserMode GetPathTraceDenoiserMode() const
    {
        return GetRenderingConfiguration().denoiser;
    }
    int GetPathTraceRenderScaleIndex() const
    {
        return GetRenderingConfiguration().renderScaleIndex;
    }

    // Typed override controls: each shows value, writes the override only when edited, marks a present
    // override (with a button that restores the project value) and returns the value after the edit.
    bool OverrideCheckbox(const char* label, bool value, std::optional<bool>& slot, RenderingOverrideField field);
    float OverrideSliderFloat(const char* label, float value, float minimum, float maximum, const char* format,
        int flags, std::optional<float>& slot, RenderingOverrideField field);
    std::uint32_t OverrideSliderUInt(const char* label, std::uint32_t value, int minimum, int maximum, int flags,
        std::optional<std::uint32_t>& slot, RenderingOverrideField field);
    template<typename Enum>
    Enum OverrideCombo(const char* label, Enum value, const char* const* items, int count, std::optional<Enum>& slot,
        RenderingOverrideField field);
    void DrawOverrideMarker(RenderingOverrideField field);

    RenderingSettings m_savedRendering;
    RenderingOverrides m_renderingOverrides;

    bool m_showRenderingSettings = false;
    bool m_showPassTimings = false;

    void DrawFramePacingSpikes();
    void DrawPathTraceReferenceSettings();
    FramePacingMonitor m_framePacingMonitor;

    std::array<FramePacingMonitor::Event, 16> m_pausedFramePacingEvents{};
    unsigned int m_pausedFramePacingEventCount = 0;
    bool m_framePacingViewPaused = false;
    void DrawCpuTiming();
    std::array<CpuTiming::FrameSample, 120> m_cpuFrameSamples{};
    unsigned int m_cpuFrameSampleCount = 0;
    unsigned int m_nextCpuFrameSample = 0;
    void DrawGpuTimingMonitor(const GpuTimestampProfiler::Results& results);
    GpuTimingMonitor m_gpuTimingMonitor;
    GpuTimingMonitor m_visibilityTimings;
    std::uint32_t m_psrActiveMode = 0;
    bool m_psrLightingRequested = false;
    std::string m_psrLightingUnavailable;
    bool m_secondaryShadowReady = false;
    bool m_secondaryShadowActive = false;
    std::uint64_t m_visibilityTimingEpoch = 0;
    std::uint64_t m_sampledVisibilityEpoch = UINT64_MAX;
    std::uint64_t m_visibilityWarmupUntil = 0;
    PathTraceReferenceSettings m_visibilityTimingSettings;
    SharcSettings m_visibilityTimingSharc;
    PathTraceDenoiserMode m_visibilityTimingDenoiser = PathTraceDenoiserMode::Off;
    PathTraceDenoiserStages m_visibilityTimingStages{};
    int m_visibilityTimingScale = -1;
    int m_visibilityTimingLighting = -1;

    // D1 (ReferenceRestirResolutionDispatchAudit): what decides the GPU pass graph the monitor samples. Requested
    // settings and the renderer's effective state, so a deferred activation (library preparation) also starts fresh
    // statistics when it takes effect. Accumulated samples, jitter and frame serials are deliberately absent.
    struct GpuTimingConfiguration
    {
        std::uint64_t epoch = 0;
        int lightingMode = -1;
        RendererLightingMode activeLightingMode = RendererLightingMode::Legacy;
        int renderScale = -1;
        unsigned int renderWidth = 0;
        unsigned int renderHeight = 0;
        unsigned int outputWidth = 0;
        unsigned int outputHeight = 0;
        bool dlssActive = false;
        bool dlssInputPreparation = false;
        bool cameraJitter = false;
        PathTraceReferenceSettings settings{};
        bool pathTraceActive = false;
        bool secondaryShadowActive = false;
        PathTraceDenoiserMode denoiser = PathTraceDenoiserMode::Off;
        bool denoiserPreparing = false;
        bool temporalHistory = false;
        PathTraceSpatialFilter spatialFilter = PathTraceSpatialFilter::Atrous2;
        bool extendedDiffuseHistory = false;
        SharcSettings sharc{};
        bool sharcActive = false;

        bool operator==(const GpuTimingConfiguration&) const = default;
    };

    GpuTimingConfiguration CurrentGpuTimingConfiguration() const;
    GpuTimingConfiguration m_gpuTimingConfiguration{};
    bool m_gpuTimingConfigurationKnown = false;
    unsigned int m_gpuTimingRenderWidth = 0;
    unsigned int m_gpuTimingRenderHeight = 0;
    unsigned int m_gpuTimingOutputWidth = 0;
    unsigned int m_gpuTimingOutputHeight = 0;
    bool m_gpuTimingDlssActive = false;


    GpuTimingGraph m_gpuTimingGraph;

    DescriptorHeap* m_descriptorHeap = nullptr;
    EditorIcons m_editorIcons;

    bool m_tlasUpdated = false;

    int m_toneMapDebugView = 0;
    std::uint64_t m_totalTLASUpdates = 0;

    int m_requestedEnvironmentIndex = -1;

    // ReSTIR PT Enhanced reuse toggles (SetRestirPtReuse / ConsumeRestirPtReuseChange).
    bool m_restirPtTemporalReuse = false;
    bool m_restirPtSpatialReuse = false;
    bool m_restirPtReuseChanged = false;
    int m_restirPtSpatialPairs = 3;
    int m_restirPtStagnancyMode = 0;
    bool m_restirPtFirefly = false;
    bool m_restirPtRegir = true;
    bool m_restirPtRegirWindow32 = false;
    int m_restirPtRegirSpatialPairs = 0;
    int m_restirPtRegirSlots = 512;
    bool m_restirPtRegirCone = true;
    bool m_restirPtRegirDirect = true;

#ifndef NDEBUG
    bool m_shadingNormalCorrectionEnabled = true;
#endif
    ExposureDisplay m_exposureDisplay;
    bool m_referenceRayCountsEnabled = false;
    ReferenceRayCountResult m_referenceRayCounts{};

    // SHARC.4E tuning (Debug-only): indices into the density (5x5, 8x8) and minimum-sample (8, 16, 32, 64, 256)
    // choices. Enabling SHARC is a project setting.
    int m_sharcDensity = 1;
    int m_sharcMinimumSamples = 3;
    bool m_sharcFreezeTraining = false;
    bool m_sharcStatistics = false;
    std::uint32_t m_sharcResetRequest = 0;
    int m_sharcSpatialResolution = 1;
    bool m_sharcHistoryPrior = false;
    // NVIDIA SHARC: 0 Engine D6 (comparison only), 1 NVIDIA (production); capacity index into 2^20..2^22 (default 2^20).
    int m_sharcImplementation = 1;
    int m_sharcNvidiaCapacity = 0;
    bool m_sharcNvidiaFeedback = true;
    SharcStatus m_sharcStatus;
    void DrawSharcSettings();
    std::string m_pathTraceNrdStatus;
    std::string m_pathTraceDenoiserPreparation;
    PathTraceDenoiserStages m_pathTraceDenoiserStages{};
    std::string m_dlssState = "Not initialized";
    bool m_dlssRetry = false;
    bool m_dlssCanRetry = false;
    bool m_dlssInputPreparation = false;
    std::string m_dlssDetail;
    std::string m_rayReconstructionState = "Not initialized";
    std::string m_rayReconstructionDetail;
    bool m_rayReconstructionCanRetry = false;
    // UPSCALING.2C/3C: 0 = 100% (Native), 1 = 75% (diagnostic resolve), 2 = 75% DLSS (experimental).
    int m_pathTraceRenderScale = 0;
    bool m_pathTraceCameraJitterActive = false;
    std::uint32_t m_pathTraceCameraJitterPhase = 0;
    std::uint32_t m_pathTraceCameraJitterPhaseCount = 0;
    float m_pathTraceCameraJitterX = 0.0f;
    float m_pathTraceCameraJitterY = 0.0f;

    PathTraceReferenceAvailability m_pathTraceAvailability = PathTraceReferenceAvailability::NotRequested;
    std::string m_pathTraceUnavailableReason;
    bool m_pathTraceActiveLastFrame = false;
    std::uint32_t m_pathTraceAccumulatedSamples = 0;
    std::uint64_t m_pathTraceResourceBytes = 0;

    PathTraceRasterPrimaryStatus m_rasterPrimaryStatus;

    LightingModeTransitionStatus m_lightingTransition;
    // UI time at which the indicator first observed the current preparation; negative when none.
    double m_lightingPreparationStartTime = -1.0;
    RendererLightingMode m_lightingPreparationMode = RendererLightingMode::Legacy;
    // The failure the user dismissed; its notice stays closed until a different failure or a new attempt.
    std::string m_dismissedLightingFailure;
};
