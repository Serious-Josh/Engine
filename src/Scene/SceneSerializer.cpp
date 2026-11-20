#include "Scene/SceneSerializer.h"
#include "Scene/LegacyLightingMigration.h"
#include "Scene/PhysicalLightingMigration.h"
#include "Scene/ColliderScaleMigration.h"
#include "Core/Log.h"
#include "Debug/StreamingTelemetry.h"
#include "assets/AssetID.h"

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

    // Builds the scene's JSON tree exactly as Json::parse does, and refuses any object that repeats a field (plain parsing
    // would silently keep the last one). M3.5: this replaces Json::parse with a parser callback, whose cost grew
    // quadratically with the scene (5.2 s instead of 0.37 s for an 8,410-object scene, minutes at 65,536 objects); the SAX
    // handler forwards to nlohmann's own DOM builder (nlohmann::detail, pinned at 3.12.0) and is linear.
    class DuplicateCheckingSceneParser
    {
        public:
        explicit DuplicateCheckingSceneParser(Json& result) :
            m_dom(result, true)
        {
        }

        bool null() { return m_dom.null(); }
        bool boolean(bool value) { return m_dom.boolean(value); }
        bool number_integer(Json::number_integer_t value) { return m_dom.number_integer(value); }
        bool number_unsigned(Json::number_unsigned_t value) { return m_dom.number_unsigned(value); }
        bool number_float(Json::number_float_t value, const std::string& text) { return m_dom.number_float(value, text); }
        bool string(std::string& value) { return m_dom.string(value); }
        bool binary(Json::binary_t& value) { return m_dom.binary(value); }
        bool start_array(std::size_t size) { return m_dom.start_array(size); }
        bool end_array() { return m_dom.end_array(); }

        bool start_object(std::size_t size)
        {
            m_objectFields.emplace_back();
            return m_dom.start_object(size);
        }

        bool end_object()
        {
            m_objectFields.pop_back();
            return m_dom.end_object();
        }

        bool key(std::string& value)
        {
            if(!m_objectFields.back().insert(value).second)
            {
                throw std::runtime_error("Duplicate scene field: " + value);
            }

            return m_dom.key(value);
        }

        bool parse_error(std::size_t position, const std::string& token, const nlohmann::detail::exception& error)
        {
            return m_dom.parse_error(position, token, error);
        }

        private:
        nlohmann::detail::json_sax_dom_parser<Json, nlohmann::detail::string_input_adapter_type> m_dom;
        std::vector<std::set<std::string>> m_objectFields;
    };

    Json ParseSceneJson(std::string_view bytes)
    {
        Json result;
        DuplicateCheckingSceneParser parser(result);
        Json::sax_parse(bytes.data(), bytes.data() + bytes.size(), &parser);
        return result;
    }

    // M3.5: reads a whole scene source file after checking its size against MaximumSceneSourceBytes, so an oversized file is
    // refused before anything is allocated for it.
    bool ReadSceneSource(const std::filesystem::path& path, std::string& bytes, std::string& error)
    {
        std::error_code sizeError;
        const auto size = std::filesystem::file_size(path, sizeError);

        if(sizeError)
        {
            error = "Failed to open scene file.";
            return false;
        }

        if(size > MaximumSceneSourceBytes || size > SIZE_MAX)
        {
            error = "Scene file is " + std::to_string(size) + " bytes; the largest admitted scene file is " +
                std::to_string(MaximumSceneSourceBytes) + " bytes.";
            return false;
        }

        std::ifstream file(path, std::ios::binary);
        bytes.assign(static_cast<std::size_t>(size), '\0');

        if(!file || !file.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
            file.peek() != std::char_traits<char>::eof())
        {
            error = "Failed to read scene file (it may have changed while being read).";
            return false;
        }

        return true;
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
            {"airParticipation", Physics::MovingSolidParticipationName(component.airParticipation)},
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
        const bool participationSchema = schemaVersion >= SceneDocument::kMovingSolidParticipationSchemaVersion;

        if(participationSchema)
        {
            ValidateFields(value, {"bodyType", "colliderType", "boxDimensions", "sphereRadius", "capsuleRadius",
                "capsuleCylinderHeight", "capsuleLocalRotation", "colliderLocalPositionOffset", "density", "friction",
                "restitution", "generateContactEvents", "airParticipation"});
        }
        else if(contactEventsSchema)
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

        // R1.5: v1-v26 keep Automatic; v27 requires one of the three names (no numbers, no other spelling).
        if(participationSchema)
        {
            const Json& participation = value.at("airParticipation");

            if(!participation.is_string() ||
                !Physics::TryParseMovingSolidParticipation(participation.get<std::string>(), component.airParticipation))
            {
                throw std::runtime_error("Physics airParticipation must be Automatic, Always or Never.");
            }
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

            if(object.airMotor)
            {
                const AirMotorComponent& motor = *object.airMotor;
                objects.back()["airMotor"] = {
                    {"enabled", motor.enabled},
                    {"type", AirMotorTypeName(motor.type)},
                    {"strength", motor.strength},
                    {"falloff", motor.falloff},
                    {"halfExtents", ToJson(motor.halfExtents)},
                    {"radius", motor.radius},
                    {"length", motor.length},
                    {"inwardPull", motor.inwardPull},
                    {"coupling", motor.coupling},
                    {"emissionOrigin", ToJson(motor.emissionOrigin)},
                    {"emissionSemiAxes", ToJson(motor.emissionSemiAxes)},
                    {"emissionRotation", ToJson(motor.emissionRotation)}
                };
            }

            if(object.mistEmitter)
            {
                const MistEmitterComponent& emitter = *object.mistEmitter;
                objects.back()["mistEmitter"] = {
                    {"enabled", emitter.enabled},
                    {"shape", MistEmitterShapeName(emitter.shape)},
                    {"emissionOrigin", ToJson(emitter.emissionOrigin)},
                    {"emissionRotation", ToJson(emitter.emissionRotation)},
                    {"radius", emitter.radius},
                    {"halfExtents", ToJson(emitter.halfExtents)},
                    {"rate", emitter.rate},
                    {"color", ToJson(emitter.color)},
                    {"density", emitter.density},
                    {"temperature", emitter.temperature},
                    {"mode", MistEmitterModeName(emitter.mode)},
                    {"emissionRate", emitter.emissionRate},
                    {"jetSpeed", emitter.jetSpeed},
                    {"temperatureResponseRate", emitter.temperatureResponseRate},
                    {"targetDensity", emitter.targetDensity},
                    {"replenishmentRate", emitter.replenishmentRate},
                    {"fillHeight", emitter.fillHeight},
                    {"edgeSoftness", emitter.edgeSoftness},
                    {"volumeTemperatureRate", emitter.volumeTemperatureRate}
                };
            }

            if(object.voxel)
            {
                objects.back()["voxel"] = {
                    {"participation", VoxelParticipationToString(object.voxel->participation)},
                    {"orientation", VoxelOrientationToString(object.voxel->orientation)}
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


        Json scene = {
            {"schemaVersion", SceneDocument::kSchemaVersion},
            {"kind", document.kind == SceneKind::World ? "world" : "local"},
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

        // W2: a Local Level always states its owner (null when unassigned); a World always lists its members.
        if(document.kind == SceneKind::Local)
        {
            scene["owningWorld"] = document.owningWorld.empty() ? Json(nullptr) : Json(document.owningWorld);
            const auto& overrides = document.environmentOverrides;
            scene["environmentOverrides"] = {{"sky", overrides.sky}, {"exposure", overrides.exposure}, {"haze", overrides.haze},
                {"mist", overrides.mist}, {"airEnvironment", overrides.airEnvironment}, {"wind", overrides.wind}};
        }
        else
        {
            Json members = Json::array();

            for(const auto& member : document.members)
            {
                members.push_back({{"level", member.level}, {"placement", {{"position", ToJson(member.placement.position)},
                    {"rotation", ToJson(member.placement.rotation)}}}});
            }

            scene["members"] = std::move(members);
        }

        if(document.voxelBake)
        {
            const VoxelBakeSettings& bake = *document.voxelBake;
            scene["voxelBake"] = {
                {"baseExponent", bake.baseExponent},
                {"regionBricks", bake.regionBricks},
                {"narrowBand", bake.narrowBand},
                {"coarseClamp", bake.coarseClamp},
                {"manifest", bake.manifestReference}
            };
        }

        // AIR.4F F4.4.1: every field, so a saved scene never depends on the defaults of a later version.
        if(document.mistRendering)
        {
            const MistRenderSettings& mist = *document.mistRendering;
            scene["mistRendering"] = {
                {"opacity", mist.opacity},
                {"worley", {{"enabled", mist.detail}, {"strength", mist.detailStrength}, {"scale", mist.detailScale}}},
                {"curl", {{"enabled", mist.curl}, {"strength", mist.curlStrength}, {"scale", mist.curlScale}, {"flowInfluence", mist.curlFlow}}},
                {"wisps", {{"enabled", mist.wisps}, {"strength", mist.wispStrength}, {"scale", mist.wispScale}}},
                {"densityDetail", {{"enabled", mist.densityDetail}, {"strength", mist.densityDetailStrength}}},
                {"lighting", {{"enabled", mist.lighting}, {"sunShadows", mist.sunShadows}, {"scattering", mist.scattering},
                    {"anisotropy", mist.anisotropy}, {"ambient", mist.ambient}, {"scatteringTint", mist.scatteringTint},
                    {"multipleScattering", mist.multipleScattering}, {"indirectScattering", mist.indirectScattering}}},
                {"nearClearance", {{"distance", mist.nearDistance}, {"transition", mist.nearTransition}}}
            };
        }

        // AIR.4F F4.5.1b: every field, as for mistRendering.
        if(document.airEnvironment)
        {
            scene["airEnvironment"] = {
                {"ambientTemperature", document.airEnvironment->ambientTemperature},
                {"relaxationTime", document.airEnvironment->relaxationTime}
            };
        }

        // R1.1: every field, as for airEnvironment.
        if(document.airSimulation)
        {
            const AirSimulationSettings& simulation = *document.airSimulation;
            scene["airSimulation"] = {
                {"enabled", simulation.enabled},
                {"pressureProjection", simulation.pressureProjection},
                {"buoyancy", simulation.buoyancy},
                {"buoyancyGravity", simulation.buoyancyGravity},
                {"maximumStepsPerFrame", simulation.maximumStepsPerFrame},
                {"ambientVelocity", ToJson(simulation.ambientVelocity)},
                {"dampingRate", simulation.dampingRate},
                {"maximumMotorSpeed", simulation.maximumMotorSpeed},
                {"mistDissipationRate", simulation.mistDissipationRate},
                {"movingSolids", simulation.movingSolids}
            };
        }

        // AIR.4F.2d.3: every field.
        if(document.environmentalHaze)
        {
            const EnvironmentalHazeSettings& haze = *document.environmentalHaze;
            scene["environmentalHaze"] = {
                {"enabled", haze.enabled},
                {"attenuationDistance", haze.attenuationDistance},
                {"color", haze.color},
                {"nearDistance", haze.nearDistance},
                {"nearTransition", haze.nearTransition},
                {"ambient", haze.ambient},
                {"heightBounds", {{"enabled", haze.heightBounds}, {"baseHeight", haze.baseHeight}, {"maxHeight", haze.maxHeight},
                    {"transition", haze.heightTransition}}}
            };
        }

        return scene;
    }

    SceneDocument FromJson(const Json& value, const LegacyLightingMigrationOptions& migration = {})
    {
        ValidateFields(value, {"schemaVersion", "kind", "owningWorld", "members", "environmentOverrides", "id", "name", "camera", "directionalLight", "objects", "environmentReference", "lightingSettings",
            "voxelBake", "mistRendering", "airEnvironment", "airSimulation", "environmentalHaze"});
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
             schemaVersion != SceneDocument::kAcousticsSchemaVersion &&
             schemaVersion != SceneDocument::kVoxelSchemaVersion &&
             schemaVersion != SceneDocument::kAirMotorSchemaVersion &&
             schemaVersion != SceneDocument::kAirMotorEmissionSchemaVersion &&
             schemaVersion != SceneDocument::kAirMotorShapeSchemaVersion &&
             schemaVersion != SceneDocument::kMistEmitterSchemaVersion &&
             schemaVersion != SceneDocument::kMistRenderingSchemaVersion &&
             schemaVersion != SceneDocument::kMistEmitterModeSchemaVersion &&
             schemaVersion != SceneDocument::kAirSimulationSchemaVersion &&
             schemaVersion != SceneDocument::kMovingSolidParticipationSchemaVersion &&
             schemaVersion != SceneDocument::kSceneKindSchemaVersion &&
             schemaVersion != SceneDocument::kMembershipSchemaVersion &&
             schemaVersion != SceneDocument::kEnvironmentOverridesSchemaVersion))
        {
            throw std::runtime_error("Unsupported scene schema version.");
        }
        const bool legacySchema = schemaVersion == SceneDocument::kLegacySchemaVersion;
        const bool hierarchySchema = schemaVersion.get<std::uint32_t>() >= SceneDocument::kHierarchySchemaVersion;
        SceneDocument result;

        // Transitional readers preserve protected project content. New saves write v13;
        // no migration is written to disk merely by loading a historical document.
        result.id = value.at("id").get<std::string>();

        // W1.1: from schema 28 the kind is explicit and the id is a canonical GUID. Older documents are Local Levels whose id is kept
        // exactly as written (never canonicalized on load; only migration changes it).
        if(schemaVersion.get<std::uint32_t>() >= SceneDocument::kSceneKindSchemaVersion)
        {
            const auto kind = value.find("kind");

            if(kind == value.end())
            {
                throw std::runtime_error("Scene schema " + std::to_string(SceneDocument::kSceneKindSchemaVersion) +
                    " requires a kind (\"local\" or \"world\").");
            }

            if(!kind->is_string() || (*kind != "local" && *kind != "world"))
            {
                throw std::runtime_error("Invalid scene kind " + kind->dump() + "; expected \"local\" or \"world\".");
            }

            result.kind = *kind == "world" ? SceneKind::World : SceneKind::Local;

            if(!AssetID::Parse(result.id))
            {
                throw std::runtime_error("Scene ID '" + result.id + "' is not a canonical GUID (lowercase 8-4-4-4-12 hex), "
                    "required from scene schema " + std::to_string(SceneDocument::kSceneKindSchemaVersion) + ".");
            }
        }
        else if(value.contains("kind"))
        {
            throw std::runtime_error("Unsupported scene field: kind (introduced in scene schema " +
                std::to_string(SceneDocument::kSceneKindSchemaVersion) + ").");
        }

        // W2: membership. From schema 29 a Local Level states its owning World (a GUID or null) and a World lists its members;
        // the other kind's field is refused. Earlier documents carry neither.
        if(schemaVersion.get<std::uint32_t>() >= SceneDocument::kMembershipSchemaVersion)
        {
            const char* const required = result.kind == SceneKind::World ? "members" : "owningWorld";
            const char* const forbidden = result.kind == SceneKind::World ? "owningWorld" : "members";

            if(!value.contains(required))
            {
                throw std::runtime_error(std::string("Scene schema ") + std::to_string(SceneDocument::kMembershipSchemaVersion) +
                    " requires \"" + required + "\" in a " + (result.kind == SceneKind::World ? "World." : "Local Level."));
            }

            if(value.contains(forbidden))
            {
                throw std::runtime_error(std::string("A ") + (result.kind == SceneKind::World ? "World" : "Local Level") +
                    " cannot have \"" + forbidden + "\".");
            }

            if(result.kind == SceneKind::Local)
            {
                const auto& owner = value.at("owningWorld");

                if(!owner.is_null() && !owner.is_string())
                {
                    throw std::runtime_error("owningWorld must be a World's asset ID or null.");
                }

                result.owningWorld = owner.is_null() ? std::string{} : owner.get<std::string>();

                if(owner.is_string() && !AssetID::Parse(result.owningWorld))
                {
                    throw std::runtime_error("owningWorld '" + result.owningWorld + "' is not a canonical GUID.");
                }
            }
            else
            {
                const auto& members = value.at("members");

                if(!members.is_array())
                {
                    throw std::runtime_error("members must be an array.");
                }

                for(const auto& item : members)
                {
                    ValidateFields(item, {"level", "placement"});
                    const auto& placement = item.at("placement");
                    // Placement is translation and rotation only: a scale (or any other field) is refused.
                    ValidateFields(placement, {"position", "rotation"});
                    SceneMembership member;

                    if(!item.at("level").is_string())
                    {
                        throw std::runtime_error("A member's level must be a Local Level's asset ID.");
                    }

                    member.level = item.at("level").get<std::string>();
                    member.placement.position = ReadVector3(placement.at("position"));
                    member.placement.rotation = ReadVector3(placement.at("rotation"));
                    result.members.push_back(std::move(member));
                }
            }
        }
        else if(value.contains("owningWorld") || value.contains("members"))
        {
            throw std::runtime_error("Unsupported scene field: owningWorld/members (introduced in scene schema " +
                std::to_string(SceneDocument::kMembershipSchemaVersion) + ").");
        }

        // W3: a Local Level's six environment override switches, required from schema 30 (exact keys, booleans); Worlds have none.
        // Earlier Local Levels keep the default (every group enabled), which is how they behaved before W3.
        if(schemaVersion.get<std::uint32_t>() >= SceneDocument::kEnvironmentOverridesSchemaVersion && result.kind == SceneKind::Local)
        {
            const auto overrides = value.find("environmentOverrides");

            if(overrides == value.end())
            {
                throw std::runtime_error("Scene schema " + std::to_string(SceneDocument::kEnvironmentOverridesSchemaVersion) +
                    " requires \"environmentOverrides\" in a Local Level.");
            }

            ValidateFields(*overrides, {"sky", "exposure", "haze", "mist", "airEnvironment", "wind"});
            const auto flag = [&](const char* name)
            {
                const auto found = overrides->find(name);

                if(found == overrides->end() || !found->is_boolean())
                {
                    throw std::runtime_error(std::string("environmentOverrides.") + name + " must be true or false.");
                }

                return found->get<bool>();
            };

            result.environmentOverrides = {flag("sky"), flag("exposure"), flag("haze"), flag("mist"), flag("airEnvironment"), flag("wind")};
        }
        else if(value.contains("environmentOverrides"))
        {
            throw std::runtime_error(result.kind == SceneKind::World ? "A World cannot have \"environmentOverrides\"." :
                "Unsupported scene field: environmentOverrides (introduced in scene schema " +
                std::to_string(SceneDocument::kEnvironmentOverridesSchemaVersion) + ").");
        }

        result.name = value.at("name").get<std::string>();
        result.environmentReference=value.value("environmentReference",std::string{});

        if(value.contains("voxelBake"))
        {
            if(schemaVersion.get<std::uint32_t>() < SceneDocument::kVoxelSchemaVersion)
            {
                throw std::runtime_error("Voxel bake settings require scene schema version 19.");
            }

            const Json& voxelBake = value.at("voxelBake");
            ValidateFields(voxelBake, {"baseExponent", "regionBricks", "narrowBand", "coarseClamp", "manifest"});
            VoxelBakeSettings bake;
            bake.baseExponent = voxelBake.at("baseExponent").get<std::int32_t>();
            bake.regionBricks = voxelBake.at("regionBricks").get<std::uint32_t>();
            bake.narrowBand = voxelBake.at("narrowBand").get<std::uint32_t>();
            bake.coarseClamp = voxelBake.at("coarseClamp").get<std::uint32_t>();
            bake.manifestReference = voxelBake.at("manifest").get<std::string>();
            std::string voxelError;

            if(!ValidateVoxelBakeSettings(bake, &voxelError))
            {
                throw std::runtime_error("Invalid voxel bake settings: " + voxelError);
            }

            result.voxelBake = std::move(bake);
        }

        // AIR.4F F4.4.1: a missing field keeps its default; a value that is not a finite number (or not a boolean) keeps the default
        // too, and a number out of its range is clamped, so no file can hand the renderer an invalid constant.
        if(value.contains("mistRendering"))
        {
            if(schemaVersion.get<std::uint32_t>() < SceneDocument::kMistRenderingSchemaVersion)
            {
                throw std::runtime_error("Mist rendering settings require scene schema version 24.");
            }

            const Json& block = value.at("mistRendering");

            if(!block.is_object())
            {
                throw std::runtime_error("Mist rendering settings must be an object.");
            }

            // AIR.4F F4.4.2 added "wisps" and F4.4.2e "densityDetail" to the schema 24 block (absent: off, as before).
            // AIR.4F.2d.3b added "nearClearance" (absent: off).
            ValidateFields(block, {"opacity", "worley", "curl", "wisps", "densityDetail", "lighting", "nearClearance"});
            MistRenderSettings mist;
            const auto readNumber = [](const Json& group, const char* key, float& target)
            {
                if(group.contains(key) && group.at(key).is_number())
                {
                    target = group.at(key).get<float>();
                }
            };
            const auto readFlag = [](const Json& group, const char* key, bool& target)
            {
                if(group.contains(key) && group.at(key).is_boolean())
                {
                    target = group.at(key).get<bool>();
                }
            };
            const auto readGroup = [&](const char* key, std::initializer_list<std::string_view> fields) -> const Json*
            {
                if(!block.contains(key))
                {
                    return nullptr;
                }

                if(!block.at(key).is_object())
                {
                    throw std::runtime_error(std::string("Mist rendering '") + key + "' must be an object.");
                }

                ValidateFields(block.at(key), fields);
                return &block.at(key);
            };

            readNumber(block, "opacity", mist.opacity);

            if(const Json* worley = readGroup("worley", {"enabled", "strength", "scale"}))
            {
                readFlag(*worley, "enabled", mist.detail);
                readNumber(*worley, "strength", mist.detailStrength);
                readNumber(*worley, "scale", mist.detailScale);
            }

            if(const Json* curl = readGroup("curl", {"enabled", "strength", "scale", "flowInfluence"}))
            {
                readFlag(*curl, "enabled", mist.curl);
                readNumber(*curl, "strength", mist.curlStrength);
                readNumber(*curl, "scale", mist.curlScale);
                readNumber(*curl, "flowInfluence", mist.curlFlow);
            }

            if(const Json* wisps = readGroup("wisps", {"enabled", "strength", "scale"}))
            {
                readFlag(*wisps, "enabled", mist.wisps);
                readNumber(*wisps, "strength", mist.wispStrength);
                readNumber(*wisps, "scale", mist.wispScale);
            }

            if(const Json* densityDetail = readGroup("densityDetail", {"enabled", "strength"}))
            {
                readFlag(*densityDetail, "enabled", mist.densityDetail);
                readNumber(*densityDetail, "strength", mist.densityDetailStrength);
            }

            // AIR.4F.2d.2 added "scatteringTint" (three numbers) and "multipleScattering", AIR.4F.2d.3e "indirectScattering"; absent,
            // their neutral defaults (so an older scene keeps its look).
            if(const Json* lighting = readGroup("lighting", {"enabled", "sunShadows", "scattering", "anisotropy", "ambient", "scatteringTint",
                "multipleScattering", "indirectScattering"}))
            {
                readFlag(*lighting, "enabled", mist.lighting);
                readFlag(*lighting, "sunShadows", mist.sunShadows);
                readNumber(*lighting, "scattering", mist.scattering);
                readNumber(*lighting, "anisotropy", mist.anisotropy);
                readNumber(*lighting, "ambient", mist.ambient);
                readNumber(*lighting, "multipleScattering", mist.multipleScattering);
                readNumber(*lighting, "indirectScattering", mist.indirectScattering);

                // Like every number in the block, a malformed tint keeps its default rather than failing the load.
                if(lighting->contains("scatteringTint") && lighting->at("scatteringTint").is_array() && lighting->at("scatteringTint").size() == 3)
                {
                    for(std::size_t channel = 0; channel < 3; ++channel)
                    {
                        const Json& value = lighting->at("scatteringTint")[channel];

                        if(value.is_number())
                        {
                            mist.scatteringTint[channel] = value.get<float>();
                        }
                    }
                }
            }

            if(const Json* clearance = readGroup("nearClearance", {"distance", "transition"}))
            {
                readNumber(*clearance, "distance", mist.nearDistance);
                readNumber(*clearance, "transition", mist.nearTransition);
            }

            result.mistRendering = SanitizeMistRenderSettings(mist);
        }

        // AIR.4F F4.5.1b: the air environment, in schema 24 files (as F4.4.2 added "wisps"). A missing field keeps its default; a value
        // that is not a number, or is out of range, refuses the scene (it drives the simulation, so it is never silently changed).
        // AIR.4F.2d.3: the environmental haze (schema 25). Missing fields keep their defaults; invalid values are refused.
        if(value.contains("environmentalHaze"))
        {
            if(schemaVersion.get<std::uint32_t>() < SceneDocument::kMistEmitterModeSchemaVersion)
            {
                throw std::runtime_error("Environmental haze requires scene schema version 25.");
            }

            const Json& block = value.at("environmentalHaze");

            if(!block.is_object())
            {
                throw std::runtime_error("Environmental haze must be an object.");
            }

            ValidateFields(block, {"enabled", "attenuationDistance", "color", "nearDistance", "nearTransition", "ambient", "heightBounds"});
            EnvironmentalHazeSettings haze;
            // AIR.4F.2d.3a: a block saved before the ambient contribution existed scattered the sun alone, and keeps doing so.
            haze.ambient = 0.0f;

            if(block.contains("enabled"))
            {
                if(!block.at("enabled").is_boolean())
                {
                    throw std::runtime_error("Environmental haze 'enabled' must be a boolean.");
                }

                haze.enabled = block.at("enabled").get<bool>();
            }

            if(block.contains("attenuationDistance"))
            {
                haze.attenuationDistance = ReadNumber(block.at("attenuationDistance"), "Fog attenuation distance");
            }

            if(block.contains("color"))
            {
                const Json& color = block.at("color");

                if(!color.is_array() || color.size() != 3)
                {
                    throw std::runtime_error("Haze colour must be three numbers.");
                }

                for(std::size_t channel = 0; channel < 3; ++channel)
                {
                    haze.color[channel] = ReadNumber(color[channel], "Haze colour");
                }
            }

            if(block.contains("nearDistance"))
            {
                haze.nearDistance = ReadNumber(block.at("nearDistance"), "Near fog distance");
            }

            if(block.contains("nearTransition"))
            {
                haze.nearTransition = ReadNumber(block.at("nearTransition"), "Near fog transition");
            }

            if(block.contains("ambient"))
            {
                haze.ambient = ReadNumber(block.at("ambient"), "Haze ambient contribution");
            }

            // AIR.4F.2d.3b: the height bounds (absent: off). Every block written since has the group, so its absence also marks a block
            // from before the clearances were separated.
            const bool sharedClearance = !block.contains("heightBounds");

            if(!sharedClearance)
            {
                const Json& bounds = block.at("heightBounds");

                if(!bounds.is_object())
                {
                    throw std::runtime_error("Haze height bounds must be an object.");
                }

                ValidateFields(bounds, {"enabled", "baseHeight", "maxHeight", "transition"});

                if(bounds.contains("enabled"))
                {
                    if(!bounds.at("enabled").is_boolean())
                    {
                        throw std::runtime_error("Haze height bounds 'enabled' must be a boolean.");
                    }

                    haze.heightBounds = bounds.at("enabled").get<bool>();
                }

                if(bounds.contains("baseHeight"))
                {
                    haze.baseHeight = ReadNumber(bounds.at("baseHeight"), "Haze base height");
                }

                if(bounds.contains("maxHeight"))
                {
                    haze.maxHeight = ReadNumber(bounds.at("maxHeight"), "Haze max height");
                }

                if(bounds.contains("transition"))
                {
                    haze.heightTransition = ReadNumber(bounds.at("transition"), "Haze height transition");
                }
            }

            std::string hazeError;

            if(!ValidateEnvironmentalHazeSettings(haze, &hazeError))
            {
                throw std::runtime_error("Invalid environmental haze: " + hazeError);
            }

            result.environmentalHaze = haze;

            // Before AIR.4F.2d.3b the haze block's clearance also cleared the AIR mist. Such a scene keeps that look: the AIR mist gets
            // the same clearance (its rendering block is created with the defaults if the scene had none).
            if(sharedClearance && haze.nearDistance > 0.0f)
            {
                MistRenderSettings mist = result.mistRendering.value_or(MistRenderSettings{});
                mist.nearDistance = haze.nearDistance;
                mist.nearTransition = haze.nearTransition;
                result.mistRendering = SanitizeMistRenderSettings(mist);
            }
        }

        if(value.contains("airEnvironment"))
        {
            if(schemaVersion.get<std::uint32_t>() < SceneDocument::kMistRenderingSchemaVersion)
            {
                throw std::runtime_error("Air environment settings require scene schema version 24.");
            }

            const Json& block = value.at("airEnvironment");

            if(!block.is_object())
            {
                throw std::runtime_error("Air environment settings must be an object.");
            }

            ValidateFields(block, {"ambientTemperature", "relaxationTime"});
            AirEnvironmentSettings environment;

            if(block.contains("ambientTemperature"))
            {
                environment.ambientTemperature = ReadNumber(block.at("ambientTemperature"), "Ambient temperature");
            }

            if(block.contains("relaxationTime"))
            {
                environment.relaxationTime = ReadNumber(block.at("relaxationTime"), "Thermal relaxation time");
            }

            std::string environmentError;

            if(!ValidateAirEnvironmentSettings(environment, &environmentError))
            {
                throw std::runtime_error("Invalid air environment: " + environmentError);
            }

            result.airEnvironment = environment;
        }

        // R1.1: missing fields keep their defaults; unknown fields, wrong types and out-of-range values refuse the scene.
        if(value.contains("airSimulation"))
        {
            if(schemaVersion.get<std::uint32_t>() < SceneDocument::kAirSimulationSchemaVersion)
            {
                throw std::runtime_error("AIR simulation settings require scene schema version 26.");
            }

            const Json& block = value.at("airSimulation");

            if(!block.is_object())
            {
                throw std::runtime_error("AIR simulation settings must be an object.");
            }

            // R1.5: movingSolids exists from schema 27; a schema 26 block carrying it is refused, as any unknown field.
            if(schemaVersion.get<std::uint32_t>() >= SceneDocument::kMovingSolidParticipationSchemaVersion)
            {
                ValidateFields(block, {"enabled", "pressureProjection", "buoyancy", "buoyancyGravity", "maximumStepsPerFrame",
                    "ambientVelocity", "dampingRate", "maximumMotorSpeed", "mistDissipationRate", "movingSolids"});
            }
            else
            {
                ValidateFields(block, {"enabled", "pressureProjection", "buoyancy", "buoyancyGravity", "maximumStepsPerFrame",
                    "ambientVelocity", "dampingRate", "maximumMotorSpeed", "mistDissipationRate"});
            }

            AirSimulationSettings simulation;

            const auto readFlag = [&](const char* field, bool& target)
            {
                if(!block.contains(field))
                {
                    return;
                }

                if(!block.at(field).is_boolean())
                {
                    throw std::runtime_error(std::string("AIR simulation ") + field + " must be a boolean.");
                }

                target = block.at(field).get<bool>();
            };
            const auto readFloat = [&](const char* field, float& target)
            {
                if(block.contains(field))
                {
                    target = ReadNumber(block.at(field), field);
                }
            };

            readFlag("enabled", simulation.enabled);
            readFlag("pressureProjection", simulation.pressureProjection);
            readFlag("buoyancy", simulation.buoyancy);
            readFlag("movingSolids", simulation.movingSolids);
            readFloat("buoyancyGravity", simulation.buoyancyGravity);
            readFloat("dampingRate", simulation.dampingRate);
            readFloat("maximumMotorSpeed", simulation.maximumMotorSpeed);
            readFloat("mistDissipationRate", simulation.mistDissipationRate);

            if(block.contains("maximumStepsPerFrame"))
            {
                if(!block.at("maximumStepsPerFrame").is_number_integer())
                {
                    throw std::runtime_error("AIR simulation maximumStepsPerFrame must be an integer.");
                }

                // Read wide, so a huge value is refused by validation instead of wrapping into range.
                const std::int64_t steps = block.at("maximumStepsPerFrame").get<std::int64_t>();
                simulation.maximumStepsPerFrame = steps == 1 || steps == 2 ? static_cast<int>(steps) : 0;
            }

            if(block.contains("ambientVelocity"))
            {
                simulation.ambientVelocity = ReadVector3(block.at("ambientVelocity"));
            }

            std::string simulationError;

            if(!ValidateAirSimulationSettings(simulation, &simulationError))
            {
                throw std::runtime_error("Invalid AIR simulation settings: " + simulationError);
            }

            result.airSimulation = simulation;
        }

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
                "audioSource", "audioListener", "acoustics", "probeVolume", "voxel", "airMotor", "mistEmitter"});
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

            if(item.contains("airMotor"))
            {
                if(schemaVersion.get<std::uint32_t>() < SceneDocument::kAirMotorSchemaVersion)
                {
                    throw std::runtime_error("Air Motors require scene schema version 20.");
                }

                const auto& value = item.at("airMotor");
                // AIR.4D D2 emission fields: required from schema 21. Schema 20 motors take the defaults (the origin at the object, a
                // point emission), which is where AIR.3 motors were centred. AIR.4D D2.1 replaces the emission radius with the
                // emission shape from schema 22; older motors are migrated below.
                const bool emissionSchema = schemaVersion.get<std::uint32_t>() >= SceneDocument::kAirMotorEmissionSchemaVersion;
                const bool shapeSchema = schemaVersion.get<std::uint32_t>() >= SceneDocument::kAirMotorShapeSchemaVersion;

                if(shapeSchema)
                {
                    ValidateFields(value, {"enabled", "type", "strength", "falloff", "halfExtents", "radius", "length", "inwardPull", "coupling",
                        "emissionOrigin", "emissionSemiAxes", "emissionRotation"});
                }
                else if(emissionSchema)
                {
                    ValidateFields(value, {"enabled", "type", "strength", "falloff", "halfExtents", "radius", "length", "inwardPull", "coupling",
                        "emissionOrigin", "emissionRadius"});
                }
                else
                {
                    ValidateFields(value, {"enabled", "type", "strength", "falloff", "halfExtents", "radius", "length", "inwardPull", "coupling"});
                }

                if(!value.at("enabled").is_boolean() || !value.at("type").is_string())
                {
                    throw std::runtime_error("Air Motor enabled must be a boolean and type a name.");
                }

                AirMotorComponent motor;
                motor.enabled = value.at("enabled").get<bool>();

                if(!TryParseAirMotorType(value.at("type").get<std::string>(), motor.type))
                {
                    throw std::runtime_error("Air Motor type must be Directional, Omni, Vortex, Moving or Cylinder.");
                }

                motor.strength = ReadNumber(value.at("strength"), "Air Motor strength");
                motor.falloff = ReadNumber(value.at("falloff"), "Air Motor falloff");
                motor.halfExtents = ReadVector3(value.at("halfExtents"));
                motor.radius = ReadNumber(value.at("radius"), "Air Motor radius");
                motor.length = ReadNumber(value.at("length"), "Air Motor length");
                motor.inwardPull = ReadNumber(value.at("inwardPull"), "Air Motor inward pull");
                motor.coupling = ReadNumber(value.at("coupling"), "Air Motor coupling");

                if(emissionSchema)
                {
                    motor.emissionOrigin = ReadVector3(value.at("emissionOrigin"));
                }

                if(shapeSchema)
                {
                    motor.emissionSemiAxes = ReadVector3(value.at("emissionSemiAxes"));
                    motor.emissionRotation = ReadVector3(value.at("emissionRotation"));
                }
                else
                {
                    // Schema 20 had no emission radius: every AIR.3 motor emitted from a point.
                    const float legacyEmissionRadius = emissionSchema ? ReadNumber(value.at("emissionRadius"), "Air Motor emission radius") : 0.0f;

                    if(!std::isfinite(legacyEmissionRadius) || legacyEmissionRadius < 0.0f || legacyEmissionRadius > AirMotorComponent::MaximumSize)
                    {
                        throw std::runtime_error("Invalid Air Motor: emission radius must be finite, from 0 to 256 m.");
                    }

                    if(MigrateLegacyAirMotorEmission(motor, legacyEmissionRadius))
                    {
                        LogAirMotorReconfiguration(object.name, object.id, motor.type,
                            "schema " + std::to_string(schemaVersion.get<std::uint32_t>()));
                    }
                }

                std::string motorError;

                if(!ValidateAirMotorComponent(motor, &motorError))
                {
                    throw std::runtime_error("Invalid Air Motor: " + motorError);
                }

                object.airMotor = motor;
            }

            if(item.contains("mistEmitter"))
            {
                if(schemaVersion.get<std::uint32_t>() < SceneDocument::kMistEmitterSchemaVersion)
                {
                    throw std::runtime_error("Mist Emitters require scene schema version 23.");
                }

                const auto& value = item.at("mistEmitter");
                // AIR.4F F4.5.1b added "temperature" (absent: 0, no heating). AIR.4F.2b (schema 25) added "mode", "emissionRate" and
                // "jetSpeed"; absent, an emitter is Legacy with the defaults for the others, exactly as before modes existed.
                ValidateFields(value, {"enabled", "shape", "emissionOrigin", "emissionRotation", "radius", "halfExtents", "rate", "color",
                    "density", "temperature", "mode", "emissionRate", "jetSpeed", "temperatureResponseRate", "targetDensity",
                    "replenishmentRate", "fillHeight", "edgeSoftness", "volumeTemperatureRate"});
                // AIR.4F.2b.1 added "temperatureResponseRate" and AIR.4F.2c the Volume fields, all within schema 25 (absent: the defaults).
                bool modeFields = false;

                for(const char* name : {"mode", "emissionRate", "jetSpeed", "temperatureResponseRate", "targetDensity", "replenishmentRate",
                    "fillHeight", "edgeSoftness", "volumeTemperatureRate"})
                {
                    modeFields = modeFields || value.contains(name);
                }

                if(modeFields && schemaVersion.get<std::uint32_t>() < SceneDocument::kMistEmitterModeSchemaVersion)
                {
                    throw std::runtime_error("Mist Emitter modes require scene schema version 25.");
                }

                if(!value.at("enabled").is_boolean() || !value.at("shape").is_string())
                {
                    throw std::runtime_error("Mist Emitter enabled must be a boolean and shape a name.");
                }

                MistEmitterComponent emitter;
                emitter.enabled = value.at("enabled").get<bool>();

                if(!TryParseMistEmitterShape(value.at("shape").get<std::string>(), emitter.shape))
                {
                    throw std::runtime_error("Mist Emitter shape must be Sphere or Box.");
                }

                emitter.emissionOrigin = ReadVector3(value.at("emissionOrigin"));
                emitter.emissionRotation = ReadVector3(value.at("emissionRotation"));
                emitter.radius = ReadNumber(value.at("radius"), "Mist Emitter radius");
                emitter.halfExtents = ReadVector3(value.at("halfExtents"));
                emitter.rate = ReadNumber(value.at("rate"), "Mist Emitter rate");
                emitter.color = ReadVector3(value.at("color"));
                emitter.density = ReadNumber(value.at("density"), "Mist Emitter density");

                if(value.contains("temperature"))
                {
                    emitter.temperature = ReadNumber(value.at("temperature"), "Mist Emitter temperature");
                }

                if(value.contains("mode"))
                {
                    if(!value.at("mode").is_string() || !TryParseMistEmitterMode(value.at("mode").get<std::string>(), emitter.mode))
                    {
                        throw std::runtime_error("Mist Emitter mode must be Legacy, Point or Volume.");
                    }
                }

                if(value.contains("emissionRate"))
                {
                    emitter.emissionRate = ReadNumber(value.at("emissionRate"), "Mist Emitter emission rate");
                }

                if(value.contains("jetSpeed"))
                {
                    emitter.jetSpeed = ReadNumber(value.at("jetSpeed"), "Mist Emitter jet speed");
                }

                if(value.contains("temperatureResponseRate"))
                {
                    emitter.temperatureResponseRate = ReadNumber(value.at("temperatureResponseRate"), "Mist Emitter temperature response rate");
                }

                // AIR.4F.2c: the Volume fields.
                const std::pair<const char*, float*> volumeFields[] = {{"targetDensity", &emitter.targetDensity},
                    {"replenishmentRate", &emitter.replenishmentRate}, {"fillHeight", &emitter.fillHeight}, {"edgeSoftness", &emitter.edgeSoftness},
                    {"volumeTemperatureRate", &emitter.volumeTemperatureRate}};

                for(const auto& [name, field] : volumeFields)
                {
                    if(value.contains(name))
                    {
                        *field = ReadNumber(value.at(name), (std::string("Mist Emitter ") + name).c_str());
                    }
                }
                std::string emitterError;

                if(!ValidateMistEmitterComponent(emitter, &emitterError))
                {
                    throw std::runtime_error("Invalid Mist Emitter: " + emitterError);
                }

                object.mistEmitter = emitter;
            }

            if(item.contains("voxel"))
            {
                if(schemaVersion.get<std::uint32_t>() < SceneDocument::kVoxelSchemaVersion)
                {
                    throw std::runtime_error("Voxel fields require scene schema version 19.");
                }

                const auto& value = item.at("voxel");
                ValidateFields(value, {"participation", "orientation"});
                VoxelSettings voxel;

                if(!TryParseVoxelParticipation(value.at("participation").get<std::string>(), voxel.participation) ||
                    !TryParseVoxelOrientation(value.at("orientation").get<std::string>(), voxel.orientation))
                {
                    throw std::runtime_error("Invalid voxel settings.");
                }

                object.voxel = voxel;
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

// The written form of a document that passed ValidateSceneDocumentWrite; shared by SaveSceneDocument and SerializeSceneDocument.
static std::string SceneDocumentBytes(const SceneDocument& document)
{
    return ToJson(document).dump(4) + '\n';
}

static CreateSceneDocumentResult WriteSceneDocument(
    const SceneDocument& document,
    const std::filesystem::path& path,
    std::string* errorMessage,
    bool replaceExisting)
{
    std::string validationError;

    if (!ValidateSceneDocumentWrite(document, &validationError))
    {
        Fail(errorMessage, validationError);
        return CreateSceneDocumentResult::Error;
    }

    std::filesystem::path temporaryPath;

    try
    {
        // Serialize before touching the filesystem.
        const std::string serialized = SceneDocumentBytes(document);

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
        std::string bytes;
        std::string readError;

        if(!ReadSceneSource(path, bytes, readError))
        {
            return Fail(errorMessage, readError);
        }

        const StreamingTelemetry::Scope parseTiming(StreamingTelemetry::Metric::SceneParseNs, StreamingTelemetry::Metric::SceneParses);
        Json json = ParseSceneJson(bytes);

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

bool ParseSceneDocument(std::string_view bytes, SceneDocument& document, std::string* errorMessage,
    std::uint64_t* storedSchemaVersion)
{
    if (storedSchemaVersion)
    {
        *storedSchemaVersion = 0;
    }

    const StreamingTelemetry::Scope parseTiming(StreamingTelemetry::Metric::SceneParseNs, StreamingTelemetry::Metric::SceneParses);
    StreamingTelemetry::Add(StreamingTelemetry::Metric::SceneParseBytes, bytes.size());

    try
    {
        if(bytes.size() > MaximumSceneSourceBytes)
        {
            return Fail(errorMessage, "Scene source exceeds the largest admitted scene file (" +
                std::to_string(MaximumSceneSourceBytes) + " bytes).");
        }

        const Json json = ParseSceneJson(bytes);

        if (storedSchemaVersion && json.is_object())
        {
            const auto version = json.find("schemaVersion");

            if (version != json.end() && version->is_number_unsigned())
            {
                *storedSchemaVersion = version->get<std::uint64_t>();
            }
        }

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

bool SerializeSceneDocument(const SceneDocument& document, std::string& bytes, std::string* errorMessage)
{
    std::string validationError;

    if (!ValidateSceneDocumentWrite(document, &validationError))
    {
        return Fail(errorMessage, validationError);
    }

    try
    {
        bytes = SceneDocumentBytes(document);
    }
    catch (const std::exception& exception)
    {
        return Fail(errorMessage, exception.what());
    }

    if (errorMessage)
    {
        errorMessage->clear();
    }

    return true;
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
