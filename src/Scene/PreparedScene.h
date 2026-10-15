#pragma once

#include "Scene/SceneDocument.h"
#include "Scene/SceneObjectResolver.h"

#include <string>
#include <vector>

class SceneAssetResolver;
class FileSceneAssetCatalog;

struct PreparedScene
{
    std::string environmentReference;
    SceneCameraDocument camera;
    DirectionalLight directionalLight;
    std::vector<ResolvedSceneObject> objects;
};

// Explicit file-backed capability. Authored slots are retained without claiming
// that mesh-local metadata has been validated; every runtime mesh remains null.
struct PreparedMetadataScene
{
    SceneDocument document;
    std::vector<ResolvedSceneObject> objects;
};

bool PrepareSceneMetadata(const SceneDocument& document, FileSceneAssetCatalog& catalog,
    PreparedMetadataScene& output, std::string* errorMessage = nullptr);

// Validates and resolves the entire document.
// On failure, output remains unchanged.
bool PrepareScene(
    const SceneDocument& document,
    SceneAssetResolver& resolver,
    PreparedScene& output,
    std::string* errorMessage = nullptr);
