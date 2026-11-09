#include "Scene/SceneSerializer.h"
#include "Scene/LegacyLightingMigration.h"
#include "Scene/PhysicalLightingMigration.h"
#include "Scene/ColliderScaleMigration.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <stdexcept>
#include <set>
#include <utility>

namespace
{
    const char* ProbePlacementToString(ProbePlacement placement)
    {
        switch(placement)
        {
            case ProbePlacement::Centroid:
                return "centroid";
            case ProbePlacement::Uniform3D:
                return "uniform3D";
            case ProbePlacement::UniformFloor:
            default:
                return "uniformFloor";
        }
    }

    ProbePlacement ProbePlacementFromString(const std::string& text)
    {
        if(text == "uniformFloor")
        {
            return ProbePlacement::UniformFloor;
        }

        if(text == "centroid")
        {
            return ProbePlacement::Centroid;
        }

        if(text == "uniform3D")
        {
            return ProbePlacement::Uniform3D;
        }

        throw std::runtime_error("Unknown probe placement '" + text + "'.");
    }

    using Json = nlohmann::json;

    void ValidateFields(const Json& object, std::initializer_list<std::string_view> allowedFields)
    {
        if (!object.is_object())
        {
            throw std::runtime_error("Expected scene object fields.");
        }

        for (auto field = object.begin(); field != object.end(); ++field)
        {
            if (std::find(allowedFields.begin(), allowedFields.end(), field.key()) == allowedFields.end())
            {
                throw std::runtime_error("Unsupported scene field: " + field.key());
            }
        }
    }

    template <typename Input>
    Json ParseSceneJson(Input&& input)
    {
        std::vector<std::set<std::string>> objectFields;
        return Json::parse(std::forward<Input>(input), [&](int, Json::parse_event_t event, Json& value)
        {
            if (event == Json::parse_event_t::object_start)
            {
                objectFields.emplace_back();
            }
            else if (event == Json::parse_event_t::key)
            {
                if (!objectFields.back().insert(value.get<std::string>()).second)
                {
                    throw std::runtime_error("Duplicate scene field: " + value.get<std::string>());
                }
            }
            else if (event == Json::parse_event_t::object_end)
            {
                objectFields.pop_back();
            }
            return true;
        });
    }

    Json ToJson(const Vector3& value)
    {
        return Json::array({value.x, value.y, value.z});
    }

    float ReadNumber(const Json& value, const char* fieldName)
    {
        if (!value.is_number())
        {
            throw std::runtime_error(std::string(fieldName) + " must be a number.");
        }

        const float result = value.get<float>();
        if (!std::isfinite(result))
        {
            throw std::runtime_error(std::string(fieldName) + " must be finite and representable as a float.");
        }
        return result;
    }

    Vector3 ReadVector3(const Json& value)
    {
        if (!value.is_array() || value.size() != 3)
        {
            throw std::runtime_error("Expected a 3-element vector.");
        }

        return {
            ReadNumber(value.at(0), "Vector x"),
            ReadNumber(value.at(1), "Vector y"),
            ReadNumber(value.at(2), "Vector z")
        };
    }

    Json ToJson(const Transform& value)
    {
        return {
            {"position", ToJson(value.position)},
            {"rotation", ToJson(value.rotation)},
            {"scale", ToJson(value.scale)}
        };
    }

    Transform ReadTransform(const Json& value)
    {
        ValidateFields(value, {"position", "rotation", "scale"});
        Transform result;
        result.position = ReadVector3(value.at("position"));
        result.rotation = ReadVector3(value.at("rotation"));
        result.scale = ReadVector3(value.at("scale"));
        return result;
    }

    const char* ColliderTypeName(Physics::ColliderType colliderType)
    {
        switch(colliderType)
        {
        case Physics::ColliderType::Box:
            return "Box";

        case Physics::ColliderType::Sphere:
            return "Sphere";

        case Physics::ColliderType::Capsule:
            return "Capsule";

        default:
            throw std::runtime_error("Unsupported physics collider type.");
        }
    }

    Json ToJson(const Physics::PhysicsComponent& component)
    {
        const char* bodyType = nullptr;
        switch (component.bodyType)
        {
        case Physics::BodyType::Static:
            bodyType = "Static";
            break;
        case Physics::BodyType::Dynamic:
            bodyType = "Dynamic";
            break;
        case Physics::BodyType::Kinematic:
            bodyType = "Kinematic";
            break;
        default:
            throw std::runtime_error("Unsupported physics body type.");
        }

        return {
            {"bodyType", bodyType},
            {"colliderType", ColliderTypeName(component.colliderType)},
            {"sphereRadius", component.sphereRadius},
            {"capsuleRadius", component.capsuleRadius},
            {"capsuleCylinderHeight", component.capsuleCylinderHeight},
            {"capsuleLocalRotation", ToJson(component.capsuleLocalRotation)},
            {"generateContactEvents", component.generateContactEvents},
            {"boxDimensions", {
                {"width", component.boxDimensions.width},
                {"height", component.boxDimensions.height},
                {"depth", component.boxDimensions.depth}
            }},
            {"colliderLocalPositionOffset", ToJson(component.colliderLocalPositionOffset)},
            {"density", component.density},
            {"friction", component.friction},
            {"restitution", component.restitution}
        };
    }

