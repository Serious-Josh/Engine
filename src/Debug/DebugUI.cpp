#include "DebugUI.h"
#include "LightingModeTransitionPresentation.h"

#include "Renderer/DescriptorHeap.h"
#include "Platform/Windows/WindowsWindow.h"

#include <cmath>
#include <format>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include "ImGuizmo.h"

void DebugUI::RecordCpuFrameTiming(const CpuTiming::FrameSample& sample)
{
    if (!sample.valid)
    {
        return;
    }

    m_cpuFrameSamples[m_nextCpuFrameSample] = sample;

    m_nextCpuFrameSample = (m_nextCpuFrameSample + 1u) % m_cpuFrameSamples.size();
    m_cpuFrameSampleCount = (std::min)(m_cpuFrameSampleCount + 1u, static_cast<unsigned int>(m_cpuFrameSamples.size()));
}

void DebugUI::DrawFramePacingSpikes()
{
    if (!ImGui::CollapsingHeader("Frame Pacing Spikes"))
    {
        return;
    }

    ImGui::TextWrapped(

        "GPU boundary gaps above 10 ms; newest 16. These are not proven GPU idle times or evidence of causation.");

    if (ImGui::Button(m_framePacingViewPaused ? "Resume##FramePacing" : "Pause##FramePacing"))
    {
        m_framePacingViewPaused = !m_framePacingViewPaused;

        if (m_framePacingViewPaused)
        {
            // Freeze only the view; live collection and pending CPU matches continue.
            m_pausedFramePacingEventCount = m_framePacingMonitor.EventCount();

            for (unsigned int i = 0; i < m_pausedFramePacingEventCount; ++i)
            {
                m_pausedFramePacingEvents[i] = m_framePacingMonitor.NewestEvent(i);
            }
        }
    }

    if (m_framePacingViewPaused)
    {

        ImGui::SameLine();

        ImGui::TextUnformatted("View paused; collection continues.");
    }

    const unsigned int eventCount =
        m_framePacingViewPaused ? m_pausedFramePacingEventCount : m_framePacingMonitor.EventCount();

    if (eventCount == 0)
    {
        ImGui::TextUnformatted("No qualifying gaps collected.");
    }

    for (unsigned int i = 0; i < eventCount; ++i)
    {
        const auto& event =

            m_framePacingViewPaused ? m_pausedFramePacingEvents[i] : m_framePacingMonitor.NewestEvent(i);
        const bool hasPrevious = event.previous.state == FramePacingMonitor::RecordState::Available;
        const bool hasCurrent = event.current.state == FramePacingMonitor::RecordState::Available;
        const auto& previous = event.previous.sample;
        const auto& current = event.current.sample;
        ImGui::PushID(&event);
        const bool expanded = ImGui::TreeNode(
            "Event", "GPU gap %.3f ms | frames %llu -> %llu%s", event.gap.milliseconds,
            static_cast<unsigned long long>(event.gap.previousSerial),
            static_cast<unsigned long long>(event.gap.currentSerial), hasPrevious && hasCurrent ? "" : " | incomplete");

        const auto timing = [](const char* label, bool available, double milliseconds)
        {
            if (available)
            {
                ImGui::Text("%s: %.3f ms", label, milliseconds);
            }
            else
            {
                ImGui::Text("%s: unavailable", label);
            }
        };
        timing("Previous Present", hasPrevious && previous.presentationReturned, previous.present);
        timing("Current fence wait", hasCurrent, current.fenceWait);
        timing("Current command recording", hasCurrent, current.recording);
        const auto interval = [](CpuTiming::Clock::time_point begin, CpuTiming::Clock::time_point end)
        { return std::chrono::duration<double, std::milli>(end - begin).count(); };
        timing("Execute start-to-start", hasPrevious && hasCurrent,
               interval(previous.executeStart, current.executeStart));

        if (expanded)
        {
            const auto status = [](const char* label, FramePacingMonitor::RecordState state)
            {
                if (state != FramePacingMonitor::RecordState::Available)
                {
                    ImGui::Text("%s CPU record: %s", label,
                                state == FramePacingMonitor::RecordState::Pending ? "awaiting publication"
                                                                                  : "missing or expired");
                }
            };
            status("Previous", event.previous.state);
            status("Current", event.current.state);

            timing("Previous Execute duration", hasPrevious, previous.execute);
            timing("Current Execute duration", hasCurrent, current.execute);
            timing("Previous frame execution", hasPrevious, previous.frameExecution);
            timing("Current frame execution", hasCurrent, current.frameExecution);
            timing("Previous command recording", hasPrevious, previous.recording);
            timing("Previous fence wait", hasPrevious, previous.fenceWait);
            timing("Current Present", hasCurrent && current.presentationReturned, current.present);
            timing("Previous Present return to current Execute",
                   hasPrevious && hasCurrent && previous.presentationReturned,
                   interval(previous.presentReturn, current.executeStart));
            // Relative CPU anchors preserve ordering without exposing implementation-specific clock epochs.
            timing("Previous Execute after previous frame start", hasPrevious,
                   interval(previous.frameStart, previous.executeStart));
            timing("Previous Present return after previous frame start", hasPrevious && previous.presentationReturned,
                   interval(previous.frameStart, previous.presentReturn));
            timing("Current frame start after previous frame start", hasPrevious && hasCurrent,
                   interval(previous.frameStart, current.frameStart));
            timing("Current Execute after previous frame start", hasPrevious && hasCurrent,
                   interval(previous.frameStart, current.executeStart));

            if (hasCurrent)
            {
                ImGui::Text("Current Begin waited: %s", current.fenceWaited ? "yes" : "no");
            }

            ImGui::TreePop();
        }

        ImGui::PopID();
        ImGui::Separator();
    }
}

