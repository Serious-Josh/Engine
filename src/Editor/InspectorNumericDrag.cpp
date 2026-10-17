#include "Editor/InspectorNumericDrag.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cassert>

namespace
{
constexpr int MaximumComponents = 4;

// The value at activation is kept in ImGui's per-window storage, keyed by the widget ID
// plus the component index, so it survives across the frames of one drag.
ImGuiID StartValueKey(ImGuiID widgetID, int componentIndex)
{
    return ImHashData(&componentIndex, sizeof(componentIndex), widgetID);
}

void StoreStartValues(ImGuiID widgetID, const float* values, int componentCount)
{
    ImGuiStorage* storage = ImGui::GetStateStorage();

    for(int componentIndex = 0; componentIndex < componentCount; ++componentIndex)
    {
        storage->SetFloat(StartValueKey(widgetID, componentIndex), values[componentIndex]);
    }
}

void RestoreStartValues(ImGuiID widgetID, float* values, int componentCount)
{
    ImGuiStorage* storage = ImGui::GetStateStorage();

    for(int componentIndex = 0; componentIndex < componentCount; ++componentIndex)
    {
        values[componentIndex] = storage->GetFloat(StartValueKey(widgetID, componentIndex), values[componentIndex]);
    }
}

bool DiffersFromStartValues(ImGuiID widgetID, const float* values, int componentCount)
{
    ImGuiStorage* storage = ImGui::GetStateStorage();

    for(int componentIndex = 0; componentIndex < componentCount; ++componentIndex)
    {
        const float startValue = storage->GetFloat(StartValueKey(widgetID, componentIndex), values[componentIndex]);

        if(values[componentIndex] != startValue)
        {
            return true;
        }
    }

    return false;
}
}

InspectorDragResult InspectorDragFloats(const char* label, float* values, int componentCount,
    const InspectorDragSpec& spec)
{
    assert(componentCount >= 1 && componentCount <= MaximumComponents);

    ImGuiSliderFlags flags = ImGuiSliderFlags_None;

    if(spec.clampTypedInput)
    {
        flags |= ImGuiSliderFlags_AlwaysClamp;
    }

    // Start values are keyed by the label's ID in the current ID stack. That stays stable
    // across frames, whichever component of a multi-component group is active.
    const ImGuiID widgetID = ImGui::GetID(label);

    if(componentCount == 1)
    {
        ImGui::DragScalar(label, ImGuiDataType_Float, values, spec.speed, &spec.minimum, &spec.maximum,
            spec.format, flags);
    }
    else
    {
        // A group: ImGui reports the group as active, activated or deactivated whenever
        // one of its component fields is.
        ImGui::DragScalarN(label, ImGuiDataType_Float, values, componentCount, spec.speed,
            &spec.minimum, &spec.maximum, spec.format, flags);
    }

    if(ImGui::IsItemActivated())
    {
        StoreStartValues(widgetID, values, componentCount);
    }

    // Escape while dragging (ImGui already reverts typed text on Escape): restore the
    // starting value and end the interaction without completing an edit.
    const bool dragging = ImGui::IsItemActive() && !ImGui::TempInputIsActive(ImGui::GetActiveID());

    if(dragging && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        RestoreStartValues(widgetID, values, componentCount);
        ImGui::ClearActiveID();
        return InspectorDragResult::Cancelled;
    }

    if(ImGui::IsItemDeactivated())
    {
        // A completed interaction that left the value unchanged (a click, an Escape-reverted
        // text entry or a drag returned to its start) is not an edit.
        if(DiffersFromStartValues(widgetID, values, componentCount))
        {
            return InspectorDragResult::Committed;
        }

        return InspectorDragResult::Cancelled;
    }

    if(ImGui::IsItemActive())
    {
        return InspectorDragResult::Editing;
    }

    return InspectorDragResult::Idle;
}

InspectorDragResult InspectorDragFloatTransformWidth(const char* label, float& value, const InspectorDragSpec& spec)
{
    // Transform's DragFloat3 divides the full item width into three component boxes with
    // PushMultiItemsWidths. Ask ImGui for that same first-component width.
    constexpr int transformComponentCount = 3;
    const float fullWidth = ImGui::CalcItemWidth();
    ImGui::PushMultiItemsWidths(transformComponentCount, fullWidth);
    const float componentWidth = ImGui::CalcItemWidth();

    for(int component = 0; component < transformComponentCount; ++component)
    {
        ImGui::PopItemWidth();
    }

    // Draw the box with a hidden label, then put the visible label directly after it,
    // separated by ItemInnerSpacing.x, the gap ImGui leaves before a Transform label.
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(componentWidth);
    const InspectorDragResult result = InspectorDragFloats("##value", &value, 1, spec);
    ImGui::PopID();

    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));

    return result;
}
