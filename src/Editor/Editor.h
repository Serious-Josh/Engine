#pragma once

#include "Scene/Transform.h"
#include "SceneSelection.h"
#include "AssetBrowser.h"
#include "EditorWorkspace.h"
#include "InspectorPanel.h"
#include "SceneHierarchyPanel.h"
#include "Scene/FileSceneAssetCatalog.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>
#include <variant>
#include <memory>

class IDemoScene;
class FileBackedDemoScene;
class Scene;
class SphereGridScene;
class StressTestScene;
class DemoSceneManager;
class DebugUI;
class ProjectPaths;
class ProjectAssetRegistry;

class Editor
{
public:
    enum class FileRequestType
    {
        New,
        Save,
        Load,
        Demo
    };

    struct FileRequest
    {
        FileRequestType type;

        std::filesystem::path path;

        std::string sceneName;
    };

    enum class SceneChangeDecision
    {
        None,
        Save,
        Discard,
        Cancel
    };

    // Normal user-close requests may be deferred until material edits are resolved.
    void ProcessAssetRename(const ProjectPaths& paths, ProjectAssetRegistry& registry,
        const std::filesystem::path& activeScenePath);
    void ProcessAssetDeletion(const ProjectPaths& paths, ProjectAssetRegistry& registry);
    void ProcessAssetMove(const ProjectPaths& paths, ProjectAssetRegistry& registry);
    bool RequestApplicationClose();
    bool TakeApplicationCloseApproval();

    void ImportFileDrop(
        const std::vector<std::filesystem::path>& files,
        float screenX,
        float screenY,
        const ProjectPaths& paths,
        ProjectAssetRegistry& registry);

    bool HasBlockingMaterialOperation() const
    {
        return m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending ||
            m_inspectorPanel.HasUnsavedChanges();
    }

    // The queued creation's own scene-operation UI lock is not a conflict.
    bool HasBlockingObjectCreation() const
    {
        return m_materialLeaveAction != MaterialLeaveAction::None || m_inspectorPanel.HasUnsavedChanges() ||
            m_materialAssignmentPending || m_transformFieldEditing || m_gizmoWasUsing;
    }

    void ConfigureMaterialThumbnails(Renderer& renderer)
    {
        m_materialDropRenderer = &renderer;
        m_assetBrowser.ConfigureMaterialThumbnails(renderer);
        m_inspectorPanel.ConfigureThumbnails(renderer);
    }

    void SetThumbnailCompletedFence(std::uint64_t fence)
    {
        m_assetBrowser.SetThumbnailCompletedFence(fence);
    }

    bool NeedsThumbnailReset() const
    {
        return m_assetBrowser.NeedsThumbnailReset();
    }

    void ResetThumbnailRequests()
    {
        m_assetBrowser.ResetThumbnailRequests();
    }

    const std::optional<MaterialPreviewRequest>& GetMaterialPreviewRequest()
    {
        return m_inspectorPanel.GetPreviewRequest();
    }

    void SetSceneOperationPending(bool pending) noexcept;
    void OnSceneActivated();
    // Interaction cleanup alone does not change the history session or cursor.
    void ResetSceneInteractions();
    std::uint64_t GetSceneHistoryEpoch() const noexcept { return m_historyEpoch; }
    bool CanPreserveSceneHistory(const IDemoScene& scene, const std::filesystem::path& canonicalProject,
        std::uint64_t epoch) const;
    // Caller has checked session compatibility before Renderer publication. No fallible lookup follows.
    void PublishRestoredObjectSelection(const FileBackedDemoScene& scene,
        std::optional<SceneObjectMetadata> selection, std::string activeSceneName) noexcept;
    // Future checked structural publication may explicitly rebind the same editing session.
    // Unsupported creation continues using OnSceneActivated().
    bool RebindSceneHistoryAfterStructureChange(const IDemoScene& scene);
    // Call only after the candidate runtime is published. Persistent identity is
    // resolved before rebinding selection; structural edits clear index history.
    void PublishCreatedObjectSelection(const Scene& scene, SceneObjectMetadata metadata,
        std::string activeSceneName) noexcept;
    struct SceneMaterialSavedRevision
    {
        MaterialSavedRevision saved;

        std::uint64_t catalogIdentity = 0;
        std::uint64_t runtimeIdentity = 0;
        std::uint64_t structureRevision = 0;
    };
    std::vector<SceneMaterialSavedRevision> TakeSavedMaterialRevisions();
    struct SceneMaterialWorkingRevision
    {
        MaterialWorkingRevision working;

