#include "EditorWorkspace.h"
#include "imgui_internal.h"

#include <algorithm>

namespace
{
    constexpr float SplitterSize = 6.0f;
    constexpr float SplitterHitSize = 14.0f;
    constexpr float DividerSize = 2.0f;

    struct SplitterInteraction
    {
        ImDrawList* drawList = nullptr;

        float delta = 0.0f;

        bool hovered = false;
        bool active = false;
    };

    EditorWorkspaceRect ExpandedHitRectangle(
        const EditorWorkspaceRect& rectangle,
        const EditorWorkspaceRect& center,
        bool vertical)
    {
        EditorWorkspaceRect hit = rectangle;
        const float extra = (std::max)(0.0f, SplitterHitSize - (vertical ? rectangle.size.x : rectangle.size.y));

        // Expand into the reserved center only, never across Inspector or browser controls.
        if(vertical)
        {
            const float padding = (std::min)(extra, center.size.x);
            hit.position.x -= padding;
            hit.size.x += padding;
        }
        else
        {
            const float padding = (std::min)(extra, center.size.y);
            hit.position.y -= padding;
            hit.size.y += padding;
            hit.size.x = center.size.x;
        }

        return hit;
    }

    SplitterInteraction ProcessSplitter(
        const char* name,
        const EditorWorkspaceRect& rectangle,
        bool vertical)
    {
        if(rectangle.size.x <= 0.0f || rectangle.size.y <= 0.0f)
        {
            return {};
        }

        ImGui::SetNextWindowPos(rectangle.position, ImGuiCond_Always);
        ImGui::SetNextWindowSize(rectangle.size, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing;
        SplitterInteraction interaction;

        if(ImGui::Begin(name, nullptr, flags))
        {
            EditorWorkspace::KeepCurrentWindowBehindFloatingUI();
            interaction.drawList = ImGui::GetWindowDrawList();
            ImGui::InvisibleButton("Resize", rectangle.size);

            interaction.active = ImGui::IsItemActive();
            interaction.hovered = ImGui::IsItemHovered();

            if(interaction.active || interaction.hovered)
            {
                ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
            }

            if(interaction.active)
            {
                interaction.delta = vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
            }
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
        return interaction;
    }

    void PositionSplitter(
        const char* name,
        const EditorWorkspaceRect& rectangle,
        const EditorWorkspaceRect& hit,
        bool vertical,
        const SplitterInteraction& interaction)
    {
        if(rectangle.size.x <= 0.0f || rectangle.size.y <= 0.0f || !interaction.drawList)
        {
            return;
        }

        // Move the input window to the final boundary before the next frame's hit testing.
        ImGui::SetWindowPos(name, hit.position, ImGuiCond_Always);
        ImGui::SetWindowSize(name, hit.size, ImGuiCond_Always);
        EditorWorkspaceRect divider = rectangle;

        if(vertical)
        {
            divider.size.x = (std::min)(DividerSize, rectangle.size.x);
            divider.position.x += (rectangle.size.x - divider.size.x) * 0.5f;
        }
        else
        {
            divider.size.y = (std::min)(DividerSize, rectangle.size.y);
            divider.position.y += (rectangle.size.y - divider.size.y) * 0.5f;
        }

        // Fill the existing layout strip so the narrow visible line leaves no unused gap.
        const ImVec2 end(rectangle.position.x + rectangle.size.x, rectangle.position.y + rectangle.size.y);
        // Draw in the input window's normal layer, at the final same-frame boundary.
        // Its initial clip rectangle may precede a drag-induced layout change.
        interaction.drawList->PushClipRect(rectangle.position, end, false);
        interaction.drawList->AddRectFilled(rectangle.position, end, ImGui::GetColorU32(ImGuiCol_WindowBg));
        const ImGuiCol color = interaction.active ? ImGuiCol_SeparatorActive :
            interaction.hovered ? ImGuiCol_SeparatorHovered : ImGuiCol_Separator;
        interaction.drawList->AddRectFilled(
            divider.position,
            ImVec2(divider.position.x + divider.size.x, divider.position.y + divider.size.y),
            ImGui::GetColorU32(color));
        interaction.drawList->PopClipRect();
    }
}

void EditorWorkspace::KeepCurrentWindowBehindFloatingUI()
{
    // ImGui's public window flags prevent focus promotion but do not establish a
    // persistent lower layer. Limit this internal display-order operation to fixed
    // workspace windows; popup/modal focus and input blocking remain ImGui-owned.
    ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
}

void EditorWorkspace::Calculate()
{
    const float width = m_workspace.size.x;
    const float height = m_workspace.size.y;

    // Preserve at least half the workspace for the scene on small windows.
    const float horizontalSize = m_assetBrowserVisible ? (std::min)(SplitterSize, height * 0.05f) : 0.0f;
    const float browserMax = (std::max)(0.0f, height - (std::min)(180.0f, height * 0.5f) - horizontalSize);
    const float browserHeight = m_assetBrowserVisible ?
        (std::clamp)(m_assetBrowserHeight, (std::min)(140.0f, browserMax), browserMax) : 0.0f;
    const float upperHeight = height - browserHeight - horizontalSize;

    const float verticalSize = m_inspectorVisible ? (std::min)(SplitterSize, width * 0.05f) : 0.0f;
    const float inspectorMax = (std::max)(0.0f, width - (std::min)(240.0f, width * 0.5f) - verticalSize);
    const float inspectorWidth = m_inspectorVisible ?
        (std::clamp)(m_inspectorWidth, (std::min)(200.0f, inspectorMax), inspectorMax) : 0.0f;
    const float centerWidth = width - inspectorWidth - verticalSize;
    const ImVec2 origin = m_workspace.position;

    m_sceneViewport = {origin, ImVec2(centerWidth, upperHeight)};
    m_verticalSplitter = {ImVec2(origin.x + centerWidth, origin.y), ImVec2(verticalSize, upperHeight)};
    m_inspector = {ImVec2(origin.x + centerWidth + verticalSize, origin.y), ImVec2(inspectorWidth, upperHeight)};
    m_horizontalSplitter = {ImVec2(origin.x, origin.y + upperHeight), ImVec2(width, horizontalSize)};
    m_assetBrowser = {ImVec2(origin.x, origin.y + upperHeight + horizontalSize), ImVec2(width, browserHeight)};
}

void EditorWorkspace::Update()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    m_workspace = {viewport->WorkPos, ImVec2(
        (std::max)(0.0f, viewport->WorkSize.x),
        (std::max)(0.0f, viewport->WorkSize.y))};
    Calculate();

    SplitterInteraction horizontal;
    SplitterInteraction vertical;
    SplitterInteraction horizontalTail;

    // Resolve all input before panels consume their final rectangles.
    if(m_assetBrowserVisible)
    {
        horizontal = ProcessSplitter("##WorkspaceHorizontalSplitter",
            ExpandedHitRectangle(m_horizontalSplitter, m_sceneViewport, false), false);
        // Keep the original narrow handle beneath Inspector, without expanding into its controls.
        auto tail = m_horizontalSplitter;
        tail.position.x += m_sceneViewport.size.x;
        tail.size.x -= m_sceneViewport.size.x;
        horizontalTail = ProcessSplitter("##WorkspaceHorizontalTail", tail, false);
        horizontal.hovered = horizontal.hovered || horizontalTail.hovered;
        horizontal.active = horizontal.active || horizontalTail.active;
        m_assetBrowserHeight = m_assetBrowser.size.y - horizontal.delta - horizontalTail.delta;
        Calculate();
    }

    if(m_inspectorVisible)
    {
        auto hit = ExpandedHitRectangle(m_verticalSplitter, m_sceneViewport, true);

        // Give the horizontal handle exclusive ownership of the bottom corner.
        if(m_assetBrowserVisible)
        {
            hit.size.y = (std::max)(0.0f, hit.size.y - (SplitterHitSize - m_horizontalSplitter.size.y));
        }

        vertical = ProcessSplitter("##WorkspaceVerticalSplitter", hit, true);
        m_inspectorWidth = m_inspector.size.x - vertical.delta;
        Calculate();
    }

    // Recompute from this frame's live handles; hidden/released handles cannot leave a queued resize.
    m_splitterDragging = (horizontal.active || vertical.active) &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::GetIO().AppFocusLost;

    auto verticalHit = ExpandedHitRectangle(m_verticalSplitter, m_sceneViewport, true);

    if(m_assetBrowserVisible)
    {
        verticalHit.size.y = (std::max)(0.0f,
            verticalHit.size.y - (SplitterHitSize - m_horizontalSplitter.size.y));
    }

    PositionSplitter("##WorkspaceHorizontalSplitter", m_horizontalSplitter,
        ExpandedHitRectangle(m_horizontalSplitter, m_sceneViewport, false), false, horizontal);
    PositionSplitter("##WorkspaceVerticalSplitter", m_verticalSplitter, verticalHit, true, vertical);

    if(m_assetBrowserVisible)
    {
        auto tail = m_horizontalSplitter;
        tail.position.x += m_sceneViewport.size.x;
        tail.size.x -= m_sceneViewport.size.x;
        horizontalTail.hovered = horizontal.hovered;
        horizontalTail.active = horizontal.active;
        PositionSplitter("##WorkspaceHorizontalTail", tail, tail, false, horizontalTail);
    }
}
