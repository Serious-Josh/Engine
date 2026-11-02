#include "Editor/Editor.h"
#include "Editor/EditorIcons.h"
#include "imgui.h"

#include <utility>

void Editor::SetGameStatus(GameState state, bool canPlay, std::string message)
{
    if(state != GameState::Editing && m_gameState == GameState::Editing)
    {
        // Entering Play drops any viewport gesture so no authoring tool resumes mid-gesture, then
        // gives the game view input focus; clicking another panel takes it back as usual.
        SuspendViewportInteraction();
        m_viewportFocused = true;
    }

    m_gameState = state;
    m_canPlayGame = canPlay;
    m_gameMessage = std::move(message);
}

bool Editor::HasGameplayFocus() const
{
    if(m_gameState != GameState::Playing || !m_viewportFocused || !ImGui::GetCurrentContext())
    {
        return false;
    }

    const ImGuiIO& io = ImGui::GetIO();
    return !io.AppFocusLost && !io.WantTextInput && !io.WantCaptureKeyboard && !ImGui::IsAnyItemActive();
}

std::optional<EditorWorkspaceRect> Editor::GetGameplayCursorArea() const
{
    if(!HasGameplayFocus() || m_viewportImage.size.x <= 0.0f || m_viewportImage.size.y <= 0.0f)
    {
        return std::nullopt;
    }

    return m_viewportImage;
}

bool Editor::WasGameViewClicked() const
{
    return m_gameState == GameState::Playing && m_viewportHovered && ImGui::GetCurrentContext() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

void Editor::HideGameplayCursor()
{
    if(ImGui::GetCurrentContext())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }
}

std::optional<Editor::GameRequest> Editor::TakeGameRequest()
{
    return std::exchange(m_gameRequest, std::nullopt);
}

namespace
{
    // Square toolbar button showing an Editor icon; the tooltip also explains a disabled state.
    bool DrawIconButton(const char* id, const EditorIcons& icons, EditorIcon icon, bool enabled, const char* tooltip)
    {
        const float size = ImGui::GetFrameHeight();
        ImGui::BeginDisabled(!enabled);
        const bool pressed = ImGui::Button(id, ImVec2(size, size));
        const float inset = ImGui::GetStyle().FramePadding.y;
        const ImVec2 minimum = ImGui::GetItemRectMin();
        icons.Draw(*ImGui::GetWindowDrawList(), icon, ImVec2(minimum.x + inset, minimum.y + inset),
            ImVec2(size - inset * 2.0f, size - inset * 2.0f), ImGui::GetColorU32(ImGuiCol_Text));
        ImGui::EndDisabled();

        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && tooltip && *tooltip)
        {
            ImGui::SetTooltip("%s", tooltip);
        }

        return pressed;
    }
}

void Editor::DrawGamePlayControls(const EditorIcons& icons, float centerX)
{
    const float buttonWidth = ImGui::GetFrameHeight();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX((std::max)(0.0f, centerX - buttonWidth - spacing * 0.5f));

    // Physics Simulation and Game Play share one simulation; each refuses while the other runs.
    const bool canPlay = m_gameState == GameState::Editing && m_physicsState == PhysicsState::Stopped &&
        m_canPlayGame && CanStartPhysics();
    const char* playTooltip = canPlay ? "Play" :
        m_physicsState != PhysicsState::Stopped ? "Stop Physics Simulation before starting Game Play." :
        !m_gameMessage.empty() ? m_gameMessage.c_str() :
        "Finish the current editor operation, or wait for the scene to finish streaming, before playing.";

    if(DrawIconButton("##GamePlay", icons, EditorIcon::Play, canPlay, playTooltip))
    {
        m_gameRequest = GameRequest::Play;
    }

    ImGui::SameLine();
    const char* stopTooltip = m_gameState == GameState::Stopping ? "Stopping..." : "Stop";

    if(DrawIconButton("##GameStop", icons, EditorIcon::Stop, m_gameState == GameState::Playing, stopTooltip))
    {
        m_gameRequest = GameRequest::Stop;
    }
}
