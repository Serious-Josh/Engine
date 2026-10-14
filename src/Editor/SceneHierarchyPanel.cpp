#include "SceneHierarchyPanel.h"

#include <algorithm>
#include <utility>
#include <cstring>
#include <limits>

namespace
{
    bool ReadHierarchyPayload(const ImGuiPayload* payload, const HierarchyDragIdentity& identity,
        std::string& objectID)
    {
        if(!payload || !payload->IsDataType(HierarchyDragPayloadType) ||
            payload->DataSize < static_cast<int>(sizeof(HierarchyDragPayload)))
        {
            return false;
        }

        HierarchyDragPayload header;
        std::memcpy(&header, payload->Data, sizeof(header));

        if(header.version != 1 || header.identity != identity || identity.sessionEpoch == 0 ||
            header.objectIDBytes < 2 || sizeof(header) + header.objectIDBytes != static_cast<size_t>(payload->DataSize))
        {
            return false;
        }

        const auto* text = static_cast<const char*>(payload->Data) + sizeof(header);

        if(text[header.objectIDBytes - 1] != '\0' || std::memchr(text, '\0', header.objectIDBytes - 1))
        {
            return false;
        }

        objectID.assign(text, header.objectIDBytes - 1);
        return true;
    }

    void DrawObjectIcon(ImDrawList& draw, ImVec2 origin, float size)
    {
        const ImVec2 top(origin.x + size * 0.5f, origin.y);
        const ImVec2 left(origin.x, origin.y + size * 0.3f);
        const ImVec2 right(origin.x + size, left.y);
        const ImVec2 center(top.x, origin.y + size * 0.6f);
        const ImVec2 bottom(top.x, origin.y + size);
        const ImU32 color = ImGui::GetColorU32(ImGuiCol_TextDisabled);

        draw.AddLine(top, left, color);
        draw.AddLine(top, right, color);
        draw.AddLine(left, center, color);
        draw.AddLine(right, center, color);
        draw.AddLine(center, bottom, color);
        draw.AddLine(left, ImVec2(left.x, origin.y + size * 0.7f), color);
        draw.AddLine(right, ImVec2(right.x, origin.y + size * 0.7f), color);
        draw.AddLine(ImVec2(left.x, origin.y + size * 0.7f), bottom, color);
        draw.AddLine(ImVec2(right.x, origin.y + size * 0.7f), bottom, color);
    }
}

std::optional<HierarchyReorderRequest> SceneHierarchyPanel::TakeReorderRequest()
{
    return std::exchange(m_reorderRequest, std::nullopt);
}

void SceneHierarchyPanel::DrawReorderInteraction(const SceneHierarchyEntry& entry,
    const SceneHierarchySnapshot& snapshot, const HierarchyDragIdentity& identity)
{
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();

    if(ImGui::BeginDragDropSource())
    {
        if(entry.objectID.size() < static_cast<size_t>((std::numeric_limits<int>::max)()) -
            sizeof(HierarchyDragPayload) - 1)
        {
            HierarchyDragPayload header;
            header.identity = identity;
            header.objectIDBytes = static_cast<std::uint32_t>(entry.objectID.size() + 1);
            std::vector<char> bytes(sizeof(header) + header.objectIDBytes);
            std::memcpy(bytes.data(), &header, sizeof(header));
            std::memcpy(bytes.data() + sizeof(header), entry.objectID.c_str(), header.objectIDBytes);
            ImGui::SetDragDropPayload(HierarchyDragPayloadType, bytes.data(), bytes.size(), ImGuiCond_Once);
            ImGui::TextUnformatted(entry.name.c_str());
        }

        ImGui::EndDragDropSource();
    }

    if(!ImGui::BeginDragDropTarget())
    {
        return;
    }

    std::string sourceID;

    if(!ReadHierarchyPayload(ImGui::GetDragDropPayload(), identity, sourceID))
    {
        ImGui::EndDragDropTarget();
        return;
    }

    const auto source = std::find_if(snapshot.entries.begin(), snapshot.entries.end(),
        [&](const SceneHierarchyEntry& candidate) { return candidate.objectID == sourceID; });

    if(source == snapshot.entries.end())
    {
        ImGui::EndDragDropTarget();
        return;
    }

    const auto* payload = ImGui::AcceptDragDropPayload(HierarchyDragPayloadType,
        ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);

    if(payload)
    {
        const bool insertAfter = ImGui::GetIO().MousePos.y >= (rowMin.y + rowMax.y) * 0.5f;
        const float indicatorY = insertAfter ? rowMax.y : rowMin.y;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(rowMin.x, indicatorY), ImVec2(rowMax.x, indicatorY),
            ImGui::GetColorU32(ImGuiCol_DragDropTarget), 2.0f);

        if(payload->IsDelivery())
        {
            m_reorderRequest = HierarchyReorderRequest{identity, std::move(sourceID), entry.objectID, insertAfter};
        }
    }

    ImGui::EndDragDropTarget();
}

