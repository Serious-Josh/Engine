#include "Scene/SceneDocumentAdapter.h"

#include "Scene/Scene.h"
#include "Scene/SceneCommit.h"

#include <utility>
#include <vector>

bool ApplySceneDocumentSettings(
    const SceneDocument& document,
    Scene& scene,
    std::string* errorMessage)
{
    if (!ValidateSceneDocument(document, errorMessage))
    {
        return false;
    }

    Camera& camera = scene.GetCamera();

    camera.SetPosition(document.camera.position);
    camera.SetTarget(document.camera.target);
    camera.SetVerticalFov(document.camera.verticalFov);
    camera.SetAspectRatio(document.camera.aspectRatio);
    camera.SetNearPlane(document.camera.nearPlane);
    camera.SetFarPlane(document.camera.farPlane);

    scene.GetDirectionalLight() = document.directionalLight;

    return true;
}

bool CaptureSceneDocument(
    const SceneDocument& source,
    const Scene& scene,
    const SceneObjectMapping& mapping,
    SceneDocument& captured,
    std::string* errorMessage)
{
    const auto fail = [&](const char* message)
    {
        if (errorMessage)
        {
            *errorMessage = message;
        }

        return false;
    };

    if (!ValidateSceneDocument(source, errorMessage))
    {
        return false;
    }

    const auto& objects = scene.GetRenderObjects();

    if (mapping.scene != &scene || mapping.structureRevision != scene.GetStructureRevision())
    {
        return fail("Runtime scene structure no longer matches the committed document.");
    }

    if (objects.size() != source.objects.size() || mapping.idToRuntimeIndex.size() != objects.size() ||
        mapping.committedObjects.size() != objects.size())
    {
        return fail("Runtime scene object count or mapping does not match the document.");
    }

    SceneDocument candidate = source;
    std::vector<bool> visited(objects.size(), false);

    for (auto& object : candidate.objects)
    {
        const auto found = mapping.idToRuntimeIndex.find(object.id);

        if (found == mapping.idToRuntimeIndex.end() || found->second >= objects.size())
        {
            return fail("Document object has no valid runtime mapping.");
        }

        const auto index = found->second;

        if (visited[index])
        {
            return fail("Multiple document objects map to the same runtime object.");
        }

        visited[index] = true;
        const auto& runtime = objects[index];
        const auto& committed = mapping.committedObjects[index];

        if (runtime.mesh != committed.mesh || runtime.material != committed.material ||
            runtime.slotMaterials != committed.slotMaterials)
        {
            return fail("Runtime asset replacement cannot be persisted as a transform edit.");
        }

        object.transform = runtime.transform;
    }

    const Camera& camera = scene.GetCamera();
    candidate.camera.position = camera.GetPosition();
    candidate.camera.target = camera.GetTarget();
    candidate.camera.verticalFov = camera.GetVerticalFov();
    candidate.camera.nearPlane = camera.GetNearPlane();
    candidate.camera.farPlane = camera.GetFarPlane();
    candidate.directionalLight = scene.GetDirectionalLight();

    if (!ValidateSceneDocument(candidate, errorMessage))
    {
        return false;
    }

    captured = std::move(candidate);
    return true;
}

bool SceneDocumentsEqual(
    const SceneDocument& left,
    const SceneDocument& right)
{
    const auto sameVector = [](const Vector3& a, const Vector3& b)
    {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    };

    if (left.schemaVersion != right.schemaVersion || left.id != right.id ||
        left.name != right.name || left.objects.size() != right.objects.size())
    {
        return false;
    }

    if (!sameVector(left.camera.position, right.camera.position) || !sameVector(left.camera.target, right.camera.target))
    {
        return false;
    }

    if (!sameVector(left.directionalLight.direction, right.directionalLight.direction) || !sameVector(left.directionalLight.color, right.directionalLight.color))
    {
        return false;
    }

    if (left.camera.verticalFov != right.camera.verticalFov ||
        left.camera.aspectRatio != right.camera.aspectRatio ||
        left.camera.nearPlane != right.camera.nearPlane ||
        left.camera.farPlane != right.camera.farPlane)
    {
        return false;
    }

    if (left.directionalLight.intensity != right.directionalLight.intensity ||
        left.directionalLight.ambientStrength != right.directionalLight.ambientStrength ||
        left.directionalLight.angularRadius != right.directionalLight.angularRadius)
    {
        return false;
    }

    for (std::size_t index = 0; index < left.objects.size(); ++index)
    {
        const auto& a = left.objects[index];
        const auto& b = right.objects[index];

        if (a.id != b.id || a.name != b.name || a.meshReference != b.meshReference ||
            a.materialReference != b.materialReference || a.materialSlotBindings != b.materialSlotBindings)
        {
            return false;
        }

        if (!sameVector(a.transform.position, b.transform.position) ||
            !sameVector(a.transform.rotation, b.transform.rotation) ||
            !sameVector(a.transform.scale, b.transform.scale))
        {
            return false;
        }
    }

    return true;
}
