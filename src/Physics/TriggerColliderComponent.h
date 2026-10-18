#pragma once

#include "Physics/PhysicsComponent.h"

namespace Physics
{
// Authored sensor geometry only; no body, material, mesh or runtime resource ownership.
// Persisted independently of PhysicsComponent; runtime sensors are a later stage.
struct TriggerColliderComponent
{
    ColliderType colliderType = ColliderType::Box;
    BoxDimensions boxDimensions;
    float sphereRadius = 0.5f;
    float capsuleRadius = 0.5f;
    float capsuleCylinderHeight = 1.0f;

    // Unscaled meters. Capsule rotation turns its +Y axis about this offset,
    // without rotating the offset itself. Rotation is Rz * Ry * Rx in radians.
    Vector3 colliderLocalPositionOffset{0.0f, 0.0f, 0.0f};
    Vector3 capsuleLocalRotation{0.0f, 0.0f, 0.0f};

    bool operator==(const TriggerColliderComponent& other) const;
};

bool ValidateTriggerColliderComponent(const TriggerColliderComponent& component, std::string* errorMessage = nullptr);
}
