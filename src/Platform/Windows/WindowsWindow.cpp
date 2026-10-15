#include "WindowsWindow.h"
#include "WindowsText.h"
#include "Core/InputCodes.h"
#include "Core/Assert.h"
#include <Core/Log.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Windowsx.h>
#include <shellapi.h>

#include <utility>
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam);

LRESULT CALLBACK WindowProc(
    HWND hwnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    WindowsWindow* window = nullptr;

    if (uMsg == WM_NCCREATE)
    {
        const CREATESTRUCTW* createStruct = reinterpret_cast<const CREATESTRUCTW*>(lParam);

        window = static_cast<WindowsWindow*>(createStruct->lpCreateParams);

        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    }
    else
    {
        window = reinterpret_cast<WindowsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    const LRESULT imguiResult = ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam);

    // ImGui owns client-area cursor selection while its backend is active.
    // Do not replace its handled cursor with the fallback arrow below.
    // Other messages still reach Engine's input/event dispatch as before.
    if(uMsg == WM_SETCURSOR && imguiResult != 0)
    {
        return imguiResult;
    }

    switch (uMsg)
    {
    case WM_DROPFILES:
    {
        const HDROP drop = reinterpret_cast<HDROP>(wParam);
        POINT point{};
        WindowsWindow::FileDrop request;

        if(window && DragQueryPoint(drop, &point))
        {
            request.x = point.x;
            request.y = point.y;
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);

            for(UINT index = 0; index < count; ++index)
            {
                const UINT length = DragQueryFileW(drop, index, nullptr, 0);
                std::wstring path(length + 1, L'\0');

                if(DragQueryFileW(drop, index, path.data(), length + 1))
                {
                    path.resize(length);
                    request.files.emplace_back(path);
                }
            }

            window->QueueFileDrop(std::move(request));
        }

        DragFinish(drop);
        return 0;
    }

    case WM_SETCURSOR:
    {
        if (LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));

            return TRUE;
        }

        break;
    }

    case WM_MOUSEMOVE:
    {
        Event event;
        event.type = EventType::MouseMoved;
        event.mouseX = static_cast<int>(GET_X_LPARAM(lParam));
        event.mouseY = static_cast<int>(GET_Y_LPARAM(lParam));

        if (window)
        {
            window->DispatchEvent(event);
        }

        return 0;
    }

    case WM_LBUTTONDOWN:
    {

        Event event;
        event.type = EventType::MouseButtonPressed;
        event.code = MouseButton::Left;

        if (window)
        {
            window->DispatchEvent(event);
        }

        return 0;
    }

    case WM_LBUTTONUP:
    {
        Event event;

        event.type = EventType::MouseButtonReleased;
        event.code = MouseButton::Left;

        if (window)
        {
            window->DispatchEvent(event);
        }

        return 0;
    }

    case WM_RBUTTONDOWN:
    {
        Event event;
        event.type = EventType::MouseButtonPressed;

        event.code = MouseButton::Right;

        if (window)
        {
            window->DispatchEvent(event);
        }

        return 0;
    }

    case WM_RBUTTONUP:
    {
        Event event;
        event.type = EventType::MouseButtonReleased;

        event.code = MouseButton::Right;

        if (window)
        {
            window->DispatchEvent(event);
        }

        return 0;
    }

    case WM_MBUTTONDOWN:
    {
        Event event;
        event.type = EventType::MouseButtonPressed;

        event.code = MouseButton::Middle;

        if (window)
        {
            window->DispatchEvent(event);
        }

        return 0;
    }

    case WM_MBUTTONUP:
    {
        Event event;
        event.type = EventType::MouseButtonReleased;

        event.code = MouseButton::Middle;

        if (window)
        {
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

        if (window)
        {
            window->DispatchEvent(event);
        }

        return 0;
    }

    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
    {
        // F10 arrives as a system key even without Alt. Forward it through
        // Engine input just like F9/F11; retain default handling for Alt keys.
        if (uMsg == WM_SYSKEYDOWN && wParam != VK_F10)
        {
            return DefWindowProcW(hwnd, uMsg, wParam, lParam);
        }

        Event event;

        event.type = EventType::KeyPressed;
        event.code = static_cast<unsigned int>(wParam);

        if (window)
        {
            window->DispatchEvent(event);
        }

        return 0;
    }

    case WM_SYSKEYUP:
    case WM_KEYUP:
    {
        if (uMsg == WM_SYSKEYUP && wParam != VK_F10)
        {
            return DefWindowProcW(hwnd, uMsg, wParam, lParam);
        }

        Event event;

        event.type = EventType::KeyReleased;
        event.code = static_cast<unsigned int>(wParam);

        if (window)
        {
            window->DispatchEvent(event);
        }

        return 0;
    }

    case WM_SIZE:
    {
        Event event;

        event.type = EventType::WindowResized;
        event.width = (wParam == SIZE_MINIMIZED) ? 0u : static_cast<unsigned int>(LOWORD(lParam));

        event.height = (wParam == SIZE_MINIMIZED) ? 0u : static_cast<unsigned int>(HIWORD(lParam));

        if (window)
        {
            window->DispatchEvent(event);
        }

        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    case WM_CLOSE:
    {
        Log::Info("WM_CLOSE received.");

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

    const auto title=WindowsText::FromUTF8(properties.title);

    WNDCLASSW windowClass{};

    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(32512));
    windowClass.lpszClassName = L"EngineWindowClass";

    if (!RegisterClassW(&windowClass))
    {
        return false;
    }

    m_windowHandle = CreateWindowExW(0, L"EngineWindowClass", title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                     CW_USEDEFAULT, static_cast<int>(properties.width),
                                     static_cast<int>(properties.height), nullptr, nullptr, instance, this);

    if (m_windowHandle == nullptr)
    {
        return false;
    }

    DragAcceptFiles(static_cast<HWND>(m_windowHandle), TRUE);
    ShowWindow(static_cast<HWND>(m_windowHandle), SW_SHOW);

    return true;
}

void WindowsWindow::Shutdown()
{
    m_fileDrops.clear();
    if (m_windowHandle != nullptr)
    {
        DestroyWindow(static_cast<HWND>(m_windowHandle));
        m_windowHandle = nullptr;
    }

    HINSTANCE instance = GetModuleHandle(nullptr);

    UnregisterClassW(L"EngineWindowClass", instance);
}

void WindowsWindow::QueueFileDrop(FileDrop drop)
{
    m_fileDrops.push_back(std::move(drop));
}

std::vector<WindowsWindow::FileDrop> WindowsWindow::TakeFileDrops()
{
    return std::exchange(m_fileDrops, {});
}

void WindowsWindow::ProcessMessages()
{
    MSG message{};

    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {

        if (message.message == WM_QUIT)
        {
            m_shouldClose = true;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void WindowsWindow::WaitForMessage()
{
    WaitMessage();
}

bool WindowsWindow::ShouldClose() const
{
    return m_shouldClose;
}

void WindowsWindow::Close()
{
    m_shouldClose = true;
}

void WindowsWindow::SetSize(

    unsigned int width,
    unsigned int height)
{
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

std::filesystem::path WindowsWindow::GetExecutableDirectory()
{
    wchar_t executablePath[MAX_PATH];

    DWORD length = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);

    if (length == 0 || length == MAX_PATH)
    {
        return {};
    }

    return std::filesystem::path(executablePath).parent_path();
}
