#pragma once

#include "Renderer/RendererStatistics.h"
#include "Renderer/Passes/PathTraceSpatialPass.h"
#include "Renderer/Passes/ToneMapPass.h"
#include "Renderer/RendererLightingMode.h"
#include "Renderer/LightingModeTransition.h"
#include "GpuTimingMonitor.h"
#include "GpuTimingGraph.h"
#include "CpuFrameTiming.h"
#include "FramePacingMonitor.h"
#include "Editor/EditorIcons.h"

#include <climits>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12CommandQueue;
struct HWND__;

using HWND = HWND__*;

class DescriptorHeap;

enum class ReflectionDenoiserMode
{
    Nrd,
    Native
};

class DebugUI
{
    // Local diagnostics drive the Editor's render settings directly (UPSCALING.3C.2).
    friend class SceneHierarchyVerification;

    public:
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
    ReflectionDenoiserMode GetReflectionDenoiserMode() const
    {
        return static_cast<ReflectionDenoiserMode>(m_reflectionDenoiserMode);
    }
    bool IsReflectionContributionLimiterEnabled() const
    {
        return m_contributionLimiterEnabled;
    }
    ToneMapDebugView GetToneMapDebugView() const
    {
        return static_cast<ToneMapDebugView>(m_toneMapDebugView);
    }

    // RTXPT D.3B.0 path-trace denoiser diagnostic view (the setter serves automated diagnostics).
    PathTraceDenoiserVisualization GetPathTraceDenoiserVisualization() const
    {
        return m_pathTraceDenoiserVisualization;
    }

    void SetPathTraceDenoiserVisualization(const PathTraceDenoiserVisualization& visualization)
    {
        m_pathTraceDenoiserVisualization = visualization;
    }

    bool IsShadingNormalCorrectionEnabled() const
    {
#ifndef NDEBUG
        return m_shadingNormalCorrectionEnabled;
#else
        return true;
#endif
    }

    bool IsRTGIEnabled() const
    {
        return m_rtgiEnabled;
    }
    float GetRTGIContactAOStrength() const
    {
        return m_rtgiContactAOStrength;
    }
    float GetRTGIContactAORadius() const
    {
        return m_rtgiContactAORadius;
    }

    // RTXPT.1 experimental lighting mode. Session-only Editor state: never saved to preferences,
    // scenes or projects, and Legacy whenever the Editor starts.
    RendererLightingMode GetLightingMode() const
    {
        switch (m_lightingMode)
        {
        case 1:
            return RendererLightingMode::PathTracedReference;
        case 2:
            return RendererLightingMode::PathTracedRasterPrimary;
        case 3:
            return RendererLightingMode::PathTracedRasterPrimaryReSTIRDI;
        default:
            return RendererLightingMode::Legacy;
        }
    }

    PathTraceReferenceSettings GetPathTraceReferenceSettings() const;

    // SHARC.4E experimental cached-radiance rendering (the raster-primary path-traced modes), and its last statistics.
    SharcSettings GetSharcSettings() const;
    void SetSharcStatus(const SharcStatus& status)
    {
        m_sharcStatus = status;
    }

    // SHARC.4E.1: the path-tracing denoiser (Path Traced Raster Primary, and with ReSTIR DI since SHARC.5B). NRD.2: a
    // mode; Off outside those lighting modes.
    PathTraceDenoiserMode GetPathTraceDenoiserMode() const
    {
        return m_lightingMode == 2 || m_lightingMode == 3 ? m_pathTraceDenoiserMode : PathTraceDenoiserMode::Off;
    }

    // DENOISE.1: the denoiser stages to run (diagnostics; the defaults are the production pipeline).
    PathTraceDenoiserStages GetPathTraceDenoiserStages() const
    {
        return m_pathTraceDenoiserStages;
    }

    // UPSCALING.2B: experimental camera jitter in the raster-primary path-traced modes (off by default, not saved).
    bool IsPathTraceCameraJitterEnabled() const
    {
        return m_pathTraceCameraJitter && (m_lightingMode == 2 || m_lightingMode == 3);
    }

    // UPSCALING.2C: the experimental 75% render scale in the raster-primary path-traced modes (off by default, not
    // saved). Other modes always render at the output extent.
    bool IsPathTraceReducedRenderScale() const
    {
        return m_pathTraceRenderScale != 0 && (m_lightingMode == 2 || m_lightingMode == 3);
    }

    // UPSCALING.3C/3D: whether the reduced scale is a DLSS mode (the diagnostic 75% one or a preset).
    bool IsPathTraceDlssSelected() const
    {
        return m_pathTraceRenderScale >= 2 && (m_lightingMode == 2 || m_lightingMode == 3);
    }

