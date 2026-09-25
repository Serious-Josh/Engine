#pragma once

#include "Core/Events/Event.h"

#include <functional>

class WindowsWindow
{
public:
    using EventCallback = std::function<void(const Event&)>;

    WindowsWindow();
    ~WindowsWindow();

    bool Initialize();
    void Shutdown();

    void ProcessMessages();
    bool ShouldClose() const;
    void Close();

    void SetEventCallback(EventCallback callback);
    void DispatchEvent(const Event& event);

private:
    void* m_windowHandle = nullptr;
    bool m_shouldClose = false;

    EventCallback m_eventCallback;
};