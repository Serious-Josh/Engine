#include "Platform/Windows/EditorFileDialogs.h"

#include <Windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace EditorFileDialogs
{
    std::optional<std::filesystem::path> Select(
        Kind kind, void* owner, const std::filesystem::path& directory, std::string& error)
    {
        error.clear();
        const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        if(FAILED(initialized))
        {
            error = "Cannot initialize the native file dialog.";
            return std::nullopt;
        }

        struct Cleanup
        {
            ~Cleanup()
            {
                CoUninitialize();
            }
        } cleanup;
        Microsoft::WRL::ComPtr<IFileDialog> dialog;
        HRESULT result = CoCreateInstance(kind == Kind::SaveScene ? CLSID_FileSaveDialog : CLSID_FileOpenDialog,
            nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));

        if(SUCCEEDED(result))
        {
            // Overwrites are confirmed after containment validation by the Engine, exactly once.
            result = dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR |
                FOS_STRICTFILETYPES | (kind == Kind::SaveScene ? 0 : FOS_FILEMUSTEXIST));
        }

        const bool project = kind == Kind::OpenProject;
        const COMDLG_FILTERSPEC filter{project ? L"Engine project (*.engineproj)" : L"Engine scene (*.scene)",
            project ? L"*.engineproj" : L"*.scene"};

        if(SUCCEEDED(result))
        {
            result = dialog->SetFileTypes(1, &filter);
        }

        if(SUCCEEDED(result))
        {
            result = dialog->SetDefaultExtension(project ? L"engineproj" : L"scene");
        }

        const wchar_t* title = L"Open Scene";

        if(project)
        {
            title = L"Open Project";
        }
        else if(kind == Kind::SaveScene)
        {
            title = L"Save Scene As";
        }

        if(SUCCEEDED(result))
        {
            result = dialog->SetTitle(title);
        }

        if(SUCCEEDED(result) && !directory.empty())
        {
            Microsoft::WRL::ComPtr<IShellItem> folder;

            if(SUCCEEDED(SHCreateItemFromParsingName(directory.c_str(), nullptr, IID_PPV_ARGS(&folder))))
            {
                result = dialog->SetFolder(folder.Get());
            }
        }

        if(SUCCEEDED(result) && kind == Kind::SaveScene)
        {
            result = dialog->SetFileName(L"Untitled.scene");
        }

        if(SUCCEEDED(result))
        {
            result = dialog->Show(static_cast<HWND>(owner));
        }

        if(result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        {
            return std::nullopt;
        }

        Microsoft::WRL::ComPtr<IShellItem> selected;

        if(SUCCEEDED(result))
        {
            result = dialog->GetResult(&selected);
        }

        PWSTR text = nullptr;

        if(SUCCEEDED(result))
        {
            result = selected->GetDisplayName(SIGDN_FILESYSPATH, &text);
        }

        if(FAILED(result))
        {
            error = "Native file selection failed (HRESULT " + std::to_string(result) + ").";
            return std::nullopt;
        }

        std::filesystem::path path(text);
        CoTaskMemFree(text);
        return path;
    }

    bool ConfirmOverwrite(void* owner, const std::filesystem::path& path)
    {
        const auto message = L"Replace this existing scene file?\n\n" + path.wstring();
        return MessageBoxW(static_cast<HWND>(owner), message.c_str(), L"Replace scene file",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    }

    bool Launch(const std::filesystem::path& executable, const std::filesystem::path& manifest, std::string& error,
        ChildEditor* child)
    {
        error.clear();
        // Filesystem names cannot contain quotes. Both quoted arguments end in a filename,
        // so there is no trailing backslash that could escape the closing quote.
        auto command = L"\"" + executable.wstring() + L"\" \"" + manifest.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};

        if(!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
            executable.parent_path().c_str(), &startup, &process))
        {
            error = "Cannot launch the selected project (Windows error " + std::to_string(GetLastError()) + ").";
            return false;
        }

        CloseHandle(process.hThread);

        if(child)
        {
            child->process = process.hProcess;
            child->processID = process.dwProcessId;
        }
        else
        {
            CloseHandle(process.hProcess);
        }

        return true;
    }

    bool Relaunch(const std::filesystem::path& manifest, std::string& error, ChildEditor* child)
    {
        std::wstring executable(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));

        if(length == 0 || length >= executable.size())
        {
            error = "Cannot resolve the current Editor executable.";
            return false;
        }

        executable.resize(length);
        return Launch(executable, manifest, error, child);
    }

    ChildState PollChild(const ChildEditor& child, unsigned long& exitCode)
    {
        exitCode = 0;

        if(WaitForSingleObject(child.process, 0) == WAIT_OBJECT_0)
        {
            DWORD code = 0;
            GetExitCodeProcess(child.process, &code);
            exitCode = code;
            return ChildState::Exited;
        }

        struct Search
        {
            DWORD processID;
            bool found;
        } search{child.processID, false};

        EnumWindows([](HWND window, LPARAM parameter) -> BOOL
        {
            auto& state = *reinterpret_cast<Search*>(parameter);
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);

            if(owner == state.processID && IsWindowVisible(window) && !GetWindow(window, GW_OWNER))
            {
                state.found = true;
                return FALSE;
            }

            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));

        return search.found ? ChildState::WindowShown : ChildState::Starting;
    }

    void CloseChild(ChildEditor& child)
    {
        if(child.process)
        {
            CloseHandle(child.process);
        }

        child = {};
    }
}
