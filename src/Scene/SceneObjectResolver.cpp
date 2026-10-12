#include "Scene/SceneObjectResolver.h"

#include "Scene/SceneAssetResolver.h"

#include <utility>

bool ResolveSceneObjects(
    const SceneDocument& document,
    SceneAssetResolver& resolver,
    std::vector<ResolvedSceneObject>& output,
    std::string* errorMessage)
{
    if (!ValidateSceneDocument(document, errorMessage))
    {
        return false;
    }

    std::vector<ResolvedSceneObject> resolved;
    resolved.reserve(document.objects.size());

    for (const SceneObjectDocument& object : document.objects)
    {
        if (object.meshReference.empty())
        {
            if (errorMessage)
            {
                *errorMessage =
                    "Object '" + object.id + "' has no mesh reference.";
            }

            return false;
        }

        if (object.materialReference.empty())
        {
            if (errorMessage)
            {
                *errorMessage =
                    "Object '" + object.id + "' has no material reference.";
            }

            return false;
        }

        Mesh* mesh = resolver.ResolveMesh(object.meshReference);

        if (!mesh)
        {
            if (errorMessage)
            {
                *errorMessage =
                    "Could not resolve mesh '" + object.meshReference +
                    "' for object '" + object.id + "'.";
            }

            return false;
        }

        Material* material =
            resolver.ResolveMaterial(object.materialReference);

        if (!material)
        {
            if (errorMessage)
            {
                *errorMessage =
                    "Could not resolve material '" + object.materialReference +
                    "' for object '" + object.id + "'.";
            }

            return false;
        }

        ResolvedSceneObject entry;
        entry.id = object.id;
        entry.name = object.name;

        entry.renderObject.mesh = mesh;
        entry.renderObject.material = material;
        entry.renderObject.transform = object.transform;
        entry.renderObject.previousTransform = object.transform;

        resolved.push_back(std::move(entry));
    }

    output = std::move(resolved);
    return true;
}