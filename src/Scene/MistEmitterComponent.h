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

    bool enabled = true;
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

    bool operator==(const MistEmitterComponent& other) const
    {
        return enabled == other.enabled && shape == other.shape && emissionOrigin.x == other.emissionOrigin.x &&
            emissionOrigin.y == other.emissionOrigin.y && emissionOrigin.z == other.emissionOrigin.z &&
            emissionRotation.x == other.emissionRotation.x && emissionRotation.y == other.emissionRotation.y &&
            emissionRotation.z == other.emissionRotation.z && radius == other.radius && halfExtents.x == other.halfExtents.x &&
            halfExtents.y == other.halfExtents.y && halfExtents.z == other.halfExtents.z && rate == other.rate && color.x == other.color.x &&
            color.y == other.color.y && color.z == other.color.z && density == other.density;
    }
};

bool ValidateMistEmitterComponent(const MistEmitterComponent& emitter, std::string* error = nullptr);

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
};
