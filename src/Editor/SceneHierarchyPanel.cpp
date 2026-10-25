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

        if(header.version != 2 || header.identity != identity || identity.sessionEpoch == 0 ||
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

std::optional<HierarchyDropRequest> SceneHierarchyPanel::TakeDropRequest()
{
    return std::exchange(m_dropRequest, std::nullopt);
}

void SceneHierarchyPanel::UpdateTree(const SceneHierarchySnapshot& snapshot, std::uint64_t epoch, const void* sceneIdentity)
{
    if(epoch != m_expansionEpoch || (epoch == 0 && sceneIdentity != m_expansionScene))
    {
        m_collapsed.clear();
        m_observedSelectionID.clear();
        m_pendingRevealID.clear();
        m_treeSnapshot.entries.clear();
        m_expansionEpoch = epoch;
        m_expansionScene = sceneIdentity;
        m_dropStatus.clear();
        m_rowsDirty = true;
    }

    const bool sameTopology = snapshot.entries.size() == m_treeSnapshot.entries.size() &&
        std::equal(snapshot.entries.begin(), snapshot.entries.end(), m_treeSnapshot.entries.begin(),
            [](const auto& left, const auto& right)
            {
                return left.objectID == right.objectID && left.parentID == right.parentID;
            });

    if(!sameTopology || m_tree.objectIDs.size() != snapshot.entries.size())
    {
        std::string error;

        if(!BuildHierarchyTree(snapshot, m_tree, error))
        {
            m_rows.clear();
            m_tree = {};
            m_pendingRevealID.clear();
            m_dropStatus = error;
            return;
        }

        m_treeSnapshot = snapshot;
        std::unordered_set<std::string> present(m_tree.objectIDs.begin(), m_tree.objectIDs.end());
        std::erase_if(m_collapsed, [&](const auto& id) { return !present.contains(id); });
        m_rowsDirty = true;
    }

    if(m_rowsDirty)
    {
        m_rows = VisibleHierarchyRows(m_tree, m_collapsed);
        m_rowsDirty = false;
    }
}

void SceneHierarchyPanel::ObserveSelection(const SceneHierarchySnapshot& snapshot,
    std::optional<size_t> selectedObject, std::uint64_t epoch, const void* sceneIdentity)
{
    UpdateTree(snapshot, epoch, sceneIdentity);
    std::string selectedID;

    if(selectedObject)
    {
        for(const auto& entry : snapshot.entries)
        {
            if(entry.runtimeIndex == selectedObject)
            {
                selectedID = entry.objectID;
                break;
            }
        }
    }

    if(selectedID != m_observedSelectionID)
    {
        m_observedSelectionID = selectedID;
        m_pendingRevealID = std::move(selectedID);
    }
}

void SceneHierarchyPanel::ExpandPendingSelection()
{
    if(m_pendingRevealID.empty())
    {
        return;
    }

    const auto selected = std::find(m_tree.objectIDs.begin(), m_tree.objectIDs.end(), m_pendingRevealID);

    if(selected == m_tree.objectIDs.end())
    {
        m_pendingRevealID.clear();
        return;
    }

    size_t index = static_cast<size_t>(selected - m_tree.objectIDs.begin());

    // Forests are validated; retain a bound and index checks for unexpected stale data.
    for(size_t visited = 0; visited < m_tree.parents.size(); ++visited)
    {
        if(index >= m_tree.parents.size() || !m_tree.parents[index])
        {
            break;
        }

        index = *m_tree.parents[index];

        if(index >= m_tree.objectIDs.size())
        {
            m_pendingRevealID.clear();
            return;
        }

        m_collapsed.erase(m_tree.objectIDs[index]);
    }

    m_rows = VisibleHierarchyRows(m_tree, m_collapsed);
    m_rowsDirty = false;
}

void SceneHierarchyPanel::DrawDropInteraction(const SceneHierarchyEntry* entry,
    const HierarchyDragIdentity& identity, const DropValidator& validateDrop)
{
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();

    if(entry && ImGui::BeginDragDropSource())
    {
        if(entry->objectID.size() < static_cast<size_t>((std::numeric_limits<int>::max)()) - sizeof(HierarchyDragPayload) - 1)
        {
            HierarchyDragPayload header;
            header.identity = identity;
            header.objectIDBytes = static_cast<std::uint32_t>(entry->objectID.size() + 1);
            m_dragBytes.resize(sizeof(header) + header.objectIDBytes);
            std::memcpy(m_dragBytes.data(), &header, sizeof(header));
            std::memcpy(m_dragBytes.data() + sizeof(header), entry->objectID.c_str(), header.objectIDBytes);
            ImGui::SetDragDropPayload(HierarchyDragPayloadType, m_dragBytes.data(), m_dragBytes.size(), ImGuiCond_Once);
            ImGui::TextUnformatted(entry->name.c_str());
            ImGui::TextUnformatted(ImGui::GetIO().KeyAlt ? "Keep Local" : "Keep World (Alt: Keep Local)");
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

    HierarchyDropRequest request{identity, std::move(sourceID), entry ? entry->objectID : std::string{},
        entry ? HierarchyDropZone::Parent : HierarchyDropZone::Root, !ImGui::GetIO().KeyAlt};

    if(entry)
    {
        const float fraction = (ImGui::GetIO().MousePos.y - rowMin.y) / (rowMax.y - rowMin.y);
        request.zone = fraction < 0.25f ? HierarchyDropZone::Before :
            fraction > 0.75f ? HierarchyDropZone::After : HierarchyDropZone::Parent;
    }

    std::string error;
    const bool valid = validateDrop && validateDrop(request, error);
    const auto* payload = ImGui::AcceptDragDropPayload(HierarchyDragPayloadType,
        ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);

    if(payload)
    {
        const ImU32 color = valid ? ImGui::GetColorU32(ImGuiCol_DragDropTarget) : IM_COL32(235, 85, 75, 255);
        auto* draw = ImGui::GetWindowDrawList();

        if(request.zone == HierarchyDropZone::Before || request.zone == HierarchyDropZone::After)
        {
            const float y = request.zone == HierarchyDropZone::Before ? rowMin.y : rowMax.y;
            draw->AddLine(ImVec2(rowMin.x, y), ImVec2(rowMax.x, y), color, 3.0f);
        }
        else
        {
            draw->AddRect(rowMin, rowMax, color, 2.0f, 0, 2.0f);
        }

        ImGui::SetTooltip("%s%s", valid ? (request.keepWorld ? "Keep World" : "Keep Local") : error.c_str(),
            valid ? " - row center: parent; edges: order; Alt: Keep Local" : "");

        if(payload->IsDelivery())
        {
            m_dropStatus = valid ? std::string{} : error;

            if(valid)
            {
                m_dropRequest = std::move(request);
            }
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
    const HierarchyDragIdentity& dragIdentity, bool allowReorder, bool allowCreation, std::span<const std::size_t> selectedObjects,
    const DropValidator& validateDrop, const std::function<EditorIcon(std::string_view)>& iconForObject)
{
    ObserveSelection(snapshot, selectedObject, dragIdentity.sessionEpoch, sceneIdentity);
    m_focused = false;
#if defined(ENGINE_HAS_VERIFICATION)
    m_verificationRows.clear();
#endif
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
    if(ImGui::Begin("Hierarchy", nullptr, flags))
    {
        m_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        EditorWorkspace::KeepCurrentWindowBehindFloatingUI();
        // Keep the button on the right of the content header; title bar, scrolling and row menus stay standard ImGui.
        const float buttonWidth = ImGui::GetFrameHeight();
        const auto headerStart = ImGui::GetCursorPos();
        const float available = ImGui::GetContentRegionAvail().x;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (std::max)(1.0f, available - buttonWidth - ImGui::GetStyle().ItemSpacing.x));
        ImGui::TextUnformatted(snapshot.sceneName.c_str());
        ImGui::PopTextWrapPos();
        const float afterTitle = ImGui::GetCursorPosY();
        ImGui::SetCursorPos(ImVec2(headerStart.x + (std::max)(0.0f, available - buttonWidth), headerStart.y));
        ImGui::BeginDisabled(!allowCreation);

        if(ImGui::Button("+", ImVec2(buttonWidth, buttonWidth)))
        {
            ImGui::OpenPopup("HierarchyCreate");
        }

        if(ImGui::BeginPopup("HierarchyCreate"))
        {
            // Top level, not a primitive: a Transform-only object with no mesh or material.
            if(ImGui::MenuItem("Empty Object"))
            {
                m_emptyObjectRequest = true;
            }

            // Top level: a meshless object carrying only a Camera component.
            if(ImGui::MenuItem("Camera"))
            {
                m_cameraObjectRequest = true;
            }

            // Top level: meshless objects carrying only a Light component of the chosen type.
            if(ImGui::BeginMenu("Light"))
            {
                if(ImGui::MenuItem("Point Light"))
                {
                    m_lightObjectRequest = LightType::Point;
                }

                if(ImGui::MenuItem("Spot Light"))
                {
                    m_lightObjectRequest = LightType::Spot;
                }

                if(ImGui::MenuItem("Directional Light"))
                {
                    m_lightObjectRequest = LightType::Directional;
                }

                ImGui::EndMenu();
            }

            if(ImGui::BeginMenu("Create Primitive"))
            {
                if(ImGui::MenuItem("Cube"))
                {
                    m_primitiveRequest = ScenePrimitive::Cube;
                }

                if(ImGui::MenuItem("Sphere"))
                {
                    m_primitiveRequest = ScenePrimitive::Sphere;
                }

                ImGui::EndMenu();
            }

            ImGui::EndPopup();
        }

        ImGui::EndDisabled();
        ImGui::SetCursorPosY((std::max)(afterTitle, headerStart.y + buttonWidth + ImGui::GetStyle().ItemSpacing.y));
        ImGui::Separator();

        if(!error.empty())
        {
            ImGui::TextWrapped("%s", error.c_str());
        }

        if(!m_dropStatus.empty())
        {
            ImGui::TextWrapped("%s", m_dropStatus.c_str());
        }

        const float listTop = ImGui::GetCursorScreenPos().y;
        ExpandPendingSelection();
        ImGui::PushID(sceneIdentity);
        ImGui::Selectable("Scene Root (drop to unparent)", false, ImGuiSelectableFlags_None,
            ImVec2(0, ImGui::GetFrameHeight()));

#if defined(ENGINE_HAS_VERIFICATION)
        m_verificationRoot = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
#endif
        if(allowReorder)
        {
            DrawDropInteraction(nullptr, dragIdentity, validateDrop);
        }

        if(allowCreation && ImGui::BeginPopupContextItem("HierarchyRootContext"))
        {
            if(ImGui::MenuItem("Create Empty Object"))
            {
                m_emptyObjectRequest = true;
            }

            ImGui::EndPopup();
        }

        const auto& style = ImGui::GetStyle();
        const auto& background = style.Colors[ImGuiCol_WindowBg];
        const float luminance = background.x * 0.2126f + background.y * 0.7152f + background.z * 0.0722f;
        const float stripeTone = luminance > 0.5f ? 0.0f : 1.0f;
        const ImU32 stripeColor = ImGui::GetColorU32(ImVec4(stripeTone, stripeTone, stripeTone, 0.09f));
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_rows.size()), ImGui::GetFrameHeightWithSpacing());

        if(!m_pendingRevealID.empty())
        {
            const auto row = std::find_if(m_rows.begin(), m_rows.end(), [&](const auto& candidate)
            {
                return snapshot.entries[candidate.documentIndex].objectID == m_pendingRevealID;
            });

            if(row != m_rows.end())
            {
                // Submit the row even outside the clip range before asking ImGui to scroll.
                clipper.IncludeItemByIndex(static_cast<int>(row - m_rows.begin()));
            }
            else
            {
                m_pendingRevealID.clear();
            }
        }

        if(IsRenaming())
        {
            const auto row = std::find_if(m_rows.begin(), m_rows.end(), [&](const auto& candidate)
            {
                return snapshot.entries[candidate.documentIndex].objectID == m_renameObjectID;
            });

            if(row == m_rows.end())
            {
                CancelRename();
            }
            else
            {
                clipper.IncludeItemByIndex(static_cast<int>(row - m_rows.begin()));
            }
        }

        while(clipper.Step())
        {
            for(int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
            {
                const auto row = m_rows[index];
                const auto& entry = snapshot.entries[row.documentIndex];
                ImGui::PushID(entry.objectID.empty() ? std::to_string(row.documentIndex).c_str() : entry.objectID.c_str());
                const auto rowPosition = ImGui::GetCursorScreenPos();
                const float desiredIndent = static_cast<float>((std::min)(row.depth, size_t{32})) * ImGui::GetStyle().IndentSpacing;
                const float indent = (std::min)(desiredIndent, (std::max)(0.0f, ImGui::GetContentRegionAvail().x - 140.0f));
                const bool hasChildren = !m_tree.children[row.documentIndex].empty();
                const bool collapsed = m_collapsed.contains(m_tree.objectIDs[row.documentIndex]);
                const float arrowWidth = ImGui::GetFrameHeight();
                const bool selected = entry.runtimeIndex && (entry.runtimeIndex == selectedObject ||
                    std::find(selectedObjects.begin(), selectedObjects.end(), *entry.runtimeIndex) != selectedObjects.end());

                // m_rows is the flattened expanded tree: collapsed children consume no
                // indices, and clipper skips do not reset parity. Draw beneath all widgets.
                if((index & 1) != 0 && !selected)
                {
                    const ImVec2 minimum(rowPosition.x - style.ItemSpacing.x * 0.5f,
                        rowPosition.y - style.ItemSpacing.y * 0.5f);
                    const ImVec2 maximum(rowPosition.x + ImGui::GetContentRegionAvail().x + style.ItemSpacing.x * 0.5f,
                        minimum.y + ImGui::GetFrameHeightWithSpacing());
                    ImGui::GetWindowDrawList()->AddRectFilled(minimum, maximum, stripeColor);
                }

                ImGui::BeginDisabled(!entry.runtimeIndex);

                if(entry.objectID == m_renameObjectID && IsRenaming())
                {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent + arrowWidth);
                    DrawRenameInput();
                }
                else
                {
                    if(ImGui::Selectable("##Object", selected, ImGuiSelectableFlags_None,
                        ImVec2(0, ImGui::GetFrameHeight())))
                    {
                        if(hasChildren && ImGui::GetIO().MousePos.x >= rowPosition.x + indent &&
                            ImGui::GetIO().MousePos.x < rowPosition.x + indent + arrowWidth)
                        {
                            if(collapsed)
                            {
                                m_collapsed.erase(entry.objectID);
                            }
                            else
                            {
                                m_collapsed.insert(entry.objectID);
                            }

                            m_rowsDirty = true;
                        }
                        else
                        {
                            clickedObject = entry.runtimeIndex;
                        }
                    }

#if defined(ENGINE_HAS_VERIFICATION)
                    m_verificationRows[entry.objectID] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
#endif
                    const auto& io = ImGui::GetIO();

                    if(ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
                        io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < io.MouseDragThreshold * io.MouseDragThreshold)
                    {
                        clickedObject = entry.runtimeIndex;

                        if(!entry.objectID.empty())
                        {
                            m_contextRequest = entry.objectID;
                        }
                    }

                    if(ImGui::IsItemHovered() && !ImGui::GetDragDropPayload())
                    {
                        ImGui::SetTooltip("%s\nDepth: %zu\n%s", entry.name.c_str(), row.depth, entry.objectID.c_str());
                    }

                    if(allowReorder && !IsRenaming() && !entry.objectID.empty())
                    {
                        DrawDropInteraction(&entry, dragIdentity, validateDrop);
                    }

                    auto* draw = ImGui::GetWindowDrawList();
                    const auto color = ImGui::GetColorU32(ImGuiCol_Text);

                    if(hasChildren)
                    {
                        const float x = rowPosition.x + indent + arrowWidth * 0.3f;
                        const float y = rowPosition.y + arrowWidth * 0.35f;
                        const float size = arrowWidth * 0.3f;

                        if(collapsed)
                        {
                            draw->AddTriangleFilled(ImVec2(x, y), ImVec2(x, y + size), ImVec2(x + size, y + size * 0.5f), color);
                        }
                        else
                        {
                            draw->AddTriangleFilled(ImVec2(x, y), ImVec2(x + size, y), ImVec2(x + size * 0.5f, y + size), color);
                        }
                    }

                    const float iconSize = ImGui::GetFontSize();
                    const ImVec2 iconOrigin(rowPosition.x + indent + arrowWidth,
                        rowPosition.y + (ImGui::GetFrameHeight() - iconSize) * 0.5f);
                    if(m_icons)
                    {
                        m_icons->Draw(*draw, iconForObject ? iconForObject(entry.objectID) : EditorIcon::Box,
                            iconOrigin, ImVec2(iconSize, iconSize), color);
                    }
                    else
                    {
                        DrawObjectIcon(*draw, iconOrigin, iconSize);
                    }

                    draw->AddText(ImVec2(rowPosition.x + indent + arrowWidth + iconSize + ImGui::GetStyle().ItemInnerSpacing.x, rowPosition.y), color,
                        entry.name.empty() ? "(Unnamed Object)" : entry.name.c_str());
                }

                if(entry.objectID == m_pendingRevealID && !m_pendingRevealID.empty())
                {
                    const auto* draw = ImGui::GetWindowDrawList();
                    const float top = ImGui::GetItemRectMin().y;
                    const float bottom = ImGui::GetItemRectMax().y;

                    if(top < draw->GetClipRectMin().y)
                    {
                        ImGui::SetScrollHereY(0.0f);
                    }
                    else if(bottom > draw->GetClipRectMax().y)
                    {
                        ImGui::SetScrollHereY(1.0f);
                    }

                    m_pendingRevealID.clear();
                }

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

std::optional<ScenePrimitive> SceneHierarchyPanel::TakePrimitiveRequest()
{
    return std::exchange(m_primitiveRequest, std::nullopt);
}

bool SceneHierarchyPanel::TakeEmptyObjectRequest()
{
    return std::exchange(m_emptyObjectRequest, false);
}

bool SceneHierarchyPanel::TakeCameraObjectRequest()
{
    return std::exchange(m_cameraObjectRequest, false);
}

std::optional<LightType> SceneHierarchyPanel::TakeLightObjectRequest()
{
    return std::exchange(m_lightObjectRequest, std::nullopt);
}
