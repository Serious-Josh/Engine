#pragma once

#include "Scene/SceneDocument.h"
#include "Renderer/RenderObject.h"

#include <string>
#include <vector>

class SceneAssetResolver;

struct ResolvedSceneObject
{
    std::string id;
    std::string name;
    RenderObject renderObject;

    // Preparation provenance: null mesh is permitted only for an empty binding.
    bool meshBindingEmpty = false;
};

// Resolves every document object before returning any results.
// On failure, output remains unchanged.
bool ResolveSceneObjects(
    const SceneDocument& document,
    SceneAssetResolver& resolver,
    std::vector<ResolvedSceneObject>& output,
    std::string* errorMessage = nullptr);