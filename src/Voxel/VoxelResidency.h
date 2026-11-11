#pragma once

// VX.5 Step 1: CPU side of GPU voxel residency for static obstacle queries. See Documentation/Voxel/VoxelArchitecture.md §6.10.
//
// Three parts, none of which touches the GPU:
//   1. Packing: a decoded region becomes blocked-only bricks (80 bytes each) and a brick index table.
//   2. Lookup: the exact CPU mirror of the HLSL queries, over the same buffer layout the GPU will use. Integer voxel coordinates
//      are authoritative; every query returns `blocked` and `known`.
//   3. Planning: the camera-centred toroidal window, which regions should be resident, and fence-gated slot allocation, so no
//      page, index slot or table version is reused while an older submission may still read it.
//
// Blocked means occupied or solid (VoxelArchitecture.md §8.1). Known-empty space is only what the format guarantees empty: a
// region inside the manifest's extent that the manifest does not list. Outside the extent, in a region that is not resident, in a
// region whose asset failed, or before anything is published, a query answers unknown.

#include "Voxel/VoxelLattice.h"
#include "Voxel/VoxelManifest.h"
#include "Voxel/VoxelRegion.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

class VoxelResidencyVerification;

namespace Voxel::Residency
{
// ---------------------------------------------------------------------------------------------------------------------------
// 1. Packing

// One allocated brick on the GPU: 20 u32 words, 80 bytes.
//   words 0-15: blocked1, one bit per voxel, bit (x + 8y + 64z) in word bit / 32
//   words 16-17: blocked2, one bit per 2^3 block (4^3 grid), bit (x + 4y + 16z) of the block
//   word 18: blocked4, one bit per 4^3 block (2^3 grid), bit (x + 2y + 4z) of the block, bits 8-31 zero
//   word 19: zero
// An allocated brick always holds a surface voxel, so at the brick level (8^3) it is blocked; no bit is needed.
struct PackedBrick
{
    std::array<std::uint32_t, 20> words{};

    bool operator==(const PackedBrick&) const = default;
};

static_assert(sizeof(PackedBrick) == 80);

inline constexpr std::uint32_t BricksPerPage = 64;
inline constexpr std::size_t PageBytes = BricksPerPage * sizeof(PackedBrick);

PackedBrick PackBrick(const BrickMask& occupancy, const BrickMask& solid) noexcept;

// A region ready for upload. `index` has R^3 entries in brick-cell order (x + R * (y + R * z)): a local brick ordinal into
// `bricks`, or UniformOutside / UniformInside. The GPU index slot holds the same entries with ordinals resolved to pool bricks.
struct PackedRegion
{
    RegionKey key;
    std::uint32_t regionBricks = 0;
    std::vector<std::uint32_t> index;
    std::vector<PackedBrick> bricks;

    std::uint32_t PageCount() const noexcept
    {
        return static_cast<std::uint32_t>((bricks.size() + BricksPerPage - 1) / BricksPerPage);
    }
};

// Decodes a region asset's bytes into `region` and checks it is the region `entry` lists: key, input hash, size, brick count,
// and the manifest's settings. Empty on success, otherwise why not. Shared by every reader of published regions.
std::string DecodeListedRegion(const ManifestData& manifest, const ManifestEntry& entry, std::span<const std::byte> bytes, RegionData& region);

// Fails for data that is internally inconsistent (decoded regions never are).
std::expected<PackedRegion, std::string> PackRegion(const RegionData& region);

// The index slot contents for a region placed in `pages` (PageCount() of them): ordinal o becomes pages[o / 64] * 64 + o % 64.
std::vector<std::uint32_t> ResolveIndex(const PackedRegion& region, std::span<const std::uint32_t> pages);

// ---------------------------------------------------------------------------------------------------------------------------
// 2. Lookup

enum class RegionState : std::uint32_t
{
    // Zero, so a cleared table answers unknown everywhere.
    Unknown = 0,
    // Inside the manifest's extent and not listed: uniformly outside by the format.
    KnownEmpty = 1,
    UniformInside = 2,
    // Resident: its index slot is valid.
    Populated = 3
};

// One toroidal table entry: the region it describes, its state and, for Populated, its index slot.
//   packed: state (bits 0-1), index slot (bits 2-31)
struct TableEntry
{
    std::int32_t key[3] = {0, 0, 0};
    std::uint32_t packed = 0;

