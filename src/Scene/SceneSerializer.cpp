#include "Scene/SceneSerializer.h"

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

    // Schema 11 Light: exactly these fields, every field for every type. Type is a name so the
    // file stays readable; angles are radians like the Camera FOV.
    Json ToJson(const LightComponent& light)
    {
        return {
            {"enabled", light.enabled},
            {"type", LightTypeName(light.type)},
            {"color", ToJson(light.color)},
            {"intensity", light.intensity},
            {"range", light.range},
            {"innerConeAngle", light.innerConeAngle},
            {"outerConeAngle", light.outerConeAngle}
        };
    }

    LightComponent ReadLightComponent(const Json& value)
    {
        ValidateFields(value, {"enabled", "type", "color", "intensity", "range", "innerConeAngle", "outerConeAngle"});

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
        std::string error;

        if(!ValidateLightComponent(light, &error))
        {
            throw std::runtime_error(error);
        }

        return light;
    }

    Json ToJson(const CameraComponent& camera)
    {
        return {
            {"enabled", camera.enabled},
            {"projection", "Perspective"},
            {"verticalFov", camera.verticalFov},
            {"nearPlane", camera.nearPlane},
            {"farPlane", camera.farPlane},
            {"main", camera.main}
        };
    }

    CameraComponent ReadCameraComponent(const Json& value)
    {
        ValidateFields(value, {"enabled", "projection", "verticalFov", "nearPlane", "farPlane", "main"});

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
        }

        const auto& camera = document.camera;
        const auto& light = document.directionalLight;

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
                {"farPlane", camera.farPlane}
            }},
            {"directionalLight", {
                {"direction", ToJson(light.direction)},
                {"color", ToJson(light.color)},
                {"intensity", light.intensity},
                {"ambientStrength", light.ambientStrength},
                {"angularRadius", light.angularRadius}
            }}
        };
    }

    SceneDocument FromJson(const Json& value)
    {
        ValidateFields(value, {"schemaVersion", "id", "name", "camera", "directionalLight", "objects", "environmentReference"});
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
             schemaVersion != SceneDocument::kLightSchemaVersion))
        {
            throw std::runtime_error("Unsupported scene schema version.");
        }
        const bool legacySchema = schemaVersion == SceneDocument::kLegacySchemaVersion;
        const bool hierarchySchema = schemaVersion.get<std::uint32_t>() >= SceneDocument::kHierarchySchemaVersion;
        SceneDocument result;

        // Transitional readers preserve protected project content. New saves write v11;
        // no migration is written to disk merely by loading a historical document.
        result.id = value.at("id").get<std::string>();
        result.name = value.at("name").get<std::string>();
        result.environmentReference=value.value("environmentReference",std::string{});

        const Json& camera = value.at("camera");
        ValidateFields(camera, {"position", "target", "verticalFov", "aspectRatio", "nearPlane", "farPlane"});
        result.camera.position = ReadVector3(camera.at("position"));
        result.camera.target = ReadVector3(camera.at("target"));
        result.camera.verticalFov = ReadNumber(camera.at("verticalFov"), "Camera vertical FOV");
        result.camera.aspectRatio = ReadNumber(camera.at("aspectRatio"), "Camera aspect ratio");
        result.camera.nearPlane = ReadNumber(camera.at("nearPlane"), "Camera near plane");
        result.camera.farPlane = ReadNumber(camera.at("farPlane"), "Camera far plane");

        const Json& light = value.at("directionalLight");
        ValidateFields(light, {"direction", "color", "intensity", "ambientStrength", "angularRadius"});
        result.directionalLight.direction = ReadVector3(light.at("direction"));
        result.directionalLight.color = ReadVector3(light.at("color"));
        result.directionalLight.intensity = ReadNumber(light.at("intensity"), "Light intensity");
        result.directionalLight.ambientStrength =
            ReadNumber(light.at("ambientStrength"), "Light ambient strength");
        result.directionalLight.angularRadius =
            ReadNumber(light.at("angularRadius"), "Light angular radius");

        const Json& objects = value.at("objects");

        if (!objects.is_array())
        {
            throw std::runtime_error("Scene objects must be an array.");
        }

        for (const Json& item : objects)
        {
            ValidateFields(item, {"id", "parentID", "name", "transform", "meshReference", "materialReference",
                "materialSlotBindings", "physics", "triggerCollider", "characterController", "camera", "light"});
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
                    object.camera = ReadCameraComponent(item.at("camera"));
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
                    object.light = ReadLightComponent(item.at("light"));
                }
                catch(const std::exception& exception)
                {
                    throw std::runtime_error("Invalid Light for object " + object.id + ": " + exception.what());
                }
            }

            result.objects.push_back(std::move(object));
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
    try
    {
        std::ifstream file(path, std::ios::binary);

        if (!file)
        {
            return Fail(errorMessage, "Failed to open scene file.");
        }

        Json json = ParseSceneJson(file);

        SceneDocument loaded = FromJson(json);

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
