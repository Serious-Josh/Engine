#pragma once

// VX.4B: authored voxel intent. Pure data: no voxel, asset or renderer state. See Documentation/Voxel/VoxelArchitecture.md, section 4.
//
// The numeric ranges duplicate the voxel module's (VoxelLattice.h, VoxelRegion.h) so that the scene does not depend on it;
// Engine/VoxelScene.cpp asserts they agree.

#include <cstdint>
#include <string>
#include <string_view>

// Whether an object's mesh is static voxel geometry. Absent component means Automatic.
enum class VoxelParticipation : std::uint8_t
{
    // A Static physics body participates; Dynamic and Kinematic bodies, Character Controllers and objects without physics do not.
    Automatic,
    // Participates even without physics. Ignored, with a warning, on a Dynamic or Kinematic body or a Character Controller: motion
    // wins, so a moving object is never baked as a wall.
    Static,
    // Never participates.
    Excluded
};

// How the object's closed meshes are oriented when classifying solids.
enum class VoxelOrientation : std::uint8_t
{
    // The scene policy: an inverted closed mesh not enclosed by a solid is an accidental inversion and counts as solid.
    Automatic,
    // As modelled: inverted meshes stay inverted at any depth (intentional cavities, deliberately negative volumes).
    AsAuthored,
    // Every closed mesh of the object is solid, whatever its winding.
    Outward
};

struct VoxelSettings
{
    VoxelParticipation participation = VoxelParticipation::Automatic;
    VoxelOrientation orientation = VoxelOrientation::Automatic;

    bool operator==(const VoxelSettings&) const = default;
};

// A scene's voxel bake: lattice settings and the published manifest. Absent means the scene has never been baked.
struct VoxelBakeSettings
{
    // Voxel size 2^baseExponent metres, -7..4 (0.125 m by default).
    std::int32_t baseExponent = -3;
    // Bricks per region axis: 16, 32 or 64.
    std::uint32_t regionBricks = 32;
    // Narrow band in voxels, 1..32, and coarse clamp in bricks, 1..16.
    std::uint32_t narrowBand = 4;
    std::uint32_t coarseClamp = 4;
    // "asset:<uuid>" of the current .voxelmanifest; empty until the first published bake. Switching it is what publishes a bake.
    std::string manifestReference;

    bool operator==(const VoxelBakeSettings&) const = default;
};

const char* VoxelParticipationToString(VoxelParticipation participation) noexcept;
bool TryParseVoxelParticipation(std::string_view text, VoxelParticipation& participation) noexcept;
const char* VoxelOrientationToString(VoxelOrientation orientation) noexcept;
bool TryParseVoxelOrientation(std::string_view text, VoxelOrientation& orientation) noexcept;

bool ValidateVoxelSettings(const VoxelSettings& settings, std::string* error = nullptr);
bool ValidateVoxelBakeSettings(const VoxelBakeSettings& settings, std::string* error = nullptr);
