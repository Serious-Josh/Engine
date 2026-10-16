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
           boxDimensions == other.boxDimensions &&
           colliderLocalPositionOffset.x == other.colliderLocalPositionOffset.x &&
           colliderLocalPositionOffset.y == other.colliderLocalPositionOffset.y &&
           colliderLocalPositionOffset.z == other.colliderLocalPositionOffset.z &&
           density == other.density && friction == other.friction && restitution == other.restitution;
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

    if (component.colliderType != ColliderType::Box)
    {
        return Fail(errorMessage, "Unsupported physics collider type.");
    }

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
