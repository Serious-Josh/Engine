#pragma once

#include "Renderer/EditorVoxelOverlay.h"
#include "Voxel/VoxelAssetStore.h"
#include "Voxel/VoxelLattice.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// VX.4C.2: a minimal Editor voxel visualization. One on-demand build reads the published manifest and region assets through
// the existing decoders (never the source meshes), one region at a time, and produces a bounded list of cubes for the Renderer.
// Nothing is cached between builds and nothing here writes an asset. See VoxelArchitecture.md §6.9.
namespace VoxelVisualization
{
struct RegionSummary
{
    Voxel::RegionKey key;
    bool uniformInside = false;
    std::uint32_t bricks = 0;
};

struct RegionError
{
    Voxel::RegionKey key;
    std::string message;
};

struct Result
{
    std::string manifestReference;
    // Set when the manifest itself cannot be shown; nothing else is then valid.
    std::string error;
    int baseExponent = Voxel::DefaultBaseExponent;
    // Every region the manifest lists (for selection), and the ones drawn: all of them, or the isolated one.
    std::vector<RegionSummary> regions;
    std::optional<Voxel::RegionKey> isolated;
    EditorVoxelOverlay::Data data;
    // The anchor region (the manifest's minimum region): lattice coordinates are voxels relative to its origin.
    std::array<std::int32_t, 3> anchorRegion{0, 0, 0};
    // Regions whose asset is missing or does not decode as its manifest entry: drawn as red boxes, no cubes.
    std::vector<RegionError> regionErrors;
    // The cell limit was reached: regions from `firstUndrawnRegion` on (in manifest order) have no or only some cubes.
    bool truncated = false;
    std::size_t firstUndrawnRegion = 0;
    // Exact counts of the drawn regions' decoded voxels (not of the cubes, which omit fully enclosed voxels).
    std::uint64_t surfaceVoxels = 0;
    std::uint64_t interiorVoxels = 0;
    bool cancelled = false;
};

// Builds the overlay for the manifest, or for one isolated region. A displayed voxel becomes a cube when at least one of its six
// neighbours inside the same region is not displayed; faces on a region's boundary are always drawn. A uniformly inside brick or
// region is one cube. At most maxCells cubes; then `truncated` is set.
Result Build(const Voxel::AssetStore& store, const std::string& manifestReference, const std::optional<Voxel::RegionKey>& isolate,
    const std::atomic<bool>* cancel = nullptr, std::size_t maxCells = EditorVoxelOverlay::MaximumCells);

// World from lattice units: the anchor region's origin (exact in double, then rounded to float) times the voxel size.
Matrix4 WorldFromLattice(const Result& result);

// Runs one Build on a background thread. Destroying it cancels and joins.
class Job
{
public:
    Job(std::shared_ptr<const Voxel::AssetStore> store, std::string manifestReference, std::optional<Voxel::RegionKey> isolate);
    ~Job();
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;

    bool IsFinished() const noexcept;
    // Waits if still running.
    Result Take();

private:
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_finished{false};
    Result m_result;
    std::thread m_thread;
};
}
