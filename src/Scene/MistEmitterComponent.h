#pragma once

// AIR.4F F2: an authored Mist Emitter. Pure data: no GPU or simulation state. See Documentation/Air/AirArchitecture.md, AIR.4F F2.
//
// The emitter's shape is centred on its emission origin (object-local metres, rotated with the object, scale ignored) and turned by
// `emissionRotation` (object-local Euler radians, Rz * Ry * Rx like Transform, applied after the object's rotation): a sphere of
// `radius`, or a box of `halfExtents` along the turned axes.
//
// Emission: wherever the shape covers air that the origin sees, that air's mist density rises by `rate * density` per second, in
// the emitter's colour. Per fluid piece and fixed step dt, with c the piece's free sub-cells whose centres lie in the shape (the
// origin's own sub-cell always counts) and n all its sub-cells:
//     added density = rate * density * dt * c / n,  and the piece's mist += (color * added, added)   (premultiplied).
// So the rate is a density per second within the shape, independent of the shape's size; a piece only partly inside thickens in
// proportion. A piece receives mist only if a straight path through open fluid joins the origin to its sub-cell in the shape nearest
// the origin. Solid voxel geometry is never crossed, the emitter's own object included: an origin inside solid emits nothing.
//
// AIR.4F.2b: that rule is the Legacy mode, the mode of every emitter saved before modes existed. Point mode
// (Documentation/Air/AirPointEmitters4F2b.md) instead adds a fixed quantity of mist, `emissionRate` (density x m^3 per second), whatever
// its radius: the radius spreads it over a smooth footprint around the origin, renormalized over the air the origin sees, so a small
// point is dense and a large one thin. Its temperature heats the footprint, and with `jetSpeed` > 0 it drives a jet along its emission
// +Z axis (the object's rotation, then the emission rotation) through a Moving Air Motor. `shape`, `halfExtents`, `rate` and `density`
// are Legacy-only and `emissionRate` and `jetSpeed` Point-only; every field is kept in both modes, so switching never discards values.

#include "Math/Vector3.h"

#include <cstdint>
#include <string>
#include <string_view>

// Persisted as a name (scene) and as u32 (cooked); keep values stable.
enum class MistEmitterShape : std::uint32_t
{
    Sphere = 0,
    Box = 1
};

const char* MistEmitterShapeName(MistEmitterShape shape) noexcept;
bool TryParseMistEmitterShape(std::string_view text, MistEmitterShape& shape) noexcept;

// AIR.4F.2b: persisted as a name (scene) and as u32 (cooked); keep values stable.
enum class MistEmitterMode : std::uint32_t
{
    Legacy = 0,
    Point = 1,
    // AIR.4F.2c: maintains a target density within a box (Documentation/Air/AirVolumeEmitters4F2c.md).
    Volume = 2
};

const char* MistEmitterModeName(MistEmitterMode mode) noexcept;
bool TryParseMistEmitterMode(std::string_view text, MistEmitterMode& mode) noexcept;