void DebugUI::DrawCpuTiming()
{
    if (!ImGui::CollapsingHeader("CPU Timing"))
    {
        return;
    }

    if (m_cpuFrameSampleCount == 0)
    {
        ImGui::TextUnformatted("Waiting for a completed CPU frame.");
        return;
    }

    const unsigned int latestIndex = (m_nextCpuFrameSample + m_cpuFrameSamples.size() - 1u) % m_cpuFrameSamples.size();

    const CpuTiming::FrameSample& latest = m_cpuFrameSamples[latestIndex];

    ImGui::Text("Completed CPU frame %llu | Begin fence wait: %s", static_cast<unsigned long long>(latest.frameSerial),
                latest.fenceWaited ? "entered" : "not needed");
    ImGui::TextUnformatted("Wall-clock durations, including blocking. Latest / last 120 average / maximum.");
    ImGui::TextUnformatted("Begin contains wait + reset; Render contains all renderer stages.");
    ImGui::TextUnformatted("Start interval ends at this frame's entry; execution measures this frame's work.");
    ImGui::TextUnformatted("Execute start-to-start is CPU submission cadence, not GPU start or idle time.");

    struct TimingRow
    {
        const char* label;
        double CpuTiming::FrameSample::* value;
        bool CpuTiming::FrameSample::* valid = nullptr;
    };
    static constexpr TimingRow rows[] = {
        {"Frame start-to-start", &CpuTiming::FrameSample::frameInterval, &CpuTiming::FrameSample::frameIntervalValid},
        {"Frame execution (incl. messages/UI)", &CpuTiming::FrameSample::frameExecution},
        {"Renderer::Render total", &CpuTiming::FrameSample::render},
        {"CommandContext::Begin total", &CpuTiming::FrameSample::begin},
        {"Begin fence wait only", &CpuTiming::FrameSample::fenceWait},
        {"Begin allocator/list reset", &CpuTiming::FrameSample::commandReset},
        {"Renderer command recording", &CpuTiming::FrameSample::recording},
        {"CommandContext::End", &CpuTiming::FrameSample::end},
        {"CommandContext::Execute (incl. Signal)", &CpuTiming::FrameSample::execute},
        {"Execute start-to-start", &CpuTiming::FrameSample::executeInterval,
         &CpuTiming::FrameSample::executeIntervalValid},
        {"SwapChain::Present (VSync)", &CpuTiming::FrameSample::present}};

    if (ImGui::BeginTable("CPU frame durations", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
    {
        for (const char* heading : {"CPU stage", "Latest ms", "Average ms", "Maximum ms"})
        {
            ImGui::TableSetupColumn(heading);
        }
        ImGui::TableHeadersRow();

        for (const TimingRow& row : rows)
        {
            double sum = 0.0;
            double maximum = 0.0;
            unsigned int count = 0;

            for (unsigned int i = 0; i < m_cpuFrameSampleCount; ++i)
            {
                const CpuTiming::FrameSample& sample = m_cpuFrameSamples[i];

                if (row.valid != nullptr && !(sample.*(row.valid)))
                {
                    continue;
                }

                const double value = sample.*(row.value);

                sum += value;
                maximum = (std::max)(maximum, value);
                ++count;
            }

            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.label);
            ImGui::TableNextColumn();

            if (row.valid == nullptr || latest.*(row.valid))
            {
                ImGui::Text("%.3f", latest.*(row.value));
            }
            else
            {
                ImGui::TextUnformatted("--");
            }
            ImGui::TableNextColumn();

            if (count > 0)
            {
                ImGui::Text("%.3f", sum / count);
            }
            else
            {
                ImGui::TextUnformatted("--");
            }
            ImGui::TableNextColumn();

            if (count > 0)
            {
                ImGui::Text("%.3f", maximum);
            }
            else
            {
                ImGui::TextUnformatted("--");
            }
        }

        ImGui::EndTable();
    }
}

void DebugUI::DrawGpuTimingMonitor(const GpuTimestampProfiler::Results& results)
{
    GpuTimingMonitor& monitor = m_gpuTimingMonitor;

    const bool open = ImGui::CollapsingHeader("GPU Timing Monitor");

    if (open)
    {
        if (ImGui::Checkbox("Monitoring enabled", &monitor.enabled))
        {
            // A disabled interval should not leave an old excursion latched.
            monitor.ResetExcursions();
        }

        if (ImGui::SliderFloat("Spike/drop threshold (ms)", &monitor.threshold, 0.01f, 5.0f, "%.2f"))
        {
            monitor.ResetExcursions();
        }

        if (ImGui::Button(monitor.frozen ? "Unfreeze" : "Freeze"))
        {
            monitor.frozen = !monitor.frozen;
        }

        ImGui::SameLine();

        if (ImGui::Button("Clear history"))
        {
            monitor.Clear();
        }
    }

    if (!open)
    {
        return;
    }

    ImGui::TextUnformatted("Resolved GPU frames; freeze pauses monitor sampling. Times in ms.");

    if (monitor.hasFrame)
    {
        ImGui::Text("Monitor GPU frame: %llu", static_cast<unsigned long long>(monitor.sampledFrame));
    }

    ImGui::TextUnformatted("Recent events (newest first)");

    ImGui::BeginChild("GPU timing events", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 6.0f), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    if (monitor.eventCount == 0)
    {
        ImGui::TextUnformatted("No events (requires 10 previous valid samples).");
    }

    for (unsigned int i = 0; i < monitor.eventCount; ++i)
    {
        const unsigned int eventIndex =

            (monitor.nextEvent + GpuTimingMonitor::EventCapacity - 1u - i) % GpuTimingMonitor::EventCapacity;
        const GpuTimingMonitor::Event& event = monitor.events[eventIndex];

        const bool spike = event.delta > 0.0;

        const ImVec4 eventColor = spike ? ImVec4(1.0f, 0.55f, 0.35f, 1.0f) : ImVec4(0.35f, 0.85f, 1.0f, 1.0f);

        ImGui::TextColored(eventColor, "%s | %s | %.3f -> %.3f | %+.3f ms | frame %llu",
                           GpuTimingRegionNames[event.region], spike ? "Spike" : "Drop", event.baseline, event.current,
                           event.delta, static_cast<unsigned long long>(event.frame));
    }

    ImGui::EndChild();

    struct RegionStatistics
    {
        unsigned int region;
        double average;
        double minimum;
        double maximum;
    };

    std::array<RegionStatistics, GpuTimingMonitor::RegionCount> rows{};

    for (unsigned int region = 0; region < rows.size(); ++region)
    {
        const GpuTimingMonitor::History& history = monitor.histories[region];

        RegionStatistics& row = rows[region];
        row.region = region;

        if (history.count == 0)
        {
            continue;
        }

        row.minimum = history.samples[0];
        row.maximum = history.samples[0];

        for (unsigned int sampleIndex = 0; sampleIndex < history.count; ++sampleIndex)
        {
            const double value = history.samples[sampleIndex];

            row.average += value;
            row.minimum = (std::min)(row.minimum, value);
            row.maximum = (std::max)(row.maximum, value);
        }

        row.average /= history.count;
    }

    std::sort(rows.begin(), rows.end(),
              [](const RegionStatistics& left, const RegionStatistics& right)
              {
                  const double leftRange = left.maximum - left.minimum;
                  const double rightRange = right.maximum - right.minimum;

                  if (leftRange == rightRange)
                  {
                      return left.region < right.region;
                  }

                  return leftRange > rightRange;
              });
    ImGui::TextUnformatted("Last 120 valid samples per region, sorted by range");

    if (ImGui::BeginTable("GPU timing statistics", 6,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 12.0f)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);

        for (const char* heading : {"Region", "Current", "Average", "Min", "Max", "Range"})
        {
            ImGui::TableSetupColumn(heading);
        }

        ImGui::TableHeadersRow();

        for (const RegionStatistics& row : rows)
        {
            const GpuTimingMonitor::History& history = monitor.histories[row.region];
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(GpuTimingRegionNames[row.region]);

            ImGui::TableNextColumn();

            if (history.currentValid)
            {
                ImGui::Text("%.3f", history.current);
            }
            else
            {
                ImGui::TextUnformatted("--");
            }

            const double values[] = {row.average, row.minimum, row.maximum, row.maximum - row.minimum};

            for (double value : values)
            {
                ImGui::TableNextColumn();

                if (history.count != 0)
                {
                    ImGui::Text("%.3f", value);
                }
                else
                {
                    ImGui::TextUnformatted("--");
                }
            }
        }

        ImGui::EndTable();
    }
}

static void AllocateImGuiDescriptor(
    ImGui_ImplDX12_InitInfo* info,
    D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
    D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle)
{
    DescriptorHeap* descriptorHeap = static_cast<DescriptorHeap*>(info->UserData);

    unsigned int index = descriptorHeap->Allocate();

    if (index == UINT_MAX)
    {
        *cpuHandle = {};
        *gpuHandle = {};

        return;
    }

    *cpuHandle = descriptorHeap->GetCPUHandle(index);

    *gpuHandle = descriptorHeap->GetGPUHandle(index);
}

static void FreeImGuiDescriptor(
    ImGui_ImplDX12_InitInfo* info,
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
{
    DescriptorHeap* descriptorHeap = static_cast<DescriptorHeap*>(info->UserData);

    unsigned int index = descriptorHeap->GetIndex(cpuHandle);

    if (index != UINT_MAX)
    {
        descriptorHeap->Free(index);
    }
}

bool DebugUI::Initialize(
    HWND windowHandle,
    ID3D12Device* device,
    ID3D12CommandQueue* commandQueue,
    DescriptorHeap& descriptorHeap)
{
    m_descriptorHeap = &descriptorHeap;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(windowHandle))
    {
        ImGui::DestroyContext();

        m_descriptorHeap = nullptr;
        return false;
    }

    ImGui_ImplDX12_InitInfo initInfo{};

    initInfo.Device = device;
    initInfo.CommandQueue = commandQueue;
    initInfo.NumFramesInFlight = 2;
    initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    initInfo.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    initInfo.SrvDescriptorHeap = descriptorHeap.GetHeap();

    initInfo.UserData = &descriptorHeap;

    initInfo.SrvDescriptorAllocFn = AllocateImGuiDescriptor;
    initInfo.SrvDescriptorFreeFn = FreeImGuiDescriptor;

    if (!ImGui_ImplDX12_Init(&initInfo))
    {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        m_descriptorHeap = nullptr;
        return false;
    }

    m_editorIcons.Initialize(device, descriptorHeap, WindowsWindow::GetExecutableDirectory() / "assets/Editor/Icons");
    return true;
}

void DebugUI::Shutdown()
{
    m_editorIcons.Shutdown();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();

    ImGui::DestroyContext();

    m_descriptorHeap = nullptr;
}

void DebugUI::BeginFrame()
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuizmo::BeginFrame();
}

