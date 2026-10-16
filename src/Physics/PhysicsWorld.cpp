#include "PhysicsWorld.h"

#include <box3d/box3d.h>

#include <cmath>
#include <stdexcept>
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

void ValidateBoxDefinition(const BoxDefinition& definition)
{
    (void)ToBox3DBodyType(definition.bodyType);
    if (!IsFinite(definition.pose.position))
    {
        throw std::invalid_argument("Physics box position must be finite");
    }

    const Vector3& halfExtents = definition.halfExtents;
    if (!IsFinite(halfExtents) || halfExtents.x <= 0.0f || halfExtents.y <= 0.0f || halfExtents.z <= 0.0f)
    {
        throw std::invalid_argument("Physics box half-extents must be finite and positive");
    }

    const Quaternion& orientation = definition.pose.orientation;
    const float lengthSquared = orientation.x * orientation.x + orientation.y * orientation.y +
                                orientation.z * orientation.z + orientation.w * orientation.w;
    constexpr float unitQuaternionTolerance = 0.0001f;
    if (!std::isfinite(lengthSquared) || std::abs(lengthSquared - 1.0f) > unitQuaternionTolerance)
    {
        throw std::invalid_argument("Physics box orientation must be a unit quaternion");
    }

    if (!std::isfinite(definition.density) || definition.density <= 0.0f)
    {
        throw std::invalid_argument("Physics box density must be finite and positive");
    }

    if (!IsFinite(definition.localPositionOffset))
    {
        throw std::invalid_argument("Physics box local offset must be finite");
    }
    if (!std::isfinite(definition.friction) || definition.friction < 0.0f)
    {
        throw std::invalid_argument("Physics box friction must be finite and nonnegative");
    }
    if (!std::isfinite(definition.restitution) || definition.restitution < 0.0f || definition.restitution > 1.0f)
    {
        throw std::invalid_argument("Physics box restitution must be finite and in [0, 1]");
    }
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
}

struct WorldState
{
    b3WorldId worldId{};

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

    ~Impl()
    {
        // The world already releases every body when it is destroyed.
        const auto owner = world.lock();
        if (owner && b3Body_IsValid(bodyId))
        {
            b3DestroyBody(bodyId);
        }
    }

    bool IsValid() const
    {
        // Do not query a Box3D ID after its world has been destroyed.
        return !world.expired() && b3Body_IsValid(bodyId);
    }
};

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

float Body::GetMass() const
{
    if (!IsValid())
    {
        throw std::logic_error("Invalid physics body");
    }
    return b3Body_GetMass(m_impl->bodyId);
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

Body World::CreateBox(const BoxDefinition& definition)
{
    ValidateBoxDefinition(definition);

    auto bodyImplementation = std::make_unique<Body::Impl>();
    bodyImplementation->world = m_state;

    const Quaternion& orientation = definition.pose.orientation;
    b3BodyDef bodyDefinition = b3DefaultBodyDef();
    bodyDefinition.type = ToBox3DBodyType(definition.bodyType);
    bodyDefinition.position = ToBox3DVector(definition.pose.position);
    bodyDefinition.rotation = {{orientation.x, orientation.y, orientation.z}, orientation.w};
    bodyImplementation->bodyId = b3CreateBody(m_state->worldId, &bodyDefinition);
    if (!b3Body_IsValid(bodyImplementation->bodyId))
    {
        throw std::runtime_error("Box3D body creation failed");
    }

    const Vector3& halfExtents = definition.halfExtents;
    const b3BoxHull boxHull = b3MakeBoxHull(halfExtents.x, halfExtents.y, halfExtents.z);
    ValidateCreatedBoxHull(boxHull, definition);
    b3ShapeDef shapeDefinition = b3DefaultShapeDef();
    shapeDefinition.density = definition.density;
    shapeDefinition.baseMaterial.friction = definition.friction;
    shapeDefinition.baseMaterial.restitution = definition.restitution;

    // Box3D copies the hull data; this stack hull does not need separate ownership.
    const b3Transform localTransform{ToBox3DVector(definition.localPositionOffset), b3Quat_identity};
    bodyImplementation->shapeId = b3CreateTransformedHullShape(bodyImplementation->bodyId, &shapeDefinition,
        &boxHull.base, localTransform, {1.0f, 1.0f, 1.0f});
    if (!b3Shape_IsValid(bodyImplementation->shapeId))
    {
        throw std::runtime_error("Box3D box shape creation failed");
    }
    return Body(std::move(bodyImplementation));
}
}
