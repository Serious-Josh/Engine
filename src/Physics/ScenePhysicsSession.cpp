#include "Physics/ScenePhysicsSession.h"

#include "Scene/SceneDocument.h"

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
    : m_world(config)
{
}

ScenePhysicsSession::~ScenePhysicsSession()
{
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
        // Nothing escapes until all bodies and ID associations have succeeded.
        auto session = std::unique_ptr<ScenePhysicsSession>(new ScenePhysicsSession(config));
        session->RegisterObjects(document);
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
        if (!object.physics)
        {
            continue;
        }

        try
        {
            // The body is stored only in this not-yet-returned session; a later failure
            // destroys the session, which releases every body before its world.
            auto body = std::make_shared<Body>(CreateColliderBody(m_world, object));
            m_objectIDsByContactKey.emplace(body->GetContactKey(), object.id);

            if(object.physics->generateContactEvents)
            {
                m_contactNotificationObjects.insert(object.id);
            }

            m_bodies.emplace(object.id, std::move(body));
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
    m_world.Step();
    ++m_completedSteps;
    CaptureStepContactEvents();
}

std::uint64_t ScenePhysicsSession::GetCompletedStepCount() const noexcept
{
    return m_completedSteps;
}

std::vector<ContactEvent> ScenePhysicsSession::TakeContactEvents()
{
    return std::exchange(m_pendingContactEvents, {});
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
        snapshot.m_entries.reserve(m_bodies.size());
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
