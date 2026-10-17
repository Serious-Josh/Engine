#include "Physics/PhysicsComponent.h"

#include <cmath>

namespace Physics
{
namespace
{
bool Fail(std::string* errorMessage, const char* message)
{
    if (errorMessage)
    {
        *errorMessage = message;
    }
    return false;
}

bool IsPositiveFinite(float value)
{
    return std::isfinite(value) && value > 0.0f;
}
}

bool PhysicsComponent::operator==(const PhysicsComponent& other) const
{
    return bodyType == other.bodyType && colliderType == other.colliderType &&
           boxDimensions == other.boxDimensions && sphereRadius == other.sphereRadius &&
           capsuleRadius == other.capsuleRadius && capsuleCylinderHeight == other.capsuleCylinderHeight &&
           capsuleLocalRotation.x == other.capsuleLocalRotation.x &&
           capsuleLocalRotation.y == other.capsuleLocalRotation.y &&
           capsuleLocalRotation.z == other.capsuleLocalRotation.z &&
           colliderLocalPositionOffset.x == other.colliderLocalPositionOffset.x &&
           colliderLocalPositionOffset.y == other.colliderLocalPositionOffset.y &&
           colliderLocalPositionOffset.z == other.colliderLocalPositionOffset.z &&
           density == other.density && friction == other.friction && restitution == other.restitution &&
           generateContactEvents == other.generateContactEvents;
}

bool ValidatePhysicsComponent(const PhysicsComponent& component, std::string* errorMessage)
{
    if (errorMessage)
    {
        errorMessage->clear();
    }

    switch (component.bodyType)
    {
    case BodyType::Static:
    case BodyType::Dynamic:
    case BodyType::Kinematic:
        break;
    default:
        return Fail(errorMessage, "Unsupported physics body type.");
    }

    const bool knownColliderType = component.colliderType == ColliderType::Box ||
        component.colliderType == ColliderType::Sphere ||
        component.colliderType == ColliderType::Capsule;

    if(!knownColliderType)
    {
        return Fail(errorMessage, "Unsupported physics collider type.");
    }

    // Every stored float must round-trip through JSON and the strict binary decoder.
    // Inactive dimensions may be nonpositive, but never nonfinite.
    if (!std::isfinite(component.boxDimensions.width) || !std::isfinite(component.boxDimensions.height) ||
        !std::isfinite(component.boxDimensions.depth) || !std::isfinite(component.sphereRadius))
    {
        return Fail(errorMessage, "Stored physics shape dimensions and sphere radius must be finite (meters).");
    }

    if(!std::isfinite(component.capsuleRadius) || !std::isfinite(component.capsuleCylinderHeight))
    {
        return Fail(errorMessage, "Stored physics capsule radius and cylinder height must be finite (meters).");
    }

    const Vector3& capsuleRotation = component.capsuleLocalRotation;

    if(!std::isfinite(capsuleRotation.x) || !std::isfinite(capsuleRotation.y) || !std::isfinite(capsuleRotation.z))
    {
        return Fail(errorMessage, "Stored physics capsule local rotation must be finite (radians).");
    }

    if (component.colliderType == ColliderType::Sphere && !IsPositiveFinite(component.sphereRadius))
    {
        return Fail(errorMessage, "Physics sphere radius must be finite and positive (meters).");
    }

    if(component.colliderType == ColliderType::Capsule)
    {
        if(!IsPositiveFinite(component.capsuleRadius))
        {
            return Fail(errorMessage, "Physics capsule radius must be finite and positive (meters).");
        }

        if(!IsPositiveFinite(component.capsuleCylinderHeight))
        {
            return Fail(errorMessage, "Physics capsule cylinder height must be finite and positive (meters).");
        }
    }

    if (component.colliderType == ColliderType::Box)
    {
        if (!IsPositiveFinite(component.boxDimensions.width))
        {
            return Fail(errorMessage, "Physics box width must be finite and positive (meters).");
        }
        if (!IsPositiveFinite(component.boxDimensions.height))
        {
            return Fail(errorMessage, "Physics box height must be finite and positive (meters).");
        }
        if (!IsPositiveFinite(component.boxDimensions.depth))
        {
            return Fail(errorMessage, "Physics box depth must be finite and positive (meters).");
        }
    }

    const Vector3& localOffset = component.colliderLocalPositionOffset;
    if (!std::isfinite(localOffset.x) || !std::isfinite(localOffset.y) || !std::isfinite(localOffset.z))
    {
        return Fail(errorMessage, "Physics collider local position offset must be finite (meters).");
    }
    if (!IsPositiveFinite(component.density))
    {
        return Fail(errorMessage, "Physics density must be finite and positive (kg/m^3).");
    }
    if (!std::isfinite(component.friction) || component.friction < 0.0f)
    {
        return Fail(errorMessage, "Physics friction must be finite and nonnegative.");
    }
    if (!std::isfinite(component.restitution) || component.restitution < 0.0f || component.restitution > 1.0f)
    {
        return Fail(errorMessage, "Physics restitution must be finite and in [0, 1].");
    }

    return true;
}
}
