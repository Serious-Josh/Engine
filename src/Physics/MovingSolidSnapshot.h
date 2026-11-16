#pragma once

#include "Math/Vector3.h"
#include "Physics/PhysicsComponent.h"
#include "Physics/PhysicsWorld.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Physics
{
enum class MovingSolidKind
{
    Dynamic,
    Kinematic,
    CharacterController
};

// Solid collider geometry in the solid's own frame (pose applies), unscaled meters as Box3D
// registered it, so collider scale is already included. Only the fields of `type` are set:
//   Box:     halfExtents about center (no local rotation exists for boxes);
//   Sphere:  radius about center;
//   Capsule: radius about the segment lowerCenter..upperCenter (local rotation included).
struct MovingSolidShape
{
    ColliderType type = ColliderType::Box;
    Vector3 center{0.0f, 0.0f, 0.0f};
    Vector3 halfExtents{0.0f, 0.0f, 0.0f};
    Vector3 lowerCenter{0.0f, 0.0f, 0.0f};
    Vector3 upperCenter{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
};

// One participating moving solid at the end of the snapshot's completed step.
// The velocity of any world point x on the solid is linearVelocity + angularVelocity x (x - pose.position).
struct MovingSolidEntry
{
    std::string objectID;
    MovingSolidKind kind = MovingSolidKind::Dynamic;
    MovingSolidShape shape;
    Pose pose;
    // Pose at the start of the most recent step, or the current pose when `discontinuous`.
    Pose previousPose;
    // World space, of the body origin (pose.position), m/s; angular rad/s.
    Vector3 linearVelocity{0.0f, 0.0f, 0.0f};
    Vector3 angularVelocity{0.0f, 0.0f, 0.0f};
    // A new session-unique value at every discontinuity (admission, participation start, teleport).
    // A consumer holding a different epoch for this object must not interpolate or sweep across it.
    std::uint64_t motionEpoch = 0;
    // True when a discontinuity happened after the most recent step began: previousPose then
    // equals pose and carries no motion.
    bool discontinuous = false;
};

// Value-owned, immutable once published: no session, Box3D or scene references. Published as
// shared_ptr<const MovingSolidSnapshot>, so any thread may read a snapshot it was handed.
class MovingSolidSnapshot
{
public:
    std::span<const MovingSolidEntry> GetEntries() const noexcept;
    // Sorted by object ID; null if the object does not participate.
    const MovingSolidEntry* Find(std::string_view objectID) const noexcept;
    // The physics session generation (PhysicsSimulationController) and its completed step count.
    std::uint64_t GetSessionGeneration() const noexcept;
    std::uint64_t GetCompletedSteps() const noexcept;

private:
    std::vector<MovingSolidEntry> m_entries;
    std::uint64_t m_sessionGeneration = 0;
    std::uint64_t m_completedSteps = 0;
    friend class ScenePhysicsSession;
};
}
