#pragma once

#include <filesystem>

namespace EditorDistributionPaths
{
    bool IsPortable();
    std::filesystem::path UserDirectory();
    std::filesystem::path RuntimeDirectory();
}