    // Each schema has an exact physics field set: v2 is Box-only, v3 adds Sphere, v4 adds
    // Capsule, v5 adds Capsule local rotation and v6 adds generateContactEvents. Older
    // schemas never accept newer fields or collider types.
    Physics::PhysicsComponent ReadPhysicsComponent(const Json& value, std::uint32_t schemaVersion)
    {
        const bool sphereSchema = schemaVersion >= SceneDocument::kSphereSchemaVersion;
        const bool capsuleSchema = schemaVersion >= SceneDocument::kCapsuleSchemaVersion;
        const bool capsuleRotationSchema = schemaVersion >= SceneDocument::kCapsuleRotationSchemaVersion;
        const bool contactEventsSchema = schemaVersion >= SceneDocument::kContactEventsSchemaVersion;

        if(contactEventsSchema)
        {
            ValidateFields(value, {"bodyType", "colliderType", "boxDimensions", "sphereRadius", "capsuleRadius",
                "capsuleCylinderHeight", "capsuleLocalRotation", "colliderLocalPositionOffset", "density", "friction",
                "restitution", "generateContactEvents"});
        }
        else if(capsuleRotationSchema)
        {
            ValidateFields(value, {"bodyType", "colliderType", "boxDimensions", "sphereRadius", "capsuleRadius",
                "capsuleCylinderHeight", "capsuleLocalRotation", "colliderLocalPositionOffset", "density", "friction",
                "restitution"});
        }
        else if(capsuleSchema)
        {
            ValidateFields(value, {"bodyType", "colliderType", "boxDimensions", "sphereRadius", "capsuleRadius",
                "capsuleCylinderHeight", "colliderLocalPositionOffset", "density", "friction", "restitution"});
        }
        else if(sphereSchema)
        {
            ValidateFields(value, {"bodyType", "colliderType", "boxDimensions", "sphereRadius", "colliderLocalPositionOffset",
                "density", "friction", "restitution"});
        }
        else
        {
            ValidateFields(value, {"bodyType", "colliderType", "boxDimensions", "colliderLocalPositionOffset",
                "density", "friction", "restitution"});
        }

        Physics::PhysicsComponent component;
        const auto bodyType = value.at("bodyType").get<std::string>();
        if (bodyType == "Static")
        {
            component.bodyType = Physics::BodyType::Static;
        }
        else if (bodyType == "Dynamic")
        {
            component.bodyType = Physics::BodyType::Dynamic;
        }
        else if (bodyType == "Kinematic")
        {
            component.bodyType = Physics::BodyType::Kinematic;
        }
        else
        {
            throw std::runtime_error("Unsupported physics body type: " + bodyType);
        }

        const auto colliderType = value.at("colliderType").get<std::string>();

        if(sphereSchema && colliderType == "Sphere")
        {
            component.colliderType = Physics::ColliderType::Sphere;
        }
        else if(capsuleSchema && colliderType == "Capsule")
        {
            component.colliderType = Physics::ColliderType::Capsule;
        }
        else if(colliderType != "Box")
        {
            throw std::runtime_error("Unsupported physics collider type.");
        }

        // Older schemas keep the in-memory defaults for shapes they cannot store.
        if(sphereSchema)
        {
            component.sphereRadius = ReadNumber(value.at("sphereRadius"), "Physics sphere radius");
        }

        if(capsuleSchema)
        {
            component.capsuleRadius = ReadNumber(value.at("capsuleRadius"), "Physics capsule radius");
            component.capsuleCylinderHeight = ReadNumber(value.at("capsuleCylinderHeight"),
                "Physics capsule cylinder height");
        }

        // v1-v4 keep the default zero Capsule rotation.
        if(capsuleRotationSchema)
        {
            component.capsuleLocalRotation = ReadVector3(value.at("capsuleLocalRotation"));
        }

        // v1-v5 keep the default false; v6 requires a JSON boolean (no numeric coercion).
        if(contactEventsSchema)
        {
            const Json& generateContactEvents = value.at("generateContactEvents");

            if(!generateContactEvents.is_boolean())
            {
                throw std::runtime_error("Physics generateContactEvents must be a boolean.");
            }

            component.generateContactEvents = generateContactEvents.get<bool>();
        }

        const auto& dimensions = value.at("boxDimensions");
        ValidateFields(dimensions, {"width", "height", "depth"});
        component.boxDimensions.width = ReadNumber(dimensions.at("width"), "Physics box width");
        component.boxDimensions.height = ReadNumber(dimensions.at("height"), "Physics box height");
        component.boxDimensions.depth = ReadNumber(dimensions.at("depth"), "Physics box depth");
        component.colliderLocalPositionOffset = ReadVector3(value.at("colliderLocalPositionOffset"));
        component.density = ReadNumber(value.at("density"), "Physics density");
        component.friction = ReadNumber(value.at("friction"), "Physics friction");
        component.restitution = ReadNumber(value.at("restitution"), "Physics restitution");

        std::string error;
        if (!Physics::ValidatePhysicsComponent(component, &error))
        {
            throw std::runtime_error(error);
        }
        return component;
    }

    Json ToJson(const Physics::TriggerColliderComponent& component)
    {
        return {
            {"colliderType", ColliderTypeName(component.colliderType)},
            {"boxDimensions", {{"width", component.boxDimensions.width},
                {"height", component.boxDimensions.height}, {"depth", component.boxDimensions.depth}}},
            {"sphereRadius", component.sphereRadius},
            {"capsuleRadius", component.capsuleRadius},
            {"capsuleCylinderHeight", component.capsuleCylinderHeight},
            {"colliderLocalPositionOffset", ToJson(component.colliderLocalPositionOffset)},
            {"capsuleLocalRotation", ToJson(component.capsuleLocalRotation)}
        };
    }

    Physics::TriggerColliderComponent ReadTriggerCollider(const Json& value)
    {
        ValidateFields(value, {"colliderType", "boxDimensions", "sphereRadius", "capsuleRadius",
            "capsuleCylinderHeight", "colliderLocalPositionOffset", "capsuleLocalRotation"});
        Physics::TriggerColliderComponent component;
        const auto type = value.at("colliderType").get<std::string>();

        if(type == "Box")
        {
            component.colliderType = Physics::ColliderType::Box;
        }
        else if(type == "Sphere")
        {
            component.colliderType = Physics::ColliderType::Sphere;
        }
        else if(type == "Capsule")
        {
            component.colliderType = Physics::ColliderType::Capsule;
        }
        else
        {
            throw std::runtime_error("Unsupported trigger collider type: " + type);
        }

        const auto& dimensions = value.at("boxDimensions");
        ValidateFields(dimensions, {"width", "height", "depth"});
        component.boxDimensions.width = ReadNumber(dimensions.at("width"), "Trigger box width");
        component.boxDimensions.height = ReadNumber(dimensions.at("height"), "Trigger box height");
        component.boxDimensions.depth = ReadNumber(dimensions.at("depth"), "Trigger box depth");
        component.sphereRadius = ReadNumber(value.at("sphereRadius"), "Trigger sphere radius");
        component.capsuleRadius = ReadNumber(value.at("capsuleRadius"), "Trigger capsule radius");
        component.capsuleCylinderHeight = ReadNumber(value.at("capsuleCylinderHeight"), "Trigger capsule cylinder height");
        component.colliderLocalPositionOffset = ReadVector3(value.at("colliderLocalPositionOffset"));
        component.capsuleLocalRotation = ReadVector3(value.at("capsuleLocalRotation"));
        std::string error;

        if(!Physics::ValidateTriggerColliderComponent(component, &error))
        {
            throw std::runtime_error(error);
        }

        return component;
    }

    Json ToJson(const Physics::CharacterControllerComponent& component)
    {
        return {
            {"capsuleRadius", component.capsuleRadius},
            {"capsuleCylinderHeight", component.capsuleCylinderHeight},
            {"localOffset", ToJson(component.localOffset)},
            {"maximumHorizontalSpeed", component.maximumHorizontalSpeed},
            {"gravityScale", component.gravityScale},
            {"jumpSpeed", component.jumpSpeed},
            {"maximumWalkableSlope", component.maximumWalkableSlope},
            {"groundSnapDistance", component.groundSnapDistance},
            {"collisionClearance", component.collisionClearance}
        };
    }

