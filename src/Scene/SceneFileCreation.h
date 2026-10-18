#pragma once

#include <filesystem>
#include <string>

class ProjectPaths;

// Output is unchanged on failure. Does not load or activate the scene.
bool CreateNewSceneFile(
    const ProjectPaths& project,
    std::filesystem::path& createdPath,
    std::string* errorMessage = nullptr);
