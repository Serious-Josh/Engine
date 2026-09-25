#pragma once

class WindowsWindow{
    public:
        WindowsWindow();
        ~WindowsWindow();

        bool Initialize();
        void Shutdown();

        void ProcessMessages();
        bool ShouldClose() const;

    private:
        void* m_windowHandle = nullptr;
        bool m_shouldClose = false;
};