#pragma once

// AIR.3: an authored Air Motor. Pure data: no GPU or simulation state. See Documentation/Air/AirArchitecture.md, AIR.3.
//
// The motor's pose is its object's presented world pose (position and orientation; the object's scale is ignored, as for a
// Character Controller). Local +Z is the motor's axis or forward direction. Its size is authored here, in metres.
//
// Strength is an acceleration in m/s^2 applied to the air inside the motor's region, scaled by the falloff:
//   - Directional: an oriented box of halfExtents; pushes along +Z (negative strength pulls along -Z).
//   - Omni: a sphere of radius; pushes radially outward (negative strength pulls inward).
//   - Vortex: a cylinder of radius and length along +Z; spins air about +Z (counter-clockwise looking down +Z for positive
//     strength), with an optional inward pull towards the axis.
//   - Cylinder: a cylinder of radius and length along +Z; pushes along +Z like a fan or jet.
//   - Moving: a sphere of radius around the object; pulls nearby air towards the object's own velocity at `coupling` per second.
// Falloff is the fraction of the region, from its edge inwards, over which the effect fades to zero at the edge: 0 is a hard edge,
// 1 fades all the way from the centre (or axis).
//
// AIR.4D D2 emission: every motor but Moving is placed at its emission origin, a point in the object's local space (metres, scale
// ignored). The region, its falloff and the force are all centred there, so an origin of 0 is the AIR.3 motor. Force reaches only
// air the motor's emission geometry sees through open fluid in a straight line (never through static voxel geometry, including the
// object's own): Directional, a ray from the origin along +Z to the box's +Z face; Cylinder, a disc of `radius` at the origin facing
// +Z. A Moving motor emits from its object's position.
//
// AIR.4D D2.1 Omni and Vortex emitters. Their shape is centred on the emission origin and turned by `emissionRotation` (object-local
// Euler radians, Rz * Ry * Rx like Transform, applied after the object's rotation; the object's scale is still ignored). The motor's
// local axes below are these turned axes. The centre is only a reference: it emits nothing itself.
//   - Omni: an ellipsoid with semi-axes `emissionSemiAxes` (x, y, z). Air inside it gets no force. Outside, at distance d from the
//     surface (the nearest surface point), the region is d <= `radius` (the influence distance), the falloff runs over it like any
//     region (t = d / radius), and the force points along the surface's outward normal at the nearest point. Semi-axes of exactly
//     (0, 0, 0) are a point Omni: the AIR.3 / D2 motor, pushing radially from the centre, which emits.
//   - Vortex: a planar ellipse in the local XY plane with semi-axes `emissionSemiAxes` x and y (z is unused). The region is a tube:
//     the distance d to the nearest ring point is at most `radius`, with t = d / radius. The force follows the ellipse's tangent at
//     the nearest point (counter-clockwise looking down +Z for positive strength), and `inwardPull` acts along its in-plane inward
//     normal there. `length` does not apply.
// Emission comes from the surface or ring: the nearest point first, then up to seven exposed points of a fixed lattice near it
// (Omni: 8 bands by 16 longitudes, in a frame tilted from the shape's axes; Vortex: 64 points around the ring). An Omni point
// emits only if the step from the surface 2^-10 m outward along the normal stays in open fluid; a ring point, only if it lies in
// open fluid. Solid is never skipped.

#include "Math/Vector3.h"

#include <cstdint>
#include <string>
#include <string_view>

// Persisted as a name (scene) and as u32 (cooked); keep values stable.
enum class AirMotorType : std::uint32_t
{
    Directional = 0,
    Omni = 1,
    Vortex = 2,
    Moving = 3,
    Cylinder = 4
};

const char* AirMotorTypeName(AirMotorType type) noexcept;
bool TryParseAirMotorType(std::string_view text, AirMotorType& type) noexcept;

struct AirMotorComponent
{
    // Value limits. Strength and coupling are bounded so that no setting can push air faster than the solver tolerates; the
    // solver also caps the speed of air a motor touches (AirSolverSettings::maximumMotorSpeed).
    static constexpr float MaximumStrength = 1000.0f;
    static constexpr float MinimumSize = 0.05f;
    static constexpr float MaximumSize = 256.0f;
    static constexpr float MaximumInwardPull = 1000.0f;
    static constexpr float MaximumCoupling = 60.0f;
    // The emission origin's limit per axis, in metres.
    static constexpr float MaximumEmissionOffset = 256.0f;
    // The emission rotation's limit per axis, in radians (one full turn).
    static constexpr float MaximumEmissionRotation = 6.28318548f;

