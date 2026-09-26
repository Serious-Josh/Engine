#include "WindowsWindow.h"
#include "Core/InputCodes.h"
#include "Core/Assert.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Windowsx.h>

#include <utility>

LRESULT CALLBACK WindowProc(
    HWND hwnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam
){
    WindowsWindow* window = nullptr;

    if(uMsg == WM_NCCREATE)
    {
        const CREATESTRUCTW* createStruct =
            reinterpret_cast<const CREATESTRUCTW*>(lParam);

        window =
            static_cast<WindowsWindow*>(createStruct->lpCreateParams);

        SetWindowLongPtrW(
            hwnd,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(window)
        );
    }
    else
    {
        window =
            reinterpret_cast<WindowsWindow*>(
                GetWindowLongPtrW(hwnd, GWLP_USERDATA)
            );
    }

    switch(uMsg)
    {
        case WM_MOUSEMOVE:
        {
            Event event;
            event.type = EventType::MouseMoved;
            event.mouseX = static_cast<int>(GET_X_LPARAM(lParam));
            event.mouseY = static_cast<int>(GET_Y_LPARAM(lParam));

            if(window){
                window->DispatchEvent(event);
            }

            return 0;

        }

        case WM_LBUTTONDOWN:
        {
            Event event;
            event.type = EventType::MouseButtonPressed;
            event.code = MouseButton::Left;

            if(window){
                window->DispatchEvent(event);
            }

            return 0;
        }

        case WM_LBUTTONUP:
        {
            Event event;
            event.type = EventType::MouseButtonReleased;
            event.code = MouseButton::Left;

            if(window){
                window->DispatchEvent(event);
            }

            return 0;
        }

        case WM_RBUTTONDOWN:
        {
            Event event;
            event.type = EventType::MouseButtonPressed;
            event.code = MouseButton::Right;

            if(window){
                window->DispatchEvent(event);
            }

            return 0;
        }

        case WM_RBUTTONUP:
        {
            Event event;
            event.type = EventType::MouseButtonReleased;
            event.code = MouseButton::Right;

            if(window){
                window->DispatchEvent(event);
            }

            return 0;
        }

        case WM_MBUTTONDOWN:
        {
            Event event;
            event.type = EventType::MouseButtonPressed;
            event.code = MouseButton::Middle;

            if(window){
                window->DispatchEvent(event);
            }

            return 0;
        }

        case WM_MBUTTONUP:
        {
            Event event;
            event.type = EventType::MouseButtonReleased;
            event.code = MouseButton::Middle;

            if(window){
                window->DispatchEvent(event);
            }

            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            Event event;
            event.type = EventType::MouseScrolled;

            const short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            event.scrollY = static_cast<float>(delta) / WHEEL_DELTA;

            if(window){
                window->DispatchEvent(event);
            }

            return 0;
        }

        case WM_KEYDOWN:
        {
            Event event;
            event.type = EventType::KeyPressed;
            event.code = static_cast<unsigned int>(wParam);

            if(window){
                window->DispatchEvent(event);
            }

            return 0;
        }

        case WM_KEYUP:
        {
            Event event;
            event.type = EventType::KeyReleased;
            event.code = static_cast<unsigned int>(wParam);

            if(window){
                window->DispatchEvent(event);
            }

            return 0;
        }

        case WM_SIZE:
        {
            Event event;
            event.type = EventType::WindowResized;
            event.width = static_cast<unsigned int>(LOWORD(lParam));
            event.height = static_cast<unsigned int>(HIWORD(lParam));

            if(window){
                window->DispatchEvent(event);
            }

            return DefWindowProcW(hwnd, uMsg, wParam, lParam);
        }

        case WM_CLOSE:
        {
            Event event;
            event.type = EventType::WindowClose;

            if (window)
            {
                window->DispatchEvent(event);
            }

            return 0;
        }

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

bool WindowsWindow::Initialize(const WindowProperties& properties)
{
    SetSize(properties.width, properties.height);

    HINSTANCE instance = GetModuleHandle(nullptr);

    std::wstring title(properties.title.begin(), properties.title.end());

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
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        static_cast<int>(properties.width),
        static_cast<int>(properties.height),
        nullptr,
        nullptr,
        instance,
        this
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

void WindowsWindow::Close(){
    m_shouldClose = true;
}

void WindowsWindow::SetSize(unsigned int width, unsigned int height){
    m_clientWidth = width;
    m_clientHeight = height;
}

unsigned int WindowsWindow::GetWidth() const
{
    return m_clientWidth;
}

unsigned int WindowsWindow::GetHeight() const
{
    return m_clientHeight;
}

void* WindowsWindow::GetNativeHandle() const
{
    return m_windowHandle;
}

void WindowsWindow::SetEventCallback(EventCallback callback)
{
    ENGINE_ASSERT(callback);
    m_eventCallback = std::move(callback);
}

void WindowsWindow::DispatchEvent(const Event& event)
{
    if (m_eventCallback)
    {
        m_eventCallback(event);
    }
}