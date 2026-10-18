#pragma once

#include "EditorWorkspace.h"
#include "HierarchyTreePlanning.h"
#include <functional>
#include <span>
#include "assets/PrimitiveMeshAssets.h"
#include "HierarchyDragPayload.h"
#include "Scene/SceneHierarchySnapshot.h"

class SceneHierarchyPanel
{
public:
    std::optional<ScenePrimitive> TakePrimitiveRequest();

    // True once after the top-level "Empty Object" menu entry was chosen.
    bool TakeEmptyObjectRequest();
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
    std::optional<HierarchyDropRequest> TakeDropRequest();
    using DropValidator = std::function<bool(const HierarchyDropRequest&, std::string&)>;

    // Observe even while hidden; Editor remains the sole selection owner.
    void ObserveSelection(const SceneHierarchySnapshot& snapshot, std::optional<size_t> selectedObject,
        std::uint64_t epoch, const void* sceneIdentity);

    // Returns a click intent only; selection remains owned by Editor.
    std::optional<std::size_t> Draw(const EditorWorkspaceRect& rectangle,
        const SceneHierarchySnapshot& snapshot, std::optional<std::size_t> selectedObject,
        const void* sceneIdentity, const std::string& error,
        const HierarchyDragIdentity& dragIdentity, bool allowReorder, bool allowCreation = false, std::span<const std::size_t> selectedObjects = {}, const DropValidator& validateDrop = {});

private:
    void DrawRenameInput();
    void DrawDropInteraction(const SceneHierarchyEntry* entry, const HierarchyDragIdentity& identity,
        const DropValidator& validateDrop);
    void UpdateTree(const SceneHierarchySnapshot& snapshot, std::uint64_t epoch, const void* sceneIdentity);
    void ExpandPendingSelection();
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
    bool m_emptyObjectRequest = false;
    std::optional<HierarchyDropRequest> m_dropRequest;
    SceneHierarchySnapshot m_treeSnapshot;
    SceneHierarchy::Forest m_tree;
    std::vector<HierarchyTreeRow> m_rows;
    std::unordered_set<std::string> m_collapsed;
    std::uint64_t m_expansionEpoch = 0;
    const void* m_expansionScene = nullptr;
    bool m_rowsDirty = true;
    std::string m_observedSelectionID;
    std::string m_pendingRevealID;
    std::vector<char> m_dragBytes;
    std::string m_dropStatus;
#if defined(ENGINE_HAS_VERIFICATION)
    friend class SceneHierarchyVerification;
    std::unordered_map<std::string, std::pair<ImVec2, ImVec2>> m_verificationRows;
    std::pair<ImVec2, ImVec2> m_verificationRoot;
#endif
};
