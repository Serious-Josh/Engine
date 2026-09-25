#pragma once

#include "Platform/Windows/WindowsWindow.h"

class Engine {
public:
    void Initialize();
    void Run();
    void Shutdown();

private:
    WindowsWindow m_window;
};