#pragma once

#include "Scene/Transform.h"
#include "Scene/DirectionalLight.h"
#include "Physics/PhysicsComponent.h"
#include "Physics/TriggerColliderComponent.h"
#include "Physics/CharacterControllerComponent.h"

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <optional>

struct SceneCameraDocument
{
    Vector3 position{0.0f, 0.0f, -5.0f};
    Vector3 target{0.0f, 0.0f, 0.0f};

    float verticalFov = 1.04719755f;
    float aspectRatio = 16.0f / 9.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
};

struct SceneObjectDocument
{
    std::string id;
    std::string name = "Object";


    Transform transform;

    //persistant asset references
    std::string meshReference;
    std::string materialReference;

    // Canonical material asset references keyed by stable mesh-local slot ID.
    // Missing entries inherit materialReference (or builtin:default when empty).
    std::map<std::string, std::string> materialSlotBindings;

    // Optional authored configuration; no runtime body or transform ownership.
    std::optional<Physics::PhysicsComponent> physics;

    // Optional authored trigger geometry; owns no sensor resources.
    std::optional<Physics::TriggerColliderComponent> triggerCollider;

    // Generic Optional authored configuration; exclusive with PhysicsComponent.
    std::optional<Physics::CharacterControllerComponent> characterController;

    // Empty means root; otherwise the exact ID of another object in this scene. Persisted from
    // schema 9 / cooked payload 9; historical versions load every object as a root.
    std::string parentID;
};

struct SceneDocument
{
    static constexpr std::uint32_t kLegacySchemaVersion = 1;
    static constexpr std::uint32_t kBoxPhysicsSchemaVersion = 2;
    static constexpr std::uint32_t kSphereSchemaVersion = 3;
    static constexpr std::uint32_t kCapsuleSchemaVersion = 4;
    static constexpr std::uint32_t kCapsuleRotationSchemaVersion = 5;
    static constexpr std::uint32_t kContactEventsSchemaVersion = 6;
    static constexpr std::uint32_t kTriggerColliderSchemaVersion = 7;
    static constexpr std::uint32_t kCharacterControllerSchemaVersion = 8;
    static constexpr std::uint32_t kHierarchySchemaVersion = 9;
    static constexpr std::uint32_t kSchemaVersion = kHierarchySchemaVersion;

    std::uint32_t schemaVersion = kSchemaVersion;

    std::string id;
    std::string name = "Untitled Scene";
    // Empty selects neutral; otherwise a canonical project texture asset reference.
    std::string environmentReference;

    std::vector<SceneObjectDocument> objects;

    SceneCameraDocument camera;
    DirectionalLight directionalLight;
};

bool ValidateSceneDocument(
    const SceneDocument& document,
    std::string* errorMessage = nullptr);

// Shared authoritative component/coexistence validation for proposed object values.
bool ValidateSceneObjectComponents(const SceneObjectDocument& object, std::string* errorMessage = nullptr);
// Shared output admission; all current codecs preserve the complete authored document.
bool ValidateSceneDocumentPersistence(const SceneDocument& document, std::string* errorMessage = nullptr);
