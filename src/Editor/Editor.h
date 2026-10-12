#pragma once

#include "Scene/Transform.h"
#include "AssetBrowser.h"
#include "EditorWorkspace.h"
#include "InspectorPanel.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

class IDemoScene;
class SphereGridScene;
class StressTestScene;
class DemoSceneManager;
class DebugUI;

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

    void SetSceneOperationPending(bool pending) noexcept;
    void OnSceneActivated();
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
        float deltaTime);

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
