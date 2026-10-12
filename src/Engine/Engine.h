#pragma once

#include "Core/Time.h"
#include "Core/Input.h"

#include "DemoContent/DemoSceneManager.h"

#include "Platform/Windows/WindowsWindow.h"
#include "Platform/WindowProperties.h"

#include "Renderer/Renderer.h"

#include "Editor/Editor.h"

#include "assets/ProjectAssetRegistry.h"

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

    private:
    void Frame();
    bool ActivateRequestedFileScene();
    void ProcessEditorSceneRequests(IDemoScene& activeScene);
    void ProcessEditorMaterialRequests();
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