    RegionState State() const noexcept
    {
        return static_cast<RegionState>(packed & 3u);
    }

    std::uint32_t IndexSlot() const noexcept
    {
        return packed >> 2;
    }

    bool operator==(const TableEntry&) const = default;
};

static_assert(sizeof(TableEntry) == 16);

TableEntry MakeEntry(const RegionKey& key, RegionState state, std::uint32_t indexSlot = 0) noexcept;

// The table slot of a region: its key modulo the window size per axis, non-negative for negative keys.
std::uint32_t TableSlot(const RegionKey& key, std::uint32_t window) noexcept;

// A read-only view of the GPU buffers, in their GPU layout. Index slots are R^3 u32 each; bricks are addressed as
// page * 64 + ordinal.
struct WorldView
{
    // False until residency is published: every query is then unknown.
    bool valid = false;
    std::uint32_t window = 0;
    int regionBricksLog2 = 0;
    std::span<const TableEntry> table;
    std::span<const std::uint32_t> indexPool;
    std::span<const PackedBrick> brickPool;
};

struct Sample
{
    bool blocked = false;
    bool known = false;

    bool operator==(const Sample&) const = default;
};

// Level L is a lattice-aligned cube of 2^L voxels per axis; cell c at level L covers voxels [c * 2^L, (c + 1) * 2^L).
// Levels 0-2 read one mask bit, level 3 one index entry, levels 4 up to the region level OR their bricks (8^(L-3) reads), and
// the region level and above OR whole-region summaries. Every level is the exact OR of the voxels it covers.
inline constexpr int BrickLevel = BrickVoxelsLog2;

int RegionLevel(const WorldView& view) noexcept;

Sample QueryVoxel(const WorldView& view, const Coordinate& voxel) noexcept;
Sample QueryCell(const WorldView& view, const Coordinate& cell, int level) noexcept;

// Conservative box query over the inclusive voxel range [minimum, maximum]: blocked when any covered voxel is blocked, known
// only when every covered voxel is known. The level is chosen so at most `maximumCells` cells are read; the cells cover the box
// by flooring the minimum and maximum, so the coarser answer can only add voxels, never miss one.
Sample QueryBox(const WorldView& view, const Coordinate& minimum, const Coordinate& maximum, std::uint64_t maximumCells = 64) noexcept;

// World-position wrapper: floor(position / voxelSize). Exact while |position| / voxelSize < 2^53 in double; the GPU version, in
// float, is exact only while |position| / voxelSize < 2^24 (2 km at 2^-3 m voxels). Prefer integer coordinates.
Coordinate WorldToVoxelFloor(const std::array<double, 3>& position, int baseExponent) noexcept;

// ---------------------------------------------------------------------------------------------------------------------------
// 3. Planning

// Fixed-capacity slots whose reuse waits for the GPU: a retired slot carries the fence of the last submission that may read it
// and returns to the free list only once that fence has completed. Used for brick pages, index slots and table versions.
class SlotAllocator
{
public:
    explicit SlotAllocator(std::uint32_t capacity = 0);

    // Adds `count` new free slots, numbered after the existing ones (capacity grows as GPU storage is added).
    void Grow(std::uint32_t count);
    // All or nothing: `count` free slots, or none when fewer are free.
    std::optional<std::vector<std::uint32_t>> Allocate(std::uint32_t count);
    void Retire(std::span<const std::uint32_t> slots, std::uint64_t fence);
    void Collect(std::uint64_t completedFence);
    // After a GPU drain (or a context reset, which drains): every retired slot is free.
    void CollectAll();

    std::uint32_t Capacity() const noexcept
    {
        return m_capacity;
    }

    std::uint32_t FreeCount() const noexcept
    {
        return static_cast<std::uint32_t>(m_free.size());
    }

    std::uint32_t RetiredCount() const noexcept;

private:
    friend class ::VoxelResidencyVerification;

    struct Retired
    {
        std::vector<std::uint32_t> slots;
        std::uint64_t fence = 0;
    };