struct MistEmitterComponent
{
    static constexpr float MinimumSize = 0.05f;
    static constexpr float MaximumSize = 256.0f;
    // Density per second.
    static constexpr float MaximumRate = 1000.0f;
    static constexpr float MaximumDensity = 100.0f;
    // The emission origin's limit per axis, in metres, and the rotation's (one full turn), as for Air Motors.
    static constexpr float MaximumEmissionOffset = 256.0f;
    static constexpr float MaximumEmissionRotation = 6.28318548f;
    // AIR.4F F4.5.1b: the temperature's range, kelvin from the scene's ambient (the air field's supported ΔT).
    static constexpr float MinimumTemperature = -200.0f;
    static constexpr float MaximumTemperature = 1000.0f;
    // AIR.4F.2b, Point: the largest radius (it bounds the footprint's cells and its fixed-point weight sums on the GPU), the smallest
    // footprint the deposition uses (half a 0.5 m cell; a smaller radius is a smaller gizmo, not a smaller footprint), the largest
    // emission rate (density x m^3 per second) and the largest jet speed (m/s, well within a step's 8-cell backtrace).
    static constexpr float MaximumPointRadius = 2.0f;
    static constexpr float MinimumPointFootprint = 0.25f;
    static constexpr float MaximumEmissionRate = 1000.0f;
    static constexpr float MaximumJetSpeed = 30.0f;
    // AIR.4F.2d.4 Stage 1: the jet's Moving motor is a nozzle: a sphere of the jet's reach (the footprint, so the authored radius, but at
    // least JetMinimumReach), centred JetCentreOffset reaches downstream of the origin, so it drives the air the source feeds and little
    // behind it. The minimum is 1.25 cells of the default 0.5 m air cell: 0.5 m left the exit speed (5.1 to 6.0 m/s for 6) and momentum
    // (3x) dependent on where the origin sits in its cell; 0.625 m holds them within 3% and 12% (AirConcentratedPointJets4F2d4.md,
    // Stage 1). The coupling is the Moving motor's largest: at the fixed 1/60 s step the sphere's inner half reaches the jet speed each
    // step and its outer half blends by the falloff, never beyond it. Before Stage 1 the minimum was 1 m and the coupling 10/s, which
    // drove air about 1.7 m wide around a one-cell mist stream and reached only about 80% of the speed. Fixed, not authored.
    static constexpr float JetMinimumReach = 0.625f;
    static constexpr float JetCentreOffset = 0.5f;
    static constexpr float JetCoupling = 60.0f;
    static constexpr float JetFalloff = 0.5f;
    // AIR.4F.2b.1, Point: the temperature response rate's range (1/s) and default (AirPointEmitterHeating4F2b1.md).
    static constexpr float MinimumTemperatureResponseRate = 0.1f;
    static constexpr float MaximumTemperatureResponseRate = 120.0f;
    // 60/s: the source piece reaches 90% of its target within 0.2 s, so air leaving a fast jet carries most of it. The 1/s that
    // Legacy keeps heated a 4 m/s jet's stream to only 10% of its target (measured in the report).
    static constexpr float DefaultTemperatureResponseRate = 60.0f;
    // AIR.4F.2c, Volume: the limits of the target density, the replenishment rate (1/s), the fill height and edge softness (m) and
    // the temperature response (1/s; default 0.2/s, a 5 s time constant).
    static constexpr float MaximumTargetDensity = 100.0f;
    static constexpr float MaximumReplenishmentRate = 60.0f;
    static constexpr float MinimumFillHeight = 0.05f;
    static constexpr float MaximumFillHeight = 512.0f;
    static constexpr float MaximumEdgeSoftness = 64.0f;
    static constexpr float MaximumVolumeTemperatureRate = 60.0f;
    static constexpr float DefaultVolumeTemperatureRate = 0.2f;

    bool enabled = true;
    MistEmitterMode mode = MistEmitterMode::Legacy;
    MistEmitterShape shape = MistEmitterShape::Sphere;
    Vector3 emissionOrigin{0.0f, 0.0f, 0.0f};
    Vector3 emissionRotation{0.0f, 0.0f, 0.0f};
    // Sphere: its radius. Box: half its size along each turned axis. Both are kept, so switching shape never discards either.
    float radius = 0.5f;
    Vector3 halfExtents{0.5f, 0.5f, 0.5f};
    // Density per second added to air within the shape.
    float rate = 1.0f;
    // Linear RGB, each 0 to 1.
    Vector3 color{1.0f, 1.0f, 1.0f};
    // Multiplies the rate: how dense (later, how opaque) this emitter's mist is.
    float density = 1.0f;
    // AIR.4F F4.5.1b: kelvin from the scene's ambient that the air inside the shape approaches (time constant 1 s, wherever the
    // emitter would add mist, whatever its rate): positive heats, negative cools, 0 leaves the air's temperature alone.
    float temperature = 0.0f;
    // AIR.4F.2b, Point: the mist added per second (density x m^3, the integral of the added density over the air it lands in),
    // independent of the radius.
    float emissionRate = 1.0f;
    // AIR.4F.2b, Point: m/s along the emission +Z axis that air at the source is pulled toward; 0 is no jet.
    float jetSpeed = 0.0f;
    // AIR.4F.2b.1, Point: how fast air at the source approaches `temperature`, per second. Each step a piece moves toward it by
    // 1 - exp(-rate x w x dt), w its largest footprint weight (1 at the origin, 0 at the footprint's edge): bounded, never past it.
    float temperatureResponseRate = DefaultTemperatureResponseRate;
    // AIR.4F.2c, Volume: the box (`halfExtents` about the emission origin, turned by the emission rotation) holds mist toward
    // `targetDensity` x its profile. Each step a piece's density rises by (1 - exp(-replenishmentRate x dt)) x max(target - density, 0):
    // it never removes mist. The profile is 1 from the box's bottom up to `fillHeight` (along its local +Y), fading to 0 over
    // `edgeSoftness` inside the sides and below the fill height's top; the bottom is not faded, so floor fog reaches the floor.
    float targetDensity = 0.5f;
    float replenishmentRate = 1.0f;
    float fillHeight = 1.0f;
    float edgeSoftness = 0.25f;
    // AIR.4F.2c, Volume: how fast the air inside approaches `temperature`, per second (scaled by the profile); 0 temperature leaves it.
    float volumeTemperatureRate = DefaultVolumeTemperatureRate;

