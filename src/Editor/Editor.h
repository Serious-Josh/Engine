#pragma once

#include "Scene/Transform.h"
#include "AssetBrowser.h"
#include "EditorWorkspace.h"
#include "InspectorPanel.h"
#include "Scene/FileSceneAssetCatalog.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

class IDemoScene;
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
    bool RequestApplicationClose();
    bool TakeApplicationCloseApproval();

    void ImportFileDrop(
        const std::vector<std::filesystem::path>& files,
        float screenX,
        float screenY,
        const ProjectPaths& paths,
        ProjectAssetRegistry& registry);

    void ConfigureThumbnails(
        ID3D12Device* device,
        ResourceUploader& uploader,
        DescriptorHeap& heap)
    {
        m_assetBrowser.ConfigureThumbnails(device, uploader, heap);
    }

    bool HasBlockingMaterialOperation() const
    {
        return m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending ||
            m_inspectorPanel.HasUnsavedChanges();
    }

    void ConfigureMaterialThumbnails(Renderer& renderer)
    {
        m_assetBrowser.ConfigureMaterialThumbnails(renderer);
    }

    void SetThumbnailCompletedFence(std::uint64_t fence)
    {
        m_assetBrowser.SetThumbnailCompletedFence(fence);
    }

    void ConfirmThumbnailSubmission(std::uint64_t fence)
    {
        m_assetBrowser.ConfirmThumbnailSubmission(fence);
    }

    bool NeedsThumbnailReset() const
    {
        return m_assetBrowser.NeedsThumbnailReset();
    }

    void ClearThumbnailsAfterGPUDrain()
    {
        m_assetBrowser.ClearThumbnailsAfterGPUDrain();
    }

    std::vector<Texture*> GetThumbnailTextures()
    {
        return m_assetBrowser.GetThumbnailTextures();
    }

    const std::optional<MaterialPreviewRequest>& GetMaterialPreviewRequest()
    {
        return m_inspectorPanel.GetPreviewRequest();
    }

    void SetSceneOperationPending(bool pending) noexcept;
    void OnSceneActivated();
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
    };
    std::optional<MaterialAssignmentRequest> TakeMaterialAssignmentRequest();
    void OnMaterialAssignmentCompleted(const SceneObjectMetadata& selection, bool succeeded, std::string message);

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

    uint64_t GetSelectionStructureRevision() const
    {
        return m_selectionStructureRevision;
    }

private:
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
    bool m_viewportHovered = false;
    bool m_viewportFocused = false;

    EditorWorkspaceRect m_viewportImage;

    float m_cameraYaw = 0.0f;
    float m_cameraPitch = 0.0f;

    struct TransformUndoEntry
    {
        size_t objectIndex;

        Transform previousTransform;
    };

    std::vector<TransformUndoEntry> m_transformUndoHistory;
    std::vector<TransformUndoEntry> m_transformRedoHistory;

    bool m_gizmoWasUsing = false;

    size_t m_gizmoUndoObject = InvalidObjectSelection;

    Transform m_gizmoUndoStart;

    bool m_transformFieldEditing = false;

    size_t m_transformFieldUndoObject = InvalidObjectSelection;

    Transform m_transformFieldUndoStart;
    const Scene* m_transformFieldScene = nullptr;
    std::uint64_t m_transformFieldRevision = 0;
    void DrawSelectedObjectTransformControls(IDemoScene& demoScene);
    void FinishTransformFieldEdit(Scene& scene);

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
    void ClearTransformUndoHistory();
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
};
