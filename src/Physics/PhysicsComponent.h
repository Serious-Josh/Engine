#pragma once

#include "Math/Vector3.h"

#include <string>
#include <string_view>

namespace Physics
{
enum class BodyType
{
    Static,
    Dynamic,
    Kinematic
};

enum class ColliderType
{
    Box = 0,
    Sphere = 1,
    Capsule = 2
};

// R1.5: whether a body joins AIR's moving-solid overlay. Authored per body; in effect only while the scene's
// AirSimulationSettings::movingSolids is on, and only for Dynamic and Kinematic bodies (a Static body never participates, whatever
// its value). Automatic follows the size rule in MovingSolidParticipation.h; Always and Never override it for a movable body.
// Character Controllers have no PhysicsComponent and always participate while the scene feature is on.
enum class MovingSolidParticipation
{
    Automatic = 0,
    Always = 1,
    Never = 2
};

// Stable serialized names ("Automatic", "Always", "Never").
const char* MovingSolidParticipationName(MovingSolidParticipation participation) noexcept;
bool TryParseMovingSolidParticipation(std::string_view name, MovingSolidParticipation& participation) noexcept;

struct BoxDimensions
{
    float width = 1.0f;
    float height = 1.0f;
    float depth = 1.0f;

    bool operator==(const BoxDimensions&) const = default;
};

// Authored values only. Owns no Box3D resources and does not contain simulation state.
struct PhysicsComponent
{
    BodyType bodyType = BodyType::Static;
    ColliderType colliderType = ColliderType::Box;

    // Shape dimensions/radii and the offset are authored in the object's local (unscaled) space. Physics and the
    // Editor overlay use them times the object's scale (ColliderScaling.h); these stored values never change.
    // Scenes before schema 17 stored absolute meters and are converted on load.
    BoxDimensions boxDimensions;
    float sphereRadius = 0.5f;

    // Capsule along local +Y. Cylinder height excludes the hemispherical caps, so
    // total height is capsuleCylinderHeight + 2 * capsuleRadius. The endpoint centers
    // are (0, -capsuleCylinderHeight / 2, 0) and (0, +capsuleCylinderHeight / 2, 0).
    float capsuleRadius = 0.5f;
    float capsuleCylinderHeight = 1.0f;

    // Capsule-only local rotation, Euler radians composed Rz * Ry * Rx like Transform.
    // It turns the capsule axis about the offset point; it never rotates the offset itself.
    Vector3 capsuleLocalRotation{0.0f, 0.0f, 0.0f};

    Vector3 colliderLocalPositionOffset{0.0f, 0.0f, 0.0f};
    float density = 1000.0f;
    float friction = 0.5f;
    float restitution = 0.0f;

    // Opt-in to gameplay-facing contact Begin/End notifications for this object. The
    // collider stays solid: geometry, mass, materials and collision response are unchanged.
    bool generateContactEvents = false;

    // R1.5: kept whatever the body type, so switching between Dynamic, Kinematic and Static never loses it. Persisted from scene
    // schema 27 and cooked from payload 37; earlier scenes read Automatic.
    MovingSolidParticipation airParticipation = MovingSolidParticipation::Automatic;

    bool operator==(const PhysicsComponent& other) const;
};

// Scene admission validates public authored data; malformed values are never clamped.
bool ValidatePhysicsComponent(const PhysicsComponent& component, std::string* errorMessage = nullptr);
}
