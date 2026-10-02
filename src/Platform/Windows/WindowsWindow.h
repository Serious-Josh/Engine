#pragma once

#include "Core/Events/Event.h"
#include "Platform/WindowProperties.h"

#include <functional>
#include <filesystem>

class WindowsWindow
{
public:
    using EventCallback = std::function<void(const Event&)>;

    WindowsWindow();
    ~WindowsWindow();

    bool Initialize(const WindowProperties& properties);
    void Shutdown();

    void ProcessMessages();
    void WaitForMessage();
    bool ShouldClose() const;
    void Close();

    void SetSize(unsigned int width, unsigned int height);
    unsigned int GetWidth() const;
    unsigned int GetHeight() const;

    void* GetNativeHandle() const;

    void SetEventCallback(EventCallback callback);
    void DispatchEvent(const Event& event);

    static std::filesystem::path GetExecutableDirectory();

private:
    void* m_windowHandle = nullptr;
    bool m_shouldClose = false;

    EventCallback m_eventCallback;

    unsigned int m_clientWidth = 0;
    unsigned int m_clientHeight = 0;
};