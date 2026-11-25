#include "assets/CookedScene.h"
#include "Scene/LegacyLightingMigration.h"
#include "Scene/PhysicalLightingMigration.h"
#include "Scene/ColliderScaleMigration.h"
#include "assets/CookedEnvironment.h"
#include "assets/CookedSurface.h"
#include "Scene/SceneSerializer.h"
#include "assets/EngineAssetPak.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

// M3.5: a cooked scene the policy admits must also fit one asset pak entry.
static_assert(CookedScene::Policy{}.maxBytes <= EngineAssetPak::Limits{}.payloadBytes &&
    CookedScene::Policy{}.maxBytes <= EngineAssetPak::Limits{}.readBytes, "Cooked scenes must fit an asset pak entry");

namespace CookedScene
{
namespace
{
constexpr char LegacyMagic[] = "ENGSCN01";
constexpr char BoxPhysicsMagic[] = "ENGSCN02";
constexpr char SpherePhysicsMagic[] = "ENGSCN03";
constexpr char CapsulePhysicsMagic[] = "ENGSCN04";
constexpr char CapsuleRotationMagic[] = "ENGSCN05";
constexpr char ContactEventsMagic[] = "ENGSCN06";
constexpr char TriggerColliderMagic[] = "ENGSCN07";
constexpr char CharacterControllerMagic[] = "ENGSCN08";
constexpr char HierarchyMagic[] = "ENGSCN09";
constexpr char CameraMagic[] = "ENGSCN10";
constexpr char LightMagic[] = "ENGSCN11";
constexpr char EnvironmentNoneMagic[] = "ENGSCN12";
constexpr char ObjectLightingMagic[] = "ENGSCN13";
constexpr char PhysicalLightingMagic[] = "ENGSCN14";
constexpr char Magic[] = "ENGSCN15";

// Per-object hierarchy marker. Every version writes it first; payloads 9+ may carry a parent,
// whose exact object ID follows the object name. Runtime slots are never encoded.
constexpr std::uint64_t RootMarker = UINT32_MAX;
constexpr std::uint64_t ParentMarker = 1;
constexpr size_t HeaderBytes = 32;
constexpr size_t PayloadBytesOffset = 16;

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool IsSupportedMaterialReference(std::string_view text)
{
    return text.empty() || text == "builtin:default" ||
           (AssetID::ParseReference(text) && AssetID::ParseReference(text)->ToReference() == text);
}
struct Writer
{
    const Policy& policy;
    std::vector<std::byte> bytes;
    void WriteUnsigned(std::uint64_t value, size_t width)
    {
        Require(bytes.size() <= policy.maxBytes && width <= policy.maxBytes - bytes.size(),
                "Cooked scene exceeds byte limit.");
        for (size_t byteIndex = 0; byteIndex < width; ++byteIndex)
        {
            bytes.push_back(std::byte((value >> (8 * byteIndex)) & 255));
        }
    }
    void WriteString(const std::string& text)
    {
        Require(text.size() <= policy.maxStringBytes && text.size() <= UINT32_MAX &&
                    text.find('\0') == std::string::npos,
                "Invalid cooked scene string.");
        WriteUnsigned(text.size(), 4);
        for (unsigned char character : text)
        {
            WriteUnsigned(character, 1);
        }
    }
    void WriteFloat(float value)
    {
        WriteUnsigned(std::bit_cast<std::uint32_t>(value), 4);
    }
    void WriteVector(const Vector3& value)
    {
        WriteFloat(value.x);
        WriteFloat(value.y);
        WriteFloat(value.z);
    }
};

struct Reader
{
    std::span<const std::byte> bytes;
    const Policy& policy;
    size_t cursor = 0;
    std::uint64_t ReadUnsigned(size_t width)
    {
        Require(cursor <= bytes.size() && width <= bytes.size() - cursor, "Truncated cooked scene metadata.");
        std::uint64_t value = 0;
        for (size_t byteIndex = 0; byteIndex < width; ++byteIndex)
        {
            value |= std::uint64_t(std::to_integer<unsigned char>(bytes[cursor++])) << (8 * byteIndex);
        }
        return value;
    }
    std::string ReadString()
    {
        const auto length = ReadUnsigned(4);
        Require(length <= policy.maxStringBytes && length <= bytes.size() - cursor,
                "Invalid cooked scene string length.");
        std::string text(reinterpret_cast<const char*>(bytes.data() + cursor), static_cast<size_t>(length));
        cursor += static_cast<size_t>(length);
        Require(text.find('\0') == std::string::npos, "Embedded NUL in cooked scene string.");
        return text;
    }
    float ReadFloat()
    {
        const float value = std::bit_cast<float>(static_cast<std::uint32_t>(ReadUnsigned(4)));
        Require(std::isfinite(value), "Nonfinite cooked scene scalar.");
        return value;
    }
    Vector3 ReadVector()
    {
        const auto x = ReadFloat();
        const auto y = ReadFloat();
        const auto z = ReadFloat();
        return {x, y, z};
    }
};
void ValidateMetadata(const Metadata& sceneMetadata, const Policy& policy)
{
    std::string error;
    if (!ValidateSceneDocument(sceneMetadata.document, &error))
    {
        throw std::runtime_error("Invalid cooked scene document: " + error);
    }
    // Physical sky S0: only an Hdri sky with an asset reference is bound to a cooked environment.
    Require(sceneMetadata.environment.has_value() == SceneSkyUsesEnvironmentAsset(sceneMetadata.document),
        "Environment binding does not match scene selection.");
    Require(sceneMetadata.document.objects.size() <= policy.maxObjects &&
                sceneMetadata.meshes.size() == sceneMetadata.document.objects.size(),
            "Invalid cooked scene object count.");
    for (size_t index = 0; index < sceneMetadata.meshes.size(); ++index)
    {
        const auto& object = sceneMetadata.document.objects[index];
        const auto& binding = sceneMetadata.meshes[index];
        Require(IsSupportedMaterialReference(object.materialReference), "Unsupported material reference.");
        Require(binding.has_value() == !object.meshReference.empty(), "Missing cooked mesh binding.");
        if (!binding)
        {
            Require(object.materialSlotBindings.empty(), "Empty mesh has slot bindings.");
            continue;
        }
        const auto id = AssetID::ParseReference(object.meshReference);
        Require(id && id->ToReference() == object.meshReference,
                "Cooked scene mesh must be a canonical asset identity.");
        Require(binding->generation && binding->payload.formatVersion == CookedMesh::V1::Version &&
                    binding->payload.cookCompatibility == CookedMesh::V1::CookCompatibility &&
                    binding->payload.payloadBytes >= CookedMesh::V1::HeaderBytes &&
                    binding->payload.payloadBytes <= CookedMesh::Policy{}.maxPayloadBytes &&
                    binding->payload.payloadSHA256 != std::array<std::uint8_t, 32>{},
                "Invalid cooked mesh generation/evidence.");
        Require(!binding->slots.empty() && binding->slots.size() <= policy.maxSlots,
                "Invalid cooked mesh slot count.");
        std::set<std::string> slots;
        for (const auto& slot : binding->slots)
        {
            Require(!slot.id.empty() && !slot.name.empty() && slots.insert(slot.id).second,
                    "Invalid/duplicate cooked mesh slot.");
        }
        for (const auto& [slot, reference] : object.materialSlotBindings)
        {
            Require(slots.contains(slot) && AssetID::ParseReference(reference) &&
                        AssetID::ParseReference(reference)->ToReference() == reference,
                    "Unknown slot or invalid material assignment.");
        }
    }
}

void WritePhysicsComponent(Writer& writer, const std::optional<Physics::PhysicsComponent>& component)
{
    writer.WriteUnsigned(component.has_value() ? 1 : 0, 4);
    if (!component)
    {
        return;
    }

    // Wire enum values are explicit and independent of C++ enum ordering.
    switch (component->bodyType)
    {
    case Physics::BodyType::Static:
        writer.WriteUnsigned(0, 4);
        break;
    case Physics::BodyType::Dynamic:
        writer.WriteUnsigned(1, 4);
        break;
    case Physics::BodyType::Kinematic:
        writer.WriteUnsigned(2, 4);
        break;
    default:
        throw std::runtime_error("Unsupported cooked physics body type.");
    }
    switch (component->colliderType)
    {
    case Physics::ColliderType::Box:
        writer.WriteUnsigned(0, 4);
        break;
    case Physics::ColliderType::Sphere:
        writer.WriteUnsigned(1, 4);
        break;
    case Physics::ColliderType::Capsule:
        writer.WriteUnsigned(2, 4);
        break;
    default:
        throw std::runtime_error("Unsupported cooked physics collider type.");
    }
    writer.WriteFloat(component->boxDimensions.width);
    writer.WriteFloat(component->boxDimensions.height);
    writer.WriteFloat(component->boxDimensions.depth);
    writer.WriteVector(component->colliderLocalPositionOffset);
    writer.WriteFloat(component->density);
    writer.WriteFloat(component->friction);
    writer.WriteFloat(component->restitution);
    writer.WriteFloat(component->sphereRadius); // v3 appends radius; v2's Box fields keep their meaning.

    // v4 appends the Capsule fields; every v3 field keeps its offset and meaning.
    writer.WriteFloat(component->capsuleRadius);
    writer.WriteFloat(component->capsuleCylinderHeight);

    // v5 appends the Capsule local rotation (Euler radians); every v4 field keeps its offset.
    writer.WriteVector(component->capsuleLocalRotation);

    // v6 appends the contact-event opt-in as a u32 flag (0 or 1); earlier fields keep their offsets.
    writer.WriteUnsigned(component->generateContactEvents ? 1 : 0, 4);

    // V37 (R1.5) appends the AIR participation as a u32 (0 Automatic, 1 Always, 2 Never); earlier fields keep their offsets.
    writer.WriteUnsigned(static_cast<std::uint32_t>(component->airParticipation), 4);
}

std::optional<Physics::PhysicsComponent> ReadPhysicsComponent(Reader& reader, std::uint32_t version)
{
    const auto presence = reader.ReadUnsigned(4);
    Require(presence <= 1, "Invalid cooked physics presence flag.");
    if (presence == 0)
    {
        return std::nullopt;
    }

    Physics::PhysicsComponent component;
    switch (reader.ReadUnsigned(4))
    {
    case 0:
        component.bodyType = Physics::BodyType::Static;
        break;
    case 1:
        component.bodyType = Physics::BodyType::Dynamic;
        break;
    case 2:
        component.bodyType = Physics::BodyType::Kinematic;
        break;
    default:
        throw std::runtime_error("Unsupported cooked physics body type.");
    }
    // Each version accepts only the collider tags it defined: v2 Box, v3 Sphere, v4 Capsule.
    const auto colliderType = reader.ReadUnsigned(4);
    const bool sphereVersion = version >= SpherePhysicsVersion;
    const bool capsuleVersion = version >= CapsulePhysicsVersion;

    if(sphereVersion && colliderType == 1)
    {
        component.colliderType = Physics::ColliderType::Sphere;
    }
    else if(capsuleVersion && colliderType == 2)
    {
        component.colliderType = Physics::ColliderType::Capsule;
    }
    else
    {
        Require(colliderType == 0, "Unsupported cooked physics collider type.");
    }

    // v2 stores 48 bytes including presence; v3 appends a four-byte radius.
    // For v2, retain PhysicsComponent's default radius of 0.5 meters.
    component.boxDimensions.width = reader.ReadFloat();
    component.boxDimensions.height = reader.ReadFloat();
    component.boxDimensions.depth = reader.ReadFloat();
    component.colliderLocalPositionOffset = reader.ReadVector();
    component.density = reader.ReadFloat();
    component.friction = reader.ReadFloat();
    component.restitution = reader.ReadFloat();
    if(sphereVersion)
    {
        component.sphereRadius = reader.ReadFloat();
    }

    // v2/v3 keep PhysicsComponent's default Capsule values (0.5 m radius, 1 m cylinder).
    if(capsuleVersion)
    {
        component.capsuleRadius = reader.ReadFloat();
        component.capsuleCylinderHeight = reader.ReadFloat();
    }

    // v2-v4 keep the default zero Capsule rotation.
    if(version >= CapsuleRotationVersion)
    {
        component.capsuleLocalRotation = reader.ReadVector();
    }

    // v2-v5 keep the default false; any v6 flag other than 0 or 1 is malformed.
    if(version >= ContactEventsVersion)
    {
        const auto generateContactEvents = reader.ReadUnsigned(4);
        Require(generateContactEvents <= 1, "Invalid cooked physics contact-event flag.");
        component.generateContactEvents = generateContactEvents == 1;
    }

    // Before V37 every body reads Automatic; any V37 value other than 0, 1 or 2 is malformed.
    if(version >= MovingSolidParticipationVersion)
    {
        const auto participation = reader.ReadUnsigned(4);
        Require(participation <= 2, "Invalid cooked physics AIR participation.");
        component.airParticipation = static_cast<Physics::MovingSolidParticipation>(participation);
    }

    std::string error;
    if (!Physics::ValidatePhysicsComponent(component, &error))
    {
        throw std::runtime_error("Invalid cooked physics: " + error);
    }
    return component;
}

void WriteTriggerCollider(Writer& writer, const std::optional<Physics::TriggerColliderComponent>& component)
{
    writer.WriteUnsigned(component.has_value() ? 1 : 0, 4);

    if(!component)
    {
        return;
    }

    switch(component->colliderType)
    {
    case Physics::ColliderType::Box:
        writer.WriteUnsigned(0, 4);
        break;
    case Physics::ColliderType::Sphere:
        writer.WriteUnsigned(1, 4);
        break;
    case Physics::ColliderType::Capsule:
        writer.WriteUnsigned(2, 4);
        break;
    default:
        throw std::runtime_error("Unsupported cooked trigger collider type.");
    }

    writer.WriteFloat(component->boxDimensions.width);
    writer.WriteFloat(component->boxDimensions.height);
    writer.WriteFloat(component->boxDimensions.depth);
    writer.WriteFloat(component->sphereRadius);
    writer.WriteFloat(component->capsuleRadius);
    writer.WriteFloat(component->capsuleCylinderHeight);
    writer.WriteVector(component->capsuleLocalRotation);
    writer.WriteVector(component->colliderLocalPositionOffset);
}

std::optional<Physics::TriggerColliderComponent> ReadTriggerCollider(Reader& reader)
{
    const auto presence = reader.ReadUnsigned(4);
    Require(presence <= 1, "Invalid cooked trigger presence flag.");

    if(presence == 0)
    {
        return std::nullopt;
    }

    Physics::TriggerColliderComponent component;

    switch(reader.ReadUnsigned(4))
    {
    case 0:
        component.colliderType = Physics::ColliderType::Box;
        break;
    case 1:
        component.colliderType = Physics::ColliderType::Sphere;
        break;
    case 2:
        component.colliderType = Physics::ColliderType::Capsule;
        break;
    default:
        throw std::runtime_error("Unsupported cooked trigger collider type.");
    }

    component.boxDimensions.width = reader.ReadFloat();
    component.boxDimensions.height = reader.ReadFloat();
    component.boxDimensions.depth = reader.ReadFloat();
    component.sphereRadius = reader.ReadFloat();
    component.capsuleRadius = reader.ReadFloat();
    component.capsuleCylinderHeight = reader.ReadFloat();
    component.capsuleLocalRotation = reader.ReadVector();
    component.colliderLocalPositionOffset = reader.ReadVector();
    std::string error;

    if(!Physics::ValidateTriggerColliderComponent(component, &error))
    {
        throw std::runtime_error("Invalid cooked trigger collider: " + error);
    }

    return component;
}

// V8: u32 presence followed by eleven explicit float32 scalars when present.
// No native struct bytes/padding. Absent = 4 bytes; present = 4 + 44 = 48 bytes.
void WriteCharacterController(Writer& writer, const std::optional<Physics::CharacterControllerComponent>& component)
{
    writer.WriteUnsigned(component.has_value() ? 1 : 0, 4);

    if(!component)
    {
        return;
    }

    writer.WriteFloat(component->capsuleRadius);
    writer.WriteFloat(component->capsuleCylinderHeight);
    writer.WriteVector(component->localOffset);
    writer.WriteFloat(component->maximumHorizontalSpeed);
    writer.WriteFloat(component->gravityScale);
    writer.WriteFloat(component->jumpSpeed);
    writer.WriteFloat(component->maximumWalkableSlope);
    writer.WriteFloat(component->groundSnapDistance);
    writer.WriteFloat(component->collisionClearance);
}

std::optional<Physics::CharacterControllerComponent> ReadCharacterController(Reader& reader)
{
    const auto presence = reader.ReadUnsigned(4);
    Require(presence <= 1, "Invalid cooked Character Controller presence flag.");

    if(presence == 0)
    {
        return std::nullopt;
    }

    Physics::CharacterControllerComponent component;
    component.capsuleRadius = reader.ReadFloat();
    component.capsuleCylinderHeight = reader.ReadFloat();
    component.localOffset = reader.ReadVector();
    component.maximumHorizontalSpeed = reader.ReadFloat();
    component.gravityScale = reader.ReadFloat();
    component.jumpSpeed = reader.ReadFloat();
    component.maximumWalkableSlope = reader.ReadFloat();
    component.groundSnapDistance = reader.ReadFloat();
    component.collisionClearance = reader.ReadFloat();
    std::string error;

    if(!Physics::ValidateCharacterControllerComponent(component, &error))
    {
        throw std::runtime_error("Invalid cooked Character Controller: " + error);
    }

    return component;
}

// Exposure (V15, 48 bytes): u32 mode, float32 aperture/shutter/ISO/compensation/metering min/max/low/high
// percentile/brightening/darkening speed, u32 locked.
constexpr std::uint32_t CookedExposureBytes = 48;

void WriteCameraExposure(Writer& writer, const CameraExposure& exposure)
{
    writer.WriteUnsigned(static_cast<std::uint32_t>(exposure.mode), 4);
    writer.WriteFloat(exposure.aperture);
    writer.WriteFloat(exposure.shutterSeconds);
    writer.WriteFloat(exposure.iso);
    writer.WriteFloat(exposure.compensation);
    writer.WriteFloat(exposure.meteringMinEV100);
    writer.WriteFloat(exposure.meteringMaxEV100);
    writer.WriteFloat(exposure.lowPercentile);
    writer.WriteFloat(exposure.highPercentile);
    writer.WriteFloat(exposure.brighteningSpeed);
    writer.WriteFloat(exposure.darkeningSpeed);
    writer.WriteUnsigned(exposure.locked ? 1 : 0, 4);
}

CameraExposure ReadCameraExposure(Reader& reader)
{
    CameraExposure exposure;
    const auto mode = reader.ReadUnsigned(4);
    Require(mode <= static_cast<std::uint32_t>(ExposureMode::Auto), "Invalid cooked exposure mode.");
    exposure.mode = static_cast<ExposureMode>(mode);
    exposure.aperture = reader.ReadFloat();
    exposure.shutterSeconds = reader.ReadFloat();
    exposure.iso = reader.ReadFloat();
    exposure.compensation = reader.ReadFloat();
    exposure.meteringMinEV100 = reader.ReadFloat();
    exposure.meteringMaxEV100 = reader.ReadFloat();
    exposure.lowPercentile = reader.ReadFloat();
    exposure.highPercentile = reader.ReadFloat();
    exposure.brighteningSpeed = reader.ReadFloat();
    exposure.darkeningSpeed = reader.ReadFloat();
    const auto locked = reader.ReadUnsigned(4);
    Require(locked <= 1, "Invalid cooked exposure lock flag.");
    exposure.locked = locked != 0;
    const char* reason = nullptr;
    Require(ValidateCameraExposure(exposure, &reason), reason ? reason : "Invalid cooked exposure.");
    return exposure;
}

// V10 Camera: u32 presence; when present, u32 record length (24, or 72 from V15 with exposure), then
// u32 enabled, u32 projection, float32 FOV/near/far, u32 main[, exposure]. No native padding.
void WriteCameraComponent(Writer& writer, const std::optional<CameraComponent>& camera)
{
    writer.WriteUnsigned(camera.has_value() ? 1 : 0, 4);

    if(!camera)
    {
        return;
    }

    writer.WriteUnsigned(24 + CookedExposureBytes, 4);
    writer.WriteUnsigned(camera->enabled ? 1 : 0, 4);
    writer.WriteUnsigned(static_cast<std::uint32_t>(camera->projection), 4);
    writer.WriteFloat(camera->verticalFov);
    writer.WriteFloat(camera->nearPlane);
    writer.WriteFloat(camera->farPlane);
    writer.WriteUnsigned(camera->main ? 1 : 0, 4);
    WriteCameraExposure(writer, camera->exposure);
}

// Payloads before 15 carry no exposure: hasExposure is false and the decoder applies the reference default.
std::optional<CameraComponent> ReadCameraComponent(Reader& reader, std::uint32_t version, bool& hasExposure)
{
    const auto presence = reader.ReadUnsigned(4);
    Require(presence <= 1, "Invalid cooked Camera presence flag.");

    if(presence == 0)
    {
        return std::nullopt;
    }

    hasExposure = version >= CameraExposureVersion;
    Require(reader.ReadUnsigned(4) == (hasExposure ? 24 + CookedExposureBytes : 24u), "Invalid cooked Camera record length.");
    const auto enabled = reader.ReadUnsigned(4);
    const auto projection = reader.ReadUnsigned(4);
    Require(enabled <= 1, "Invalid cooked Camera enabled flag.");
    Require(projection == static_cast<std::uint32_t>(CameraProjection::Perspective),
        "Unsupported cooked Camera projection.");

    CameraComponent camera;
    camera.enabled = enabled != 0;
    camera.projection = CameraProjection::Perspective;
    camera.verticalFov = reader.ReadFloat();
    camera.nearPlane = reader.ReadFloat();
    camera.farPlane = reader.ReadFloat();
    const auto main = reader.ReadUnsigned(4);
    Require(main <= 1, "Invalid cooked Camera Main flag.");
    camera.main = main != 0;
    if(hasExposure) camera.exposure = ReadCameraExposure(reader);
    std::string error;

    if(!ValidateCameraComponent(camera, &error))
    {
        throw std::runtime_error("Invalid cooked Camera: " + error);
    }

    return camera;
}

// V13 Light: V11's 36-byte body plus float32 angular radius (40 bytes total).
// V14 Light: V13's 40 bytes (intensity in lumens or lux) plus a u32 range attenuation flag (44 bytes).
// The reader keeps the old bodies for payloads 11/12/13 (legacy units). No native padding.
void WriteLightComponent(Writer& writer, const std::optional<LightComponent>& light)
{
    writer.WriteUnsigned(light.has_value() ? 1 : 0, 4);

    if(!light)
    {
        return;
    }

    writer.WriteUnsigned(44, 4);
    writer.WriteUnsigned(light->enabled ? 1 : 0, 4);
    writer.WriteUnsigned(static_cast<std::uint32_t>(light->type), 4);
    writer.WriteVector(light->color);
    writer.WriteFloat(light->intensity);
    writer.WriteFloat(light->range);
    writer.WriteFloat(light->innerConeAngle);
    writer.WriteFloat(light->outerConeAngle);
    writer.WriteFloat(light->angularRadius);
    // V39: bit 1 marks a light that does not affect the atmosphere, so the default writes the V38 word.
    writer.WriteUnsigned((light->rangeAttenuation ? 1u : 0u) | (light->affectsAtmosphere ? 0u : 2u), 4);
}

// Payloads before 14 carry legacy units: their range goes to legacyRange and the historical rules apply
// (MigrateLegacyLightUnits converts the document afterwards).
// V16 Script: u32 presence; when present u32 enabled, then the script reference and class name strings.
void WriteScriptComponent(Writer& writer, const std::optional<ScriptComponent>& script)
{
    writer.WriteUnsigned(script.has_value() ? 1 : 0, 4);

    if(!script)
    {
        return;
    }

    writer.WriteUnsigned(script->enabled ? 1 : 0, 4);
    writer.WriteString(script->scriptReference);
    writer.WriteString(script->className);
}

std::optional<ScriptComponent> ReadScriptComponent(Reader& reader)
{
    const auto presence = reader.ReadUnsigned(4);
    Require(presence <= 1, "Invalid cooked Script presence flag.");

    if(presence == 0)
    {
        return std::nullopt;
    }

    const auto enabled = reader.ReadUnsigned(4);
    Require(enabled <= 1, "Invalid cooked Script enabled flag.");
    ScriptComponent script;
    script.enabled = enabled != 0;
    script.scriptReference = reader.ReadString();
    script.className = reader.ReadString();
    std::string error;
    Require(ValidateScriptComponent(script, &error), error.c_str());
    return script;
}

// V17 Audio: u32 Audio Source presence; when present u32 enabled, the event GUID and path strings and u32 autoplay.
// Then u32 Audio Listener presence; when present u32 enabled.
// V20 adds u32 source-centric and u32 baked after autoplay, and u32 baked after the listener's enabled.
// V24 adds u32 pathing after the source's baked flag. Older payloads read as no pathing.
void WriteAudioComponents(Writer& writer, const std::optional<AudioSourceComponent>& source,
    const std::optional<AudioListenerComponent>& listener)
{
    writer.WriteUnsigned(source.has_value() ? 1 : 0, 4);

    if(source)
    {
        writer.WriteUnsigned(source->enabled ? 1 : 0, 4);
        writer.WriteString(source->eventGuid);
        writer.WriteString(source->eventPath);
        writer.WriteUnsigned(source->autoplay ? 1 : 0, 4);
        writer.WriteUnsigned(source->sourceCentricReflections ? 1 : 0, 4);
        writer.WriteUnsigned(source->bakedReflections ? 1 : 0, 4);
        writer.WriteUnsigned(source->pathing ? 1 : 0, 4);
    }

    writer.WriteUnsigned(listener.has_value() ? 1 : 0, 4);

    if(listener)
    {
        writer.WriteUnsigned(listener->enabled ? 1 : 0, 4);
        writer.WriteUnsigned(listener->bakedReflections ? 1 : 0, 4);
    }
}

// V19 Acoustics: u32 presence; when present u32 enabled, u32 assignment count, then key and reference strings.
void WriteAcousticSettings(Writer& writer, const std::optional<AcousticSettings>& acoustics)
{
    writer.WriteUnsigned(acoustics.has_value() ? 1 : 0, 4);

    if(acoustics)
    {
        writer.WriteUnsigned(acoustics->enabled ? 1 : 0, 4);
        writer.WriteUnsigned(acoustics->submeshMaterials.size(), 4);

        for(const auto& [key, reference] : acoustics->submeshMaterials)
        {
            writer.WriteString(key);
            writer.WriteString(reference);
        }
    }
}

void ReadAcousticSettings(Reader& reader, std::optional<AcousticSettings>& acoustics)
{
    const auto presence = reader.ReadUnsigned(4);
    Require(presence <= 1, "Invalid cooked acoustics presence flag.");

    if(presence == 0)
    {
        return;
    }

    const auto enabled = reader.ReadUnsigned(4);
    Require(enabled <= 1, "Invalid cooked acoustics enabled flag.");
    const auto count = reader.ReadUnsigned(4);
    Require(count <= AcousticSettings::MaximumAssignments, "Too many cooked acoustic material assignments.");
    AcousticSettings value;
    value.enabled = enabled != 0;

    for(std::uint64_t index = 0; index < count; ++index)
    {
        std::string key = reader.ReadString();
        value.submeshMaterials[std::move(key)] = reader.ReadString();
    }

    std::string error;
    Require(ValidateAcousticSettings(value, &error), error.c_str());
    acoustics = std::move(value);
}

// V21 Probe Volume: u32 presence; when present u32 enabled, u32 placement, f32 x3 size, f32 spacing, f32 height, u32 manual count, f32 x3 each, reference string.
// V22 adds u32 filterInsideSolids. V21 probes were generated unfiltered, so reading one as unfiltered keeps its fingerprint valid.
// V23 adds u32 rays, u32 diffuse samples, u32 bounces, f32 irradiance minimum and the baked reflections reference string. Older
// payloads read as the default settings and no baked data.
// V24 adds u32 pathing enabled, u32 pathing samples, f32 radius, f32 threshold, f32 visibility range, f32 path range and the baked
// pathing reference string. Older payloads read as pathing enabled, the default settings and no baked pathing.
void WriteProbeVolume(Writer& writer, const std::optional<ProbeVolumeComponent>& volume)
{
    writer.WriteUnsigned(volume.has_value() ? 1 : 0, 4);

    if(!volume)
    {
        return;
    }

    writer.WriteUnsigned(volume->enabled ? 1 : 0, 4);
    writer.WriteUnsigned(static_cast<std::uint32_t>(volume->placement), 4);

    for(const float edge : volume->size)
    {
        writer.WriteFloat(edge);
    }

    writer.WriteFloat(volume->probeSpacing);
    writer.WriteFloat(volume->heightAboveFloor);
    writer.WriteUnsigned(volume->manualProbes.size(), 4);

    for(const auto& probe : volume->manualProbes)
    {
        writer.WriteFloat(probe[0]);
        writer.WriteFloat(probe[1]);
        writer.WriteFloat(probe[2]);
    }

    writer.WriteString(volume->probeBatch);
    writer.WriteUnsigned(volume->filterInsideSolids ? 1 : 0, 4);
    writer.WriteUnsigned(volume->bakeSettings.rays, 4);
    writer.WriteUnsigned(volume->bakeSettings.diffuseSamples, 4);
    writer.WriteUnsigned(volume->bakeSettings.bounces, 4);
    writer.WriteFloat(volume->bakeSettings.irradianceMinimumDistance);
    writer.WriteString(volume->bakedReflections);
    writer.WriteUnsigned(volume->pathingEnabled ? 1 : 0, 4);
    writer.WriteUnsigned(volume->pathingSettings.samples, 4);
    writer.WriteFloat(volume->pathingSettings.radius);
    writer.WriteFloat(volume->pathingSettings.threshold);
    writer.WriteFloat(volume->pathingSettings.visibilityRange);
    writer.WriteFloat(volume->pathingSettings.pathRange);
    writer.WriteString(volume->bakedPathing);
}

// V25 Air Motor: u32 present; then u32 enabled, u32 type, f32 strength, f32 falloff, 3 x f32 half extents, f32 radius, f32 length,
// f32 inward pull, f32 coupling (52 bytes). V26 adds 3 x f32 emission origin and f32 emission radius (68 bytes); V25 motors read
// the defaults. V27 replaces the emission radius with 3 x f32 emission semi-axes and 3 x f32 emission rotation (88 bytes); V25 and
// V26 motors are migrated (MigrateLegacyAirMotorEmission).
void WriteAirMotor(Writer& writer, const std::optional<AirMotorComponent>& motor)
{
    writer.WriteUnsigned(motor.has_value() ? 1 : 0, 4);

    if(!motor)
    {
        return;
    }

    writer.WriteUnsigned(motor->enabled ? 1 : 0, 4);
    writer.WriteUnsigned(static_cast<std::uint32_t>(motor->type), 4);
    writer.WriteFloat(motor->strength);
    writer.WriteFloat(motor->falloff);
    writer.WriteVector(motor->halfExtents);
    writer.WriteFloat(motor->radius);
    writer.WriteFloat(motor->length);
    writer.WriteFloat(motor->inwardPull);
    writer.WriteFloat(motor->coupling);
    writer.WriteVector(motor->emissionOrigin);
    writer.WriteVector(motor->emissionSemiAxes);
    writer.WriteVector(motor->emissionRotation);
}

void ReadAirMotor(Reader& reader, std::optional<AirMotorComponent>& motor, std::uint32_t version, const std::string& objectName,
    const std::string& objectID)
{
    const std::uint32_t present = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
    Require(present <= 1, "Invalid Air Motor presence.");

    if(present == 0)
    {
        motor.reset();
        return;
    }

    AirMotorComponent value;
    const std::uint32_t enabled = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
    Require(enabled <= 1, "Invalid Air Motor enabled flag.");
    value.enabled = enabled == 1;
    value.type = static_cast<AirMotorType>(reader.ReadUnsigned(4));
    value.strength = reader.ReadFloat();
    value.falloff = reader.ReadFloat();
    value.halfExtents = reader.ReadVector();
    value.radius = reader.ReadFloat();
    value.length = reader.ReadFloat();
    value.inwardPull = reader.ReadFloat();
    value.coupling = reader.ReadFloat();

    if(version >= AirMotorEmissionVersion)
    {
        value.emissionOrigin = reader.ReadVector();
    }

    if(version >= AirMotorShapeVersion)
    {
        value.emissionSemiAxes = reader.ReadVector();
        value.emissionRotation = reader.ReadVector();
    }
    else
    {
        // V25 had no emission radius: every AIR.3 motor emitted from a point.
        const float legacyEmissionRadius = version >= AirMotorEmissionVersion ? reader.ReadFloat() : 0.0f;
        Require(std::isfinite(legacyEmissionRadius) && legacyEmissionRadius >= 0.0f && legacyEmissionRadius <= AirMotorComponent::MaximumSize,
            "Air Motor emission radius must be finite, from 0 to 256 m.");

        if(MigrateLegacyAirMotorEmission(value, legacyEmissionRadius))
        {
            LogAirMotorReconfiguration(objectName, objectID, value.type, "cooked payload " + std::to_string(version));
        }
    }

    std::string error;
    Require(ValidateAirMotorComponent(value, &error), error.c_str());
    motor = value;
}

// V28 Mist Emitter: u32 present; then u32 enabled, u32 shape, 3 x f32 emission origin, 3 x f32 emission rotation, f32 radius,
// 3 x f32 half extents, f32 rate, 3 x f32 colour, f32 density (68 bytes); from V29, f32 temperature (72 bytes). From V30 (AIR.4F.2b):
// u32 mode, u32 n, then n x f32 mode fields: emission rate, jet speed, (AIR.4F.2b.1) temperature response rate, and (AIR.4F.2c) target
// density, replenishment rate, fill height, edge softness and volume temperature rate (n = 8). The count lets AIR.4F.2c append its Volume fields
// within V30: a reader takes the fields it knows, defaults any a shorter list lacks, and skips the rest.
// V30: the mode fields this writer stores, and the most a reader accepts (room for AIR.4F.2c's Volume fields).
constexpr std::uint32_t MistEmitterModeFieldCount = 8;
constexpr std::uint32_t MaximumMistEmitterModeFields = 64;

// V31 environmental haze: u32 present; then u32 enabled, f32 attenuation distance, 3 x f32 colour, f32 near distance, f32 near
// transition (32 bytes). V32 appends f32 ambient contribution (36 bytes); a V31 haze reads ambient 0. V33 appends u32 height bounds,
// f32 base height, f32 max height, f32 height transition (52 bytes); earlier hazes read the bounds off.
void WriteEnvironmentalHaze(Writer& writer, const std::optional<EnvironmentalHazeSettings>& haze)
{
    writer.WriteUnsigned(haze.has_value() ? 1 : 0, 4);

    if(!haze)
    {
        return;
    }

    writer.WriteUnsigned(haze->enabled ? 1 : 0, 4);
    writer.WriteFloat(haze->attenuationDistance);

    for(const float channel : haze->color)
    {
        writer.WriteFloat(channel);
    }

    writer.WriteFloat(haze->nearDistance);
    writer.WriteFloat(haze->nearTransition);
    writer.WriteFloat(haze->ambient);
    writer.WriteUnsigned(haze->heightBounds ? 1 : 0, 4);
    writer.WriteFloat(haze->baseHeight);
    writer.WriteFloat(haze->maxHeight);
    writer.WriteFloat(haze->heightTransition);
}

void ReadEnvironmentalHaze(Reader& reader, std::uint32_t payloadVersion, std::optional<EnvironmentalHazeSettings>& haze)
{
    const std::uint32_t present = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
    Require(present <= 1, "Invalid environmental haze presence.");

    if(present == 0)
    {
        haze.reset();
        return;
    }

    EnvironmentalHazeSettings value;
    const std::uint32_t enabled = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
    Require(enabled <= 1, "Invalid environmental haze enabled flag.");
    value.enabled = enabled == 1;
    value.attenuationDistance = reader.ReadFloat();

    for(float& channel : value.color)
    {
        channel = reader.ReadFloat();
    }

    value.nearDistance = reader.ReadFloat();
    value.nearTransition = reader.ReadFloat();
    value.ambient = payloadVersion >= HazeAmbientVersion ? reader.ReadFloat() : 0.0f;

    if(payloadVersion >= HazeHeightVersion)
    {
        const std::uint32_t bounded = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
        Require(bounded <= 1, "Invalid environmental haze height bounds flag.");
        value.heightBounds = bounded == 1;
        value.baseHeight = reader.ReadFloat();
        value.maxHeight = reader.ReadFloat();
        value.heightTransition = reader.ReadFloat();
    }
    std::string error;
    Require(ValidateEnvironmentalHazeSettings(value, &error), error.c_str());
    haze = value;
}

// V34 (R1.1) air configuration, after the haze: u32 environment present; if so f32 ambient temperature, f32 relaxation time. Then u32
// simulation present; if so u32 enabled, u32 pressure projection, u32 buoyancy, f32 buoyancy gravity, u32 maximum steps per frame,
// 3 x f32 ambient velocity, f32 damping rate, f32 maximum motor speed, f32 mist dissipation rate (48 bytes).
void WriteAirConfiguration(Writer& writer, const std::optional<AirEnvironmentSettings>& environment,
    const std::optional<AirSimulationSettings>& simulation)
{
    writer.WriteUnsigned(environment.has_value() ? 1 : 0, 4);

    if(environment)
    {
        writer.WriteFloat(environment->ambientTemperature);
        writer.WriteFloat(environment->relaxationTime);
    }

    writer.WriteUnsigned(simulation.has_value() ? 1 : 0, 4);

    if(!simulation)
    {
        return;
    }

    writer.WriteUnsigned(simulation->enabled ? 1 : 0, 4);
    writer.WriteUnsigned(simulation->pressureProjection ? 1 : 0, 4);
    writer.WriteUnsigned(simulation->buoyancy ? 1 : 0, 4);
    writer.WriteFloat(simulation->buoyancyGravity);
    writer.WriteUnsigned(static_cast<std::uint32_t>(simulation->maximumStepsPerFrame), 4);
    writer.WriteFloat(simulation->ambientVelocity.x);
    writer.WriteFloat(simulation->ambientVelocity.y);
    writer.WriteFloat(simulation->ambientVelocity.z);
    writer.WriteFloat(simulation->dampingRate);
    writer.WriteFloat(simulation->maximumMotorSpeed);
    writer.WriteFloat(simulation->mistDissipationRate);
}

void ReadAirConfiguration(Reader& reader, std::optional<AirEnvironmentSettings>& environment, std::optional<AirSimulationSettings>& simulation)
{
    const auto readFlag = [&](const char* message)
    {
        const std::uint32_t flag = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
        Require(flag <= 1, message);
        return flag == 1;
    };
    std::string error;

    environment.reset();

    if(readFlag("Invalid air environment presence."))
    {
        AirEnvironmentSettings value;
        value.ambientTemperature = reader.ReadFloat();
        value.relaxationTime = reader.ReadFloat();
        Require(ValidateAirEnvironmentSettings(value, &error), error.c_str());
        environment = value;
    }

    simulation.reset();

    if(!readFlag("Invalid AIR simulation presence."))
    {
        return;
    }

    AirSimulationSettings value;
    value.enabled = readFlag("Invalid AIR simulation enabled flag.");
    value.pressureProjection = readFlag("Invalid AIR pressure projection flag.");
    value.buoyancy = readFlag("Invalid AIR buoyancy flag.");
    value.buoyancyGravity = reader.ReadFloat();
    const std::uint32_t steps = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
    value.maximumStepsPerFrame = steps == 1 || steps == 2 ? static_cast<int>(steps) : 0;
    value.ambientVelocity.x = reader.ReadFloat();
    value.ambientVelocity.y = reader.ReadFloat();
    value.ambientVelocity.z = reader.ReadFloat();
    value.dampingRate = reader.ReadFloat();
    value.maximumMotorSpeed = reader.ReadFloat();
    value.mistDissipationRate = reader.ReadFloat();
    Require(ValidateAirSimulationSettings(value, &error), error.c_str());
    simulation = value;
}

// V35 (R1.3) mist appearance, after the air configuration: u32 present; if so, in MistRenderSettings' declaration order, f32 opacity;
// u32 detail, f32 strength, f32 scale; u32 curl, f32 strength, f32 scale, f32 flow; u32 wisps, f32 strength, f32 scale; u32 density
// detail, f32 strength; u32 lighting, u32 sun shadows, f32 scattering, f32 anisotropy, f32 ambient, 3 x f32 tint, f32 multiple
// scattering, f32 indirect scattering; f32 near distance, f32 near transition (all 23 fields, 25 values: 100 bytes after the presence
// word).
void WriteMistRendering(Writer& writer, const std::optional<MistRenderSettings>& mist)
{
    writer.WriteUnsigned(mist.has_value() ? 1 : 0, 4);

    if(!mist)
    {
        return;
    }

    const auto flag = [&](bool value)
    {
        writer.WriteUnsigned(value ? 1 : 0, 4);
    };

    writer.WriteFloat(mist->opacity);
    flag(mist->detail);
    writer.WriteFloat(mist->detailStrength);
    writer.WriteFloat(mist->detailScale);
    flag(mist->curl);
    writer.WriteFloat(mist->curlStrength);
    writer.WriteFloat(mist->curlScale);
    writer.WriteFloat(mist->curlFlow);
    flag(mist->wisps);
    writer.WriteFloat(mist->wispStrength);
    writer.WriteFloat(mist->wispScale);
    flag(mist->densityDetail);
    writer.WriteFloat(mist->densityDetailStrength);
    flag(mist->lighting);
    flag(mist->sunShadows);
    writer.WriteFloat(mist->scattering);
    writer.WriteFloat(mist->anisotropy);
    writer.WriteFloat(mist->ambient);

    for(const float channel : mist->scatteringTint)
    {
        writer.WriteFloat(channel);
    }

    writer.WriteFloat(mist->multipleScattering);
    writer.WriteFloat(mist->indirectScattering);
    writer.WriteFloat(mist->nearDistance);
    writer.WriteFloat(mist->nearTransition);
}

// Strict, unlike a scene file's load (which sanitizes): a cook writes only validated documents, so anything else is corruption.
void ReadMistRendering(Reader& reader, std::optional<MistRenderSettings>& mist)
{
    const auto flag = [&](const char* message)
    {
        const std::uint32_t value = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
        Require(value <= 1, message);
        return value == 1;
    };

    mist.reset();

    if(!flag("Invalid mist rendering presence."))
    {
        return;
    }

    MistRenderSettings value;
    value.opacity = reader.ReadFloat();
    value.detail = flag("Invalid mist detail flag.");
    value.detailStrength = reader.ReadFloat();
    value.detailScale = reader.ReadFloat();
    value.curl = flag("Invalid mist curl flag.");
    value.curlStrength = reader.ReadFloat();
    value.curlScale = reader.ReadFloat();
    value.curlFlow = reader.ReadFloat();
    value.wisps = flag("Invalid mist wisps flag.");
    value.wispStrength = reader.ReadFloat();
    value.wispScale = reader.ReadFloat();
    value.densityDetail = flag("Invalid mist density detail flag.");
    value.densityDetailStrength = reader.ReadFloat();
    value.lighting = flag("Invalid mist lighting flag.");
    value.sunShadows = flag("Invalid mist sun shadows flag.");
    value.scattering = reader.ReadFloat();
    value.anisotropy = reader.ReadFloat();
    value.ambient = reader.ReadFloat();

    for(float& channel : value.scatteringTint)
    {
        channel = reader.ReadFloat();
    }

    value.multipleScattering = reader.ReadFloat();
    value.indirectScattering = reader.ReadFloat();
    value.nearDistance = reader.ReadFloat();
    value.nearTransition = reader.ReadFloat();
    Require(ValidateMistRenderSettings(value), "Invalid cooked mist rendering settings.");
    mist = value;
}

// V36 (R1.4a) voxel bake, after the mist appearance: u32 present; if so, i32 base exponent, u32 bricks per region axis, u32 narrow
// band, u32 coarse clamp, then the manifest reference as a string (empty for settings that were never baked). Only the authored
// settings and the reference travel: residency and AIR state are rebuilt at runtime, and the manifest itself is an archive entry.
void WriteVoxelBake(Writer& writer, const std::optional<VoxelBakeSettings>& bake)
{
    writer.WriteUnsigned(bake.has_value() ? 1 : 0, 4);

    if(!bake)
    {
        return;
    }

    writer.WriteUnsigned(std::bit_cast<std::uint32_t>(bake->baseExponent), 4);
    writer.WriteUnsigned(bake->regionBricks, 4);
    writer.WriteUnsigned(bake->narrowBand, 4);
    writer.WriteUnsigned(bake->coarseClamp, 4);
    writer.WriteString(bake->manifestReference);
}

// Strict, like the mist appearance: the reference must be empty or a canonical asset reference, because the archive resolves the
// manifest by exactly that identity.
void ReadVoxelBake(Reader& reader, std::optional<VoxelBakeSettings>& bake)
{
    bake.reset();
    const std::uint32_t present = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
    Require(present <= 1, "Invalid voxel bake presence.");

    if(present == 0)
    {
        return;
    }

    VoxelBakeSettings value;
    value.baseExponent = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(reader.ReadUnsigned(4)));
    value.regionBricks = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
    value.narrowBand = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
    value.coarseClamp = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
    value.manifestReference = reader.ReadString();
    Require(ValidateVoxelBakeSettings(value), "Invalid cooked voxel bake settings.");

