#include "Physics/CharacterControllerMover.h"

#include <box3d/collision.h>
#include <box3d/constants.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace Physics
{
namespace
{
bool IsFinite(const Vector3& vector)
{
    return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
}

float LengthSquared(const Vector3& vector)
{
    return vector.x * vector.x + vector.y * vector.y + vector.z * vector.z;
}

bool IsWalkable(const Vector3& normal, float walkableNormalY)
{
    return normal.y >= walkableNormalY - CharacterWalkableNormalTolerance;
}

// Normals within about 5.7 degrees of vertical (floors, ceilings) carry no usable
// horizontal constraint; vertical passes and the final occupancy check cover them.
constexpr float MinimumHorizontalNormalLength = 0.1f;
constexpr float NegligibleAdvanceSquared = 1.0e-10f;
constexpr float DisplacementTolerance = 1.0e-6f;

enum class PassAxis
{
    // Horizontal request. Non-walkable planes are restated horizontally (they act as
    // walls and can never lift the capsule). Only while supported do walkable planes stay
    // fully 3D, so the controller follows walkable slopes, and ceilings stay fully 3D, so
    // that slope lift is still bounded by overhead geometry.
    Horizontal,
    // Vertical gravity/jump displacement against full 3D planes: floors stop descent,
    // ceilings stop ascent, steep slopes slide the capsule outward.
    Vertical
};

// Rigid planes for one solver pass. b3SolvePlanes enforces dot(n, d) >= offset - slop. For
// a horizontal d that equals dot(nh, d) >= offset - slop (nh: horizontal part of n, length
// L), i.e. the unit plane nh / L with offset' = (offset - slop) / L + slop (CM.6).
std::vector<b3CollisionPlane> MakeRigidPlanes(const std::vector<MoverPlane>& planes, PassAxis axis,
    bool supported, float walkableNormalY, std::vector<bool>& restated)
{
    const float slop = B3_LINEAR_SLOP;
    std::vector<b3CollisionPlane> rigidPlanes;
    rigidPlanes.reserve(planes.size());
    restated.clear();

    for(const auto& plane : planes)
    {
        // While supported, walkable planes may lift the capsule along a slope, so downward-
        // facing (ceiling) planes must stay fully 3D too; restating them would drop them.
        const bool keepFull = axis == PassAxis::Vertical ||
            (supported && (IsWalkable(plane.normal, walkableNormalY) || plane.normal.y < 0.0f));

        // FLT_MAX push limit: solids are rigid; clipping keeps slides tangent.
        if(keepFull)
        {
            rigidPlanes.push_back({{{plane.normal.x, plane.normal.y, plane.normal.z}, plane.offset}, FLT_MAX, 0.0f, true});
            restated.push_back(false);
            continue;
        }

        const float horizontalLength = std::hypot(plane.normal.x, plane.normal.z);

        if(horizontalLength < MinimumHorizontalNormalLength)
        {
            continue;
        }

        const b3Vec3 normal{plane.normal.x / horizontalLength, 0.0f, plane.normal.z / horizontalLength};
        const float offset = (plane.offset - slop) / horizontalLength + slop;
        rigidPlanes.push_back({{normal, offset}, FLT_MAX, 0.0f, true});
        restated.push_back(true);
    }

    return rigidPlanes;
}

struct PassResult
{
    Vector3 position;
    bool capacityExceeded = false;
    // A restated (wall-like) plane pushed back on the request: a step may help.
    bool blockedByWall = false;
};

// Bounded collide/solve/cast iterations toward start + displacement. After each advance the
// target is re-levelled on the axis the pass does not control, so slope climbing is not
// pulled back to the start height and steep-slope sliding is not pulled back sideways.
PassResult SolvePass(const CharacterQueryScene& world, const CharacterMotionSettings& settings, Vector3 start,
    Vector3 displacement, PassAxis axis, bool supported, CharacterMoveResult& statistics)
{
    PassResult pass{start};

    if(LengthSquared(displacement) == 0.0f)
    {
        return pass;
    }

    Vector3 target = start + displacement;
    std::vector<bool> restated;

    for(int iteration = 0; iteration < MaximumCharacterMoveIterations; ++iteration)
    {
        const MoverPlaneQuery query = world.CollideSolidMover(pass.position, settings.queryCapsule,
            CharacterMovePlaneCapacity);

        if(query.overflowCount != 0)
        {
            // A truncated constraint set cannot be made safe by solving what remains.
            pass.capacityExceeded = true;
            return pass;
        }

        statistics.peakPlaneCount = (std::max)(statistics.peakPlaneCount, query.planes.size());
        auto rigidPlanes = MakeRigidPlanes(query.planes, axis, supported, settings.walkableNormalY, restated);
        const Vector3 remaining = target - pass.position;
        const b3PlaneSolverResult solution = b3SolvePlanes({remaining.x, remaining.y, remaining.z},
            rigidPlanes.data(), static_cast<int>(rigidPlanes.size()));
        statistics.solverIterations += solution.iterationCount;
        ++statistics.outerIterations;

        for(std::size_t index = 0; index < rigidPlanes.size(); ++index)
        {
            pass.blockedByWall = pass.blockedByWall || (restated[index] && rigidPlanes[index].push > 0.0f);
        }

        Vector3 delta{solution.delta.x, solution.delta.y, solution.delta.z};

        if(axis == PassAxis::Horizontal && !supported)
        {
            // Every plane is restated, so the solution is horizontal; keep y exactly zero.
            delta.y = 0.0f;
        }

        if(!IsFinite(delta))
        {
            throw std::runtime_error("Character plane solver produced a nonfinite displacement");
        }

        if(LengthSquared(delta) < NegligibleAdvanceSquared)
        {
            break;
        }

        const float fraction = world.CastSolidMover(pass.position, settings.queryCapsule, delta);
        statistics.minimumCastFraction = (std::min)(statistics.minimumCastFraction, fraction);
        const Vector3 advance = delta * fraction;
        pass.position = pass.position + advance;

        if(axis == PassAxis::Horizontal)
        {
            target.y = pass.position.y;
        }
        else
        {
            target.x = pass.position.x;
            target.z = pass.position.z;
        }

        if(LengthSquared(advance) < NegligibleAdvanceSquared)
        {
            break;
        }
    }

    return pass;
}

bool OverlapsCore(const CharacterQueryScene& world, const CharacterMotionSettings& settings, Vector3 center)
{
    const float shrink = (std::min)(CharacterPenetrationTolerance, settings.solidRadius * 0.5f);
    const MoverCapsule core{settings.queryCapsule.lowerCenter, settings.queryCapsule.upperCenter,
        settings.solidRadius - shrink};
    return world.CountSolidOverlaps(center, core) != 0;
}

// Steps must end within Box3D's own solver tolerance of every plane. Casts ignore initial
// overlap, so a lift starting in contact could otherwise creep into a ceiling.
bool WithinSolverSlop(const CharacterQueryScene& world, const CharacterMotionSettings& settings, Vector3 center)
{
    const MoverPlaneQuery query = world.CollideSolidMover(center, settings.queryCapsule, CharacterMovePlaneCapacity);

    if(query.overflowCount != 0)
    {
        return false;
    }

    constexpr float roundoff = 1.0e-4f;

    for(const auto& plane : query.planes)
    {
        if(plane.offset > B3_LINEAR_SLOP + roundoff)
        {
            return false;
        }
    }

    return true;
}

// Horizontal progress along the requested direction.
float Progress(Vector3 start, Vector3 position, Vector3 horizontal)
{
    return (position.x - start.x) * horizontal.x + (position.z - start.z) * horizontal.z;
}

// Up, forward, down step attempt from a supported start. Returns the landed center only
// when overhead and forward clearance exist, the landing is walkable within the provisional
// step height, and the landed capsule does not overlap solids.
std::optional<Vector3> TryStep(const CharacterQueryScene& world, const CharacterMotionSettings& settings, Vector3 start,
    Vector3 horizontal, CharacterMoveResult& statistics)
{
    // Up: overhead clearance limits the lift.
    const float upFraction = world.CastSolidMover(start, settings.queryCapsule, {0.0f, ProvisionalMaximumStepHeight, 0.0f});
    const float lift = ProvisionalMaximumStepHeight * upFraction;

    if(lift <= CharacterGroundProbeDistance || !WithinSolverSlop(world, settings, start + Vector3{0.0f, lift, 0.0f}))
    {
        return std::nullopt;
    }

    // Forward: the same horizontal request from the raised position.
    const PassResult forward = SolvePass(world, settings, start + Vector3{0.0f, lift, 0.0f}, horizontal,
        PassAxis::Horizontal, true, statistics);

    if(forward.capacityExceeded)
    {
        return std::nullopt;
    }

    // Down: a landing must exist within the lift (plus probe); otherwise this is a gap.
    const float drop = lift + CharacterGroundProbeDistance;
    const float downFraction = world.CastSolidMover(forward.position, settings.queryCapsule, {0.0f, -drop, 0.0f});

    if(downFraction >= 1.0f)
    {
        return std::nullopt;
    }

    const Vector3 landed = forward.position + Vector3{0.0f, -drop * downFraction, 0.0f};
    const CharacterGroundState landing = EvaluateCharacterGround(world, landed, settings.queryCapsule,
        settings.walkableNormalY);

    // The obstacle height is the landing support point above the starting foot, not the
    // lift: a capsule's rounded base can rest on an edge up to a radius above its lift.
    const float startFootY = start.y + settings.queryCapsule.lowerCenter.y - settings.queryCapsule.radius;
    const float supportRise = landing.point.y - startFootY;

    const bool landingTooHigh = supportRise > ProvisionalMaximumStepHeight + CharacterGroundProbeDistance;

    if(!landing.grounded || landed.y < start.y || landingTooHigh ||
        OverlapsCore(world, settings, landed) || !WithinSolverSlop(world, settings, landed))
    {
        return std::nullopt;
    }

    return landed;
}

CharacterMoveResult Reject(CharacterMoveResult result, CharacterMoveStatus status, CharacterMotionState& state)
{
    // Conservative: keep the last accepted pose and support; clear vertical velocity so a
    // repeatedly rejected fall cannot accumulate unbounded speed.
    result.status = status;
    result.resolvedDisplacement = {};
    result.contacts.clear();
    state.verticalVelocity = 0.0f;
    result.verticalVelocity = 0.0f;
    result.ground = state.ground;
    return result;
}
}

namespace
{
// A capsule on a ledge touches the edge, whose closest-point normal is diagonal, rather
// than the walkable face beside it. This probes that face directly: a tiny sphere just
// inside the edge (away from the capsule) is cast down onto the same shape, and the face
// normal there decides support. All probe sizes derive from the ground probe distance.
std::optional<Vector3> FindWalkableFaceBelowEdge(const CharacterQueryScene& world, const MoverPlane& edge, Vector3 edgePoint,
    float walkableNormalY)
{
    const float horizontalLength = std::hypot(edge.normal.x, edge.normal.z);

    if(horizontalLength < MinimumHorizontalNormalLength)
    {
        return std::nullopt;
    }

    const float probeRadius = CharacterGroundProbeDistance;
    const Vector3 inward{-edge.normal.x / horizontalLength, 0.0f, -edge.normal.z / horizontalLength};
    const MoverCapsule probe{{0.0f, -probeRadius * 0.5f, 0.0f}, {0.0f, probeRadius * 0.5f, 0.0f}, probeRadius};
    const Vector3 start = edgePoint + inward * (2.0f * probeRadius) + Vector3{0.0f, 3.0f * probeRadius, 0.0f};
    const Vector3 drop{0.0f, -6.0f * probeRadius, 0.0f};

    // A probe starting inside a solid means the contact was a side face, not an edge.
    if(world.CountSolidOverlaps(start, probe) != 0)
    {
        return std::nullopt;
    }

    const float fraction = world.CastSolidMover(start, probe, drop);

    if(fraction >= 1.0f)
    {
        return std::nullopt;
    }

    const MoverPlaneQuery face = world.CollideSolidMover(start + drop * fraction, probe, CharacterMovePlaneCapacity);

    if(face.overflowCount != 0)
    {
        return std::nullopt;
    }

    for(const auto& plane : face.planes)
    {
        if(plane.shapeKey == edge.shapeKey && IsWalkable(plane.normal, walkableNormalY))
        {
            return plane.normal;
        }
    }

    return std::nullopt;
}
}

CharacterGroundState EvaluateCharacterGround(const CharacterQueryScene& world, Vector3 center,
    const MoverCapsule& queryCapsule, float walkableNormalY)
{
    CharacterGroundState ground;
    const Vector3 probeCenter = center - Vector3{0.0f, CharacterGroundProbeDistance, 0.0f};
    const MoverPlaneQuery query = world.CollideSolidMover(probeCenter, queryCapsule, CharacterMovePlaneCapacity);

    // A truncated set cannot prove support; report airborne and let the movement passes fail.
    if(query.overflowCount != 0)
    {
        return ground;
    }

    // Plane points are relative to the query origin. Only contacts below the lower
    // hemisphere center can support the capsule; an edge candidate's normal must also point
    // clearly upward (side faces touch at the equator with a near-zero normal y).

    // Planes arrive sorted by shape key, so the first of equal normals has the lowest key.
    for(const auto& plane : query.planes)
    {
        std::optional<Vector3> supportNormal;

        if(IsWalkable(plane.normal, walkableNormalY))
        {
            supportNormal = plane.normal;
        }
        else if(plane.point.y < queryCapsule.lowerCenter.y && plane.normal.y >= MinimumHorizontalNormalLength)
        {
            supportNormal = FindWalkableFaceBelowEdge(world, plane, probeCenter + plane.point, walkableNormalY);
        }

        if(supportNormal && (!ground.grounded || supportNormal->y > ground.normal.y))
        {
            ground = {true, *supportNormal, plane.shapeKey, probeCenter + plane.point};
        }
    }

    return ground;
}

CharacterMoveResult SimulateCharacterTick(const CharacterQueryScene& world, const CharacterMotionSettings& settings,
    const CharacterTickInput& input, CharacterMotionState& state)
{
    if(!IsFinite(state.center) || !std::isfinite(state.verticalVelocity) || !IsFinite(input.horizontalVelocity) ||
        input.horizontalVelocity.y != 0.0f || !std::isfinite(settings.solidRadius) || settings.solidRadius <= 0.0f ||
        settings.solidRadius > settings.queryCapsule.radius)
    {
        throw std::invalid_argument("Character tick requires finite state, horizontal velocity and valid radii");
    }

    constexpr float timeStep = World::FixedTimeStep;
    const Vector3 start = state.center;
    const bool startGrounded = state.ground.grounded;
    CharacterMoveResult result;
    float verticalVelocity = state.verticalVelocity;

    // Stage 1: jump acceptance from the ground state accepted at the end of the previous tick.
    bool leavingGround = false;

    if(input.jumpRequested)
    {
        if(!startGrounded)
        {
            result.jumpOutcome = CharacterJumpOutcome::IgnoredAirborne;
        }
        else if(settings.jumpSpeed <= 0.0f)
        {
            result.jumpOutcome = CharacterJumpOutcome::IgnoredZeroSpeed;
        }
        else
        {
            result.jumpOutcome = CharacterJumpOutcome::Accepted;
            verticalVelocity = settings.jumpSpeed;
            leavingGround = true;
        }
    }

    // Stage 2: gravity. A supported controller has no vertical velocity: integrating gravity
    // into walkable support would slide it down slopes and accumulate speed while standing.
    // Support is kept by the ground probe and snap instead. Otherwise semi-implicit Euler:
    // velocity is updated before the displacement is computed from it.
    const bool supported = startGrounded && !leavingGround;

    if(supported)
    {
        verticalVelocity = 0.0f;
    }
    else
    {
        verticalVelocity += settings.verticalGravity * timeStep;
    }

    if(!std::isfinite(verticalVelocity))
    {
        throw std::runtime_error("Character vertical velocity became nonfinite");
    }

    const Vector3 horizontal = input.horizontalVelocity * timeStep;
    const float vertical = verticalVelocity * timeStep;
    result.requestedDisplacement = horizontal;
    result.intendedDisplacement = {horizontal.x, vertical, horizontal.z};

    // Stage 3: horizontal pass.
    const PassResult plain = SolvePass(world, settings, start, horizontal, PassAxis::Horizontal, supported, result);

    if(plain.capacityExceeded)
    {
        return Reject(std::move(result), CharacterMoveStatus::QueryCapacityExceeded, state);
    }

    Vector3 position = plain.position;

    // Stage 4: step attempt, only when supported and a wall-like plane cut the request short.
    const float requestedProgress = LengthSquared(horizontal);
    constexpr float stepProgressMargin = 1.0e-4f;

    if(supported && plain.blockedByWall && Progress(start, position, horizontal) < requestedProgress - stepProgressMargin)
    {
        const auto stepped = TryStep(world, settings, start, horizontal, result);

        if(stepped && Progress(start, *stepped, horizontal) > Progress(start, position, horizontal) + stepProgressMargin)
        {
            position = *stepped;
            result.stepped = true;
        }
    }

    // Stage 5: vertical pass for gravity/jump displacement.
    const Vector3 beforeVertical = position;
    const PassResult verticalPass = SolvePass(world, settings, position, {0.0f, vertical, 0.0f}, PassAxis::Vertical,
        supported, result);

    if(verticalPass.capacityExceeded)
    {
        return Reject(std::move(result), CharacterMoveStatus::QueryCapacityExceeded, state);
    }

    position = verticalPass.position;
    const float resolvedVertical = position.y - beforeVertical.y;

    // Stage 6: ground snap, only for a supported controller that is not moving upward.
    if(supported && verticalVelocity <= 0.0f && settings.groundSnapDistance > 0.0f &&
        !EvaluateCharacterGround(world, position, settings.queryCapsule, settings.walkableNormalY).grounded)
    {
        const Vector3 snap{0.0f, -settings.groundSnapDistance, 0.0f};
        const float fraction = world.CastSolidMover(position, settings.queryCapsule, snap);

        if(fraction < 1.0f)
        {
            const Vector3 candidate = position + snap * fraction;

            // Only walkable support may hold the snap; steep surfaces and gaps leave it airborne.
            if(EvaluateCharacterGround(world, candidate, settings.queryCapsule, settings.walkableNormalY).grounded)
            {
                position = candidate;
                result.snapped = true;
            }
        }
    }

    // Stage 7: independent occupancy check. Planes alone are not proof of separation.
    if(!IsFinite(position) || OverlapsCore(world, settings, position))
    {
        return Reject(std::move(result), CharacterMoveStatus::PenetrationRejected, state);
    }

    // Stage 8: ground and vertical velocity at the accepted position.
    const CharacterGroundState ground = EvaluateCharacterGround(world, position, settings.queryCapsule,
        settings.walkableNormalY);

    if(std::abs(resolvedVertical - vertical) > DisplacementTolerance)
    {
        // Floors, ceilings and steep slopes absorbed part of the motion: keep only what happened.
        verticalVelocity = resolvedVertical / timeStep;
    }

    if(result.snapped || (ground.grounded && verticalVelocity <= 0.0f))
    {
        verticalVelocity = 0.0f;
    }

    MoverPlaneQuery contacts = world.CollideSolidMover(position, settings.queryCapsule, CharacterMovePlaneCapacity);

    if(contacts.overflowCount != 0)
    {
        return Reject(std::move(result), CharacterMoveStatus::QueryCapacityExceeded, state);
    }

    // Stage 9: accept.
    result.contacts = std::move(contacts.planes);
    result.resolvedDisplacement = position - start;
    result.verticalVelocity = verticalVelocity;
    result.ground = ground;
    state.center = position;
    state.verticalVelocity = verticalVelocity;
    state.ground = ground;

    const Vector3 shortfall = result.intendedDisplacement - result.resolvedDisplacement;
    const bool nothingIntended = LengthSquared(result.intendedDisplacement) == 0.0f;
    const bool nothingMoved = LengthSquared(result.resolvedDisplacement) == 0.0f;

    if(nothingIntended && nothingMoved)
    {
        result.status = CharacterMoveStatus::NoMovement;
    }
    else if(!result.stepped && !result.snapped && LengthSquared(shortfall) <= 1.0e-12f)
    {
        result.status = CharacterMoveStatus::Unobstructed;
    }
    else
    {
        result.status = CharacterMoveStatus::Constrained;
    }

    return result;
}
}

