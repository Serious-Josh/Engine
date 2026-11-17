#include "ProjectLocator.h"

#include <filesystem>

#ifndef ENGINE_SOURCE_ROOT
#error ENGINE_SOURCE_ROOT must be defined by the Engine target.
#endif

namespace ProjectLocator
{
    std::filesystem::path GetDefaultProjectRoot()
    {
#if defined(_DEBUG)
        // W1.5 diagnostics (Debug builds only): verification drivers that use the default project can be pointed at a copy, so
        // opening it (which migrates legacy scenes, W1.4) never changes the checked-in DefaultProject.
        if(const wchar_t* redirected = _wgetenv(L"ENGINE_DIAG_DEFAULT_PROJECT_ROOT"); redirected && *redirected)
        {
            return std::filesystem::path(redirected);
        }
#endif

        return std::filesystem::path(ENGINE_SOURCE_ROOT)
            / "Projects"
            / "DefaultProject";
    }
}
