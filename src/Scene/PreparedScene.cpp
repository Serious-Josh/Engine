#include "Scene/PreparedScene.h"

#include "Scene/SceneAssetResolver.h"

#include <utility>

bool PrepareScene(
    const SceneDocument& document,
    SceneAssetResolver& resolver,
    PreparedScene& output,
    std::string* errorMessage)
{
    if (!ValidateSceneDocumentPersistence(document, errorMessage))
    {
        return false;
    }

    PreparedScene prepared;
    prepared.environmentReference = document.environmentReference;
    prepared.camera = document.camera;
    prepared.directionalLight = document.directionalLight;

    if (!ResolveSceneObjects(
            document,
            resolver,
            prepared.objects,
            errorMessage))
    {
        return false;
    }

    output = std::move(prepared);
    return true;
}