    Physics::CharacterControllerComponent ReadCharacterController(const Json& value)
    {
        ValidateFields(value, {"capsuleRadius", "capsuleCylinderHeight", "localOffset", "maximumHorizontalSpeed",
            "gravityScale", "jumpSpeed", "maximumWalkableSlope", "groundSnapDistance", "collisionClearance"});
        Physics::CharacterControllerComponent component;
        component.capsuleRadius = ReadNumber(value.at("capsuleRadius"), "Controller capsule radius");
        component.capsuleCylinderHeight = ReadNumber(value.at("capsuleCylinderHeight"), "Controller cylinder height");
        component.localOffset = ReadVector3(value.at("localOffset"));
        component.maximumHorizontalSpeed = ReadNumber(value.at("maximumHorizontalSpeed"), "Controller horizontal speed");
        component.gravityScale = ReadNumber(value.at("gravityScale"), "Controller gravity scale");
        component.jumpSpeed = ReadNumber(value.at("jumpSpeed"), "Controller jump speed");
        component.maximumWalkableSlope = ReadNumber(value.at("maximumWalkableSlope"), "Controller walkable slope");
        component.groundSnapDistance = ReadNumber(value.at("groundSnapDistance"), "Controller snap distance");
        component.collisionClearance = ReadNumber(value.at("collisionClearance"), "Controller clearance");
        std::string error;

        if(!Physics::ValidateCharacterControllerComponent(component, &error))
        {
            throw std::runtime_error(error);
        }

        return component;
    }

    // Schema 13 adds angular radius; earlier components inherit the historical sun radius.
    // Schema 14 stores physical units (intensity: lumens, or lux for Directional) and the range attenuation mode.
    // Every field is retained for every type. Type is a name so the
    // file stays readable; angles are radians like the Camera FOV.
    Json ToJson(const LightComponent& light)
    {
        return {
            {"enabled", light.enabled},
            {"type", LightTypeName(light.type)},
            {"color", ToJson(light.color)},
            {"intensity", light.intensity},
            {"range", light.range},
            {"rangeAttenuation", light.rangeAttenuation},
            {"innerConeAngle", light.innerConeAngle},
            {"outerConeAngle", light.outerConeAngle},
            {"angularRadius", light.angularRadius}
        };
    }

    // legacyRange is set (and the historical rules apply, before MigrateLegacyLightUnits) for legacy-unit input.
    LightComponent ReadLightComponent(const Json& value, float* legacyRange)
    {
        if(legacyRange)
        {
            ValidateFields(value, {"enabled", "type", "color", "intensity", "range", "innerConeAngle", "outerConeAngle", "angularRadius"});
        }
        else
        {
            ValidateFields(value, {"enabled", "type", "color", "intensity", "range", "rangeAttenuation", "innerConeAngle",
                "outerConeAngle", "angularRadius"});
        }

        if(!value.at("enabled").is_boolean())
        {
            throw std::runtime_error("Light enabled must be a boolean.");
        }

        if(!value.at("type").is_string())
        {
            throw std::runtime_error("Light type must be Point, Spot or Directional.");
        }

        LightComponent light;
        const std::string typeName = value.at("type").get<std::string>();

        if(typeName == "Point")
        {
            light.type = LightType::Point;
        }
        else if(typeName == "Spot")
        {
            light.type = LightType::Spot;
        }
        else if(typeName == "Directional")
        {
            light.type = LightType::Directional;
        }
        else
        {
            throw std::runtime_error("Light type must be Point, Spot or Directional.");
        }

        light.enabled = value.at("enabled").get<bool>();
        light.color = ReadVector3(value.at("color"));
        light.intensity = ReadNumber(value.at("intensity"), "Light intensity");
        light.range = ReadNumber(value.at("range"), "Light range");
        light.innerConeAngle = ReadNumber(value.at("innerConeAngle"), "Light inner cone angle");
        light.outerConeAngle = ReadNumber(value.at("outerConeAngle"), "Light outer cone angle");
        if(value.contains("angularRadius")) light.angularRadius = ReadNumber(value.at("angularRadius"), "Angular radius");

        if(legacyRange)
        {
            // Historical lights always faded to zero at their range.
            *legacyRange = light.range;
            light.rangeAttenuation = true;
            ValidateLegacyLightValues(light, *legacyRange);
            return light;
        }

        if(!value.at("rangeAttenuation").is_boolean())
        {
            throw std::runtime_error("Light rangeAttenuation must be a boolean.");
        }

        light.rangeAttenuation = value.at("rangeAttenuation").get<bool>();

        std::string error;

        if(!ValidateLightComponent(light, &error))
        {
            throw std::runtime_error(error);
        }

        return light;
    }

    // Photographic exposure (CameraExposure.h). Optional in schema 14 (added within the physical lighting
    // milestone): an absent object means the scene's exact reference exposure, so appearance is unchanged.
    Json ToJson(const CameraExposure& exposure)
    {
        return {
            {"mode", exposure.mode == ExposureMode::Auto ? "Auto" : "Manual"},
            {"aperture", exposure.aperture},
            {"shutterSeconds", exposure.shutterSeconds},
            {"iso", exposure.iso},
            {"compensation", exposure.compensation},
            {"meteringMinEV100", exposure.meteringMinEV100},
            {"meteringMaxEV100", exposure.meteringMaxEV100},
            {"lowPercentile", exposure.lowPercentile},
            {"highPercentile", exposure.highPercentile},
            {"brighteningSpeed", exposure.brighteningSpeed},
            {"darkeningSpeed", exposure.darkeningSpeed},
            {"locked", exposure.locked}
        };
    }

