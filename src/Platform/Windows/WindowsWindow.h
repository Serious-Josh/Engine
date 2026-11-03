#pragma once

#include "Core/Events/Event.h"
#include "Platform/WindowProperties.h"

#include <chrono>
#include <functional>
#include <optional>
#include <filesystem>
#include <vector>

class WindowsWindow
{
    public:
    using EventCallback = std::function<void(const Event&)>;

    WindowsWindow();
    ~WindowsWindow();

    bool Initialize(const WindowProperties& properties);
    void Shutdown();

    struct FileDrop
    {
        std::vector<std::filesystem::path> files;
        int x = 0;
        int y = 0;
    };

    void QueueFileDrop(FileDrop drop);
    std::vector<FileDrop> TakeFileDrops();
    void ProcessMessages();
    void WaitForMessage();
    bool ShouldClose() const;
    void Close();
    // Removes the window from the screen and taskbar without destroying it (shutdown in progress).
    void Hide();

    void SetSize(
        unsigned int width,
        unsigned int height);
    unsigned int GetWidth() const;
    unsigned int GetHeight() const;

    void* GetNativeHandle() const;

    // AS.6B gameplay cursor lock: confines the cursor to a client-space rectangle and hides it,
    // or releases it (nullopt). Hosts call this every frame; focus loss releases it immediately.
    struct CursorRectangle
    {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
    };
    void SetCursorConfinement(const std::optional<CursorRectangle>& clientRectangle);
    bool IsCursorConfined() const noexcept { return m_cursorConfined; }
    void ReleaseCursorConfinement();

    void SetEventCallback(EventCallback callback);
    void DispatchEvent(const Event& event);

    static std::filesystem::path GetExecutableDirectory();

    private:
    std::vector<FileDrop> m_fileDrops;

    void* m_windowHandle = nullptr;

    bool m_shouldClose = false;

    // Stall detector: Windows shows "Not Responding" after about 5 s without message processing.
    // A gap longer than kMessageStallThreshold between pump passes is logged with its length.
    std::chrono::steady_clock::time_point m_lastMessagePump{};

    EventCallback m_eventCallback;

    unsigned int m_clientWidth = 0;
    unsigned int m_clientHeight = 0;

    bool m_cursorConfined = false;
};
