#pragma once

#include "Core/Time.h"
#include "Core/Input.h"

#include "DemoContent/DemoSceneManager.h"

#include "Platform/Windows/WindowsWindow.h"
#include "Platform/WindowProperties.h"

#include "Renderer/Renderer.h"

#include "Editor/Editor.h"

#include "assets/ProjectAssetRegistry.h"
#include "Scene/FileSceneRuntime.h"
#include "Scene/FileSceneAssetCatalog.h"

#include <cstdint>

enum class EngineState
{
    Uninitialized,
    Running,
    Stopped
};

class Engine
{
    public:
    bool Initialize();
    void Run();
    void Shutdown();
    // Main-thread, non-UI seam. Queues copied identity; executes at the next safe frame boundary.
    bool RequestMaterialAssignment(const SceneObjectMetadata& selection, std::string reference, std::string& error);
    bool RequestMaterialAssignment(RenderObjectID index, std::string reference, std::string& error);

    private:
    void Frame();
    bool ActivateRequestedFileScene();
    bool ExecuteMaterialAssignment();
    struct MaterialAssignmentRequest
    {
        SceneObjectMetadata selection;

        std::string reference;
    };
    std::optional<MaterialAssignmentRequest> m_pendingMaterialAssignment;
    void ProcessEditorSceneRequests(IDemoScene& activeScene);
    void ProcessEditorMaterialRequests();
    void ProcessSavedMaterialRevisions();
    void ProcessWorkingMaterialNotifications();
    void PublishWorkingMaterialParameters();
    struct WorkingMaterialOverride
    {
        Editor::SceneMaterialWorkingRevision selection;

        std::optional<MaterialAssetData> parameters;
        std::optional<MaterialAssetData> lastPublished;

        std::uint64_t promotionSequence = 0;
        std::uint64_t lastTableRevision = 0;
    };
    std::optional<WorkingMaterialOverride> m_workingMaterialOverride;

    std::vector<Editor::SceneMaterialWorkingRevision> m_pendingWorkingMaterialRevisions;

    std::vector<RuntimeMaterialVersions> m_workingMaterialRestores;

    std::uint64_t m_lastWorkingMaterialSequence = 0;

    std::vector<Editor::SceneMaterialSavedRevision> m_pendingSavedMaterialRevisions;
    std::vector<Editor::SceneMaterialSavedRevision> m_savedMaterialRevisionQueue;
    // Successfully propagated saved revisions, scoped to this runtime/catalog instance.
    struct SavedMaterialObservation
    {
        MaterialSavedRevision saved;

        RuntimeMaterialVersions runtime;
    };
    std::vector<SavedMaterialObservation> m_savedMaterialObservations;

    void QueueEditorSceneOperation();

    WindowProperties m_windowProperties;

    WindowsWindow m_window;

    Time m_time;

    Input m_input;

    Renderer m_renderer;

    std::optional<Renderer::FrameRenderResult> m_diagnosticRenderOutcome;

    DemoSceneManager m_demoSceneManager;

    uint64_t m_renderedSceneStructureRevision = 0;

    Shader* m_demoVertexShader = nullptr;
    Shader* m_demoPixelShader = nullptr;

    Editor m_editor;

    std::optional<Editor::FileRequest> m_pendingEditorSceneOperation;

    std::string m_sceneOperationError;

    EngineState m_state = EngineState::Uninitialized;

    bool m_sceneResourcesReleasedForSwitch = false;

    ProjectAssetRegistry m_projectAssetRegistry;
};