        RuntimeMaterialVersions runtime;
    };
    std::vector<SceneMaterialWorkingRevision> TakeWorkingMaterialRevisions();

    struct MaterialAssignmentRequest
    {
        SceneObjectMetadata selection;

        std::string reference;
        std::optional<std::string> slotID;
        std::optional<AssetID> slotMaterial;
    };
    std::optional<MaterialAssignmentRequest> TakeMaterialAssignmentRequest();
    void OnMaterialAssignmentCompleted(const SceneObjectMetadata& selection, bool succeeded, std::string message);

    struct MaterialBindingRequest
    {
        SceneObjectMetadata selection;
        std::optional<AssetID> allSlotsMaterial;
        std::map<std::string, std::string> overrides;
        MeshMetadata layout;
        std::uint64_t operationID = 0;
        std::uint64_t epoch = 0;
    };
    std::optional<MaterialBindingRequest> TakeViewportMaterialRequest();
    bool IsViewportMaterialRequestCurrent(const MaterialBindingRequest& request) const noexcept;
    void NotifyViewportMaterialRetry(const MaterialBindingRequest& request, std::string_view message);
    void CompleteViewportMaterialRequest(const MaterialBindingRequest& request,
        bool succeeded, bool published, std::string_view message) noexcept;
    std::optional<size_t> GetMaterialDropHighlight(const IDemoScene& scene) const;

    struct MeshCreationRequest
    {
        std::string meshReference;
        std::string name;

        Transform transform;

        std::uint64_t runtimeIdentity = 0;
    };
    std::optional<MeshCreationRequest> TakeMeshCreationRequest();
    void OnMeshCreationCompleted(bool succeeded, const std::string& error);

    struct HistoryReplayIdentity
    {
        std::uint64_t operationID = 0;
        std::uint64_t epoch = 0;
        bool redo = false;
        std::optional<std::string> selectionID;
    };
    struct HistoryReplayRequest
    {
        HistoryReplayIdentity identity;
        SceneObjectDocument object;
        size_t insertionIndex = 0;
    };
    std::uint64_t AllocateSceneHistoryOperationID() noexcept { return ++m_historyOperationSerial; }
    bool HasPendingSceneHistoryOperation() const noexcept;
    bool MatchesPendingHistoryReplay(const HistoryReplayIdentity& identity) const noexcept;
    std::optional<HistoryReplayRequest> TakeSceneHistoryReplayRequest();
    void InvalidateSceneHistoryRedoAfterEdit() noexcept;
    bool StageObjectDeletionHistory(SceneObjectDocument object, size_t documentIndex,
        std::optional<std::string> selectionBefore, std::optional<std::string> selectionAfter,
        std::uint64_t operationID, std::uint64_t epoch, bool replay, std::string& error);
    void CompleteObjectDeletionHistory(std::uint64_t operationID, std::uint64_t epoch,
        bool succeeded, const std::string& error);
    void CompleteSceneHistoryReplay(const HistoryReplayIdentity& identity, bool succeeded, const std::string& error);

    std::optional<SceneObjectMetadata> TakeSceneObjectDeletionRequest();
    void OnSceneObjectDeletionCompleted(const IDemoScene& scene, const SceneObjectMetadata& selection,
        bool succeeded, const std::string& error);

    void SuspendViewportInteraction();

    bool IsWorkspaceSplitterDragging() const
    {
        return m_workspace.IsSplitterDragging();
    }

    SceneChangeDecision DrawSceneChangeConfirmation(const std::string& error);

    // Taking a request clears it. Pending actions cannot be overwritten.
    std::optional<FileRequest> TakeFileRequest();
    bool HasFileRequest(FileRequestType type) const noexcept;
    std::optional<AssetBrowser::MaterialCreationRequest> TakeMaterialCreationRequest();
    void OnMaterialCreationCompleted(
        const std::filesystem::path& relativePath,
        const std::string& error);

    const EditorWorkspaceRect& PrepareWorkspace(
        IDemoScene& demoScene,
        DemoSceneManager& sceneManager,
        DebugUI& debugUI);

    void Draw(
        IDemoScene& demoScene,
        DemoSceneManager& sceneManager,
        DebugUI& debugUI,
        std::uint64_t sceneTextureHandle,
        unsigned int sceneWidth,
        unsigned int sceneHeight,
        float deltaTime,
        const ProjectPaths& projectPaths,
        const ProjectAssetRegistry& assetRegistry,
        const std::optional<MaterialPreviewImage>& materialPreview);

    size_t GetSelectedObject() const
    {
        return m_selectedObject;
    }

