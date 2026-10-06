#pragma once

#include "Core/Time.h"
#include "Core/Input.h"
#include "DemoContent/DemoSceneManager.h"
#include "Platform/Windows/WindowsWindow.h"
#include "Platform/WindowProperties.h"
#include "Renderer/Renderer.h"
#include "Editor/Editor.h"

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

    Input& GetInput();

    unsigned int GetWindowWidth() const;
    unsigned int GetWindowHeight() const;

private:
    void Frame();

    WindowProperties m_windowProperties;
    WindowsWindow m_window;
    Time m_time;
    Input m_input;
    Renderer m_renderer;

    DemoSceneManager m_demoSceneManager;

    Shader* m_demoVertexShader = nullptr;
    Shader* m_demoPixelShader = nullptr;
    
    Editor m_editor;

    EngineState m_state = EngineState::Uninitialized;
};