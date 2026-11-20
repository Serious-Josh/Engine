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

    if(document.mistRendering && !ValidateMistRenderSettings(*document.mistRendering))
    {
        return Fail(errorMessage, "Invalid mist rendering settings.");
    }

    if(std::string hazeError; document.environmentalHaze && !ValidateEnvironmentalHazeSettings(*document.environmentalHaze, &hazeError))
    {
        return Fail(errorMessage, ("Invalid environmental haze: " + hazeError).c_str());
    }

    if(std::string environmentError; document.airEnvironment && !ValidateAirEnvironmentSettings(*document.airEnvironment, &environmentError))
    {
        return Fail(errorMessage, "Invalid air environment: " + environmentError);
    }

    if(std::string simulationError; document.airSimulation && !ValidateAirSimulationSettings(*document.airSimulation, &simulationError))
    {
        return Fail(errorMessage, "Invalid AIR simulation settings: " + simulationError);
    }

    if(document.voxelBake)
    {
        std::string voxelError;
        if(!ValidateVoxelBakeSettings(*document.voxelBake, &voxelError))
        {
            return Fail(errorMessage, "Invalid voxel bake settings: " + voxelError);
        }
    }

    if(!document.environmentReference.empty() && document.environmentReference != SceneDocument::NoneEnvironmentReference)
    {
        const auto id=AssetID::ParseReference(document.environmentReference);
        if(!id || id->ToReference()!=document.environmentReference) return Fail(errorMessage,"Invalid scene environment asset reference.");
    }
    if(document.id.empty()){
        return Fail(errorMessage, "Scene ID cannot be empty.");
    }

    if(document.kind != SceneKind::Local && document.kind != SceneKind::World)
    {
        return Fail(errorMessage, "Invalid scene kind.");
    }

    // W2: membership, per document. Agreement between a World and its Local Levels is checked across documents
    // (ProjectSceneMembership), never here.
    if(document.kind == SceneKind::Local)
    {
        if(!document.members.empty())
        {
            return Fail(errorMessage, "A Local Level cannot list member Local Levels.");
        }

        if(!document.owningWorld.empty() && (!AssetID::Parse(document.owningWorld) || document.owningWorld == document.id))
        {
            return Fail(errorMessage, "Invalid owning World '" + document.owningWorld + "': it must be another scene's canonical GUID.");
        }
    }
    else
    {
        if(!document.owningWorld.empty())
        {
            return Fail(errorMessage, "A World cannot have an owning World.");
        }

        std::unordered_set<std::string> levels;

        for(const auto& member : document.members)
        {
            if(!AssetID::Parse(member.level) || member.level == document.id)
            {
                return Fail(errorMessage, "Invalid member Local Level '" + member.level + "': it must be another scene's canonical GUID.");
            }

            if(!levels.insert(member.level).second)
            {
                return Fail(errorMessage, "Local Level " + member.level + " is listed more than once.");
            }

            if(!IsFinite(member.placement.position) || !IsFinite(member.placement.rotation))
            {
                return Fail(errorMessage, "The placement of Local Level " + member.level + " is not finite.");
            }
        }
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

    // M3.5: the object bound applies to every document, flat or hierarchical (the hierarchy checks it again below), so an
    // oversized scene is refused at load, save and cook rather than only at activation.
    if(document.objects.size() > MaximumSceneObjects)
    {
        return Fail(errorMessage, "The scene has " + std::to_string(document.objects.size()) + " objects; a scene holds at most " +
            std::to_string(MaximumSceneObjects) + ".");
    }

    std::unordered_set<std::string> objectIDs;
    const SceneObjectDocument* mainCamera = nullptr;

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

bool ValidateProbeVolume(const ProbeVolumeComponent& volume, std::string* error)
{
    const char* reason = nullptr;

    for(const float edge : volume.size)
    {
        if(!std::isfinite(edge) || edge <= 0.0f)
        {
            reason = "Probe Volume size must be positive.";
        }
    }

    if(!std::isfinite(volume.probeSpacing) || volume.probeSpacing <= 0.0f)
    {
        reason = "Probe spacing must be positive.";
    }
    else if(!std::isfinite(volume.heightAboveFloor) || volume.heightAboveFloor < 0.0f)
    {
        reason = "Height above floor must not be negative.";
    }
    else if(volume.placement != ProbePlacement::UniformFloor && volume.placement != ProbePlacement::Centroid && volume.placement != ProbePlacement::Uniform3D)
    {
        reason = "Unknown probe placement.";
    }
    else if(volume.manualProbes.size() > ProbeVolumeComponent::MaximumManualProbes || volume.probeBatch.size() > ProbeVolumeComponent::MaximumReferenceBytes ||
        volume.bakedReflections.size() > ProbeVolumeComponent::MaximumReferenceBytes ||
        volume.bakedPathing.size() > ProbeVolumeComponent::MaximumReferenceBytes)
    {
        reason = "Probe Volume data is too large.";
    }
    else if(!ValidateReflectionBakeSettings(volume.bakeSettings))
    {
        reason = "Probe Volume bake settings are out of range.";
    }
    else if(!ValidatePathingBakeSettings(volume.pathingSettings))
    {
        reason = "Probe Volume pathing settings are out of range.";
    }

    for(const auto& probe : volume.manualProbes)
    {
        if(!std::isfinite(probe[0]) || !std::isfinite(probe[1]) || !std::isfinite(probe[2]))
        {
            reason = "A manual probe has a non-finite position.";
        }
    }

    if(reason && error)
    {
        *error = reason;
    }

    return reason == nullptr;
}

bool ValidateReflectionBakeSettings(const ReflectionBakeSettings& settings, std::string* error)
{
    using Limits = ReflectionBakeSettings;
    const char* reason = nullptr;

    if(settings.rays < Limits::MinimumRays || settings.rays > Limits::MaximumRays)
    {
        reason = "Bake rays must be between 1 and 1048576.";
    }
    else if(settings.diffuseSamples < Limits::MinimumDiffuseSamples || settings.diffuseSamples > Limits::MaximumDiffuseSamples)
    {
        reason = "Bake diffuse samples must be between 1 and 65536.";
    }
    else if(settings.bounces < Limits::MinimumBounces || settings.bounces > Limits::MaximumBounces)
    {
        reason = "Bake bounces must be between 1 and 256.";
    }
    else if(!std::isfinite(settings.irradianceMinimumDistance) ||
        settings.irradianceMinimumDistance < Limits::MinimumIrradianceDistance ||
        settings.irradianceMinimumDistance > Limits::MaximumIrradianceDistance)
    {
        reason = "Bake irradiance minimum must be between 0.01 and 100 metres.";
    }

    if(reason && error)
    {
        *error = reason;
    }

    return reason == nullptr;
}

bool ValidateAcousticSettings(const AcousticSettings& settings, std::string* error)
{
    if(settings.submeshMaterials.size() > AcousticSettings::MaximumAssignments)
    {
        if(error)
        {
            *error = "Too many acoustic material assignments.";
        }

        return false;
    }

    for(const auto& [key, reference] : settings.submeshMaterials)
    {
        if(key.empty() || reference.empty() || key.size() > AcousticSettings::MaximumTextBytes ||
            reference.size() > AcousticSettings::MaximumTextBytes)
        {
            if(error)
            {
                *error = "Acoustic material assignments need a submesh key and a material reference.";
            }

            return false;
        }
    }

    return true;
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

    if(!object.parentID.empty() && !object.parentIsLevelNode && (object.physics || object.triggerCollider || object.characterController))
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

    if(object.acoustics && !ValidateAcousticSettings(*object.acoustics, &componentError))
    {
        return Fail(errorMessage, "Invalid acoustics for object " + object.id + ": " + componentError);
    }

    if(object.airMotor && !ValidateAirMotorComponent(*object.airMotor, &componentError))
    {
        return Fail(errorMessage, "Invalid Air Motor for object " + object.id + ": " + componentError);
    }

    if(object.mistEmitter && !ValidateMistEmitterComponent(*object.mistEmitter, &componentError))
    {
        return Fail(errorMessage, "Invalid Mist Emitter for object " + object.id + ": " + componentError);
    }

    if(object.voxel && !ValidateVoxelSettings(*object.voxel, &componentError))
    {
        return Fail(errorMessage, "Invalid voxel settings for object " + object.id + ": " + componentError);
    }

    if(object.probeVolume && !ValidateProbeVolume(*object.probeVolume, &componentError))
    {
        return Fail(errorMessage, "Invalid Probe Volume for object " + object.id + ": " + componentError);
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

bool ValidateSceneDocumentWrite(const SceneDocument& document, std::string* errorMessage)
{
    if(!ValidateSceneDocumentPersistence(document, errorMessage))
    {
        return false;
    }

    // Never canonicalized here: a legacy id is changed only by explicit migration.
    if(!AssetID::Parse(document.id))
    {
        return Fail(errorMessage, "Scene ID '" + document.id + "' is not a canonical GUID; scenes written in schema " +
            std::to_string(SceneDocument::kSchemaVersion) + " require one. This scene predates schema " +
            std::to_string(SceneDocument::kSceneKindSchemaVersion) + " and must be migrated before it can be saved.");
    }

    // W2: a World always has at least one Local Level; membership changes that would empty it are refused.
    if(document.kind == SceneKind::World && document.members.empty())
    {
        return Fail(errorMessage, "World '" + document.name + "' has no Local Levels; a World must contain at least one.");
    }

    return true;
}

bool ValidateSceneKindOpenable(const SceneDocument& document, std::string* errorMessage)
{
    if(document.kind == SceneKind::World)
    {
        return Fail(errorMessage, "Scene '" + document.name + "' is a World; opening Worlds is not supported yet.");
    }

    return true;
}

AirSimulationSettings ResolveAirSimulation(const SceneDocument* document, const std::optional<AirSimulationSettings>& sessionOverride)
{
    if(sessionOverride)
    {
        return *sessionOverride;
    }

    if(document != nullptr && document->airSimulation)
    {
        return *document->airSimulation;
    }

    return AirSimulationSettings{};
}
