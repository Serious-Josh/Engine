#include "WindowsWindow.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

LRESULT CALLBACK WindowProc(
    HWND hwnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam
){
    switch(uMsg){
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

WindowsWindow::WindowsWindow()
{
}

WindowsWindow::~WindowsWindow()
{
    Shutdown();
}

bool WindowsWindow::Initialize()
{
    HINSTANCE instance = GetModuleHandle(nullptr);

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"EngineWindowClass";

    if(!RegisterClassW(&windowClass)){
        return false;
    }

    m_windowHandle = CreateWindowExW(
        0,
        L"EngineWindowClass",
        L"Engine",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1280,
        720,
        nullptr,
        nullptr,
        instance,
        nullptr
    );

    if(m_windowHandle == nullptr){
        return false;
    }

    ShowWindow(
        static_cast<HWND>(m_windowHandle),
        SW_SHOW
    );

    return true;
}

void WindowsWindow::Shutdown()
{
    if(m_windowHandle != nullptr){
        DestroyWindow(static_cast<HWND>(m_windowHandle));
        m_windowHandle = nullptr;
    }

    HINSTANCE instance = GetModuleHandle(nullptr);
    UnregisterClassW(L"EngineWindowClass", instance);
}

void WindowsWindow::ProcessMessages()
{
    MSG message{};

    while(PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)){
        if(message.message == WM_QUIT){
            m_shouldClose = true;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

bool WindowsWindow::ShouldClose() const
{
    return m_shouldClose;
}