    if(!value.manifestReference.empty())
    {
        const std::optional<AssetID> manifest = AssetID::ParseReference(value.manifestReference);
        Require(manifest.has_value() && manifest->ToReference() == value.manifestReference, "Invalid cooked voxel manifest reference.");
    }

    bake = std::move(value);
}

// V37 (R1.5) AIR moving-solid enable, after the voxel bake: u32 0 or 1. Kept outside the V34 air configuration block so that block's
// layout never changes; it belongs to airSimulation, so a scene without that block writes 0 and must read 0.
void WriteMovingSolidSetting(Writer& writer, const std::optional<AirSimulationSettings>& simulation)
{
    writer.WriteUnsigned(simulation && simulation->movingSolids ? 1 : 0, 4);
}

void ReadMovingSolidSetting(Reader& reader, std::optional<AirSimulationSettings>& simulation)
{
    const std::uint32_t value = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
    Require(value <= 1, "Invalid cooked AIR moving-solid flag.");
    Require(value == 0 || simulation.has_value(), "A cooked AIR moving-solid flag without an AIR simulation block.");

    if(simulation)
    {
        simulation->movingSolids = value == 1;
    }
}

// V38 (physical sky S0) sky, after the moving-solid setting: u32 sky type stored; u32 sky type (0 Neutral, 1 Hdri, 2 Physical; 0
// when not stored); u32 physical present; if so, in PhysicalSkySettings' declaration order: u32 model; f32 planet radius, f32 sea level, f32 atmosphere height, 3 x f32 ground
// albedo; 3 x f32 Rayleigh scattering, 3 x f32 Rayleigh absorption, f32 Rayleigh scale height; 3 x f32 Mie scattering, 3 x f32 Mie
// absorption, f32 Mie anisotropy, f32 Mie scale height; 3 x f32 ozone absorption, f32 ozone minimum altitude, f32 ozone layer width;
// f32 multiple scattering; u32 atmospheric scattering, u32 show ground, u32 sample below horizon; u32 intensity mode, f32 exposure
// compensation, f32 multiplier; f32 colour saturation, 3 x f32 horizon tint, 3 x f32 zenith tint, f32 horizon-zenith shift; u32 update
// mode, f32 update period (44 values, 176 bytes after the presence word).
void WriteChannels(Writer& writer, const std::array<float, 3>& channels)
{
    for(const float channel : channels)
    {
        writer.WriteFloat(channel);
    }
}

void ReadChannels(Reader& reader, std::array<float, 3>& channels)
{
    for(float& channel : channels)
    {
        channel = reader.ReadFloat();
    }
}

void WriteSky(Writer& writer, const std::optional<SkyType>& skyType, const std::optional<PhysicalSkySettings>& physicalSky)
{
    writer.WriteUnsigned(skyType.has_value() ? 1 : 0, 4);
    writer.WriteUnsigned(skyType ? static_cast<std::uint32_t>(*skyType) : 0u, 4);
    writer.WriteUnsigned(physicalSky.has_value() ? 1 : 0, 4);

    if(!physicalSky)
    {
        return;
    }

    const PhysicalSkySettings& sky = *physicalSky;
    writer.WriteUnsigned(static_cast<std::uint32_t>(sky.model), 4);
    writer.WriteFloat(sky.planetRadiusKm);
    writer.WriteFloat(sky.seaLevelMeters);
    writer.WriteFloat(sky.atmosphereHeightKm);
    WriteChannels(writer, sky.groundAlbedo);
    WriteChannels(writer, sky.rayleighScattering);
    WriteChannels(writer, sky.rayleighAbsorption);
    writer.WriteFloat(sky.rayleighScaleHeightKm);
    WriteChannels(writer, sky.mieScattering);
    WriteChannels(writer, sky.mieAbsorption);
    writer.WriteFloat(sky.mieAnisotropy);
    writer.WriteFloat(sky.mieScaleHeightKm);
    WriteChannels(writer, sky.ozoneAbsorption);
    writer.WriteFloat(sky.ozoneMinimumAltitudeKm);
    writer.WriteFloat(sky.ozoneLayerWidthKm);
    writer.WriteFloat(sky.multipleScattering);
    writer.WriteUnsigned(sky.atmosphericScattering ? 1 : 0, 4);
    writer.WriteUnsigned(sky.showGroundBelowHorizon ? 1 : 0, 4);
    writer.WriteUnsigned(sky.sampleBelowHorizon ? 1 : 0, 4);
    writer.WriteUnsigned(static_cast<std::uint32_t>(sky.intensityMode), 4);
    writer.WriteFloat(sky.exposureCompensation);
    writer.WriteFloat(sky.multiplier);
    writer.WriteFloat(sky.colorSaturation);
    WriteChannels(writer, sky.horizonTint);
    WriteChannels(writer, sky.zenithTint);
    writer.WriteFloat(sky.horizonZenithShift);
    writer.WriteUnsigned(static_cast<std::uint32_t>(sky.updateMode), 4);
    writer.WriteFloat(sky.updatePeriodSeconds);
}

void ReadSky(Reader& reader, std::optional<SkyType>& skyType, std::optional<PhysicalSkySettings>& physicalSky)
{
    const auto readFlag = [&](const char* message)
    {
        const std::uint32_t flag = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
        Require(flag <= 1, message);
        return flag == 1;
    };

    const bool typeStored = readFlag("Invalid cooked sky type presence.");
    const std::uint32_t type = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
    Require(type <= static_cast<std::uint32_t>(SkyType::Physical) && (typeStored || type == 0), "Invalid cooked sky type.");
    skyType = typeStored ? std::optional<SkyType>(static_cast<SkyType>(type)) : std::nullopt;
    physicalSky.reset();

    if(!readFlag("Invalid cooked physical sky presence."))
    {
        return;
    }

    PhysicalSkySettings sky;
    const std::uint32_t model = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
    Require(model <= static_cast<std::uint32_t>(PhysicalSkyModel::CustomPlanet), "Invalid cooked physical sky model.");
    sky.model = static_cast<PhysicalSkyModel>(model);
    sky.planetRadiusKm = reader.ReadFloat();
    sky.seaLevelMeters = reader.ReadFloat();
    sky.atmosphereHeightKm = reader.ReadFloat();
    ReadChannels(reader, sky.groundAlbedo);
    ReadChannels(reader, sky.rayleighScattering);
    ReadChannels(reader, sky.rayleighAbsorption);
    sky.rayleighScaleHeightKm = reader.ReadFloat();
    ReadChannels(reader, sky.mieScattering);
    ReadChannels(reader, sky.mieAbsorption);
    sky.mieAnisotropy = reader.ReadFloat();
    sky.mieScaleHeightKm = reader.ReadFloat();
    ReadChannels(reader, sky.ozoneAbsorption);
    sky.ozoneMinimumAltitudeKm = reader.ReadFloat();
    sky.ozoneLayerWidthKm = reader.ReadFloat();
    sky.multipleScattering = reader.ReadFloat();
    sky.atmosphericScattering = readFlag("Invalid cooked physical sky atmospheric scattering flag.");
    sky.showGroundBelowHorizon = readFlag("Invalid cooked physical sky ground flag.");
    sky.sampleBelowHorizon = readFlag("Invalid cooked physical sky below-horizon sampling flag.");
    const std::uint32_t intensityMode = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
    Require(intensityMode <= static_cast<std::uint32_t>(PhysicalSkyIntensityMode::Multiplier),
        "Invalid cooked physical sky intensity mode.");
    sky.intensityMode = static_cast<PhysicalSkyIntensityMode>(intensityMode);
    sky.exposureCompensation = reader.ReadFloat();
    sky.multiplier = reader.ReadFloat();
    sky.colorSaturation = reader.ReadFloat();
    ReadChannels(reader, sky.horizonTint);
    ReadChannels(reader, sky.zenithTint);
    sky.horizonZenithShift = reader.ReadFloat();
    const std::uint32_t updateMode = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
    Require(updateMode <= static_cast<std::uint32_t>(PhysicalSkyUpdateMode::Realtime), "Invalid cooked physical sky update mode.");
    sky.updateMode = static_cast<PhysicalSkyUpdateMode>(updateMode);
    sky.updatePeriodSeconds = reader.ReadFloat();

    std::string error;
    Require(ValidatePhysicalSkySettings(sky, &error), error.c_str());
    physicalSky = sky;
}

// Physical sky S5 (V41): a physical sky's night sky and cloud layers.
void WriteNightAndClouds(Writer& writer, const PhysicalSkySettings& sky)
{
    writer.WriteFloat(sky.nightSkyIntensity);
    writer.WriteFloat(sky.airglowLuminance);
    writer.WriteFloat(sky.planetRotationDegrees);
    writer.WriteUnsigned(static_cast<std::uint32_t>(sky.cloudLayers.size()), 4);

    for(const CloudLayerSettings& layer : sky.cloudLayers)
    {
        writer.WriteUnsigned(static_cast<std::uint32_t>(layer.kind), 4);
        writer.WriteUnsigned(static_cast<std::uint32_t>(layer.controlMode), 4);
        writer.WriteString(layer.preset);

        for(const float value : {layer.altitudeKm, layer.thicknessKm, layer.coverage, layer.density, layer.shapeScaleKm,
            layer.detailScaleKm, layer.erosion, layer.shapeFactor, layer.phaseForward, layer.phaseBackward, layer.phaseBlend,
            layer.multipleScattering, layer.ambientStrength, layer.windDirectionDegrees, layer.windSpeedMetersPerSecond})
        {
            writer.WriteFloat(value);
        }

        WriteChannels(writer, layer.albedo);
        writer.WriteUnsigned(layer.castShadows ? 1 : 0, 4);
        writer.WriteString(layer.coverageMap);
        writer.WriteFloat(layer.coverageMapExtentKm);
    }
}

void ReadNightAndClouds(Reader& reader, PhysicalSkySettings& sky)
{
    sky.nightSkyIntensity = reader.ReadFloat();
    sky.airglowLuminance = reader.ReadFloat();
    sky.planetRotationDegrees = reader.ReadFloat();
    const std::uint32_t count = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
    Require(count <= PhysicalSkySettings::MaximumCloudLayers, "Invalid cooked cloud layer count.");
    sky.cloudLayers.clear();

    for(std::uint32_t index = 0; index < count; ++index)
    {
        CloudLayerSettings layer;
        const std::uint32_t kind = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
        const std::uint32_t mode = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
        Require(kind <= static_cast<std::uint32_t>(CloudLayerKind::Volumetric) &&
            mode <= static_cast<std::uint32_t>(CloudControlMode::Manual), "Invalid cooked cloud layer kind or control mode.");
        layer.kind = static_cast<CloudLayerKind>(kind);
        layer.controlMode = static_cast<CloudControlMode>(mode);
        layer.preset = reader.ReadString();

        for(float* value : {&layer.altitudeKm, &layer.thicknessKm, &layer.coverage, &layer.density, &layer.shapeScaleKm,
            &layer.detailScaleKm, &layer.erosion, &layer.shapeFactor, &layer.phaseForward, &layer.phaseBackward, &layer.phaseBlend,
            &layer.multipleScattering, &layer.ambientStrength, &layer.windDirectionDegrees, &layer.windSpeedMetersPerSecond})
        {
            *value = reader.ReadFloat();
        }

        ReadChannels(reader, layer.albedo);
        const std::uint32_t shadows = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
        Require(shadows <= 1, "Invalid cooked cloud shadow flag.");
        layer.castShadows = shadows == 1;
        layer.coverageMap = reader.ReadString();
        layer.coverageMapExtentKm = reader.ReadFloat();
        sky.cloudLayers.push_back(std::move(layer));
    }

    std::string error;
    Require(ValidatePhysicalSkySettings(sky, &error), error.c_str());
}

void WriteMistEmitter(Writer& writer, const std::optional<MistEmitterComponent>& emitter)
{
    writer.WriteUnsigned(emitter.has_value() ? 1 : 0, 4);

    if(!emitter)
    {
        return;
    }

    writer.WriteUnsigned(emitter->enabled ? 1 : 0, 4);
    writer.WriteUnsigned(static_cast<std::uint32_t>(emitter->shape), 4);
    writer.WriteVector(emitter->emissionOrigin);
    writer.WriteVector(emitter->emissionRotation);
    writer.WriteFloat(emitter->radius);
    writer.WriteVector(emitter->halfExtents);
    writer.WriteFloat(emitter->rate);
    writer.WriteVector(emitter->color);
    writer.WriteFloat(emitter->density);
    writer.WriteFloat(emitter->temperature);
    writer.WriteUnsigned(static_cast<std::uint32_t>(emitter->mode), 4);
    writer.WriteUnsigned(MistEmitterModeFieldCount, 4);
    writer.WriteFloat(emitter->emissionRate);
    writer.WriteFloat(emitter->jetSpeed);
    writer.WriteFloat(emitter->temperatureResponseRate);
    writer.WriteFloat(emitter->targetDensity);
    writer.WriteFloat(emitter->replenishmentRate);
    writer.WriteFloat(emitter->fillHeight);
    writer.WriteFloat(emitter->edgeSoftness);
    writer.WriteFloat(emitter->volumeTemperatureRate);
}

void ReadMistEmitter(Reader& reader, std::optional<MistEmitterComponent>& emitter, std::uint32_t version)
{
    const std::uint32_t present = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
    Require(present <= 1, "Invalid Mist Emitter presence.");

    if(present == 0)
    {
        emitter.reset();
        return;
    }

    MistEmitterComponent value;
    const std::uint32_t enabled = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
    Require(enabled <= 1, "Invalid Mist Emitter enabled flag.");
    value.enabled = enabled == 1;
    value.shape = static_cast<MistEmitterShape>(reader.ReadUnsigned(4));
    value.emissionOrigin = reader.ReadVector();
    value.emissionRotation = reader.ReadVector();
    value.radius = reader.ReadFloat();
    value.halfExtents = reader.ReadVector();
    value.rate = reader.ReadFloat();
    value.color = reader.ReadVector();
    value.density = reader.ReadFloat();
    // Before V29 an emitter has no temperature: 0, no heating.
    value.temperature = version >= MistEmitterTemperatureVersion ? reader.ReadFloat() : 0.0f;

    // Before V30 an emitter is Legacy, with the defaults for the Point fields.
    if(version >= MistEmitterModeVersion)
    {
        value.mode = static_cast<MistEmitterMode>(reader.ReadUnsigned(4));
        const std::uint32_t fieldCount = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
        Require(fieldCount <= MaximumMistEmitterModeFields, "Invalid Mist Emitter mode field count.");

        // In stored order; a shorter list leaves the rest at their defaults, and fields past these are skipped.
        float* const fields[] = {&value.emissionRate, &value.jetSpeed, &value.temperatureResponseRate, &value.targetDensity,
            &value.replenishmentRate, &value.fillHeight, &value.edgeSoftness, &value.volumeTemperatureRate};

        for(std::uint32_t field = 0; field < fieldCount; ++field)
        {
            const float number = reader.ReadFloat();

            if(field < std::size(fields))
            {
                *fields[field] = number;
            }
        }
    }

    std::string error;
    Require(ValidateMistEmitterComponent(value, &error), error.c_str());
    emitter = value;
}

void ReadProbeVolume(Reader& reader, std::optional<ProbeVolumeComponent>& volume, std::uint32_t version)
{
    const auto presence = reader.ReadUnsigned(4);
    Require(presence <= 1, "Invalid cooked Probe Volume presence flag.");

    if(presence == 0)
    {
        return;
    }

    const auto enabled = reader.ReadUnsigned(4);
    Require(enabled <= 1, "Invalid cooked Probe Volume enabled flag.");
    const auto placement = reader.ReadUnsigned(4);
    Require(placement <= static_cast<std::uint32_t>(ProbePlacement::Uniform3D), "Invalid cooked probe placement.");
    ProbeVolumeComponent value;
    value.enabled = enabled != 0;
    value.placement = static_cast<ProbePlacement>(placement);

    for(float& edge : value.size)
    {
        edge = reader.ReadFloat();
    }

    value.probeSpacing = reader.ReadFloat();
    value.heightAboveFloor = reader.ReadFloat();
    const auto count = reader.ReadUnsigned(4);
    Require(count <= ProbeVolumeComponent::MaximumManualProbes, "Too many cooked manual probes.");

    for(std::uint64_t index = 0; index < count; ++index)
    {
        std::array<float, 3> probe{};
        probe[0] = reader.ReadFloat();
        probe[1] = reader.ReadFloat();
        probe[2] = reader.ReadFloat();
        value.manualProbes.push_back(probe);
    }

    value.probeBatch = reader.ReadString();
    value.filterInsideSolids = false;

    if(version >= ProbeFilterVersion)
    {
        const auto filter = reader.ReadUnsigned(4);
        Require(filter <= 1, "Invalid cooked Probe Volume filter flag.");
        value.filterInsideSolids = filter != 0;
    }

    if(version >= BakedReflectionsVersion)
    {
        value.bakeSettings.rays = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
        value.bakeSettings.diffuseSamples = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
        value.bakeSettings.bounces = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
        value.bakeSettings.irradianceMinimumDistance = reader.ReadFloat();
        value.bakedReflections = reader.ReadString();
    }

    if(version >= PathingVersion)
    {
        const auto pathing = reader.ReadUnsigned(4);
        Require(pathing <= 1, "Invalid cooked Probe Volume pathing flag.");
        value.pathingEnabled = pathing != 0;
        value.pathingSettings.samples = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
        value.pathingSettings.radius = reader.ReadFloat();
        value.pathingSettings.threshold = reader.ReadFloat();
        value.pathingSettings.visibilityRange = reader.ReadFloat();
        value.pathingSettings.pathRange = reader.ReadFloat();
        value.bakedPathing = reader.ReadString();
    }

    std::string error;
    Require(ValidateProbeVolume(value, &error), error.c_str());
    volume = std::move(value);
}

bool ReadCookedFlag(Reader& reader, const char* error)
{
    const auto flag = reader.ReadUnsigned(4);
    Require(flag <= 1, error);
    return flag != 0;
}

void ReadAudioComponents(Reader& reader, std::uint32_t version, std::optional<AudioSourceComponent>& source,
    std::optional<AudioListenerComponent>& listener)
{
    const auto sourcePresence = reader.ReadUnsigned(4);
    Require(sourcePresence <= 1, "Invalid cooked Audio Source presence flag.");

    if(sourcePresence == 1)
    {
        const auto enabled = reader.ReadUnsigned(4);
        Require(enabled <= 1, "Invalid cooked Audio Source enabled flag.");
        AudioSourceComponent value;
        value.enabled = enabled != 0;
        value.eventGuid = reader.ReadString();
        value.eventPath = reader.ReadString();
        const auto autoplay = reader.ReadUnsigned(4);
        Require(autoplay <= 1, "Invalid cooked Audio Source autoplay flag.");
        value.autoplay = autoplay != 0;

        if(version >= ReflectionSettingsVersion)
        {
            value.sourceCentricReflections = ReadCookedFlag(reader, "Invalid cooked Audio Source reflections flag.");
            value.bakedReflections = ReadCookedFlag(reader, "Invalid cooked Audio Source baked flag.");
        }

        if(version >= PathingVersion)
        {
            value.pathing = ReadCookedFlag(reader, "Invalid cooked Audio Source pathing flag.");
        }

        std::string error;
        Require(ValidateAudioSourceComponent(value, &error), error.c_str());
        source = std::move(value);
    }

    const auto listenerPresence = reader.ReadUnsigned(4);
    Require(listenerPresence <= 1, "Invalid cooked Audio Listener presence flag.");

    if(listenerPresence == 1)
    {
        const auto enabled = reader.ReadUnsigned(4);
        Require(enabled <= 1, "Invalid cooked Audio Listener enabled flag.");
        AudioListenerComponent value;
        value.enabled = enabled != 0;

        if(version >= ReflectionSettingsVersion)
        {
            value.bakedReflections = ReadCookedFlag(reader, "Invalid cooked Audio Listener baked flag.");
        }

        listener = value;
    }
}

std::optional<LightComponent> ReadLightComponent(Reader& reader, std::uint32_t version, float& legacyRange)
{
    const auto presence = reader.ReadUnsigned(4);
    Require(presence <= 1, "Invalid cooked Light presence flag.");

    if(presence == 0)
    {
        return std::nullopt;
    }

    Require(reader.ReadUnsigned(4) == (version >= PhysicalLightingVersion ? 44u : version == ObjectLightingVersion ? 40u : 36u),
        "Invalid cooked Light record length.");
    const auto enabled = reader.ReadUnsigned(4);
    const auto type = reader.ReadUnsigned(4);
    Require(enabled <= 1, "Invalid cooked Light enabled flag.");
    Require(type <= static_cast<std::uint32_t>(LightType::Directional), "Unsupported cooked Light type.");

    LightComponent light;
    light.enabled = enabled != 0;
    light.type = static_cast<LightType>(type);
    light.color = reader.ReadVector();
    light.intensity = reader.ReadFloat();
    const bool legacyUnits = version < PhysicalLightingVersion;
    light.range = reader.ReadFloat();
    light.innerConeAngle = reader.ReadFloat();
    light.outerConeAngle = reader.ReadFloat();
    if(version >= ObjectLightingVersion) light.angularRadius = reader.ReadFloat();

    if(legacyUnits)
    {
        // Historical lights always faded to zero at their range.
        legacyRange = light.range;
        light.rangeAttenuation = true;
        ValidateLegacyLightValues(light, legacyRange);
        return light;
    }

    const auto flags = reader.ReadUnsigned(4);
    Require(flags <= (version >= LightAtmosphereVersion ? 3u : 1u), "Invalid cooked Light range attenuation flag.");
    light.rangeAttenuation = (flags & 1u) != 0;
    light.affectsAtmosphere = (flags & 2u) == 0;

    std::string error;

    if(!ValidateLightComponent(light, &error))
    {
        throw std::runtime_error("Invalid cooked Light: " + error);
    }

    return light;
}

std::vector<std::byte> Encode(const Metadata& sceneMetadata, const Policy& policy)
{
    std::string persistenceError;

    if(!ValidateSceneDocumentPersistence(sceneMetadata.document, &persistenceError))
    {
        throw std::runtime_error(persistenceError);
    }

    ValidateMetadata(sceneMetadata, policy);

    Writer writer{policy, {}};
    for (size_t index = 0; index < 8; ++index)
    {
        writer.WriteUnsigned(static_cast<unsigned char>(Magic[index]), 1);
    }
    // Header: magic, version, header size, total bytes, object count and flags.
    writer.WriteUnsigned(Version, 4);
    writer.WriteUnsigned(HeaderBytes, 4);
    writer.WriteUnsigned(0, 8);
    writer.WriteUnsigned(sceneMetadata.document.objects.size(), 4);
    writer.WriteUnsigned(sceneMetadata.document.environmentReference == SceneDocument::NoneEnvironmentReference ? 2 :
                         (sceneMetadata.environment ? 1 : 0), 4);

    // Scene fields precede the object records in every supported version.
    const auto& document = sceneMetadata.document;
    writer.WriteString(document.id);
    writer.WriteString(document.name);
    writer.WriteVector(document.camera.position);
    writer.WriteVector(document.camera.target);
    writer.WriteFloat(document.camera.verticalFov);
    writer.WriteFloat(document.camera.aspectRatio);
    writer.WriteFloat(document.camera.nearPlane);
    writer.WriteFloat(document.camera.farPlane);
    writer.WriteFloat(document.lightingSettings.referenceEV100);
    writer.WriteFloat(document.lightingSettings.environmentLuminanceScale);
    WriteCameraExposure(writer, document.camera.exposure);
    WriteEnvironmentalHaze(writer, document.environmentalHaze);
    WriteAirConfiguration(writer, document.airEnvironment, document.airSimulation);
    WriteMistRendering(writer, document.mistRendering);
    WriteVoxelBake(writer, document.voxelBake);
    WriteMovingSolidSetting(writer, document.airSimulation);
    WriteSky(writer, document.skyType, document.physicalSky);
    // Physical sky S4 (V40): the HDRI sky's yaw, after the sky record.
    writer.WriteFloat(document.lightingSettings.environmentRotationDegrees);

    // Physical sky S5 (V41): a physical sky's night sky and clouds.
    if(document.physicalSky)
    {
        WriteNightAndClouds(writer, *document.physicalSky);
    }

    for (size_t index = 0; index < document.objects.size(); ++index)
    {
        const auto& object = document.objects[index];
        // Transforms are authored local TRS; a root's local is its world transform.
        writer.WriteUnsigned(object.parentID.empty() ? RootMarker : ParentMarker, 4);
        writer.WriteString(object.id);
        writer.WriteString(object.name);

        if(!object.parentID.empty())
        {
            writer.WriteString(object.parentID);
        }

        writer.WriteVector(object.transform.position);
        writer.WriteVector(object.transform.rotation);
        writer.WriteVector(object.transform.scale);

        writer.WriteString(object.meshReference);
        writer.WriteString(object.materialReference);
        writer.WriteUnsigned(object.materialSlotBindings.size(), 4);
        for (const auto& [slot, reference] : object.materialSlotBindings)
        {
            writer.WriteString(slot);
            writer.WriteString(reference);
        }

        writer.WriteUnsigned(sceneMetadata.meshes[index] ? 1 : 0, 4);
        if (const auto& mesh = sceneMetadata.meshes[index])
        {
            writer.WriteUnsigned(mesh->generation, 8);
            writer.WriteUnsigned(mesh->payload.payloadBytes, 8);
            writer.WriteUnsigned(mesh->payload.formatVersion, 4);
            writer.WriteUnsigned(mesh->payload.cookCompatibility, 4);
            for (auto byte : mesh->payload.payloadSHA256)
            {
                writer.WriteUnsigned(byte, 1);
            }
            writer.WriteUnsigned(mesh->slots.size(), 4);
            for (const auto& slot : mesh->slots)
            {
                writer.WriteString(slot.id);
                writer.WriteString(slot.name);
                writer.WriteUnsigned(slot.sourceMaterialIndex, 4);
            }
        }

        WritePhysicsComponent(writer, object.physics);
        WriteTriggerCollider(writer, object.triggerCollider);
        WriteCharacterController(writer, object.characterController);
        WriteCameraComponent(writer, object.camera);
        WriteLightComponent(writer, object.light);
        WriteScriptComponent(writer, object.script);
        WriteAudioComponents(writer, object.audioSource, object.audioListener);
        WriteAcousticSettings(writer, object.acoustics);
        WriteProbeVolume(writer, object.probeVolume);
        WriteAirMotor(writer, object.airMotor);
        WriteMistEmitter(writer, object.mistEmitter);
    }

    if (sceneMetadata.environment)
    {
        writer.WriteString(document.environmentReference);
        writer.WriteUnsigned(sceneMetadata.environment->generation, 8);
        for (auto byte : sceneMetadata.environment->sha256)
        {
            writer.WriteUnsigned(std::to_integer<unsigned>(byte), 1);
        }
    }

    // Backpatch total length only after the optional environment record is written.
    for (size_t index = 0; index < 8; ++index)
    {
        writer.bytes[PayloadBytesOffset + index] =
            std::byte((static_cast<std::uint64_t>(writer.bytes.size()) >> (8 * index)) & 255);
    }
    return std::move(writer.bytes);
}
} // namespace
std::vector<std::byte> Cook(const SceneDocument& document, std::span<const MeshInput> meshes,
                            const ProjectAssetRegistry& registry, const Policy& policy,
                            const EnvironmentInput* environment)
{
    std::string persistenceError;

    if(!ValidateSceneDocumentPersistence(document, &persistenceError))
    {
        throw std::runtime_error(persistenceError);
    }

    Require(meshes.size() <= policy.maxObjects && document.objects.size() <= policy.maxObjects &&
                document.objects.size() <= UINT32_MAX,
            "Scene/input object count exceeds admission.");
    Metadata sceneMetadata{document, {}};
    sceneMetadata.meshes.reserve(document.objects.size());
    std::map<std::string, const MeshInput*> inputs;
    for (const auto& mesh : meshes)
    {
        Require(mesh.generation && inputs.emplace(mesh.id.ToString(), &mesh).second,
                "Duplicate or invalid prepared mesh input.");
    }
    std::map<std::string, MeshBinding> resolved;
    const auto material = [&](const std::string& reference)
    {
        Require(IsSupportedMaterialReference(reference), "Unsupported authored material reference.");
        if (auto id = AssetID::ParseReference(reference))
        {
            auto* materialRecord = registry.Find(*id);
            Require(materialRecord && materialRecord->type == AssetType::Material,
                    "Unresolved authored material AssetID/type.");
        }
    };
    for (const auto& object : document.objects)
    {
        material(object.materialReference);
        for (const auto& [slot, reference] : object.materialSlotBindings)
        {
            material(reference);
        }
        if (object.meshReference.empty())
        {
            sceneMetadata.meshes.emplace_back();
            continue;
        }
        const auto id = AssetID::ParseReference(object.meshReference);
        Require(id && id->ToReference() == object.meshReference,
                "Unsupported mesh reference; prepared cooked AssetID required.");
        const auto input = inputs.find(id->ToString());
        Require(input != inputs.end(), "Unresolved cooked mesh reference.");
        auto found = resolved.find(id->ToString());
        if (found == resolved.end())
        {
            auto metadata = CookedMesh::DecodeMetadata(input->second->payload, {});
            Require(metadata.status == CookedMesh::Status::Success,
                    "Incompatible or invalid prepared cooked mesh.");
            found = resolved
                        .emplace(id->ToString(),
                                 MeshBinding{metadata.snapshot->evidence, input->second->generation,
                                             metadata.snapshot->metadata.materialSlots})
                        .first;
        }
        sceneMetadata.meshes.push_back(found->second);
    }
    // Physical sky S0: a Neutral or Physical sky keeps its stored HDRI reference but needs no environment; none is cooked.
    if (SceneSkyUsesEnvironmentAsset(document))
    {
        const auto id = AssetID::ParseReference(document.environmentReference);
        const auto* record = id ? registry.Find(*id) : nullptr;
        Require(id && record && record->type == AssetType::Texture && environment && environment->id == *id,
                "Missing authored HDR environment input/type.");
        CookedEnvironment::Decode(environment->payload, *id, environment->generation);
        sceneMetadata.environment =
            EnvironmentBinding{environment->generation, CookedSurface::Hash(environment->payload)};
    }
    else
    {
        Require(!environment, "Unexpected environment input for unassigned scene.");
    }
    return Encode(sceneMetadata, policy);
}

