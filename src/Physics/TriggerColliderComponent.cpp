#include "Physics/TriggerColliderComponent.h"

#include <box3d/box3d.h>
#include <box3d/constants.h>

#include <cmath>

namespace Physics
{
namespace
{
bool SameVector(const Vector3& left, const Vector3& right)
{
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

bool IsFinite(const Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool Fail(std::string* errorMessage, const char* message)
{
    if(errorMessage)
    {
        *errorMessage = message;
    }

    return false;
}

bool ValidBounds(b3Vec3 center, b3Vec3 extent)
{
    const b3Vec3 lower = b3Sub(center, extent);
    const b3Vec3 upper = b3Add(center, extent);

    // Overlap calculations square distances. Finite endpoints alone do not
    // guarantee representable arithmetic, or preserve geometry at a large offset.
    return b3IsValidVec3(lower) && b3IsValidVec3(upper) &&
        lower.x < upper.x && lower.y < upper.y && lower.z < upper.z &&
        std::isfinite(b3LengthSquared(lower)) && std::isfinite(b3LengthSquared(upper)) &&
        std::isfinite(b3DistanceSquared(lower, upper));
}

b3Quat CapsuleOrientation(const Vector3& rotation)
{
    const float sineX = std::sin(rotation.x * 0.5f);
    const float cosineX = std::cos(rotation.x * 0.5f);
    const float sineY = std::sin(rotation.y * 0.5f);
    const float cosineY = std::cos(rotation.y * 0.5f);
    const float sineZ = std::sin(rotation.z * 0.5f);
    const float cosineZ = std::cos(rotation.z * 0.5f);
    b3Quat orientation{
        {sineX * cosineY * cosineZ - cosineX * sineY * sineZ,
         cosineX * sineY * cosineZ + sineX * cosineY * sineZ,
         cosineX * cosineY * sineZ - sineX * sineY * cosineZ},
        cosineX * cosineY * cosineZ + sineX * sineY * sineZ};

    return b3NormalizeQuat(orientation);
}
}

bool TriggerColliderComponent::operator==(const TriggerColliderComponent& other) const
{
    return colliderType == other.colliderType && boxDimensions == other.boxDimensions &&
        sphereRadius == other.sphereRadius && capsuleRadius == other.capsuleRadius &&
        capsuleCylinderHeight == other.capsuleCylinderHeight &&
        SameVector(colliderLocalPositionOffset, other.colliderLocalPositionOffset) &&
        SameVector(capsuleLocalRotation, other.capsuleLocalRotation);
}

bool ValidateTriggerColliderComponent(const TriggerColliderComponent& component, std::string* errorMessage)
{
    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(component.colliderType != ColliderType::Box && component.colliderType != ColliderType::Sphere &&
        component.colliderType != ColliderType::Capsule)
    {
        return Fail(errorMessage, "Unsupported trigger collider type.");
    }

    const auto& dimensions = component.boxDimensions;

    // Inactive values persist across shape switches, as for PhysicsComponent.
    if(!std::isfinite(dimensions.width) || !std::isfinite(dimensions.height) ||
        !std::isfinite(dimensions.depth) || !std::isfinite(component.sphereRadius) ||
        !std::isfinite(component.capsuleRadius) || !std::isfinite(component.capsuleCylinderHeight) ||
        !IsFinite(component.colliderLocalPositionOffset) || !IsFinite(component.capsuleLocalRotation))
    {
        return Fail(errorMessage, "Stored trigger dimensions, radii, offset and rotation must be finite.");
    }

    const auto& offset = component.colliderLocalPositionOffset;
    const b3Vec3 center{offset.x, offset.y, offset.z};

    if(component.colliderType == ColliderType::Box)
    {
        const b3Vec3 halfExtents{dimensions.width * 0.5f, dimensions.height * 0.5f, dimensions.depth * 0.5f};

        if(halfExtents.x <= 0.0f || halfExtents.y <= 0.0f || halfExtents.z <= 0.0f)
        {
            return Fail(errorMessage, "Trigger box dimensions must be positive, representable full dimensions (meters).");
        }

        // This computes stack geometry only; it creates no world, body or sensor.
        const b3BoxHull hull = b3MakeBoxHull(halfExtents.x, halfExtents.y, halfExtents.z);

        if(hull.base.aabb.upperBound.x != halfExtents.x || hull.base.aabb.upperBound.y != halfExtents.y ||
            hull.base.aabb.upperBound.z != halfExtents.z)
        {
            return Fail(errorMessage, "Trigger box dimensions are below Box3D's supported minimum.");
        }

        if(!std::isfinite(hull.base.volume) || !std::isfinite(hull.base.surfaceArea) ||
            !b3IsValidMatrix3(hull.base.centralInertia) || !ValidBounds(center, halfExtents))
        {
            return Fail(errorMessage, "Trigger box geometry or offset exceeds float range or loses dimension precision.");
        }

        return true;
    }

    const float radius = component.colliderType == ColliderType::Sphere ?
        component.sphereRadius : component.capsuleRadius;
    const float radiusSquared = radius * radius;

    if(radius <= 0.0f || radiusSquared <= 0.0f || !std::isfinite(radiusSquared))
    {
        return Fail(errorMessage, "Active trigger radius must be positive with a representable squared radius (meters).");
    }

    const b3Vec3 radiusExtent{radius, radius, radius};

    if(component.colliderType == ColliderType::Sphere)
    {
        if(!ValidBounds(center, radiusExtent))
        {
            return Fail(errorMessage, "Trigger sphere offset cannot preserve finite radius bounds in float precision.");
        }

        return true;
    }

    const float height = component.capsuleCylinderHeight;

    if(height <= 0.0f)
    {
        return Fail(errorMessage, "Trigger capsule cylinder height must be positive (meters, excluding caps).");
    }

    const b3Vec3 halfAxis = b3RotateVector(CapsuleOrientation(component.capsuleLocalRotation),
        {0.0f, height * 0.5f, 0.0f});
    const b3Vec3 lowerCenter = b3Sub(center, halfAxis);
    const b3Vec3 upperCenter = b3Add(center, halfAxis);
    const float distanceSquared = b3DistanceSquared(lowerCenter, upperCenter);

    if(!std::isfinite(distanceSquared) || !(distanceSquared > B3_MIN_CAPSULE_LENGTH * B3_MIN_CAPSULE_LENGTH))
    {
        return Fail(errorMessage, "Trigger capsule center distance must be finite and exceed Box3D's minimum (0.005 m).");
    }

    if(std::abs(std::sqrt(distanceSquared) - height) > height * 0.01f ||
        !ValidBounds(lowerCenter, radiusExtent) || !ValidBounds(upperCenter, radiusExtent))
    {
        return Fail(errorMessage, "Trigger capsule offset/rotation loses height precision (over 1%) or finite radius bounds.");
    }

    return true;
}
}