    bool operator==(const MistEmitterComponent& other) const
    {
        return enabled == other.enabled && mode == other.mode && emissionRate == other.emissionRate && jetSpeed == other.jetSpeed &&
            temperatureResponseRate == other.temperatureResponseRate && targetDensity == other.targetDensity &&
            replenishmentRate == other.replenishmentRate && fillHeight == other.fillHeight && edgeSoftness == other.edgeSoftness &&
            volumeTemperatureRate == other.volumeTemperatureRate &&
            shape == other.shape && emissionOrigin.x == other.emissionOrigin.x &&
            emissionOrigin.y == other.emissionOrigin.y && emissionOrigin.z == other.emissionOrigin.z &&
            emissionRotation.x == other.emissionRotation.x && emissionRotation.y == other.emissionRotation.y &&
            emissionRotation.z == other.emissionRotation.z && radius == other.radius && halfExtents.x == other.halfExtents.x &&
            halfExtents.y == other.halfExtents.y && halfExtents.z == other.halfExtents.z && rate == other.rate && color.x == other.color.x &&
            color.y == other.color.y && color.z == other.color.z && density == other.density && temperature == other.temperature;
    }
};

bool ValidateMistEmitterComponent(const MistEmitterComponent& emitter, std::string* error = nullptr);

// AIR.4F.2b, Point: the deposition footprint's radius (the radius, at least MinimumPointFootprint), and the jet's reach.
float MistEmitterPointFootprint(const MistEmitterComponent& emitter) noexcept;
float MistEmitterJetReach(const MistEmitterComponent& emitter) noexcept;

// AIR.4F.2d.4 Stage 1: whether the Inspector warns that this emitter's jet runs without the air's pressure projection: an enabled Point
// emitter with a jet while the projection is off. Without it the jet's moving air ends at the nozzle and most of its mist is lost there
// (measured: 96%).
bool MistEmitterJetNeedsProjectionWarning(const MistEmitterComponent& emitter, bool pressureProjection) noexcept;

// A stable, non-zero tag for the emitter of the object with this ID, naming it in the status read back from the GPU. Distinct from
// the same object's AirMotorTag.
std::uint64_t MistEmitterTag(std::string_view objectID) noexcept;

// Where an emitter's origin lies in the air field and how many fluid pieces it reached on its latest step (computed on the GPU,
// read back for the Inspector, a few frames old).
struct MistEmitterStatus
{
    enum class Origin : std::uint32_t
    {
        // In a fluid piece with voxel data: emits.
        Open = 0,
        // In a sub-cell no piece holds (solid voxel geometry, or a dropped piece): emits nothing.
        Solid = 1,
        // In a cell without voxel data, which air treats as free: emits.
        Unknown = 2,
        // Outside the air window: emits nothing.
        Outside = 3
    };

    Origin origin = Origin::Outside;
    // Fluid pieces that received mist on the latest step.
    std::uint32_t pieces = 0;
    // AIR.4F.2b, Point: the share of the latest step's emission that fell outside the air window and was lost (0 to 1).
    float outsideShare = 0.0f;
    // AIR.4F.2b, Point with a jet: every Air Motor slot (AirField::MaximumMotors) was taken, so the jet moved no air (mist is still
    // emitted). Jets take slots after every Air Motor component, so they never displace one.
    bool jetWithoutCapacity = false;
};
