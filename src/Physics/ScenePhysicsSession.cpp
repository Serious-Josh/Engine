#include "Physics/ScenePhysicsSession.h"

#include "Scene/SceneDocument.h"
#include "Scene/SceneHierarchy.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace Physics
{
namespace
{
bool IsFinite(const Vector3& vector)
{
    return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
}

Quaternion OrientationFromEulerRadians(const Vector3& rotation)
{
    const float sineX = std::sin(rotation.x * 0.5f);
    const float cosineX = std::cos(rotation.x * 0.5f);
    const float sineY = std::sin(rotation.y * 0.5f);
    const float cosineY = std::cos(rotation.y * 0.5f);
    const float sineZ = std::sin(rotation.z * 0.5f);
    const float cosineZ = std::cos(rotation.z * 0.5f);

    // Match Transform/Matrix4: active rotations composed as Rz * Ry * Rx.
    Quaternion orientation{
        sineX * cosineY * cosineZ - cosineX * sineY * sineZ,
        cosineX * sineY * cosineZ + sineX * cosineY * sineZ,
        cosineX * cosineY * sineZ - sineX * sineY * cosineZ,
        cosineX * cosineY * cosineZ + sineX * sineY * sineZ
    };
    const float length = std::sqrt(orientation.x * orientation.x + orientation.y * orientation.y +
        orientation.z * orientation.z + orientation.w * orientation.w);
    orientation.x /= length;
    orientation.y /= length;
    orientation.z /= length;
    orientation.w /= length;
    return orientation;
}

void ValidatePhysicsObject(const SceneObjectDocument& object)
{
    std::string error;

    if (!ValidatePhysicsComponent(*object.physics, &error))
    {
        throw std::invalid_argument(error);
    }

    const Transform& authoredTransform = object.transform;

    if (!IsFinite(authoredTransform.position) ||
        !IsFinite(authoredTransform.rotation) ||
        !IsFinite(authoredTransform.scale))
    {
        throw std::invalid_argument("Authored physics object transform must be finite");
    }
}

BoxDefinition MakeBoxDefinition(const SceneObjectDocument& object)
{
    const PhysicsComponent& component = *object.physics;
    BoxDefinition definition;

    // Body: authored origin and rotation; visual scale never reaches physics.
    definition.pose.position = object.transform.position;
    definition.pose.orientation = OrientationFromEulerRadians(object.transform.rotation);
    definition.bodyType = component.bodyType;

    // Geometry: full authored dimensions become half-extents exactly once.
    definition.halfExtents = {
        component.boxDimensions.width * 0.5f,
        component.boxDimensions.height * 0.5f,
        component.boxDimensions.depth * 0.5f
    };
    // Offset is baked into local hull geometry, not added to the body origin.
    // Authored visual scale is deliberately absent from both conversions.
    definition.localPositionOffset = component.colliderLocalPositionOffset;

    // Material.
    definition.density = component.density;
    definition.friction = component.friction;
    definition.restitution = component.restitution;

    return definition;
}

SphereDefinition MakeSphereDefinition(const SceneObjectDocument& object)
{
    const PhysicsComponent& component = *object.physics;
    SphereDefinition definition;

    // Body: authored origin and rotation; visual scale never reaches physics.
    definition.pose.position = object.transform.position;
    definition.pose.orientation = OrientationFromEulerRadians(object.transform.rotation);
    definition.bodyType = component.bodyType;

    // Geometry: the stored radius is already the physical radius in meters; no halving.
    definition.radius = component.sphereRadius;
    // As for Box, the offset becomes the shape's local center, not a body-origin shift.
    // Authored visual scale is deliberately absent from both radius and offset.
    definition.localPositionOffset = component.colliderLocalPositionOffset;

    // Material.
    definition.density = component.density;
    definition.friction = component.friction;
    definition.restitution = component.restitution;

    return definition;
}

CapsuleDefinition MakeCapsuleDefinition(const SceneObjectDocument& object)
{
    const PhysicsComponent& component = *object.physics;
    CapsuleDefinition definition;

    // Body: authored origin and rotation; visual scale never reaches physics.
    definition.pose.position = object.transform.position;
    definition.pose.orientation = OrientationFromEulerRadians(object.transform.rotation);
    definition.bodyType = component.bodyType;

    // Geometry: +Y capsule with the authored radius and cylinder height (caps excluded).
    // The offset moves both hemisphere centers once, inside the shape, not the body origin.
    definition.radius = component.capsuleRadius;
    definition.cylinderHeight = component.capsuleCylinderHeight;
    definition.localPositionOffset = component.colliderLocalPositionOffset;

    // Same Euler convention as the authored body rotation (qz * qy * qx), applied only
    // to the capsule axis.
    definition.localOrientation = OrientationFromEulerRadians(component.capsuleLocalRotation);

    // Material.
    definition.density = component.density;
    definition.friction = component.friction;
    definition.restitution = component.restitution;

    return definition;
}

// Active rotation of a vector by a unit quaternion: v + 2w(q x v) + 2q x (q x v).
Vector3 RotateVector(const Quaternion& rotation, const Vector3& vector)
{
    const Vector3 axis{rotation.x, rotation.y, rotation.z};
    const Vector3 twiceCross = Vector3::Cross(axis, vector) * 2.0f;
    return vector + twiceCross * rotation.w + Vector3::Cross(axis, twiceCross);
}

Body CreateColliderBody(World& world, const SceneObjectDocument& object)
{
    ValidatePhysicsObject(object);

    switch (object.physics->colliderType)
    {
    case ColliderType::Box:
        return world.CreateBox(MakeBoxDefinition(object));

    case ColliderType::Sphere:
        return world.CreateSphere(MakeSphereDefinition(object));

    case ColliderType::Capsule:
        return world.CreateCapsule(MakeCapsuleDefinition(object));

    default:
        // ValidatePhysicsComponent already rejects unknown collider types.
        throw std::invalid_argument("Unsupported physics collider type.");
    }
}
}

ScenePhysicsSession::ScenePhysicsSession(const WorldConfig& config)
    : m_world(config), m_characterGravity(config.gravity.y)
{
}

ScenePhysicsSession::~ScenePhysicsSession()
{
    // Teardown discards overlap state; it never fabricates Exit events.
    m_pendingTriggerEvents.clear();
    m_triggerOverlaps.clear();
    m_triggerShapeOwners.clear();
    m_retiredTriggerPairs.clear();
    m_sensors.clear();
    // Controller observers, then their Kinematic hosts (controller triggers were in m_sensors).
    m_characterControllers.clear();
    m_sensorBodies.clear();
    // External readers may hold a locked shared reference. Release physical bodies
    // now, while the world lives, regardless of those wrapper references.
    for (auto& [objectID, body] : m_bodies)
    {
        body->Reset();
    }
    m_bodies.clear();
}

std::unique_ptr<ScenePhysicsSession> ScenePhysicsSession::Create(const SceneDocument& document,
    std::string* errorMessage, const WorldConfig& config)
{
    if (errorMessage)
    {
        errorMessage->clear();
    }
    try
    {
        // A parented document's whole forest is validated before native allocation. Simulation
        // components are root-only, so every registered body/trigger/controller uses its authored
        // local TRS as its world spawn pose; physics-free descendants register nothing. Flat
        // documents keep their existing registration checks and messages unchanged.
        const bool flat = std::all_of(document.objects.begin(), document.objects.end(),
            [](const SceneObjectDocument& object)
            {
                return object.parentID.empty();
            });
        SceneHierarchy::Forest hierarchy;

        if(!flat && !SceneHierarchy::Build(document.objects, hierarchy, errorMessage))
        {
            return nullptr;
        }

        // Nothing escapes until all bodies, ID associations and controllers have succeeded.
        // Controllers register last: spawn admission queries the completed solid world.
        auto session = std::unique_ptr<ScenePhysicsSession>(new ScenePhysicsSession(config));
        session->RegisterObjects(document);
        session->RegisterCharacterControllers(document);
        return session;
    }
    catch (const std::exception& exception)
    {
        if (errorMessage)
        {
            *errorMessage = exception.what();
        }
        return nullptr;
    }
}

void ScenePhysicsSession::RegisterObjects(const SceneDocument& document)
{
    std::set<std::string> objectIDs;
    // Register in authored order, not map order, for repeatable Box3D insertion.
    for (const SceneObjectDocument& object : document.objects)
    {
        if (object.id.empty())
        {
            throw std::invalid_argument("Scene physics registration requires nonempty stable object IDs");
        }
        if (!objectIDs.insert(object.id).second)
        {
            throw std::invalid_argument("Duplicate scene physics object ID: " + object.id);
        }
        if(!object.physics && !object.triggerCollider)
        {
            continue;
        }

        try
        {
            if(object.triggerCollider)
            {
                std::string error;

                if(!ValidateTriggerColliderComponent(*object.triggerCollider, &error) ||
                    !IsFinite(object.transform.position) || !IsFinite(object.transform.rotation) ||
                    !IsFinite(object.transform.scale))
                {
                    throw std::invalid_argument("Trigger Collider: " +
                        (error.empty() ? std::string("authored transform must be finite") : error));
                }
            }

            // The body is stored only in this not-yet-returned session; a later failure
            // destroys the session, which releases every body before its world.
            if(object.physics)
            {
                auto body = std::make_shared<Body>(CreateColliderBody(m_world, object));
                m_objectIDsByContactKey.emplace(body->GetContactKey(), object.id);
                m_triggerShapeOwners.emplace(body->GetShapeKey(), TriggerShapeOwner{object.id, false});

                if(object.physics->generateContactEvents)
                {
                    m_contactNotificationObjects.insert(object.id);
                }

                if(object.physics->bodyType != BodyType::Static)
                {
                    m_movingBodyShapes.emplace(body->GetShapeKey(), object.id);
                }

                m_bodies.emplace(object.id, std::move(body));
            }

            // A controller's Trigger Collider attaches to its Kinematic host at admission.
            if(object.triggerCollider && !object.characterController)
            {
                Body* host = nullptr;

                if(object.physics)
                {
                    host = m_bodies.at(object.id).get();
                }
                else
                {
                    Pose pose{object.transform.position, OrientationFromEulerRadians(object.transform.rotation)};
                    auto inserted = m_sensorBodies.emplace(object.id, m_world.CreateSensorBody(pose));
                    host = &inserted.first->second;
                }

                auto inserted = m_sensors.emplace(object.id, m_world.AttachSensor(*host, *object.triggerCollider));
                m_triggerShapeOwners.emplace(inserted.first->second.GetShapeKey(), TriggerShapeOwner{object.id, true});
            }
        }
        catch (const std::exception& exception)
        {
            throw std::runtime_error("Physics registration failed for object " + object.id + ": " + exception.what());
        }
    }
}

std::weak_ptr<const Body> ScenePhysicsSession::FindBody(std::string_view objectID) const
{
    const auto found = m_bodies.find(objectID);
    if (found == m_bodies.end())
    {
        return {};
    }
    return found->second;
}

std::size_t ScenePhysicsSession::GetBodyCount() const noexcept
{
    return m_bodies.size();
}

void ScenePhysicsSession::Step()
{
    // Controllers move against start-of-tick geometry; their hosts then take the accepted
    // poses, so this step's native sensor events describe those poses. After the single
    // native step, carry and recovery adjust controllers and hosts for the next step.
    MoveCharacterControllers();
    SynchronizeCharacterHosts();
    m_world.Step();
    ++m_completedSteps;
    ReconcileCharacterControllers();
    SynchronizeCharacterHosts();
    CaptureStepContactEvents();
    ProcessStepTriggerEvents(m_world.CollectSensorTouchEvents());
}

void ScenePhysicsSession::RegisterCharacterControllers(const SceneDocument& document)
{
    // Authored order for error reporting; runtime processing order is by stable ID.
    for(const SceneObjectDocument& object : document.objects)
    {
        if(!object.characterController)
        {
            continue;
        }

        const CharacterControllerComponent& component = *object.characterController;
        const std::string prefix = "Character Controller admission failed for object " + object.id + ": ";
        std::string error;

        // Stage 1: authored configuration and upright/yaw-only pose (CM.3 validation).
        if(object.physics)
        {
            throw std::invalid_argument(prefix + "a PhysicsComponent cannot share the object.");
        }

        if(!ValidateCharacterControllerTransform(component, object.transform, &error))
        {
            throw std::invalid_argument(prefix + error);
        }

        // Stage 2: runtime geometry and motion settings. Orientation never changes during
        // movement, so the rotated center offset is fixed. Capsule endpoints are exactly
        // upright; gravity and grounding use world +Y regardless of the authored yaw.
        CharacterControllerRuntime runtime;
        runtime.configuration = component;
        runtime.spawnPose = {object.transform.position, OrientationFromEulerRadians(object.transform.rotation)};
        runtime.pose = runtime.spawnPose;
        runtime.centerOffset = RotateVector(runtime.spawnPose.orientation, component.localOffset);
        const float halfCylinder = component.capsuleCylinderHeight * 0.5f;
        auto& settings = runtime.settings;
        settings.queryCapsule = {{0.0f, -halfCylinder, 0.0f}, {0.0f, halfCylinder, 0.0f},
            component.capsuleRadius + component.collisionClearance};
        settings.solidRadius = component.capsuleRadius;
        settings.verticalGravity = m_characterGravity * component.gravityScale;
        settings.jumpSpeed = component.jumpSpeed;
        settings.walkableNormalY = std::cos(component.maximumWalkableSlope);
        settings.groundSnapDistance = component.groundSnapDistance;
        runtime.motion.center = runtime.spawnPose.position + runtime.centerOffset;

        // Stage 3: spawn occupancy and initial support. Casts ignore initial overlap and deep
        // overlap can yield no planes, so neither alone decides admission.
        AdmitCharacterSpawn(object.id, runtime);

        // Controllers are solid obstacles to one another: a spawn may not touch one admitted earlier.
        MoverCapsule guarded = settings.queryCapsule;
        guarded.radius += CharacterSpawnGuard;
        const auto admitted = CollectCharacterObstacles(object.id);
        const CharacterQueryScene scene{m_world, admitted};
        const std::size_t solidOverlaps = m_world.CountSolidOverlaps(runtime.motion.center, guarded);

        if(scene.CountSolidOverlaps(runtime.motion.center, guarded) != solidOverlaps)
        {
            throw std::invalid_argument("Character Controller admission failed for object " + object.id +
                ": spawn capsule overlaps or touches another Character Controller.");
        }

        runtime.motion.ground = EvaluateCharacterGround(scene, runtime.motion.center, settings.queryCapsule,
            settings.walkableNormalY);

        // Native presence: Kinematic host at the runtime pose, visitor-only observer matching
        // the solid capsule, and the authored Trigger Collider (its local offset/rotation are
        // in the host frame, so it follows the controller).
        runtime.host = m_world.CreateSensorBody(runtime.pose, BodyType::Kinematic);
        TriggerColliderComponent observerShape;
        observerShape.colliderType = ColliderType::Capsule;
        observerShape.capsuleRadius = component.capsuleRadius;
        observerShape.capsuleCylinderHeight = component.capsuleCylinderHeight;
        observerShape.colliderLocalPositionOffset = component.localOffset;
        runtime.observer = m_world.AttachSensor(runtime.host, observerShape);
        m_triggerShapeOwners.emplace(runtime.observer.GetShapeKey(), TriggerShapeOwner{object.id, false});

        if(object.triggerCollider)
        {
            auto inserted = m_sensors.emplace(object.id, m_world.AttachSensor(runtime.host, *object.triggerCollider));
            m_triggerShapeOwners.emplace(inserted.first->second.GetShapeKey(), TriggerShapeOwner{object.id, true});
        }

        m_characterControllers.emplace(object.id, std::move(runtime));
    }
}

void ScenePhysicsSession::AdmitCharacterSpawn(const std::string& objectID, const CharacterControllerRuntime& runtime) const
{
    const std::string prefix = "Character Controller admission failed for object " + objectID + ": ";
    const auto& settings = runtime.settings;
    const Vector3 center = runtime.motion.center;
    const auto describe = [this](std::uint64_t shapeKey)
    {
        const auto owner = m_triggerShapeOwners.find(shapeKey);

        if(owner == m_triggerShapeOwners.end())
        {
            return std::string("unregistered solid geometry");
        }

        return "object " + owner->second.objectID;
    };

    // (a) Every shape within radius + clearance + guard must be classified; none means clear.
    MoverCapsule guarded = settings.queryCapsule;
    guarded.radius += CharacterSpawnGuard;
    const auto touchingKeys = m_world.CollectSolidOverlapKeys(center, guarded);

    if(touchingKeys.empty())
    {
        return;
    }

    // (b) The solid capsule shrunk by the contact tolerance must overlap nothing. This rejects
    // deep floor penetration and any real interpenetration, including plane-free deep cases.
    const float contactShrink = (std::min)(CharacterSpawnContactTolerance, settings.solidRadius * 0.5f);
    const MoverCapsule core{settings.queryCapsule.lowerCenter, settings.queryCapsule.upperCenter,
        settings.solidRadius - contactShrink};
    std::uint64_t penetratingShape = 0;

    if(m_world.CountSolidOverlaps(center, core, &penetratingShape) != 0)
    {
        throw std::invalid_argument(prefix + "spawn capsule penetrates " + describe(penetratingShape) +
            "; place it with clearance or resting floor contact (it is not moved automatically).");
    }

    // (c) Every touching shape must yield planes and every plane must be walkable floor
    // contact. Walls, ceilings, steep slopes and unclassifiable overlaps are rejected; one
    // upward plane never excuses another touching shape.
    const MoverPlaneQuery planes = m_world.CollideSolidMover(center, guarded, CharacterMovePlaneCapacity);

    if(planes.overflowCount != 0)
    {
        throw std::invalid_argument(prefix + "spawn contact has too many collision planes to classify.");
    }

    for(const std::uint64_t shapeKey : touchingKeys)
    {
        bool classified = false;

        for(const auto& plane : planes.planes)
        {
            if(plane.shapeKey != shapeKey)
            {
                continue;
            }

            classified = true;

            if(plane.normal.y < settings.walkableNormalY - CharacterWalkableNormalTolerance)
            {
                throw std::invalid_argument(prefix + "spawn capsule overlaps or touches non-walkable " +
                    describe(shapeKey) + "; place it with positive clearance (it is not moved automatically).");
            }
        }

        if(!classified)
        {
            throw std::invalid_argument(prefix + "spawn capsule overlaps " + describe(shapeKey) +
                " without a classifiable contact; place it with positive clearance.");
        }
    }
}

void ScenePhysicsSession::MoveCharacterControllers()
{
    const std::uint64_t step = m_completedSteps + 1;

    for(auto& [objectID, controller] : m_characterControllers)
    {
        // Consume exactly this step's commands; absent commands mean no horizontal request.
        CharacterTickInput input;
        const auto command = controller.pendingVelocities.find(step);

        if(command != controller.pendingVelocities.end())
        {
            input.horizontalVelocity = command->second;
            controller.pendingVelocities.erase(command);
            controller.lastMovementCommandStep = step;
        }

        input.jumpRequested = controller.pendingJumps.erase(step) != 0;

        if(input.jumpRequested)
        {
            controller.lastJumpRequestStep = step;
        }

        // Limit speed in double precision so large finite inputs cannot overflow.
        Vector3& velocity = input.horizontalVelocity;
        const double speed = std::hypot(static_cast<double>(velocity.x), static_cast<double>(velocity.z));
        const double maximumSpeed = controller.configuration.maximumHorizontalSpeed;

        if(speed > maximumSpeed)
        {
            const double scale = maximumSpeed / speed;
            velocity = {static_cast<float>(velocity.x * scale), 0.0f, static_cast<float>(velocity.z * scale)};
        }

        const auto obstacles = CollectCharacterObstacles(objectID);
        const CharacterQueryScene scene{m_world, obstacles};

        try
        {
            controller.lastResult = SimulateCharacterTick(scene, controller.settings, input, controller.motion);
        }
        catch(const std::exception& exception)
        {
            throw std::runtime_error("Character Controller movement failed for object " + objectID + ": " +
                exception.what());
        }

        // Failure statuses resolve to zero, so the last accepted pose is kept. Accumulating the
        // resolved displacement (as CM.6 did) keeps a stationary pose bitwise at its spawn value.
        controller.pose.position = controller.pose.position + controller.lastResult.resolvedDisplacement;

        controller.contactOrigin = controller.motion.center;

        // Record a moving support's pose before the native step moves it.
        controller.support.reset();
        const auto& ground = controller.motion.ground;
        const auto supportOwner = m_movingBodyShapes.find(ground.shapeKey);

        if(ground.grounded && supportOwner != m_movingBodyShapes.end())
        {
            controller.support = CharacterControllerRuntime::MovingSupport{supportOwner->second,
                m_bodies.at(supportOwner->second)->GetPose()};
        }
    }
}

std::vector<CharacterObstacle> ScenePhysicsSession::CollectCharacterObstacles(std::string_view excludedObjectID) const
{
    std::vector<CharacterObstacle> obstacles;
    obstacles.reserve(m_characterControllers.size());

    for(const auto& [objectID, controller] : m_characterControllers)
    {
        if(objectID == excludedObjectID)
        {
            continue;
        }

        const auto& query = controller.settings.queryCapsule;
        obstacles.push_back({controller.motion.center, {query.lowerCenter, query.upperCenter,
            controller.settings.solidRadius}});
    }

    return obstacles;
}

void ScenePhysicsSession::SynchronizeCharacterHosts()
{
    for(auto& [objectID, controller] : m_characterControllers)
    {
        controller.host.SetKinematicPose(controller.pose);
    }
}

void ScenePhysicsSession::ReconcileCharacterControllers()
{
    for(auto& [objectID, controller] : m_characterControllers)
    {
        const auto obstacles = CollectCharacterObstacles(objectID);
        const CharacterQueryScene scene{m_world, obstacles};
        auto& result = controller.lastResult;
        const Vector3 before = controller.motion.center;
        controller.carrierObjectID.clear();

        try
        {
            // Carry: translation of the support recorded before the step (rotation is not carried).
            if(controller.support)
            {
                const auto body = m_bodies.find(controller.support->objectID);

                if(body != m_bodies.end())
                {
                    const Vector3 supportDisplacement = body->second->GetPose().position -
                        controller.support->poseBeforeStep.position;
                    result.carriedDisplacement = CarryCharacter(scene, controller.settings, controller.motion,
                        supportDisplacement);
                    controller.carrierObjectID = controller.support->objectID;
                }
            }

            // Recovery: bounded push-out from solids that moved into the controller.
            result.recovery = RecoverCharacterOverlap(scene, controller.settings, controller.motion,
                result.recoveryDisplacement);
            controller.motion.ground = EvaluateCharacterGround(scene, controller.motion.center,
                controller.settings.queryCapsule, controller.settings.walkableNormalY);
        }
        catch(const std::exception& exception)
        {
            throw std::runtime_error("Character Controller reconciliation failed for object " + objectID + ": " +
                exception.what());
        }

        result.ground = controller.motion.ground;
        controller.pose.position = controller.pose.position + (controller.motion.center - before);
        controller.support.reset();
    }
}

std::size_t ScenePhysicsSession::GetCharacterControllerCount() const noexcept
{
    return m_characterControllers.size();
}

std::optional<Pose> ScenePhysicsSession::FindCharacterControllerPose(std::string_view objectID) const
{
    const auto found = m_characterControllers.find(objectID);

    if(found == m_characterControllers.end())
    {
        return std::nullopt;
    }

    return found->second.pose;
}

std::optional<CharacterMotionState> ScenePhysicsSession::FindCharacterMotionState(std::string_view objectID) const
{
    const auto found = m_characterControllers.find(objectID);

    if(found == m_characterControllers.end())
    {
        return std::nullopt;
    }

    return found->second.motion;
}

const CharacterMoveResult* ScenePhysicsSession::FindLastCharacterMoveResult(std::string_view objectID) const
{
    const auto found = m_characterControllers.find(objectID);

    if(found == m_characterControllers.end())
    {
        return nullptr;
    }

    return &found->second.lastResult;
}

bool ScenePhysicsSession::SubmitCharacterMovement(std::string_view objectID, std::uint64_t fixedStep,
    Vector3 desiredHorizontalVelocity, std::string* errorMessage)
{
    const auto fail = [errorMessage](std::string message)
    {
        if(errorMessage)
        {
            *errorMessage = std::move(message);
        }

        return false;
    };

    const auto controller = m_characterControllers.find(objectID);

    if(controller == m_characterControllers.end())
    {
        return fail("No Character Controller is registered for object " + std::string(objectID) + ".");
    }

    if(fixedStep <= m_completedSteps)
    {
        return fail("Character movement targets a fixed step that has already run.");
    }

    if(fixedStep - m_completedSteps > MaximumCharacterCommandLeadSteps)
    {
        return fail("Character movement targets a fixed step beyond the bounded command window.");
    }

    if(!IsFinite(desiredHorizontalVelocity) || desiredHorizontalVelocity.y != 0.0f)
    {
        return fail("Character movement velocity must be finite and horizontal (y = 0).");
    }

    controller->second.pendingVelocities.insert_or_assign(fixedStep, desiredHorizontalVelocity);

    if(errorMessage)
    {
        errorMessage->clear();
    }

    return true;
}

bool ScenePhysicsSession::SubmitCharacterJump(std::string_view objectID, std::uint64_t fixedStep,
    std::string* errorMessage)
{
    const auto fail = [errorMessage](std::string message)
    {
        if(errorMessage)
        {
            *errorMessage = std::move(message);
        }

        return false;
    };

    const auto controller = m_characterControllers.find(objectID);

    if(controller == m_characterControllers.end())
    {
        return fail("No Character Controller is registered for object " + std::string(objectID) + ".");
    }

    if(fixedStep <= m_completedSteps)
    {
        return fail("Character jump targets a fixed step that has already run.");
    }

    if(fixedStep - m_completedSteps > MaximumCharacterCommandLeadSteps)
    {
        return fail("Character jump targets a fixed step beyond the bounded command window.");
    }

    controller->second.pendingJumps.insert(fixedStep);

    if(errorMessage)
    {
        errorMessage->clear();
    }

    return true;
}

void ScenePhysicsSession::RetireTriggerShape(std::uint64_t shapeKey)
{
    // Native Ends for a destroyed shape are discarded as invalid, so its contributions must be
    // erased explicitly before destruction; otherwise the pair would stay occupied forever.
    for(auto& [objectPair, contributions] : m_triggerOverlaps)
    {
        const std::size_t before = contributions.size();
        std::erase_if(contributions, [shapeKey](const TriggerShapePair& contribution)
        {
            return contribution.first == shapeKey || contribution.second == shapeKey;
        });

        if(before != 0 && contributions.empty())
        {
            m_retiredTriggerPairs.insert(objectPair);
        }
    }

    m_triggerShapeOwners.erase(shapeKey);
}

std::vector<CharacterControllerDiagnostics> ScenePhysicsSession::CaptureCharacterDiagnostics() const
{
    std::vector<CharacterControllerDiagnostics> snapshot;
    snapshot.reserve(m_characterControllers.size());

    for(const auto& [objectID, controller] : m_characterControllers)
    {
        CharacterControllerDiagnostics diagnostics;
        const auto& result = controller.lastResult;
        diagnostics.objectID = objectID;
        diagnostics.pose = controller.pose;
        diagnostics.capsuleCenter = controller.motion.center;
        diagnostics.capsuleRadius = controller.configuration.capsuleRadius;
        diagnostics.capsuleCylinderHeight = controller.configuration.capsuleCylinderHeight;
        diagnostics.verticalVelocity = controller.motion.verticalVelocity;
        diagnostics.ground = controller.motion.ground;

        if(diagnostics.ground.grounded)
        {
            const auto owner = m_triggerShapeOwners.find(diagnostics.ground.shapeKey);

            if(owner != m_triggerShapeOwners.end())
            {
                diagnostics.groundObjectID = owner->second.objectID;
            }
        }

        diagnostics.lastMovementCommandStep = controller.lastMovementCommandStep;
        diagnostics.lastJumpRequestStep = controller.lastJumpRequestStep;
        diagnostics.lastStatus = result.status;
        diagnostics.lastJumpOutcome = result.jumpOutcome;
        diagnostics.stepped = result.stepped;
        diagnostics.snapped = result.snapped;
        diagnostics.recovery = result.recovery;
        diagnostics.carriedDisplacement = result.carriedDisplacement;
        diagnostics.recoveryDisplacement = result.recoveryDisplacement;
        diagnostics.carrierObjectID = controller.carrierObjectID;
        diagnostics.contactCount = result.contacts.size();

        // Bounded copy; contact points become world positions.
        const std::size_t copied = (std::min)(result.contacts.size(), MaximumCharacterDiagnosticContacts);
        diagnostics.contacts.reserve(copied);

        for(std::size_t index = 0; index < copied; ++index)
        {
            MoverPlane contact = result.contacts[index];
            contact.point = controller.contactOrigin + contact.point;
            diagnostics.contacts.push_back(contact);
        }

        diagnostics.observerRegistered = controller.observer.IsValid() &&
            m_triggerShapeOwners.contains(controller.observer.GetShapeKey());
        diagnostics.ownsTriggerCollider = m_sensors.contains(objectID);
        snapshot.push_back(std::move(diagnostics));
    }

    return snapshot;
}

bool ScenePhysicsSession::IsTriggerOverlapping(std::string_view triggerObjectID, std::string_view otherObjectID) const
{
    const auto found = m_triggerOverlaps.find({std::string(triggerObjectID), std::string(otherObjectID)});
    return found != m_triggerOverlaps.end() && !found->second.empty();
}

bool ScenePhysicsSession::RetireCharacterController(std::string_view objectID, std::string* errorMessage)
{
    const auto controller = m_characterControllers.find(objectID);

    if(controller == m_characterControllers.end())
    {
        if(errorMessage)
        {
            *errorMessage = "No Character Controller is registered for object " + std::string(objectID) + ".";
        }

        return false;
    }

    // Retire contributions first, then destroy sensors before their host.
    RetireTriggerShape(controller->second.observer.GetShapeKey());
    const auto ownedTrigger = m_sensors.find(objectID);

    if(ownedTrigger != m_sensors.end())
    {
        RetireTriggerShape(ownedTrigger->second.GetShapeKey());
        m_sensors.erase(ownedTrigger);
    }

    m_characterControllers.erase(controller);
    return true;
}

bool ScenePhysicsSession::ReplaceCharacterObserver(std::string_view objectID, std::string* errorMessage)
{
    const auto controller = m_characterControllers.find(objectID);

    if(controller == m_characterControllers.end())
    {
        if(errorMessage)
        {
            *errorMessage = "No Character Controller is registered for object " + std::string(objectID) + ".";
        }

        return false;
    }

    auto& runtime = controller->second;
    TriggerColliderComponent observerShape;
    observerShape.colliderType = ColliderType::Capsule;
    observerShape.capsuleRadius = runtime.configuration.capsuleRadius;
    observerShape.capsuleCylinderHeight = runtime.configuration.capsuleCylinderHeight;
    observerShape.colliderLocalPositionOffset = runtime.configuration.localOffset;

    // Create the replacement first so a failure leaves the old observer and its occupancy intact.
    Sensor replacement = m_world.AttachSensor(runtime.host, observerShape);
    RetireTriggerShape(runtime.observer.GetShapeKey());
    runtime.observer = std::move(replacement);
    m_triggerShapeOwners.emplace(runtime.observer.GetShapeKey(), TriggerShapeOwner{std::string(objectID), false});
    return true;
}

std::uint64_t ScenePhysicsSession::GetCompletedStepCount() const noexcept
{
    return m_completedSteps;
}

std::vector<ContactEvent> ScenePhysicsSession::TakeContactEvents()
{
    return std::exchange(m_pendingContactEvents, {});
}

std::vector<TriggerEvent> ScenePhysicsSession::TakeTriggerEvents()
{
    return std::exchange(m_pendingTriggerEvents, {});
}

void ScenePhysicsSession::ProcessStepTriggerEvents(const std::vector<SensorTouchEvent>& observations)
{
    struct ShapeTransitions
    {
        std::set<TriggerShapePair> begins;
        std::set<TriggerShapePair> ends;
    };

    std::map<TriggerObjectPair, ShapeTransitions> transitions;

    for(const auto& observation : observations)
    {
        const auto sensor = m_triggerShapeOwners.find(observation.sensorShapeKey);
        const auto visitor = m_triggerShapeOwners.find(observation.visitorShapeKey);

        if(sensor == m_triggerShapeOwners.end() || visitor == m_triggerShapeOwners.end() ||
            !sensor->second.sensor || sensor->second.objectID == visitor->second.objectID ||
            !m_world.IsShapeKeyValid(observation.sensorShapeKey) || !m_world.IsShapeKeyValid(observation.visitorShapeKey))
        {
            continue;
        }

        auto& pair = transitions[{sensor->second.objectID, visitor->second.objectID}];
        const TriggerShapePair contribution{observation.sensorShapeKey, observation.visitorShapeKey};
        (observation.began ? pair.begins : pair.ends).insert(contribution);
    }

    // Pairs emptied by same-session retirement: Exit now unless a replacement began this step.
    for(const auto& retiredPair : m_retiredTriggerPairs)
    {
        transitions[retiredPair];
    }

    m_retiredTriggerPairs.clear();
    std::vector<TriggerEvent> stepEvents;

    for(const auto& [objectPair, changes] : transitions)
    {
        const auto previous = m_triggerOverlaps.find(objectPair);
        const bool wasOverlapping = previous != m_triggerOverlaps.end();
        auto& active = m_triggerOverlaps[objectPair];

        // Treat a complete step as one batch. Duplicate observations collapse in sets.
        // A contribution in both sets preserves its prior membership: the API supplies
        // no within-step chronology from which to infer a transient Enter/Exit.
        for(const auto& contribution : changes.ends)
        {
            if(!changes.begins.contains(contribution))
            {
                active.erase(contribution);
            }
        }

        for(const auto& contribution : changes.begins)
        {
            if(!changes.ends.contains(contribution))
            {
                active.insert(contribution);
            }
        }

        const bool isOverlapping = !active.empty();

        if(wasOverlapping != isOverlapping)
        {
            stepEvents.push_back({m_completedSteps, isOverlapping ? TriggerEventType::Enter : TriggerEventType::Exit,
                objectPair.first, objectPair.second});
        }

        if(!isOverlapping)
        {
            m_triggerOverlaps.erase(objectPair);
        }
    }

    std::sort(stepEvents.begin(), stepEvents.end(), TriggerEventPrecedes);
    m_pendingTriggerEvents.insert(m_pendingTriggerEvents.end(), stepEvents.begin(), stepEvents.end());
}

std::vector<ContactNotification> ScenePhysicsSession::SelectContactNotifications(
    const std::vector<ContactEvent>& contactEvents) const
{
    std::vector<ContactNotification> notifications;

    for(const ContactEvent& contactEvent : contactEvents)
    {
        // Eligibility follows the participant, not its canonical A/B position.
        const bool notifyObjectA = m_contactNotificationObjects.contains(contactEvent.objectA);
        const bool notifyObjectB = m_contactNotificationObjects.contains(contactEvent.objectB);

        if(!notifyObjectA && !notifyObjectB)
        {
            continue;
        }

        notifications.push_back({contactEvent, notifyObjectA, notifyObjectB});
    }

    return notifications;
}

void ScenePhysicsSession::CaptureStepContactEvents()
{
    // Map native transitions to canonical object pairs; unknown keys and self-pairs are
    // ignored. Ends are applied before begins so an end-then-begin within one step stays
    // balanced in the per-pair count.
    struct PairTransition
    {
        std::pair<std::string, std::string> objectPair;
        bool began;
    };

    std::vector<PairTransition> endTransitions;
    std::vector<PairTransition> beginTransitions;

    for(const ContactTouchEvent& touchEvent : m_world.CollectContactTouchEvents())
    {
        const auto objectA = m_objectIDsByContactKey.find(touchEvent.bodyKeyA);
        const auto objectB = m_objectIDsByContactKey.find(touchEvent.bodyKeyB);

        if(objectA == m_objectIDsByContactKey.end() || objectB == m_objectIDsByContactKey.end())
        {
            continue;
        }

        if(objectA->second == objectB->second)
        {
            continue;
        }

        auto objectPair = std::minmax(objectA->second, objectB->second);
        PairTransition transition{{objectPair.first, objectPair.second}, touchEvent.began};

        if(touchEvent.began)
        {
            beginTransitions.push_back(std::move(transition));
        }
        else
        {
            endTransitions.push_back(std::move(transition));
        }
    }

    std::vector<ContactEvent> stepEvents;

    for(const PairTransition& transition : endTransitions)
    {
        auto touching = m_touchingContactCounts.find(transition.objectPair);

        // An End without a matching Begin cannot change object-level state.
        if(touching == m_touchingContactCounts.end())
        {
            continue;
        }

        --touching->second;

        if(touching->second == 0)
        {
            m_touchingContactCounts.erase(touching);
            stepEvents.push_back({m_completedSteps, ContactEventType::End,
                transition.objectPair.first, transition.objectPair.second});
        }
    }

    for(const PairTransition& transition : beginTransitions)
    {
        const int touchingContacts = ++m_touchingContactCounts[transition.objectPair];

        if(touchingContacts == 1)
        {
            stepEvents.push_back({m_completedSteps, ContactEventType::Begin,
                transition.objectPair.first, transition.objectPair.second});
        }
    }

    // Native event order is not part of the contract; publish in a deterministic order.
    std::sort(stepEvents.begin(), stepEvents.end(), ContactEventPrecedes);
    m_pendingContactEvents.insert(m_pendingContactEvents.end(), stepEvents.begin(), stepEvents.end());
}

std::optional<RuntimePoseSnapshot> ScenePhysicsSession::CapturePoses(std::string* errorMessage) const
{
    if (errorMessage)
    {
        errorMessage->clear();
    }
    try
    {
        RuntimePoseSnapshot snapshot;
        snapshot.m_entries.reserve(m_bodies.size() + m_characterControllers.size());
        // The map already provides deterministic stable-ID order. No sorting,
        // body ownership, or authored transform copies are needed in the result.
        for (const auto& [objectID, body] : m_bodies)
        {
            try
            {
                const Pose pose = body->GetPose();
                std::string validationError;
                if (!ValidateRuntimePose(pose, &validationError))
                {
                    throw std::runtime_error(validationError);
                }
                snapshot.m_entries.push_back({objectID, pose});
            }
            catch (const std::exception& exception)
            {
                throw std::runtime_error("Physics pose capture failed for object " + objectID + ": " + exception.what());
            }
        }

        // Controller poses join the same snapshot: Physics and controllers are exclusive per
        // object, so each object ID still has exactly one runtime pose owner.
        for(const auto& [objectID, controller] : m_characterControllers)
        {
            std::string validationError;

            if(!ValidateRuntimePose(controller.pose, &validationError))
            {
                throw std::runtime_error("Character Controller pose capture failed for object " + objectID + ": " +
                    validationError);
            }

            snapshot.m_entries.push_back({objectID, controller.pose});
        }

        std::sort(snapshot.m_entries.begin(), snapshot.m_entries.end(),
            [](const RuntimePoseEntry& left, const RuntimePoseEntry& right)
        {
            return left.objectID < right.objectID;
        });
        return snapshot;
    }
    catch (const std::exception& exception)
    {
        if (errorMessage)
        {
            *errorMessage = exception.what();
        }
        return std::nullopt;
    }
}
}
