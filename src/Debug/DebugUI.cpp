#include "DebugUI.h"
#include "LightingModeTransitionPresentation.h"

#include "Engine/RenderingConfigurationResolver.h"
#include "Renderer/DescriptorHeap.h"
#include "Platform/Windows/WindowsWindow.h"

#include <cmath>
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

DebugUI::GpuTimingConfiguration DebugUI::CurrentGpuTimingConfiguration() const
{
    GpuTimingConfiguration configuration;
    configuration.epoch = m_visibilityTimingEpoch;
    configuration.lightingMode = static_cast<int>(GetEffectiveRenderingSettings().lightingMode);
    configuration.activeLightingMode = m_lightingTransition.active;
    configuration.renderScale = GetPathTraceRenderScaleIndex();
    configuration.renderWidth = m_gpuTimingRenderWidth;
    configuration.renderHeight = m_gpuTimingRenderHeight;
    configuration.outputWidth = m_gpuTimingOutputWidth;
    configuration.outputHeight = m_gpuTimingOutputHeight;
    configuration.dlssActive = m_gpuTimingDlssActive;
    configuration.dlssInputPreparation = m_dlssInputPreparation;
    configuration.cameraJitter = GetRenderingConfiguration().cameraJitter;
    configuration.settings = GetPathTraceReferenceSettings();
    configuration.pathTraceActive = m_pathTraceActiveLastFrame;
    configuration.secondaryShadowActive = m_secondaryShadowActive;
    configuration.primaryDiShadowActive = m_primaryDiShadowActive;
    configuration.denoiser = GetPathTraceDenoiserMode();
    configuration.denoiserPreparing = !m_pathTraceDenoiserPreparation.empty();
    configuration.temporalHistory = m_pathTraceDenoiserStages.temporalHistory;
    configuration.spatialFilter = m_pathTraceDenoiserStages.spatialFilter;
    configuration.extendedDiffuseHistory = m_pathTraceDenoiserStages.extendedDiffuseHistory;
    configuration.sharc = GetSharcSettings();
    configuration.sharcActive = m_sharcStatus.active;
    return configuration;
}