std::vector<std::byte> CookFile(const std::filesystem::path& file, std::span<const MeshInput> meshes,
                                const ProjectAssetRegistry& registry, const Policy& policy)
{
    SceneDocument document;
    std::string error;
    if (!LoadSceneDocumentForCooking(file, document, policy.maxSourceBytes, &error))
    {
        throw std::runtime_error("Scene cooker: " + error);
    }
    return Cook(document, meshes, registry, policy);
}

Metadata Decode(std::span<const std::byte> bytes, const Policy& policy)
{
    Require(bytes.size() >= HeaderBytes && bytes.size() <= policy.maxBytes,
            "Cooked scene byte limit or truncated header.");
    // Read and admit the header before reserving object/mesh storage.
    Reader reader{bytes, policy};
    std::string magic;
    for (size_t index = 0; index < 8; ++index)
    {
        magic.push_back(static_cast<char>(reader.ReadUnsigned(1)));
    }
    const auto payloadVersion = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
    Require(IsSupportedVersion(payloadVersion), "Unsupported cooked scene version.");
    const char* expectedMagic = Magic;

    if(payloadVersion == LegacyVersion)
    {
        expectedMagic = LegacyMagic;
    }
    else if(payloadVersion == BoxPhysicsVersion)
    {
        expectedMagic = BoxPhysicsMagic;
    }
    else if(payloadVersion == SpherePhysicsVersion)
    {
        expectedMagic = SpherePhysicsMagic;
    }
    else if(payloadVersion == CapsulePhysicsVersion)
    {
        expectedMagic = CapsulePhysicsMagic;
    }
    else if(payloadVersion == CapsuleRotationVersion)
    {
        expectedMagic = CapsuleRotationMagic;
    }
    else if(payloadVersion == ContactEventsVersion)
    {
        expectedMagic = ContactEventsMagic;
    }
    else if(payloadVersion == TriggerColliderVersion)
    {
        expectedMagic = TriggerColliderMagic;
    }
    else if(payloadVersion == CharacterControllerVersion)
    {
        expectedMagic = CharacterControllerMagic;
    }
    else if(payloadVersion == HierarchyVersion)
    {
        expectedMagic = HierarchyMagic;
    }
    else if(payloadVersion == CameraVersion)
    {
        expectedMagic = CameraMagic;
    }

    if(payloadVersion == LightVersion) expectedMagic = LightMagic;
    if(payloadVersion == EnvironmentNoneVersion) expectedMagic = EnvironmentNoneMagic;
    if(payloadVersion == ObjectLightingVersion) expectedMagic = ObjectLightingMagic;
    if(payloadVersion == PhysicalLightingVersion) expectedMagic = PhysicalLightingMagic;

    Require(magic == expectedMagic, "Invalid cooked scene magic/version pair.");
    // Keep this short-circuit read: a bad header size does not consume the length.
    Require(reader.ReadUnsigned(4) == HeaderBytes && reader.ReadUnsigned(8) == bytes.size(),
            "Invalid cooked scene header/size.");
    const auto count = reader.ReadUnsigned(4);
    Require(count <= policy.maxObjects && count <= (bytes.size() - HeaderBytes) / 64,
            "Invalid cooked scene count.");
    const auto flags = reader.ReadUnsigned(4);
    Require(flags <= (payloadVersion >= EnvironmentNoneVersion ? 2u : 1u), "Unsupported cooked scene flags.");

    Metadata sceneMetadata;
    sceneMetadata.payloadVersion = payloadVersion;
    auto& document = sceneMetadata.document;
    document.id = reader.ReadString();
    document.name = reader.ReadString();
    document.camera.position = reader.ReadVector();
    document.camera.target = reader.ReadVector();
    document.camera.verticalFov = reader.ReadFloat();
    document.camera.aspectRatio = reader.ReadFloat();
    document.camera.nearPlane = reader.ReadFloat();
    document.camera.farPlane = reader.ReadFloat();
    std::optional<LegacySceneSun> legacySun;
    float legacyAmbientStrength = 0.0f;
    std::vector<LegacyLightValues> legacyLights;
    std::vector<size_t> camerasWithoutExposure;
    const bool viewportExposure = payloadVersion >= CameraExposureVersion;
    if(payloadVersion >= PhysicalLightingVersion)
    {
        document.lightingSettings.referenceEV100 = reader.ReadFloat();
        document.lightingSettings.environmentLuminanceScale = reader.ReadFloat();
        if(viewportExposure) document.camera.exposure = ReadCameraExposure(reader);

        // AIR.4F.2d.3: before V31 a scene has no haze.
        if(payloadVersion >= EnvironmentalHazeVersion)
        {
            ReadEnvironmentalHaze(reader, payloadVersion, document.environmentalHaze);
        }

        // R1.1: before V34 a scene has neither block, so it resolves to the defaults.
        if(payloadVersion >= AirConfigurationVersion)
        {
            ReadAirConfiguration(reader, document.airEnvironment, document.airSimulation);
        }

        // R1.3: before V35 a scene has no cooked mist appearance (the defaults apply).
        if(payloadVersion >= MistRenderingVersion)
        {
            ReadMistRendering(reader, document.mistRendering);
        }

        // R1.4a: before V36 a scene has no cooked voxel bake.
        if(payloadVersion >= VoxelBakeVersion)
        {
            ReadVoxelBake(reader, document.voxelBake);
        }

        // R1.5: before V37 a scene's AIR block (if any) reads moving solids off.
        if(payloadVersion >= MovingSolidParticipationVersion)
        {
            ReadMovingSolidSetting(reader, document.airSimulation);
        }

        // Physical sky S0: before V38 a scene stores no sky type (it is derived from the environment reference) and no physical sky.
        if(payloadVersion >= PhysicalSkyVersion)
        {
            ReadSky(reader, document.skyType, document.physicalSky);
        }

        // Physical sky S4: before V40 an HDRI sky is not rotated.
        if(payloadVersion >= HdriRotationVersion)
        {
            document.lightingSettings.environmentRotationDegrees = reader.ReadFloat();
            Require(std::isfinite(document.lightingSettings.environmentRotationDegrees) &&
                std::abs(document.lightingSettings.environmentRotationDegrees) <=
                SceneLightingSettings::MaximumEnvironmentRotationDegrees, "Invalid cooked HDRI rotation.");
        }

        // Physical sky S5: before V41 a physical sky has the default night sky and no clouds.
        if(payloadVersion >= CloudVersion && document.physicalSky)
        {
            ReadNightAndClouds(reader, *document.physicalSky);
        }
    }
    else if(payloadVersion == ObjectLightingVersion)
    {
        legacyAmbientStrength = reader.ReadFloat();
        Require(std::isfinite(legacyAmbientStrength) && legacyAmbientStrength >= 0.0f, "Invalid cooked ambient strength.");
    }
    else
    {
        legacySun.emplace();
        legacySun->direction = reader.ReadVector();
        legacySun->color = reader.ReadVector();
        legacySun->intensity = reader.ReadFloat();
        legacySun->ambientStrength = reader.ReadFloat();
        legacySun->angularRadius = reader.ReadFloat();
    }

    document.objects.reserve(static_cast<size_t>(count));
    sceneMetadata.meshes.reserve(static_cast<size_t>(count));
    for (size_t index = 0; index < count; ++index)
    {
        const auto hierarchyMarker = reader.ReadUnsigned(4);
        const bool hasParent = hierarchyMarker == ParentMarker;
        Require(hierarchyMarker == RootMarker || (hasParent && payloadVersion >= HierarchyVersion),
                "Invalid cooked hierarchy marker for this payload version.");
        SceneObjectDocument object;
        object.id = reader.ReadString();
        object.name = reader.ReadString();

        if(hasParent)
        {
            // An empty parent string would be an ambiguous second root encoding.
            object.parentID = reader.ReadString();
            Require(!object.parentID.empty(), "Cooked parent marker requires a nonempty parent ID.");
        }

        object.transform.position = reader.ReadVector();
        object.transform.rotation = reader.ReadVector();
        object.transform.scale = reader.ReadVector();

        object.meshReference = reader.ReadString();
        object.materialReference = reader.ReadString();
        const auto bindings = reader.ReadUnsigned(4);
        Require(bindings <= policy.maxSlots && bindings <= (bytes.size() - reader.cursor) / 8,
                "Invalid slot binding count.");
        std::string previous;
        for (size_t slotIndex = 0; slotIndex < bindings; ++slotIndex)
        {
            auto slot = reader.ReadString();
            auto reference = reader.ReadString();
            Require(slot > previous, "Unsorted/duplicate slot binding.");
            previous = slot;
            object.materialSlotBindings.emplace(std::move(slot), std::move(reference));
        }

        const auto present = reader.ReadUnsigned(4);
        Require(present <= 1, "Invalid mesh presence flag.");
        std::optional<MeshBinding> mesh;
        if (present)
        {
            mesh.emplace();
            mesh->generation = reader.ReadUnsigned(8);
            mesh->payload.payloadBytes = reader.ReadUnsigned(8);
            const auto version = reader.ReadUnsigned(4);
            Require(version <= UINT16_MAX, "Invalid mesh payload version.");
            mesh->payload.formatVersion = static_cast<std::uint16_t>(version);
            mesh->payload.cookCompatibility = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
            for (auto& byte : mesh->payload.payloadSHA256)
            {
                byte = static_cast<std::uint8_t>(reader.ReadUnsigned(1));
            }
            const auto slots = reader.ReadUnsigned(4);
            Require(slots <= policy.maxSlots && slots <= (bytes.size() - reader.cursor) / 12,
                    "Invalid mesh slot count.");
            mesh->slots.reserve(static_cast<size_t>(slots));
            for (size_t slotIndex = 0; slotIndex < slots; ++slotIndex)
            {
                MeshMaterialSlot slot;
                slot.id = reader.ReadString();
                slot.name = reader.ReadString();
                slot.sourceMaterialIndex = static_cast<std::uint32_t>(reader.ReadUnsigned(4));
                mesh->slots.push_back(std::move(slot));
            }
        }

        // Version 1 has no physics bytes. Never consume v2 records from that layout.
        if (payloadVersion != LegacyVersion)
        {
            object.physics = ReadPhysicsComponent(reader, payloadVersion);
        }

        if(payloadVersion >= TriggerColliderVersion)
        {
            try
            {
                object.triggerCollider = ReadTriggerCollider(reader);
            }
            catch(const std::exception& exception)
            {
                throw std::runtime_error("Invalid cooked trigger for object " + object.id + ": " + exception.what());
            }
        }

        if(payloadVersion >= CharacterControllerVersion)
        {
            try
            {
                object.characterController = ReadCharacterController(reader);
            }
            catch(const std::exception& exception)
            {
                throw std::runtime_error("Invalid cooked controller for object " + object.id + ": " + exception.what());
            }
        }

        if(payloadVersion >= CameraVersion)
        {
            try
            {
                bool hasExposure = false;
                object.camera = ReadCameraComponent(reader, payloadVersion, hasExposure);
                if(object.camera && !hasExposure) camerasWithoutExposure.push_back(document.objects.size());
            }
            catch(const std::exception& exception)
            {
                throw std::runtime_error("Invalid cooked Camera for object " + object.id + ": " + exception.what());
            }
        }

        if(payloadVersion >= LightVersion)
        {
            try
            {
                float legacyRange = std::numeric_limits<float>::quiet_NaN();
                object.light = ReadLightComponent(reader, payloadVersion, legacyRange);
                if(object.light && payloadVersion < PhysicalLightingVersion && object.light->type != LightType::Directional)
                    legacyLights.push_back({object.id, legacyRange});
            }
            catch(const std::exception& exception)
            {
                throw std::runtime_error("Invalid cooked Light for object " + object.id + ": " + exception.what());
            }
        }

        if(payloadVersion >= ScriptVersion)
        {
            try
            {
                object.script = ReadScriptComponent(reader);
            }
            catch(const std::exception& exception)
            {
                throw std::runtime_error("Invalid cooked Script for object " + object.id + ": " + exception.what());
            }
        }

        if(payloadVersion >= AudioVersion)
        {
            try
            {
                ReadAudioComponents(reader, payloadVersion, object.audioSource, object.audioListener);
            }
            catch(const std::exception& exception)
            {
                throw std::runtime_error("Invalid cooked audio components for object " + object.id + ": " +
                    exception.what());
            }
        }

        if(payloadVersion >= AcousticsVersion)
        {
            try
            {
                ReadAcousticSettings(reader, object.acoustics);
            }
            catch(const std::exception& exception)
            {
                throw std::runtime_error("Invalid cooked acoustics for object " + object.id + ": " + exception.what());
            }
        }

        if(payloadVersion >= ProbeVolumeVersion)
        {
            try
            {
                ReadProbeVolume(reader, object.probeVolume, payloadVersion);
            }
            catch(const std::exception& exception)
            {
                throw std::runtime_error("Invalid cooked Probe Volume for object " + object.id + ": " + exception.what());
            }
        }

        if(payloadVersion >= AirMotorVersion)
        {
            try
            {
                ReadAirMotor(reader, object.airMotor, payloadVersion, object.name, object.id);
            }
            catch(const std::exception& exception)
            {
                throw std::runtime_error("Invalid cooked Air Motor for object " + object.id + ": " + exception.what());
            }
        }

        if(payloadVersion >= MistEmitterVersion)
        {
            try
            {
                ReadMistEmitter(reader, object.mistEmitter, payloadVersion);
            }
            catch(const std::exception& exception)
            {
                throw std::runtime_error("Invalid cooked Mist Emitter for object " + object.id + ": " + exception.what());
            }
        }

        document.objects.push_back(std::move(object));
        sceneMetadata.meshes.push_back(std::move(mesh));
    }

    if (flags & 1)
    {
        document.environmentReference = reader.ReadString();
        EnvironmentBinding environmentBinding;
        environmentBinding.generation = reader.ReadUnsigned(8);
        for (auto& byte : environmentBinding.sha256)
        {
            byte = std::byte(reader.ReadUnsigned(1));
        }
        Require(!document.environmentReference.empty() && environmentBinding.generation,
                "Invalid environment binding.");
        sceneMetadata.environment = environmentBinding;
    }

    if(flags & 2) document.environmentReference = SceneDocument::NoneEnvironmentReference;

    // Reject trailing bytes before applying document and binding validation.
    Require(reader.cursor == bytes.size(), "Trailing cooked scene bytes.");
    if(payloadVersion < ObjectLightingVersion)
    {
        legacyAmbientStrength = MigrateLegacyLighting(document, legacySun);
        sceneMetadata.meshes.resize(document.objects.size());
    }
    if(payloadVersion < PhysicalLightingVersion)
    {
        // Historical payloads are converted at the default reference exposure; repository content is re-cooked.
        MigrateLegacyLightUnits(document, payloadVersion, legacyAmbientStrength, legacyLights, {});
    }
    // Cameras cooked before exposure existed take the default exposure (Auto), with manual settings at the scene
    // reference exposure.
    const auto reference = Photometry::ReferenceCameraExposure(document.lightingSettings.referenceEV100);
    if(!viewportExposure) document.camera.exposure = reference;
    for(const size_t index : camerasWithoutExposure) document.objects[index].camera->exposure = reference;

    // Payloads cooked before collider scaling stored absolute collider meters; convert them once, as the JSON
    // reader does, before validation sees the document.
    if(payloadVersion < ColliderScaleVersion)
    {
        LogColliderScaleMigration(document, MigrateStoredColliderScale(document),
            "cooked payload " + std::to_string(payloadVersion));
    }

    ValidateMetadata(sceneMetadata, policy);
    return sceneMetadata;
}

