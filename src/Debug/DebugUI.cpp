#include "DebugUI.h"

#include "Renderer/DescriptorHeap.h"

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

    return true;
}

void DebugUI::Shutdown()
{
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

                const uint32_t mask = 1u << index;

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
            const uint32_t reblurMask = (1u << specularIndex) | (1u << diffuseIndex);

            if ((gpu.validRegions & reblurMask) == reblurMask)
            {
                ImGui::Text("  NRD REBLUR: %.3f ms", gpu.milliseconds[specularIndex] + gpu.milliseconds[diffuseIndex]);
            }
            else
            {
                ImGui::TextUnformatted("  NRD REBLUR: -- (inactive / unavailable)");
            }

            drawGpuTiming("  NRD reflection resolve", GpuTimestampProfiler::Region::NrdResolve);

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
                                   "GI Signed Reconstructed (Pre-NRD)",
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
                ImGui::TextWrapped("Signed reconstructed irradiance before NRD. Requires active signed GI; otherwise displays normal rendering.");
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
