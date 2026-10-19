#include "EditorDistributionPaths.h"
#include "Platform/Windows/WindowsWindow.h"

#include <Windows.h>
#include <ShlObj.h>
#include <stdexcept>

namespace EditorDistributionPaths
{
    bool IsPortable()
    {
        return std::filesystem::is_regular_file(
            WindowsWindow::GetExecutableDirectory() / "EnginePortable.marker");
    }

    namespace
    {
        std::filesystem::path LocalAppDataDirectory(const char* leafDirectory)
        {
            PWSTR localDirectory = nullptr;
            const HRESULT result = SHGetKnownFolderPath(
                FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localDirectory);

            if(FAILED(result))
            {
                throw std::runtime_error("Cannot locate writable Local AppData for the Editor.");
            }

            const auto directory = std::filesystem::path(localDirectory) / "Engine" / leafDirectory;
            CoTaskMemFree(localDirectory);
            std::filesystem::create_directories(directory);
            return directory;
        }
    }

    std::filesystem::path UserDirectory()
    {
        return LocalAppDataDirectory("PortableEditor");
    }

    std::filesystem::path EditorSettingsDirectory()
    {
        if(IsPortable())
        {
            return UserDirectory();
        }

        return LocalAppDataDirectory("Editor");
    }

    std::filesystem::path RuntimeDirectory()
    {
        if(IsPortable())
        {
            return WindowsWindow::GetExecutableDirectory() / "Game";
        }

        return std::filesystem::path(ENGINE_RUNTIME_DEPLOY_DIR);
    }
}
