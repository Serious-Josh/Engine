#include "Physics/CharacterControllerComponent.h"
#include "Physics/TriggerColliderComponent.h"
#include "Scene/Transform.h"

#include <cmath>
#include <limits>

namespace Physics
{
namespace
{
bool Fail(std::string* errorMessage, const char* message)
{
    if(errorMessage)
    {
        *errorMessage = message;
    }

    return false;
}

bool Finite(const Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

// Reuse the existing pinned capsule precision checks; this creates no native resources.
bool ValidateGeometry(const CharacterControllerComponent& component, Vector3 center,
    std::string* errorMessage, Vector3 rotation = {})
{
    TriggerColliderComponent geometry;
    geometry.colliderType = ColliderType::Capsule;
    geometry.capsuleRadius = component.capsuleRadius + component.collisionClearance;
    geometry.capsuleCylinderHeight = component.capsuleCylinderHeight;
    geometry.colliderLocalPositionOffset = center;
    geometry.capsuleLocalRotation = rotation;
    return ValidateTriggerColliderComponent(geometry, errorMessage);
}
}

bool CharacterControllerComponent::operator==(const CharacterControllerComponent& other) const
{
    return capsuleRadius == other.capsuleRadius && capsuleCylinderHeight == other.capsuleCylinderHeight &&
        localOffset.x == other.localOffset.x && localOffset.y == other.localOffset.y && localOffset.z == other.localOffset.z &&
        maximumHorizontalSpeed == other.maximumHorizontalSpeed && gravityScale == other.gravityScale &&
        jumpSpeed == other.jumpSpeed && maximumWalkableSlope == other.maximumWalkableSlope &&
        groundSnapDistance == other.groundSnapDistance && collisionClearance == other.collisionClearance;
}

bool ValidateCharacterControllerComponent(const CharacterControllerComponent& component, std::string* errorMessage)
{
    if(errorMessage)
    {
        errorMessage->clear();
    }

    const float settings[]{component.capsuleRadius, component.capsuleCylinderHeight,
        component.maximumHorizontalSpeed, component.gravityScale, component.jumpSpeed,
        component.maximumWalkableSlope, component.groundSnapDistance, component.collisionClearance};

    for(float value : settings)
    {
        if(!std::isfinite(value))
        {
            return Fail(errorMessage, "Character Controller settings must be finite.");
        }
    }

    if(!Finite(component.localOffset) || component.capsuleRadius <= 0.0f || component.capsuleCylinderHeight <= 0.0f)
    {
        return Fail(errorMessage, "Character Controller requires positive capsule dimensions and a finite local offset.");
    }

    if(component.maximumHorizontalSpeed < 0.0f || component.gravityScale < 0.0f || component.jumpSpeed < 0.0f ||
        component.groundSnapDistance < 0.0f || component.collisionClearance < 0.0f ||
        component.maximumWalkableSlope < 0.0f || component.maximumWalkableSlope >= 1.57079632679f)
    {
        return Fail(errorMessage, "Character Controller speeds, gravity, snap and clearance must be nonnegative; slope is radians in [0, pi/2).");
    }

    const double totalHeight = static_cast<double>(component.capsuleCylinderHeight) + 2.0 * component.capsuleRadius;
    const float guardedRadius = component.capsuleRadius + component.collisionClearance;

    if(totalHeight > std::numeric_limits<float>::max() || !std::isfinite(guardedRadius) ||
        (component.collisionClearance > 0.0f && guardedRadius <= component.capsuleRadius))
    {
        return Fail(errorMessage, "Character Controller height or clearance is not representable in native float precision.");
    }

    // Check the actual capsule as well as its expanded query volume.
    auto unguarded = component;
    unguarded.collisionClearance = 0.0f;
    return ValidateGeometry(unguarded, component.localOffset, errorMessage) &&
        ValidateGeometry(component, component.localOffset, errorMessage);
}

bool ValidateCharacterControllerTransform(const CharacterControllerComponent& component,
    const Transform& transform, std::string* errorMessage)
{
    if(!ValidateCharacterControllerComponent(component, errorMessage))
    {
        return false;
    }

    if(!Finite(transform.position) || !Finite(transform.rotation) || !Finite(transform.scale))
    {
        return Fail(errorMessage, "Character Controller transform must be finite.");
    }

    constexpr float uprightTolerance = 1.0e-5f;

    if(std::abs(transform.rotation.x) > uprightTolerance || std::abs(transform.rotation.z) > uprightTolerance)
    {
        return Fail(errorMessage, "Character Controller requires yaw-only orientation (pitch/roll tolerance 1e-5 radians).");
    }

    // Evaluate the actual authored rotation within tolerance; never rewrite it as pure yaw.
    const Matrix4 rotation = Matrix4::Rotation(transform.rotation);
    const auto& offset = component.localOffset;
    const Vector3 rotatedOffset{
        rotation.m[0][0] * offset.x + rotation.m[0][1] * offset.y + rotation.m[0][2] * offset.z,
        rotation.m[1][0] * offset.x + rotation.m[1][1] * offset.y + rotation.m[1][2] * offset.z,
        rotation.m[2][0] * offset.x + rotation.m[2][1] * offset.y + rotation.m[2][2] * offset.z};
    const Vector3 worldCenter = transform.position + rotatedOffset;
    auto unguarded = component;
    unguarded.collisionClearance = 0.0f;
    return ValidateGeometry(unguarded, worldCenter, errorMessage, transform.rotation) &&
        ValidateGeometry(component, worldCenter, errorMessage, transform.rotation);
}
}
