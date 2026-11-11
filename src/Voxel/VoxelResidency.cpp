#include "Voxel/VoxelResidency.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Voxel::Residency
{
namespace
{
constexpr std::uint32_t Blocked2Word = 16;
constexpr std::uint32_t Blocked4Word = 18;

bool TestBit(const BrickMask& mask, int bit) noexcept
{
    return ((mask[static_cast<std::size_t>(bit >> 6)] >> (bit & 63)) & 1u) != 0;
}

void SetWordBit(std::array<std::uint32_t, 20>& words, std::uint32_t firstWord, int bit) noexcept
{
    words[firstWord + static_cast<std::uint32_t>(bit >> 5)] |= 1u << (bit & 31);
}

bool TestWordBit(const PackedBrick& brick, std::uint32_t firstWord, int bit) noexcept
{
    return ((brick.words[firstWord + static_cast<std::uint32_t>(bit >> 5)] >> (bit & 31)) & 1u) != 0;
}

std::int64_t Chebyshev(const RegionKey& key, const std::array<std::int32_t, 3>& centre) noexcept
{
    return (std::max)({std::llabs(std::int64_t{key.x} - centre[0]), std::llabs(std::int64_t{key.y} - centre[1]),
        std::llabs(std::int64_t{key.z} - centre[2])});
}

const TableEntry* FindRegion(const WorldView& view, const RegionKey& key) noexcept
{
    if(!view.valid || view.window == 0 || view.table.size() != std::size_t{view.window} * view.window * view.window)
    {
        return nullptr;
    }

    const TableEntry& entry = view.table[TableSlot(key, view.window)];
    const bool matches = entry.key[0] == key.x && entry.key[1] == key.y && entry.key[2] == key.z;
    return matches ? &entry : nullptr;
}

// Accumulates an OR of cells: blocked when any is blocked; known when every cell is known, or any is known blocked (then the
// answer is certain whatever the unknown cells hold).
struct Accumulator
{
    bool anyBlocked = false;
    bool allKnown = true;

    void Add(const Sample& sample) noexcept
    {
        anyBlocked = anyBlocked || (sample.known && sample.blocked);
        allKnown = allKnown && sample.known;
    }

    Sample Result() const noexcept
    {
        const bool known = anyBlocked || allKnown;
        return {anyBlocked, known};
    }
};

constexpr Sample Unknown{false, false};
constexpr Sample KnownFree{false, true};
constexpr Sample KnownBlocked{true, true};

// A whole region: a populated region holds a surface voxel, so it is blocked.
Sample RegionSummary(const TableEntry* entry) noexcept
{
    if(entry == nullptr)
    {
        return Unknown;
    }

    switch(entry->State())
    {
    case RegionState::KnownEmpty:
        return KnownFree;
    case RegionState::UniformInside:
    case RegionState::Populated:
        return KnownBlocked;
    case RegionState::Unknown:
        break;
    }

    return Unknown;
}

std::uint32_t IndexEntry(const WorldView& view, const TableEntry& entry, std::int64_t brickX, std::int64_t brickY, std::int64_t brickZ)
{
    const std::int64_t bricks = std::int64_t{1} << view.regionBricksLog2;
    const std::size_t cells = static_cast<std::size_t>(bricks * bricks * bricks);
    const std::size_t offset = static_cast<std::size_t>(entry.IndexSlot()) * cells +
        static_cast<std::size_t>(brickX + bricks * (brickY + bricks * brickZ));
    return offset < view.indexPool.size() ? view.indexPool[offset] : UniformOutside;
}
}

// ---------------------------------------------------------------------------------------------------------------------------
// Packing

std::string DecodeListedRegion(const ManifestData& manifest, const ManifestEntry& entry, std::span<const std::byte> bytes, RegionData& region)
{
    if(const DecodeResult decoded = DecodeRegion(bytes, region); !decoded)
    {
        return "Does not decode: " + decoded.message;
    }

    const bool matchesEntry = region.key == entry.key && region.inputHash == entry.inputHash && bytes.size() == entry.byteSize &&
        region.GetBrickCount() == entry.brickCount;
    const bool matchesSettings = region.baseExponent == manifest.baseExponent && region.regionBricks == manifest.regionBricks &&
        region.settingsHash == manifest.settingsHash;

    if(!matchesEntry)
    {
        return "The asset records a different region.";
    }

    return matchesSettings ? std::string{} : "The asset was baked with other settings.";
}

PackedBrick PackBrick(const BrickMask& occupancy, const BrickMask& solid) noexcept
{
    PackedBrick brick;

    for(int z = 0; z < BrickVoxels; ++z)
    {
        for(int y = 0; y < BrickVoxels; ++y)
        {
            for(int x = 0; x < BrickVoxels; ++x)
            {
                const int bit = x + BrickVoxels * (y + BrickVoxels * z);

                if(!TestBit(occupancy, bit) && !TestBit(solid, bit))
                {
                    continue;
                }

                SetWordBit(brick.words, 0, bit);
                SetWordBit(brick.words, Blocked2Word, (x / 2) + 4 * ((y / 2) + 4 * (z / 2)));
                SetWordBit(brick.words, Blocked4Word, (x / 4) + 2 * ((y / 4) + 2 * (z / 4)));
            }
        }
    }

    return brick;
}

std::expected<PackedRegion, std::string> PackRegion(const RegionData& region)
{
    const std::size_t bricks = region.regionBricks;
    const std::size_t cells = bricks * bricks * bricks;
    const std::size_t count = region.brickCells.size();

    if(region.brickIndex.size() != cells || region.occupancy.size() != count || region.solid.size() != count)
    {
        return std::unexpected("The region's tables have inconsistent sizes.");
    }

    // Slots are numbered in ascending cell order by the format, so the slot is the brick's ordinal.
    for(const std::uint32_t value : region.brickIndex)
    {
        if(value != UniformOutside && value != UniformInside && value >= count)
        {
            return std::unexpected("The region's brick index refers to a missing brick.");
        }
    }

    PackedRegion packed;
    packed.key = region.key;
    packed.regionBricks = region.regionBricks;
    packed.index = region.brickIndex;
    packed.bricks.reserve(count);

    for(std::size_t slot = 0; slot < count; ++slot)
    {
        packed.bricks.push_back(PackBrick(region.occupancy[slot], region.solid[slot]));
    }

    return packed;
}

std::vector<std::uint32_t> ResolveIndex(const PackedRegion& region, std::span<const std::uint32_t> pages)
{
    std::vector<std::uint32_t> resolved;
    resolved.reserve(region.index.size());

    for(const std::uint32_t value : region.index)
    {
        if(value == UniformOutside || value == UniformInside)
        {
            resolved.push_back(value);
            continue;
        }

        resolved.push_back(pages[value / BricksPerPage] * BricksPerPage + value % BricksPerPage);
    }

    return resolved;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Lookup

TableEntry MakeEntry(const RegionKey& key, RegionState state, std::uint32_t indexSlot) noexcept
{
    TableEntry entry;
    entry.key[0] = key.x;
    entry.key[1] = key.y;
    entry.key[2] = key.z;
    entry.packed = static_cast<std::uint32_t>(state) | (indexSlot << 2);
    return entry;
}

std::uint32_t TableSlot(const RegionKey& key, std::uint32_t window) noexcept
{
    const std::int64_t size = window;
    const auto wrap = [size](std::int32_t value) { return static_cast<std::uint32_t>(((std::int64_t{value} % size) + size) % size); };
    return wrap(key.x) + window * (wrap(key.y) + window * wrap(key.z));
}

int RegionLevel(const WorldView& view) noexcept
{
    return BrickLevel + view.regionBricksLog2;
}

Sample QueryCell(const WorldView& view, const Coordinate& cell, int level) noexcept
{
    if(!view.valid || level < 0)
    {
        return Unknown;
    }

    const int regionLevel = RegionLevel(view);

    // At or above the region level: OR of whole-region summaries.
    if(level >= regionLevel)
    {
        const int shift = level - regionLevel;
        const std::int64_t span = std::int64_t{1} << shift;
        Accumulator accumulator;

        for(std::int64_t z = 0; z < span; ++z)
        {
            for(std::int64_t y = 0; y < span; ++y)
            {
                for(std::int64_t x = 0; x < span; ++x)
                {
                    const RegionKey key{0, static_cast<std::int32_t>((cell.x << shift) + x), static_cast<std::int32_t>((cell.y << shift) + y),
                        static_cast<std::int32_t>((cell.z << shift) + z)};
                    accumulator.Add(RegionSummary(FindRegion(view, key)));
                }
            }
        }

        return accumulator.Result();
    }

    // Below the region level: the region holding the cell, then the cell inside it.
    const int toRegion = regionLevel - level;
    const Coordinate region = FloorShift(cell, toRegion);
    const TableEntry* entry = FindRegion(view, RegionKey{0, static_cast<std::int32_t>(region.x), static_cast<std::int32_t>(region.y),
        static_cast<std::int32_t>(region.z)});

    if(entry == nullptr || entry->State() != RegionState::Populated)
    {
        return RegionSummary(entry);
    }

    const std::int64_t localMask = (std::int64_t{1} << toRegion) - 1;
    const Coordinate local{cell.x & localMask, cell.y & localMask, cell.z & localMask};

    // Brick level and above: an allocated or uniformly inside brick is blocked.
    if(level >= BrickLevel)
    {
        const int shift = level - BrickLevel;
        const std::int64_t span = std::int64_t{1} << shift;

        for(std::int64_t z = 0; z < span; ++z)
        {
            for(std::int64_t y = 0; y < span; ++y)
            {
                for(std::int64_t x = 0; x < span; ++x)
                {
                    const std::uint32_t value = IndexEntry(view, *entry, (local.x << shift) + x, (local.y << shift) + y, (local.z << shift) + z);

                    if(value != UniformOutside)
                    {
                        return KnownBlocked;
                    }
                }
            }
        }

        return KnownFree;
    }

    // Inside one brick: its mask at the requested level.
    const int toBrick = BrickLevel - level;
    const std::uint32_t value = IndexEntry(view, *entry, local.x >> toBrick, local.y >> toBrick, local.z >> toBrick);

    if(value == UniformOutside)
    {
        return KnownFree;
    }

    if(value == UniformInside)
    {
        return KnownBlocked;
    }

    if(value >= view.brickPool.size())
    {
        return Unknown;
    }

    const PackedBrick& brick = view.brickPool[value];
    const std::int64_t inBrickMask = (std::int64_t{1} << toBrick) - 1;
    const int x = static_cast<int>(local.x & inBrickMask);
    const int y = static_cast<int>(local.y & inBrickMask);
    const int z = static_cast<int>(local.z & inBrickMask);

    switch(level)
    {
    case 0:
        return {TestWordBit(brick, 0, x + 8 * (y + 8 * z)), true};
    case 1:
        return {TestWordBit(brick, Blocked2Word, x + 4 * (y + 4 * z)), true};
    default:
        return {TestWordBit(brick, Blocked4Word, x + 2 * (y + 2 * z)), true};
    }
}

Sample QueryVoxel(const WorldView& view, const Coordinate& voxel) noexcept
{
    return QueryCell(view, voxel, 0);
}

Sample QueryBox(const WorldView& view, const Coordinate& minimum, const Coordinate& maximum, std::uint64_t maximumCells) noexcept
{
    if(!view.valid)
    {
        return Unknown;
    }

    if(minimum.x > maximum.x || minimum.y > maximum.y || minimum.z > maximum.z)
    {
        return KnownFree;
    }

    // The finest level whose reads fit: cells at level L cover [floor(min / 2^L), floor(max / 2^L)], a superset of the box.
    const int regionLevel = RegionLevel(view);

    for(int level = 0; level <= regionLevel; ++level)
    {
        const Coordinate low = FloorShift(minimum, level);
        const Coordinate high = FloorShift(maximum, level);
        const std::uint64_t cells = static_cast<std::uint64_t>(high.x - low.x + 1) * static_cast<std::uint64_t>(high.y - low.y + 1) *
            static_cast<std::uint64_t>(high.z - low.z + 1);
        const bool aboveBricks = level > BrickLevel && level < regionLevel;
        const std::uint64_t readsPerCell = aboveBricks ? std::uint64_t{1} << (3 * (level - BrickLevel)) : 1;

        if(cells * readsPerCell > maximumCells)
        {
            continue;
        }

        Accumulator accumulator;

        for(std::int64_t z = low.z; z <= high.z; ++z)
        {
            for(std::int64_t y = low.y; y <= high.y; ++y)
            {
                for(std::int64_t x = low.x; x <= high.x; ++x)
                {
                    accumulator.Add(QueryCell(view, {x, y, z}, level));
                }
            }
        }

        return accumulator.Result();
    }

    // Too large for the read limit even in whole regions: say nothing rather than guess.
    return Unknown;
}

Coordinate WorldToVoxelFloor(const std::array<double, 3>& position, int baseExponent) noexcept
{
    return {static_cast<std::int64_t>(std::floor(std::ldexp(position[0], -baseExponent))),
        static_cast<std::int64_t>(std::floor(std::ldexp(position[1], -baseExponent))),
        static_cast<std::int64_t>(std::floor(std::ldexp(position[2], -baseExponent)))};
}

// ---------------------------------------------------------------------------------------------------------------------------
// Slot allocation

SlotAllocator::SlotAllocator(std::uint32_t capacity) :
    m_capacity(capacity)
{
    m_free.reserve(capacity);

    // Highest first, so Allocate hands out low slots first (pop_back).
    for(std::uint32_t slot = capacity; slot > 0; --slot)
    {
        m_free.push_back(slot - 1);
    }
}

void SlotAllocator::Grow(std::uint32_t count)
{
    // Prepended, so existing free slots (lower numbers) are still handed out first.
    std::vector<std::uint32_t> added;
    added.reserve(count);

    for(std::uint32_t slot = m_capacity + count; slot > m_capacity; --slot)
    {
        added.push_back(slot - 1);
    }

    m_free.insert(m_free.begin(), added.begin(), added.end());
    m_capacity += count;
}

std::optional<std::vector<std::uint32_t>> SlotAllocator::Allocate(std::uint32_t count)
{
    if(count > m_free.size())
    {
        return std::nullopt;
    }

    std::vector<std::uint32_t> slots(m_free.end() - count, m_free.end());
    m_free.resize(m_free.size() - count);
    return slots;
}

void SlotAllocator::Retire(std::span<const std::uint32_t> slots, std::uint64_t fence)
{
    if(slots.empty())
    {
        return;
    }

    // Fences only grow, so the queue stays in completion order.
    m_retired.push_back({std::vector<std::uint32_t>(slots.begin(), slots.end()), fence});
}

void SlotAllocator::Collect(std::uint64_t completedFence)
{
    while(!m_retired.empty() && m_retired.front().fence <= completedFence)
    {
        m_free.insert(m_free.end(), m_retired.front().slots.begin(), m_retired.front().slots.end());
        m_retired.pop_front();
    }
}

void SlotAllocator::CollectAll()
{
    Collect((std::numeric_limits<std::uint64_t>::max)());
}

std::uint32_t SlotAllocator::RetiredCount() const noexcept
{
    std::size_t count = 0;

    for(const Retired& retired : m_retired)
    {
        count += retired.slots.size();
    }

    return static_cast<std::uint32_t>(count);
}

// ---------------------------------------------------------------------------------------------------------------------------
// Planning

Planner::Planner(PlannerSettings settings) :
    m_settings(settings)
{
}

void Planner::SetManifest(const ManifestData& manifest)
{
    m_manifest = manifest;
    m_entries.clear();

    for(std::size_t index = 0; index < manifest.entries.size(); ++index)
    {
        m_entries.emplace(manifest.entries[index].key, index);
    }

    // Window and ceilings follow the manifest's region size.
    const std::uint32_t regionVoxels = std::uint32_t{manifest.regionBricks} * BrickVoxels;
    const double regionMetres = std::ldexp(static_cast<double>(regionVoxels), manifest.baseExponent);
    const auto reach = static_cast<std::uint32_t>(std::ceil(m_settings.coverageMetres / regionMetres));
    std::uint32_t window = (std::min)(2 * reach + 1, m_settings.maximumWindow);
    m_window = window % 2 == 0 ? window - 1 : (std::max)(window, 1u);

    const std::size_t sliceBytes = std::size_t{manifest.regionBricks} * manifest.regionBricks * manifest.regionBricks * sizeof(std::uint32_t);
    m_pageCapacity = static_cast<std::uint32_t>(m_settings.brickPoolBytes / PageBytes);
    m_slotCapacity = static_cast<std::uint32_t>(m_settings.indexPoolBytes / sliceBytes);

    // Keep what the new manifest still lists unchanged; forget everything else (Plan reports resident ones as evictions).
    for(auto record = m_records.begin(); record != m_records.end();)
    {
        const auto found = m_entries.find(record->first);
        const bool unchanged = found != m_entries.end() && manifest.entries[found->second].kind == RegionKind::Populated &&
            manifest.entries[found->second].reference == record->second.reference &&
            manifest.entries[found->second].inputHash == record->second.inputHash;

        if(unchanged)
        {
            ++record;
            continue;
        }

        if(record->second.status == RegionRecord::Status::Resident)
        {
            m_pendingEvictions.push_back(record->first);
        }

        record = m_records.erase(record);
    }
}

void Planner::ClearManifest()
{
    for(const auto& [key, record] : m_records)
    {
        if(record.status == RegionRecord::Status::Resident)
        {
            m_pendingEvictions.push_back(key);
        }
    }

    m_records.clear();
    m_entries.clear();
    m_manifest.reset();
}

bool Planner::SetCamera(const std::array<double, 3>& position)
{
    if(!m_manifest)
    {
        return false;
    }

    const double regionMetres = std::ldexp(static_cast<double>(std::uint32_t{m_manifest->regionBricks} * BrickVoxels), m_manifest->baseExponent);
    bool moved = false;

    for(std::size_t axis = 0; axis < 3; ++axis)
    {
        const double regions = position[axis] / regionMetres;
        const auto containing = static_cast<std::int32_t>(std::floor(regions));
        const double fromMiddle = std::abs(regions - (static_cast<double>(m_centre[axis]) + 0.5));

        if(!m_hasCamera || fromMiddle > 0.5 + m_settings.hysteresisRegions)
        {
            moved = moved || m_centre[axis] != containing;
            m_centre[axis] = containing;
        }
    }

    m_hasCamera = true;
    return moved;
}

bool Planner::InWindow(const RegionKey& key) const noexcept
{
    const std::int64_t half = (m_window - 1) / 2;
    return key.level == 0 && Chebyshev(key, m_centre) <= half;
}

bool Planner::InExtent(const RegionKey& key) const noexcept
{
    if(!m_manifest)
    {
        return false;
    }

    const std::array<std::int32_t, 3> coordinates{key.x, key.y, key.z};

    for(std::size_t axis = 0; axis < 3; ++axis)
    {
        if(coordinates[axis] < m_manifest->minimumRegion[axis] || coordinates[axis] > m_manifest->maximumRegion[axis])
        {
            return false;
        }
    }

    return true;
}

std::uint32_t Planner::CommittedPages() const noexcept
{
    std::uint32_t pages = 0;

    for(const auto& [key, record] : m_records)
    {
        pages += record.status == RegionRecord::Status::Failed ? 0 : record.pages;
    }

    return pages;
}

std::uint32_t Planner::CommittedSlots() const noexcept
{
    std::uint32_t slots = 0;

    for(const auto& [key, record] : m_records)
    {
        slots += record.status == RegionRecord::Status::Failed ? 0 : 1;
    }

    return slots;
}

Planner::Update Planner::Plan(double now)
{
    Update update;
    update.evict = std::move(m_pendingEvictions);
    m_pendingEvictions.clear();

    if(!m_manifest || !m_hasCamera)
    {
        return update;
    }

    // Leave the window: resident regions are evicted; loads in flight are simply no longer wanted.
    for(auto record = m_records.begin(); record != m_records.end();)
    {
        const bool active = record->second.status != RegionRecord::Status::Failed;

        if(active && !InWindow(record->first))
        {
            if(record->second.status == RegionRecord::Status::Resident)
            {
                update.evict.push_back(record->first);
            }

            record = m_records.erase(record);
            continue;
        }

        ++record;
    }

    // Candidates: populated window regions not resident or loading, and not failed (unless a transient retry is due).
    std::vector<const ManifestEntry*> candidates;
    const std::int32_t half = static_cast<std::int32_t>((m_window - 1) / 2);

    for(std::int32_t z = m_centre[2] - half; z <= m_centre[2] + half; ++z)
    {
        for(std::int32_t y = m_centre[1] - half; y <= m_centre[1] + half; ++y)
        {
            for(std::int32_t x = m_centre[0] - half; x <= m_centre[0] + half; ++x)
            {
                const auto found = m_entries.find(RegionKey{0, x, y, z});

                if(found == m_entries.end() || m_manifest->entries[found->second].kind != RegionKind::Populated)
                {
                    continue;
                }

                const auto record = m_records.find(found->first);
                const bool retryDue = record != m_records.end() && record->second.status == RegionRecord::Status::Failed &&
                    !record->second.permanent && now >= record->second.retryAt;

                if(record == m_records.end() || retryDue)
                {
                    candidates.push_back(&m_manifest->entries[found->second]);
                }
            }
        }
    }

    // Nearest first; ties by key, so plans are deterministic.
    std::sort(candidates.begin(), candidates.end(), [this](const ManifestEntry* a, const ManifestEntry* b)
    {
        const std::int64_t distanceA = Chebyshev(a->key, m_centre);
        const std::int64_t distanceB = Chebyshev(b->key, m_centre);
        return distanceA != distanceB ? distanceA < distanceB : a->key < b->key;
    });

    for(std::size_t index = 0; index < candidates.size(); ++index)
    {
        const ManifestEntry& entry = *candidates[index];
        const auto pages = static_cast<std::uint32_t>((entry.brickCount + BricksPerPage - 1) / BricksPerPage);
        const std::int64_t distance = Chebyshev(entry.key, m_centre);

        // Make room by evicting resident regions farther than this one, farthest first.
        while(CommittedPages() + pages > m_pageCapacity || CommittedSlots() + 1 > m_slotCapacity)
        {
            auto farthest = m_records.end();

            for(auto record = m_records.begin(); record != m_records.end(); ++record)
            {
                const bool evictable = record->second.status == RegionRecord::Status::Resident && Chebyshev(record->first, m_centre) > distance;

                if(evictable && (farthest == m_records.end() || Chebyshev(record->first, m_centre) > Chebyshev(farthest->first, m_centre)))
                {
                    farthest = record;
                }
            }

            if(farthest == m_records.end())
            {
                break;
            }

            update.evict.push_back(farthest->first);
            m_records.erase(farthest);
        }

        if(CommittedPages() + pages > m_pageCapacity || CommittedSlots() + 1 > m_slotCapacity)
        {
            // Nothing farther to give up: this one and every farther candidate stay unknown.
            update.overBudget = candidates.size() - index;
            break;
        }

        RegionRecord& record = m_records[entry.key];
        record.status = RegionRecord::Status::Loading;
        record.reference = entry.reference;
        record.inputHash = entry.inputHash;
        record.pages = pages;
        update.load.push_back({entry.key, entry.reference, entry.inputHash, pages});
    }

    return update;
}

void Planner::OnLoaded(const RegionKey& key, std::uint32_t indexSlot)
{
    const auto record = m_records.find(key);

    if(record != m_records.end() && record->second.status == RegionRecord::Status::Loading)
    {
        record->second.status = RegionRecord::Status::Resident;
        record->second.indexSlot = indexSlot;
        record->second.attempts = 0;
        record->second.message.clear();
    }
}

void Planner::OnLoadFailed(const RegionKey& key, bool transient, double now, const std::string& message)
{
    const auto record = m_records.find(key);

    if(record == m_records.end())
    {
        return;
    }

    RegionRecord& failed = record->second;
    failed.status = RegionRecord::Status::Failed;
    failed.message = message;
    failed.permanent = !transient || failed.attempts >= m_settings.retryDelaysSeconds.size();

    if(!failed.permanent)
    {
        failed.retryAt = now + m_settings.retryDelaysSeconds[failed.attempts];
        ++failed.attempts;
    }
}

bool Planner::IsWanted(const RegionKey& key, const std::string& reference) const
{
    const auto record = m_records.find(key);
    return record != m_records.end() && record->second.status == RegionRecord::Status::Loading && record->second.reference == reference;
}

std::vector<TableEntry> Planner::BuildTable() const
{
    std::vector<TableEntry> table(std::size_t{m_window} * m_window * m_window);

    if(!m_manifest || !m_hasCamera)
    {
        return table;
    }

    const std::int32_t half = static_cast<std::int32_t>((m_window - 1) / 2);

    for(std::int32_t z = m_centre[2] - half; z <= m_centre[2] + half; ++z)
    {
        for(std::int32_t y = m_centre[1] - half; y <= m_centre[1] + half; ++y)
        {
            for(std::int32_t x = m_centre[0] - half; x <= m_centre[0] + half; ++x)
            {
                const RegionKey key{0, x, y, z};
                RegionState state = RegionState::Unknown;
                std::uint32_t slot = 0;
                const auto found = m_entries.find(key);

                if(!InExtent(key))
                {
                    state = RegionState::Unknown;
                }
                else if(found == m_entries.end())
                {
                    state = RegionState::KnownEmpty;
                }
                else if(m_manifest->entries[found->second].kind == RegionKind::UniformInside)
                {
                    state = RegionState::UniformInside;
                }
                else if(const auto record = m_records.find(key); record != m_records.end() && record->second.status == RegionRecord::Status::Resident)
                {
                    state = RegionState::Populated;
                    slot = record->second.indexSlot;
                }

                table[TableSlot(key, m_window)] = MakeEntry(key, state, slot);
            }
        }
    }

    return table;
}
}