    // UPSCALING.3D: the selected render-scale entry in the raster-primary path-traced modes (0 otherwise): 0 Native,
    // 1 bilinear 75%, 2 DLSS 75% (diagnostic), 3 DLSS Quality, 4 DLSS Balanced, 5 DLSS Performance.
    int GetPathTraceRenderScaleIndex() const
    {
        return m_lightingMode == 2 || m_lightingMode == 3 ? m_pathTraceRenderScale : 0;
    }

    // UPSCALING.3B: diagnostic DLSS input preparation (off by default, not saved). It never evaluates DLSS.
    bool IsDlssInputPreparationEnabled() const
    {
        return m_dlssInputPreparation;
    }

    // UPSCALING.3A: the DLSS Super Resolution backend's state, set once at renderer initialization (informational).
    void SetDlssReadout(std::string state, std::string detail)
    {
        m_dlssState = std::move(state);
        m_dlssDetail = std::move(detail);
    }

    // The renderer's jitter of the last recorded frame (render pixels), shown under the control.
    // NRD.4B: why REBLUR fell back to the Engine denoiser (empty while it denoises).
    void SetPathTraceNrdStatus(const std::string& status)
    {
        m_pathTraceNrdStatus = status;
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

    void SetPathTraceRasterPrimaryStatus(PathTraceRasterPrimaryAvailability availability, const std::string& reason);
    void SetPathTraceReSTIRDIStatus(PathTraceRasterPrimaryAvailability availability, const std::string& reason);

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
    int m_secondaryShadowVisibility = PathTraceReferenceSettings{}.secondaryShadowRayQuery ? 1 : 0;
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


    GpuTimingGraph m_gpuTimingGraph;

    DescriptorHeap* m_descriptorHeap = nullptr;
    EditorIcons m_editorIcons;

    bool m_tlasUpdated = false;

    int m_toneMapDebugView = 0;
    PathTraceDenoiserVisualization m_pathTraceDenoiserVisualization{};
    int m_reflectionDenoiserMode = static_cast<int>(ReflectionDenoiserMode::Nrd);

    bool m_contributionLimiterEnabled = true;

    std::uint64_t m_totalTLASUpdates = 0;

    int m_requestedEnvironmentIndex = -1;

#ifndef NDEBUG
    bool m_shadingNormalCorrectionEnabled = true;
#endif
    bool m_rtgiEnabled = true;

    float m_rtgiContactAOStrength = 1.0f;
    float m_rtgiContactAORadius = 2.5f;

    int m_lightingMode = 0;
    int m_pathTraceSamplesPerFrame = 1;
    int m_pathTraceMaxPathDepth = 3;
    int m_pathTraceRussianRouletteDepth = 3;
    int m_pathTraceMaxAccumulatedSamples = 1 << 16;

    // SHARC.4E: indices into the density (5x5, 8x8) and minimum-sample (8, 16, 32, 64, 256) choices.
    bool m_sharcEnabled = false;
    int m_sharcDensity = 0;
    int m_sharcMinimumSamples = 3;
    bool m_sharcFreezeTraining = false;
    bool m_sharcStatistics = false;
    std::uint32_t m_sharcResetRequest = 0;
    int m_sharcSpatialResolution = 1;
    bool m_sharcHistoryPrior = false;
    SharcStatus m_sharcStatus;
    void DrawSharcSettings();
    // SHARC.4E.1: the path-tracing denoiser in the raster-primary path-traced modes (Engine by default in the Editor).
    // NRD.2: the selector offers Off and Engine only.
    PathTraceDenoiserMode m_pathTraceDenoiserMode = PathTraceDenoiserMode::Engine;
    std::string m_pathTraceNrdStatus;
    PathTraceDenoiserStages m_pathTraceDenoiserStages{};
    bool m_pathTraceCameraJitter = false;
    std::string m_dlssState = "Not initialized";
    bool m_dlssInputPreparation = false;
    std::string m_dlssDetail;
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

    PathTraceRasterPrimaryAvailability m_rasterPrimaryAvailability = PathTraceRasterPrimaryAvailability::NotRequested;
    std::string m_rasterPrimaryReason;
    PathTraceRasterPrimaryAvailability m_restirDIAvailability = PathTraceRasterPrimaryAvailability::NotRequested;
    std::string m_restirDIReason;

    LightingModeTransitionStatus m_lightingTransition;
    // UI time at which the indicator first observed the current preparation; negative when none.
    double m_lightingPreparationStartTime = -1.0;
    RendererLightingMode m_lightingPreparationMode = RendererLightingMode::Legacy;
    // The failure the user dismissed; its notice stays closed until a different failure or a new attempt.
    std::string m_dismissedLightingFailure;
};
