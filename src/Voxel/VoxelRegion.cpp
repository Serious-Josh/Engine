#include "Voxel/VoxelRegion.h"

#include "Voxel/VoxelBinary.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <stdexcept>

namespace Voxel
{
namespace
{
    using namespace RegionFormat;

    constexpr std::array<char, 8> Magic{'E', 'N', 'G', 'V', 'O', 'X', 'R', 'G'};

    struct KnownChannel
    {
        ChannelId id;
        ElementFormat format;
    };

    // In table order. Every one is required.
    constexpr std::array<KnownChannel, 6> KnownChannels{{
        {ChannelId::BrickIndex, ElementFormat::U32},
        {ChannelId::CoarseDistance, ElementFormat::U8},
        {ChannelId::BrickCells, ElementFormat::U32},
        {ChannelId::Occupancy, ElementFormat::Mask512},
        {ChannelId::Solid, ElementFormat::Mask512},
        {ChannelId::Distance, ElementFormat::Snorm8Apron},
    }};

    std::uint64_t ChannelBytes(ChannelId id, std::uint64_t cells, std::uint64_t bricks) noexcept
    {
        switch(id)
        {
        case ChannelId::BrickIndex:
            return cells * 4;
        case ChannelId::CoarseDistance:
            return cells;
        case ChannelId::BrickCells:
            return bricks * BrickCellBytes;
        case ChannelId::Occupancy:
        case ChannelId::Solid:
            return bricks * BrickMaskBytes;
        case ChannelId::Distance:
            return bricks * ApronBrickVoxelCount;
        }

        return 0;
    }

    bool IsUniform(std::uint32_t value) noexcept
    {
        return value == UniformOutside || value == UniformInside;
    }

    // Region cells, addressed by brick-cell coordinates.
    class CellGrid
    {
    public:
        explicit CellGrid(const RegionData& data) :
            m_data(data),
            m_size(data.regionBricks)
        {
        }

        int Size() const noexcept
        {
            return m_size;
        }

        bool Contains(int x, int y, int z) const noexcept
        {
            return x >= 0 && y >= 0 && z >= 0 && x < m_size && y < m_size && z < m_size;
        }

        std::size_t Linear(int x, int y, int z) const noexcept
        {
            return static_cast<std::size_t>(x) + static_cast<std::size_t>(m_size) * (static_cast<std::size_t>(y) +
                static_cast<std::size_t>(m_size) * static_cast<std::size_t>(z));
        }

        std::uint32_t Value(int x, int y, int z) const noexcept
        {
            return m_data.brickIndex[Linear(x, y, z)];
        }

    private:
        const RegionData& m_data;
        int m_size;
    };

    std::string ValidateSettings(const RegionData& data)
    {
        if(!IsValidRegionKey(data.key))
        {
            return "Region key is outside the lattice range.";
        }

        if(!IsValidBaseExponent(data.baseExponent))
        {
            return "Base exponent is outside the supported range.";
        }

        if(!RegionBricksLog2(data.regionBricks))
        {
            return "Bricks per region axis must be 16, 32 or 64.";
        }

        if(data.narrowBand < MinimumNarrowBand || data.narrowBand > MaximumNarrowBand)
        {
            return "Narrow band is outside the supported range.";
        }

        if(data.coarseClamp < MinimumCoarseClamp || data.coarseClamp > MaximumCoarseClamp)
        {
            return "Coarse clamp is outside the supported range.";
        }

        return {};
    }