void DebugUI::DrawPerformanceWindow(
    const std::vector<std::string>& environmentNames,
    size_t currentEnvironmentIndex,
    const RendererStatistics& statistics)
{
    ImGuiIO& io = ImGui::GetIO();
    const GpuTimestampProfiler::Results& gpu = statistics.gpuTimings;

    // Sample completed results independently of timing-window visibility.
    m_gpuTimingMonitor.Update(gpu);

    const auto visibilitySettings = GetPathTraceReferenceSettings();

    if(m_sampledVisibilityEpoch != m_visibilityTimingEpoch ||
        !(visibilitySettings == m_visibilityTimingSettings) ||
        !(GetSharcSettings() == m_visibilityTimingSharc) ||
        GetPathTraceDenoiserMode() != m_visibilityTimingDenoiser ||
        m_pathTraceDenoiserStages.temporalHistory != m_visibilityTimingStages.temporalHistory ||
        m_pathTraceDenoiserStages.spatialFilter != m_visibilityTimingStages.spatialFilter ||
        m_pathTraceDenoiserStages.historyResetRequest != m_visibilityTimingStages.historyResetRequest ||
        m_pathTraceDenoiserStages.extendedDiffuseHistory != m_visibilityTimingStages.extendedDiffuseHistory ||
        m_pathTraceRenderScale != m_visibilityTimingScale || m_lightingMode != m_visibilityTimingLighting ||
        !m_pathTraceActiveLastFrame)
    {
        m_visibilityTimings.Clear();
        m_sampledVisibilityEpoch = m_visibilityTimingEpoch;
        m_visibilityTimingSettings = visibilitySettings;
        m_visibilityTimingSharc = GetSharcSettings();
        m_visibilityTimingDenoiser = GetPathTraceDenoiserMode();
        m_visibilityTimingStages = GetPathTraceDenoiserStages();
        m_visibilityTimingScale = m_pathTraceRenderScale;
        m_visibilityTimingLighting = m_lightingMode;
        // Completed timestamps lag recording. Discard buffered/reset frames and 60 warm-up frames.
        m_visibilityWarmupUntil = gpu.frameSerial + 62;
    }

    if(gpu.frameSerial > m_visibilityWarmupUntil)
    {
        m_visibilityTimings.Update(gpu);
    }


    if(m_showPassTimings)
    {
        ImGui::SetNextWindowPos(ImVec2(20.0f, 40.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(460.0f, 650.0f), ImGuiCond_FirstUseEver);

        if(ImGui::Begin("Pass Timings", &m_showPassTimings))
        {
            ImGui::Text("%.1f FPS", io.Framerate);

            if (io.Framerate > 0.0f)
            {
                ImGui::Text("%.2f ms (smoothed frame interval)", 1000.0f / io.Framerate);
            }

            ImGui::Separator();

            ImGui::Text("Geometry draws: %llu", static_cast<unsigned long long>(statistics.geometryDrawCalls));

            ImGui::Text("Triangles: %llu", static_cast<unsigned long long>(statistics.submittedTriangles));

            ImGui::Text("Render objects: %llu", static_cast<unsigned long long>(statistics.renderObjectCount));

            ImGui::Text("TLAS instances: %llu", static_cast<unsigned long long>(statistics.tlasInstanceCount));

            ImGui::Separator();


            auto drawGpuTiming = [&](const char* name, GpuTimestampProfiler::Region region)
            {
                const unsigned int index = static_cast<unsigned int>(region);

                const uint64_t mask = 1ull << index;

                if ((gpu.validRegions & mask) != 0)
                {
                    ImGui::Text("%s: %.3f ms", name, gpu.milliseconds[index]);
                }
                else
                {
                    ImGui::Text("%s: -- (inactive / unavailable)", name);
                }
            };

            drawGpuTiming("GPU frame", GpuTimestampProfiler::Region::TotalFrame);

            if (gpu.interFrameGapValid)
            {
                ImGui::Text("GPU inter-frame gap: %.3f ms", gpu.interFrameGapMilliseconds);
            }
            else
            {
                ImGui::Text("GPU inter-frame gap: -- (%s)",
                            gpu.interFrameGapOutOfOrder ? "timestamp order error" : "unavailable");
            }
            if (gpu.interFrameGapSampleCount > 0)
            {
                ImGui::Text("  Last 120 valid: average %.3f ms | max %.3f ms", gpu.interFrameGapAverage,
                            gpu.interFrameGapMaximum);
            }
            else
            {
                ImGui::TextUnformatted("  Last 120 valid: average -- | max --");
            }
            ImGui::TextUnformatted("  Timestamp boundary interval; not proven GPU idle time.");

            ImGui::TextUnformatted("Geometry / scene");

            drawGpuTiming("  TLAS update", GpuTimestampProfiler::Region::TlasUpdate);

            drawGpuTiming("  Geometry", GpuTimestampProfiler::Region::Geometry);
            drawGpuTiming("  Selection mask", GpuTimestampProfiler::Region::SelectionMask);

            ImGui::TextUnformatted("Shadows");
            drawGpuTiming("  RT shadows", GpuTimestampProfiler::Region::RayTracedShadows);

            drawGpuTiming("  Shadow denoise", GpuTimestampProfiler::Region::ShadowDenoise);

            ImGui::TextUnformatted("Ambient occlusion");
            drawGpuTiming("  Ray-Traced AO", GpuTimestampProfiler::Region::RayTracedAO);
            drawGpuTiming("  AO Temporal", GpuTimestampProfiler::Region::AOTemporal);
            drawGpuTiming("  AO Denoise", GpuTimestampProfiler::Region::AODenoise);

            ImGui::TextUnformatted("Reflections");
            drawGpuTiming("  RT reflections", GpuTimestampProfiler::Region::RayTracedReflections);

            drawGpuTiming("  Reflection limiter", GpuTimestampProfiler::Region::ReflectionContributionLimiter);
            drawGpuTiming("  Native temporal", GpuTimestampProfiler::Region::ReflectionTemporal);
            drawGpuTiming("  Native spatial denoise", GpuTimestampProfiler::Region::ReflectionDenoise);

            drawGpuTiming("  NRD prepare", GpuTimestampProfiler::Region::NrdPrepare);

            drawGpuTiming("  NRD REBLUR Specular", GpuTimestampProfiler::Region::NrdReblurSpecular);
            drawGpuTiming("  NRD REBLUR Diffuse", GpuTimestampProfiler::Region::NrdReblurDiffuse);

            const unsigned int specularIndex = static_cast<unsigned int>(GpuTimestampProfiler::Region::NrdReblurSpecular);
            const unsigned int diffuseIndex = static_cast<unsigned int>(GpuTimestampProfiler::Region::NrdReblurDiffuse);
            const uint64_t reblurMask = (1ull << specularIndex) | (1ull << diffuseIndex);

            if ((gpu.validRegions & reblurMask) == reblurMask)
            {
                ImGui::Text("  NRD REBLUR: %.3f ms", gpu.milliseconds[specularIndex] + gpu.milliseconds[diffuseIndex]);
            }
            else
            {
                ImGui::TextUnformatted("  NRD REBLUR: -- (inactive / unavailable)");
            }

            drawGpuTiming("  NRD reflection resolve", GpuTimestampProfiler::Region::NrdResolve);

            ImGui::Text("Primary DI shadows: %s",
                m_lightingTransition.active == RendererLightingMode::PathTracedRasterPrimaryReSTIRDI ?
                (m_primaryDiShadowActive ? "RayQuery (DI.1)" : "TraceRay (Original)") : "Not in use");
            ImGui::Text("Secondary shadows: %s | rolling GPU median (last 30 samples)",
                m_secondaryShadowActive ? "RayQuery (GI.6)" : "TraceRay (Original)");
            if(ImGui::Button("Restart GPU comparison warm-up"))
            {
                m_visibilityTimings.Clear();
                m_visibilityWarmupUntil = gpu.frameSerial + 62;
            }

            const auto drawVisibilityMedian = [this](const char* label, GpuTimestampProfiler::Region region)
            {
                const auto& history = m_visibilityTimings.histories[static_cast<unsigned int>(region)];

                if(history.count >= GpuTimingMonitor::MinimumBaselineSamples && history.currentValid)
                {
                    ImGui::Text("  %s: %.3f ms", label, GpuTimingMonitor::CalculateBaseline(history));
                }
                else
                {
                    ImGui::Text("  %s: -- (warming up / inactive)", label);
                }
            };
            drawVisibilityMedian("Total GPU frame", GpuTimestampProfiler::Region::TotalFrame);
            drawVisibilityMedian("Split indirect", GpuTimestampProfiler::Region::PathTraceSplitIndirect);
            drawVisibilityMedian("Split direct", GpuTimestampProfiler::Region::PathTraceSplitDirect);
            drawVisibilityMedian("SHARC training", GpuTimestampProfiler::Region::RadianceCacheTraining);
            drawVisibilityMedian("NRD denoising", GpuTimestampProfiler::Region::NrdPathTraceDenoise);
            drawVisibilityMedian("Hybrid diffuse", GpuTimestampProfiler::Region::NrdHybridDiffuse);
            drawVisibilityMedian("Hybrid specular", GpuTimestampProfiler::Region::NrdHybridSpecular);

            ImGui::TextUnformatted("Path traced reference");
            drawGpuTiming("  Path trace iteration", GpuTimestampProfiler::Region::PathTraceReference);
            drawGpuTiming("    Split ReSTIR DI direct", GpuTimestampProfiler::Region::PathTraceSplitDirect);
            drawGpuTiming("    Split ReSTIR DI indirect", GpuTimestampProfiler::Region::PathTraceSplitIndirect);

            ImGui::TextUnformatted("Global illumination");
            drawGpuTiming("  Ray-Traced GI", GpuTimestampProfiler::Region::RayTracedGI);
            drawGpuTiming("  NRD diffuse resolve", GpuTimestampProfiler::Region::NrdDiffuseResolve);

            drawGpuTiming("  GI spatial denoise", GpuTimestampProfiler::Region::GISpatialDenoise);

            ImGui::TextUnformatted("Deferred lighting / presentation");
            drawGpuTiming("  Deferred lighting", GpuTimestampProfiler::Region::DeferredLighting);

            drawGpuTiming("  Tone map", GpuTimestampProfiler::Region::ToneMap);
            drawGpuTiming("  DebugUI rendering", GpuTimestampProfiler::Region::DebugUI);

            ImGui::Separator();

            ImGui::Text("TLAS update %s", m_tlasUpdated ? "yes" : "no");
            ImGui::Text("TLAS updates: %llu", static_cast<unsigned long long>(m_totalTLASUpdates));

        }

        ImGui::End();
    }

    if (m_showRenderingSettings)
    {
        if (ImGui::Begin("Rendering Settings", &m_showRenderingSettings))
        {

            DrawCpuTiming();
            DrawFramePacingSpikes();



            DrawGpuTimingMonitor(gpu);

            DrawPathTraceReferenceSettings();

            ImGui::TextUnformatted("Environment");

            const char* currentEnvironmentName = currentEnvironmentIndex < environmentNames.size()
                                                     ? environmentNames[currentEnvironmentIndex].c_str()
                                                     : "Unknown";

            if (ImGui::BeginCombo("HDRI", currentEnvironmentName))
            {
                for (size_t i = 0; i < environmentNames.size(); ++i)
                {
                    ImGui::PushID(static_cast<int>(i));
                    const bool selected = i == currentEnvironmentIndex;

                    if (ImGui::Selectable(environmentNames[i].c_str(), selected))
                    {
                        m_requestedEnvironmentIndex = static_cast<int>(i);
                    }

                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                    ImGui::PopID();
                }

                ImGui::EndCombo();
            }

            ImGui::Separator();

            const char* views[] = {"None",
                                   "Specular tracking distance",
                                   "NRD ViewZ",
                                   "NRD Normal",
                                   "NRD Roughness",
                                   "NRD Motion",
                                   "NRD Spec Radiance",
                                   "NRD Hit Distance",
                                   "NrdResolvedReflection",
                                   "GBuffer Motion",
                                   "Selection Mask (BLACK SCREEN)",
                                   "GI Raw",
                                   "GI Hit Distance",
                                   "GI Denoised",
                                   "AO Raw",
                                   "AO Temporal",
                                   "AO Filtered",
                                   "GBuffer World Normal",
                                   "GBuffer Albedo",
                                   "Environment Specular Only",
                                   "Direct Lighting Only",
                                   "HDRI Diffuse Ambient Only",
                                   "HDRI Diffuse Ambient Without AO",
                                   "HDRI Diffuse Ambient Direct Integral",
                                   "Irradiance Cubemap Minus Direct Integral",
                                   "HDRI Irradiance Tangent Frame Difference",
                                   "HDRI Diffuse Ambient Multi-Orientation",
                                   "Multi-Orientation Minus Single-Orientation",
                                   "HDRI Direct Integral Mip 0",
                                   "HDRI Direct Integral Mip 4",
                                   "HDRI Direct Integral Mip 6",
                                   "GI Temporal Irradiance",
#ifndef NDEBUG
                                   "Shading Normal View Horizon",
#endif
            };
            const char* denoisers[] = {"NRD", "Native"};

            ImGui::Combo("Reflection denoiser", &m_reflectionDenoiserMode, denoisers, IM_ARRAYSIZE(denoisers));
            ImGui::Checkbox("NRD contribution limiter", &m_contributionLimiterEnabled);
            ImGui::Checkbox("Enable RTGI", &m_rtgiEnabled);
            ImGui::SliderFloat("RTGI Contact AO Strength", &m_rtgiContactAOStrength, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("RTGI Contact AO Radius", &m_rtgiContactAORadius, 0.05f, 5.0f, "%.2f");
            ImGui::Combo("Diagnostic view", &m_toneMapDebugView, views, IM_ARRAYSIZE(views));

            // RTXPT D.3B.0: path-trace denoiser views. They need the diagnostic extraction and temporal passes
            // (Renderer flags); otherwise the presentation is unchanged.
            {
                const char* denoiserViews[] = {"Normal", "Temporal (demodulated)", "Spatial (demodulated)",
                    "Spatial - temporal difference", "Estimated uncertainty", "History length", "Raw extraction"};
                const char* signals[] = {"Diffuse lighting", "Specular"};
                int denoiserView = static_cast<int>(m_pathTraceDenoiserVisualization.view);
                int signal = static_cast<int>(m_pathTraceDenoiserVisualization.signal);
                ImGui::Combo("PT denoiser view", &denoiserView, denoiserViews, IM_ARRAYSIZE(denoiserViews));
                ImGui::Combo("PT denoiser signal", &signal, signals, IM_ARRAYSIZE(signals));
                ImGui::SliderFloat("PT view exposure (stops)", &m_pathTraceDenoiserVisualization.exposureStops, -8.0f, 16.0f,
                    "%.1f");
                ImGui::SliderFloat("PT difference amplification", &m_pathTraceDenoiserVisualization.differenceAmplification,
                    1.0f, 1024.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
                ImGui::SliderFloat("PT uncertainty floor (log10)", &m_pathTraceDenoiserVisualization.uncertaintyFloorLog10,
                    -6.0f, -1.0f, "%.1f");
                m_pathTraceDenoiserVisualization.view = static_cast<PathTraceDenoiserView>(denoiserView);
                m_pathTraceDenoiserVisualization.signal = static_cast<std::uint32_t>(signal);

                if(denoiserView == 3)
                {
                    ImGui::TextWrapped("Grey: |spatial - temporal| luminance x 2^stops x amplification. What filtering "
                        "changed, not error.");
                }
                else if(denoiserView == 4)
                {
                    ImGui::TextWrapped("sigma / Y of the spatial filter's estimate, log scale: blue = floor, green, red = "
                        "100%. Magenta: history 1 (unknown). Cyan: no estimate. Stripes: history 2-7. Not error.");
                }
                else if(denoiserView == 5)
                {
                    ImGui::TextWrapped("Red: history 1 (new or rejected). Orange to yellow: 2-7. Yellow-green to green: "
                        "8 to 32.");
                }
            }

#ifndef NDEBUG
            ImGui::Checkbox("Shading normal horizon correction", &m_shadingNormalCorrectionEnabled);

            if (GetToneMapDebugView() == ToneMapDebugView::ShadingNormalViewHorizon)
            {
                ImGui::TextWrapped("Magenta: Ng dot V > 0 and Ns dot V <= 0. Black: no crossing. "
                                   "Yellow: invalid input. Ng is the unperturbed interpolated vertex normal.");
            }
#endif

            if (GetToneMapDebugView() == ToneMapDebugView::GISignedReconstructed)
            {
                ImGui::TextWrapped("Temporal diffuse irradiance before spatial reconstruction. Historical cooked bundles show their original reconstructed signal.");
            }

            if (GetToneMapDebugView() == ToneMapDebugView::HDRIDirectIntegralMip0 ||
                GetToneMapDebugView() == ToneMapDebugView::HDRIDirectIntegralMip4 ||
                GetToneMapDebugView() == ToneMapDebugView::HDRIDirectIntegralMip6)
            {
                ImGui::TextUnformatted(
                    "4096-sample direct HDRI diffuse integral at selected source mip; no AO, standard display mapping.");
            }

            if (GetToneMapDebugView() == ToneMapDebugView::HDRIDiffuseAmbientMultiOrientation)
            {
                ImGui::TextUnformatted(
                    "Four 1024-sample HDRI integrals at 0/45/90/135 degrees, averaged; no AO, standard display mapping.");
            }
            else if (GetToneMapDebugView() == ToneMapDebugView::MultiOrientationMinusSingleOrientation)
            {
                ImGui::TextUnformatted("Multi 4x1024 minus single 4096 samples; residual scale 10, no tone mapping.");
            }
            if (GetToneMapDebugView() == ToneMapDebugView::HDRIIrradianceTangentFrameDifference)
            {
                ImGui::TextUnformatted(
                    "4096 samples per tangent frame: original minus 90-degree rotated; residual scale 10, no tone mapping.");
            }

            if (GetToneMapDebugView() == ToneMapDebugView::HDRIDiffuseAmbientDirectIntegral)
            {
                ImGui::TextUnformatted("4096-sample mip-0 HDRI diffuse integral at the raster normal, without AO; expensive.");
            }

            else if (GetToneMapDebugView() == ToneMapDebugView::IrradianceCubemapMinusDirectIntegral)
            {
                ImGui::TextUnformatted(
                    "Cube minus direct irradiance: gray zero, red positive, blue negative; scale 10, no tone mapping.");
            }

            if (GetToneMapDebugView() == ToneMapDebugView::EnvironmentSpecularOnly)
            {
                ImGui::TextUnformatted("Production environment specular before DXR blending; standard display mapping.");
            }
            else if (GetToneMapDebugView() == ToneMapDebugView::DirectLightingOnly)
            {
                ImGui::TextUnformatted(
                    "Production directional diffuse and specular, including shadows; standard display mapping.");
            }
            else if (GetToneMapDebugView() == ToneMapDebugView::HDRIDiffuseAmbientOnly)
            {
                ImGui::TextUnformatted("Production RTGI-off HDRI diffuse ambient including AO; standard display mapping.");
            }
            else if (GetToneMapDebugView() == ToneMapDebugView::HDRIDiffuseAmbientWithoutAO)
            {
                ImGui::TextUnformatted("Same HDRI diffuse ambient with AO visibility fixed to one; standard display mapping.");
            }
            if (GetToneMapDebugView() == ToneMapDebugView::GBufferWorldNormal)
            {
                ImGui::TextUnformatted("Raw raster world normal: XYZ mapped to RGB; black background, magenta invalid.");
            }
            else if (GetToneMapDebugView() == ToneMapDebugView::GBufferAlbedo)
            {
                ImGui::TextUnformatted("Raster linear albedo encoded to sRGB for display; no lighting or tone mapping.");
            }
            ImGui::Separator();

            if (GetToneMapDebugView() == ToneMapDebugView::SpecularTrackingDistance)
            {
                ImGui::TextUnformatted("Magenta: invalid | Blue: environment miss | Gray: geometry");
            }
            else if (GetToneMapDebugView() == ToneMapDebugView::GBufferMotion)
            {
                ImGui::TextUnformatted("Gray: zero UV motion | R: signed X | G: signed Y (20x)");
            }

            else if (GetToneMapDebugView() == ToneMapDebugView::NrdViewZ)
            {
                ImGui::TextUnformatted("Gray: Z / (Z + 10) | Magenta: beyond NRD range | Red: invalid Z");
            }

            else if (GetToneMapDebugView() == ToneMapDebugView::NrdMotion)
            {
                ImGui::TextUnformatted("Black: exact zero XYZ | RGB: amplified absolute XYZ (100x)");

                ImGui::TextUnformatted("Any nonzero component has at least 25% intensity; camera motion should stay black.");
            }
            else if (GetToneMapDebugView() == ToneMapDebugView::NrdSpecularRadiance)
            {
                ImGui::TextUnformatted("Decoded demodulated radiance with AgX display mapping");
            }
            else if (GetToneMapDebugView() == ToneMapDebugView::NrdRoughness ||
                     GetToneMapDebugView() == ToneMapDebugView::NrdHitDistance)
            {

                ImGui::TextUnformatted("Decoded value: 0 = black | 1 = white");
            }
            else if (GetToneMapDebugView() == ToneMapDebugView::GIRaw)
            {
                ImGui::TextUnformatted("Raw diffuse GI irradiance (noisy) | Black: invalid surface");
            }
            else if (GetToneMapDebugView() == ToneMapDebugView::GIHitDistance)
            {
                ImGui::TextUnformatted("Gray: hit distance | Blue: miss | Magenta: invalid");
            }
            else if (GetToneMapDebugView() == ToneMapDebugView::AORaw)
            {
                ImGui::TextUnformatted("White: unoccluded | Black: occluded | Background: white");
            }
            else if (GetToneMapDebugView() == ToneMapDebugView::AOTemporal)
            {

                ImGui::TextUnformatted("Temporally accumulated ray-traced ambient occlusion.");
            }
            else if (GetToneMapDebugView() == ToneMapDebugView::AOFiltered)
            {
                ImGui::TextUnformatted("Temporally accumulated AO after edge-aware spatial filtering.");
            }
        }

        ImGui::End();
    }

}

void DebugUI::EndFrame(
    ID3D12GraphicsCommandList* commandList,
    bool* textureUploadRequested)
{
    m_editorIcons.Prepare(commandList);
    m_gpuTimingGraph.Draw(m_gpuTimingMonitor);

    ImGui::Render();

    if (textureUploadRequested)
    {
        *textureUploadRequested = false;

        const ImDrawData* drawData = ImGui::GetDrawData();

        if (drawData && drawData->Textures)
        {
            for (const ImTextureData* texture : *drawData->Textures)
            {
                if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates)
                {
                    *textureUploadRequested = true; // Request observed, not proof of wait duration.
                }
            }
        }
    }

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
}

void DebugUI::SetTLASUpdated(bool updated)
{
    m_tlasUpdated = updated;

    if (updated)
    {
        ++m_totalTLASUpdates;
    }
}

PathTraceReferenceSettings DebugUI::GetPathTraceReferenceSettings() const
{
    PathTraceReferenceSettings settings;
    settings.secondaryShadowRayQuery = m_secondaryShadowVisibility == 1;
    settings.primaryDiShadowRayQuery = m_primaryDiShadowVisibility == 1;
    settings.samplesPerFrame = static_cast<std::uint32_t>(m_pathTraceSamplesPerFrame);
    settings.maxPathDepth = static_cast<std::uint32_t>(m_pathTraceMaxPathDepth);
    settings.russianRouletteDepth = static_cast<std::uint32_t>(m_pathTraceRussianRouletteDepth);
    settings.maxAccumulatedSamples = static_cast<std::uint32_t>(m_pathTraceMaxAccumulatedSamples);
    return settings;
}

namespace
{
    constexpr std::uint32_t SharcTrainingTiles[] = {5, 8};
    constexpr std::uint32_t SharcMinimumSamples[] = {8, 16, 32, 64, 256};
}

SharcSettings DebugUI::GetSharcSettings() const
{
    SharcSettings settings;
    settings.enabled = m_sharcEnabled;
    settings.trainingTile = SharcTrainingTiles[m_sharcDensity];
    settings.minimumSamples = SharcMinimumSamples[m_sharcMinimumSamples];
    settings.freezeTraining = m_sharcFreezeTraining;
    settings.statistics = m_sharcStatistics;
    settings.resetRequest = m_sharcResetRequest;
    settings.adaptiveResolution = m_sharcSpatialResolution == 1;
    settings.historyPrior = m_sharcHistoryPrior && m_pathTraceDenoiserMode == PathTraceDenoiserMode::Engine;
    return settings;
}

void DebugUI::DrawSharcSettings()
{
    ImGui::TextUnformatted("SHARC radiance cache (experimental, not saved)");
    ImGui::Checkbox("Enable SHARC", &m_sharcEnabled);

    if (!m_sharcEnabled)
    {
        return;
    }

    const char* densities[] = {"5x5 (1/25)", "8x8 (1/64)"};
    ImGui::Combo("Training density", &m_sharcDensity, densities, IM_ARRAYSIZE(densities));
    const char* minimumSamples[] = {"8", "16", "32", "64", "256"};
    ImGui::Combo("Minimum cache samples", &m_sharcMinimumSamples, minimumSamples, IM_ARRAYSIZE(minimumSamples));
    // Switching rebuilds the cache.
    const char* spatialResolutions[] = {"Uniform 10 cm", "Adaptive (default)"};
    ImGui::Combo("Spatial resolution", &m_sharcSpatialResolution, spatialResolutions, IM_ARRAYSIZE(spatialResolutions));
    // Initializes rejected denoiser history from the cache, so it needs the Engine PT denoiser.
    ImGui::BeginDisabled(m_pathTraceDenoiserMode != PathTraceDenoiserMode::Engine);
    ImGui::Checkbox("SHARC-assisted history initialization (experimental)", &m_sharcHistoryPrior);
    ImGui::EndDisabled();
    ImGui::Checkbox("Freeze training", &m_sharcFreezeTraining);
    ImGui::SameLine();

    if (ImGui::Button("Reset cache"))
    {
        ++m_sharcResetRequest;
    }

    ImGui::Checkbox("SHARC statistics", &m_sharcStatistics);

    if (!m_sharcStatus.preparation.empty())
    {
        ImGui::TextWrapped("%s", m_sharcStatus.preparation.c_str());
        return;
    }

    if (!m_sharcStatus.active)
    {
        ImGui::TextDisabled("Inactive: waiting for a scene with ray-traced geometry.");
        return;
    }

    ImGui::Text("Trained frames: %llu%s", static_cast<unsigned long long>(m_sharcStatus.trainedFrames),
                m_sharcFreezeTraining ? " (frozen)" : "");
    ImGui::Text("Cache memory: %.1f MiB", static_cast<double>(m_sharcStatus.cacheBytes) / (1024.0 * 1024.0));

    if (m_sharcStatistics && m_sharcStatus.statisticsValid)
    {
        const double eligible = m_sharcStatus.eligibleQueries > 0 ? m_sharcStatus.eligibleQueries : 1.0;
        ImGui::Text("Lookup hits: %.1f%%, cached paths: %.1f%% of %u eligible",
                    100.0 * m_sharcStatus.lookupHits / eligible, 100.0 * m_sharcStatus.substitutions / eligible,
                    m_sharcStatus.eligibleQueries);
        ImGui::Text("Occupied entries: %u", m_sharcStatus.occupiedEntries);
    }
}

void DebugUI::SetPathTraceReferenceStatus(
    PathTraceReferenceAvailability availability,
    const std::string& unavailableReason,
    bool activeLastFrame,
    std::uint32_t accumulatedSamples,
    std::uint64_t resourceBytes)
{
    m_pathTraceAvailability = availability;
    m_pathTraceUnavailableReason = unavailableReason;
    m_pathTraceActiveLastFrame = activeLastFrame;
    m_pathTraceAccumulatedSamples = accumulatedSamples;
    m_pathTraceResourceBytes = resourceBytes;
}

void DebugUI::SetPathTraceRasterPrimaryStatus(PathTraceRasterPrimaryAvailability availability, const std::string& reason)
{
    m_rasterPrimaryAvailability = availability;
    m_rasterPrimaryReason = reason;
}

void DebugUI::SetPathTraceReSTIRDIStatus(PathTraceRasterPrimaryAvailability availability, const std::string& reason)
{
    m_restirDIAvailability = availability;
    m_restirDIReason = reason;
}

void DebugUI::SetLightingModeTransitionStatus(const LightingModeTransitionStatus& status)
{
    m_lightingTransition = status;
}

void DebugUI::DrawLightingModeTransitionIndicator(
    float viewportX,
    float viewportY,
    float viewportWidth,
    float viewportHeight)
{
    constexpr float Margin = 12.0f;
    const bool preparing = IsLightingModePreparing(m_lightingTransition.stage);
    const bool failed = m_lightingTransition.stage == LightingModeTransitionStage::Failed;

    if(!preparing)
    {
        m_lightingPreparationStartTime = -1.0;
    }

    if(!failed)
    {
        // Idle forgets the dismissal; a new attempt may fail again and is then shown again.
        m_dismissedLightingFailure.clear();
    }

    if(viewportWidth <= 0.0f || viewportHeight <= 0.0f)
    {
        return;
    }

    const bool showFailure = failed && GetLightingModeFailureKey(m_lightingTransition) != m_dismissedLightingFailure;

    if(!preparing && !showFailure)
    {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(viewportX + viewportWidth - Margin, viewportY + viewportHeight - Margin),
        ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove;

    if(preparing)
    {
        // Elapsed time as this UI observed it, from the first snapshot of this preparation.
        if(m_lightingPreparationStartTime < 0.0 || m_lightingPreparationMode != m_lightingTransition.requested)
        {
            m_lightingPreparationStartTime = ImGui::GetTime();
            m_lightingPreparationMode = m_lightingTransition.requested;
        }

        const int elapsedSeconds = static_cast<int>(ImGui::GetTime() - m_lightingPreparationStartTime);

        // Display only: clicks pass through to the viewport underneath.
        if(ImGui::Begin("##LightingModePreparation", nullptr, flags | ImGuiWindowFlags_NoInputs))
        {
            ImGui::Text("Preparing %s", GetLightingModeDisplayName(m_lightingTransition.requested));
            ImGui::TextDisabled("%s, %d s. Rendering %s.", GetLightingModeStageLabel(m_lightingTransition.stage),
                elapsedSeconds, GetLightingModeDisplayName(m_lightingTransition.active));
            // A negative fraction is ImGui's indeterminate animation: DXC reports no progress.
            ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(260.0f, 0.0f), "");
        }

        ImGui::End();
        return;
    }

    if(ImGui::Begin("##LightingModeFailure", nullptr, flags))
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
        ImGui::Text("%s could not be activated.", GetLightingModeDisplayName(m_lightingTransition.requested));
        ImGui::TextDisabled("Rendering %s. Saving a fix to the shader retries automatically.",
            GetLightingModeDisplayName(m_lightingTransition.active));
        ImGui::TextUnformatted(SummarizeLightingModeDiagnostic(m_lightingTransition.diagnostic, 3).c_str());
        ImGui::PopTextWrapPos();

        if(ImGui::SmallButton("Dismiss"))
        {
            m_dismissedLightingFailure = GetLightingModeFailureKey(m_lightingTransition);
        }
    }

    ImGui::End();
}

void DebugUI::DrawPathTraceReferenceSettings()
{
    ImGui::TextUnformatted("Lighting mode (experimental, not saved)");

    const char* modes[] = {"Legacy", "Path Traced Reference", "Path Traced Raster Primary",
        "Path Traced Raster Primary + ReSTIR DI"};
    // While the Renderer prepares the requested mode, the selector shows it and accepts no other request.
    const bool preparing = IsLightingModePreparing(m_lightingTransition.stage);
    ImGui::BeginDisabled(preparing);
    ImGui::Combo("Lighting mode", &m_lightingMode, modes, IM_ARRAYSIZE(modes));
    ImGui::EndDisabled();

    const bool transitionFailed = m_lightingTransition.stage == LightingModeTransitionStage::Failed &&
        m_lightingTransition.requested == GetLightingMode();

    if(preparing)
    {
        ImGui::TextDisabled("Preparing %s (%s); %s keeps rendering.",
            GetLightingModeDisplayName(m_lightingTransition.requested),
            GetLightingModeStageLabel(m_lightingTransition.stage),
            GetLightingModeDisplayName(m_lightingTransition.active));
    }
    else if(transitionFailed)
    {
        ImGui::TextWrapped("Unavailable, rendering %s: %s", GetLightingModeDisplayName(m_lightingTransition.active),
            SummarizeLightingModeDiagnostic(m_lightingTransition.diagnostic, 3).c_str());
    }

    if (m_lightingMode == 0)
    {
        ImGui::Separator();
        return;
    }

    const char* visibilityModes[] = {"TraceRay (Original)", "RayQuery (GI.6)"};
    const char* primaryDiVisibilityModes[] = {"TraceRay (Original)", "RayQuery (DI.1)"};
    ImGui::BeginDisabled(!m_primaryDiShadowReady || preparing || m_lightingMode != 3);
    ImGui::Combo("Primary DI Shadow Visibility", &m_primaryDiShadowVisibility,
        primaryDiVisibilityModes, IM_ARRAYSIZE(primaryDiVisibilityModes));
    ImGui::EndDisabled();
    ImGui::Text("Active primary DI shadows: %s",
        m_lightingTransition.active == RendererLightingMode::PathTracedRasterPrimaryReSTIRDI ?
        primaryDiVisibilityModes[m_primaryDiShadowActive ? 1 : 0] : "Not in use");

    if(m_lightingMode == 3 && m_primaryDiShadowFailed)
    {
        ImGui::TextDisabled("DI.1 pipelines failed (see the DI.1 log warning); original retained.");
    }
    else if(m_lightingMode == 3 && !m_primaryDiShadowReady)
    {
        ImGui::TextDisabled("DI.1 pipelines still preparing; original retained.");
    }

    ImGui::BeginDisabled(!m_secondaryShadowReady || preparing);
    ImGui::Combo("Secondary Shadow Visibility", &m_secondaryShadowVisibility,
        visibilityModes, IM_ARRAYSIZE(visibilityModes));
    ImGui::EndDisabled();
    ImGui::Text("Active secondary shadows: %s", visibilityModes[m_secondaryShadowActive ? 1 : 0]);

    if(!m_secondaryShadowReady)
    {
        ImGui::TextDisabled("GI.6 unavailable or pipelines still preparing; original retained.");
    }

    // Both path-traced modes share these settings; ranges match IsValidPathTraceReferenceSettings and every
    // change restarts accumulation.
    DrawPathTraceSampleDiagnostic();
    ImGui::SliderInt("PT samples per frame", &m_pathTraceSamplesPerFrame, 1,
                     static_cast<int>(PathTraceReferenceSettings::MaximumSamplesPerFrame));
    ImGui::SliderInt("PT max path depth", &m_pathTraceMaxPathDepth, 1,
                     static_cast<int>(PathTraceReferenceSettings::MaximumPathDepth));
    ImGui::SliderInt("PT roulette from depth", &m_pathTraceRussianRouletteDepth, 1,
                     static_cast<int>(PathTraceReferenceSettings::MaximumPathDepth) + 1);
    ImGui::SliderInt("PT sample target", &m_pathTraceMaxAccumulatedSamples, 1, 1 << 16, "%d",
                     ImGuiSliderFlags_Logarithmic);

    if (m_pathTraceMaxAccumulatedSamples < m_pathTraceSamplesPerFrame)
    {
        m_pathTraceMaxAccumulatedSamples = m_pathTraceSamplesPerFrame;
    }

    const bool rasterPrimary = m_lightingMode == 2;
    const bool restirDI = m_lightingMode == 3;

    // UPSCALING.3A: informational only; there is no DLSS mode yet.
    ImGui::Text("DLSS Super Resolution: %s", m_dlssState.c_str());

    if (!m_dlssDetail.empty() && ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", m_dlssDetail.c_str());
    }

    // UPSCALING.3B: prepares DLSS inputs (motion vectors, exposure, DLSS jitter phases); DLSS itself never runs.
    ImGui::Checkbox("DLSS input preparation (diagnostic)", &m_dlssInputPreparation);

    // SHARC.4E.1 / 5B: the denoiser and SHARC serve both raster-primary modes.
    if (rasterPrimary || restirDI)
    {
        // UPSCALING.2C (experimental, 100% by default): a reduced internal resolution, presented through a bilinear
        // diagnostic resolve. This is not DLSS and not an upscaler.
        const char* renderScales[] = {"100% (Native)", "75% (diagnostic bilinear resolve, not DLSS)",
                                      "75% DLSS Super Resolution (diagnostic)", "DLSS Quality", "DLSS Balanced",
                                      "DLSS Performance"};
        ImGui::Combo("PT render scale (experimental)", &m_pathTraceRenderScale, renderScales, IM_ARRAYSIZE(renderScales));

        // UPSCALING.2B (experimental, off by default): diagnostics for future temporal upscaling.
        ImGui::Checkbox("PT camera jitter (experimental)", &m_pathTraceCameraJitter);

        if (m_pathTraceCameraJitterActive)
        {
            ImGui::Text("Jitter phase %u/%u: (%+.3f, %+.3f) px", m_pathTraceCameraJitterPhase,
                        m_pathTraceCameraJitterPhaseCount, m_pathTraceCameraJitterX, m_pathTraceCameraJitterY);
        }

        // NRD.2: only the implemented modes are offered; the combo index is the PathTraceDenoiserMode value. NRD.4B: REBLUR
        // (experimental); NRD.RELAX.4: RELAX (experimental); HYBRID.5: the hybrid (experimental).
        const char* denoiserModes[] = {"Off", "Engine", "NRD REBLUR (experimental)", "NRD RELAX (experimental)",
                                       "NRD Hybrid (experimental)"};
        int denoiserMode = static_cast<int>(m_pathTraceDenoiserMode);

        if (ImGui::Combo("PT denoiser", &denoiserMode, denoiserModes, IM_ARRAYSIZE(denoiserModes)))
        {
            m_pathTraceDenoiserMode = static_cast<PathTraceDenoiserMode>(denoiserMode);
        }

        if (!m_pathTraceDenoiserPreparation.empty())
        {
            ImGui::TextWrapped("%s", m_pathTraceDenoiserPreparation.c_str());
        }

        if (IsNrdPathTraceDenoiserMode(m_pathTraceDenoiserMode))
        {
            if (!m_pathTraceNrdStatus.empty())
            {
                ImGui::TextWrapped("%s", m_pathTraceNrdStatus.c_str());
            }

            if (ImGui::Button("Reset PT denoiser history"))
            {
                ++m_pathTraceDenoiserStages.historyResetRequest;
            }
        }

        // DENOISE.1: stage isolation for diagnosing the denoiser; the defaults are the production pipeline.
        if (m_pathTraceDenoiserMode == PathTraceDenoiserMode::Engine)
        {
            const char* spatialFilters[] = {"Off", "5x5 single pass", "A-trous, 1 iteration",
                                            "A-trous, 2 iterations (default)"};
            int spatialFilter = static_cast<int>(m_pathTraceDenoiserStages.spatialFilter);
            ImGui::Checkbox("PT temporal history", &m_pathTraceDenoiserStages.temporalHistory);
            ImGui::Combo("PT spatial filter", &spatialFilter, spatialFilters, IM_ARRAYSIZE(spatialFilters));
            m_pathTraceDenoiserStages.spatialFilter = static_cast<PathTraceSpatialFilter>(spatialFilter);

            if (ImGui::Button("Reset PT denoiser history"))
            {
                ++m_pathTraceDenoiserStages.historyResetRequest;
            }

            // DENOISE.3B (experimental, off by default).
            ImGui::Checkbox("PT extended diffuse history (experimental)", &m_pathTraceDenoiserStages.extendedDiffuseHistory);

            if (!m_pathTraceDenoiserStages.temporalHistory)
            {
                ImGui::TextWrapped(spatialFilter == 0 ? "Raw: this frame's samples, no denoising."
                                                      : "Spatial only: this frame's samples, spatially filtered.");
            }
        }

        DrawSharcSettings();
    }

    if (preparing || transitionFailed)
    {
        // Explained above, next to the selector.
    }
    else if (rasterPrimary && m_rasterPrimaryAvailability == PathTraceRasterPrimaryAvailability::Unavailable)
    {
        ImGui::TextWrapped("Unavailable, rendering Legacy: %s", m_rasterPrimaryReason.c_str());
    }
    else if (restirDI && m_restirDIAvailability == PathTraceRasterPrimaryAvailability::Unavailable)
    {
        ImGui::TextWrapped("Unavailable, rendering Legacy: %s", m_restirDIReason.c_str());
    }
    else if (!rasterPrimary && !restirDI && m_pathTraceAvailability == PathTraceReferenceAvailability::Unavailable)
    {
        ImGui::TextWrapped("Unavailable, rendering Legacy: %s", m_pathTraceUnavailableReason.c_str());
    }
    else if (m_pathTraceActiveLastFrame)
    {
        ImGui::Text("Accumulated %u / %d samples per pixel", m_pathTraceAccumulatedSamples,
                    m_pathTraceMaxAccumulatedSamples);
        ImGui::Text("Accumulation memory: %.2f MiB",
                    static_cast<double>(m_pathTraceResourceBytes) / (1024.0 * 1024.0));
    }
    else
    {
        ImGui::TextUnformatted("Waiting for a scene with ray-traced geometry.");
    }

    ImGui::Separator();
}

void DebugUI::DrawMenu()
{
    if(ImGui::MenuItem("Pass Timings", nullptr, &m_showPassTimings) && m_showPassTimings)
    {
        ImGui::SetWindowFocus("Pass Timings");
    }


    ImGui::MenuItem("Rendering Settings", nullptr, &m_showRenderingSettings);
    ImGui::Separator();
    ImGui::MenuItem("GPU Timing Graph", nullptr, &m_gpuTimingGraph.visible);
}

void DebugUI::DrawPathTraceSampleDiagnostic()
{
    if(!ImGui::CollapsingHeader("Indirect sample provenance (R.9I.1)")) return;
    ImGui::Checkbox("Capture selected PT pixel", &m_sampleDiagnosticSettings.enabled);
    ImGui::InputInt("Render pixel X", &m_sampleDiagnosticSettings.x);
    ImGui::InputInt("Render pixel Y", &m_sampleDiagnosticSettings.y);
    ImGui::InputFloat("Detail luminance threshold (capture only)", &m_sampleDiagnosticSettings.threshold);
    if(!std::isfinite(m_sampleDiagnosticSettings.threshold) || m_sampleDiagnosticSettings.threshold < 0.0f)
        m_sampleDiagnosticSettings.threshold = 0.0f;
    if(ImGui::Button("Clear sample records"))
    {
        ++m_sampleDiagnosticSettings.clearRequest;
        m_sampleDiagnosticStatus.history.clear();
        m_sampleDiagnosticStatus.havePeak = false;
    }
    const auto& status = m_sampleDiagnosticStatus;
    ImGui::Text("%s", status.state.c_str());
    ImGui::Text("Pixel (%d,%d), target %u x %u; run %llu; current N=%u",
        m_sampleDiagnosticSettings.x, m_sampleDiagnosticSettings.y, status.width, status.height,
        static_cast<unsigned long long>(status.run), status.currentSamples);
    ImGui::Text("Captured %llu; dropped %llu; history overwritten %llu (last 128 kept)",
        static_cast<unsigned long long>(status.captures), static_cast<unsigned long long>(status.dropped),
        static_cast<unsigned long long>(status.overwritten));
    ImGui::TextWrapped("Scene-linear HDR before denoisers, exposure, tone mapping and DLSS. Mean is after the entire batch, not after each individual sample. No new records at the sample target.");
    const auto rgb = [](const char* label, const float* value)
    {
        ImGui::Text("%s RGB (%.7g, %.7g, %.7g), Y %.7g", label, value[0], value[1], value[2], value[3]);
    };
    const auto record = [&](const PathTraceSampleRecord& value)
    {
        ImGui::Text("Frame %u, sample %u, old N=%u, batch=%u; depth=%u target=%u seed=%u",
            value.meta[0], value.meta[1], value.meta[2], value.meta[3], value.config[0], value.config[1], value.config[2]);
        rgb("Raw sample", value.sample); rgb("Indirect", value.indirect); rgb("Progressive mean", value.mean);
        rgb("Primary", value.primary); rgb("Secondary direct", value.secondary); rgb("Deeper direct", value.deeper);
        rgb("Miss", value.miss); rgb("SHARC substitution", value.cache);
        ImGui::Text("SHARC %s; rejected %s; expanded provenance %s; secondary distance %.7g",
            value.config[3] & 1u ? "yes" : "no", value.config[3] & 2u ? "yes" : "no",
            value.config[3] & 4u ? "yes" : "no", value.hit[0]);
        if(value.config[3] & 4u)
        {
            rgb("Dominant individual secondary light", value.dominant);
            rgb("Its diffuse", value.diffuse); rgb("Its specular remainder", value.specular);
            ImGui::Text("Vertex %u, light %u type %u, material %u, roughness %.7g",
                value.info[0], value.info[1], value.info[2], value.info[3], value.hit[1]);
            ImGui::Text("f*cos*Li (%.7g,%.7g,%.7g), cosine %.7g",
                value.factors[0], value.factors[1], value.factors[2], value.factors[3]);
            ImGui::Text("Li (%.7g,%.7g,%.7g), light distance %.7g, light PDF %.7g, MIS/pdf %.7g",
                value.incidence[0], value.incidence[1], value.incidence[2], value.incidence[3], value.hit[2], value.hit[3]);
            ImGui::Text("Incoming throughput (%.7g,%.7g,%.7g), incoming BSDF PDF %.7g",
                value.throughput[0], value.throughput[1], value.throughput[2], value.throughput[3]);
        }
    };
    if(!status.history.empty())
    {
        ImGui::SeparatorText("Latest completed GPU record"); record(status.history.back());
    }
    if(status.havePeak && ImGui::TreeNode("Peak indirect sample (this capture/run)"))
    {
        record(status.peak); ImGui::TreePop();
    }
    if(ImGui::TreeNode("Recent individual samples"))
    {
        ImGui::BeginChild("sample-history", ImVec2(0, 180), true);
        for(auto it = status.history.rbegin(); it != status.history.rend(); ++it)
            ImGui::Text("frame %u sample %u N=%u+%u: indirect Y %.7g, mean Y %.7g, sample Y %.7g%s",
                it->meta[0], it->meta[1], it->meta[2], it->meta[3], it->indirect[3], it->mean[3], it->sample[3],
                it->config[3] & 1u ? " SHARC" : "");
        ImGui::EndChild(); ImGui::TreePop();
    }
    ImGui::TextWrapped("Light types: 0 point, 1 spot, 2 directional, 3 sun, 4 environment. Sun/environment indices use UINT_MAX-1/-2. UINT_MAX means no dominant visible light. Raw BSDF f and a per-bounce weight trace are not captured; f*cos*Li and incoming throughput/PDF are captured without additional evaluations.");
    DrawPathTraceAutoCapture();
}

namespace
{
    float AutoLuminance(float r, float g, float b)
    {
        return 0.2126f * r + 0.7152f * g + 0.0722f * b;
    }

    std::string AutoRGB(const float* value)
    {
        return std::format("({:.7g}, {:.7g}, {:.7g}) Y {:.7g}", value[0], value[1], value[2], value[3]);
    }

    std::string AutoLightType(std::uint32_t type)
    {
        constexpr const char* names[] = {"point", "spot", "directional", "sun", "environment"};
        return type < 5 ? names[type] : "unavailable";
    }

    std::string AutoLightIndex(std::uint32_t index)
    {
        if(index == 0xFFFFFFFFu)
        {
            return "unavailable";
        }

        if(index == 0xFFFFFFFEu)
        {
            return "sun";
        }

        if(index == 0xFFFFFFFDu)
        {
            return "environment";
        }

        return std::to_string(index);
    }

    // Plain-text candidate block shared by the UI and report; every value is read back, none reconstructed by
    // division.
    std::string AutoCandidateText(unsigned rank, const PathTraceAutoCandidate& candidate)
    {
        const auto& record = candidate.record;
        std::string text = std::format("#{} pixel ({}, {}) run {} frame {} sample {} old N {} batch {} depth {}"
            " target {} seed {}\n", rank, candidate.x, candidate.y, candidate.run, record.meta[0], record.meta[1],
            record.meta[2], record.meta[3], record.config[0], record.config[1], record.config[2]);
        text += std::format("  flags: SHARC substitution {}, raw sample rejected {}, bucket maximum {}\n",
            record.config[3] & 1u ? "yes" : "no", record.config[3] & 2u ? "yes" : "no",
            candidate.bucketMaximum ? "yes" : "no (stronger same-bucket event lost the write lock)");
        text += "  raw sample RGB " + AutoRGB(record.sample) + "\n";
        text += "  indirect RGB " + AutoRGB(record.indirect) + "\n";

        if(candidate.meanAvailable)
        {
            text += std::format("  progressive mean after batch (N {}) RGB {}\n", record.meta[2] + record.meta[3],
                AutoRGB(record.mean));
        }
        else
        {
            text += "  progressive mean: unavailable (mean write lock contended or bucket reassigned)\n";
        }

        text += "  primary RGB " + AutoRGB(record.primary) + "\n";
        text += "  secondary direct RGB " + AutoRGB(record.secondary) + "\n";
        text += "  deeper direct RGB " + AutoRGB(record.deeper) + "\n";
        text += "  indirect miss RGB " + AutoRGB(record.miss) + "\n";
        text += "  SHARC substitution RGB " + AutoRGB(record.cache) + "\n";
        const std::string hitDistance = record.hit[0] < 0.0f ? std::string("unavailable (no secondary hit)") :
            std::format("{:.7g}", record.hit[0]);
        text += "  first secondary hit distance " + hitDistance + "\n";

        if(record.info[1] == 0xFFFFFFFFu)
        {
            text += "  dominant secondary light: unavailable (no visible secondary/deeper light sample)\n";
            return text;
        }

        const float throughputY = AutoLuminance(record.throughput[0], record.throughput[1], record.throughput[2]);
        const float responseY = AutoLuminance(record.factors[0], record.factors[1], record.factors[2]);
        const float incidentY = AutoLuminance(record.incidence[0], record.incidence[1], record.incidence[2]);
        text += std::format("  dominant light: vertex {} type {} index {} light distance {:.7g}\n", record.info[0],
            AutoLightType(record.info[2]), AutoLightIndex(record.info[1]), record.incidence[3]);
        text += "  dominant contribution RGB " + AutoRGB(record.dominant) + "\n";
        text += "    diffuse part RGB " + AutoRGB(record.diffuse) + "\n";
        text += "    specular remainder RGB " + AutoRGB(record.specular) + "\n";
        text += std::format("  incoming throughput RGB ({:.7g}, {:.7g}, {:.7g}) Y {:.7g}, incoming BSDF PDF {:.7g}\n",
            record.throughput[0], record.throughput[1], record.throughput[2], throughputY, record.throughput[3]);
        text += std::format("  incident Li RGB ({:.7g}, {:.7g}, {:.7g}) Y {:.7g}\n", record.incidence[0],
            record.incidence[1], record.incidence[2], incidentY);
        text += std::format("  secondary f*cos*Li RGB ({:.7g}, {:.7g}, {:.7g}) Y {:.7g}, cosine {:.7g}\n",
            record.factors[0], record.factors[1], record.factors[2], responseY, record.factors[3]);
        text += std::format("  light PDF {:.7g}, MIS/pdf weight {:.7g}\n", record.hit[2], record.hit[3]);
        const std::string material = record.info[3] == 0xFFFFFFFFu ? std::string("unavailable") :
            std::to_string(record.info[3]);
        text += std::format("  secondary material index {}, roughness {:.7g}\n", material, record.hit[1]);

        // Forward product only: the shader recorded dominant as throughput * (f*cos*Li * weight).
        const float weight = record.hit[3];
        const float product[3] = {record.throughput[0] * record.factors[0] * weight,
            record.throughput[1] * record.factors[1] * weight, record.throughput[2] * record.factors[2] * weight};
        const std::string share = record.indirect[3] > 0.0f ?
            std::format("{:.4g}", record.dominant[3] / record.indirect[3]) : std::string("unavailable");
        text += std::format("  check: throughput x f*cos*Li x weight = ({:.7g}, {:.7g}, {:.7g}) Y {:.7g}"
            " vs dominant Y {:.7g}; dominant share of indirect {}\n", product[0], product[1], product[2],
            AutoLuminance(product[0], product[1], product[2]), record.dominant[3], share);
        return text;
    }
}

std::string DebugUI::BuildPathTraceAutoCaptureReport() const
{
    const auto& status = m_sampleDiagnosticStatus;
    const auto& automatic = status.automatic;
    const auto settings = GetPathTraceReferenceSettings();
    constexpr const char* denoisers[] = {"Off", "Engine", "NRD REBLUR", "NRD RELAX", "NRD hybrid"};
    constexpr const char* lightingModes[] = {"Legacy", "Path Traced Reference", "Path Traced Raster Primary",
        "Path Traced Raster Primary ReSTIR DI"};
    const int denoiser = static_cast<int>(GetPathTraceDenoiserMode());
    const char* denoiserName = denoiser >= 0 && denoiser < 5 ? denoisers[denoiser] : "unknown";

    std::string text = "Engine diagnostic R.9I.1D automatic specular firefly capture (report v1)\n";
    text += "Candidates are large raw indirect path samples (firefly candidates), not confirmed visible white pixels.\n";
    text += "Values are scene-linear, before denoisers, exposure, tone mapping and DLSS.\n\n";
    text += std::format("Capture: automatic, strongest indirect luminance per hashed bucket ({} buckets) per frame,"
        " merged into top {}; detection threshold {:.7g} (filter only)\n", PathTraceAutoBucketCount,
        PathTraceAutoCapture::CandidateCapacity, automatic.threshold);
    text += std::format("State: {}\n", automatic.state);
    text += std::format("Render target {} x {}; run/reset {}; current accumulated N {}\n", status.width, status.height,
        automatic.run, status.currentSamples);
    text += std::format("Lighting mode {}; PT samples/frame {}, max depth {}, roulette from depth {}, target {};"
        " denoiser {}; render scale index {} (DLSS {}); SHARC enabled {}\n",
        lightingModes[static_cast<int>(GetLightingMode())], settings.samplesPerFrame, settings.maxPathDepth,
        settings.russianRouletteDepth, settings.maxAccumulatedSamples, denoiserName, GetPathTraceRenderScaleIndex(),
        IsPathTraceDlssSelected() ? "yes" : "no", m_sharcEnabled ? "yes" : "no");
    text += "Scene light count, environment and sun state: unavailable in this report.\n";
    text += std::format("Frames observed {}; frames not observed (readback busy/allocation) {}\n",
        automatic.framesCaptured, automatic.framesSkipped);
    text += std::format("Threshold-crossing samples {}; bucket winners read back {}; retained {}; not retained"
        " (outranked/displaced) {}\n", automatic.crossings, automatic.bucketEvents, automatic.candidates.size(),
        automatic.notRetained);
    text += std::format("Bucket maxima lost to write-lock contention {}; record lock failures {}; mean lock failures"
        " {}; mean unavailable {}; invalid records {}\n", automatic.maximaLost, automatic.lockFailures,
        automatic.meanLockFailures, automatic.meanUnavailable, automatic.invalid);
    text += "Coverage: only each bucket's strongest sample per frame can be retained; the list is not exhaustive.\n";
    text += "Light types: point, spot, directional, sun, environment. Raw BSDF f and per-bounce weights are not"
        " captured.\n\n";

    if(automatic.candidates.empty())
    {
        text += "No candidates retained.\n";
    }

    for(unsigned i = 0; i < automatic.candidates.size(); ++i)
    {
        text += AutoCandidateText(i + 1, automatic.candidates[i]);
    }

    return text;
}

void DebugUI::DrawPathTraceAutoCapture()
{
    auto& settings = m_sampleDiagnosticSettings;
    ImGui::SeparatorText("Automatic firefly capture (R.9I.1D)");
    ImGui::Checkbox("Enable automatic capture", &settings.autoEnabled);
    ImGui::InputFloat("Detection luminance threshold", &settings.autoThreshold);

    if(!std::isfinite(settings.autoThreshold) || settings.autoThreshold < 0.0f)
    {
        settings.autoThreshold = 0.0f;
    }

    if(ImGui::Button("Clear automatic records"))
    {
        ++settings.autoClearRequest;
        m_sampleDiagnosticStatus.automatic.candidates.clear();
    }

    ImGui::SameLine();

    // ImGui's platform clipboard; no new dependency.
    if(ImGui::Button("Copy Diagnostic Report"))
    {
        ImGui::SetClipboardText(BuildPathTraceAutoCaptureReport().c_str());
    }

    const auto& automatic = m_sampleDiagnosticStatus.automatic;
    ImGui::Text("%s", automatic.state.c_str());
    ImGui::Text("Run %llu; frames observed %llu, not observed %llu; retained %zu of %u",
        static_cast<unsigned long long>(automatic.run), static_cast<unsigned long long>(automatic.framesCaptured),
        static_cast<unsigned long long>(automatic.framesSkipped), automatic.candidates.size(),
        PathTraceAutoCapture::CandidateCapacity);
    ImGui::Text("Crossings %llu; bucket winners %llu; not retained %llu; maxima lost %llu; mean unavailable %llu;"
        " invalid %llu", static_cast<unsigned long long>(automatic.crossings),
        static_cast<unsigned long long>(automatic.bucketEvents), static_cast<unsigned long long>(automatic.notRetained),
        static_cast<unsigned long long>(automatic.maximaLost), static_cast<unsigned long long>(automatic.meanUnavailable),
        static_cast<unsigned long long>(automatic.invalid));
    ImGui::TextWrapped("Detection only: the threshold never changes radiance. Candidates are large raw indirect samples, not confirmed visible white pixels. Restart accumulation after enabling.");

    if(automatic.candidates.empty() || !ImGui::TreeNode("Strongest candidates"))
    {
        return;
    }

    ImGui::BeginChild("auto-candidates", ImVec2(0, 260), true);

    for(unsigned i = 0; i < automatic.candidates.size(); ++i)
    {
        const auto& candidate = automatic.candidates[i];
        const auto& record = candidate.record;
        const float throughputY = AutoLuminance(record.throughput[0], record.throughput[1], record.throughput[2]);
        const float responseY = AutoLuminance(record.factors[0], record.factors[1], record.factors[2]);
        const void* id = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(i + 1));

        if(ImGui::TreeNode(id, "#%u (%u,%u) indirect Y %.7g, throughput Y %.4g, f*cos*Li Y %.4g", i + 1, candidate.x,
            candidate.y, record.indirect[3], throughputY, responseY))
        {
            ImGui::TextUnformatted(AutoCandidateText(i + 1, candidate).c_str());
            ImGui::TreePop();
        }
    }

    ImGui::EndChild();
    ImGui::TreePop();
}
