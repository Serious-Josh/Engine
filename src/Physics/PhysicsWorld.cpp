#include "PhysicsWorld.h"

#include <box3d/box3d.h>
#include <box3d/constants.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Physics
{
namespace
{
bool IsFinite(Vector3 vector)
{
    return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
}

b3Vec3 ToBox3DVector(Vector3 vector)
{
    return {vector.x, vector.y, vector.z};
}

b3BodyType ToBox3DBodyType(BodyType bodyType)
{
    switch (bodyType)
    {
    case BodyType::Static:
        return b3_staticBody;
    case BodyType::Dynamic:
        return b3_dynamicBody;
    case BodyType::Kinematic:
        return b3_kinematicBody;
    default:
        throw std::invalid_argument("Unsupported physics body type");
    }
}

// Shared body and material checks. The shape name keeps messages specific, for
// example "Physics box position must be finite".
void ValidateBodyPose(const Pose& pose, BodyType bodyType, const std::string& shapeName)
{
    // Throws for an unsupported body type before any other check.
    (void)ToBox3DBodyType(bodyType);

    if (!IsFinite(pose.position))
    {
        throw std::invalid_argument("Physics " + shapeName + " position must be finite");
    }

    const Quaternion& orientation = pose.orientation;
    const float lengthSquared = orientation.x * orientation.x + orientation.y * orientation.y +
                                orientation.z * orientation.z + orientation.w * orientation.w;
    constexpr float unitQuaternionTolerance = 0.0001f;

    if (!std::isfinite(lengthSquared) || std::abs(lengthSquared - 1.0f) > unitQuaternionTolerance)
    {
        throw std::invalid_argument("Physics " + shapeName + " orientation must be a unit quaternion");
    }
}

void ValidateShapeMaterial(float density, Vector3 localPositionOffset, float friction, float restitution,
    const std::string& shapeName)
{
    if (!std::isfinite(density) || density <= 0.0f)
    {
        throw std::invalid_argument("Physics " + shapeName + " density must be finite and positive");
    }

    if (!IsFinite(localPositionOffset))
    {
        throw std::invalid_argument("Physics " + shapeName + " local offset must be finite");
    }

    if (!std::isfinite(friction) || friction < 0.0f)
    {
        throw std::invalid_argument("Physics " + shapeName + " friction must be finite and nonnegative");
    }

    if (!std::isfinite(restitution) || restitution < 0.0f || restitution > 1.0f)
    {
        throw std::invalid_argument("Physics " + shapeName + " restitution must be finite and in [0, 1]");
    }
}

void ValidateBoxDefinition(const BoxDefinition& definition)
{
    ValidateBodyPose(definition.pose, definition.bodyType, "box");

    const Vector3& halfExtents = definition.halfExtents;
    if (!IsFinite(halfExtents) || halfExtents.x <= 0.0f || halfExtents.y <= 0.0f || halfExtents.z <= 0.0f)
    {
        throw std::invalid_argument("Physics box half-extents must be finite and positive");
    }

    ValidateShapeMaterial(definition.density, definition.localPositionOffset, definition.friction,
        definition.restitution, "box");
}

void ValidateSphereDefinition(const SphereDefinition& definition)
{
    ValidateBodyPose(definition.pose, definition.bodyType, "sphere");

    if (!std::isfinite(definition.radius) || definition.radius <= 0.0f)
    {
        throw std::invalid_argument("Physics sphere radius must be finite and positive");
    }

    ValidateShapeMaterial(definition.density, definition.localPositionOffset, definition.friction,
        definition.restitution, "sphere");
}

void ValidateCapsuleDefinition(const CapsuleDefinition& definition)
{
    ValidateBodyPose(definition.pose, definition.bodyType, "capsule");

    if(!std::isfinite(definition.radius) || definition.radius <= 0.0f)
    {
        throw std::invalid_argument("Physics capsule radius must be finite and positive");
    }

    if(!std::isfinite(definition.cylinderHeight) || definition.cylinderHeight <= 0.0f)
    {
        throw std::invalid_argument("Physics capsule cylinder height must be finite and positive");
    }

    const Quaternion& localOrientation = definition.localOrientation;
    const float lengthSquared = localOrientation.x * localOrientation.x + localOrientation.y * localOrientation.y +
                                localOrientation.z * localOrientation.z + localOrientation.w * localOrientation.w;
    constexpr float unitQuaternionTolerance = 0.0001f;

    if(!std::isfinite(lengthSquared) || std::abs(lengthSquared - 1.0f) > unitQuaternionTolerance)
    {
        throw std::invalid_argument("Physics capsule local rotation must be a unit quaternion");
    }

    ValidateShapeMaterial(definition.density, definition.localPositionOffset, definition.friction,
        definition.restitution, "capsule");
}

void ValidateCreatedBoxHull(const b3BoxHull& hull, const BoxDefinition& definition)
{
    // The upstream box helper clamps small extents. Reject that change rather than
    // silently registering geometry that differs from the authored dimensions.
    const b3Vec3 actualHalfExtents = hull.base.aabb.upperBound;
    const Vector3& expectedHalfExtents = definition.halfExtents;
    if (actualHalfExtents.x != expectedHalfExtents.x || actualHalfExtents.y != expectedHalfExtents.y ||
        actualHalfExtents.z != expectedHalfExtents.z)
    {
        throw std::invalid_argument("Physics box half-extents are below Box3D's supported minimum");
    }

    const float mass = hull.base.volume * definition.density;
    if (!std::isfinite(hull.base.volume) || !std::isfinite(hull.base.surfaceArea) ||
        !b3IsValidMatrix3(hull.base.centralInertia) || !std::isfinite(mass) || mass <= 0.0f)
    {
        throw std::invalid_argument("Physics box geometry or mass exceeds the supported float range");
    }

    const b3Vec3 localOffset = ToBox3DVector(definition.localPositionOffset);
    const b3Vec3 lowerBound = b3Sub(localOffset, actualHalfExtents);
    const b3Vec3 upperBound = b3Add(localOffset, actualHalfExtents);
    if (!b3IsValidVec3(lowerBound) || !b3IsValidVec3(upperBound) ||
        lowerBound.x >= upperBound.x || lowerBound.y >= upperBound.y || lowerBound.z >= upperBound.z)
    {
        throw std::invalid_argument("Physics box local offset cannot preserve its dimensions in float precision");
    }
}

void ValidateSphereGeometry(const b3Sphere& sphere, float density)
{
    // Box3D v0.1.0 has no sphere minimum or clamp; b3CreateSphereShape copies the
    // radius unchanged. Instead reject radii whose mass or inertia cannot be
    // represented in single precision, which Box3D would otherwise treat as massless.
    const b3MassData massData = b3ComputeSphereMass(&sphere, density);

    if (!std::isfinite(massData.mass) || massData.mass <= 0.0f ||
        !b3IsValidMatrix3(massData.inertia) || massData.inertia.cx.x <= 0.0f)
    {
        throw std::invalid_argument("Physics sphere radius is outside the range whose mass and inertia "
                                    "Box3D can represent");
    }

    // The offset is the sphere's local center; its bounds must stay finite and keep
    // a nonzero width on every axis after adding the radius.
    const b3Vec3 radiusExtent{sphere.radius, sphere.radius, sphere.radius};
    const b3Vec3 lowerBound = b3Sub(sphere.center, radiusExtent);
    const b3Vec3 upperBound = b3Add(sphere.center, radiusExtent);

    if (!b3IsValidVec3(lowerBound) || !b3IsValidVec3(upperBound) ||
        lowerBound.x >= upperBound.x || lowerBound.y >= upperBound.y || lowerBound.z >= upperBound.z)
    {
        throw std::invalid_argument("Physics sphere local offset cannot preserve its radius in float precision");
    }
}

// Hemisphere centers in the body frame: localOffset -/+ Rlocal * (0, h/2, 0). The local
// rotation turns only the axis, the offset is applied exactly once, and visual scale
// never enters. Box3D rotates and translates both centers with the body. For the identity
// rotation b3RotateVector returns the axis unchanged, so unrotated centers are bit-identical
// to the P.3C.5 construction.
b3Capsule MakeCapsuleGeometry(const CapsuleDefinition& definition)
{
    const float halfCylinderHeight = definition.cylinderHeight * 0.5f;
    const Quaternion& orientation = definition.localOrientation;
    const b3Quat localRotation{{orientation.x, orientation.y, orientation.z}, orientation.w};
    const b3Vec3 halfAxis = b3RotateVector(localRotation, {0.0f, halfCylinderHeight, 0.0f});
    const b3Vec3 localOffset = ToBox3DVector(definition.localPositionOffset);

    b3Capsule capsule{};
    capsule.center1 = b3Sub(localOffset, halfAxis);
    capsule.center2 = b3Add(localOffset, halfAxis);
    capsule.radius = definition.radius;

    return capsule;
}

void ValidateCapsuleGeometry(const b3Capsule& capsule, const CapsuleDefinition& definition)
{
    // Box3D v0.1.0 does not clamp capsules, but b3CreateCapsuleShape silently creates a
    // sphere when the center distance is at or below B3_MIN_CAPSULE_LENGTH (the 5 mm
    // linear slop). Reject that case using Box3D's own test, so simulation never uses
    // a different shape from the authored Capsule.
    const float minimumLength = B3_MIN_CAPSULE_LENGTH;
    const float centerDistanceSquared = b3DistanceSquared(capsule.center1, capsule.center2);

    if(!(centerDistanceSquared > minimumLength * minimumLength))
    {
        throw std::invalid_argument("Physics capsule cylinder height must exceed Box3D's minimum capsule length "
                                    "(0.005 m) after applying the local offset");
    }

    // b3ComputeCapsuleMass normalizes the center-to-center axis and asserts if that
    // vector is not finite, so an overflowing squared distance must be rejected first.
    if(!std::isfinite(centerDistanceSquared))
    {
        throw std::invalid_argument("Physics capsule dimensions are outside the range whose mass and inertia "
                                    "Box3D can represent");
    }

    // Native capsule mass and inertia must be representable; otherwise Box3D would
    // treat the body as massless or produce nonfinite dynamics.
    const b3MassData massData = b3ComputeCapsuleMass(&capsule, definition.density);
    const b3Matrix3& inertia = massData.inertia;

    if(!std::isfinite(massData.mass) || massData.mass <= 0.0f ||
        !b3IsValidMatrix3(inertia) || inertia.cx.x <= 0.0f || inertia.cy.y <= 0.0f || inertia.cz.z <= 0.0f)
    {
        throw std::invalid_argument("Physics capsule dimensions are outside the range whose mass and inertia "
                                    "Box3D can represent");
    }

    // Engine precision rule: adding a large offset can round both centers so far that
    // the registered cylinder no longer matches the authored height. Allow ordinary
    // float rounding, but reject a deviation above one percent.
    constexpr float maximumRelativeHeightError = 0.01f;
    const float registeredHeight = std::sqrt(centerDistanceSquared);
    const float heightError = std::abs(registeredHeight - definition.cylinderHeight);

    if(!(heightError <= definition.cylinderHeight * maximumRelativeHeightError))
    {
        throw std::invalid_argument("Physics capsule local offset cannot preserve its cylinder height in float precision");
    }

    // Both hemispheres must have finite bounds that keep a nonzero width on every axis.
    const b3Vec3 radiusExtent{capsule.radius, capsule.radius, capsule.radius};

    for(const b3Vec3& center : {capsule.center1, capsule.center2})
    {
        const b3Vec3 lowerBound = b3Sub(center, radiusExtent);
        const b3Vec3 upperBound = b3Add(center, radiusExtent);

        if(!b3IsValidVec3(lowerBound) || !b3IsValidVec3(upperBound) ||
            lowerBound.x >= upperBound.x || lowerBound.y >= upperBound.y || lowerBound.z >= upperBound.z)
        {
            throw std::invalid_argument("Physics capsule local offset cannot preserve its radius in float precision");
        }
    }
}

b3Quat TriggerCapsuleOrientation(const Vector3& rotation)
{
    // Match TC.1's normalized Rz * Ry * Rx calculation so minimum-length admission
    // and native Capsule-to-Sphere fallback evaluate the same endpoints.
    const float sineX = std::sin(rotation.x * 0.5f);
    const float cosineX = std::cos(rotation.x * 0.5f);
    const float sineY = std::sin(rotation.y * 0.5f);
    const float cosineY = std::cos(rotation.y * 0.5f);
    const float sineZ = std::sin(rotation.z * 0.5f);
    const float cosineZ = std::cos(rotation.z * 0.5f);
    const b3Quat orientation{
        {sineX * cosineY * cosineZ - cosineX * sineY * sineZ,
         cosineX * sineY * cosineZ + sineX * cosineY * sineZ,
         cosineX * cosineY * sineZ - sineX * sineY * cosineZ},
        cosineX * cosineY * cosineZ + sineX * sineY * sineZ};

    return b3NormalizeQuat(orientation);
}

b3ShapeDef MakeShapeDefinition(float density, float friction, float restitution)
{
    b3ShapeDef shapeDefinition = b3DefaultShapeDef();
    shapeDefinition.density = density;
    shapeDefinition.baseMaterial.friction = friction;
    shapeDefinition.baseMaterial.restitution = restitution;

    // Begin/end touch events only; this flag does not change collision response.
    shapeDefinition.enableContactEvents = true;
    shapeDefinition.enableSensorEvents = true;

    return shapeDefinition;
}

// Map key for a shape ID. The generation is included, so a recycled shape slot can never
// be mistaken for the shape that previously used it.
std::uint64_t ShapeKey(b3ShapeId shapeId)
{
    const std::uint64_t index = static_cast<std::uint32_t>(shapeId.index1);
    const std::uint64_t world = shapeId.world0;
    const std::uint64_t generation = shapeId.generation;

    return (index << 32) | (world << 16) | generation;
}

std::uint64_t BodyKey(b3BodyId bodyId)
{
    return ShapeKey(b3ShapeId{bodyId.index1, bodyId.world0, bodyId.generation});
}
}

struct WorldState
{
    b3WorldId worldId{};

    struct RegisteredShape
    {
        b3ShapeId shapeId;
        b3BodyId bodyId;
        bool sensor;
    };

    // This registry is independent of the solid contact map. Both native index and
    // generation participate in lookup; logical keys are never reused in this world.
    std::unordered_map<std::uint64_t, std::uint64_t> shapeKeysByNativeID;
    std::unordered_map<std::uint64_t, RegisteredShape> registeredShapes;
    std::unordered_multimap<std::uint64_t, std::uint64_t> shapeKeysByBodyID;
    std::uint64_t lastShapeKey = 0;

    std::uint64_t RegisterShape(b3ShapeId shapeId, b3BodyId bodyId, bool sensor)
    {
        if(lastShapeKey == (std::numeric_limits<std::uint64_t>::max)())
        {
            throw std::overflow_error("Physics shape identity exhausted");
        }

        const auto key = ++lastShapeKey;
        registeredShapes.emplace(key, RegisteredShape{shapeId, bodyId, sensor});

        try
        {
            shapeKeysByNativeID.emplace(ShapeKey(shapeId), key);
            shapeKeysByBodyID.emplace(BodyKey(bodyId), key);
        }
        catch(...)
        {
            registeredShapes.erase(key);
            shapeKeysByNativeID.erase(ShapeKey(shapeId));
            throw;
        }

        return key;
    }

    void UnregisterShape(b3ShapeId shapeId)
    {
        const auto found = shapeKeysByNativeID.find(ShapeKey(shapeId));

        if(found != shapeKeysByNativeID.end())
        {
            const auto registered = registeredShapes.find(found->second);
            const auto bodyRange = shapeKeysByBodyID.equal_range(BodyKey(registered->second.bodyId));

            for(auto entry = bodyRange.first; entry != bodyRange.second; ++entry)
            {
                if(entry->second == found->second)
                {
                    shapeKeysByBodyID.erase(entry);
                    break;
                }
            }

            registeredShapes.erase(found->second);
            shapeKeysByNativeID.erase(found);
        }
    }

    void UnregisterBody(b3BodyId bodyId)
    {
        const auto range = shapeKeysByBodyID.equal_range(BodyKey(bodyId));

        for(auto entry = range.first; entry != range.second; ++entry)
        {
            const auto shape = registeredShapes.find(entry->second);
            shapeKeysByNativeID.erase(ShapeKey(shape->second.shapeId));
            registeredShapes.erase(shape);
        }

        shapeKeysByBodyID.erase(BodyKey(bodyId));
    }

    // Registered shapes and their bodies' contact keys. Entries are removed when a body is
    // destroyed while the world lives; keys are never reused.
    std::unordered_map<std::uint64_t, std::uint64_t> shapeContactKeys;
    std::uint64_t lastContactKey = 0;

    ~WorldState()
    {
        if (b3World_IsValid(worldId))
        {
            b3DestroyWorld(worldId);
        }
    }
};

struct Body::Impl
{
    // A body wrapper must not prolong the world's lifetime.
    std::weak_ptr<WorldState> world;
    b3BodyId bodyId{};
    b3ShapeId shapeId{};
    std::uint64_t contactKey = 0;
    std::uint64_t shapeKey = 0;

    ~Impl()
    {
        // The world already releases every body when it is destroyed.
        const auto owner = world.lock();

        if(owner)
        {
            owner->UnregisterBody(bodyId);
        }

        if (owner && b3Body_IsValid(bodyId))
        {
            // Later events naming this destroyed shape no longer map to any body.
            owner->shapeContactKeys.erase(ShapeKey(shapeId));
            b3DestroyBody(bodyId);
        }
    }

    bool IsValid() const
    {
        // Do not query a Box3D ID after its world has been destroyed.
        return !world.expired() && b3Body_IsValid(bodyId);
    }
};

struct Sensor::Impl
{
    std::weak_ptr<WorldState> world;
    b3BodyId bodyId{};
    b3ShapeId shapeId{};
    std::uint64_t shapeKey = 0;

    bool IsValid() const
    {
        return !world.expired() && b3Body_IsValid(bodyId) && b3Shape_IsValid(shapeId);
    }

    ~Impl()
    {
        const auto owner = world.lock();

        if(owner)
        {
            owner->UnregisterShape(shapeId);
        }

        if(IsValid())
        {
            // Zero-density attachment never recomputes or perturbs solid mass data.
            b3DestroyShape(shapeId, false);
        }
    }
};

Sensor::Sensor() = default;
Sensor::~Sensor() = default;
Sensor::Sensor(Sensor&&) noexcept = default;
Sensor& Sensor::operator=(Sensor&&) noexcept = default;

Sensor::Sensor(std::unique_ptr<Impl> implementation)
    : m_impl(std::move(implementation))
{
}

bool Sensor::IsValid() const
{
    return m_impl && m_impl->IsValid();
}

std::uint64_t Sensor::GetShapeKey() const
{
    if(!IsValid())
    {
        throw std::logic_error("Invalid physics sensor");
    }

    return m_impl->shapeKey;
}

Body::Body() = default;

Body::Body(std::unique_ptr<Impl> implementation)
    : m_impl(std::move(implementation))
{
}

Body::~Body() = default;
Body::Body(Body&&) noexcept = default;
Body& Body::operator=(Body&&) noexcept = default;

void Body::Reset()
{
    m_impl.reset();
}

bool Body::IsValid() const
{
    return m_impl && m_impl->IsValid();
}

Pose Body::GetPose() const
{
    if (!IsValid())
    {
        throw std::logic_error("Invalid physics body");
    }

    const b3Pos position = b3Body_GetPosition(m_impl->bodyId);
    const b3Quat orientation = b3Body_GetRotation(m_impl->bodyId);
    return {
        {position.x, position.y, position.z},
        {orientation.v.x, orientation.v.y, orientation.v.z, orientation.s}
    };
}

bool Body::IsAwake() const
{
    if (!IsValid())
    {
        throw std::logic_error("Invalid physics body");
    }

    return b3Body_IsAwake(m_impl->bodyId);
}

bool Body::HasContact() const
{
    if (!IsValid())
    {
        throw std::logic_error("Invalid physics body");
    }

    const int contactCapacity = b3Body_GetContactCapacity(m_impl->bodyId);
    if (contactCapacity == 0)
    {
        return false;
    }

    std::vector<b3ContactData> contacts(contactCapacity);
    const int touchingContactCount = b3Body_GetContactData(m_impl->bodyId, contacts.data(), contactCapacity);
    return touchingContactCount > 0;
}

BodyType Body::GetType() const
{
    if (!IsValid())
    {
        throw std::logic_error("Invalid physics body");
    }

    switch (b3Body_GetType(m_impl->bodyId))
    {
    case b3_staticBody:
        return BodyType::Static;
    case b3_dynamicBody:
        return BodyType::Dynamic;
    case b3_kinematicBody:
        return BodyType::Kinematic;
    default:
        throw std::logic_error("Unsupported registered physics body type");
    }
}

BoxProperties Body::GetBoxProperties() const
{
    if (!IsValid())
    {
        throw std::logic_error("Invalid physics body");
    }

    // Box3D asserts on a mismatched shape getter, so check the type first.
    if(!b3Shape_IsValid(m_impl->shapeId) || b3Shape_GetType(m_impl->shapeId) != b3_hullShape)
    {
        throw std::logic_error("Physics body has no box hull");
    }

    const b3HullData* hull = b3Shape_GetHull(m_impl->shapeId);

    if (!hull)
    {
        throw std::logic_error("Physics body has no box hull");
    }

    const b3Vec3 halfExtents = b3MulSV(0.5f, b3Sub(hull->aabb.upperBound, hull->aabb.lowerBound));

    return {
        {halfExtents.x, halfExtents.y, halfExtents.z},
        {hull->center.x, hull->center.y, hull->center.z},
        b3Shape_GetDensity(m_impl->shapeId),
        b3Shape_GetFriction(m_impl->shapeId),
        b3Shape_GetRestitution(m_impl->shapeId)
    };
}

SphereProperties Body::GetSphereProperties() const
{
    if (!IsValid())
    {
        throw std::logic_error("Invalid physics body");
    }

    // Box3D asserts on a mismatched shape getter, so check the type first.
    if(!b3Shape_IsValid(m_impl->shapeId) || b3Shape_GetType(m_impl->shapeId) != b3_sphereShape)
    {
        throw std::logic_error("Physics body has no sphere shape");
    }

    const b3Sphere sphere = b3Shape_GetSphere(m_impl->shapeId);

    return {
        sphere.radius,
        {sphere.center.x, sphere.center.y, sphere.center.z},
        b3Shape_GetDensity(m_impl->shapeId),
        b3Shape_GetFriction(m_impl->shapeId),
        b3Shape_GetRestitution(m_impl->shapeId)
    };
}

CapsuleProperties Body::GetCapsuleProperties() const
{
    if(!IsValid())
    {
        throw std::logic_error("Invalid physics body");
    }

    // Box3D asserts on a mismatched shape getter, so check the type first.
    if(!b3Shape_IsValid(m_impl->shapeId) || b3Shape_GetType(m_impl->shapeId) != b3_capsuleShape)
    {
        throw std::logic_error("Physics body has no capsule shape");
    }

    const b3Capsule capsule = b3Shape_GetCapsule(m_impl->shapeId);

    return {
        {capsule.center1.x, capsule.center1.y, capsule.center1.z},
        {capsule.center2.x, capsule.center2.y, capsule.center2.z},
        capsule.radius,
        b3Shape_GetDensity(m_impl->shapeId),
        b3Shape_GetFriction(m_impl->shapeId),
        b3Shape_GetRestitution(m_impl->shapeId)
    };
}

std::uint64_t Body::GetContactKey() const
{
    if(!IsValid())
    {
        throw std::logic_error("Invalid physics body");
    }

    return m_impl->contactKey;
}

std::uint64_t Body::GetShapeKey() const
{
    if(!IsValid() || m_impl->shapeKey == 0)
    {
        throw std::logic_error("Physics body has no registered solid shape");
    }

    return m_impl->shapeKey;
}

float Body::GetMass() const
{
    if (!IsValid())
    {
        throw std::logic_error("Invalid physics body");
    }
    return b3Body_GetMass(m_impl->bodyId);
}

void Body::SetKinematicPose(const Pose& pose)
{
    if(!IsValid() || b3Body_GetType(m_impl->bodyId) != b3_kinematicBody)
    {
        throw std::logic_error("Only a live Kinematic body can be posed directly");
    }

    ValidateBodyPose(pose, BodyType::Kinematic, "kinematic host");
    const Quaternion& orientation = pose.orientation;
    b3Body_SetTransform(m_impl->bodyId, b3ToPos(ToBox3DVector(pose.position)),
        {{orientation.x, orientation.y, orientation.z}, orientation.w});
}

Vector3 Body::GetWorldCenterOfMass() const
{
    if (!IsValid())
    {
        throw std::logic_error("Invalid physics body");
    }
    const b3Pos center = b3Body_GetWorldCenterOfMass(m_impl->bodyId);
    return {center.x, center.y, center.z};
}

World::World(const WorldConfig& config)
    : m_substeps(config.substeps)
{
    constexpr int maximumSubsteps = 64;
    if (!IsFinite(config.gravity) || m_substeps < 1 || m_substeps > maximumSubsteps)
    {
        throw std::invalid_argument("Invalid physics world configuration");
    }

    b3WorldDef worldDefinition = b3DefaultWorldDef();
    worldDefinition.gravity = ToBox3DVector(config.gravity);
    worldDefinition.workerCount = 1;

    auto worldState = std::make_shared<WorldState>();
    worldState->worldId = b3CreateWorld(&worldDefinition);
    if (!b3World_IsValid(worldState->worldId))
    {
        throw std::runtime_error("Box3D world creation failed");
    }
    m_state = std::move(worldState);
}

World::~World() = default;

void World::SetGravity(Vector3 gravity)
{
    if (!IsFinite(gravity))
    {
        throw std::invalid_argument("Invalid gravity");
    }

    b3World_SetGravity(m_state->worldId, ToBox3DVector(gravity));
}

void World::Step()
{
    b3World_Step(m_state->worldId, FixedTimeStep, m_substeps);
}

std::vector<ContactTouchEvent> World::CollectContactTouchEvents() const
{
    // Read after b3World_Step returns: Box3D fills these arrays during the step and clears
    // them at the start of the next one.
    const b3ContactEvents nativeEvents = b3World_GetContactEvents(m_state->worldId);
    const auto& shapeContactKeys = m_state->shapeContactKeys;
    std::vector<ContactTouchEvent> touchEvents;
    touchEvents.reserve(static_cast<std::size_t>(nativeEvents.beginCount) + static_cast<std::size_t>(nativeEvents.endCount));

    const auto appendEvent = [&](b3ShapeId shapeIdA, b3ShapeId shapeIdB, bool began)
    {
        const auto bodyA = shapeContactKeys.find(ShapeKey(shapeIdA));
        const auto bodyB = shapeContactKeys.find(ShapeKey(shapeIdB));

        if(bodyA == shapeContactKeys.end() || bodyB == shapeContactKeys.end())
        {
            return;
        }

        touchEvents.push_back({began, bodyA->second, bodyB->second});
    };

    for(int eventIndex = 0; eventIndex < nativeEvents.beginCount; ++eventIndex)
    {
        const b3ContactBeginTouchEvent& beginEvent = nativeEvents.beginEvents[eventIndex];
        appendEvent(beginEvent.shapeIdA, beginEvent.shapeIdB, true);
    }

    // End events may name destroyed shapes; the key map, not the native ID, decides validity.
    for(int eventIndex = 0; eventIndex < nativeEvents.endCount; ++eventIndex)
    {
        const b3ContactEndTouchEvent& endEvent = nativeEvents.endEvents[eventIndex];
        appendEvent(endEvent.shapeIdA, endEvent.shapeIdB, false);
    }

    return touchEvents;
}

Body World::PublishBody(std::unique_ptr<Body::Impl> bodyImplementation)
{
    bodyImplementation->contactKey = ++m_state->lastContactKey;
    m_state->shapeContactKeys[ShapeKey(bodyImplementation->shapeId)] = bodyImplementation->contactKey;
    bodyImplementation->shapeKey = m_state->RegisterShape(bodyImplementation->shapeId, bodyImplementation->bodyId, false);

    return Body(std::move(bodyImplementation));
}

std::unique_ptr<Body::Impl> World::CreateBodyImplementation(const Pose& pose, BodyType bodyType)
{
    auto bodyImplementation = std::make_unique<Body::Impl>();
    bodyImplementation->world = m_state;

    const Quaternion& orientation = pose.orientation;
    b3BodyDef bodyDefinition = b3DefaultBodyDef();
    bodyDefinition.type = ToBox3DBodyType(bodyType);
    bodyDefinition.position = ToBox3DVector(pose.position);
    bodyDefinition.rotation = {{orientation.x, orientation.y, orientation.z}, orientation.w};

    bodyImplementation->bodyId = b3CreateBody(m_state->worldId, &bodyDefinition);

    if (!b3Body_IsValid(bodyImplementation->bodyId))
    {
        throw std::runtime_error("Box3D body creation failed");
    }

    return bodyImplementation;
}

Body World::CreateBox(const BoxDefinition& definition)
{
    // Validation precedes all Box3D work. After the body exists, any later throw
    // destroys it through Body::Impl, so a failed creation leaves nothing behind.
    ValidateBoxDefinition(definition);

    auto bodyImplementation = CreateBodyImplementation(definition.pose, definition.bodyType);

    // Geometry: authored half-extents, rejected if Box3D's helper had to clamp them.
    const Vector3& halfExtents = definition.halfExtents;
    const b3BoxHull boxHull = b3MakeBoxHull(halfExtents.x, halfExtents.y, halfExtents.z);
    ValidateCreatedBoxHull(boxHull, definition);

    const b3ShapeDef shapeDefinition = MakeShapeDefinition(definition.density, definition.friction,
        definition.restitution);

    // Box3D copies the hull data; this stack hull does not need separate ownership.
    const b3Transform localTransform{ToBox3DVector(definition.localPositionOffset), b3Quat_identity};
    bodyImplementation->shapeId = b3CreateTransformedHullShape(bodyImplementation->bodyId, &shapeDefinition,
        &boxHull.base, localTransform, {1.0f, 1.0f, 1.0f});

    if (!b3Shape_IsValid(bodyImplementation->shapeId))
    {
        throw std::runtime_error("Box3D box shape creation failed");
    }

    // Ownership passes to the returned wrapper only after every check has succeeded.
    return PublishBody(std::move(bodyImplementation));
}

Body World::CreateSphere(const SphereDefinition& definition)
{
    // Unlike Box, sphere geometry is validated before the body exists, so most
    // failures create no Box3D resources at all.
    ValidateSphereDefinition(definition);

    // The local offset is the sphere center in the body frame, matching the Box hull
    // offset: Box3D rotates and translates it with the body. The body origin stays at
    // the authored object origin, and no visual scale is applied to radius or center.
    const b3Sphere sphere{ToBox3DVector(definition.localPositionOffset), definition.radius};
    ValidateSphereGeometry(sphere, definition.density);

    auto bodyImplementation = CreateBodyImplementation(definition.pose, definition.bodyType);

    const b3ShapeDef shapeDefinition = MakeShapeDefinition(definition.density, definition.friction,
        definition.restitution);

    // Box3D clones the sphere geometry; this stack value does not need separate ownership.
    bodyImplementation->shapeId = b3CreateSphereShape(bodyImplementation->bodyId, &shapeDefinition, &sphere);

    if (!b3Shape_IsValid(bodyImplementation->shapeId))
    {
        throw std::runtime_error("Box3D sphere shape creation failed");
    }

    // Ownership passes to the returned wrapper only after every check has succeeded.
    return PublishBody(std::move(bodyImplementation));
}
Body World::CreateCapsule(const CapsuleDefinition& definition)
{
    // As for Sphere, geometry is validated before the body exists, so most failures
    // create no Box3D resources at all.
    ValidateCapsuleDefinition(definition);

    const b3Capsule capsule = MakeCapsuleGeometry(definition);
    ValidateCapsuleGeometry(capsule, definition);

    auto bodyImplementation = CreateBodyImplementation(definition.pose, definition.bodyType);

    const b3ShapeDef shapeDefinition = MakeShapeDefinition(definition.density, definition.friction,
        definition.restitution);

    // Box3D clones the capsule geometry; this stack value does not need separate ownership.
    bodyImplementation->shapeId = b3CreateCapsuleShape(bodyImplementation->bodyId, &shapeDefinition, &capsule);

    if(!b3Shape_IsValid(bodyImplementation->shapeId))
    {
        throw std::runtime_error("Box3D capsule shape creation failed");
    }

    // Ownership passes to the returned wrapper only after every check has succeeded.
    return PublishBody(std::move(bodyImplementation));
}

Body World::CreateSensorBody(const Pose& pose, BodyType bodyType)
{
    if(bodyType == BodyType::Dynamic)
    {
        throw std::invalid_argument("Sensor hosts are Static or Kinematic");
    }

    ValidateBodyPose(pose, bodyType, "sensor host");
    return Body(CreateBodyImplementation(pose, bodyType));
}

Sensor World::AttachSensor(Body& body, const TriggerColliderComponent& component)
{
    std::string error;

    if(!ValidateTriggerColliderComponent(component, &error))
    {
        throw std::invalid_argument(error);
    }

    if(!body.IsValid() || body.m_impl->world.lock() != m_state)
    {
        throw std::invalid_argument("Trigger sensor requires a live body in this world");
    }

    auto implementation = std::make_unique<Sensor::Impl>();
    implementation->world = m_state;
    implementation->bodyId = body.m_impl->bodyId;
    b3ShapeDef definition = b3DefaultShapeDef();
    definition.isSensor = true;
    definition.enableSensorEvents = true;
    definition.enableContactEvents = false;
    definition.density = 0.0f;
    definition.updateBodyMass = false;
    const b3Vec3 offset = ToBox3DVector(component.colliderLocalPositionOffset);

    switch(component.colliderType)
    {
    case ColliderType::Box:
    {
        const auto& dimensions = component.boxDimensions;
        const b3BoxHull hull = b3MakeBoxHull(dimensions.width * 0.5f,
            dimensions.height * 0.5f, dimensions.depth * 0.5f);
        implementation->shapeId = b3CreateTransformedHullShape(implementation->bodyId, &definition,
            &hull.base, {offset, b3Quat_identity}, {1.0f, 1.0f, 1.0f});
        break;
    }
    case ColliderType::Sphere:
    {
        const b3Sphere sphere{offset, component.sphereRadius};
        implementation->shapeId = b3CreateSphereShape(implementation->bodyId, &definition, &sphere);
        break;
    }
    case ColliderType::Capsule:
    {
        const b3Quat orientation = TriggerCapsuleOrientation(component.capsuleLocalRotation);
        const b3Vec3 halfAxis = b3RotateVector(orientation, {0.0f, component.capsuleCylinderHeight * 0.5f, 0.0f});
        const b3Capsule capsule{b3Sub(offset, halfAxis), b3Add(offset, halfAxis), component.capsuleRadius};
        implementation->shapeId = b3CreateCapsuleShape(implementation->bodyId, &definition, &capsule);
        break;
    }
    default:
        throw std::invalid_argument("Unsupported trigger collider type");
    }

    if(!implementation->IsValid())
    {
        throw std::runtime_error("Box3D trigger sensor creation failed");
    }

    implementation->shapeKey = m_state->RegisterShape(implementation->shapeId, implementation->bodyId, true);
    return Sensor(std::move(implementation));
}

bool World::IsShapeKeyValid(std::uint64_t shapeKey) const
{
    const auto found = m_state->registeredShapes.find(shapeKey);
    return found != m_state->registeredShapes.end() && b3Shape_IsValid(found->second.shapeId);
}

std::vector<SensorTouchEvent> World::CollectSensorTouchEvents() const
{
    const auto nativeEvents = b3World_GetSensorEvents(m_state->worldId);
    std::vector<SensorTouchEvent> observations;
    const auto append = [&](b3ShapeId sensorId, b3ShapeId visitorId, bool began)
    {
        const auto sensorKey = m_state->shapeKeysByNativeID.find(ShapeKey(sensorId));
        const auto visitorKey = m_state->shapeKeysByNativeID.find(ShapeKey(visitorId));

        if(sensorKey == m_state->shapeKeysByNativeID.end() || visitorKey == m_state->shapeKeysByNativeID.end())
        {
            return;
        }

        const auto& sensor = m_state->registeredShapes.at(sensorKey->second);
        const auto& visitor = m_state->registeredShapes.at(visitorKey->second);

        // End IDs can name destroyed resources. Only validity tests are permitted;
        // identity/role comes from captured metadata, never a native shape getter.
        if(!sensor.sensor || !IsShapeKeyValid(sensorKey->second) || !IsShapeKeyValid(visitorKey->second) ||
            (sensor.bodyId.index1 == visitor.bodyId.index1 && sensor.bodyId.generation == visitor.bodyId.generation))
        {
            return;
        }

        observations.push_back({began, sensorKey->second, visitorKey->second});

        if(visitor.sensor)
        {
            observations.push_back({began, visitorKey->second, sensorKey->second});
        }
    };

    // Arrays belong to Box3D and expire at its next step. Return copied keys only.
    for(int index = 0; index < nativeEvents.beginCount; ++index)
    {
        const auto& event = nativeEvents.beginEvents[index];
        append(event.sensorShapeId, event.visitorShapeId, true);
    }

    for(int index = 0; index < nativeEvents.endCount; ++index)
    {
        const auto& event = nativeEvents.endEvents[index];
        append(event.sensorShapeId, event.visitorShapeId, false);
    }

    return observations;
}
}

