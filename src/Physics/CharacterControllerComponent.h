#pragma once

#include "Math/Vector3.h"
#include <string>

struct Transform;

namespace Physics
{
// Generic Optional authored configuration only. Dimensions ignore visual scale.
// +Y capsule centers are localOffset +/- (0, cylinderHeight / 2, 0).
struct CharacterControllerComponent
{
    float capsuleRadius = 0.3f; // Meters, both hemispherical ends.
    float capsuleCylinderHeight = 1.2f; // Meters between centers; total height adds 2 * radius.
    Vector3 localOffset{0.0f, 0.0f, 0.0f}; // Unscaled meters from object origin to capsule center.
    float maximumHorizontalSpeed = 5.0f; // Meters/second, desired XZ velocity limit; zero disables travel.
    float gravityScale = 1.0f; // Nonnegative multiplier on world gravity; zero disables gravity.
    float jumpSpeed = 5.0f; // Meters/second upward launch velocity; zero disables jump impulse.
    float maximumWalkableSlope = 0.78539816339f; // Radians from +Y normal, [0, pi/2).
    float groundSnapDistance = 0.1f; // Meters of proposed support reacquisition; zero disables snap.
    float collisionClearance = 0.002f; // Meters added to query radius; zero permits no added margin.

    bool operator==(const CharacterControllerComponent& other) const;
};

bool ValidateCharacterControllerComponent(const CharacterControllerComponent& component,
    std::string* errorMessage = nullptr);
// Euler Rz * Ry * Rx radians: stored pitch/roll must each be within 1e-5 of zero.
// No wrapping, clamping, flattening or visual-scale inheritance. Also checks world-space precision.
bool ValidateCharacterControllerTransform(const CharacterControllerComponent& component,
    const Transform& transform, std::string* errorMessage = nullptr);
}
