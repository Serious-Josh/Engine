#include "Editor/Editor.h"
#include "imgui.h"

#include <utility>

void Editor::SetPhysicsStatus(PhysicsState state, bool canPlay, std::string message)
{
    m_physicsState = state;
    m_canPlayPhysics = canPlay;
    m_physicsMessage = std::move(message);
}

std::optional<Editor::PhysicsRequest> Editor::TakePhysicsRequest()
{
    return std::exchange(m_physicsRequest, std::nullopt);
}

bool Editor::CanStartPhysics() const
{
    return !m_sceneOperationPending && !m_fileRequest && !m_renameIntent &&
        !HasBlockingObjectCreation() && !HasBlockingMaterialOperation() && !HasPendingSceneHistoryOperation() &&
        !m_transformFieldEditing && !m_gizmoWasUsing && !m_inspectorPanel.HasUnsavedChanges();
}

void Editor::DrawPhysicsControls()
{
    ImGui::Separator();
    ImGui::BeginDisabled(m_physicsState != PhysicsState::Stopped || !m_canPlayPhysics || !CanStartPhysics());
    if (ImGui::Button("Play"))
    {
        m_physicsRequest = PhysicsRequest::Play;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_physicsState == PhysicsState::Stopped);
    if (ImGui::Button("Stop"))
    {
        m_physicsRequest = PhysicsRequest::Stop;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    const char* state = m_physicsState == PhysicsState::Playing ? "Playing" :
        m_physicsState == PhysicsState::Stopping ? "Stopping..." : "Stopped";
    ImGui::TextUnformatted(state);
    DrawCharacterDriveToggle();
    if (!m_physicsMessage.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_physicsMessage.c_str());
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", m_physicsMessage.c_str());
        }
    }
}