    // Sizes, the slot numbering (canonical: ascending cell order) and the redundant cell list.
    std::string ValidateIndex(const RegionData& data, const CellGrid& grid)
    {
        const std::size_t cells = static_cast<std::size_t>(grid.Size()) * grid.Size() * grid.Size();
        const std::size_t bricks = data.brickCells.size();

        if(data.brickIndex.size() != cells || data.coarseDistance.size() != cells)
        {
            return "Brick index or coarse distance does not hold one entry per cell.";
        }

        if(bricks == 0)
        {
            return "Region has no allocated brick; a uniform region belongs in the manifest, not in a region asset.";
        }

        if(bricks > cells || data.occupancy.size() != bricks || data.solid.size() != bricks ||
            data.distance.size() != bricks * ApronBrickVoxelCount)
        {
            return "Brick channel sizes do not match the brick count.";
        }

        std::uint32_t next = 0;

        for(int z = 0; z < grid.Size(); ++z)
        {
            for(int y = 0; y < grid.Size(); ++y)
            {
                for(int x = 0; x < grid.Size(); ++x)
                {
                    const std::uint32_t value = grid.Value(x, y, z);

                    if(IsUniform(value))
                    {
                        continue;
                    }

                    if(value == ReservedAmbiguous || value == ReservedNotResident)
                    {
                        return "Brick index holds a reserved value.";
                    }

                    if(value != next || next >= bricks)
                    {
                        return "Brick slots are not numbered in ascending cell order.";
                    }

                    if(data.brickCells[next] != PackBrickCell(x, y, z))
                    {
                        return "Brick cell list does not match the brick index.";
                    }

                    ++next;
                }
            }
        }

        if(next != bricks)
        {
            return "A brick is not referenced by the brick index.";
        }

        for(const BrickMask& mask : data.occupancy)
        {
            if(std::all_of(mask.begin(), mask.end(), [](std::uint64_t word) { return word == 0; }))
            {
                return "An allocated brick has no surface-occupied voxel.";
            }
        }

        return {};
    }

    // Coarse distance is 0 exactly at allocated bricks, and elsewhere at most the saturated Chebyshev distance to the nearest
    // allocated brick of this region (bricks in neighbouring regions may be nearer, never farther).
    std::string ValidateCoarse(const RegionData& data, const CellGrid& grid)
    {
        const int size = grid.Size();
        const int saturated = data.coarseClamp + 1;
        std::vector<std::uint8_t> bound(data.brickIndex.size());

        for(std::size_t cell = 0; cell < bound.size(); ++cell)
        {
            bound[cell] = IsUniform(data.brickIndex[cell]) ? static_cast<std::uint8_t>(saturated) : 0;
        }

        // Each pass extends the in-region distance by one brick: a 3 x 3 x 3 minimum, separable per axis.
        std::vector<std::uint8_t> window(bound.size());

        for(int pass = 0; pass < data.coarseClamp; ++pass)
        {
            window = bound;

            for(int axis = 0; axis < 3; ++axis)
            {
                std::vector<std::uint8_t> next(window.size());

                for(int z = 0; z < size; ++z)
                {
                    for(int y = 0; y < size; ++y)
                    {
                        for(int x = 0; x < size; ++x)
                        {
                            const int position[3] = {x, y, z};
                            std::uint8_t value = window[grid.Linear(x, y, z)];

                            for(int step = -1; step <= 1; step += 2)
                            {
                                int neighbour[3] = {x, y, z};
                                neighbour[axis] = position[axis] + step;

                                if(grid.Contains(neighbour[0], neighbour[1], neighbour[2]))
                                {
                                    value = std::min(value, window[grid.Linear(neighbour[0], neighbour[1], neighbour[2])]);
                                }
                            }

                            next[grid.Linear(x, y, z)] = value;
                        }
                    }
                }

                window.swap(next);
            }

            for(std::size_t cell = 0; cell < bound.size(); ++cell)
            {
                bound[cell] = std::min<std::uint8_t>(bound[cell], static_cast<std::uint8_t>(std::min(window[cell] + 1, saturated)));
            }
        }

        for(std::size_t cell = 0; cell < bound.size(); ++cell)
        {
            const std::uint8_t value = data.coarseDistance[cell];

            if(!IsUniform(data.brickIndex[cell]))
            {
                if(value != 0)
                {
                    return "Coarse distance of an allocated brick is not zero.";
                }
            }
            else if(value == 0 || value > saturated || value > bound[cell])
            {
                return "Coarse distance contradicts the allocated bricks.";
            }
        }

        return {};
    }

