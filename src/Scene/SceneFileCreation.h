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

// Physical sky S2 (§11 decision 4): what a new scene starts with. Its own sky (the Sky override on) of type Physical (Earth
// defaults), its own exposure with EV_ref 15, and a "Sun": a Directional Light of 120,000 lux above the atmosphere at 45 degrees
// elevation. Existing scenes keep their sky and exposure.
void ApplyNewSceneSkyDefaults(SceneDocument& document);

// Output is unchanged on failure. Does not load or activate the scene.
bool CreateNewSceneFile(
    const ProjectPaths& project,
    std::filesystem::path& createdPath,
    std::string* errorMessage = nullptr);
