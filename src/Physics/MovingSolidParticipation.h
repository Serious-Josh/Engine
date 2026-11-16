#pragma once

// R1.5: which bodies join AIR's moving-solid overlay. Pure and authored-data only: no physics session, Box3D or renderer state,
// so the Editor (file and archive scenes) and the packaged runtime resolve the same decision from the same document.
//
// Rules, in order:
//   1. The scene's AirSimulationSettings::movingSolids is off: nothing participates (the overlay and its warm-pressure path are off).
//   2. Character Controllers participate (they have no PhysicsComponent; the physics session always publishes them).
//   3. Static bodies never participate, whatever their authored value (the session rejects them).
//   4. Never: excluded. Always: included.
//   5. Automatic: included when the body's effective collider extent is at least one AIR cell edge (AutomaticMinimumExtent).
//
// Effective extent: the largest full dimension of the collider physics registers, ScaleCollider(physics, object scale) (the same
// call ScenePhysicsSession makes; physics objects are scene roots, so the object's scale is its world scale):
//   Box      max(width, height, depth)
//   Sphere   2 * radius
//   Capsule  cylinderHeight + 2 * radius (the axis length, never shorter than the diameter)
// The extent is measured in the collider's own frame, independent of its pose. A body exactly one cell long participates.

#include "Physics/ColliderScaling.h"
#include "Physics/PhysicsComponent.h"

#include <algorithm>

namespace Physics
{
// One AIR cell edge at the production lattice (AirFieldSettings::cellExponent = -1: 0.5 m). Pinned here rather than read from the
// renderer, so the authored policy does not depend on renderer configuration; R1.5's tests check that the two still agree.
inline constexpr float AutomaticMinimumExtent = 0.5f;

enum class MovingSolidParticipationReason
{
    SceneDisabled,
    StaticBody,
    Never,
    Always,
    AutomaticLargeEnough,
    AutomaticTooSmall
};

struct MovingSolidParticipationDecision
{
    bool participates = false;
    MovingSolidParticipationReason reason = MovingSolidParticipationReason::SceneDisabled;
    // The effective extent the Automatic rule measured, in metres (0 when it was not measured).
    float extent = 0.0f;
};

inline float MovingSolidExtent(const PhysicsComponent& authored, const Vector3& objectScale)
{
    const PhysicsComponent effective = ScaleCollider(authored, objectScale);

    switch(effective.colliderType)
    {
    case ColliderType::Sphere:
        return 2.0f * effective.sphereRadius;
    case ColliderType::Capsule:
        return effective.capsuleCylinderHeight + 2.0f * effective.capsuleRadius;
    case ColliderType::Box:
        break;
    }

    return (std::max)({effective.boxDimensions.width, effective.boxDimensions.height, effective.boxDimensions.depth});
}

// A body with `physics` at `objectScale`, in a scene whose moving-solid feature is `sceneEnabled`.
inline MovingSolidParticipationDecision ResolveMovingSolidParticipation(bool sceneEnabled, const PhysicsComponent& physics,
    const Vector3& objectScale)
{
    MovingSolidParticipationDecision decision;

    if(!sceneEnabled)
    {
        return decision;
    }

    if(physics.bodyType == BodyType::Static)
    {
        decision.reason = MovingSolidParticipationReason::StaticBody;
        return decision;
    }

    if(physics.airParticipation == MovingSolidParticipation::Never)
    {
        decision.reason = MovingSolidParticipationReason::Never;
        return decision;
    }

    if(physics.airParticipation == MovingSolidParticipation::Always)
    {
        decision.participates = true;
        decision.reason = MovingSolidParticipationReason::Always;
        return decision;
    }

    decision.extent = MovingSolidExtent(physics, objectScale);
    decision.participates = decision.extent >= AutomaticMinimumExtent;
    decision.reason = decision.participates ? MovingSolidParticipationReason::AutomaticLargeEnough :
        MovingSolidParticipationReason::AutomaticTooSmall;
    return decision;
}
}