namespace Physics
{
namespace
{
// Closest points between two upright capsule axes (both segments vertical, endpoints at
// x = z = 0 relative to their centers), returned as world points.
std::pair<Vector3, Vector3> ClosestAxisPoints(Vector3 selfCenter, const MoverCapsule& self, Vector3 otherCenter,
    const MoverCapsule& other)
{
    const float selfBottom = selfCenter.y + self.lowerCenter.y;
    const float selfTop = selfCenter.y + self.upperCenter.y;
    const float otherBottom = otherCenter.y + other.lowerCenter.y;
    const float otherTop = otherCenter.y + other.upperCenter.y;
    float selfY = 0.0f;
    float otherY = 0.0f;

    if(selfTop < otherBottom)
    {
        selfY = selfTop;
        otherY = otherBottom;
    }
    else if(otherTop < selfBottom)
    {
        selfY = selfBottom;
        otherY = otherTop;
    }
    else
    {
        // Overlapping vertical ranges: the midpoint of the shared range is a closest pair.
        selfY = ((std::max)(selfBottom, otherBottom) + (std::min)(selfTop, otherTop)) * 0.5f;
        otherY = selfY;
    }

    return {{selfCenter.x, selfY, selfCenter.z}, {otherCenter.x, otherY, otherCenter.z}};
}

// Earliest fraction in [0, 1] at which a point moving from start by translation reaches
// distance radius from the vertical segment (x, z) between bottomY and topY.
float SweepPointAgainstUprightCapsule(Vector3 start, Vector3 translation, float x, float z, float bottomY,
    float topY, float radius)
{
    float earliest = 1.0f;
    const auto consider = [&earliest](float t)
    {
        if(t >= 0.0f && t < earliest)
        {
            earliest = t;
        }
    };

    // Side of the cylinder: horizontal quadratic, valid only within the axis range.
    const float dx = start.x - x;
    const float dz = start.z - z;
    const float a = translation.x * translation.x + translation.z * translation.z;

    if(a > 1.0e-12f)
    {
        const float b = 2.0f * (dx * translation.x + dz * translation.z);
        const float c = dx * dx + dz * dz - radius * radius;
        const float discriminant = b * b - 4.0f * a * c;

        if(discriminant >= 0.0f)
        {
            const float t = (-b - std::sqrt(discriminant)) / (2.0f * a);
            const float y = start.y + translation.y * t;

            if(y >= bottomY && y <= topY)
            {
                consider(t);
            }
        }
    }

    // Hemisphere ends.
    for(const float endY : {bottomY, topY})
    {
        const Vector3 offset{start.x - x, start.y - endY, start.z - z};
        const float sphereA = LengthSquared(translation);
        const float sphereB = 2.0f * (offset.x * translation.x + offset.y * translation.y + offset.z * translation.z);
        const float sphereC = LengthSquared(offset) - radius * radius;
        const float discriminant = sphereB * sphereB - 4.0f * sphereA * sphereC;

        if(sphereA > 1.0e-12f && discriminant >= 0.0f)
        {
            consider((-sphereB - std::sqrt(discriminant)) / (2.0f * sphereA));
        }
    }

    return earliest;
}

bool PlanePrecedes(const MoverPlane& left, const MoverPlane& right)
{
    return std::tuple(left.shapeKey, left.normal.x, left.normal.y, left.normal.z, left.offset) <
        std::tuple(right.shapeKey, right.normal.x, right.normal.y, right.normal.z, right.offset);
}

bool IsValidRestingPose(const CharacterQueryScene& world, const CharacterMotionSettings& settings, Vector3 center)
{
    return IsFinite(center) && !OverlapsCore(world, settings, center) && WithinSolverSlop(world, settings, center);
}
}

std::size_t CharacterQueryScene::CountSolidOverlaps(Vector3 origin, const MoverCapsule& capsule) const
{
    std::size_t count = world.CountSolidOverlaps(origin, capsule);

    for(const auto& obstacle : obstacles)
    {
        const auto [self, other] = ClosestAxisPoints(origin, capsule, obstacle.center, obstacle.solidCapsule);
        const float reach = capsule.radius + obstacle.solidCapsule.radius;

        if(LengthSquared(self - other) < reach * reach)
        {
            ++count;
        }
    }

    return count;
}

MoverPlaneQuery CharacterQueryScene::CollideSolidMover(Vector3 origin, const MoverCapsule& capsule,
    std::size_t capacity) const
{
    MoverPlaneQuery query = world.CollideSolidMover(origin, capsule, capacity);

    for(const auto& obstacle : obstacles)
    {
        const auto [self, other] = ClosestAxisPoints(origin, capsule, obstacle.center, obstacle.solidCapsule);
        const Vector3 separation = self - other;
        const float distance = std::sqrt(LengthSquared(separation));
        const float reach = capsule.radius + obstacle.solidCapsule.radius;

        // Like native mover planes, report contacts within the solver slop of touching.
        if(distance >= reach + B3_LINEAR_SLOP)
        {
            continue;
        }

        if(query.planes.size() == capacity)
        {
            ++query.overflowCount;
            continue;
        }

        // Coincident axes have no defined direction; use +x deterministically.
        const Vector3 normal = distance > 1.0e-6f ? separation * (1.0f / distance) : Vector3{1.0f, 0.0f, 0.0f};
        const Vector3 contact = other + normal * obstacle.solidCapsule.radius;
        query.planes.push_back({0, normal, reach - distance, contact - origin});
    }

    std::sort(query.planes.begin(), query.planes.end(), PlanePrecedes);
    return query;
}

float CharacterQueryScene::CastSolidMover(Vector3 origin, const MoverCapsule& capsule, Vector3 translation) const
{
    float fraction = world.CastSolidMover(origin, capsule, translation);

    for(const auto& obstacle : obstacles)
    {
        // Two upright capsules touch when the moving center reaches the Minkowski capsule:
        // the other axis extended by this capsule's half-axis, with the summed radius.
        const float reach = capsule.radius + obstacle.solidCapsule.radius;
        const float bottomY = obstacle.center.y + obstacle.solidCapsule.lowerCenter.y - capsule.upperCenter.y;
        const float topY = obstacle.center.y + obstacle.solidCapsule.upperCenter.y - capsule.lowerCenter.y;
        const auto [self, other] = ClosestAxisPoints(origin, capsule, obstacle.center, obstacle.solidCapsule);
        const Vector3 separation = self - other;

        if(LengthSquared(separation) < reach * reach)
        {
            // Already touching: allow separating motion, block anything closing the gap.
            const float closing = separation.x * translation.x + separation.y * translation.y +
                separation.z * translation.z;

            if(closing < 0.0f)
            {
                fraction = 0.0f;
            }

            continue;
        }

        fraction = (std::min)(fraction, SweepPointAgainstUprightCapsule(origin, translation, obstacle.center.x,
            obstacle.center.z, bottomY, topY, reach));
    }

    return fraction;
}

Vector3 CarryCharacter(const CharacterQueryScene& world, const CharacterMotionSettings& settings,
    CharacterMotionState& state, Vector3 supportDisplacement)
{
    if(!IsFinite(supportDisplacement))
    {
        throw std::invalid_argument("Moving support displacement must be finite");
    }

    if(LengthSquared(supportDisplacement) == 0.0f)
    {
        return {};
    }

    // Direct carry: the controller keeps its pose relative to the support.
    const Vector3 carried = state.center + supportDisplacement;

    if(IsValidRestingPose(world, settings, carried))
    {
        state.center = carried;
        return supportDisplacement;
    }

    // Blocked carry (e.g. the support moves the controller toward a wall): bounded swept move.
    CharacterMoveResult statistics;
    const PassResult swept = SolvePass(world, settings, state.center, supportDisplacement, PassAxis::Vertical, true,
        statistics);

    if(!swept.capacityExceeded && IsValidRestingPose(world, settings, swept.position))
    {
        const Vector3 applied = swept.position - state.center;
        state.center = swept.position;
        return applied;
    }

    return {};
}

CharacterRecovery RecoverCharacterOverlap(const CharacterQueryScene& world, const CharacterMotionSettings& settings,
    CharacterMotionState& state, Vector3& displacement)
{
    displacement = {};
    const MoverPlaneQuery query = world.CollideSolidMover(state.center, settings.queryCapsule,
        CharacterMovePlaneCapacity);

    if(query.overflowCount != 0)
    {
        return CharacterRecovery::Unresolved;
    }

    constexpr float roundoff = 1.0e-4f;
    const bool penetrating = std::any_of(query.planes.begin(), query.planes.end(), [](const MoverPlane& plane)
    {
        return plane.offset > B3_LINEAR_SLOP + roundoff;
    });

    if(!penetrating)
    {
        return CharacterRecovery::NotNeeded;
    }

    // Zero target: the solver returns the push that restores every plane to its tolerance.
    std::vector<bool> restated;
    auto rigidPlanes = MakeRigidPlanes(query.planes, PassAxis::Vertical, true, settings.walkableNormalY, restated);
    const b3PlaneSolverResult solution = b3SolvePlanes({0.0f, 0.0f, 0.0f}, rigidPlanes.data(),
        static_cast<int>(rigidPlanes.size()));
    const Vector3 push{solution.delta.x, solution.delta.y, solution.delta.z};
    const float maximumPush = settings.queryCapsule.radius;

    if(!IsFinite(push) || LengthSquared(push) > maximumPush * maximumPush)
    {
        return CharacterRecovery::Unresolved;
    }

    const Vector3 recovered = state.center + push;

    if(!IsValidRestingPose(world, settings, recovered))
    {
        return CharacterRecovery::Unresolved;
    }

    state.center = recovered;
    displacement = push;
    return CharacterRecovery::Recovered;
}
}
