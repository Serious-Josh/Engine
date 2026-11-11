#pragma once

// VX.2: CPU voxelization of one region from a geometry snapshot. See Documentation/Voxel/VoxelArchitecture.md, section 5.
//
// Every stored value is a pure function of (snapshot, settings, lattice coordinate), so independently baked regions agree
// wherever they overlap (apron voxels, coarse distance) and any region size R gives the same voxels:
//   * Occupancy: the triangle touches the voxel's closed cube expanded by OccupancyPadding (face, edge and corner contact count).
//   * Classification: Inside when the per-piece-rounded winding numbers of the closed pieces sum to at least 1. Open geometry
//     never classifies; it is an obstacle through occupancy only.
//   * Distance: exact unsigned distance from the voxel centre to the nearest triangle, clamped to the narrow band, quantised to
//     127 steps and negated when Inside.
//   * Coarse distance: saturated Chebyshev distance, in bricks, to the nearest brick with a surface voxel, in any region.
// All geometry is evaluated in absolute lattice units (world / voxel size, an exact power-of-two scale).
//
// Classification changes only across closed-piece triangles, so it is evaluated once per connected set of voxels no closed triangle
// touches (open-surface voxels included), and once per surface-free connected set of bricks; such sets are at least half a voxel
// from every closed surface. Voxels touched by closed triangles are classified one by one. Each evaluation uses the accelerated
// ray count or the exact sum (BakeSettings::classification); both give each closed piece's integer winding number.
//
// Distance searches candidates in order of a per-brick lower bound and stops once no remaining triangle can be nearer, starting
// from the narrow band (anything farther quantises to the same 127). Occupancy rejects voxels clearly beyond a triangle's plane
// before the full test. These prunings leave every stored value unchanged.
//
// Single-threaded. The result passes ValidateRegion, or the bake fails and says why: geometry is never altered to satisfy the
// decoder.

#include "Voxel/VoxelAcceleration.h"
#include "Voxel/VoxelGeometry.h"
#include "Voxel/VoxelRegion.h"

#include <cstdint>
#include <expected>
#include <string>

namespace Voxel
{
// In voxels. Makes contacts lost to floating-point rounding count as touching: the conservative side.
inline constexpr double OccupancyPadding = 1e-6;

enum class ClassificationMode
{
    // Ray crossings through WindingIndex, with the exact sum where a ray is not certain. The default.
    Accelerated,
    // The solid-angle sum over every triangle of every enclosing closed piece. The VX.2 reference.
    Exact
};

struct BakeSettings
{
    std::int8_t baseExponent = DefaultBaseExponent;
    std::uint8_t regionBricks = 1u << DefaultRegionBricksLog2;
    std::uint8_t narrowBand = 4;
    std::uint8_t coarseClamp = 4;
    ClassificationMode classification = ClassificationMode::Accelerated;
    // Classification budget in triangle evaluations (solid angles, or ray tests plus exact-fallback solid angles). Exact mode
    // refuses up front from an upper-bound estimate; accelerated mode fails when the count passes it (VoxelArchitecture.md 10.5).
    double maximumWindingWork = 2e9;
};

enum class RegionBakeKind
{
    // No surface voxel: recorded in the manifest, never stored as a region asset.
    UniformOutside,
    UniformInside,
    Populated
};

struct RegionBakeStatistics
{
    std::size_t candidateTriangles = 0;
    std::size_t closedPieces = 0;
    std::size_t allocatedBricks = 0;
    std::size_t windingEvaluations = 0;
    // Triangles evaluated by classification: the classification cost.
    std::uint64_t windingWork = 0;
    // Accelerated mode: piece rays cast, and pieces that fell back to the exact sum.
    std::uint64_t rayQueries = 0;
    std::uint64_t exactFallbacks = 0;
    std::uint64_t triangleCubeTests = 0;
    std::uint64_t distanceTests = 0;

    // Wall-clock stage times. Occupancy is computed lazily during the other stages and is excluded from them.
    double candidateSeconds = 0.0;
    double occupancySeconds = 0.0;
    double classificationSeconds = 0.0;
    double distanceSeconds = 0.0;
    double totalSeconds = 0.0;
};

struct RegionBakeResult
{
    RegionBakeKind kind = RegionBakeKind::UniformOutside;
    // Populated only.
    RegionData data;
    RegionBakeStatistics statistics;
};

struct WorldBox
{
    Point minimum;
    Point maximum;
};

// Bricks of halo the baker reads on every side of a region: enough for the coarse clamp and the distance band.
int RegionHaloBricks(const BakeSettings& settings) noexcept;

// The world box whose overlapping triangles are a region's geometry inputs: the region, its halo and one voxel of slack.
// BakeRegion collects exactly the triangles whose bounds overlap it.
WorldBox RegionGeometryBounds(const RegionKey& key, const BakeSettings& settings) noexcept;

// The world box whose overlapping closed pieces can classify a region's voxels and aprons; whole pieces are used, however far they
// extend. BakeRegion uses exactly the closed pieces whose bounds overlap it.
WorldBox RegionClassificationBounds(const RegionKey& key, const BakeSettings& settings) noexcept;

// Inclusive bounds overlap.
bool PieceOverlaps(const SnapshotPiece& piece, const WorldBox& box) noexcept;

// The settings hash a region records. Stable across runs and platforms.
std::uint64_t HashBakeSettings(const BakeSettings& settings) noexcept;

// Bakes one region using acceleration built once for the snapshot; share `acceleration` across every region of the snapshot.
std::expected<RegionBakeResult, std::string> BakeRegion(const GeometrySnapshot& snapshot, const BakeAcceleration& acceleration, const RegionKey& key,
    const BakeSettings& settings);

// Convenience for a single region: builds the acceleration for this call only.
std::expected<RegionBakeResult, std::string> BakeRegion(const GeometrySnapshot& snapshot, const RegionKey& key, const BakeSettings& settings);
}