bool ArchiveScene::ValidateReferences(std::string* error) const
{
    const auto fail = [&](const char* message)
    {
        if (error)
        {
            *error = message;
        }
        return false;
    };
    if (metadata.meshes.size() != metadata.document.objects.size())
    {
        return fail("Archive scene mesh/object count mismatch.");
    }
    if (!provider || !generation)
    {
        return fail("Archive scene ownership/generation missing.");
    }
    const auto* scene = provider->ArchiveEntry(sceneAsset);
    if (!scene || scene->type != EngineAssetPak::Type::Scene || !IsSupportedVersion(scene->payloadVersion) ||
        scene->payloadVersion != metadata.payloadVersion || scene->generation != generation)
    {
        return fail("Stale archive scene entry/type/version/generation.");
    }
    if (SceneSkyUsesEnvironmentAsset(metadata.document))
    {
        const auto id = AssetID::ParseReference(metadata.document.environmentReference);
        const auto* entry = id ? provider->ArchiveEntry(*id) : nullptr;
        if (!metadata.environment || !entry || entry->type != EngineAssetPak::Type::Resource ||
            entry->payloadVersion != CookedEnvironment::Version ||
            entry->generation != metadata.environment->generation ||
            entry->sha256 != metadata.environment->sha256)
        {
            return fail("Missing/stale authored environment entry, version, digest or generation.");
        }
    }
    else if (metadata.environment)
    {
        return fail("Unexpected environment binding.");
    }
    for (size_t index = 0; index < metadata.meshes.size(); ++index)
    {
        const auto& mesh = metadata.meshes[index];
        if (!mesh)
        {
            continue;
        }
        const auto id = AssetID::ParseReference(metadata.document.objects[index].meshReference);
        if (!id)
        {
            return fail("Invalid archive scene mesh identity.");
        }
        const auto found = provider->Lookup(*id);
        if (found.status != CookedMeshSource::Status::Success || found.evidence->payload != mesh->payload ||
            found.evidence->contentGeneration != mesh->generation)
        {
            return fail("Missing/incompatible archive mesh identity, payload or content generation.");
        }
    }
    return true;
}

std::shared_ptr<const ArchiveScene> OpenArchive(const std::filesystem::path& toc,
                                                const std::filesystem::path& pak, const AssetID& id,
                                                const Policy& policy)
{
    const auto opened = CookedMeshSource::ArchiveProvider::OpenArchive(toc, pak);
    Require(bool(opened.provider), "Cannot open cooked scene archive pair.");
    const auto* entry = opened.provider->ArchiveEntry(id);
    Require(entry && entry->type == EngineAssetPak::Type::Scene && IsSupportedVersion(entry->payloadVersion),
            "Missing or unsupported cooked scene entry.");
    auto bytes =
        opened.provider->ReadEntry(id, EngineAssetPak::Type::Scene, entry->payloadVersion, policy.maxBytes);
    auto result = std::shared_ptr<const ArchiveScene>(
        new ArchiveScene(id, entry->generation, opened.provider, Decode(bytes, policy)));
    std::string error;
    if (!result->ValidateReferences(&error))
    {
        throw std::runtime_error("Cooked scene reference validation: " + error);
    }
    return result;
}
} // namespace CookedScene
