#include "Scene/SceneObjectResolver.h"

#include "Scene/SceneAssetResolver.h"
#include "Renderer/Mesh.h"

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
        // Empty bindings are intentional document state. Nonempty references
        // still pass through the resolver and retain their checked failure behavior.
        Mesh* mesh = object.meshReference.empty() ? nullptr : resolver.ResolveMesh(object.meshReference);

        if(!mesh && !object.meshReference.empty())
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
            resolver.ResolveMaterial(object.materialReference.empty() ? "builtin:default" : object.materialReference);

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
        entry.meshBindingEmpty = object.meshReference.empty();
        entry.id = object.id;
        entry.name = object.name;

        entry.renderObject.mesh = mesh;
        entry.renderObject.material = material;
        entry.renderObject.transform = object.transform;
        entry.renderObject.previousTransform = object.transform;

        if(mesh)
        {
            for(const auto& slot : mesh->GetMetadata().materialSlots)
            {
                entry.renderObject.slotMaterials.emplace(slot.id, material);
            }
        }

        for(const auto& [slotID, reference] : object.materialSlotBindings)
        {
            const auto slot = entry.renderObject.slotMaterials.find(slotID);

            if(slot == entry.renderObject.slotMaterials.end())
            {
                if(errorMessage)
                {
                    *errorMessage = "Unknown mesh material slot '" + slotID + "' for object '" + object.id + "'.";
                }

                return false;
            }

            Material* overrideMaterial = resolver.ResolveMaterial(reference);

            if(!overrideMaterial)
            {
                if(errorMessage)
                {
                    *errorMessage = "Could not resolve material '" + reference + "' for slot '" + slotID + "'.";
                }

                return false;
            }

            slot->second = overrideMaterial;
        }

        resolved.push_back(std::move(entry));
    }

    output = std::move(resolved);
    return true;
}
