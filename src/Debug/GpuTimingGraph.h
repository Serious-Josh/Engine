#pragma once

#include "GpuTimingRegionNames.h"
#include "imgui.h"
#include <cstdio>

// Drawing and display controls only. Collection remains in GpuTimingMonitor.
class GpuTimingGraph
{
    public:
    GpuTimingGraph()
    {
        m_selected[static_cast<unsigned int>(GpuTimestampProfiler::Region::NrdReblurSpecular)] = true;
        m_selected[static_cast<unsigned int>(GpuTimestampProfiler::Region::NrdReblurDiffuse)] = true;
        m_selected[static_cast<unsigned int>(GpuTimestampProfiler::Region::RayTracedReflections)] = true;
    }

    void Draw(GpuTimingMonitor& monitor)
    {
        if (!visible)
        {
            return;
        }

        ImGui::SetNextWindowSize(ImVec2(1000.0f, 600.0f), ImGuiCond_FirstUseEver);

        if (ImGui::Begin("GPU Timing Graph", &visible))
        {
            DrawControls(monitor);

            if (!monitor.enabled || monitor.frozen)
            {
                ImGui::TextUnformatted("Shared history collection is paused by GPU Timing Monitor controls.");
            }

            ImGui::BeginChild("Pass selection", ImVec2(240.0f, 0.0f), true);

            DrawSelection();
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("Graph panel", ImVec2(0.0f, 0.0f), false);
            const auto& history = m_paused ? m_snapshot : monitor.graphHistory;
            DrawPlot(history);
            DrawLegend(history);
            ImGui::EndChild();
        }

        ImGui::End();
    }

    bool visible = false;

    private:
    std::array<bool, GpuTimingMonitor::RegionCount> m_selected{};

    GpuTimingMonitor::GraphHistory m_snapshot;

    bool m_paused = false;
    bool m_autoScale = true;

    int m_visibleSamples = 120;
    int m_mode = 0;

    float m_manualMinimum = 0.0f;
    float m_manualMaximum = 5.0f;

    static ImVec4 RegionColor(unsigned int region)
    {
        // Golden-ratio hue spacing gives stable colors without a palette limit.
        const float hue = std::fmod(static_cast<float>(region) * 0.61803398875f, 1.0f);
        ImVec4 color(
            0.0f,
            0.0f,
            0.0f,
            1.0f);
        ImGui::ColorConvertHSVtoRGB(hue, 0.65f, 0.95f, color.x, color.y, color.z);
        return color;
    }

