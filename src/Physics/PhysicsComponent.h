#pragma once

#include "Math/Vector3.h"

#include <string>

namespace Physics
{
enum class BodyType
{
    Static,
    Dynamic,
    Kinematic
};

enum class ColliderType
{
    Box = 0,
    Sphere = 1,
    Capsule = 2
};

struct BoxDimensions
{
    float width = 1.0f;
    float height = 1.0f;
    float depth = 1.0f;

    bool operator==(const BoxDimensions&) const = default;
};

// Authored values only. Owns no Box3D resources and does not contain simulation state.
struct PhysicsComponent
{
    BodyType bodyType = BodyType::Static;
    ColliderType colliderType = ColliderType::Box;

    // Stored shape dimensions/radius and shared offset are meters, independent of visual scale.
    BoxDimensions boxDimensions;
    float sphereRadius = 0.5f;

    // Capsule along local +Y. Cylinder height excludes the hemispherical caps, so
    // total height is capsuleCylinderHeight + 2 * capsuleRadius. The endpoint centers
    // are (0, -capsuleCylinderHeight / 2, 0) and (0, +capsuleCylinderHeight / 2, 0).
    float capsuleRadius = 0.5f;
    float capsuleCylinderHeight = 1.0f;

    // Capsule-only local rotation, Euler radians composed Rz * Ry * Rx like Transform.
    // It turns the capsule axis about the offset point; it never rotates the offset itself.
    Vector3 capsuleLocalRotation{0.0f, 0.0f, 0.0f};

    Vector3 colliderLocalPositionOffset{0.0f, 0.0f, 0.0f};
    float density = 1000.0f;
    float friction = 0.5f;
    float restitution = 0.0f;

    // Opt-in to gameplay-facing contact Begin/End notifications for this object. The
    // collider stays solid: geometry, mass, materials and collision response are unchanged.
    bool generateContactEvents = false;

    bool operator==(const PhysicsComponent& other) const;
};

// Scene admission validates public authored data; malformed values are never clamped.
bool ValidatePhysicsComponent(const PhysicsComponent& component, std::string* errorMessage = nullptr);
}
