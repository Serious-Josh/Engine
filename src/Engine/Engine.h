#pragma once

#include "Core/Time.h"
#include "Core/input.h"
#include "Platform/WindowProperties.h"
#include "Platform/Windows/WindowsWindow.h"

enum class EngineState
{
    Uninitialized,
    Running,
    Stopped
};

class Engine {
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

        EngineState m_state = EngineState::Uninitialized;
};