    void DrawControls(GpuTimingMonitor& monitor)
    {
        if (ImGui::Button(m_paused ? "Resume" : "Pause"))
        {
            m_paused = !m_paused;

            if (m_paused)
            {
                m_snapshot = monitor.graphHistory;
            }
        }

        ImGui::SameLine();

        if (ImGui::Button("Clear graph history"))
        {
            monitor.graphHistory = {};
            m_snapshot = {};
        }

        ImGui::SetNextItemWidth(220.0f);
        ImGui::Combo("Mode", &m_mode, "Absolute Timing (ms)\0Delta From Baseline (ms)\0");

        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderInt("Visible samples", &m_visibleSamples, 30, 120);
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scale Y", &m_autoScale);

        if (!m_autoScale)
        {
            ImGui::SetNextItemWidth(100.0f);

            ImGui::InputFloat("Y min (ms)", &m_manualMinimum);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100.0f);
            ImGui::InputFloat("Y max (ms)", &m_manualMaximum);
        }
    }

    void DrawSelection()
    {
        if (ImGui::Button("Select All"))
        {
            m_selected.fill(true);
        }

        ImGui::SameLine();

        if (ImGui::Button("Clear Selection"))
        {
            m_selected.fill(false);
        }

        for (unsigned int region = 0; region < m_selected.size(); ++region)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, RegionColor(region));

            ImGui::Checkbox(GpuTimingRegionNames[region], &m_selected[region]);
            ImGui::PopStyleColor();
        }
    }

    bool GetValue(
        const GpuTimingMonitor::FrameSample& sample,
        unsigned int region,
        double& value) const
    {
        const uint32_t validMask = m_mode == 0 ? sample.validTimings : sample.validDeltas;

        if ((validMask & (1u << region)) == 0)
        {
            return false;
        }

        value = m_mode == 0 ? sample.timings[region] : sample.deltas[region];
        return true;
    }

    void CalculateScale(
        const GpuTimingMonitor::GraphHistory& history,
        unsigned int firstSample,
        double& minimum,
        double& maximum) const
    {
        minimum = 0.0;
        maximum = 0.0;

        if (!m_autoScale)
        {
            if (std::isfinite(m_manualMinimum) && std::isfinite(m_manualMaximum) && m_manualMaximum > m_manualMinimum)
            {
                minimum = m_manualMinimum;
                maximum = m_manualMaximum;
                return;
            }
        }

        for (unsigned int i = firstSample; i < history.count; ++i)
        {
            const auto& sample = history.GetSample(i);

            for (unsigned int region = 0; region < m_selected.size(); ++region)
            {
                double value = 0.0;

                if (m_selected[region] && GetValue(sample, region, value))
                {
                    minimum = (std::min)(minimum, value);

                    maximum = (std::max)(maximum, value);
                }
            }
        }

        const double padding = (std::max)((maximum - minimum) * 0.1, 0.05);
        maximum += padding;

        if (m_mode != 0)
        {
            minimum -= padding;
        }
    }

    void DrawPlot(const GpuTimingMonitor::GraphHistory& history)
    {
        const unsigned int visibleCount = (std::min)(history.count, static_cast<unsigned int>(m_visibleSamples));

        const unsigned int firstSample = history.count - visibleCount;
        double minimum;
        double maximum;
        CalculateScale(history, firstSample, minimum, maximum);

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float width = (std::max)(available.x, 160.0f);
        const float height = (std::max)(available.y * 0.6f, 160.0f);
        const ImVec2 topLeft(
            origin.x + 65.0f,
            origin.y + 20.0f);
        const ImVec2 bottomRight(
            origin.x + width - 12.0f,
            origin.y + height - 35.0f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 gridColor = ImGui::GetColorU32(ImGuiCol_Border);
        const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
        auto valueY = [&](double value)
        {
            return bottomRight.y -
                   static_cast<float>((value - minimum) / (maximum - minimum)) * (bottomRight.y - topLeft.y);
        };

        draw->AddText(origin, textColor, m_mode == 0 ? "GPU time (ms)" : "Delta (ms)");

        for (unsigned int line = 0; line <= 4; ++line)
        {
            const double value = minimum + (maximum - minimum) * line / 4.0;
            const float y = valueY(value);

            draw->AddLine(ImVec2(topLeft.x, y), ImVec2(bottomRight.x, y), gridColor);
            char label[32];
            std::snprintf(label, sizeof(label), "%.2f", value);
            draw->AddText(ImVec2(origin.x, y - 6.0f), textColor, label);
            const float x = topLeft.x + (bottomRight.x - topLeft.x) * line / 4.0f;
            draw->AddLine(ImVec2(x, topLeft.y), ImVec2(x, bottomRight.y), gridColor);
        }

        if (m_mode != 0 && minimum <= 0.0 && maximum >= 0.0)
        {
            draw->AddLine(ImVec2(topLeft.x, valueY(0.0)), ImVec2(bottomRight.x, valueY(0.0)), textColor);
        }

        DrawLines(history, firstSample, topLeft, bottomRight, minimum, maximum);

        if (visibleCount > 0)
        {
            char label[128];
            std::snprintf(label, sizeof(label), "GPU frames %llu - %llu (oldest to newest)",
                          static_cast<unsigned long long>(history.GetSample(firstSample).frame),
                          static_cast<unsigned long long>(history.GetSample(history.count - 1u).frame));
            draw->AddText(ImVec2(topLeft.x, bottomRight.y + 12.0f), textColor, label);
        }

        ImGui::Dummy(ImVec2(width, height));
    }

    void DrawLines(
        const GpuTimingMonitor::GraphHistory& history,
        unsigned int firstSample,
        ImVec2 topLeft,
        ImVec2 bottomRight,
        double minimum,
        double maximum)
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(topLeft, bottomRight, true);
        const unsigned int count = history.count - firstSample;

        for (unsigned int region = 0; region < m_selected.size(); ++region)
        {
            if (!m_selected[region])
            {
                continue;
            }

            const ImU32 color = ImGui::ColorConvertFloat4ToU32(RegionColor(region));
            bool hasPrevious = false;

            ImVec2 previous;

            uint64_t previousFrame = 0;

            for (unsigned int i = 0; i < count; ++i)
            {

                const auto& sample = history.GetSample(firstSample + i);
                double value = 0.0;

                if (!GetValue(sample, region, value))
                {
                    hasPrevious = false;
                    continue;
                }

                const float fraction = count > 1 ? static_cast<float>(i) / (count - 1u) : 1.0f;

                const ImVec2 point(
                    topLeft.x + fraction * (bottomRight.x - topLeft.x),
                    bottomRight.y - static_cast<float>((value - minimum) / (maximum - minimum)) * (bottomRight.y - topLeft.y));

                // Missing resolved frames and invalid region samples break the line.

                if (hasPrevious && sample.frame == previousFrame + 1u)
                {
                    draw->AddLine(previous, point, color, 1.5f);
                }

                draw->AddCircleFilled(point, 1.8f, color);
                previous = point;

                previousFrame = sample.frame;
                hasPrevious = true;
            }
        }

        draw->PopClipRect();
    }

    void DrawLegend(const GpuTimingMonitor::GraphHistory& history)
    {
        for (unsigned int region = 0; region < m_selected.size(); ++region)
        {
            if (!m_selected[region])
            {
                continue;
            }

            double value = 0.0;

            const bool valid = history.count > 0 && GetValue(history.GetSample(history.count - 1u), region, value);

            if (valid)
            {
                const double timing = history.GetSample(history.count - 1u).timings[region];

                if (m_mode == 0)
                {
                    ImGui::TextColored(RegionColor(region), "%s: %.3f ms", GpuTimingRegionNames[region], timing);
                }
                else
                {
                    ImGui::TextColored(RegionColor(region), "%s: %.3f ms (delta %+.3f ms)",
                                       GpuTimingRegionNames[region], timing, value);
                }
            }
            else
            {
                ImGui::TextColored(RegionColor(region), "%s: --", GpuTimingRegionNames[region]);
            }
        }
    }
};