    std::uint32_t m_capacity = 0;
    std::vector<std::uint32_t> m_free;
    std::deque<Retired> m_retired;
};

struct PlannerSettings
{
    // Camera-centred coverage radius; the window is 2 * ceil(coverage / regionSize) + 1 regions per axis, at most maximumWindow.
    double coverageMetres = 64.0;
    std::uint32_t maximumWindow = 9;
    // Ceilings, not reservations: pages and slots are counted against them as regions become resident.
    std::size_t brickPoolBytes = std::size_t{64} << 20;
    std::size_t indexPoolBytes = std::size_t{32} << 20;
    // The window centre moves only when the camera is this far (in regions) beyond the centre region's middle, plus one half.
    double hysteresisRegions = 0.25;
    // A transient read failure is retried after these delays, then treated as permanent.
    std::array<double, 3> retryDelaysSeconds{0.5, 2.0, 8.0};
};

// Decides which populated regions should be resident, in what order to load them, and what each table entry says. It owns no
// GPU state: the caller loads, uploads and reports back. Single-threaded (the Engine's main thread).
class Planner
{
public:
    explicit Planner(PlannerSettings settings = {});

    // A new manifest. Resident and loading regions whose reference and input hash are unchanged are kept; the rest are evicted.
    // Failures are forgotten for regions whose reference changed.
    void SetManifest(const ManifestData& manifest);
    void ClearManifest();
    bool HasManifest() const noexcept
    {
        return m_manifest.has_value();
    }

    // World position of the camera; moves the window centre subject to hysteresis. Returns true when the centre moved.
    bool SetCamera(const std::array<double, 3>& position);

    struct Request
    {
        RegionKey key;
        std::string reference;
        std::uint64_t inputHash = 0;
        std::uint32_t pages = 0;
    };

    struct Update
    {
        // Resident regions to drop (outside the window, or superseded by a new manifest).
        std::vector<RegionKey> evict;
        // Regions to start loading, nearest first; each fits the remaining page and slot ceilings.
        std::vector<Request> load;
        // Desired regions that do not fit the ceilings; they stay unknown. Farthest regions are the ones left out.
        std::size_t overBudget = 0;
    };

    // Evictions take effect immediately; requested loads are marked loading. `now` is in seconds, for retry timing.
    Update Plan(double now);

    // Load results. A transient failure (a read error) is retried; a permanent one (decode or validation) is not.
    void OnLoaded(const RegionKey& key, std::uint32_t indexSlot);
    void OnLoadFailed(const RegionKey& key, bool transient, double now, const std::string& message);
    // A finished load whose region is no longer wanted (evicted or superseded meanwhile): the caller drops the data.
    bool IsWanted(const RegionKey& key, const std::string& reference) const;

    // The table for the current window: W^3 entries, each the state of the window region at that toroidal slot.
    std::vector<TableEntry> BuildTable() const;

    std::uint32_t Window() const noexcept
    {
        return m_window;
    }

    std::array<std::int32_t, 3> Centre() const noexcept
    {
        return m_centre;
    }

    std::uint32_t PageCapacity() const noexcept
    {
        return m_pageCapacity;
    }

    std::uint32_t SlotCapacity() const noexcept
    {
        return m_slotCapacity;
    }

    struct RegionRecord
    {
        enum class Status
        {
            Loading,
            Resident,
            Failed
        };

        Status status = Status::Loading;
        std::string reference;
        std::uint64_t inputHash = 0;
        std::uint32_t pages = 0;
        std::uint32_t indexSlot = 0;
        bool permanent = false;
        std::uint32_t attempts = 0;
        double retryAt = 0.0;
        std::string message;
    };

    const std::map<RegionKey, RegionRecord>& Records() const noexcept
    {
        return m_records;
    }

private:
    bool InWindow(const RegionKey& key) const noexcept;
    bool InExtent(const RegionKey& key) const noexcept;
    std::uint32_t CommittedPages() const noexcept;
    std::uint32_t CommittedSlots() const noexcept;

    PlannerSettings m_settings;
    std::optional<ManifestData> m_manifest;
    std::map<RegionKey, std::size_t> m_entries;
    std::uint32_t m_window = 1;
    std::uint32_t m_pageCapacity = 0;
    std::uint32_t m_slotCapacity = 0;
    std::array<std::int32_t, 3> m_centre{0, 0, 0};
    bool m_hasCamera = false;
    std::map<RegionKey, RegionRecord> m_records;
    // Resident regions dropped by SetManifest or ClearManifest, reported by the next Plan.
    std::vector<RegionKey> m_pendingEvictions;
};
}