void DebugUI::DrawPerformanceWindow(
    const std::vector<std::string>& environmentNames,
    size_t currentEnvironmentIndex,
    const RendererStatistics& statistics)
{
    ImGuiIO& io = ImGui::GetIO();
    const GpuTimestampProfiler::Results& gpu = statistics.gpuTimings;

    // D1: a different pass graph starts fresh statistics. Frames up to the newest completed one plus every slot still
    // in flight were recorded before this request reaches the renderer, so they are skipped as well.
    const GpuTimingConfiguration gpuTimingConfiguration = CurrentGpuTimingConfiguration();

    if(!m_gpuTimingConfigurationKnown || !(gpuTimingConfiguration == m_gpuTimingConfiguration))
    {
        m_gpuTimingMonitor.Invalidate(gpu.frameSerial + GpuTimestampProfiler::FrameCount + 1);
        m_referenceRayCounts = {};
        m_gpuTimingConfiguration = gpuTimingConfiguration;
        m_gpuTimingConfigurationKnown = true;
    }

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
        m_pathTraceRenderScale != m_visibilityTimingScale ||
        static_cast<int>(GetEffectiveRenderingSettings().lightingMode) != m_visibilityTimingLighting ||
        !m_pathTraceActiveLastFrame)
    {
        m_visibilityTimings.Clear();
        m_sampledVisibilityEpoch = m_visibilityTimingEpoch;
        m_visibilityTimingSettings = visibilitySettings;
        m_visibilityTimingSharc = GetSharcSettings();
        m_visibilityTimingDenoiser = GetPathTraceDenoiserMode();
        m_visibilityTimingStages = m_pathTraceDenoiserStages;
        m_visibilityTimingScale = m_pathTraceRenderScale;
        m_visibilityTimingLighting = static_cast<int>(GetEffectiveRenderingSettings().lightingMode);
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

            ImGui::SeparatorText("Ray Counts (Reference)");
            if(ImGui::Checkbox("Enable ray counts", &m_referenceRayCountsEnabled)) m_referenceRayCounts = {};
            if(!m_referenceRayCountsEnabled) ImGui::TextUnformatted("Disabled");
            else if(!m_referenceRayCounts.valid)
                ImGui::TextUnformatted(m_referenceRayCounts.pending ? "Pending GPU completion" : "Unavailable (no counted Reference dispatch)");
            else
            {
                const auto& counts = m_referenceRayCounts;
                ImGui::Text("Camera: %llu | Continuation: %llu", (unsigned long long)counts.camera,
                    (unsigned long long)counts.continuation);
                ImGui::Text("Shadow: %llu | Total: %llu", (unsigned long long)counts.shadow,
                    (unsigned long long)(counts.camera + counts.continuation + counts.shadow));
                ImGui::Text("Dispatch: %u x %u | spp: %u | max depth: %u", counts.width, counts.height, counts.spp, counts.depth);
                ImGui::Text("Completed frame: %llu", (unsigned long long)counts.frame);
            }


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

            const RenderingSettings effective = GetEffectiveRenderingSettings();
            RenderingOverrides& overrides = m_renderingOverrides;

            OverrideCombo("Reflection denoiser", effective.legacy.reflectionDenoiser, denoisers,
                IM_ARRAYSIZE(denoisers), overrides.reflectionDenoiser, RenderingOverrideField::reflectionDenoiser);
            OverrideCheckbox("NRD contribution limiter", effective.legacy.contributionLimiter,
                overrides.contributionLimiter, RenderingOverrideField::contributionLimiter);
            OverrideCheckbox("Enable RTGI", effective.legacy.rtgi, overrides.rtgi, RenderingOverrideField::rtgi);
            OverrideSliderFloat("RTGI Contact AO Strength", effective.legacy.rtgiContactAOStrength,
                LegacyRenderingSettings::MinimumContactAOStrength, LegacyRenderingSettings::MaximumContactAOStrength,
                "%.2f", 0, overrides.rtgiContactAOStrength, RenderingOverrideField::rtgiContactAOStrength);
            OverrideSliderFloat("RTGI Contact AO Radius", effective.legacy.rtgiContactAORadius,
                LegacyRenderingSettings::MinimumContactAORadius, LegacyRenderingSettings::MaximumContactAORadius,
                "%.2f", 0, overrides.rtgiContactAORadius, RenderingOverrideField::rtgiContactAORadius);
            ImGui::Combo("Diagnostic view", &m_toneMapDebugView, views, IM_ARRAYSIZE(views));

            // RTXPT D.3B.0: path-trace denoiser views. They need the diagnostic extraction and temporal passes
            // (Renderer flags); otherwise the presentation is unchanged.
            {
                const char* denoiserViews[] = {"Normal", "Temporal (demodulated)", "Spatial (demodulated)",
                    "Spatial - temporal difference", "Estimated uncertainty", "History length", "Raw extraction"};
                const char* signals[] = {"Diffuse lighting", "Specular"};
                using Limits = PathTracingRenderingSettings;
                const PathTracingRenderingSettings& pathTracing = effective.pathTracing;
                const int denoiserView = static_cast<int>(OverrideCombo("PT denoiser view", pathTracing.denoiserView,
                    denoiserViews, IM_ARRAYSIZE(denoiserViews), overrides.denoiserView,
                    RenderingOverrideField::denoiserView));
                OverrideCombo("PT denoiser signal", pathTracing.denoiserSignal, signals, IM_ARRAYSIZE(signals),
                    overrides.denoiserSignal, RenderingOverrideField::denoiserSignal);
                OverrideSliderFloat("PT view exposure (stops)", pathTracing.viewExposureStops,
                    Limits::MinimumViewExposureStops, Limits::MaximumViewExposureStops, "%.1f", 0,
                    overrides.viewExposureStops, RenderingOverrideField::viewExposureStops);
                OverrideSliderFloat("PT difference amplification", pathTracing.differenceAmplification,
                    Limits::MinimumDifferenceAmplification, Limits::MaximumDifferenceAmplification, "%.0f",
                    ImGuiSliderFlags_Logarithmic, overrides.differenceAmplification,
                    RenderingOverrideField::differenceAmplification);
                OverrideSliderFloat("PT uncertainty floor (log10)", pathTracing.uncertaintyFloorLog10,
                    Limits::MinimumUncertaintyFloorLog10, Limits::MaximumUncertaintyFloorLog10, "%.1f", 0,
                    overrides.uncertaintyFloorLog10, RenderingOverrideField::uncertaintyFloorLog10);

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

namespace
{
    constexpr std::uint32_t SharcTrainingTiles[] = {5, 8};
    constexpr std::uint32_t SharcMinimumSamples[] = {8, 16, 32, 64, 256};
}

RenderingSettings DebugUI::GetEffectiveRenderingSettings() const
{
    return ApplyRenderingOverrides(m_savedRendering, m_renderingOverrides);
}

void DebugUI::OnProjectSettingsSaved(const RenderingSettings& previousSaved, const RenderingSettings& saved)
{
    ClearConflictingOverrides(m_renderingOverrides, previousSaved, saved);
    m_savedRendering = saved;
}

RenderingDiagnostics DebugUI::GetRenderingDiagnostics() const
{
    RenderingDiagnostics diagnostics;
    diagnostics.diagnosticRenderScale = m_pathTraceRenderScale;
    diagnostics.dlssInputPreparation = m_dlssInputPreparation;
    diagnostics.denoiserStages = m_pathTraceDenoiserStages;

    SharcSettings& sharc = diagnostics.sharcTuning;
    sharc.trainingTile = SharcTrainingTiles[m_sharcDensity];
    sharc.minimumSamples = SharcMinimumSamples[m_sharcMinimumSamples];
    sharc.freezeTraining = m_sharcFreezeTraining;
    sharc.statistics = m_sharcStatistics;
    sharc.resetRequest = m_sharcResetRequest;
    sharc.adaptiveResolution = m_sharcSpatialResolution == 1;
    sharc.historyPrior = m_sharcHistoryPrior;

    diagnostics.pixelCausticView = m_pixelCausticView;
    diagnostics.pixelCausticInsertion = m_pixelCausticInsertion;
    ApplyReferenceOverrides(m_renderingOverrides, diagnostics);

    return diagnostics;
}

RenderingConfiguration DebugUI::GetRenderingConfiguration() const
{
    return ResolveRenderingConfiguration(GetEffectiveRenderingSettings(), GetRenderingDiagnostics());
}

void DebugUI::DrawOverrideMarker(RenderingOverrideField field)
{
    if(!IsOverridden(m_renderingOverrides, field))
    {
        return;
    }

    ImGui::SameLine();
    ImGui::PushID(static_cast<int>(field));

    if(ImGui::SmallButton("Overridden  x"))
    {
        ClearOverride(m_renderingOverrides, field);
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Session override; not saved. Click to use the project setting.");
    }

    ImGui::PopID();
}

bool DebugUI::OverrideCheckbox(const char* label, bool value, std::optional<bool>& slot, RenderingOverrideField field)
{
    if(ImGui::Checkbox(label, &value))
    {
        slot = value;
    }

    DrawOverrideMarker(field);
    return value;
}

float DebugUI::OverrideSliderFloat(const char* label, float value, float minimum, float maximum, const char* format,
    int flags, std::optional<float>& slot, RenderingOverrideField field)
{
    if(ImGui::SliderFloat(label, &value, minimum, maximum, format, flags))
    {
        slot = value;
    }

    DrawOverrideMarker(field);
    return value;
}

std::uint32_t DebugUI::OverrideSliderUInt(const char* label, std::uint32_t value, int minimum, int maximum, int flags,
    std::optional<std::uint32_t>& slot, RenderingOverrideField field)
{
    int edited = static_cast<int>(value);

    if(ImGui::SliderInt(label, &edited, minimum, maximum, "%d", flags))
    {
        value = static_cast<std::uint32_t>((std::max)(edited, minimum));
        slot = value;
    }

    DrawOverrideMarker(field);
    return value;
}

// The combo index is the enum value: every project enum lists its choices in display order.
template<typename Enum>
Enum DebugUI::OverrideCombo(const char* label, Enum value, const char* const* items, int count,
    std::optional<Enum>& slot, RenderingOverrideField field)
{
    int selection = static_cast<int>(value);

    if(ImGui::Combo(label, &selection, items, count))
    {
        value = static_cast<Enum>(selection);
        slot = value;
    }

    DrawOverrideMarker(field);
    return value;
}

void DebugUI::DrawSharcSettings()
{
    ImGui::TextUnformatted("SHARC radiance cache (experimental; tuning is not saved)");
    const bool sharcEnabled = OverrideCheckbox("Enable SHARC", GetEffectiveRenderingSettings().pathTracing.sharc,
        m_renderingOverrides.sharc, RenderingOverrideField::sharc);

    if (!sharcEnabled)
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
    ImGui::BeginDisabled(GetSelectedPathTraceDenoiserMode() != PathTraceDenoiserMode::Engine);
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
    RenderingOverrides& overrides = m_renderingOverrides;
    const RenderingSettings effective = GetEffectiveRenderingSettings();
    const RenderingConfiguration resolved = GetRenderingConfiguration();
    const PathTracingRenderingSettings& pathTracing = effective.pathTracing;

    // PS.2C: the project's saved rendering settings; edits here are session overrides and are never saved.
    ImGui::TextUnformatted("Rendering (project settings; edits are session overrides)");
    const std::size_t overrideCount = CountOverrides(overrides);

    if(overrideCount == 0)
    {
        ImGui::TextDisabled("Showing the project settings; no session overrides.");
    }
    else
    {
        ImGui::Text("%zu session override%s active.", overrideCount, overrideCount == 1 ? "" : "s");
        ImGui::SameLine();

        if(ImGui::SmallButton("Restore project settings"))
        {
            ClearRenderingOverrides();
        }
    }

    const char* modes[] = {"Legacy", "Reference Path Tracer", "Path Tracing"};
    // While the Renderer prepares the requested mode, the selector shows it and accepts no other request.
    const bool preparing = IsLightingModePreparing(m_lightingTransition.stage);
    ImGui::BeginDisabled(preparing);
    const ProjectLightingMode lightingMode = OverrideCombo("Lighting mode", effective.lightingMode, modes,
        IM_ARRAYSIZE(modes), overrides.lightingMode, RenderingOverrideField::lightingMode);
    ImGui::EndDisabled();

    // Final display mapping only, in every lighting mode; switching keeps the HDR image and accumulation.
    const char* toneMappers[] = {"AgX", "GT7"};
    OverrideCombo("Tone mapper", effective.toneMapper, toneMappers, IM_ARRAYSIZE(toneMappers), overrides.toneMapper,
        RenderingOverrideField::toneMapper);

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

    if(lightingMode == ProjectLightingMode::Legacy)
    {
        ImGui::Separator();
        return;
    }

    const bool isPathTracing = lightingMode == ProjectLightingMode::PathTracing;
    const bool reference = lightingMode == ProjectLightingMode::ReferencePathTracer;

    const char* visibilityModes[] = {"TraceRay (Original)", "RayQuery (GI.6)"};
    const char* primaryDiVisibilityModes[] = {"TraceRay (Original)", "RayQuery (DI.1)"};
    ImGui::Text("Active primary DI shadows: %s",
        m_lightingTransition.active == RendererLightingMode::PathTracedRasterPrimaryReSTIRDI ?
        primaryDiVisibilityModes[m_primaryDiShadowActive ? 1 : 0] : "Not in use");

    if(isPathTracing && m_primaryDiShadowSerWorkaround)
    {
        ImGui::TextDisabled("SER compatibility: DI.1 RayQuery visibility is wrong with SM 6.9 + SER; TraceRay used.");
    }
    else if(isPathTracing && m_primaryDiShadowFailed)
    {
        ImGui::TextDisabled("DI.1 pipelines failed (see the DI.1 log warning); original retained.");
    }
    else if(isPathTracing && !m_primaryDiShadowReady)
    {
        ImGui::TextDisabled("DI.1 pipelines still preparing; original retained.");
    }

    ImGui::Text("Active secondary shadows: %s", visibilityModes[m_secondaryShadowActive ? 1 : 0]);

    if(!m_secondaryShadowReady)
    {
        ImGui::TextDisabled("GI.6 unavailable or pipelines still preparing; original retained.");
    }

    // The mode's own quality: Path Tracing's settings, or Reference's fixed quality with Debug-only Reference
    // overrides. Ranges match IsValidPathTraceReferenceSettings; every change restarts accumulation.
    using Limits = PathTracingRenderingSettings;
    const PathTraceReferenceSettings& quality = resolved.pathTrace;

    if(reference)
    {
        ImGui::TextDisabled("Reference uses fixed quality; changes are Debug-only overrides.");
        OverrideSliderUInt("PT samples per frame", quality.samplesPerFrame, 1, static_cast<int>(Limits::MaximumSamplesPerFrame),
            0, overrides.referenceSamplesPerFrame, RenderingOverrideField::referenceSamplesPerFrame);
        OverrideSliderUInt("PT max path depth", quality.maxPathDepth, 1, static_cast<int>(Limits::MaximumPathDepth), 0,
            overrides.referenceMaxPathDepth, RenderingOverrideField::referenceMaxPathDepth);
        OverrideSliderUInt("PT roulette from depth", quality.russianRouletteDepth, 1,
            static_cast<int>(Limits::MaximumRouletteDepth), 0, overrides.referenceRouletteFromDepth,
            RenderingOverrideField::referenceRouletteFromDepth);
        OverrideSliderUInt("PT sample target", quality.maxAccumulatedSamples, 1, 1 << 16, ImGuiSliderFlags_Logarithmic,
            overrides.referenceSampleTarget, RenderingOverrideField::referenceSampleTarget);
    }
    else
    {
        OverrideSliderUInt("PT samples per frame", quality.samplesPerFrame, 1, static_cast<int>(Limits::MaximumSamplesPerFrame),
            0, overrides.samplesPerFrame, RenderingOverrideField::samplesPerFrame);
        OverrideSliderUInt("PT max path depth", quality.maxPathDepth, 1, static_cast<int>(Limits::MaximumPathDepth), 0,
            overrides.maxPathDepth, RenderingOverrideField::maxPathDepth);
        OverrideSliderUInt("PT roulette from depth", quality.russianRouletteDepth, 1,
            static_cast<int>(Limits::MaximumRouletteDepth), 0, overrides.rouletteFromDepth,
            RenderingOverrideField::rouletteFromDepth);
        OverrideSliderUInt("PT sample target", quality.maxAccumulatedSamples, 1, 1 << 16, ImGuiSliderFlags_Logarithmic,
            overrides.sampleTarget, RenderingOverrideField::sampleTarget);
    }

    // PSR.2: primary surface replacement through pure-delta mirrors, single-bounce (A -> B) or two-bounce
    // (A -> B -> D). The PSR.1 reference settings (primarySurfaceReplacement, its guides) are set only by the verification
    // harnesses.
    const char* psrModes[] = {"Off", "Single-bounce", "Two-bounce"};
    const ProjectPsrMode psrMode = OverrideCombo("PT PSR mode", pathTracing.psrMode, psrModes, IM_ARRAYSIZE(psrModes),
        overrides.psrMode, RenderingOverrideField::psrMode);

    ImGui::Text("Active PSR: %s", psrModes[m_psrActiveMode >= 2u ? 2 : m_psrActiveMode]);
    if(psrMode != ProjectPsrMode::Off)
    {
        if(psrMode == ProjectPsrMode::TwoBounce && pathTracing.maxPathDepth < 3)
        {
            ImGui::TextWrapped("Two-bounce needs PT max path depth 3 or more; replacing single-bounce.");
        }

        if(!m_psrLightingRequested)
        {
            ImGui::TextWrapped("Inactive: select the Path Tracing (ReSTIR DI) lighting mode.");
        }
        else if(!m_psrLightingUnavailable.empty())
        {
            ImGui::TextWrapped("PSR status: %s.", m_psrLightingUnavailable.c_str());
        }
        else
        {
            ImGui::TextDisabled("Active: reflected surfaces are lit.");
        }
        const char* psrDebugViews[] = {"Off", "Replacement Mask", "Reflected Normals", "Reflected Albedo"};
        const ProjectPsrDebugView psrDebugView = OverrideCombo("PSR Debug View", pathTracing.psrDebugView,
            psrDebugViews, IM_ARRAYSIZE(psrDebugViews), overrides.psrDebugView, RenderingOverrideField::psrDebugView);

        if(psrDebugView == ProjectPsrDebugView::ReplacementMask)
        {
            ImGui::TextDisabled("Green replaced, red fallback, yellow pass did not run, magenta invalid.");
        }
    }

    ImGui::SeparatorText("DLSS Super Resolution");
    const bool dlssEnabled = OverrideCheckbox("Enable DLSS Super Resolution", pathTracing.dlss, overrides.dlss,
        RenderingOverrideField::dlss);
    const char* dlssQualities[] = {"Quality", "Balanced", "Performance"};
    ImGui::BeginDisabled(!dlssEnabled);
    OverrideCombo("DLSS quality", pathTracing.dlssQuality, dlssQualities, IM_ARRAYSIZE(dlssQualities),
        overrides.dlssQuality, RenderingOverrideField::dlssQuality);
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", m_dlssState.c_str());
    ImGui::TextWrapped("%s", m_dlssDetail.c_str());

    if(dlssEnabled && !IsDlssConfigurationEligible())
    {
        ImGui::TextWrapped("Requires an active raster-primary path-tracing mode and a ready denoiser.");
    }

    if(dlssEnabled && m_dlssCanRetry && ImGui::Button("Retry DLSS"))
    {
        m_dlssRetry = true;
    }

    if(ImGui::TreeNode("Advanced upscaling diagnostics"))
    {
        const char* diagnosticScales[] = {"Native", "75% bilinear", "75% DLSS"};
        ImGui::BeginDisabled(dlssEnabled);
        ImGui::Combo("Diagnostic render scale", &m_pathTraceRenderScale, diagnosticScales, IM_ARRAYSIZE(diagnosticScales));
        ImGui::EndDisabled();
        ImGui::Checkbox("DLSS input preparation only", &m_dlssInputPreparation);

        if(!dlssEnabled && m_dlssCanRetry && ImGui::Button("Retry diagnostic upscaling"))
        {
            m_dlssRetry = true;
        }

        ImGui::TreePop();
    }

    // SHARC.4E.1 / 5B: the denoiser and SHARC serve Path Tracing.
    if (isPathTracing)
    {
        // UPSCALING.2B (experimental, off by default): diagnostics for future temporal upscaling.
        OverrideCheckbox("PT camera jitter (experimental)", pathTracing.cameraJitter, overrides.cameraJitter,
            RenderingOverrideField::cameraJitter);

        if (m_pathTraceCameraJitterActive)
        {
            ImGui::Text("Jitter phase %u/%u: (%+.3f, %+.3f) px", m_pathTraceCameraJitterPhase,
                        m_pathTraceCameraJitterPhaseCount, m_pathTraceCameraJitterX, m_pathTraceCameraJitterY);
        }

        // NRD.2: only the implemented modes are offered, in ProjectPathTraceDenoiser order. NRD.4B: REBLUR
        // (experimental); NRD.RELAX.4: RELAX (experimental); HYBRID.5: the hybrid (experimental, the default).
        const char* denoiserModes[] = {"Off", "Engine", "NRD REBLUR (experimental)", "NRD RELAX (experimental)",
                                       "NRD Hybrid (experimental)"};
        OverrideCombo("PT denoiser", pathTracing.denoiser, denoiserModes, IM_ARRAYSIZE(denoiserModes),
            overrides.denoiser, RenderingOverrideField::denoiser);

        if (!m_pathTraceDenoiserPreparation.empty())
        {
            ImGui::TextWrapped("%s", m_pathTraceDenoiserPreparation.c_str());
        }

        if (IsNrdPathTraceDenoiserMode(GetSelectedPathTraceDenoiserMode()))
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
        if (GetSelectedPathTraceDenoiserMode() == PathTraceDenoiserMode::Engine)
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
    else if (const PathTraceModeAvailability pathTracingStatus = GetReSTIRDIAvailability(m_rasterPrimaryStatus);
        isPathTracing && pathTracingStatus.availability == PathTraceRasterPrimaryAvailability::Unavailable)
    {
        ImGui::TextWrapped("Unavailable, rendering Legacy: %s", pathTracingStatus.reason.c_str());
    }
    else if (!isPathTracing && m_pathTraceAvailability == PathTraceReferenceAvailability::Unavailable)
    {
        ImGui::TextWrapped("Unavailable, rendering Legacy: %s", m_pathTraceUnavailableReason.c_str());
    }
    else if (m_pathTraceActiveLastFrame)
    {
        ImGui::Text("Accumulated %u / %u samples per pixel", m_pathTraceAccumulatedSamples,
                    quality.maxAccumulatedSamples);
        ImGui::Text("Accumulation memory: %.2f MiB",
                    static_cast<double>(m_pathTraceResourceBytes) / (1024.0 * 1024.0));
    }
    else
    {
        ImGui::TextUnformatted("Waiting for a scene with ray-traced geometry.");
    }

    // R.9I.3: added to the undenoised path-traced image, which keeps its own naturally sampled caustics. Restricted
    // domain (registered static reflector/receiver, one point light, PT denoiser Off); refusals render unchanged.
    // Path Tracing's caustics are project settings; Reference's are Debug-only overrides.
    {
        ImGui::SeparatorText("Dedicated Caustics");
        bool dedicatedCaustics = false;

        if(reference)
        {
            dedicatedCaustics = OverrideCheckbox("Dedicated caustics (experimental, Debug only)",
                overrides.referenceDedicatedCaustics.value_or(false), overrides.referenceDedicatedCaustics,
                RenderingOverrideField::referenceDedicatedCaustics);
            ImGui::BeginDisabled(!dedicatedCaustics);
            OverrideCheckbox("Caustic-only view", overrides.referenceCausticOnlyView.value_or(false),
                overrides.referenceCausticOnlyView, RenderingOverrideField::referenceCausticOnlyView);
            ImGui::EndDisabled();
        }
        else
        {
            const bool wasDedicated = pathTracing.dedicatedCaustics;
            dedicatedCaustics = OverrideCheckbox("Dedicated caustics (experimental)", wasDedicated,
                overrides.dedicatedCaustics, RenderingOverrideField::dedicatedCaustics);

            // The two caustics passes are exclusive; enabling one switches the other off.
            if(dedicatedCaustics && !wasDedicated && pathTracing.pixelCentredCaustics)
            {
                overrides.pixelCentredCaustics = false;
            }

            ImGui::BeginDisabled(!dedicatedCaustics);
            OverrideCheckbox("Caustic-only view", pathTracing.causticOnlyView, overrides.causticOnlyView,
                RenderingOverrideField::causticOnlyView);
            ImGui::EndDisabled();
        }

        // R.9I.3 production integration (P1: K = 1 estimator, caustics-only views): Path Tracing only.
        ImGui::BeginDisabled(!isPathTracing);
        const bool wasPixelCentred = pathTracing.pixelCentredCaustics;
        const bool pixelCentredCaustics = OverrideCheckbox("Pixel-centred caustics (experimental)", wasPixelCentred,
            overrides.pixelCentredCaustics, RenderingOverrideField::pixelCentredCaustics);

        if(pixelCentredCaustics && !wasPixelCentred && pathTracing.dedicatedCaustics)
        {
            overrides.dedicatedCaustics = false;
        }

        ImGui::EndDisabled();

        if (pixelCentredCaustics)
        {
            static const char* const views[] = {"Ordinary image", "Caustic diffuse", "Caustic specular",
                                                "Caustic combined", "Caustic validity"};
            int view = static_cast<int>(m_pixelCausticView);

            if (ImGui::Combo("Caustic view", &view, views, IM_ARRAYSIZE(views)))
            {
                m_pixelCausticView = static_cast<std::uint32_t>(view);
            }

            // P2: needs a PT denoiser (the extraction it adds into); maxPathDepth 1 avoids the known overlap.
            ImGui::Checkbox("Add to lighting before denoising (experimental)", &m_pixelCausticInsertion);

            ImGui::TextWrapped("Pixel-centred: %s", m_pixelCausticReason.c_str());
            ImGui::Text("Seams verified: %llu of %llu", static_cast<unsigned long long>(m_pixelCausticVerifiedSeams),
                        static_cast<unsigned long long>(m_pixelCausticSeamVisits));
            ImGui::Text("GPU-completed frames: %llu (faulted %llu, last flags 0x%X)",
                        static_cast<unsigned long long>(m_pixelCausticCompletedFrames),
                        static_cast<unsigned long long>(m_pixelCausticFaultedFrames), m_pixelCausticLastFaults);
            ImGui::Text("Caustic memory: %.2f MiB",
                        static_cast<double>(m_pixelCausticResourceBytes) / (1024.0 * 1024.0));
        }

        if (resolved.dedicatedCaustics)
        {
            ImGui::TextWrapped("Status: %s", m_causticReason.c_str());
            ImGui::Text("Presented last frame: %s", m_causticPresented ? "yes" : "no");
            ImGui::Text("Accumulated frames: %llu x 65536 attempts",
                        static_cast<unsigned long long>(m_causticHistoryFrames));
            ImGui::Text("Caustic memory: %.2f MiB", static_cast<double>(m_causticResourceBytes) / (1024.0 * 1024.0));
        }

        if (m_causticFaulted)
        {
            ImGui::TextWrapped("Numerical fault latched (flags 0x%X); contribution refused until acknowledged.",
                               m_causticFaultFlags);

            if (ImGui::Button("Acknowledge fault and reset caustics"))
            {
                m_causticAcknowledgeRequested = true;
            }
        }
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
