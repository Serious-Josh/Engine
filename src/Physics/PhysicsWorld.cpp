#include "PhysicsWorld.h"

#include <box3d/box3d.h>
#include <box3d/constants.h>

#include <cmath>
#include <stdexcept>
#include <string>
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

b3ShapeDef MakeShapeDefinition(float density, float friction, float restitution)
{
    b3ShapeDef shapeDefinition = b3DefaultShapeDef();
    shapeDefinition.density = density;
    shapeDefinition.baseMaterial.friction = friction;
    shapeDefinition.baseMaterial.restitution = restitution;

    // Begin/end touch events only; this flag does not change collision response.
    shapeDefinition.enableContactEvents = true;

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
}

struct WorldState
{
    b3WorldId worldId{};

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

    ~Impl()
    {
        // The world already releases every body when it is destroyed.
        const auto owner = world.lock();
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
    if (b3Shape_GetType(m_impl->shapeId) != b3_hullShape)
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
    if (b3Shape_GetType(m_impl->shapeId) != b3_sphereShape)
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
    if(b3Shape_GetType(m_impl->shapeId) != b3_capsuleShape)
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
}
