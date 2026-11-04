#pragma once

#include "Math/Vector3.h"
#include "Physics/PhysicsComponent.h"
#include "Physics/TriggerColliderComponent.h"

#include <string>

namespace Physics
{
// Box, Sphere and Capsule collider dimensions and the collider local offset are authored in the object's local
// (unscaled) space. The collider physics and the Editor overlay use are the authored collider times the object's
// transform scale. Collider objects are always scene roots (H.7), so the object's scale is its world scale and no
// parent shear can reach a collider. Character Controllers are not scaled.
//
// Per-dimension factors, all derived from |scale| (negative scale mirrors geometry, it never inverts a size):
//   Box         each full dimension times |scale| on its own axis.
//   Sphere      radius times max(|sx|, |sy|, |sz|), the smallest sphere containing the scaled one.
//   Capsule     with axis a = R(capsuleLocalRotation) * +Y and cross axes u = R * +X, w = R * +Z:
//               cylinder height times |S a|, radius times max(|S u|, |S w|). With no capsule rotation this is
//               height * |sy| and radius * max(|sx|, |sz|). The capsule keeps its authored axis direction; a
//               tilted capsule under nonuniform or mirrored scale is not sheared or reflected with the mesh.
//   Offset      times the signed scale per axis, so the collider stays on the mirrored mesh.
// Uniform |scale| uses the same factor for every dimension, so it is exact for any capsule rotation.
struct ColliderScaleFactors
{
    Vector3 box{1.0f, 1.0f, 1.0f};
    float sphereRadius = 1.0f;
    float capsuleRadius = 1.0f;
    float capsuleCylinderHeight = 1.0f;
    Vector3 offset{1.0f, 1.0f, 1.0f};
};

ColliderScaleFactors ComputeColliderScaleFactors(const Vector3& objectScale, const Vector3& capsuleLocalRotation);

// Effective collider for an object at `objectScale`. Pure: the authored component is never modified, so scaling is
// never cumulative. Degenerate scales produce zero or nonfinite dimensions that the component validators and Box3D
// admission reject; nothing is clamped.
PhysicsComponent ScaleCollider(const PhysicsComponent& authored, const Vector3& objectScale);
TriggerColliderComponent ScaleCollider(const TriggerColliderComponent& authored, const Vector3& objectScale);

// Scene migration (schema < kColliderScaleSchemaVersion): converts collider values stored as absolute meters into
// object-local values whose ScaleCollider() result at `objectScale` reproduces the stored values.
//   Exact          every active field (the current shape's dimensions and the offset) is reproduced bit for bit.
//   Nearest        float multiplication cannot reach some stored value exactly; the nearest local value is used,
//                  within one unit in the last place of the stored value. `detail` names each field and residual so
//                  the caller reports it.
//   Unconvertible  no local value exists (zero or nonfinite scale, or overflow); the component is left exactly as
//                  stored and `detail` explains why.
// Inactive shape fields are converted when possible and otherwise kept; they never affect the outcome.
enum class ColliderMigrationOutcome
{
    Exact,
    Nearest,
    Unconvertible
};

ColliderMigrationOutcome LocalizeStoredCollider(PhysicsComponent& component, const Vector3& objectScale,
    std::string& detail);
ColliderMigrationOutcome LocalizeStoredCollider(TriggerColliderComponent& component, const Vector3& objectScale,
    std::string& detail);
}