void SceneHierarchyPanel::AutoScrollDrag(const HierarchyDragIdentity& identity, float listTop)
{
    std::string sourceID;

    if(!ReadHierarchyPayload(ImGui::GetDragDropPayload(), identity, sourceID) ||
        !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
    {
        return;
    }

    const float top = (std::max)(listTop, ImGui::GetWindowPos().y + ImGui::GetFrameHeight());
    const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y - ImGui::GetStyle().WindowPadding.y;
    const float mouseY = ImGui::GetIO().MousePos.y;
    const float edge = ImGui::GetFrameHeight();
    float direction = 0.0f;

    if(mouseY < top + edge)
    {
        direction = -1.0f;
    }
    else if(mouseY > bottom - edge)
    {
        direction = 1.0f;
    }

    ImGui::SetScrollY(ImGui::GetScrollY() + direction * 300.0f * ImGui::GetIO().DeltaTime);
}

void SceneHierarchyPanel::BeginRename(const std::string& objectID, const std::string& name)
{
    std::vector<char> buffer((std::max)(size_t{256}, name.size() + 1), '\0');
    std::copy(name.begin(), name.end(), buffer.begin());
    m_renameBuffer = std::move(buffer);
    m_renameObjectID = objectID;

    m_renameCommit.reset();
    m_focusRename = true;
}

void SceneHierarchyPanel::CancelRename()
{
    m_renameObjectID.clear();
    m_renameCommit.reset();
    m_focusRename = false;
}

std::optional<std::string> SceneHierarchyPanel::TakeRenameCommit()
{
    return std::exchange(m_renameCommit, std::nullopt);
}

int SceneHierarchyPanel::ResizeRenameBuffer(ImGuiInputTextCallbackData* data)
{
    auto& buffer = *static_cast<std::vector<char>*>(data->UserData);
    buffer.resize((std::max)(buffer.size() * 2, static_cast<size_t>(data->BufTextLen) + 1));
    data->Buf = buffer.data();
    return 0;
}

std::optional<std::string> SceneHierarchyPanel::TakeContextRequest()
{
    return std::exchange(m_contextRequest, std::nullopt);
}

void SceneHierarchyPanel::DrawRenameInput()
{
    const bool focusRequested = m_focusRename;

    if(focusRequested)
    {
        ImGui::SetKeyboardFocusHere();
        m_focusRename = false;
    }

    ImGui::SetNextItemWidth(-1.0f);
    const bool commit = ImGui::InputText("##Rename", m_renameBuffer.data(), m_renameBuffer.size(),
        ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue |
        ImGuiInputTextFlags_CallbackResize, ResizeRenameBuffer, &m_renameBuffer);

    if(focusRequested)
    {
        ImGui::SetScrollHereY(0.5f);
    }

    const bool active = ImGui::IsItemActive();
    const bool mouseClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
    const bool clickedElsewhere = mouseClicked && !ImGui::IsItemHovered();
    const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::GetIO().AppFocusLost ||
        clickedElsewhere || (!focusRequested && !active && !commit);

    if(cancel)
    {
        CancelRename();
        return;
    }

    if(commit)
    {
        m_renameCommit = std::string(m_renameBuffer.data());
        m_renameObjectID.clear();
    }
}

std::optional<std::size_t> SceneHierarchyPanel::Draw(const EditorWorkspaceRect& rectangle,
    const SceneHierarchySnapshot& snapshot, std::optional<std::size_t> selectedObject,
    const void* sceneIdentity, const std::string& error,
    const HierarchyDragIdentity& dragIdentity, bool allowReorder)
{
    if(rectangle.size.x <= 0.0f || rectangle.size.y <= 0.0f)
    {
        return std::nullopt;
    }

    ImGui::SetNextWindowPos(rectangle.position, ImGuiCond_Always);
    ImGui::SetNextWindowSize(rectangle.size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing;

    if(rectangle.size.y < ImGui::GetFrameHeight())
    {
        flags |= ImGuiWindowFlags_NoTitleBar;
    }

    std::optional<std::size_t> clickedObject;
    std::string selectedPersistentID;

    if(selectedObject)
    {
        const auto selected = std::find_if(snapshot.entries.begin(), snapshot.entries.end(),
            [&](const SceneHierarchyEntry& entry) { return entry.runtimeIndex == selectedObject; });

        if(selected != snapshot.entries.end())
        {
            selectedPersistentID = selected->objectID;
        }
    }

    if(ImGui::Begin("Hierarchy", nullptr, flags))
    {
        EditorWorkspace::KeepCurrentWindowBehindFloatingUI();
        ImGui::TextUnformatted(snapshot.sceneName.c_str());
        ImGui::Separator();

        if(!error.empty())
        {
            ImGui::TextWrapped("%s", error.c_str());
        }

        const float listTop = ImGui::GetCursorScreenPos().y;
        ImGui::PushID(sceneIdentity);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(snapshot.entries.size()), ImGui::GetFrameHeightWithSpacing());

        if(IsRenaming())
        {
            const auto target = std::find_if(snapshot.entries.begin(), snapshot.entries.end(),
                [&](const SceneHierarchyEntry& entry) { return entry.objectID == m_renameObjectID; });

            if(target == snapshot.entries.end())
            {
                CancelRename();
            }
            else
            {
                clipper.IncludeItemByIndex(static_cast<int>(target - snapshot.entries.begin()));
            }
        }

        while(clipper.Step())
        {
            for(int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
            {
                const auto& entry = snapshot.entries[index];

                if(entry.objectID.empty())
                {
                    ImGui::PushID(index);
                }
                else
                {
                    ImGui::PushID(entry.objectID.c_str());
                }

                const bool selected = entry.objectID.empty() ?
                    entry.runtimeIndex && selectedObject == entry.runtimeIndex : entry.objectID == selectedPersistentID;
                ImGui::BeginDisabled(!entry.runtimeIndex);
                const ImVec2 rowPosition = ImGui::GetCursorScreenPos();

                if(entry.objectID == m_renameObjectID && IsRenaming())
                {
                    DrawRenameInput();
                    ImGui::EndDisabled();
                    ImGui::PopID();
                    continue;
                }

                if(ImGui::Selectable("##Object", selected, ImGuiSelectableFlags_None,
                    ImVec2(0.0f, ImGui::GetFrameHeight())))
                {
                    clickedObject = entry.runtimeIndex;
                }

                const auto& io = ImGui::GetIO();

                if(ImGui::IsItemHovered() &&
                    ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
                    io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < io.MouseDragThreshold * io.MouseDragThreshold)
                {
                    clickedObject = entry.runtimeIndex;

                    if(!entry.objectID.empty())
                    {
                        m_contextRequest = entry.objectID;
                    }
                }

                if(allowReorder && !IsRenaming() && !entry.objectID.empty())
                {
                    DrawReorderInteraction(entry, snapshot, dragIdentity);
                }

                // Draw a small wire cube without relying on an icon font.
                const float iconSize = ImGui::GetFontSize() * 0.65f;
                const ImVec2 iconStart(rowPosition.x + 2.0f, rowPosition.y + 3.0f);
                auto* draw = ImGui::GetWindowDrawList();
                DrawObjectIcon(*draw, iconStart, iconSize);

                const char* name = entry.name.empty() ? "(Unnamed Object)" : entry.name.c_str();
                draw->AddText(ImVec2(rowPosition.x + iconSize + 10.0f, rowPosition.y),
                    ImGui::GetColorU32(ImGuiCol_Text), name);
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }

        ImGui::PopID();

        if(allowReorder)
        {
            AutoScrollDrag(dragIdentity, listTop);
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    return clickedObject;
}
