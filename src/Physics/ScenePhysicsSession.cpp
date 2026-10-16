#include "Physics/ScenePhysicsSession.h"

#include "Scene/SceneDocument.h"

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

BoxDefinition MakeBoxDefinition(const SceneObjectDocument& object)
{
    std::string error;
    if (!ValidatePhysicsComponent(*object.physics, &error))
    {
        throw std::invalid_argument(error);
    }
    if (!IsFinite(object.transform.position) || !IsFinite(object.transform.rotation) || !IsFinite(object.transform.scale))
    {
        throw std::invalid_argument("Authored physics object transform must be finite");
    }

    const PhysicsComponent& component = *object.physics;
    BoxDefinition definition;
    definition.pose.position = object.transform.position;
    definition.pose.orientation = OrientationFromEulerRadians(object.transform.rotation);
    definition.bodyType = component.bodyType;
    definition.halfExtents = {
        component.boxDimensions.width * 0.5f,
        component.boxDimensions.height * 0.5f,
        component.boxDimensions.depth * 0.5f
    };
    // Offset is baked into local hull geometry, not added to the body origin.
    // Authored visual scale is deliberately absent from both conversions.
    definition.localPositionOffset = component.colliderLocalPositionOffset;
    definition.density = component.density;
    definition.friction = component.friction;
    definition.restitution = component.restitution;
    return definition;
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
            const BoxDefinition definition = MakeBoxDefinition(object);
            auto body = std::make_shared<Body>(m_world.CreateBox(definition));
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