    // Classification can change only across a surface-occupied voxel, so two face-adjacent voxels that are both unoccupied
    // must have the same class. Checked within bricks, between bricks, between bricks and uniform cells, and between uniform
    // cells.
    std::string ValidateClassification(const RegionData& data, const CellGrid& grid)
    {
        constexpr std::uint64_t NotLastColumn = 0x7F7F7F7F7F7F7F7Full;
        constexpr std::uint64_t NotLastRow = 0x00FFFFFFFFFFFFFFull;

        for(std::size_t slot = 0; slot < data.brickCells.size(); ++slot)
        {
            const BrickMask& occupied = data.occupancy[slot];
            const BrickMask& solid = data.solid[slot];

            for(int word = 0; word < 8; ++word)
            {
                const std::uint64_t empty = ~occupied[word];
                const std::uint64_t alongX = empty & (empty >> 1) & NotLastColumn & (solid[word] ^ (solid[word] >> 1));
                const std::uint64_t alongY = empty & (empty >> 8) & NotLastRow & (solid[word] ^ (solid[word] >> 8));
                const std::uint64_t alongZ = word < 7 ? empty & ~occupied[word + 1] & (solid[word] ^ solid[word + 1]) : 0;

                if(alongX != 0 || alongY != 0 || alongZ != 0)
                {
                    return "Adjacent unoccupied voxels within a brick have different classifications.";
                }
            }
        }

        const int size = grid.Size();

        // A voxel's class, and whether it is unoccupied, in a cell that is uniform or allocated.
        const auto voxelClass = [&data](std::uint32_t value, const int* voxel, bool& unoccupied) {
            if(IsUniform(value))
            {
                unoccupied = true;
                return value == UniformInside;
            }

            unoccupied = !TestMask(data.occupancy[value], voxel[0], voxel[1], voxel[2]);
            return TestMask(data.solid[value], voxel[0], voxel[1], voxel[2]);
        };

        for(int z = 0; z < size; ++z)
        {
            for(int y = 0; y < size; ++y)
            {
                for(int x = 0; x < size; ++x)
                {
                    const std::uint32_t here = grid.Value(x, y, z);

                    for(int axis = 0; axis < 3; ++axis)
                    {
                        const int nx = x + (axis == 0);
                        const int ny = y + (axis == 1);
                        const int nz = z + (axis == 2);

                        if(!grid.Contains(nx, ny, nz))
                        {
                            continue;
                        }

                        const std::uint32_t there = grid.Value(nx, ny, nz);

                        if(IsUniform(here) && IsUniform(there))
                        {
                            if(here != there)
                            {
                                return "Adjacent uniform cells have different classifications without a surface between them.";
                            }

                            continue;
                        }

                        // The touching faces: this cell's last layer along `axis`, the neighbour's first. (u, v) runs over
                        // the two other axes.
                        const int otherAxis0 = axis == 0 ? 1 : 0;
                        const int otherAxis1 = axis == 2 ? 1 : 2;

                        for(int v = 0; v < BrickVoxels; ++v)
                        {
                            for(int u = 0; u < BrickVoxels; ++u)
                            {
                                int low[3];
                                int high[3];
                                low[otherAxis0] = high[otherAxis0] = u;
                                low[otherAxis1] = high[otherAxis1] = v;
                                low[axis] = BrickVoxels - 1;
                                high[axis] = 0;

                                bool lowEmpty = false;
                                bool highEmpty = false;
                                const bool lowSolid = voxelClass(here, low, lowEmpty);
                                const bool highSolid = voxelClass(there, high, highEmpty);

                                if(lowEmpty && highEmpty && lowSolid != highSolid)
                                {
                                    return "Adjacent unoccupied voxels across a brick face have different classifications.";
                                }
                            }
                        }
                    }
                }
            }
        }

        return {};
    }

