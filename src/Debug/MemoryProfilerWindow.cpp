#include "Debug/MemoryProfilerWindow.h"

#include "Debug/MemoryHistory.h"
#include "Debug/MemoryMonitor.h"
#include "Debug/MemoryRegistry.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

// Editor-only implementation; see MemoryProfilerWindow.h for the build boundary.
//
// Everything drawn comes from cached MemoryMonitor snapshots: the window never enumerates resources, queries DXGI or
// the OS, or waits on the GPU. Strings are formatted into stack buffers; history is fixed storage in the model.

namespace
{
    struct WindowState
    {
        bool visible = false;
        MemoryProfilerModel model;
    };

    WindowState& State()
    {
        // Intentionally leaked: the model is large and must not be destroyed during static teardown.
        static WindowState* state = new WindowState;

        return *state;
    }

    constexpr double MiB = 1024.0 * 1024.0;
    constexpr double GiB = 1024.0 * MiB;
    constexpr double HistorySeconds = 120.0;

    const ImVec4 WarningColor(1.0f, 0.75f, 0.25f, 1.0f);
    const ImVec4 UnavailableColor(0.6f, 0.6f, 0.6f, 1.0f);

    void FormatBytes(std::uint64_t bytes, char* text, std::size_t size)
    {
        if(bytes >= static_cast<std::uint64_t>(GiB))
        {
            std::snprintf(text, size, "%.2f GiB", bytes / GiB);
        }
        else if(bytes >= (1u << 20))
        {
            std::snprintf(text, size, "%.1f MiB", bytes / MiB);
        }
        else
        {
            std::snprintf(text, size, "%.1f KiB", bytes / 1024.0);
        }
    }

    // One table cell: a value, or a grey "unavailable" (never a zero standing in for an invalid measurement).
    void BytesCell(bool valid, std::uint64_t bytes)
    {
        if(!valid)
        {
            ImGui::TextColored(UnavailableColor, "unavailable");
            return;
        }

        char text[32];
        FormatBytes(bytes, text, sizeof(text));
        ImGui::TextUnformatted(text);
    }

