#include "Scene/SceneCommit.h"
#include "Renderer/Mesh.h"
#include "Scene/SceneDocumentAdapter.h"
#include <type_traits>

#include <cmath>
#include <utility>

namespace
{
    bool CommitValidatedObjects(const PreparedScene& prepared, Scene& scene, SceneObjectMapping& mapping)
    {
        SceneObjectMapping newMapping;
        std::vector<RenderObject> objects;
        objects.reserve(prepared.objects.size());
        newMapping.committedObjects.reserve(prepared.objects.size());
        newMapping.bindingRevisions.reserve(prepared.objects.size());
        newMapping.idToRuntimeIndex.reserve(prepared.objects.size());

        for (size_t index = 0; index < prepared.objects.size(); ++index)
        {
            const auto& object = prepared.objects[index];
            newMapping.idToRuntimeIndex.emplace(object.id, index);
            newMapping.committedObjects.push_back(object.renderObject);
            newMapping.bindingRevisions.push_back(1); // Fresh runtime session.
            objects.push_back(object.renderObject);
        }

        // ReplaceRenderObjects allocates dirty flags before swapping storage.
        // No throwing operations follow its commit point.
        scene.ReplaceRenderObjects(std::move(objects));
        Camera& camera = scene.GetCamera();
        camera.SetPosition(prepared.camera.position);
        camera.SetTarget(prepared.camera.target);
        camera.SetVerticalFov(prepared.camera.verticalFov);
        camera.SetAspectRatio(prepared.camera.aspectRatio);

        camera.SetNearPlane(prepared.camera.nearPlane);
        camera.SetFarPlane(prepared.camera.farPlane);
        scene.GetDirectionalLight() = prepared.directionalLight;

        newMapping.structureRevision = scene.GetStructureRevision();
        newMapping.scene = &scene;
        static_assert(std::is_nothrow_move_assignable_v<SceneObjectMapping>);
        mapping = std::move(newMapping);
        return true;
    }

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
        // Keep the full-preparation contract. Pending objects use the separate
        // CommitPreparedMetadataScene transaction, never this geometry-ready path.
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

    return CommitValidatedObjects(prepared, scene, mapping);
}

bool CommitPreparedEditorScene(const PreparedScene& prepared, const SceneDocument& document,
    Scene& scene, SceneObjectMapping& mapping, std::string* errorMessage)
{
    if (!ValidateSceneDocument(document, errorMessage) || prepared.objects.size() != document.objects.size())
    {
        return Fail(errorMessage, "Editor replacement authored document or object count is invalid.");
    }

    SceneDocument validation = document;
    validation.camera = prepared.camera;
    validation.directionalLight = prepared.directionalLight;

    for (size_t index = 0; index < prepared.objects.size(); ++index)
    {
        const auto& object = prepared.objects[index];
        const auto& authored = document.objects[index];
        const auto& runtime = object.renderObject;
        const auto residency = GetSceneMeshResidency(authored.meshReference, runtime.mesh);

        if (object.id != authored.id || object.name != authored.name || !runtime.material || !residency ||
            object.meshBindingEmpty != authored.meshReference.empty())
        {
            return Fail(errorMessage, "Editor replacement object identity or authored binding is inconsistent: " + authored.id);
        }

        if (*residency == SceneMeshResidency::Empty &&
            (!authored.materialSlotBindings.empty() || !runtime.slotMaterials.empty()))
        {
            return Fail(errorMessage, "Empty editor mesh assignment cannot carry material-slot overrides: " + authored.id);
        }

        if (*residency == SceneMeshResidency::Resident)
        {
            std::string error;
            const auto& mesh = *runtime.mesh;

            if (!mesh.GetVertexBufferResource() || !mesh.GetVertexCount() || !mesh.GetBounds().valid ||
                !ValidateMeshMetadata(mesh.GetMetadata(), mesh.IsIndexed() ? mesh.GetIndexCount() : mesh.GetVertexCount(), error) ||
                runtime.slotMaterials.size() != mesh.GetMetadata().materialSlots.size())
            {
                return Fail(errorMessage, "Editor replacement resident geometry/slots are incompatible: " + authored.id + ": " + error);
            }

            for (const auto& [slot, reference] : authored.materialSlotBindings)
            {
                if (!runtime.slotMaterials.contains(slot))
                {
                    return Fail(errorMessage, "Resident editor replacement has an unknown authored material slot: " + slot);
                }
            }

            for (const auto& slot : mesh.GetMetadata().materialSlots)
            {
                const auto binding = runtime.slotMaterials.find(slot.id);

                if (binding == runtime.slotMaterials.end() || !binding->second)
                {
                    return Fail(errorMessage, "Editor replacement resident material slot is unresolved: " + slot.id);
                }
            }
        }
        else
        {
            if (runtime.slotMaterials.size() != authored.materialSlotBindings.size())
            {
                return Fail(errorMessage, "Pending editor replacement must retain exactly authored overrides: " + authored.id);
            }

            for (const auto& [slot, reference] : authored.materialSlotBindings)
            {
                const auto binding = runtime.slotMaterials.find(slot);

                if (binding == runtime.slotMaterials.end() || !binding->second)
                {
                    return Fail(errorMessage, "Pending editor replacement material override is unresolved: " + slot);
                }
            }
        }

        validation.objects[index].transform = runtime.transform;
    }

    if (!SceneDocumentsEqual(document, validation))
    {
        return Fail(errorMessage, "Editor replacement transforms or scene settings differ from the authored snapshot.");
    }

    return CommitValidatedObjects(prepared, scene, mapping);
}
