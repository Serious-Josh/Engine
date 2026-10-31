#include "assets/CookedScene.h"
#include "Scene/LegacyLightingMigration.h"
#include "Scene/PhysicalLightingMigration.h"
#include "assets/CookedEnvironment.h"
#include "assets/CookedSurface.h"
#include "Scene/SceneSerializer.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

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
    const auto& reference = sceneMetadata.document.environmentReference;
    Require(sceneMetadata.environment.has_value() ==
        (!reference.empty() && reference != SceneDocument::NoneEnvironmentReference),
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
    writer.WriteUnsigned(light->rangeAttenuation ? 1 : 0, 4);
}

// Payloads before 14 carry legacy units: their range goes to legacyRange and the historical rules apply
// (MigrateLegacyLightUnits converts the document afterwards).
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

    const auto rangeAttenuation = reader.ReadUnsigned(4);
    Require(rangeAttenuation <= 1, "Invalid cooked Light range attenuation flag.");
    light.rangeAttenuation = rangeAttenuation != 0;

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
    if (!document.environmentReference.empty() && document.environmentReference != SceneDocument::NoneEnvironmentReference)
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
    if (!LoadSceneDocumentForCooking(file, document, policy.maxBytes, &error))
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
    if (!metadata.document.environmentReference.empty() && metadata.document.environmentReference != SceneDocument::NoneEnvironmentReference)
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
