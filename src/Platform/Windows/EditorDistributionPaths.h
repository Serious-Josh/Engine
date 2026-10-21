#pragma once

#include <filesystem>

namespace EditorDistributionPaths
{
    // Personal Editor configuration (preferences, ImGui layout): Known Folder LocalAppData/Engine/Editor.
    // Created on demand.
    std::filesystem::path EditorSettingsDirectory();
    std::filesystem::path RuntimeDirectory();
}