    CameraExposure ReadCameraExposure(const Json& value)
    {
        ValidateFields(value, {"mode", "aperture", "shutterSeconds", "iso", "compensation", "meteringMinEV100",
            "meteringMaxEV100", "lowPercentile", "highPercentile", "brighteningSpeed", "darkeningSpeed", "locked"});

        if(!value.at("mode").is_string() || (value.at("mode") != "Manual" && value.at("mode") != "Auto"))
        {
            throw std::runtime_error("Exposure mode must be Manual or Auto.");
        }

        if(!value.at("locked").is_boolean())
        {
            throw std::runtime_error("Exposure locked must be a boolean.");
        }

        CameraExposure exposure;
        exposure.mode = value.at("mode") == "Auto" ? ExposureMode::Auto : ExposureMode::Manual;
        exposure.aperture = ReadNumber(value.at("aperture"), "Aperture");
        exposure.shutterSeconds = ReadNumber(value.at("shutterSeconds"), "Shutter speed");
        exposure.iso = ReadNumber(value.at("iso"), "ISO");
        exposure.compensation = ReadNumber(value.at("compensation"), "Exposure compensation");
        exposure.meteringMinEV100 = ReadNumber(value.at("meteringMinEV100"), "Metering minimum");
        exposure.meteringMaxEV100 = ReadNumber(value.at("meteringMaxEV100"), "Metering maximum");
        exposure.lowPercentile = ReadNumber(value.at("lowPercentile"), "Low percentile");
        exposure.highPercentile = ReadNumber(value.at("highPercentile"), "High percentile");
        exposure.brighteningSpeed = ReadNumber(value.at("brighteningSpeed"), "Brightening speed");
        exposure.darkeningSpeed = ReadNumber(value.at("darkeningSpeed"), "Darkening speed");
        exposure.locked = value.at("locked").get<bool>();
        const char* reason = nullptr;

        if(!ValidateCameraExposure(exposure, &reason))
        {
            throw std::runtime_error(reason);
        }

        return exposure;
    }

    Json ToJson(const CameraComponent& camera)
    {
        return {
            {"enabled", camera.enabled},
            {"projection", "Perspective"},
            {"verticalFov", camera.verticalFov},
            {"nearPlane", camera.nearPlane},
            {"farPlane", camera.farPlane},
            {"main", camera.main},
            {"exposure", ToJson(camera.exposure)}
        };
    }

    // hasExposure reports whether the record carried exposure; the caller supplies the reference default otherwise.
    CameraComponent ReadCameraComponent(const Json& value, bool& hasExposure)
    {
        ValidateFields(value, {"enabled", "projection", "verticalFov", "nearPlane", "farPlane", "main", "exposure"});

        if(!value.at("enabled").is_boolean() || !value.at("main").is_boolean())
        {
            throw std::runtime_error("Camera enabled and main must be booleans.");
        }

        if(!value.at("projection").is_string() || value.at("projection") != "Perspective")
        {
            throw std::runtime_error("Camera projection must be Perspective.");
        }

        CameraComponent camera;
        camera.enabled = value.at("enabled").get<bool>();
        camera.main = value.at("main").get<bool>();
        camera.verticalFov = ReadNumber(value.at("verticalFov"), "Camera vertical FOV");
        camera.nearPlane = ReadNumber(value.at("nearPlane"), "Camera near plane");
        camera.farPlane = ReadNumber(value.at("farPlane"), "Camera far plane");
        hasExposure = value.contains("exposure");
        if(hasExposure) camera.exposure = ReadCameraExposure(value.at("exposure"));
        std::string error;

        if(!ValidateCameraComponent(camera, &error))
        {
            throw std::runtime_error(error);
        }

        return camera;
    }

    Json ToJson(const SceneDocument& document)
    {
        Json objects = Json::array();

        for (const SceneObjectDocument& object : document.objects)
        {
            // Schema 9 writes parentID on every object: "" is a root. Only authored local TRS
            // is persisted; derived world matrices are never written.
            objects.push_back({
                {"id", object.id},
                {"parentID", object.parentID},
                {"name", object.name},
                {"transform", ToJson(object.transform)},
                {"meshReference", object.meshReference},
                {"materialReference", object.materialReference}
            });

            if(!object.materialSlotBindings.empty())
            {
                Json bindings = Json::array();

                for(const auto& [slotID, reference] : object.materialSlotBindings)
                {
                    bindings.push_back({{"slotID", slotID}, {"materialReference", reference}});
                }

                objects.back()["materialSlotBindings"] = std::move(bindings);
            }

            if (object.physics)
            {
                objects.back()["physics"] = ToJson(*object.physics);
            }

            if(object.triggerCollider)
            {
                objects.back()["triggerCollider"] = ToJson(*object.triggerCollider);
            }

            if(object.characterController)
            {
                objects.back()["characterController"] = ToJson(*object.characterController);
            }

            if(object.camera)
            {
                objects.back()["camera"] = ToJson(*object.camera);
            }

            if(object.light)
            {
                objects.back()["light"] = ToJson(*object.light);
            }

            if(object.script)
            {
                objects.back()["script"] = {
                    {"enabled", object.script->enabled},
                    {"scriptReference", object.script->scriptReference},
                    {"className", object.script->className}
                };
            }

            if(object.audioSource)
            {
                objects.back()["audioSource"] = {
                    {"enabled", object.audioSource->enabled},
                    {"eventGuid", object.audioSource->eventGuid},
                    {"eventPath", object.audioSource->eventPath},
                    {"autoplay", object.audioSource->autoplay}
                };

                // Written only when set, so scenes that do not use them stay byte-identical.
                if(object.audioSource->sourceCentricReflections)
                {
                    objects.back()["audioSource"]["sourceCentricReflections"] = true;
                }

                if(object.audioSource->bakedReflections)
                {
                    objects.back()["audioSource"]["bakedReflections"] = true;
                }

                if(object.audioSource->pathing)
                {
                    objects.back()["audioSource"]["pathing"] = true;
                }
            }

            if(object.acoustics)
            {
                objects.back()["acoustics"] = {
                    {"enabled", object.acoustics->enabled},
                    {"submeshMaterials", object.acoustics->submeshMaterials}
                };
            }

            if(object.probeVolume)
            {
                const ProbeVolumeComponent& volume = *object.probeVolume;
                objects.back()["probeVolume"] = {
                    {"enabled", volume.enabled},
                    {"placement", ProbePlacementToString(volume.placement)},
                    {"size", volume.size},
                    {"probeSpacing", volume.probeSpacing},
                    {"heightAboveFloor", volume.heightAboveFloor},
                    {"manualProbes", volume.manualProbes},
                    {"probeBatch", volume.probeBatch}
                };

                // Written only when it differs from the default, as with other optional fields.
                if(!volume.filterInsideSolids)
                {
                    objects.back()["probeVolume"]["filterInsideSolids"] = false;
                }

                // M8.2, the same convention: absent means the defaults, and no baked data.
                if(volume.bakeSettings != ReflectionBakeSettings{})
                {
                    objects.back()["probeVolume"]["bakeSettings"] = {
                        {"rays", volume.bakeSettings.rays},
                        {"diffuseSamples", volume.bakeSettings.diffuseSamples},
                        {"bounces", volume.bakeSettings.bounces},
                        {"irradianceMinimumDistance", volume.bakeSettings.irradianceMinimumDistance}
                    };
                }

                if(!volume.bakedReflections.empty())
                {
                    objects.back()["probeVolume"]["bakedReflections"] = volume.bakedReflections;
                }

                // M9.5, the same convention.
                if(!volume.pathingEnabled)
                {
                    objects.back()["probeVolume"]["pathingEnabled"] = false;
                }

                if(volume.pathingSettings != PathingBakeSettings{})
                {
                    objects.back()["probeVolume"]["pathingSettings"] = {
                        {"samples", volume.pathingSettings.samples},
                        {"radius", volume.pathingSettings.radius},
                        {"threshold", volume.pathingSettings.threshold},
                        {"visibilityRange", volume.pathingSettings.visibilityRange},
                        {"pathRange", volume.pathingSettings.pathRange}
                    };
                }

                if(!volume.bakedPathing.empty())
                {
                    objects.back()["probeVolume"]["bakedPathing"] = volume.bakedPathing;
                }
            }

            if(object.audioListener)
            {
                objects.back()["audioListener"] = {{"enabled", object.audioListener->enabled}};

                if(object.audioListener->bakedReflections)
                {
                    objects.back()["audioListener"]["bakedReflections"] = true;
                }
            }
        }

        const auto& camera = document.camera;


        return {
            {"schemaVersion", SceneDocument::kSchemaVersion},
            {"id", document.id},
            {"name", document.name},
            {"environmentReference",document.environmentReference},
            {"objects", std::move(objects)},
            {"camera", {
                {"position", ToJson(camera.position)},
                {"target", ToJson(camera.target)},
                {"verticalFov", camera.verticalFov},
                {"aspectRatio", camera.aspectRatio},
                {"nearPlane", camera.nearPlane},
                {"farPlane", camera.farPlane},
                {"exposure", ToJson(camera.exposure)}
            }},
            {"lightingSettings", {{"referenceEV100", document.lightingSettings.referenceEV100},
                {"environmentLuminanceScale", document.lightingSettings.environmentLuminanceScale}}}
        };
    }

