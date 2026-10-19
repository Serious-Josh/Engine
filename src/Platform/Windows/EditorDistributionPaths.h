#pragma once

#include <filesystem>

namespace EditorDistributionPaths
{
    bool IsPortable();
    std::filesystem::path UserDirectory();

    // Personal Editor configuration (preferences, ImGui layout): the portable user directory for
    // a portable Editor, otherwise Known Folder LocalAppData/Engine/Editor. Created on demand.
    std::filesystem::path EditorSettingsDirectory();
    std::filesystem::path RuntimeDirectory();
}
