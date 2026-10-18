#include "ProjectLocator.h"
#include "Platform/Windows/EditorDistributionPaths.h"

#include <filesystem>

#ifndef ENGINE_SOURCE_ROOT
#error ENGINE_SOURCE_ROOT must be defined by the Engine target.
#endif

namespace ProjectLocator
{
    std::filesystem::path GetDefaultProjectRoot()
    {
        if(EditorDistributionPaths::IsPortable())
        {
            return EditorDistributionPaths::UserDirectory() / "Projects" / "DefaultProject";
        }

        return std::filesystem::path(ENGINE_SOURCE_ROOT)
            / "Projects"
            / "DefaultProject";
    }
}
