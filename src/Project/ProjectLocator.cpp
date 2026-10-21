#include "ProjectLocator.h"

#include <filesystem>

#ifndef ENGINE_SOURCE_ROOT
#error ENGINE_SOURCE_ROOT must be defined by the Engine target.
#endif

namespace ProjectLocator
{
    std::filesystem::path GetDefaultProjectRoot()
    {
        return std::filesystem::path(ENGINE_SOURCE_ROOT)
            / "Projects"
            / "DefaultProject";
    }
}