    bool enabled = true;
    AirMotorType type = AirMotorType::Directional;
    // m/s^2; the sign selects direction (see above). Not used by Moving motors.
    float strength = 20.0f;
    float falloff = 0.5f;
    // Directional: half the box's size along local X, Y, Z.
    Vector3 halfExtents{1.0f, 1.0f, 1.0f};
    // Moving, Cylinder: the region's radius. Omni, Vortex (AIR.4D D2.1): the influence distance beyond the emission surface or ring
    // (for a point Omni, from its centre).
    float radius = 2.0f;
    // Cylinder: the full length along local +Z, centred on the object. Not used by a Vortex since AIR.4D D2.1.
    float length = 4.0f;
    // Vortex: m/s^2 towards the axis (0 for none).
    float inwardPull = 0.0f;
    // Moving: per second; how fast air within the radius approaches the object's velocity.
    float coupling = 8.0f;
    // AIR.4D D2, every type but Moving: the emission origin in the object's local space (see above).
    Vector3 emissionOrigin{0.0f, 0.0f, 0.0f};
    // AIR.4D D2.1, Omni and Vortex: the ellipsoid's semi-axes (Omni; all 0 for a point Omni) or the ring's (Vortex, x and y), in
    // metres. Kept for every type, so switching type never discards them.
    Vector3 emissionSemiAxes{1.0f, 1.0f, 1.0f};
    // AIR.4D D2.1, Omni and Vortex: the shape's rotation about its centre, object-local Euler radians (see above).
    Vector3 emissionRotation{0.0f, 0.0f, 0.0f};

    bool operator==(const AirMotorComponent& other) const
    {
        return enabled == other.enabled && type == other.type && strength == other.strength && falloff == other.falloff &&
            halfExtents.x == other.halfExtents.x && halfExtents.y == other.halfExtents.y && halfExtents.z == other.halfExtents.z &&
            radius == other.radius && length == other.length && inwardPull == other.inwardPull && coupling == other.coupling &&
            emissionOrigin.x == other.emissionOrigin.x && emissionOrigin.y == other.emissionOrigin.y &&
            emissionOrigin.z == other.emissionOrigin.z && emissionSemiAxes.x == other.emissionSemiAxes.x &&
            emissionSemiAxes.y == other.emissionSemiAxes.y && emissionSemiAxes.z == other.emissionSemiAxes.z &&
            emissionRotation.x == other.emissionRotation.x && emissionRotation.y == other.emissionRotation.y &&
            emissionRotation.z == other.emissionRotation.z;
    }
};

bool ValidateAirMotorComponent(const AirMotorComponent& motor, std::string* error = nullptr);

// AIR.4D D2.1: whether an Omni's semi-axes are exactly (0, 0, 0), the point Omni of AIR.3 and D2.
bool IsPointOmni(const AirMotorComponent& motor) noexcept;

// AIR.4D D2.1 migration of a motor read from scene schema 20/21 or cooked payload 25/26, which had an emission radius (0 before D2)
// instead of the emission shape. A point Omni (radius 0) keeps its exact behaviour: semi-axes (0, 0, 0). Any other Omni, and every
// Vortex, cannot be expressed as a surface or ring without inventing its size: it takes placeholder semi-axes (0 for an Omni, the
// component default for a Vortex) and is disabled, and the function returns true so the loader can warn that it needs manual
// reconfiguration. Other types take the defaults, which they do not use.
bool MigrateLegacyAirMotorEmission(AirMotorComponent& motor, float legacyEmissionRadius);

// Warns, naming the object, that its migrated motor was disabled and needs its emission shape set by hand. `source` names what was
// read ("schema 21", "cooked payload 26").
void LogAirMotorReconfiguration(std::string_view objectName, std::string_view objectID, AirMotorType type, std::string_view source);

// AIR.4D D2: a stable, non-zero tag for the motor of the object with this ID (AirMotorInstance::tag), naming it in emission status
// read back from the GPU frames later.
std::uint64_t AirMotorTag(std::string_view objectID) noexcept;

// AIR.4D D2: where a motor's emission points lie in the air field, as the solver sees them (computed on the GPU, read back for the
// Inspector). The points are the emission geometry's fixed samples: points spread over the ray or disc; for an Omni ellipsoid or a
// Vortex ring (AIR.4D D2.1), its whole lattice (128 or 64 points); for a point Omni or a Moving motor, the one emitting point.
// `origin` is where the centre lies; for an Omni ellipsoid or a Vortex ring it is only a reference and emits nothing.
struct AirMotorEmissionStatus
{
    enum class Point : std::uint32_t
    {
        // In a fluid piece: emits.
        Open = 0,
        // In a sub-cell no piece holds (solid, or a dropped piece): emits nothing.
        Blocked = 1,
        // In a cell without voxel data: free air to the solver.
        Unknown = 2,
        // Outside the air window: free air to the solver.
        Outside = 3
    };

    Point origin = Point::Open;
    std::uint32_t samples = 0;
    std::uint32_t blocked = 0;
    std::uint32_t unknown = 0;
};
