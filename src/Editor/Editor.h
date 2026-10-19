#pragma once
#include "Scene/SceneHierarchyEdits.h"

#include "Scene/Transform.h"
#include "Engine/BuildGame.h"
#include "Platform/Windows/ProjectFileAssociation.h"
#include "Engine/CharacterDriveScheduling.h"
#include "Physics/CharacterControllerDiagnostics.h"
#include "SceneSelection.h"
#include "ViewportMarquee.h"
#include "AssetBrowser.h"
#include "EditorWorkspace.h"
#include "InspectorPanel.h"
#include "SceneHierarchyPanel.h"
#include "ViewportGridSnap.h"
#include "EditorPreferences.h"
#include "Scene/FileSceneAssetCatalog.h"
#include "Project/ProjectContext.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>
#include <variant>
#include <memory>
#include <utility>

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
        Demo,
        SaveAs,
        NewProject,
        OpenProject,
        ProjectSettings,
        Exit
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
            m_materialAssignmentPending || m_transformFieldEditing || m_gizmoWasUsing || m_marqueeGesture.has_value();
    }

    // Also binds the Engine-owned active project, which outlives the Editor's use of it.
    void ConfigureMaterialThumbnails(Renderer& renderer, const ProjectContext& activeProject)
    {
        m_activeProject = &activeProject;
        m_materialDropRenderer = &renderer;
        m_assetBrowser.ConfigureMaterialThumbnails(renderer, activeProject.GetPaths());
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

    bool TakeScriptRefreshRequest()
    {
        return m_assetBrowser.TakeScriptRefreshRequest();
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
    enum class PhysicsState { Stopped, Playing, Stopping };
    enum class PhysicsRequest { Play, Stop };
    void SetPhysicsStatus(PhysicsState state, bool canPlay, std::string message);
    std::optional<PhysicsRequest> TakePhysicsRequest();
    bool CanStartPhysics() const;
    bool ArePhysicsCollidersVisible() const noexcept { return m_showPhysicsColliders; }
    // G.3 viewport grid spacing for this frame: the applied translation snap interval, or empty
    // when the Grid overlay is off or Play mode has locked authored editing.
    std::optional<float> GetViewportGridSpacing() const;

    // Snap and grid values persisted in global Editor preferences (PR.5). Apply accepts only
    // valid intervals (invalid ones keep the current value) and also resets the text drafts.
    ViewportPreferences GetViewportPreferences() const;
    void ApplyViewportPreferences(const ViewportPreferences& preferences);
    // CM.9 diagnostics. Engine supplies the published controller diagnostics after each
    // physics frame (null when no session is active), and takes the diagnostic drive intent
    // immediately before the next Update; it acknowledges a jump once it is submitted.
    void SetCharacterDiagnostics(const Physics::CharacterDiagnosticsFrame* frame);
    std::optional<CharacterDriveIntent> TakeCharacterDriveIntent() const;
    void AcknowledgeCharacterDriveJump();
    bool IsPhysicsEditingLocked() const noexcept { return m_physicsState != PhysicsState::Stopped; }
    void OnSceneActivated();
    // Interaction cleanup alone does not change the history session or cursor.
    void ResetSceneInteractions();
    std::uint64_t GetSceneHistoryEpoch() const noexcept { return m_historyEpoch; }
    bool CanPreserveSceneHistory(const IDemoScene& scene, const std::filesystem::path& canonicalProject,
        std::uint64_t epoch) const;
    // Caller has checked session compatibility before Renderer publication. No fallible lookup follows.
    void PublishRestoredObjectSelection(const FileBackedDemoScene& scene,
        std::optional<SceneObjectMetadata> selection, std::string activeSceneName, bool retainGroup = true) noexcept;
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

    struct PrimitiveCreationRequest
    {
        ScenePrimitive primitive;
        std::uint64_t runtimeIdentity = 0;
        std::uint64_t historyEpoch = 0;
    };
    std::optional<PrimitiveCreationRequest> TakePrimitiveCreationRequest();

    // Hierarchy "Empty Object": a meshless, materialless object created through the same
    // checked membership and history path as primitives.
    struct EmptyObjectCreationRequest
    {
        std::uint64_t runtimeIdentity = 0;
        std::uint64_t historyEpoch = 0;
    };
    std::optional<EmptyObjectCreationRequest> TakeEmptyObjectCreationRequest();
    void OnEmptyObjectCreationFailed(const std::string& error);
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
        std::optional<std::string> bindingSourceID;
        bool membership = false;
        bool remove = false;
        std::optional<SceneHierarchyEdit> hierarchy;
    };
    std::optional<HistoryReplayRequest> TakeSceneDuplicationRequest();
    void HandleSceneObjectDuplication(IDemoScene&);
    bool QueueSceneObjectDuplication(IDemoScene& scene, const SceneObjectMetadata& target, std::string& error);
    bool PrepareHierarchyDrop(IDemoScene& scene, const HierarchyDropRequest& request,
        SceneHierarchyEdit& edit, std::string& error);
    bool SubmitHierarchyDrop(IDemoScene& scene, const HierarchyDropRequest& request, std::string& error);
    bool QueueHierarchyEmpty(IDemoScene& scene, std::string_view parentID, std::string& error);
    void ProcessMaterialLocateRequest(const ProjectPaths&,const ProjectAssetRegistry&);
    std::uint64_t AllocateSceneHistoryOperationID() noexcept { return ++m_historyOperationSerial; }
    bool HasPendingSceneHistoryOperation() const noexcept;
    bool MatchesPendingHistoryReplay(const HistoryReplayIdentity& identity) const noexcept;
    std::optional<HistoryReplayRequest> TakeSceneHistoryReplayRequest();
    void InvalidateSceneHistoryRedoAfterEdit() noexcept;
    bool StageObjectDeletionHistory(SceneObjectDocument object, size_t documentIndex,
        std::optional<std::string> selectionBefore, std::optional<std::string> selectionAfter,
        std::uint64_t operationID, std::uint64_t epoch, bool replay, std::string& error, bool insertion = false);
    bool StageHierarchyHistory(const SceneHierarchyEdit& edit,
        std::optional<std::string> selectionBefore, std::optional<std::string> selectionAfter,
        std::uint64_t operationID, std::string& error);
    // H.5 backend seam; tree UI delivery belongs to H.6.
    bool QueueHierarchyReparent(IDemoScene& scene, std::string_view objectID, std::string_view parentID,
        bool keepWorld, std::string_view beforeSibling, std::string& error);
    bool QueueHierarchyCreation(IDemoScene& scene, SceneObjectDocument object, std::string_view parentID,
        std::string& error);
    bool QueueHierarchyEdit(IDemoScene& scene, SceneHierarchyEdit edit,
        std::optional<std::string> selectionAfter, std::string& error);
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

    struct BuildGameRequest
    {
        BuildGame::Settings settings;

        // Explicit "Save Scene and Build": save the active startup scene through File > Save first.
        bool saveStartupScene = false;
    };

    // Taking a request clears it. Pending actions cannot be overwritten.
    std::optional<BuildGameRequest> TakeBuildGameRequest()
    {
        return std::exchange(m_buildGameRequest, std::nullopt);
    }

    bool TakeBuildGameCancel() { return std::exchange(m_buildGameCancel,false); }
    void SetBuildGameStatus(BuildGame::Status status) { m_buildGameStatus=std::move(status); }

    // Offers "Save Scene and Build" after a build was refused for an unsaved active startup scene.
    void SetBuildStartupSceneUnsaved(bool unsaved)
    {
        m_buildStartupSceneUnsaved = unsaved;
    }

    // Project Settings request for the shared game startup scene: an absolute scene path, or an
    // empty path to clear the setting. Engine validates and persists it, then reports the result.
    std::optional<std::filesystem::path> TakeStartupSceneRequest()
    {
        return std::exchange(m_startupSceneRequest, std::nullopt);
    }

    void OnStartupSceneChanged(bool succeeded, std::string message);

    // Blocking prompt while project manifest ownership cannot be verified (Engine fail-closed state).
    enum class OwnershipAction
    {
        None,
        Retry,
        Close
    };

    void SetProjectOwnershipLost(std::string message)
    {
        m_projectOwnershipMessage = std::move(message);
    }

    OwnershipAction TakeOwnershipAction()
    {
        return std::exchange(m_ownershipAction, OwnershipAction::None);
    }
    std::optional<FileRequest> TakeFileRequest();
    bool HasFileRequest(FileRequestType type) const noexcept;
    bool QueueFileRequest(FileRequest request);
    void InvalidateSceneFiles()
    {
        m_assetBrowser.InvalidateFileListings();
    }
    void ReportLifecycleMessage(std::string message);
    bool RebindSceneHistoryAfterSaveAs(const FileBackedDemoScene& scene, const std::filesystem::path& previousPath);
    void DrawMaterialLeaveConfirmation(IDemoScene& scene, const ProjectPaths& projectPaths,
        const ProjectAssetRegistry& assetRegistry);
    std::optional<AssetBrowser::MaterialCreationRequest> TakeMaterialCreationRequest();
    std::optional<AssetBrowser::MeshImportUnitsRequest> TakeMeshImportUnitsRequest()
    {
        return m_assetBrowser.TakeMeshImportUnitsRequest();
    }

    void OnMeshImportUnitsCompleted(const std::string& error)
    {
        m_assetBrowser.OnMeshImportUnitsCompleted(error);
    }
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

    void SetViewportPublicationIdentity(std::uint64_t rendererLifetime, std::uint64_t bindingEpoch);
    void RefreshViewportMarqueePreview(const IDemoScene& scene);
    const std::vector<std::size_t>* GetViewportMarqueePreview(const IDemoScene& scene) const;
    const std::vector<size_t>& GetSelectedObjects() const noexcept { return m_selectedObjects; }

    uint64_t GetSelectionStructureRevision() const
    {
        return m_selectionStructureRevision;
    }

private:
#if defined(ENGINE_HAS_VERIFICATION)
    friend class PortableEditorVerification;
    friend class PresentationTransformVerification;
#endif
    PhysicsState m_physicsState = PhysicsState::Stopped;
    bool m_canPlayPhysics = false;
    std::string m_physicsMessage;
    std::optional<PhysicsRequest> m_physicsRequest;
    void DrawPhysicsControls();
#if defined(ENGINE_HAS_VERIFICATION)
    friend class MaterialLocateVerification;
    friend class ViewportPanVerification;
    friend class ViewportGridSnapVerification;
    friend class EditorGridVerification;
    friend class ProjectIndependentStartupVerification;
    friend class SceneLifecycleVerification;
    friend class AssetBrowserSceneVerification;
    friend class StartupSceneVerification;
    friend class ProjectSystemWorkflowVerification;
    friend class ProjectRelaunchVerification;
    friend class SceneDuplicationVerification;
    friend class ScenePhysicsDataVerification;
    friend class PrimitiveCreationVerification;
    friend class MarqueeSelectionVerification;
    friend class ViewportMaterialDropVerification;
    friend class DescriptorRecoveryVerification;
    friend class SceneHierarchyVerification;
#endif
    void DrawMaterialDropTarget(IDemoScene& scene, const ProjectPaths& paths,
        const ProjectAssetRegistry& registry);
    void QueueViewportMaterialDrop(const IDemoScene& scene, const SceneObjectMetadata& target, const AssetID& material);
    Renderer* m_materialDropRenderer = nullptr;

    // Borrowed from Engine; null until ConfigureMaterialThumbnails binds it.
    const ProjectContext* m_activeProject = nullptr;
    std::optional<SceneObjectMetadata> m_materialDropHover;
    std::string m_materialDropStatus;
    int m_materialDropDeliveryFrame = -1;
    std::optional<MaterialBindingRequest> m_viewportMaterialRequest;
    void DrawMeshDropTarget(const IDemoScene& scene, const ProjectPaths& paths,
        const ProjectAssetRegistry& registry);
    std::optional<MeshCreationRequest> m_meshCreationRequest;
    std::optional<PrimitiveCreationRequest> m_primitiveCreationRequest;
    std::optional<EmptyObjectCreationRequest> m_emptyObjectCreationRequest;
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

    bool m_showBuildGame=false, m_buildAdvanced=false, m_buildGameCancel=false;
    char m_buildTitle[128]="MyGame";
    char m_buildOutput[1024]="";
    BuildGame::Status m_buildGameStatus;
    std::optional<BuildGameRequest> m_buildGameRequest;
    bool m_buildStartupSceneUnsaved = false;
    void DrawBuildGame(const IDemoScene& demoScene);
    std::optional<FileRequest> m_fileRequest;

    bool m_sceneOperationPending = false;
    bool m_showProjectSettings = false;
    std::string m_lifecycleMessage;
    void DrawLifecycleWindows(const IDemoScene& demoScene);

    // Project Settings: shared game startup scene. Candidates are listed from the Scenes directory
    // when the window opens or on Refresh, never per frame and without reading scene documents.
    std::optional<std::filesystem::path> m_startupSceneRequest;
    std::vector<std::filesystem::path> m_startupSceneCandidates;
    bool m_startupSceneCandidatesListed = false;
    std::string m_projectSettingsMessage;
    bool m_projectSettingsMessageIsError = false;
    void DrawProjectSettings(const IDemoScene& demoScene);
    void ListStartupSceneCandidates();

    // Windows .engineproj association: a personal Windows preference, separate from the project.
    bool m_showFileAssociation = false;
    bool m_fileAssociationStatusRead = false;
    bool m_fileAssociationConfirmReplace = false;
    bool m_fileAssociationPortableAcknowledged = false;
    ProjectFileAssociation::Status m_fileAssociationStatus;
    std::filesystem::path m_fileAssociationEffectiveExecutable;
    std::string m_fileAssociationMessage;
    void DrawFileAssociation();
    void RefreshFileAssociationStatus();

    std::string m_projectOwnershipMessage;
    OwnershipAction m_ownershipAction = OwnershipAction::None;
    void DrawProjectOwnershipLost();
    ProjectFileAssociation::RegistryLocation FileAssociationLocation() const;
#if defined(ENGINE_HAS_VERIFICATION)
    // Diagnostics drive the real window against an isolated registry key, never the user's classes.
    std::optional<ProjectFileAssociation::RegistryLocation> m_fileAssociationLocationForVerification;
#endif
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
    // Session display preference: never reset with scene interactions or authored history.
    bool m_showPhysicsColliders = true;
    // Display preference, independent of Snap; default visible. Persisted in Editor preferences.
    bool m_showViewportGrid = true;
    // Gizmo snapping (G.1 translation, G.2 rotation). One master toggle drives both modes; each
    // has its own interval. Persisted only in global Editor preferences (PR.5), never in scenes,
    // cooked payloads or history.
    bool m_snapEnabled = false;
    float m_translationSnapInterval = ViewportGridSnap::kDefaultInterval;
    float m_rotationSnapIntervalDegrees = ViewportGridSnap::kDefaultRotationIntervalDegrees;
    // Text-entry values; only a finite positive draft replaces the applied interval.
    float m_translationSnapIntervalDraft = ViewportGridSnap::kDefaultInterval;
    float m_rotationSnapIntervalDraft = ViewportGridSnap::kDefaultRotationIntervalDegrees;
    EditorWorkspaceRect m_viewportContentRegion;
    void DrawSceneViewportToolbar(const EditorWorkspaceRect& toolbarRectangle);
    void DrawGridSnapControls(float rightEdge);
    Vector3 SnapGizmoTranslation(const Vector3& dragStart, const Vector3& continuousPosition) const;
    bool IsRotationSnapActive() const;
    void UpdateGizmoRotationDrag(bool gizmoUsing, const Matrix4& gizmoModel, const Vector3& authoredRotation);

    // Transient state of one gizmo rotation drag. ImGuizmo rotates incrementally from the
    // matrix it receives, so during a drag it is fed continuousModel (its own previous output,
    // ImGuizmo layout) rather than the snapped authored rotation. Cleared whenever the gesture
    // ends or is reset; startRotation is the authored drag-start rotation of the active object.
    struct GizmoRotationDrag
    {
        Matrix4 continuousModel;
        Vector3 startRotation;
        ViewportGridSnap::RotationDrag tracker;
    };
    std::optional<GizmoRotationDrag> m_gizmoRotationDrag;
    // Euler rotation for the active object this frame: snapped when snapping is on, otherwise
    // the existing decomposition of the continuous gizmo output.
    Vector3 ResolveGizmoRotation(const Vector3& continuousRotationRadians) const;
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
    std::optional<ViewportMarquee::Gesture> m_marqueeGesture;
    std::uint64_t m_viewportRendererLifetime = 0;
    std::uint64_t m_viewportBindingEpoch = 0;
    void CancelViewportMarquee();
    bool ViewportMarqueeMatches(const IDemoScene& scene) const;
    void StartViewportMarquee(const IDemoScene& scene, ImVec2 imageMouse);
    bool UpdateViewportMarquee(IDemoScene& scene);
    void DrawViewportMarquee() const;
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
        bool insertion = false;
        std::optional<SceneHierarchyEdit> hierarchy;
    };
    struct PhysicsComponentEdit
    {
        SceneHistorySession session;
        std::string objectID;
        std::optional<Physics::PhysicsComponent> before;
        std::optional<Physics::PhysicsComponent> after;
    };
    void DrawObjectPhysicsControls(IDemoScene& scene);
    bool CommitObjectPhysics(IDemoScene& scene, const SceneObjectMetadata& selection,
        const std::optional<Physics::PhysicsComponent>& before,
        const std::optional<Physics::PhysicsComponent>& after);
    std::optional<SceneObjectMetadata> m_physicsInspection;
    std::optional<Physics::PhysicsComponent> m_physicsInspectedValue;
    std::uint64_t m_physicsInspectionEpoch = 0;
    // One drag draft per numeric physics field, in DrawObjectPhysicsControls' field order.
    // Drafts hold displayed units (degrees for rotation) and reach the scene only on commit.
    std::array<float, 15> m_physicsDragDrafts{};
    std::string m_physicsEditStatus;
    struct TriggerColliderEdit
    {
        SceneHistorySession session;
        std::string objectID;
        std::optional<Physics::TriggerColliderComponent> before;
        std::optional<Physics::TriggerColliderComponent> after;
    };
    void DrawObjectTriggerColliderControls(IDemoScene& scene);
    bool CommitObjectTriggerCollider(IDemoScene& scene, const SceneObjectMetadata& selection,
        const std::optional<Physics::TriggerColliderComponent>& before,
        const std::optional<Physics::TriggerColliderComponent>& after);
    std::optional<SceneObjectMetadata> m_triggerInspection;
    std::optional<Physics::TriggerColliderComponent> m_triggerInspectedValue;
    std::uint64_t m_triggerInspectionEpoch = 0;
    // Displayed units, committed only when the existing numeric edit transaction completes.
    std::array<float, 12> m_triggerDragDrafts{};
    std::string m_triggerEditStatus;
    std::uint64_t m_triggerControlsRevision = 0;
    struct CharacterControllerEdit
    {
        SceneHistorySession session;
        std::string objectID;
        std::optional<Physics::CharacterControllerComponent> before;
        std::optional<Physics::CharacterControllerComponent> after;
    };
    void DrawObjectCharacterControllerControls(IDemoScene& scene);
    bool CommitObjectCharacterController(IDemoScene& scene, const SceneObjectMetadata& selection,
        const std::optional<Physics::CharacterControllerComponent>& before,
        const std::optional<Physics::CharacterControllerComponent>& after);
    std::optional<SceneObjectMetadata> m_controllerInspection;
    std::optional<Physics::CharacterControllerComponent> m_controllerInspectedValue;
    std::uint64_t m_controllerInspectionEpoch = 0;
    // Displayed units, committed only when the existing numeric edit transaction completes.
    std::array<float, 11> m_controllerDragDrafts{};
    std::string m_controllerEditStatus;
    std::uint64_t m_controllerControlsRevision = 0;
    // CM.9: read-only runtime diagnostics and the explicitly activated diagnostic drive.
    bool ViewportOwnsKeyboard() const;
    void DisableCharacterDrive(std::string reason);
    void UpdateCharacterDriveInput(const IDemoScene& scene);
    void DrawCharacterDriveToggle();
    void DrawCharacterControllerDiagnostics(const SceneObjectMetadata& selection,
        const Physics::CharacterControllerComponent& component);
    struct CharacterDriveCandidate
    {
        std::string objectID;
        float maximumSpeed = 0.0f;
    };
    struct CharacterDrive
    {
        std::string objectID;
        std::uint64_t sessionGeneration = 0;
        float maximumSpeed = 0.0f;
        bool jumpPending = false;
        Vector3 velocity;
    };
    // Copy of the latest published frame; never a reference into the simulation.
    std::optional<Physics::CharacterDiagnosticsFrame> m_characterDiagnostics;
    // Controller shown by the Inspector this frame; the Play-bar toggle reads it next frame.
    std::optional<CharacterDriveCandidate> m_characterDriveCandidate;
    std::optional<CharacterDrive> m_characterDrive;
    std::string m_characterDriveStatus;
    struct ComponentPopupTarget
    {
        SceneHistorySession session;
        SceneObjectMetadata selection;
        std::uint64_t inspectionEpoch = 0;
        ImGuiID popupID = 0;
    };
    static void CloseComponentPopup(ImGuiID popupID);
    // Top-left screen position for the Add Component popup: centered on the content area,
    // directly below the button when it fits in the work area, otherwise directly above it.
    static ImVec2 CalculateAddComponentPopupPosition(ImVec2 buttonMinimum, ImVec2 buttonMaximum,
        float contentCenterX, ImVec2 popupSize, ImVec2 workMinimum, ImVec2 workMaximum, float gap);
    void DrawAddComponentControls(IDemoScene& scene);
    std::optional<SceneObjectMetadata> ResolveComponentEditTarget(IDemoScene& scene);
    void ValidateAddComponentPopup(IDemoScene& scene);
    void DismissAddComponentPopup();
    bool ComponentPopupMatches(IDemoScene& scene, const ComponentPopupTarget& target);
    void ValidateTriggerManagementPopup(IDemoScene& scene);
    void DismissTriggerManagementPopup();
    void ValidateControllerManagementPopup(IDemoScene& scene);
    void DismissControllerManagementPopup();
    bool ApplyObjectTransformEdit(IDemoScene& scene, RenderObjectID index, const Transform& transform);
    bool ApplyObjectTransformEdits(IDemoScene& scene, std::span<const RenderObjectTransformUpdate> updates);
    std::string m_transformEditStatus;
    std::optional<ComponentPopupTarget> m_addComponentPopup;
    std::optional<ComponentPopupTarget> m_triggerManagementPopup;
    std::optional<ComponentPopupTarget> m_controllerManagementPopup;
    std::array<char, 128> m_componentSearch{};
    bool m_focusComponentSearch = false;
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
    using SceneHistoryCommand = std::variant<std::unique_ptr<PhysicsComponentEdit>,
        std::unique_ptr<TriggerColliderEdit>, std::unique_ptr<CharacterControllerEdit>, TransformEdit, std::unique_ptr<GroupTransformEdit>,
        std::unique_ptr<ObjectDeletion>, std::unique_ptr<RenameObject>, std::unique_ptr<ReorderObjects>,
        std::unique_ptr<MaterialBindingsEdit>>;
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
    std::optional<HistoryReplayRequest> m_duplicationRequest;
    std::optional<StagedDeletionHistory> m_stagedDeletionHistory;
    std::uint64_t m_historyOperationSerial = 0;
    void AppendSceneHistoryEntry(SceneHistoryEntry entry) noexcept;
    static size_t ObjectDeletionPayloadBytes(const ObjectDeletion& deletion);

    static constexpr size_t kSceneHistoryCapacity = 256;
    static constexpr size_t kSceneHistoryPayloadBytes = 4 * 1024 * 1024;
    std::vector<SceneHistoryEntry> m_sceneHistory;
    std::optional<SceneHistorySession> m_historySession;
    // History entries keep their original path token; only this authorized current location
    // follows a successful Save As. Arbitrary path changes still invalidate the session.
    std::filesystem::path m_historyCurrentScenePath;
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
        float viewportHeight,
        bool viewportWasFocused = true);
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
