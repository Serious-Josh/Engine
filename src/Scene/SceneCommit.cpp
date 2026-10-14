#include "Scene/SceneCommit.h"
#include "Renderer/Mesh.h"

#include <cmath>
#include <utility>

namespace
{
    bool Fail(std::string* errorMessage, const std::string& message)
    {
        if (errorMessage)
        {
            *errorMessage = message;
        }

        return false;
    }
}

bool CommitPreparedScene(
    const PreparedScene& prepared,
    Scene& scene,
    SceneObjectMapping& mapping,
    std::string* errorMessage)
{
    // Validate the prepared snapshot before changing the runtime scene.
    // This also protects against modifications made after PrepareScene().
    SceneDocument validation;
    validation.id = "prepared-scene-validation";
    validation.camera = prepared.camera;
    validation.directionalLight = prepared.directionalLight;

    for (const ResolvedSceneObject& object : prepared.objects)
    {
        if(!object.renderObject.material ||
            (object.renderObject.mesh == nullptr) != object.meshBindingEmpty)
        {
            return Fail(errorMessage, "Prepared object has an unresolved asset.");
        }

        const auto& materials = object.renderObject.slotMaterials;
        const auto* mesh = object.renderObject.mesh;

        if((!mesh && !materials.empty()) ||
            (mesh && materials.size() != mesh->GetMetadata().materialSlots.size()))
        {
            return Fail(errorMessage, "Prepared object material slots do not match its mesh.");
        }

        if(mesh)
        {
            for(const auto& slot : mesh->GetMetadata().materialSlots)
            {
                const auto material = materials.find(slot.id);

                if(material == materials.end() || !material->second)
                {
                    return Fail(errorMessage, "Prepared object has an unresolved material slot: " + slot.id);
                }
            }
        }

        SceneObjectDocument entry;
        entry.id = object.id;
        entry.name = object.name;
        entry.transform = object.renderObject.transform;

        validation.objects.push_back(std::move(entry));
    }

    if (!ValidateSceneDocument(validation, errorMessage))
    {
        return false;
    }

    // Build the persistent-ID mapping before modifying Scene.
    SceneObjectMapping newMapping;

    for (std::size_t i = 0; i < prepared.objects.size(); ++i)
    {
        newMapping.idToRuntimeIndex.emplace(prepared.objects[i].id, i);
        newMapping.committedObjects.push_back(prepared.objects[i].renderObject);
    }

    // Commit point. Caller must ensure renderer/GPU synchronization.
    scene.ClearRenderObjects();

    Camera& camera = scene.GetCamera();
    camera.SetPosition(prepared.camera.position);
    camera.SetTarget(prepared.camera.target);
    camera.SetVerticalFov(prepared.camera.verticalFov);
    camera.SetAspectRatio(prepared.camera.aspectRatio);
    camera.SetNearPlane(prepared.camera.nearPlane);
    camera.SetFarPlane(prepared.camera.farPlane);

    scene.GetDirectionalLight() = prepared.directionalLight;

    for (const ResolvedSceneObject& object : prepared.objects)
    {
        scene.AddRenderObject(object.renderObject);
    }

    newMapping.structureRevision = scene.GetStructureRevision();
    newMapping.scene = &scene;
    mapping = std::move(newMapping);
    return true;
}
