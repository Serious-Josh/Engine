#pragma once

#include "Renderer/RendererStatistics.h"
#include "Renderer/Passes/ToneMapPass.h"
#include "GpuTimingMonitor.h"
#include "GpuTimingGraph.h"
#include "CpuFrameTiming.h"
#include "FramePacingMonitor.h"

#include <climits>
#include <cstdint>
#include <string>
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

    GpuTimingGraph m_gpuTimingGraph;

    DescriptorHeap* m_descriptorHeap = nullptr;

    bool m_tlasUpdated = false;

    int m_toneMapDebugView = 0;
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
};
