#pragma once

#include "Debug/DebugUI.h"
#include "Scene/CameraExposure.h"

#include <imgui.h>

#include <cmath>
#include <string>
#include <unordered_map>

// Photographic exposure controls shared by the Camera component inspector and the Editor viewport camera.
// Edits apply to `exposure` only when complete (a value field on deactivation, a combo or checkbox immediately) and
// only if the whole result validates; the function returns true then, so the caller commits it once. Exposure is
// presentation-only: it never restarts accumulation.
inline bool DrawExposureControls(const char* id, CameraExposure& exposure, float referenceEV100,
    const DebugUI::ExposureDisplay* live)
{
    ImGui::PushID(id);
    // A draft per widget survives between frames while one of its fields is being typed into, and otherwise
    // follows the authored value (undo, another inspector, a scene load).
    static std::unordered_map<ImGuiID, std::pair<CameraExposure, bool>> drafts;
    auto& [edited, editing] = drafts[ImGui::GetID("draft")];

    if(!editing)
    {
        edited = exposure;
    }

    bool committed = false;
    bool active = false;
    const auto value = [&](const char* label, float& field, const char* format)
    {
        ImGui::InputFloat(label, &field, 0.0f, 0.0f, format);
        committed |= ImGui::IsItemDeactivatedAfterEdit();
        active |= ImGui::IsItemActive();
    };

    int mode = static_cast<int>(edited.mode);
    const char* modes[] = {"Manual", "Auto"};

    if(ImGui::Combo("Exposure Mode", &mode, modes, IM_ARRAYSIZE(modes)))
    {
        edited.mode = static_cast<ExposureMode>(mode);
        committed = true;
    }

    if(edited.mode == ExposureMode::Manual)
    {
        value("Aperture (f-number)", edited.aperture, "f/%.2f");
        value("Shutter (s)", edited.shutterSeconds, "%.6g");
        ImGui::SameLine();
        ImGui::TextDisabled("1/%.0f", edited.shutterSeconds > 0.0f ? 1.0f / edited.shutterSeconds : 0.0f);
        value("ISO", edited.iso, "%.0f");
    }

    value("Exposure Compensation (EV)", edited.compensation, "%+.2f");

    if(edited.mode == ExposureMode::Auto)
    {
        value("Metering Min (EV100)", edited.meteringMinEV100, "%.1f");
        value("Metering Max (EV100)", edited.meteringMaxEV100, "%.1f");
        value("Low Percentile", edited.lowPercentile, "%.3f");
        value("High Percentile", edited.highPercentile, "%.3f");
        value("Brightening Speed (1/s)", edited.brighteningSpeed, "%.2f");
        value("Darkening Speed (1/s)", edited.darkeningSpeed, "%.2f");

        if(ImGui::Checkbox("Lock Exposure", &edited.locked))
        {
            committed = true;
        }
    }

    const double manual = Photometry::ManualEV100(exposure) - exposure.compensation;

    if(exposure.mode == ExposureMode::Manual)
    {
        ImGui::Text("EV100 %.2f  (effective %.2f, scene reference %.2f)", Photometry::ManualEV100(exposure), manual,
            referenceEV100);
    }

    if(live && live->valid)
    {
        ImGui::Text("Current: EV100 %.2f, x%.4g (metered %.2f, target %.2f)", live->effectiveEV100, live->multiplier,
            live->meteredEV100, live->targetEV100);

        if(live->invalidPixels != 0)
        {
            ImGui::TextDisabled("%u non-finite or negative pixels excluded from metering", live->invalidPixels);
        }
    }

    ImGui::PopID();
    editing = active;

    if(committed && !(edited == exposure) && ValidateCameraExposure(edited))
    {
        exposure = edited;
        return true;
    }

    if(committed)
    {
        // Invalid or unchanged: the authored value stays and the draft follows it again.
        editing = false;
    }

    return false;
}