namespace Physics
{
namespace
{
b3Capsule ToBox3DCapsule(const MoverCapsule& capsule)
{
    return {ToBox3DVector(capsule.lowerCenter), ToBox3DVector(capsule.upperCenter), capsule.radius};
}

void ValidateMoverQuery(Vector3 origin, const MoverCapsule& capsule)
{
    if(!IsFinite(origin) || !IsFinite(capsule.lowerCenter) || !IsFinite(capsule.upperCenter) ||
        !std::isfinite(capsule.radius) || capsule.radius <= 0.0f)
    {
        throw std::invalid_argument("Mover query requires a finite origin and capsule with a positive radius");
    }
}

// Shared by overlap, plane and cast queries so their solid eligibility cannot diverge.
bool IsSolidMoverObstacle(b3ShapeId shapeId)
{
    return !b3Shape_IsSensor(shapeId);
}

struct SolidOverlapQuery
{
    const WorldState* state = nullptr;
    std::size_t count = 0;
    std::uint64_t firstShapeKey = 0;
    std::vector<std::uint64_t> shapeKeys;
};

bool CountSolidOverlap(b3ShapeId shapeId, void* context)
{
    auto& query = *static_cast<SolidOverlapQuery*>(context);

    if(!IsSolidMoverObstacle(shapeId))
    {
        return true;
    }

    const auto found = query.state->shapeKeysByNativeID.find(ShapeKey(shapeId));
    const std::uint64_t shapeKey = found == query.state->shapeKeysByNativeID.end() ? 0 : found->second;

    if(query.count == 0 || shapeKey < query.firstShapeKey)
    {
        query.firstShapeKey = shapeKey;
    }

    query.shapeKeys.push_back(shapeKey);
    ++query.count;
    return true;
}

struct SolidPlaneQuery
{
    const WorldState* state = nullptr;
    std::size_t capacity = 0;
    MoverPlaneQuery result;
};

bool CollectSolidPlanes(b3ShapeId shapeId, const b3PlaneResult* planes, int planeCount, void* context)
{
    auto& query = *static_cast<SolidPlaneQuery*>(context);

    // Returning false would stop traversal, so ignored shapes return true without planes.
    if(!IsSolidMoverObstacle(shapeId))
    {
        return true;
    }

    const auto found = query.state->shapeKeysByNativeID.find(ShapeKey(shapeId));
    const std::uint64_t shapeKey = found == query.state->shapeKeysByNativeID.end() ? 0 : found->second;

    for(int index = 0; index < planeCount; ++index)
    {
        if(query.result.planes.size() == query.capacity)
        {
            ++query.result.overflowCount;
            continue;
        }

        const auto& plane = planes[index];
        query.result.planes.push_back({shapeKey,
            {plane.plane.normal.x, plane.plane.normal.y, plane.plane.normal.z}, plane.plane.offset,
            {plane.point.x, plane.point.y, plane.point.z}});
    }

    return true;
}

bool IsSolidMoverCastCandidate(b3ShapeId shapeId, void*)
{
    return IsSolidMoverObstacle(shapeId);
}
}

std::size_t World::CountSolidOverlaps(Vector3 origin, const MoverCapsule& capsule, std::uint64_t* firstShapeKey) const
{
    ValidateMoverQuery(origin, capsule);

    const b3Vec3 points[]{ToBox3DVector(capsule.lowerCenter), ToBox3DVector(capsule.upperCenter)};
    const b3ShapeProxy proxy{points, 2, capsule.radius};
    SolidOverlapQuery query{m_state.get()};
    b3World_OverlapShape(m_state->worldId, b3ToPos(ToBox3DVector(origin)), &proxy, b3DefaultQueryFilter(),
        CountSolidOverlap, &query);

    if(firstShapeKey)
    {
        *firstShapeKey = query.firstShapeKey;
    }

    return query.count;
}

std::vector<std::uint64_t> World::CollectSolidOverlapKeys(Vector3 origin, const MoverCapsule& capsule) const
{
    ValidateMoverQuery(origin, capsule);

    const b3Vec3 points[]{ToBox3DVector(capsule.lowerCenter), ToBox3DVector(capsule.upperCenter)};
    const b3ShapeProxy proxy{points, 2, capsule.radius};
    SolidOverlapQuery query{m_state.get()};
    b3World_OverlapShape(m_state->worldId, b3ToPos(ToBox3DVector(origin)), &proxy, b3DefaultQueryFilter(),
        CountSolidOverlap, &query);

    std::sort(query.shapeKeys.begin(), query.shapeKeys.end());
    query.shapeKeys.erase(std::unique(query.shapeKeys.begin(), query.shapeKeys.end()), query.shapeKeys.end());
    return std::move(query.shapeKeys);
}

MoverPlaneQuery World::CollideSolidMover(Vector3 origin, const MoverCapsule& capsule, std::size_t capacity) const
{
    ValidateMoverQuery(origin, capsule);

    const b3Capsule mover = ToBox3DCapsule(capsule);
    SolidPlaneQuery query{m_state.get(), capacity};
    query.result.planes.reserve(capacity);
    b3World_CollideMover(m_state->worldId, b3ToPos(ToBox3DVector(origin)), &mover, b3DefaultQueryFilter(),
        CollectSolidPlanes, &query);

    // Native tree traversal order is not a contract; publish a deterministic order.
    std::sort(query.result.planes.begin(), query.result.planes.end(), [](const MoverPlane& left, const MoverPlane& right)
    {
        return std::tuple(left.shapeKey, left.normal.x, left.normal.y, left.normal.z, left.offset) <
            std::tuple(right.shapeKey, right.normal.x, right.normal.y, right.normal.z, right.offset);
    });

    return std::move(query.result);
}

float World::CastSolidMover(Vector3 origin, const MoverCapsule& capsule, Vector3 translation) const
{
    ValidateMoverQuery(origin, capsule);

    if(!IsFinite(translation))
    {
        throw std::invalid_argument("Mover cast translation must be finite");
    }

    const b3Capsule mover = ToBox3DCapsule(capsule);
    const float fraction = b3World_CastMover(m_state->worldId, b3ToPos(ToBox3DVector(origin)), &mover,
        ToBox3DVector(translation), b3DefaultQueryFilter(), IsSolidMoverCastCandidate, nullptr);

    if(!std::isfinite(fraction) || fraction < 0.0f || fraction > 1.0f)
    {
        throw std::runtime_error("Mover cast returned an invalid fraction");
    }

    return fraction;
}
}
