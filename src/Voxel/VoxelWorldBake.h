#pragma once

// VX.4A: partitions a geometry snapshot into regions, bakes or reuses each, and publishes a complete manifest. See
// Documentation/Voxel/VoxelArchitecture.md, section 6 and "VX.4A world bake".
//
// Planning finds every region a bake must account for: regions whose geometry box (RegionGeometryBounds) overlaps a triangle,
// and regions whose classification box overlaps a closed piece (they may be uniformly Inside without a triangle nearby). Each gets
// an input hash over exactly what BakeRegion reads: the triangles of its geometry box and the whole closed pieces of its
// classification box, with the lattice and bake settings and the format version. Regions not planned are uniformly Outside.
//
// Baking runs on worker threads sharing the read-only snapshot and acceleration. The number of concurrent jobs is bounded by
// threads and by the memory budget over a per-job worst case. Outputs are placed by plan index and assets are content-addressed,
// so the manifest and every asset are byte-identical whatever the thread count or completion order.
//
// Reuse: a region keeps its previous asset only when the previous manifest was baked with the same settings, its entry has the
// same input hash, and the asset reads, decodes, and records the same key, input hash and settings hash. Anything else rebakes.
//
// Nothing is visible until Publish: a failure, a cancellation or a publish error discards every staged asset and leaves the store's
// current manifest unchanged.

#include "Voxel/VoxelAcceleration.h"
#include "Voxel/VoxelAssetStore.h"
#include "Voxel/VoxelBake.h"
#include "Voxel/VoxelManifest.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace Voxel
{
struct RegionPlan
{
    RegionKey key;
    std::uint64_t inputHash = 0;
    // Any triangle overlaps the region's geometry box. Without one, the region is uniform and classified by one evaluation.
    bool hasGeometry = false;
};

struct WorldPlan
{
    // Ascending by key.
    std::vector<RegionPlan> regions;
    std::array<std::int32_t, 3> minimumRegion{0, 0, 0};
    std::array<std::int32_t, 3> maximumRegion{-1, -1, -1};
};

// FNV-1a over every planned region's packed key and input hash, in plan order: the manifest's participationHash.
std::uint64_t PlanParticipationHash(const WorldPlan& plan) noexcept;

// Fails when the plan would exceed `maximumRegions` or the lattice range.
std::expected<WorldPlan, std::string> PlanWorld(const GeometrySnapshot& snapshot, const BakeAcceleration& acceleration,
    const BakeSettings& settings, std::size_t maximumRegions);

// A conservative bound on one region job's working memory: the region data and its encoding at the worst case (every brick
// allocated), twice over for reuse validation, plus the baker's brick grid.
std::size_t EstimateRegionJobBytes(const BakeSettings& settings) noexcept;

struct WorldBakeOptions
{
    BakeSettings settings;
    unsigned threads = 1;
    // Concurrent jobs are limited to memoryBudget / EstimateRegionJobBytes (at least one).
    std::size_t memoryBudget = std::size_t{1} << 30;
    std::size_t maximumRegions = std::size_t{1} << 20;
    // Read by workers between regions; a set flag stops the bake and publishes nothing.
    const std::atomic<bool>* cancel = nullptr;
    bool reuse = true;
    // Optional progress: set to the planned region count once planning ends, then incremented as each region finishes.
    std::atomic<std::size_t>* regionsTotal = nullptr;
    std::atomic<std::size_t>* regionsDone = nullptr;
};

struct WorldBakeStatistics
{
    std::size_t regionsPlanned = 0;
    std::size_t populated = 0;
    std::size_t uniformInside = 0;
    std::size_t uniformOutside = 0;
    std::size_t reused = 0;
    std::size_t baked = 0;
    // Previous entries that matched by hash but were missing, unreadable, corrupt or inconsistent, and were rebaked.
    std::size_t cacheRejected = 0;
    unsigned concurrentJobs = 0;
    std::uint64_t stagedBytes = 0;
    double planSeconds = 0.0;
    double bakeSeconds = 0.0;
    double publishSeconds = 0.0;
    // Why reuse was refused, at most 32 lines.
    std::vector<std::string> notes;
};

struct WorldBakeResult
{
    std::string manifestReference;
    // ManifestContentHash of the published manifest: equal for equal voxel content in any store, including ones that name assets
    // by registry UUID.
    std::uint64_t contentHash = 0;
    ManifestData manifest;
    WorldBakeStatistics statistics;
};

struct WorldBakeError
{
    bool cancelled = false;
    std::string message;
};

// The voxel content of a manifest, independent of asset references: its settings hash and extent, then per entry its level, key,
// kind, input hash and the FNV-1a of its asset's bytes (`assetHashes`, parallel to the entries; zero for uniform entries). Regions
// absent from the manifest are uniformly Outside, implied by the extent.
std::uint64_t ManifestContentHash(const ManifestData& manifest, std::span<const std::uint64_t> assetHashes) noexcept;

// A finished bake whose assets are staged but not yet published. Publish it or abandon it exactly once.
struct PreparedWorldBake
{
    WorldBakeResult result;
    // Region assets staged by this bake, then the manifest, last.
    std::vector<std::string> staged;
};

// Plans, bakes or reuses every region and stages the assets and the manifest; publishes nothing. Safe on a worker thread when
// the store's Read and Stage are. On failure or cancellation everything it staged is already discarded.
std::expected<PreparedWorldBake, WorldBakeError> PrepareWorldBake(const GeometrySnapshot& snapshot, const BakeAcceleration& acceleration,
    AssetStore& store, const WorldBakeOptions& options);

// Publishes a prepared bake: all of its assets and its manifest, or nothing (and then discards them). Call where the store's
// Publish may run, which for the project store is the main thread.
std::expected<WorldBakeResult, WorldBakeError> PublishWorldBake(AssetStore& store, PreparedWorldBake prepared);

// Discards a prepared bake that will not be published.
void AbandonWorldBake(AssetStore& store, const PreparedWorldBake& prepared) noexcept;

// PrepareWorldBake then PublishWorldBake, on the calling thread.
std::expected<WorldBakeResult, WorldBakeError> BakeWorld(const GeometrySnapshot& snapshot, const BakeAcceleration& acceleration, AssetStore& store,
    const WorldBakeOptions& options);
}
