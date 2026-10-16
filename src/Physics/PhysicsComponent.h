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
    Box
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

    // Full dimensions and local offset are explicit meters, independent of object scale.
    BoxDimensions boxDimensions;
    Vector3 colliderLocalPositionOffset{0.0f, 0.0f, 0.0f};
    float density = 1000.0f;
    float friction = 0.5f;
    float restitution = 0.0f;

    bool operator==(const PhysicsComponent& other) const;
};

// Scene admission validates public authored data; malformed values are never clamped.
bool ValidatePhysicsComponent(const PhysicsComponent& component, std::string* errorMessage = nullptr);
}