    const std::vector<size_t>& GetSelectedObjects() const noexcept { return m_selectedObjects; }

    uint64_t GetSelectionStructureRevision() const
    {
        return m_selectionStructureRevision;
    }

private:
    friend class ViewportMaterialDropVerification;
    void DrawMaterialDropTarget(IDemoScene& scene, const ProjectPaths& paths,
        const ProjectAssetRegistry& registry);
    void QueueViewportMaterialDrop(const IDemoScene& scene, const SceneObjectMetadata& target, const AssetID& material);
    Renderer* m_materialDropRenderer = nullptr;
    std::optional<SceneObjectMetadata> m_materialDropHover;
    std::string m_materialDropStatus;
    int m_materialDropDeliveryFrame = -1;
    std::optional<MaterialBindingRequest> m_viewportMaterialRequest;
    void DrawMeshDropTarget(const IDemoScene& scene, const ProjectPaths& paths,
        const ProjectAssetRegistry& registry);
    std::optional<MeshCreationRequest> m_meshCreationRequest;
    std::string m_meshCreationStatus;

    std::uint64_t m_meshDragRuntimeIdentity = 0;
    int m_meshDropDeliveryFrame = -1;

    void DrawHierarchy(IDemoScene& scene, const std::string& activeSceneName);
    void DrawSceneObjectDeletionControls(IDemoScene& scene);
    void DrawSceneObjectContextMenu(IDemoScene& scene);
    void OpenSceneObjectContext(const IDemoScene& scene, const SceneObjectMetadata& target);
    void QueueSceneObjectDeletion(const IDemoScene& scene, const SceneObjectMetadata& selection);
    bool SceneObjectDeletionMatches(const IDemoScene& scene, const SceneObjectMetadata& selection) const;
    bool SceneObjectDeletionBlocked() const;
    void CaptureViewportRightClick(const IDemoScene& scene);
    void FinishViewportRightClick(const IDemoScene& scene);

    std::optional<SceneObjectMetadata> m_viewportRightObject;
    std::optional<SceneObjectMetadata> m_sceneContextObject;
    std::uint64_t m_sceneContextEpoch = 0;
    std::optional<SceneObjectMetadata> m_sceneObjectDeletionRequest;
    std::optional<SceneObjectMetadata> m_sceneObjectDeletionPending;
    std::optional<SceneObjectMetadata> m_sceneObjectDeletionStatusObject;
    std::string m_sceneObjectDeletionStatus;

    bool m_viewportRightGesture = false;
    bool m_viewportRightDragged = false;
    bool m_openSceneContext = false;

    void DrawSceneViewport(
        std::uint64_t sceneTextureHandle,
        unsigned int sceneWidth,
        unsigned int sceneHeight);

    void DrawMainMenu(
        const IDemoScene& demoScene,
        DemoSceneManager& sceneManager,
        DebugUI& debugUI);

    enum class MaterialLeaveAction
    {
        None,
        Selection,
        ObjectSelection,
        Refresh,
        Hide,
        Scene,
        Close
    };

    enum class InspectorTarget { None, Asset, SceneObject };
    InspectorTarget m_inspectorTarget = InspectorTarget::None;

    bool m_objectInspectionRequested = false;
    struct ObjectInspectionIdentity
    {
        const Scene* scene = nullptr; // Identity only; never dereferenced after scene replacement.
        std::uint64_t revision = 0;
        std::uint64_t runtimeIdentity = 0;

        std::string objectID;
        std::string sceneID;
    };
    std::optional<ObjectInspectionIdentity> m_objectInspectionIdentity;

    std::vector<SceneMaterialSavedRevision> m_savedMaterialRevisions;

    std::vector<SceneMaterialWorkingRevision> m_workingMaterialRevisions;

    std::optional<MaterialAssignmentRequest> m_materialAssignmentRequest;

    std::optional<SceneObjectMetadata> m_materialAssignmentSelection;
    std::optional<std::string> m_materialAssignmentSlotID;

    bool m_materialAssignmentPending = false;

    std::string m_materialAssignmentStatus;

    void ActivateObjectInspection(IDemoScene& demoScene, const ProjectPaths& paths, const ProjectAssetRegistry& registry);
    std::optional<SceneObjectInspectorView> ResolveObjectInspection(
        const IDemoScene& demoScene, const ProjectPaths& paths, const ProjectAssetRegistry& registry) const;

    MaterialLeaveAction m_materialLeaveAction = MaterialLeaveAction::None;

