#pragma once

#include "Core/Time.h"
#include "Core/input.h"
#include "Platform/Windows/WindowsWindow.h"

class Engine {
public:
    bool Initialize();
    void Run();
    void Shutdown();

    Input& GetInput();

private:
    void Frame();

    WindowsWindow m_window;
    Time m_time;
    Input m_input;
};