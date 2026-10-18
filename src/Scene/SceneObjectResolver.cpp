#include "Scene/SceneObjectResolver.h"

#include "Scene/SceneAssetResolver.h"
#include "Renderer/Mesh.h"
#include "Scene/SceneSwitchTiming.h"

#include <utility>
#include <unordered_map>
#include <algorithm>

bool ResolveSceneObjects(
    const SceneDocument& document,
    SceneAssetResolver& resolver,
    std::vector<ResolvedSceneObject>& output,
    std::string* errorMessage)
{
    if (!ValidateSceneDocumentPersistence(document, errorMessage))
    {
        return false;
    }

    std::vector<ResolvedSceneObject> resolved;
    resolved.reserve(document.objects.size());
    // Canonical asset references, not paths or names. Lifetime is one preparation.
    std::unordered_map<std::string, Mesh*> meshes;

    if(SceneSwitchTiming::active)
    {
        SceneSwitchTiming::active->meshInstances += static_cast<size_t>(std::count_if(
            document.objects.begin(), document.objects.end(),
            [](const SceneObjectDocument& object) { return !object.meshReference.empty(); }));
    }

    for (const SceneObjectDocument& object : document.objects)
    {
        // Empty bindings are intentional document state. Nonempty references
        // still pass through the resolver and retain their checked failure behavior.
        Mesh* mesh = nullptr;

        if(!object.meshReference.empty())
        {
            const auto cached = meshes.find(object.meshReference);

            if(cached != meshes.end())
            {
                mesh = cached->second;

                if(SceneSwitchTiming::active)
                {
                    ++SceneSwitchTiming::active->meshReferenceReuses;
                }
            }
            else
            {
                mesh = resolver.ResolveMesh(object.meshReference);

                if(mesh)
                {
                    meshes.emplace(object.meshReference, mesh);

                    if(SceneSwitchTiming::active)
                    {
                        ++SceneSwitchTiming::active->uniqueReferences;
                    }
                }
            }
        }

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
