#include "Scene/SceneDocument.h"
#include "Scene/SceneHierarchy.h"
#include "Scene/SceneHierarchyTransforms.h"
#include "assets/AssetID.h"

#include <algorithm>
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

    if(!document.environmentReference.empty() && document.environmentReference != SceneDocument::NoneEnvironmentReference)
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

    if(const char* reason = nullptr; !ValidateSceneLightingSettings(document.lightingSettings, &reason))
        return Fail(errorMessage, reason);
    if(const char* reason = nullptr; !ValidateCameraExposure(document.camera.exposure, &reason))
        return Fail(errorMessage, std::string("Viewport camera exposure: ") + reason);

    std::unordered_set<std::string> objectIDs;
    const SceneObjectDocument* mainCamera = nullptr;
    std::uint32_t lightCount = 0;

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

        if(object.camera && object.camera->main)
        {
            if(mainCamera)
            {
                return Fail(errorMessage, "Multiple Main Cameras: " + mainCamera->id + " and " + object.id);
            }

            // Disabled cameras retain their designation and count toward uniqueness.
            mainCamera = &object;
        }

        // Disabled Lights count too: the bound is the renderer's fixed per-frame light capacity.
        if(object.light && ++lightCount > MaximumSceneLights + 1)
        {
            return Fail(errorMessage, "The scene has more than " + std::to_string(MaximumSceneLights + 1) +
                " Light components (including the dedicated sun slot).");
        }

        for(const auto& [slotID, reference] : object.materialSlotBindings)
        {
            if(slotID.empty() || !AssetID::ParseReference(reference))
            {
                return Fail(errorMessage, "Invalid material-slot binding for object: " + object.id);
            }
        }
    }

    const bool hasDirectional = std::any_of(document.objects.begin(), document.objects.end(),
        [](const auto& object) { return object.light && object.light->type == LightType::Directional; });
    if(lightCount > MaximumSceneLights && !hasDirectional)
        return Fail(errorMessage, "Lights exceed the table capacity without a directional sun slot.");

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

bool ValidateScriptComponent(const ScriptComponent& script, std::string* error)
{
    if(error)
    {
        error->clear();
    }

    const char* reason = nullptr;

    if(!script.scriptReference.empty() && !AssetID::ParseReference(script.scriptReference))
    {
        reason = "Script reference must be empty or a canonical asset reference.";
    }
    else if(script.className.size() > ScriptComponent::MaximumClassNameBytes)
    {
        reason = "Script class name is too long.";
    }
    else
    {
        // AngelScript identifiers joined by "::"; anything else can never name a script class.
        for(const char character : script.className)
        {
            const bool identifier = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') || character == '_' || character == ':';

            if(!identifier)
            {
                reason = "Script class name may contain only letters, digits, '_' and '::'.";
                break;
            }
        }
    }

    if(reason && error)
    {
        *error = reason;
    }

    return reason == nullptr;
}

bool ValidateAudioSourceComponent(const AudioSourceComponent& source, std::string* error)
{
    if(error)
    {
        error->clear();
    }

    const char* reason = nullptr;
    const std::string& guid = source.eventGuid;

    // FMOD Studio's GUID text: {8-4-4-4-12 hex digits}. Checked lexically; whether a bank defines it is Play's concern.
    if(!guid.empty())
    {
        bool lexical = guid.size() == 38 && guid.front() == '{' && guid.back() == '}';

        for(std::size_t index = 1; lexical && index + 1 < guid.size(); ++index)
        {
            const char character = guid[index];
            const bool dash = index == 9 || index == 14 || index == 19 || index == 24;
            const bool hex = (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
                (character >= 'A' && character <= 'F');
            lexical = dash ? character == '-' : hex;
        }

        if(!lexical)
        {
            reason = "Audio Source event must be empty or an FMOD GUID {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}.";
        }
    }

    if(!reason && !source.eventPath.empty() &&
        (!source.eventPath.starts_with("event:/") || source.eventPath.size() > AudioSourceComponent::MaximumEventPathBytes))
    {
        reason = "Audio Source event path must be empty or an \"event:/...\" path of at most 512 bytes.";
    }

    if(!reason && std::any_of(source.eventPath.begin(), source.eventPath.end(), [](unsigned char character)
        {
            return character < 32;
        }))
    {
        reason = "Audio Source event path must not contain control characters.";
    }

    if(reason && error)
    {
        *error = reason;
    }

    return reason == nullptr;
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

    if(object.camera && !ValidateCameraComponent(*object.camera, &componentError))
    {
        return Fail(errorMessage, "Invalid Camera for object " + object.id + ": " + componentError);
    }

    if(object.light && !ValidateLightComponent(*object.light, &componentError))
    {
        return Fail(errorMessage, "Invalid Light for object " + object.id + ": " + componentError);
    }

    if(object.script && !ValidateScriptComponent(*object.script, &componentError))
    {
        return Fail(errorMessage, "Invalid Script for object " + object.id + ": " + componentError);
    }

    if(object.audioSource && !ValidateAudioSourceComponent(*object.audioSource, &componentError))
    {
        return Fail(errorMessage, "Invalid Audio Source for object " + object.id + ": " + componentError);
    }

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
    // Current codecs preserve authored hierarchy and all optional component values.
    return ValidateSceneDocument(document, errorMessage);
}
