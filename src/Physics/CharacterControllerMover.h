#pragma once

#include "Physics/PhysicsWorld.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Physics
{
enum class CharacterMoveStatus
{
    NoMovement,            // Nothing was intended and nothing moved.
    Unobstructed,          // The intended horizontal plus vertical displacement was fully applied.
    Constrained,           // Solid constraints, a step or a snap changed the intended displacement.
    QueryCapacityExceeded, // A plane query overflowed; pose kept, vertical velocity cleared.
    PenetrationRejected    // The resolved pose grossly overlapped solid geometry; pose kept, velocity cleared.
};

// Another controller's upright solid capsule (world center; endpoints relative to it), used as
// a geometric obstacle. It is not a native shape: its planes carry shape key 0 and it never
// produces contacts, impulses or events.
struct CharacterObstacle
{
    Vector3 center;
    MoverCapsule solidCapsule;
};

// Solid geometry seen by one controller's queries: native solid shapes plus the other
// controllers. Same three queries and contracts as World's mover queries.
struct CharacterQueryScene
{
    const World& world;
    std::span<const CharacterObstacle> obstacles;

    std::size_t CountSolidOverlaps(Vector3 origin, const MoverCapsule& capsule) const;
    MoverPlaneQuery CollideSolidMover(Vector3 origin, const MoverCapsule& capsule, std::size_t capacity) const;
    float CastSolidMover(Vector3 origin, const MoverCapsule& capsule, Vector3 translation) const;
};

// Outcome of the post-step overlap check against moving solids.
enum class CharacterRecovery
{
    NotNeeded,  // Within solver tolerance of every plane.
    Recovered,  // Pushed out by a bounded, validated displacement.
    Unresolved  // No safe push existed (too deep, or blocked); the pose was kept.
};

enum class CharacterJumpOutcome
{
    None,            // No jump was requested for this step.
    Accepted,        // Grounded at the start of the step: vertical velocity set to jumpSpeed.
    IgnoredAirborne, // Requested while airborne: consumed and discarded, never replayed on landing.
    IgnoredZeroSpeed // Requested while grounded with jumpSpeed == 0: consumed with no effect.
};

// Walkable support under the capsule, from geometric contact normals only.
struct CharacterGroundState
{
    bool grounded = false;
    Vector3 normal{0.0f, 1.0f, 0.0f};
    // World shape key of the selected support (0 when airborne or unregistered).
    std::uint64_t shapeKey = 0;
    // World contact point of the selected support (an edge point for ledge support).
    Vector3 point;
};

// Runtime-only motion state owned by the session; never authored data.
struct CharacterMotionState
{
    Vector3 center;
    float verticalVelocity = 0.0f;
    CharacterGroundState ground;
};

// Per-controller constants derived once from the validated authored component.
struct CharacterMotionSettings
{
    MoverCapsule queryCapsule;
    float solidRadius = 0.0f;
    float verticalGravity = 0.0f; // World gravity y * gravityScale, m/s^2.
    float jumpSpeed = 0.0f;
    float walkableNormalY = 1.0f; // cos(maximumWalkableSlope).
    float groundSnapDistance = 0.0f;
};

struct CharacterTickInput
{
    Vector3 horizontalVelocity; // Already speed-limited, y == 0.
    bool jumpRequested = false;
};

// Value-owned outcome of one fixed tick. Contacts are the solid constraints touching the
// query capsule at the accepted position, retained for later support/moving-body stages.
// No impulse, contact event or trigger event is derived from them.
struct CharacterMoveResult
{
    CharacterMoveStatus status = CharacterMoveStatus::NoMovement;
    CharacterJumpOutcome jumpOutcome = CharacterJumpOutcome::None;
    Vector3 requestedDisplacement; // Horizontal request for this tick.
    Vector3 intendedDisplacement;  // Horizontal request plus vertical velocity * dt.
    Vector3 resolvedDisplacement;
    float verticalVelocity = 0.0f; // After the tick.
    CharacterGroundState ground;   // After the tick.
    bool stepped = false;
    bool snapped = false;
    // Post-step adjustments for this fixed step (CM.8): moving-support carry, then recovery.
    Vector3 carriedDisplacement;
    Vector3 recoveryDisplacement;
    CharacterRecovery recovery = CharacterRecovery::NotNeeded;
    int outerIterations = 0;
    int solverIterations = 0;
    std::size_t peakPlaneCount = 0;
    float minimumCastFraction = 1.0f;
    std::vector<MoverPlane> contacts;
};

// Bounds shared by every move. Outer iterations each run at most Box3D's 20 solver passes.
inline constexpr int MaximumCharacterMoveIterations = 8;
inline constexpr std::size_t CharacterMovePlaneCapacity = 64;
// The final occupancy check shrinks the solid radius by this much (at most half the
// radius): it detects gross penetration while tolerating Box3D's 0.005 m linear slop.
inline constexpr float CharacterPenetrationTolerance = 0.01f;
// Ground evaluation looks this far below the capsule: twice Box3D's linear slop, so a
// capsule resting within solver tolerance of a floor is supported, but not one hovering.
inline constexpr float CharacterGroundProbeDistance = 0.01f;
// Contact normals are approximate GJK results (CM.2 measured ~0.002); walkability accepts
// normals this far below cos(maximumWalkableSlope) so a flat floor at 0 degrees is walkable.
inline constexpr float CharacterWalkableNormalTolerance = 0.001f;
// PROVISIONAL, internal and not authored: the highest static ledge a grounded controller
// may step onto, in meters. The nine authored settings have no step height; making this
// configurable is a future data-model decision (it would need a schema change).
inline constexpr float ProvisionalMaximumStepHeight = 0.3f;

// Classifies walkable support by colliding the capsule lowered by the ground probe.
// Normals with y >= walkableNormalY - tolerance are floors; the most upward normal wins,
// ties broken by lowest shape key. Walls, corners and ceilings never ground the capsule.
CharacterGroundState EvaluateCharacterGround(const CharacterQueryScene& world, Vector3 center,
    const MoverCapsule& queryCapsule, float walkableNormalY);

// One fixed tick of grounded/airborne locomotion against solid world geometry; see
// Documentation/Physics/CharacterControllerGroundMovement.md for the exact stage order. On success
// the state holds the accepted center, vertical velocity and ground; on a failure status
// the center and ground are unchanged and vertical velocity is cleared. Throws for
// nonfinite values.
CharacterMoveResult SimulateCharacterTick(const CharacterQueryScene& world, const CharacterMotionSettings& settings,
    const CharacterTickInput& input, CharacterMotionState& state);

// Post-step: moves the controller by its supporting body's translation over the native step.
// Applied directly when the carried pose is within solver tolerance and core-clear, otherwise
// as a bounded swept move; nothing is applied when neither is valid. Returns what was applied.
Vector3 CarryCharacter(const CharacterQueryScene& world, const CharacterMotionSettings& settings,
    CharacterMotionState& state, Vector3 supportDisplacement);

// Post-step: if solids (typically moving bodies) penetrate beyond solver tolerance, solves a
// push-out from the current planes. It is accepted only if no longer than the query radius
// and the result is core-clear and within tolerance; otherwise the pose is kept (Unresolved).
CharacterRecovery RecoverCharacterOverlap(const CharacterQueryScene& world, const CharacterMotionSettings& settings,
    CharacterMotionState& state, Vector3& displacement);
}
