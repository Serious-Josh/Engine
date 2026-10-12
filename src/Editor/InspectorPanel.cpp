#include "InspectorPanel.h"
#include "EditorWorkspace.h"

void InspectorPanel::Draw(const EditorWorkspaceRect& rectangle)
{
    if(rectangle.size.x <= 0.0f || rectangle.size.y <= 0.0f)
    {
        return;
    }

    ImGui::SetNextWindowPos(rectangle.position, ImGuiCond_Always);
    ImGui::SetNextWindowSize(rectangle.size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

    // ImGui otherwise enforces title-bar height even when the assigned panel is smaller.
    if(rectangle.size.y < ImGui::GetFrameHeight())
    {
        flags |= ImGuiWindowFlags_NoTitleBar;
    }

    if(ImGui::Begin("Inspector", nullptr, flags))
    {
        ImGui::TextWrapped("No asset or scene object selected.");
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}
