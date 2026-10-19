#pragma once

#include "Scene/SceneDocument.h"

#include <filesystem>
#include <string>

class ProjectPaths;

// Builds an in-memory, never-saved scene: fresh persistent ID, "Untitled Scene", default
// camera/light/environment and no objects. Writes nothing. Output is unchanged on failure.
bool CreateCleanSceneDocument(
    SceneDocument& document,
    std::string* errorMessage = nullptr);

// Output is unchanged on failure. Does not load or activate the scene.
bool CreateNewSceneFile(
    const ProjectPaths& project,
    std::filesystem::path& createdPath,
    std::string* errorMessage = nullptr);