    std::filesystem::path m_inspectedAssetSelection;
    std::filesystem::path m_pendingAssetSelection;

    bool m_applicationCloseApproved = false;

    std::optional<FileRequest> m_fileRequest;

    bool m_sceneOperationPending = false;
    bool m_showEditorPanel = false;

    AssetBrowser m_assetBrowser;

    EditorWorkspace m_workspace;
    SceneHierarchyPanel m_hierarchyPanel;

    InspectorPanel m_inspectorPanel;

    static constexpr size_t InvalidObjectSelection = (std::numeric_limits<size_t>::max)();

    enum class GizmoMode
    {
        Translate,
        Rotate,
        Scale
    };

    GizmoMode m_gizmoMode = GizmoMode::Translate;

    bool m_cameraLooking = false;
    bool m_cameraPanning = false;
    // Keep a pan's held left button from becoming a gizmo gesture after RMB release.
    bool m_panLeftHeld = false;
    bool m_viewportHovered = false;
    bool m_viewportFocused = false;

    EditorWorkspaceRect m_viewportImage;

    float m_cameraYaw = 0.0f;
    float m_cameraPitch = 0.0f;

    struct PersistentHistoryTarget { std::string objectID; };
    struct ProceduralHistoryTarget { size_t objectIndex = InvalidObjectSelection; };
    using HistoryTarget = std::variant<PersistentHistoryTarget, ProceduralHistoryTarget>;
    struct TransformEdit
    {
        HistoryTarget target;
        Transform before;
        Transform after;
    };
    struct GroupTransformEdit
    {
        std::vector<TransformEdit> edits;
        GizmoMode mode = GizmoMode::Translate;
    };
    using SelectedObjectIdentity = SceneSelection::Identity;
    std::vector<SelectedObjectIdentity> m_orderedSelection;
    std::vector<size_t> m_selectedObjects;
    void SelectSceneObject(const IDemoScene& scene, size_t index, bool toggle = false);
    void RefreshSceneSelection(const IDemoScene& scene);
    void UpdateGroupGizmo(IDemoScene& scene, bool usingGizmo, const Transform& anchorAfter);
    std::unique_ptr<GroupTransformEdit> m_gizmoGroupEdit;
    size_t m_gizmoGroupPayloadBytes = 0;
    struct SceneHistorySession
    {
        std::filesystem::path projectIdentity;
        std::filesystem::path scenePath;
        std::string sceneIdentity;
        std::uint64_t epoch = 0;
    };
    struct ObjectDeletion
    {
        SceneObjectDocument object;
        size_t documentIndex = 0;
        std::optional<std::string> selectionBefore;
        std::optional<std::string> selectionAfter;
        SceneHistorySession session;
    };
    struct RenameObject
    {
        SceneHistorySession session;
        std::string objectID;
        std::string before;
        std::string after;
    };
    struct ReorderObjects
    {
        SceneHistorySession session;
        std::vector<std::string> before;
        std::vector<std::string> after;
    };
    struct MaterialBindingsEdit
    {
        SceneHistorySession session;
        std::string objectID;
        std::string meshReference;
        std::string fallbackReference;
        MeshMetadata layout;
        std::map<std::string, std::string> before;
        std::map<std::string, std::string> after;
    };
    void CommitObjectReorder(IDemoScene& scene, const HierarchyReorderRequest& request);
    std::string m_reorderStatus;

    struct RenameIntent
    {
        SceneHistorySession session;
        SceneObjectMetadata target;
    };
    std::optional<RenameIntent> m_renameIntent;
    std::string m_renameStatus;