    SceneDocument FromJson(const Json& value, const LegacyLightingMigrationOptions& migration = {})
    {
        ValidateFields(value, {"schemaVersion", "id", "name", "camera", "directionalLight", "objects", "environmentReference", "lightingSettings"});
        const auto& schemaVersion = value.at("schemaVersion");
        if (!schemaVersion.is_number_integer() ||
            (schemaVersion != SceneDocument::kLegacySchemaVersion &&
             schemaVersion != SceneDocument::kBoxPhysicsSchemaVersion &&
             schemaVersion != SceneDocument::kSphereSchemaVersion &&
             schemaVersion != SceneDocument::kCapsuleSchemaVersion &&
             schemaVersion != SceneDocument::kCapsuleRotationSchemaVersion &&
             schemaVersion != SceneDocument::kContactEventsSchemaVersion &&
             schemaVersion != SceneDocument::kTriggerColliderSchemaVersion &&
             schemaVersion != SceneDocument::kCharacterControllerSchemaVersion &&
             schemaVersion != SceneDocument::kHierarchySchemaVersion &&
             schemaVersion != SceneDocument::kCameraSchemaVersion &&
             schemaVersion != SceneDocument::kLightSchemaVersion &&
             schemaVersion != SceneDocument::kEnvironmentNoneSchemaVersion &&
             schemaVersion != SceneDocument::kObjectLightingSchemaVersion &&
             schemaVersion != SceneDocument::kPhysicalLightingSchemaVersion &&
             schemaVersion != SceneDocument::kScriptSchemaVersion &&
             schemaVersion != SceneDocument::kAudioSchemaVersion &&
             schemaVersion != SceneDocument::kColliderScaleSchemaVersion &&
             schemaVersion != SceneDocument::kAcousticsSchemaVersion))
        {
            throw std::runtime_error("Unsupported scene schema version.");
        }
        const bool legacySchema = schemaVersion == SceneDocument::kLegacySchemaVersion;
        const bool hierarchySchema = schemaVersion.get<std::uint32_t>() >= SceneDocument::kHierarchySchemaVersion;
        SceneDocument result;

        // Transitional readers preserve protected project content. New saves write v13;
        // no migration is written to disk merely by loading a historical document.
        result.id = value.at("id").get<std::string>();
        result.name = value.at("name").get<std::string>();
        result.environmentReference=value.value("environmentReference",std::string{});

        const Json& camera = value.at("camera");
        ValidateFields(camera, {"position", "target", "verticalFov", "aspectRatio", "nearPlane", "farPlane", "exposure"});
        const bool viewportExposure = camera.contains("exposure");
        if(viewportExposure) result.camera.exposure = ReadCameraExposure(camera.at("exposure"));
        std::vector<size_t> camerasWithoutExposure;
        result.camera.position = ReadVector3(camera.at("position"));
        result.camera.target = ReadVector3(camera.at("target"));
        result.camera.verticalFov = ReadNumber(camera.at("verticalFov"), "Camera vertical FOV");
        result.camera.aspectRatio = ReadNumber(camera.at("aspectRatio"), "Camera aspect ratio");
        result.camera.nearPlane = ReadNumber(camera.at("nearPlane"), "Camera near plane");
        result.camera.farPlane = ReadNumber(camera.at("farPlane"), "Camera far plane");

        std::optional<LegacySceneSun> legacySun;
        const bool physicalLighting = schemaVersion.get<std::uint32_t>() >= SceneDocument::kPhysicalLightingSchemaVersion;
        float legacyAmbientStrength = 0.0f;
        std::vector<LegacyLightValues> legacyLights;
        // Current settings/objects take precedence if both representations are present.
        const bool objectLighting = value.contains("lightingSettings") ||
            schemaVersion.get<std::uint32_t>() >= SceneDocument::kObjectLightingSchemaVersion;
        if(physicalLighting)
        {
            const auto& settings = value.at("lightingSettings");
            ValidateFields(settings, {"referenceEV100", "environmentLuminanceScale"});
            result.lightingSettings.referenceEV100 = ReadNumber(settings.at("referenceEV100"), "Reference exposure");
            result.lightingSettings.environmentLuminanceScale =
                ReadNumber(settings.at("environmentLuminanceScale"), "Environment luminance scale");
        }
        else if(objectLighting)
        {
            const auto& settings = value.at("lightingSettings");
            ValidateFields(settings, {"ambientStrength"});
            legacyAmbientStrength = ReadNumber(settings.at("ambientStrength"), "Ambient strength");
            if(!std::isfinite(legacyAmbientStrength) || legacyAmbientStrength < 0.0f)
                throw std::runtime_error("Invalid scene ambient strength.");
        }
        else if(value.contains("directionalLight"))
        {
            const Json& light = value.at("directionalLight");
            ValidateFields(light, {"direction", "color", "intensity", "ambientStrength", "angularRadius"});
            legacySun.emplace();
            legacySun->direction = ReadVector3(light.at("direction"));
            legacySun->color = ReadVector3(light.at("color"));
            legacySun->intensity = ReadNumber(light.at("intensity"), "Light intensity");
            legacySun->ambientStrength = ReadNumber(light.at("ambientStrength"), "Ambient strength");
            legacySun->angularRadius = ReadNumber(light.at("angularRadius"), "Angular radius");
        }

        const Json& objects = value.at("objects");

        if (!objects.is_array())
        {
            throw std::runtime_error("Scene objects must be an array.");
        }

        for (const Json& item : objects)
        {
            ValidateFields(item, {"id", "parentID", "name", "transform", "meshReference", "materialReference",
                "materialSlotBindings", "physics", "triggerCollider", "characterController", "camera", "light", "script",
                "audioSource", "audioListener", "acoustics", "probeVolume"});
            if (legacySchema && item.contains("physics"))
            {
                throw std::runtime_error("Physics fields require scene schema version 2.");
            }

            SceneObjectDocument object;
            object.id = item.at("id").get<std::string>();

            // Versions 1-8 have no parent field and load every object as a root. Version 9
            // requires an explicit string on every object; the whole forest is validated after
            // migration and is never flattened or repaired.
            if(hierarchySchema)
            {
                const auto& parent = item.at("parentID");

                if(!parent.is_string())
                {
                    throw std::runtime_error("parentID must be a string for object: " + object.id);
                }

                object.parentID = parent.get<std::string>();
            }
            else if(item.contains("parentID"))
            {
                throw std::runtime_error("parentID requires scene schema version 9.");
            }

            object.name = item.at("name").get<std::string>();
            object.transform = ReadTransform(item.at("transform"));
            object.meshReference =
                item.at("meshReference").get<std::string>();
            object.materialReference =
                item.at("materialReference").get<std::string>();

            if(item.contains("materialSlotBindings"))
            {
                const auto& bindings = item.at("materialSlotBindings");

                if(!bindings.is_array())
                {
                    throw std::runtime_error("Material-slot bindings must be an array.");
                }

                for(const auto& binding : bindings)
                {
                    ValidateFields(binding, {"slotID", "materialReference"});
                    const auto slotID = binding.at("slotID").get<std::string>();
                    const auto reference = binding.at("materialReference").get<std::string>();

                    if(!object.materialSlotBindings.emplace(slotID, reference).second)
                    {
                        throw std::runtime_error("Duplicate material-slot ID: " + slotID);
                    }
                }
            }

            if (item.contains("physics"))
            {
                try
                {
                    object.physics = ReadPhysicsComponent(item.at("physics"), schemaVersion.get<std::uint32_t>());
                }
                catch (const std::exception& exception)
                {
                    throw std::runtime_error("Invalid physics for object " + object.id + ": " + exception.what());
                }
            }

            if(item.contains("triggerCollider"))
            {
                try
                {
                    if(schemaVersion.get<std::uint32_t>() < SceneDocument::kTriggerColliderSchemaVersion)
                    {
                        throw std::runtime_error("Trigger Collider fields require scene schema version 7.");
                    }

                    object.triggerCollider = ReadTriggerCollider(item.at("triggerCollider"));
                }
                catch(const std::exception& exception)
                {
                    throw std::runtime_error("Invalid trigger collider for object " + object.id + ": " + exception.what());
                }
            }

            if(item.contains("characterController"))
            {
                if(schemaVersion.get<std::uint32_t>() < SceneDocument::kCharacterControllerSchemaVersion)
                {
                    throw std::runtime_error("Character Controller fields require scene schema version 8.");
                }

                try
                {
                    object.characterController = ReadCharacterController(item.at("characterController"));
                }
                catch(const std::exception& exception)
                {
                    throw std::runtime_error("Invalid Character Controller for object " + object.id + ": " + exception.what());
                }
            }

            if(item.contains("camera"))
            {
                if(schemaVersion.get<std::uint32_t>() < SceneDocument::kCameraSchemaVersion)
                {
                    throw std::runtime_error("Camera component fields require scene schema version 10.");
                }

                try
                {
                    bool hasExposure = false;
                    object.camera = ReadCameraComponent(item.at("camera"), hasExposure);
                    if(!hasExposure) camerasWithoutExposure.push_back(result.objects.size());
                }
                catch(const std::exception& exception)
                {
                    throw std::runtime_error("Invalid Camera for object " + object.id + ": " + exception.what());
                }
            }

            if(item.contains("light"))
            {
                if(schemaVersion.get<std::uint32_t>() < SceneDocument::kLightSchemaVersion)
                {
                    throw std::runtime_error("Light component fields require scene schema version 11.");
                }

                try
                {
                    float range = std::numeric_limits<float>::quiet_NaN();
                    object.light = ReadLightComponent(item.at("light"), physicalLighting ? nullptr : &range);
                    if(!physicalLighting && object.light->type != LightType::Directional)
                        legacyLights.push_back({object.id, range});
                }
                catch(const std::exception& exception)
                {
                    throw std::runtime_error("Invalid Light for object " + object.id + ": " + exception.what());
                }
            }

            if(item.contains("script"))
            {
                if(schemaVersion.get<std::uint32_t>() < SceneDocument::kScriptSchemaVersion)
                {
                    throw std::runtime_error("Script component fields require scene schema version 15.");
                }

                const auto& value = item.at("script");
                ValidateFields(value, {"enabled", "scriptReference", "className"});

                if(!value.at("enabled").is_boolean() || !value.at("scriptReference").is_string() ||
                    !value.at("className").is_string())
                {
                    throw std::runtime_error("Invalid Script field types for object " + object.id + ".");
                }

                ScriptComponent script;
                script.enabled = value.at("enabled").get<bool>();
                script.scriptReference = value.at("scriptReference").get<std::string>();
                script.className = value.at("className").get<std::string>();
                std::string scriptError;

                if(!ValidateScriptComponent(script, &scriptError))
                {
                    throw std::runtime_error("Invalid Script for object " + object.id + ": " + scriptError);
                }

                object.script = std::move(script);
            }

            if(item.contains("audioSource") || item.contains("audioListener"))
            {
                if(schemaVersion.get<std::uint32_t>() < SceneDocument::kAudioSchemaVersion)
                {
                    throw std::runtime_error("Audio component fields require scene schema version 16.");
                }
            }

            if(item.contains("acoustics"))
            {
                if(schemaVersion.get<std::uint32_t>() < SceneDocument::kAcousticsSchemaVersion)
                {
                    throw std::runtime_error("Acoustics fields require scene schema version 18.");
                }

                const auto& value = item.at("acoustics");
                ValidateFields(value, {"enabled", "submeshMaterials"});
                AcousticSettings acoustics;
                acoustics.enabled = value.at("enabled").get<bool>();
                acoustics.submeshMaterials = value.at("submeshMaterials").get<std::map<std::string, std::string>>();
                std::string acousticsError;

                if(!ValidateAcousticSettings(acoustics, &acousticsError))
                {
                    throw std::runtime_error("Invalid acoustics: " + acousticsError);
                }

                object.acoustics = std::move(acoustics);
            }

            if(item.contains("probeVolume"))
            {
                const auto& value = item.at("probeVolume");
                ValidateFields(value, {"enabled", "placement", "size", "probeSpacing", "heightAboveFloor", "manualProbes", "probeBatch", "filterInsideSolids",
                    "bakeSettings", "bakedReflections", "pathingEnabled", "pathingSettings", "bakedPathing"});
                ProbeVolumeComponent volume;
                volume.enabled = value.at("enabled").get<bool>();
                volume.placement = ProbePlacementFromString(value.at("placement").get<std::string>());
                volume.size = value.at("size").get<std::array<float, 3>>();
                volume.probeSpacing = value.at("probeSpacing").get<float>();
                volume.heightAboveFloor = value.at("heightAboveFloor").get<float>();
                volume.manualProbes = value.at("manualProbes").get<std::vector<std::array<float, 3>>>();
                // A generated reference like bakedReflections and bakedPathing: absent means no probes generated yet. A present value of the
                // wrong type is still refused, by value().
                volume.probeBatch = value.value("probeBatch", std::string{});
                volume.filterInsideSolids = value.value("filterInsideSolids", true);

                if(value.contains("bakeSettings"))
                {
                    const auto& bake = value.at("bakeSettings");
                    ValidateFields(bake, {"rays", "diffuseSamples", "bounces", "irradianceMinimumDistance"});
                    volume.bakeSettings.rays = bake.at("rays").get<std::uint32_t>();
                    volume.bakeSettings.diffuseSamples = bake.at("diffuseSamples").get<std::uint32_t>();
                    volume.bakeSettings.bounces = bake.at("bounces").get<std::uint32_t>();
                    volume.bakeSettings.irradianceMinimumDistance = bake.at("irradianceMinimumDistance").get<float>();
                }

                volume.bakedReflections = value.value("bakedReflections", std::string{});
                volume.pathingEnabled = value.value("pathingEnabled", true);

                if(value.contains("pathingSettings"))
                {
                    const auto& pathing = value.at("pathingSettings");
                    ValidateFields(pathing, {"samples", "radius", "threshold", "visibilityRange", "pathRange"});
                    volume.pathingSettings.samples = pathing.at("samples").get<std::uint32_t>();
                    volume.pathingSettings.radius = pathing.at("radius").get<float>();
                    volume.pathingSettings.threshold = pathing.at("threshold").get<float>();
                    volume.pathingSettings.visibilityRange = pathing.at("visibilityRange").get<float>();
                    volume.pathingSettings.pathRange = pathing.at("pathRange").get<float>();
                }

                volume.bakedPathing = value.value("bakedPathing", std::string{});
                std::string volumeError;

                if(!ValidateProbeVolume(volume, &volumeError))
                {
                    throw std::runtime_error("Invalid Probe Volume: " + volumeError);
                }

                object.probeVolume = std::move(volume);
            }

            if(item.contains("audioSource"))
            {
                const auto& value = item.at("audioSource");
                ValidateFields(value, {"enabled", "eventGuid", "eventPath", "autoplay", "sourceCentricReflections", "bakedReflections", "pathing"});

                if(!value.at("enabled").is_boolean() || !value.at("eventGuid").is_string() ||
                    !value.at("eventPath").is_string() || !value.at("autoplay").is_boolean())
                {
                    throw std::runtime_error("Invalid Audio Source field types for object " + object.id + ".");
                }

                AudioSourceComponent source;
                source.enabled = value.at("enabled").get<bool>();
                source.eventGuid = value.at("eventGuid").get<std::string>();
                source.eventPath = value.at("eventPath").get<std::string>();
                source.autoplay = value.at("autoplay").get<bool>();

                if(value.contains("sourceCentricReflections"))
                {
                    if(!value.at("sourceCentricReflections").is_boolean())
                    {
                        throw std::runtime_error("Invalid Audio Source field types for object " + object.id + ".");
                    }

                    source.sourceCentricReflections = value.at("sourceCentricReflections").get<bool>();
                }

                if(value.contains("bakedReflections"))
                {
                    if(!value.at("bakedReflections").is_boolean())
                    {
                        throw std::runtime_error("Invalid Audio Source field types for object " + object.id + ".");
                    }

                    source.bakedReflections = value.at("bakedReflections").get<bool>();
                }

                if(value.contains("pathing"))
                {
                    if(!value.at("pathing").is_boolean())
                    {
                        throw std::runtime_error("Invalid Audio Source field types for object " + object.id + ".");
                    }

                    source.pathing = value.at("pathing").get<bool>();
                }

                std::string sourceError;

                if(!ValidateAudioSourceComponent(source, &sourceError))
                {
                    throw std::runtime_error("Invalid Audio Source for object " + object.id + ": " + sourceError);
                }

                object.audioSource = std::move(source);
            }

            if(item.contains("audioListener"))
            {
                const auto& value = item.at("audioListener");
                ValidateFields(value, {"enabled", "bakedReflections"});

                if(!value.at("enabled").is_boolean() || (value.contains("bakedReflections") && !value.at("bakedReflections").is_boolean()))
                {
                    throw std::runtime_error("Invalid Audio Listener field types for object " + object.id + ".");
                }

                AudioListenerComponent listener;
                listener.enabled = value.at("enabled").get<bool>();
                listener.bakedReflections = value.value("bakedReflections", false);
                object.audioListener = listener;
            }

            result.objects.push_back(std::move(object));
        }

        if(!objectLighting) legacyAmbientStrength = MigrateLegacyLighting(result, legacySun);
        const auto applyReferenceExposure = [&]()
        {
            // Cameras authored before exposure existed take the default exposure (Auto), with manual settings at the
            // scene reference exposure.
            const auto reference = Photometry::ReferenceCameraExposure(result.lightingSettings.referenceEV100);
            if(!viewportExposure) result.camera.exposure = reference;
            for(const size_t index : camerasWithoutExposure) result.objects[index].camera->exposure = reference;
        };
        if(!physicalLighting)
        {
            LegacyLightingReport report;
            LegacyLightingMigrationOptions options = migration;
            if(!options.report) options.report = &report;
            MigrateLegacyLightUnits(result, schemaVersion.get<std::uint32_t>(), legacyAmbientStrength, legacyLights, options);
            result.schemaVersion = SceneDocument::kSchemaVersion;
            Log::Info("Scene '" + result.name + "' converted from schema " + std::to_string(schemaVersion.get<std::uint32_t>()) +
                " legacy light units at reference exposure EV100 " + std::to_string(options.referenceEV100) + " (" +
                std::to_string(options.report->lights.size()) + " lights; ranges kept with range attenuation on). Save to persist.");
        }
        applyReferenceExposure();

        // Runs after every object and transform is read. Only older schemas convert, and saves write the current
        // schema, so a scene is converted at most once.
        const std::uint32_t loadedSchema = schemaVersion.get<std::uint32_t>();

        if(loadedSchema < SceneDocument::kColliderScaleSchemaVersion)
        {
            LogColliderScaleMigration(result, MigrateStoredColliderScale(result),
                "schema " + std::to_string(loadedSchema));
        }

        return result;
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

static CreateSceneDocumentResult WriteSceneDocument(
    const SceneDocument& document,
    const std::filesystem::path& path,
    std::string* errorMessage,
    bool replaceExisting)
{
    std::string validationError;

    if (!ValidateSceneDocumentPersistence(document, &validationError))
    {
        Fail(errorMessage, validationError);
        return CreateSceneDocumentResult::Error;
    }

    std::filesystem::path temporaryPath;

    try
    {
        // Serialize before touching the filesystem.
        const std::string serialized = ToJson(document).dump(4) + '\n';

        if (path.empty() || path.filename().empty())
        {
            Fail(errorMessage, "Invalid scene file path.");
            return CreateSceneDocumentResult::Error;
        }

        // The destination directory must already exist.
        const std::filesystem::path directory =
            path.has_parent_path()
                ? path.parent_path()
                : std::filesystem::path(".");

        wchar_t temporaryName[MAX_PATH + 1]{};

        // Creates a unique, empty file in the destination directory.
        if (GetTempFileNameW(
                directory.c_str(),
                L"SCN",
                0,
                temporaryName) == 0)
        {
            Fail(
                errorMessage,
                "Failed to create temporary scene file.");
            return CreateSceneDocumentResult::Error;
        }

        temporaryPath = temporaryName;

        {
            std::ofstream file(
                temporaryPath,
                std::ios::binary | std::ios::trunc);

            if (!file)
            {
                throw std::runtime_error(
                    "Failed to open temporary scene file.");
            }

            file.write(serialized.data(),
                       static_cast<std::streamsize>(serialized.size()));

            file.flush();

            if (!file)
            {
                throw std::runtime_error(
                    "Failed to write temporary scene file.");
            }

            file.close();

            if (!file)
            {
                throw std::runtime_error(
                    "Failed to close temporary scene file.");
            }
        }

        // Same-directory rename publishes the complete file. Never allow a copy fallback.
        if (!MoveFileExW(
                temporaryPath.c_str(),
                path.c_str(),
                MOVEFILE_WRITE_THROUGH |
                    (replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0)))
        {
            const DWORD error = GetLastError();

            if (!replaceExisting && (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS))
            {
                std::error_code cleanupError;
                std::filesystem::remove(temporaryPath, cleanupError);

                if (cleanupError)
                {
                    throw std::runtime_error("Destination collision; temporary cleanup failed: " +
                        cleanupError.message());
                }

                Fail(errorMessage, "Destination scene file already exists.");
                return CreateSceneDocumentResult::Collision;
            }

            throw std::runtime_error(
                "Failed to publish scene file (Windows error " + std::to_string(error) + ").");
        }

        temporaryPath.clear();

        if (errorMessage)
        {
            errorMessage->clear();
        }

        return CreateSceneDocumentResult::Created;
    }
    catch (const std::exception& exception)
    {
        if (!temporaryPath.empty())
        {
            std::error_code cleanupError;
            std::filesystem::remove(temporaryPath, cleanupError);
        }

        Fail(errorMessage, exception.what());
        return CreateSceneDocumentResult::Error;
    }
}

bool SaveSceneDocument(
    const SceneDocument& document,
    const std::filesystem::path& path,
    std::string* errorMessage)
{
    return WriteSceneDocument(document, path, errorMessage, true) == CreateSceneDocumentResult::Created;
}

CreateSceneDocumentResult CreateSceneDocument(
    const SceneDocument& document,
    const std::filesystem::path& path,
    std::string* errorMessage)
{
    return WriteSceneDocument(document, path, errorMessage, false);
}

bool LoadSceneDocument(
    const std::filesystem::path& path,
    SceneDocument& document,
    std::string* errorMessage)
{
    return LoadSceneDocument(path, document, errorMessage, LegacyLightingMigrationOptions{});
}

bool LoadSceneDocument(
    const std::filesystem::path& path,
    SceneDocument& document,
    std::string* errorMessage,
    const LegacyLightingMigrationOptions& migration)
{
    try
    {
        std::ifstream file(path, std::ios::binary);

        if (!file)
        {
            return Fail(errorMessage, "Failed to open scene file.");
        }

        Json json = ParseSceneJson(file);

        SceneDocument loaded = FromJson(json, migration);

        std::string validationError;

        if (!ValidateSceneDocument(loaded, &validationError))
        {
            return Fail(errorMessage, validationError);
        }

        document = std::move(loaded);

        if (errorMessage)
        {
            errorMessage->clear();
        }

        return true;
    }
    catch (const std::exception& exception)
    {
        return Fail(errorMessage, exception.what());
    }
}

bool LoadSceneDocumentForCooking(const std::filesystem::path& path, SceneDocument& document, size_t maxBytes, std::string* error)
{
    try
    {
        const auto size = std::filesystem::file_size(path);
        if(size > maxBytes || size > SIZE_MAX) throw std::runtime_error("Loose scene exceeds cooker admission.");
        std::ifstream stream(path,std::ios::binary);
        std::string bytes(static_cast<size_t>(size),'\0');
        if(!stream.read(bytes.data(),static_cast<std::streamsize>(bytes.size())) || stream.peek()!=std::char_traits<char>::eof())
            throw std::runtime_error("Cannot read stable bounded cooker input.");
        const auto json = ParseSceneJson(bytes);
        auto candidate = FromJson(json);
        if(!ValidateSceneDocument(candidate,error)) return false;
        document = std::move(candidate); return true;
    }
    catch(const std::exception& exception) { if(error) *error=exception.what(); return false; }
}