    // Interior values agree with occupancy and classification; apron values equal the neighbouring brick's interior value,
    // or agree with a uniform neighbour's class. Apron voxels in another region are not checked here.
    std::string ValidateDistance(const RegionData& data, const CellGrid& grid)
    {
        const int minimumEmpty = MinimumUnoccupiedDistance(data.narrowBand);
        const int maximumOccupied = MaximumOccupiedDistance(data.narrowBand);
        const int regionVoxels = grid.Size() * BrickVoxels;

        for(std::size_t slot = 0; slot < data.brickCells.size(); ++slot)
        {
            const std::int8_t* values = data.distance.data() + slot * ApronBrickVoxelCount;
            const std::uint32_t packed = data.brickCells[slot];
            const int cell[3] = {static_cast<int>(packed & 0xFF), static_cast<int>((packed >> 8) & 0xFF),
                static_cast<int>((packed >> 16) & 0xFF)};

            for(int az = -1; az <= BrickVoxels; ++az)
            {
                for(int ay = -1; ay <= BrickVoxels; ++ay)
                {
                    for(int ax = -1; ax <= BrickVoxels; ++ax)
                    {
                        const int value = values[ApronIndex(ax, ay, az)];

                        if(value < -DistanceQuantisationMaximum)
                        {
                            return "Distance holds -128.";
                        }

                        const bool interior = ax >= 0 && ay >= 0 && az >= 0 && ax < BrickVoxels && ay < BrickVoxels && az < BrickVoxels;

                        if(interior)
                        {
                            const bool occupied = TestMask(data.occupancy[slot], ax, ay, az);
                            const bool solid = TestMask(data.solid[slot], ax, ay, az);

                            if((solid && value > 0) || (!solid && value < 0))
                            {
                                return "Distance sign contradicts the solid classification.";
                            }

                            if((occupied && std::abs(value) > maximumOccupied) || (!occupied && std::abs(value) < minimumEmpty))
                            {
                                return "Distance magnitude contradicts surface occupancy.";
                            }

                            continue;
                        }

                        const int voxel[3] = {cell[0] * BrickVoxels + ax, cell[1] * BrickVoxels + ay, cell[2] * BrickVoxels + az};

                        if(voxel[0] < 0 || voxel[1] < 0 || voxel[2] < 0 || voxel[0] >= regionVoxels || voxel[1] >= regionVoxels ||
                            voxel[2] >= regionVoxels)
                        {
                            continue;
                        }

                        const std::uint32_t neighbour = grid.Value(voxel[0] / BrickVoxels, voxel[1] / BrickVoxels, voxel[2] / BrickVoxels);

                        if(neighbour == UniformOutside)
                        {
                            if(value < minimumEmpty)
                            {
                                return "Apron distance contradicts a uniform Outside neighbour.";
                            }
                        }
                        else if(neighbour == UniformInside)
                        {
                            if(value > -minimumEmpty)
                            {
                                return "Apron distance contradicts a uniform Inside neighbour.";
                            }
                        }
                        else
                        {
                            const std::int8_t expected = data.distance[neighbour * ApronBrickVoxelCount +
                                ApronIndex(voxel[0] % BrickVoxels, voxel[1] % BrickVoxels, voxel[2] % BrickVoxels)];

                            if(value != expected)
                            {
                                return "Apron distance differs from the neighbouring brick.";
                            }
                        }
                    }
                }
            }
        }

        return {};
    }
}

std::uint64_t RegionFormat::EncodedBytes(std::uint32_t regionBricks, std::uint32_t brickCount) noexcept
{
    const std::uint64_t cells = std::uint64_t{regionBricks} * regionBricks * regionBricks;
    std::uint64_t total = HeaderBytes + KnownChannels.size() * ChannelEntryBytes + Binary::ChecksumBytes;

    for(const KnownChannel& channel : KnownChannels)
    {
        total += ChannelBytes(channel.id, cells, brickCount);
    }

    return total;
}

int MinimumUnoccupiedDistance(int narrowBand) noexcept
{
    // round(0.5 / band * 127) >= floor(63.5 / band); one step of slack for the bake's floating-point distance.
    return std::max(0, DistanceQuantisationMaximum / (2 * narrowBand) - 1);
}

int MaximumOccupiedDistance(int narrowBand) noexcept
{
    const double halfDiagonal = std::sqrt(3.0) / 2.0;
    return std::min(DistanceQuantisationMaximum,
        static_cast<int>(std::ceil(halfDiagonal / narrowBand * DistanceQuantisationMaximum)) + 1);
}

std::string ValidateRegion(const RegionData& data)
{
    std::string error = ValidateSettings(data);

    if(error.empty())
    {
        const CellGrid grid(data);

        if(error = ValidateIndex(data, grid); error.empty())
        {
            if(error = ValidateCoarse(data, grid); error.empty())
            {
                if(error = ValidateClassification(data, grid); error.empty())
                {
                    error = ValidateDistance(data, grid);
                }
            }
        }
    }

    return error;
}

std::vector<std::byte> EncodeRegion(const RegionData& data)
{
    if(const std::string error = ValidateRegion(data); !error.empty())
    {
        throw std::invalid_argument("Voxel region is invalid: " + error);
    }

    const std::uint32_t bricks = data.GetBrickCount();
    const std::uint64_t cells = std::uint64_t{data.regionBricks} * data.regionBricks * data.regionBricks;
    const std::uint64_t total = EncodedBytes(data.regionBricks, bricks);

    if(total > MaximumBytes)
    {
        throw std::length_error("Voxel region exceeds the maximum file size.");
    }

    Binary::Writer writer(static_cast<std::size_t>(total));
    writer.Bytes(Magic.data(), Magic.size());
    writer.U32(FormatVersion);
    writer.U64(*PackRegionCoordinates(data.key));
    writer.U8(data.key.level);
    writer.I8(data.baseExponent);
    writer.U8(BrickVoxels);
    writer.U8(data.regionBricks);
    writer.U8(data.narrowBand);
    writer.U8(data.coarseClamp);
    writer.U16(0);
    writer.U64(data.inputHash);
    writer.U64(data.settingsHash);
    writer.U32(bricks);
    writer.U32(static_cast<std::uint32_t>(KnownChannels.size()));

    std::uint64_t offset = HeaderBytes + KnownChannels.size() * ChannelEntryBytes;

    for(const KnownChannel& channel : KnownChannels)
    {
        const std::uint64_t size = ChannelBytes(channel.id, cells, bricks);
        writer.U32(static_cast<std::uint32_t>(channel.id));
        writer.U16(static_cast<std::uint16_t>(channel.format));
        writer.U16(ChannelRequired);
        writer.U64(offset);
        writer.U64(size);
        offset += size;
    }

    for(const std::uint32_t value : data.brickIndex)
    {
        writer.U32(value);
    }

    writer.Bytes(data.coarseDistance.data(), data.coarseDistance.size());

    for(const std::uint32_t value : data.brickCells)
    {
        writer.U32(value);
    }

    for(const auto* masks : {&data.occupancy, &data.solid})
    {
        for(const BrickMask& mask : *masks)
        {
            for(const std::uint64_t word : mask)
            {
                writer.U64(word);
            }
        }
    }

    writer.Bytes(data.distance.data(), data.distance.size());
    return writer.Finish();
}

DecodeResult DecodeRegion(std::span<const std::byte> bytes, RegionData& data)
{
    const auto fail = [](DecodeError error, std::string message) { return DecodeResult{error, std::move(message)}; };

    if(bytes.size() > MaximumBytes)
    {
        return fail(DecodeError::TooLarge, "Voxel region exceeds the maximum file size.");
    }

    if(bytes.size() < HeaderBytes + Binary::ChecksumBytes || std::memcmp(bytes.data(), Magic.data(), Magic.size()) != 0)
    {
        return fail(DecodeError::NotVoxelFormat, "Not a voxel region.");
    }

    if(Binary::LoadU32(bytes, Magic.size()) != FormatVersion)
    {
        return fail(DecodeError::UnsupportedVersion, "Unsupported voxel region format version.");
    }

    const auto body = bytes.first(bytes.size() - Binary::ChecksumBytes);

    if(Binary::LoadU64(bytes, body.size()) != Binary::Fnv1a(body))
    {
        return fail(DecodeError::Corrupt, "Voxel region checksum mismatch.");
    }

    Binary::Reader reader(body);
    reader.Take(Magic.size() + 4);
    const std::uint64_t packed = reader.U64();
    const std::uint8_t level = reader.U8();
    const std::int8_t baseExponent = reader.I8();
    const std::uint8_t brickSize = reader.U8();
    const std::uint8_t regionBricks = reader.U8();
    const std::uint8_t narrowBand = reader.U8();
    const std::uint8_t coarseClamp = reader.U8();
    const std::uint16_t reserved = reader.U16();
    const std::uint64_t inputHash = reader.U64();
    const std::uint64_t settingsHash = reader.U64();
    const std::uint32_t brickCount = reader.U32();
    const std::uint32_t channelCount = reader.U32();

    const auto key = UnpackRegionCoordinates(level, packed);

    if(!reader.ok || !key || brickSize != BrickVoxels || reserved != 0 || !RegionBricksLog2(regionBricks))
    {
        return fail(DecodeError::Invalid, "Voxel region header is invalid.");
    }

    const std::uint64_t cells = std::uint64_t{regionBricks} * regionBricks * regionBricks;

    if(brickCount == 0 || brickCount > cells)
    {
        return fail(DecodeError::Invalid, "Voxel region brick count is out of range.");
    }

    if(channelCount == 0 || channelCount > MaximumChannels)
    {
        return fail(DecodeError::Invalid, "Voxel region channel count is out of range.");
    }

    // Sections are contiguous in table order, starting right after the table and ending at the checksum.
    std::array<std::span<const std::byte>, KnownChannels.size()> sections;
    std::array<bool, KnownChannels.size()> present{};
    std::uint64_t expectedOffset = HeaderBytes + std::uint64_t{channelCount} * ChannelEntryBytes;
    std::uint32_t previousId = 0;

    if(expectedOffset > body.size())
    {
        return fail(DecodeError::Invalid, "Voxel region channel table is truncated.");
    }

    for(std::uint32_t index = 0; index < channelCount; ++index)
    {
        const std::uint32_t id = reader.U32();
        const std::uint16_t format = reader.U16();
        const std::uint16_t flags = reader.U16();
        const std::uint64_t offset = reader.U64();
        const std::uint64_t size = reader.U64();
        const auto end = Binary::CheckedAdd(offset, size);

        if(!reader.ok || id <= previousId || (flags & ~ChannelRequired) != 0 || offset != expectedOffset || !end || *end > body.size())
        {
            return fail(DecodeError::Invalid, "Voxel region channel table is invalid.");
        }

        previousId = id;
        expectedOffset = *end;

        const auto known = std::find_if(KnownChannels.begin(), KnownChannels.end(),
            [id](const KnownChannel& channel) { return static_cast<std::uint32_t>(channel.id) == id; });

        if(known == KnownChannels.end())
        {
            if((flags & ChannelRequired) != 0)
            {
                return fail(DecodeError::UnknownRequiredChannel, "Voxel region requires unknown channel " + std::to_string(id) + ".");
            }

            continue;
        }

        if(format != static_cast<std::uint16_t>(known->format) || flags != ChannelRequired ||
            size != ChannelBytes(known->id, cells, brickCount))
        {
            return fail(DecodeError::Invalid, "Voxel region channel " + std::to_string(id) + " has the wrong format or size.");
        }

        const std::size_t slot = static_cast<std::size_t>(known - KnownChannels.begin());
        sections[slot] = body.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
        present[slot] = true;
    }

    if(expectedOffset != body.size())
    {
        return fail(DecodeError::Invalid, "Voxel region sections do not end at the checksum.");
    }

    if(!std::all_of(present.begin(), present.end(), [](bool value) { return value; }))
    {
        return fail(DecodeError::Invalid, "Voxel region is missing a required channel.");
    }

    // Every allocation below is bounded by a section already inside the input.
    RegionData decoded;
    decoded.key = *key;
    decoded.baseExponent = baseExponent;
    decoded.regionBricks = regionBricks;
    decoded.narrowBand = narrowBand;
    decoded.coarseClamp = coarseClamp;
    decoded.inputHash = inputHash;
    decoded.settingsHash = settingsHash;

    decoded.brickIndex.resize(static_cast<std::size_t>(cells));
    for(std::size_t index = 0; index < decoded.brickIndex.size(); ++index)
    {
        decoded.brickIndex[index] = Binary::LoadU32(sections[0], index * 4);
    }

    decoded.coarseDistance.resize(static_cast<std::size_t>(cells));
    std::memcpy(decoded.coarseDistance.data(), sections[1].data(), sections[1].size());

    decoded.brickCells.resize(brickCount);
    for(std::size_t index = 0; index < decoded.brickCells.size(); ++index)
    {
        decoded.brickCells[index] = Binary::LoadU32(sections[2], index * 4);
    }

    for(const auto& [masks, section] : {std::pair{&decoded.occupancy, sections[3]}, std::pair{&decoded.solid, sections[4]}})
    {
        masks->resize(brickCount);
        for(std::size_t brick = 0; brick < brickCount; ++brick)
        {
            for(std::size_t word = 0; word < 8; ++word)
            {
                (*masks)[brick][word] = Binary::LoadU64(section, (brick * 8 + word) * 8);
            }
        }
    }

    decoded.distance.resize(sections[5].size());
    std::memcpy(decoded.distance.data(), sections[5].data(), sections[5].size());

    if(std::string error = ValidateRegion(decoded); !error.empty())
    {
        return fail(DecodeError::Invalid, std::move(error));
    }

    data = std::move(decoded);
    return {};
}
}
