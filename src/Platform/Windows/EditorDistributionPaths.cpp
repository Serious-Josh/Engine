#include "EditorDistributionPaths.h"

#include <Windows.h>
#include <ShlObj.h>
#include <stdexcept>

namespace EditorDistributionPaths
{
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

    std::filesystem::path EditorSettingsDirectory()
    {
        return LocalAppDataDirectory("Editor");
    }

    std::filesystem::path RuntimeDirectory()
    {
        return std::filesystem::path(ENGINE_RUNTIME_DEPLOY_DIR);
    }
}
