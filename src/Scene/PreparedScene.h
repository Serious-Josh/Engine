#pragma once

#include "Scene/SceneDocument.h"
#include "Scene/SceneObjectResolver.h"

#include <string>
#include <vector>

class SceneAssetResolver;

struct PreparedScene
{
    SceneCameraDocument camera;
    DirectionalLight directionalLight;
    std::vector<ResolvedSceneObject> objects;
};

// Validates and resolves the entire document.
// On failure, output remains unchanged.
bool PrepareScene(
    const SceneDocument& document,
    SceneAssetResolver& resolver,
    PreparedScene& output,
    std::string* errorMessage = nullptr);