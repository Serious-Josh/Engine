#pragma once

#include "Core/Time.h"
#include "Core/Input.h"
#include "Platform/Windows/WindowsWindow.h"
#include "Platform/WindowProperties.h"
#include "Renderer/Renderer.h"

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

    EngineState m_state = EngineState::Uninitialized;
};