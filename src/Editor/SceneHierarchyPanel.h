#pragma once

#include "EditorWorkspace.h"
#include <span>
#include "assets/PrimitiveMeshAssets.h"
#include "HierarchyDragPayload.h"
#include "Scene/SceneHierarchySnapshot.h"

class SceneHierarchyPanel
{
public:
    std::optional<ScenePrimitive> TakePrimitiveRequest();
    bool IsFocused() const noexcept { return m_focused; }
    void BeginRename(const std::string& objectID, const std::string& name);
    void CancelRename();
    bool IsRenaming() const
    {
        return !m_renameObjectID.empty();
    }

    std::optional<std::string> TakeRenameCommit();
    std::optional<std::string> TakeContextRequest();
    std::optional<HierarchyReorderRequest> TakeReorderRequest();

    // Returns a click intent only; selection remains owned by Editor.
    std::optional<std::size_t> Draw(const EditorWorkspaceRect& rectangle,
        const SceneHierarchySnapshot& snapshot, std::optional<std::size_t> selectedObject,
        const void* sceneIdentity, const std::string& error,
        const HierarchyDragIdentity& dragIdentity, bool allowReorder, bool allowCreation = false, std::span<const std::size_t> selectedObjects = {});

private:
    void DrawRenameInput();
    void DrawReorderInteraction(const SceneHierarchyEntry& entry, const SceneHierarchySnapshot& snapshot,
        const HierarchyDragIdentity& identity);
    void AutoScrollDrag(const HierarchyDragIdentity& identity, float listTop);
    static int ResizeRenameBuffer(ImGuiInputTextCallbackData* data);

    std::string m_renameObjectID;
    std::vector<char> m_renameBuffer;
    std::optional<std::string> m_renameCommit;
    std::optional<std::string> m_contextRequest;
    std::optional<HierarchyReorderRequest> m_reorderRequest;
    bool m_focusRename = false;
    bool m_focused = false;
    std::optional<ScenePrimitive> m_primitiveRequest;
};
