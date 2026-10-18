#include "Scene/SceneDocument.h"
#include "Scene/SceneHierarchy.h"
#include "Scene/SceneHierarchyTransforms.h"
#include "assets/AssetID.h"

#include <cmath>
#include <unordered_set>

namespace
{
    bool IsFinite(const Vector3& value)
    {
        return std::isfinite(value.x) &&
               std::isfinite(value.y) &&
               std::isfinite(value.z);
    }

    bool IsFinite(const Transform& transform)
    {
        return IsFinite(transform.position) &&
               IsFinite(transform.rotation) &&
               IsFinite(transform.scale);
    }

    bool Fail(
        std::string* errorMessage,
        const std::string& message)
    {
        if (errorMessage)
        {
            *errorMessage = message;
        }

        return false;
    }
}

bool ValidateSceneDocument(
    const SceneDocument& document,
    std::string* errorMessage)
{
    if(errorMessage){
        errorMessage->clear();
    }

    if(document.schemaVersion != SceneDocument::kSchemaVersion){
        return Fail(errorMessage, "Unsupported scene schema version.");
    }

    if(!document.environmentReference.empty())
    {
        const auto id=AssetID::ParseReference(document.environmentReference);
        if(!id || id->ToReference()!=document.environmentReference) return Fail(errorMessage,"Invalid scene environment asset reference.");
    }
    if(document.id.empty()){
        return Fail(errorMessage, "Scene ID cannot be empty.");
    }

    if (!IsFinite(document.camera.position) ||
        !IsFinite(document.camera.target) ||
        !std::isfinite(document.camera.verticalFov) ||
        !std::isfinite(document.camera.aspectRatio) ||
        !std::isfinite(document.camera.nearPlane) ||
        !std::isfinite(document.camera.farPlane))
    {
        return Fail(errorMessage, "Invalid camera values.");
    }

    if (document.camera.verticalFov <= 0.0f ||
        document.camera.verticalFov >= 3.14159265f ||
        document.camera.aspectRatio <= 0.0f ||
        document.camera.nearPlane <= 0.0f ||
        document.camera.farPlane <= document.camera.nearPlane)
    {
        return Fail(errorMessage, "Invalid camera projection.");
    }

    const DirectionalLight& light = document.directionalLight;

    if (!IsFinite(light.direction) ||
        !IsFinite(light.color) ||
        !std::isfinite(light.intensity) ||
        !std::isfinite(light.ambientStrength) ||
        !std::isfinite(light.angularRadius))
    {
        return Fail(errorMessage, "Invalid directional light values.");
    }

    if (light.intensity < 0.0f ||
        light.ambientStrength < 0.0f ||
        light.angularRadius < 0.0f)
    {
        return Fail(errorMessage, "Invalid directional light parameters.");
    }

    std::unordered_set<std::string> objectIDs;

    for (const SceneObjectDocument& object : document.objects)
    {
        if (object.id.empty())
        {
            return Fail(errorMessage, "Object ID cannot be empty.");
        }

        if (!objectIDs.insert(object.id).second)
        {
            return Fail(errorMessage, "Duplicate object ID: " + object.id);
        }

        if (!IsFinite(object.transform))
        {
            return Fail(
                errorMessage,
                "Invalid transform for object: " + object.id);
        }

        if(!ValidateSceneObjectComponents(object, errorMessage))
        {
            return false;
        }

        for(const auto& [slotID, reference] : object.materialSlotBindings)
        {
            if(slotID.empty() || !AssetID::ParseReference(reference))
            {
                return Fail(errorMessage, "Invalid material-slot binding for object: " + object.id);
            }
        }
    }

    // Existing flat validation above already checks identities/components. Avoid
    // graph allocation on ordinary frame-time captures and preserve loose count policy.
    if(std::all_of(document.objects.begin(), document.objects.end(),
        [](const SceneObjectDocument& object) { return object.parentID.empty(); }))
    {
        return true;
    }

    // Connected branches must also satisfy the H.3 transform rules, so an admitted document
    // always installs and renders; no path flattens or repairs it later.
    SceneHierarchy::Forest hierarchy;
    return SceneHierarchy::Build(document.objects, hierarchy, errorMessage) &&
        SceneHierarchy::ValidateConnectedTransforms(document.objects, hierarchy, errorMessage);
}

bool ValidateSceneObjectComponents(const SceneObjectDocument& object, std::string* errorMessage)
{
    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(object.characterController && object.physics)
    {
        return Fail(errorMessage, "Physics and Character Controller are mutually exclusive for object: " + object.id);
    }

    if(!object.parentID.empty() && (object.physics || object.triggerCollider || object.characterController))
    {
        return Fail(errorMessage, "Physics, Trigger Collider and Character Controller require a root object: " + object.id);
    }

    std::string componentError;

    if(object.physics && !Physics::ValidatePhysicsComponent(*object.physics, &componentError))
    {
        return Fail(errorMessage, "Invalid physics for object " + object.id + ": " + componentError);
    }

    if(object.triggerCollider && !Physics::ValidateTriggerColliderComponent(*object.triggerCollider, &componentError))
    {
        return Fail(errorMessage, "Invalid trigger collider for object " + object.id + ": " + componentError);
    }

    if(object.characterController &&
        !Physics::ValidateCharacterControllerTransform(*object.characterController, object.transform, &componentError))
    {
        return Fail(errorMessage, "Invalid Character Controller for object " + object.id + ": " + componentError);
    }

    return true;
}

bool ValidateSceneDocumentPersistence(const SceneDocument& document, std::string* errorMessage)
{
    // Schema 9 and cooked payload 9 persist parent relationships, so no output flattening guard remains.
    return ValidateSceneDocument(document, errorMessage);
}
