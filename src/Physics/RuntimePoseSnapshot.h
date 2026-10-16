#pragma once

#include "Physics/PhysicsWorld.h"
#include "Math/Matrix4.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

class PresentationTransformVerification;

namespace Physics
{
struct RuntimePoseEntry
{
    std::string objectID;
    Pose pose;
};

// Value-owned simulated data only: no scene references, asset bindings, scale, or body ownership.
class RuntimePoseSnapshot
{
public:
    std::span<const RuntimePoseEntry> GetEntries() const noexcept;

    // Borrowed from this snapshot, not its originating session. Valid until the
    // snapshot is destroyed, moved from, or assigned a replacement value.
    const Pose* FindPose(std::string_view objectID) const noexcept;

private:
    std::vector<RuntimePoseEntry> m_entries;
    friend class ScenePhysicsSession;
    friend class ::PresentationTransformVerification;
};

// Shared pure validation also permits malformed-data tests without mutating live bodies.
// Captured quaternions must be finite unit rotations within float tolerance.
bool ValidateRuntimePose(const Pose& pose, std::string* errorMessage = nullptr);

// Column-vector model transform: translation * quaternion rotation * visual scale.
// Throws invalid_argument for invalid pose/scale and overflow_error for a nonfinite result.
Matrix4 ComposeRuntimeModelMatrix(const Pose& pose, const Vector3& authoredVisualScale);
}
