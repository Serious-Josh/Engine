#pragma once

#include "Math/Matrix4.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// VX.4C.2 Editor voxel overlay: the GPU layout shared by the CPU builder (Engine/VoxelVisualization) and EditorVoxel.hlsl.
// Keep every constant and bit layout here identical to the HLSL. The Renderer knows nothing about voxel formats; it draws the
// cubes and boxes it is given, in lattice units, through one lattice-to-world matrix.
namespace EditorVoxelOverlay
{
// Hard limits: a build stops at MaximumCells cubes and reports truncation; at most MaximumRegions boxes.
inline constexpr std::size_t MaximumCells = 4'000'000;
inline constexpr std::size_t MaximumRegions = 65'536;

// One cube: a voxel, a whole brick or a whole region.
//   word0: region slot (bits 0-15), exposed-face mask (bits 16-21: -X, +X, -Y, +Y, -Z, +Z), class (bit 22), scale (bits 23-24)
//   word1: the cube's minimum corner in voxels inside its region: x (bits 0-9), y (bits 10-19), z (bits 20-29)
struct Cell
{
    std::uint32_t word0 = 0;
    std::uint32_t word1 = 0;
};

static_assert(sizeof(Cell) == 8);

enum class CellClass : std::uint32_t
{
    // Surface-occupied: the region's occupancy bit is set.
    Surface = 0,
    // Inside a closed solid and not surface-occupied, including uniformly inside bricks and regions.
    Interior = 1
};

enum class CellScale : std::uint32_t
{
    Voxel = 0,
    // 8^3 voxels.
    Brick = 1,
    // The whole region.
    Region = 2
};

inline constexpr std::uint32_t FaceMaskShift = 16;
inline constexpr std::uint32_t AllFaces = 0x3Fu;
inline constexpr std::uint32_t ClassShift = 22;
inline constexpr std::uint32_t ScaleShift = 23;
inline constexpr std::uint32_t LocalBits = 10;

constexpr Cell PackCell(std::uint32_t regionSlot, std::uint32_t faceMask, CellClass cellClass, CellScale scale, std::uint32_t x,
    std::uint32_t y, std::uint32_t z) noexcept
{
    Cell cell;
    cell.word0 = regionSlot | (faceMask << FaceMaskShift) | (static_cast<std::uint32_t>(cellClass) << ClassShift) |
        (static_cast<std::uint32_t>(scale) << ScaleShift);
    cell.word1 = x | (y << LocalBits) | (z << (2 * LocalBits));
    return cell;
}

// Region box states, which also select the box colour in EditorVoxel.hlsl.
enum class BoxState : std::uint32_t
{
    Populated = 0,
    UniformInside = 1,
    // The asset is missing, unreadable or does not decode as its manifest entry.
    Error = 2
};

// One region: cubes refer to it by slot, and one box is drawn per entry.
//   offset: the region's origin in regions, relative to the anchor region.
//   state:  BoxState.
struct Region
{
    std::int32_t offset[3] = {0, 0, 0};
    std::uint32_t state = 0;
};

static_assert(sizeof(Region) == 16);

// What the Renderer uploads.
struct Data
{
    std::vector<Cell> cells;
    std::vector<Region> regions;
    // Voxels per region axis: 128, 256 or 512.
    std::uint32_t regionVoxels = 0;
};

// One frame's draw request; borrowed for that Render call only. Drawn only while `generation` names the published Data.
struct Request
{
    std::uint64_t generation = 0;
    // Lattice units (voxels relative to the anchor region's origin) to world space.
    Matrix4 worldFromLattice = Matrix4::Identity();
    bool drawBoxes = true;
};
}