    void QueueObjectRename(const IDemoScene& scene, const SceneObjectMetadata& target);
    void CancelObjectRename();
    void CommitObjectRename(IDemoScene& scene, std::string name);
    bool RenameIntentMatches(const IDemoScene& scene) const;
    bool RenameSessionMatches(const IDemoScene& scene, const SceneHistorySession& session) const;
    using SceneHistoryCommand = std::variant<TransformEdit, std::unique_ptr<GroupTransformEdit>, std::unique_ptr<ObjectDeletion>, std::unique_ptr<RenameObject>, std::unique_ptr<ReorderObjects>, std::unique_ptr<MaterialBindingsEdit>>;
    struct SceneHistoryEntry
    {
        SceneHistoryCommand command;
        std::uint64_t sessionEpoch = 0;
        size_t payloadBytes = 0;
    };
    struct PendingHistoryReplay
    {
        HistoryReplayIdentity identity;
        size_t cursor = 0;
    };
    struct PendingMaterialBinding
    {
        std::uint64_t operationID = 0;
        std::uint64_t epoch = 0;
        size_t cursor = 0;
        std::optional<bool> redo;
        std::optional<SceneHistoryEntry> forward;
    };
    std::optional<PendingMaterialBinding> m_pendingMaterialBinding;
    void QueueMaterialBindingReplay(IDemoScene& scene, const MaterialBindingsEdit& edit, bool redo);
    static size_t MaterialBindingPayloadBytes(const MaterialBindingsEdit& edit);
    struct StagedDeletionHistory
    {
        SceneHistoryEntry entry;
        std::uint64_t operationID = 0;
    };
    std::optional<PendingHistoryReplay> m_pendingHistoryReplay;
    std::optional<HistoryReplayRequest> m_historyReplayRequest;
    std::optional<StagedDeletionHistory> m_stagedDeletionHistory;
    std::uint64_t m_historyOperationSerial = 0;
    void AppendSceneHistoryEntry(SceneHistoryEntry entry) noexcept;
    static size_t ObjectDeletionPayloadBytes(const ObjectDeletion& deletion);

    static constexpr size_t kSceneHistoryCapacity = 256;
    static constexpr size_t kSceneHistoryPayloadBytes = 4 * 1024 * 1024;
    std::vector<SceneHistoryEntry> m_sceneHistory;
    std::optional<SceneHistorySession> m_historySession;
    std::optional<HistoryTarget> m_gizmoHistoryTarget;
    std::optional<HistoryTarget> m_fieldHistoryTarget;
    std::string m_sceneHistoryStatus;

    size_t m_historyCursor = 0;
    size_t m_historyPayloadBytes = 0;
    std::uint64_t m_historyEpoch = 0;
    std::uint64_t m_historyRuntimeIdentity = 0;
    std::uint64_t m_historyStructureRevision = 0;
    std::uint64_t m_gizmoHistoryEpoch = 0;
    std::uint64_t m_fieldHistoryEpoch = 0;
    // Identity-only borrow outside command entries; never dereferenced after scene replacement.
    const Scene* m_historyObservedScene = nullptr;

    bool UpdateSceneHistorySession(const IDemoScene& scene, const ProjectPaths& paths, const std::string& name);
    std::optional<HistoryTarget> CaptureHistoryTarget(const IDemoScene& scene, size_t index);
    std::optional<size_t> ResolveHistoryTarget(const IDemoScene& scene, const HistoryTarget& target);
    void RecordTransformEdit(HistoryTarget target, std::uint64_t epoch, const Transform& before, const Transform& after);
    void TruncateHistoryRedo();
    void ReplaySceneHistory(IDemoScene& scene, bool redo);
    bool HistoryReplayBlocked() const;
    void ReportSceneHistoryFailure(const std::string& error);
    void ResetTransformGestures();
    void ClearSceneHistory();

    bool m_gizmoWasUsing = false;

    size_t m_gizmoUndoObject = InvalidObjectSelection;

    Transform m_gizmoUndoStart;

    bool m_transformFieldEditing = false;

    size_t m_transformFieldUndoObject = InvalidObjectSelection;

    Transform m_transformFieldUndoStart;
    const Scene* m_transformFieldScene = nullptr;
    std::uint64_t m_transformFieldRevision = 0;
    void DrawSelectedObjectTransformControls(IDemoScene& demoScene);
    void FinishTransformFieldEdit(IDemoScene& demoScene);

    void DrawTransformGizmo(
        IDemoScene& demoScene,
        float viewportWidth,
        float viewportHeight);
    void HandleViewportSelection(
        IDemoScene& demoScene,
        float viewportWidth,
        float viewportHeight);
    void HandleTransformUndo(IDemoScene& demoScene);
    void HandleTransformRedo(IDemoScene& demoScene);
    void UpdateCameraLook(
        IDemoScene& demoScene,
        float viewportWidth,
        float viewportHeight);
    void UpdateCameraMovement(
        IDemoScene& demoScene,
        float deltaTime);
    void DrawObjectControls(IDemoScene& demoScene);
    void DrawStressTestControls(StressTestScene& demoScene);
    void DrawSphereGridControls(SphereGridScene& demoScene);
    void DrawDirectionalLight(IDemoScene& demoScene);
    void DrawCamera(IDemoScene& demoScene);

    size_t m_selectedObject = InvalidObjectSelection;

    uint64_t m_selectionStructureRevision = 0;

    std::string m_selectionSceneName;
    const Scene* m_selectionScene = nullptr;
    std::uint64_t m_selectionRuntimeIdentity = 0;
};