    void LabelCell(const char* label)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(label);
    }

    void DrawControls(MemoryProfilerModel& model)
    {
        if(ImGui::Button(model.IsPaused() ? "Resume History" : "Pause History"))
        {
            model.SetPaused(!model.IsPaused());
        }

        ImGui::SameLine();

        if(ImGui::Button("Clear History"))
        {
            model.ClearHistory();
        }

        ImGui::SameLine();

        if(ImGui::Button("Reset Observed Peaks"))
        {
            model.ResetObservedPeaks();
        }
    }

    void DrawStatus(const MemoryProfilerModel& model)
    {
        if(!model.HasLatest())
        {
            ImGui::TextColored(WarningColor, "No memory sample yet.");
            return;
        }

        const MemoryMonitorSnapshot& latest = model.Latest();
        const double age = std::chrono::duration<double>(std::chrono::steady_clock::now() - latest.sampleTime).count();

        if(model.IsStale(std::chrono::steady_clock::now()))
        {
            ImGui::TextColored(WarningColor, "Stale: last sample %.1f s ago (sampling pauses while the Editor is minimized or not rendering).",
                age);
        }
        else
        {
            ImGui::Text("Sampling at %.0f Hz; last sample %.1f s ago (#%llu).", 1000.0 / MemoryMonitor::SampleInterval.count(), age,
                static_cast<unsigned long long>(latest.sequence));
        }

        ImGui::SameLine();
        ImGui::TextDisabled("| History %zu / %zu%s", model.Count(), MemoryProfilerModel::HistoryCapacity,
            model.IsPaused() ? " (paused)" : "");
    }

    void DrawOverview(const MemoryProfilerModel& model)
    {
        const MemoryMonitorSnapshot& latest = model.Latest();
        const MemoryObservedPeaks& peaks = model.ObservedPeaks();
        const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;

        ImGui::TextDisabled("CPU process (process-wide; not Engine-attributed)");

        if(ImGui::BeginTable("Process memory", 4, flags))
        {
            ImGui::TableSetupColumn("Measurement");
            ImGui::TableSetupColumn("Current");
            ImGui::TableSetupColumn("Process peak");
            ImGui::TableSetupColumn("Observed peak");
            ImGui::TableHeadersRow();

            const bool valid = latest.process.valid;
            LabelCell("Working set (resident)");
            ImGui::TableSetColumnIndex(1);
            BytesCell(valid, latest.process.workingSetBytes);
            ImGui::TableSetColumnIndex(2);
            BytesCell(valid, latest.process.peakWorkingSetBytes);
            ImGui::TableSetColumnIndex(3);
            BytesCell(peaks.workingSet.valid, peaks.workingSet.bytes);

            LabelCell("Private commit");
            ImGui::TableSetColumnIndex(1);
            BytesCell(valid, latest.process.privateBytes);
            ImGui::TableSetColumnIndex(2);
            BytesCell(valid, latest.process.peakPrivateBytes);
            ImGui::TableSetColumnIndex(3);
            BytesCell(peaks.privateBytes.valid, peaks.privateBytes.bytes);
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("GPU (DXGI reports this process's adapter usage; Engine-tracked counts only Engine-registered resources)");

        if(ImGui::BeginTable("GPU memory", 5, flags))
        {
            ImGui::TableSetupColumn("Measurement");
            ImGui::TableSetupColumn("Current");
            ImGui::TableSetupColumn("Budget / lifetime peak");
            ImGui::TableSetupColumn("Utilization");
            ImGui::TableSetupColumn("Observed peak");
            ImGui::TableHeadersRow();

            const auto segmentRow = [&](const char* label, const VideoMemorySegment& segment, const ObservedPeak& peak, bool reportsBudget)
            {
                const bool valid = latest.video.adapterValid && segment.valid;
                LabelCell(label);
                ImGui::TableSetColumnIndex(1);
                BytesCell(valid, segment.usageBytes);
                ImGui::TableSetColumnIndex(2);

                if(valid && !reportsBudget)
                {
                    ImGui::TextColored(UnavailableColor, "no budget reported");
                }
                else
                {
                    BytesCell(valid, segment.budgetBytes);
                }

                ImGui::TableSetColumnIndex(3);

                if(valid && reportsBudget)
                {
                    const float fraction = static_cast<float>(static_cast<double>(segment.usageBytes) / segment.budgetBytes);
                    char overlay[32];
                    std::snprintf(overlay, sizeof(overlay), "%.1f%%", fraction * 100.0f);
                    ImGui::ProgressBar((std::min)(fraction, 1.0f), ImVec2(-1.0f, 0.0f), overlay);
                }
                else
                {
                    ImGui::TextColored(UnavailableColor, "-");
                }

                ImGui::TableSetColumnIndex(4);
                BytesCell(peak.valid, peak.bytes);
            };

            segmentRow("DXGI local (video memory)", latest.video.local, peaks.localUsage, latest.video.local.budgetBytes > 0);
            segmentRow("DXGI non-local (shared system)", latest.video.nonLocal, peaks.nonLocalUsage, latest.video.nonLocal.budgetBytes > 0);

            const MemoryRegistrySnapshot& tracked = latest.gpuAllocations;
            LabelCell("Engine-tracked allocations");
            ImGui::TableSetColumnIndex(1);
            BytesCell(tracked.available, tracked.total.currentBytes);
            ImGui::TableSetColumnIndex(2);
            BytesCell(tracked.available, tracked.total.peakBytes);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%llu resources", static_cast<unsigned long long>(tracked.total.liveResources));
            ImGui::TableSetColumnIndex(4);
            BytesCell(peaks.tracked.valid, peaks.tracked.bytes);
            ImGui::EndTable();
        }

        if(!latest.video.adapterValid)
        {
            ImGui::TextColored(WarningColor, "DXGI video memory is unavailable (adapter not found or lost); CPU values remain valid.");
        }

        ImGui::TextDisabled("Tracked allocations exclude NRD, DLSS, swap chains and driver overhead, so they are not expected to equal DXGI usage.");
    }

    void DrawBreakdown(const MemoryProfilerModel& model)
    {
        const MemoryMonitorSnapshot& latest = model.Latest();
        const MemoryRegistrySnapshot& tracked = latest.gpuAllocations;
        const MemoryObservedPeaks& peaks = model.ObservedPeaks();
        const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;

        ImGui::TextDisabled("Engine-tracked GPU allocations by subsystem (allocation size)");

        if(ImGui::BeginTable("GPU categories", 6, flags))
        {
            ImGui::TableSetupColumn("Category");
            ImGui::TableSetupColumn("Current");
            ImGui::TableSetupColumn("Lifetime peak");
            ImGui::TableSetupColumn("Observed peak");
            ImGui::TableSetupColumn("Live resources");
            ImGui::TableSetupColumn("Share of tracked");
            ImGui::TableHeadersRow();

            for(std::size_t index = 0; index < MemoryCategoryCount; ++index)
            {
                const MemoryCategoryStatistics& category = tracked.categories[index];
                LabelCell(MemoryRegistry::CategoryName(static_cast<MemoryCategory>(index)));
                ImGui::TableSetColumnIndex(1);
                BytesCell(tracked.available, category.currentBytes);
                ImGui::TableSetColumnIndex(2);
                BytesCell(tracked.available, category.peakBytes);
                ImGui::TableSetColumnIndex(3);
                BytesCell(peaks.categories[index].valid, peaks.categories[index].bytes);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%llu", static_cast<unsigned long long>(category.liveResources));
                ImGui::TableSetColumnIndex(5);

                if(tracked.available && tracked.total.currentBytes > 0)
                {
                    ImGui::Text("%.1f%%", 100.0 * category.currentBytes / tracked.total.currentBytes);
                }
                else
                {
                    ImGui::TextColored(UnavailableColor, "-");
                }
            }

            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Engine-attributed CPU statistics (existing subsystem counters; partial coverage, so no total)");

        if(ImGui::BeginTable("CPU statistics", 3, flags))
        {
            ImGui::TableSetupColumn("Statistic");
            ImGui::TableSetupColumn("Current");
            ImGui::TableSetupColumn("Kind");
            ImGui::TableHeadersRow();

            for(std::size_t index = 0; index < CpuMemoryStatisticCount; ++index)
            {
                const auto statistic = static_cast<CpuMemoryStatistic>(index);
                LabelCell(MemoryMonitor::CpuStatisticName(statistic));
                ImGui::TableSetColumnIndex(1);
                BytesCell(latest.cpu[index].valid, latest.cpu[index].bytes);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled(MemoryMonitor::CpuStatisticKind(statistic) == CpuMemoryStatisticKind::Exact ? "exact (allocator)" :
                    "accounted (subsystem estimate)");
            }

            ImGui::EndTable();
        }
    }

    // A plotted series: a getter returning false for an invalid sample, and whether it is a reference line (a budget).
    struct Series
    {
        const char* name;
        ImU32 color;
        bool (*value)(const MemoryHistorySample&, double&);
        bool reference;
    };

    // Time runs left to right over the last HistorySeconds, ending at the newest sample. The vertical axis starts at
    // zero, so a constant or zero series stays a readable flat line. A line joins two samples only when both are valid
    // and they are no further apart than the gap threshold.
    void DrawGraph(const char* title, const MemoryProfilerModel& model, const Series* series, std::size_t seriesCount)
    {
        const std::size_t count = model.Count();
        double maximum = 0.0;

        for(std::size_t index = 0; index < count; ++index)
        {
            for(std::size_t line = 0; line < seriesCount; ++line)
            {
                double value = 0.0;

                if(series[line].value(model.Sample(index), value))
                {
                    maximum = (std::max)(maximum, value);
                }
            }
        }

        // At least 1 MiB of range, with headroom, so a flat or empty graph has a sensible scale.
        maximum = (std::max)(maximum * 1.1, MiB);

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = (std::max)(ImGui::GetContentRegionAvail().x, 240.0f);
        const float height = 170.0f;
        const ImVec2 topLeft(origin.x + 72.0f, origin.y + 20.0f);
        const ImVec2 bottomRight(origin.x + width - 12.0f, origin.y + height - 22.0f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 gridColor = ImGui::GetColorU32(ImGuiCol_Border);
        const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
        const bool useGiB = maximum >= 2.0 * GiB;
        const double unit = useGiB ? GiB : MiB;

        const auto valueY = [&](double value)
        {
            return bottomRight.y - static_cast<float>(value / maximum) * (bottomRight.y - topLeft.y);
        };

        char label[96];
        std::snprintf(label, sizeof(label), "%s (%s)", title, useGiB ? "GiB" : "MiB");
        draw->AddText(origin, textColor, label);

        for(int line = 0; line <= 4; ++line)
        {
            const double value = maximum * line / 4.0;
            const float y = valueY(value);
            draw->AddLine(ImVec2(topLeft.x, y), ImVec2(bottomRight.x, y), gridColor);
            std::snprintf(label, sizeof(label), useGiB ? "%.2f" : "%.0f", value / unit);
            draw->AddText(ImVec2(origin.x + 4.0f, y - 7.0f), textColor, label);
        }

        for(int tick = 0; tick <= 4; ++tick)
        {
            const float x = topLeft.x + (bottomRight.x - topLeft.x) * tick / 4.0f;
            draw->AddLine(ImVec2(x, topLeft.y), ImVec2(x, bottomRight.y), gridColor);
            const double secondsAgo = HistorySeconds * (4 - tick) / 4.0;

            if(tick == 4)
            {
                std::snprintf(label, sizeof(label), "newest");
            }
            else
            {
                std::snprintf(label, sizeof(label), "-%.0f s", secondsAgo);
            }

            // The rightmost label ends at the plot edge instead of running past it.
            const float labelWidth = ImGui::CalcTextSize(label).x;
            const float labelX = tick == 4 ? x - labelWidth : x - labelWidth * 0.5f;
            draw->AddText(ImVec2(labelX, bottomRight.y + 4.0f), textColor, label);
        }

        if(count > 0)
        {
            const auto newest = model.Sample(count - 1).time;
            draw->PushClipRect(topLeft, bottomRight, true);

            for(std::size_t line = 0; line < seriesCount; ++line)
            {
                bool hasPrevious = false;
                ImVec2 previous;
                std::chrono::steady_clock::time_point previousTime{};

                for(std::size_t index = 0; index < count; ++index)
                {
                    const MemoryHistorySample& sample = model.Sample(index);
                    double value = 0.0;

                    if(!series[line].value(sample, value))
                    {
                        hasPrevious = false;
                        continue;
                    }

                    const double age = std::chrono::duration<double>(newest - sample.time).count();
                    const ImVec2 point(bottomRight.x - static_cast<float>(age / HistorySeconds) * (bottomRight.x - topLeft.x), valueY(value));
                    const bool joined = hasPrevious && sample.time - previousTime <= MemoryProfilerModel::GapThreshold;

                    if(joined)
                    {
                        draw->AddLine(previous, point, series[line].color, series[line].reference ? 1.0f : 2.0f);
                    }
                    else if(!series[line].reference)
                    {
                        // An isolated sample still shows as a point.
                        draw->AddCircleFilled(point, 2.0f, series[line].color);
                    }

                    previous = point;
                    previousTime = sample.time;
                    hasPrevious = true;
                }
            }

            draw->PopClipRect();
        }

        ImGui::Dummy(ImVec2(width, height));

        // Legend with the newest values; an invalid newest sample reads "unavailable", never zero.
        for(std::size_t line = 0; line < seriesCount; ++line)
        {
            double value = 0.0;
            const bool valid = count > 0 && series[line].value(model.Sample(count - 1), value);
            char text[32];

            if(valid)
            {
                FormatBytes(static_cast<std::uint64_t>(value), text, sizeof(text));
            }
            else
            {
                std::snprintf(text, sizeof(text), "unavailable");
            }

            if(line > 0)
            {
                ImGui::SameLine(0.0f, 24.0f);
            }

            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(series[line].color), "%s%s: %s", series[line].reference ? "- - " : "",
                series[line].name, text);
        }
    }

    bool WorkingSet(const MemoryHistorySample& sample, double& value)
    {
        value = static_cast<double>(sample.workingSetBytes);
        return sample.processValid;
    }

    bool PrivateCommit(const MemoryHistorySample& sample, double& value)
    {
        value = static_cast<double>(sample.privateBytes);
        return sample.processValid;
    }

    bool LocalUsage(const MemoryHistorySample& sample, double& value)
    {
        value = static_cast<double>(sample.localUsageBytes);
        return sample.localValid;
    }

    bool LocalBudget(const MemoryHistorySample& sample, double& value)
    {
        value = static_cast<double>(sample.localBudgetBytes);
        return sample.localValid && sample.localBudgetBytes > 0;
    }

    bool NonLocalUsage(const MemoryHistorySample& sample, double& value)
    {
        value = static_cast<double>(sample.nonLocalUsageBytes);
        return sample.nonLocalValid;
    }

    bool TrackedAllocations(const MemoryHistorySample& sample, double& value)
    {
        value = static_cast<double>(sample.trackedBytes);
        return sample.trackedValid;
    }

    void DrawHistory(const MemoryProfilerModel& model)
    {
        static constexpr Series Process[] = {
            {"Working set", IM_COL32(90, 170, 255, 255), &WorkingSet, false},
            {"Private commit", IM_COL32(255, 150, 70, 255), &PrivateCommit, false}};
        static constexpr Series Local[] = {
            {"Local usage", IM_COL32(110, 220, 120, 255), &LocalUsage, false},
            {"Local budget", IM_COL32(200, 200, 200, 255), &LocalBudget, true}};
        static constexpr Series Tracked[] = {{"Engine-tracked", IM_COL32(220, 120, 220, 255), &TrackedAllocations, false}};
        static constexpr Series NonLocal[] = {{"Non-local usage", IM_COL32(240, 210, 90, 255), &NonLocalUsage, false}};

        DrawGraph("CPU process memory", model, Process, 2);
        ImGui::Spacing();
        DrawGraph("DXGI local video memory", model, Local, 2);
        ImGui::Spacing();
        DrawGraph("Engine-tracked GPU allocations", model, Tracked, 1);
        ImGui::Spacing();
        DrawGraph("DXGI non-local memory", model, NonLocal, 1);
    }
}

namespace MemoryProfilerWindow
{
    void DrawMenuItem()
    {
        ImGui::MenuItem("Memory", nullptr, &State().visible);
    }

    void Draw()
    {
        WindowState& state = State();

        // Consumed every UI frame so history covers the time the window was closed; a no-op without a new sample.
        state.model.Observe(MemoryMonitor::Snapshot());

        if(!state.visible)
        {
            return;
        }

        ImGui::SetNextWindowSize(ImVec2(820.0f, 760.0f), ImGuiCond_FirstUseEver);

        if(ImGui::Begin("Memory", &state.visible))
        {
            DrawControls(state.model);
            DrawStatus(state.model);

            if(state.model.HasLatest())
            {
                if(ImGui::CollapsingHeader("Overview", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    DrawOverview(state.model);
                }

                if(ImGui::CollapsingHeader("Breakdown", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    DrawBreakdown(state.model);
                }

                if(ImGui::CollapsingHeader("History", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    DrawHistory(state.model);
                }
            }
        }

        ImGui::End();
    }

    bool IsVisible()
    {
        return State().visible;
    }

    void SetVisible(bool visible)
    {
        State().visible = visible;
    }

    MemoryProfilerModel* ModelForTesting()
    {
        return &State().model;
    }
}
