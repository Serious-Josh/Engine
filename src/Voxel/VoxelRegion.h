#pragma once

// VX.1: one baked region of the static voxel world, and its `.voxelregion` byte format. See
// Documentation/Voxel/VoxelArchitecture.md, sections 3.1-3.3 and 5.
//
// A region is R^3 brick cells (R = 16, 32 or 64). A cell either holds an allocated 8^3 brick, which happens exactly when the brick
// contains a surface-occupied voxel, or is uniform: every voxel Outside, or every voxel Inside a closed solid. Classification is
// Outside or Inside only; open geometry affects occupancy and distance, never classification. A region with no allocated brick
// is uniform throughout and is never stored as a region asset: the manifest records it instead (absent, or UniformInside).
//
// Layout, little-endian:
//   "ENGVOXRG", u32 format version
//   u64 packed region coordinates (PackRegionCoordinates), u8 level, i8 base exponent, u8 brick size (8), u8 bricks per axis (R)
//   u8 narrow band (voxels), u8 coarse clamp (bricks), u16 reserved (0)
//   u64 input hash, u64 settings hash, u32 brick count, u32 channel count
//   channel count * {u32 channel id, u16 element format, u16 flags, u64 byte offset, u64 byte size}
//   channel sections, contiguous, in table order, starting right after the table
//   u64 FNV-1a of everything before it
// Channel ids are strictly ascending. Readers skip an unknown channel unless it is flagged required, which fails explicitly.
//
// Decode is all or nothing: the caller's Data changes only when every check has passed. Encode writes only what Decode accepts.
// The encoding is canonical: one valid byte sequence per Data, with brick slots numbered in ascending cell order.

#include "Voxel/VoxelLattice.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Voxel
{
enum class DecodeError
{
    None,
    // Not this format: wrong magic, or too short to hold a header.
    NotVoxelFormat,
    UnsupportedVersion,
    // Larger than the format's maximum; rejected before reading anything else.
    TooLarge,
    // Checksum mismatch.
    Corrupt,
    // A channel this reader does not know is flagged required.
    UnknownRequiredChannel,
    // Well-formed bytes whose contents break the layout or contradict each other.
    Invalid
};

struct DecodeResult
{
    DecodeError error = DecodeError::None;
    std::string message;

    explicit operator bool() const noexcept
    {
        return error == DecodeError::None;
    }
};

// Uniform cell values. Any other index value is a brick slot.
inline constexpr std::uint32_t UniformOutside = 0xFFFFFFFFu;
inline constexpr std::uint32_t UniformInside = 0xFFFFFFFEu;
// Reserved: formerly Ambiguous (removed in VX.0), and the GPU-only streaming sentinel. Never valid in an asset.
inline constexpr std::uint32_t ReservedAmbiguous = 0xFFFFFFFDu;
inline constexpr std::uint32_t ReservedNotResident = 0xFFFFFFFCu;

// 512 bits per brick, one per voxel: bit (x + 8y + 64z), so word z holds one 8 x 8 slice.
using BrickMask = std::array<std::uint64_t, 8>;

namespace RegionFormat
{
inline constexpr std::uint32_t FormatVersion = 1;
inline constexpr std::string_view Extension = ".voxelregion";
// Above the R = 64 worst case (~298 MB) with headroom; also the cap on untrusted input size.
inline constexpr std::size_t MaximumBytes = std::size_t{512} << 20;
inline constexpr std::uint32_t MaximumChannels = 64;

inline constexpr int MinimumNarrowBand = 1;
inline constexpr int MaximumNarrowBand = 32;
inline constexpr int MinimumCoarseClamp = 1;
inline constexpr int MaximumCoarseClamp = 16;

enum class ChannelId : std::uint32_t
{
    BrickIndex = 1,
    CoarseDistance = 2,
    BrickCells = 3,
    Occupancy = 4,
    Solid = 5,
    Distance = 6
};

enum class ElementFormat : std::uint16_t
{
    U32 = 1,
    U8 = 2,
    Mask512 = 3,
    // 10^3 signed bytes per brick: the 8^3 brick plus its apron, x fastest.
    Snorm8Apron = 4
};

// Channel flag bits. Every channel this version writes is required; unknown optional channels are skipped.
inline constexpr std::uint16_t ChannelRequired = 1;

inline constexpr std::size_t HeaderBytes = 52;
inline constexpr std::size_t ChannelEntryBytes = 24;
inline constexpr std::size_t BrickCellBytes = 4;
inline constexpr std::size_t BrickMaskBytes = 64;
inline constexpr std::size_t BytesPerBrick = BrickCellBytes + 2 * BrickMaskBytes + ApronBrickVoxelCount;

// The exact size of a region of `regionBricks` per axis and `brickCount` bricks as this version encodes it (no unknown channels).
std::uint64_t EncodedBytes(std::uint32_t regionBricks, std::uint32_t brickCount) noexcept;
}

// Quantised distance in a brick: round(clamp(d / band, -1, 1) * 127), d in voxels. -128 is never valid.
inline constexpr int DistanceQuantisationMaximum = 127;

struct RegionData
{
    RegionKey key;
    std::int8_t baseExponent = DefaultBaseExponent;
    // 16, 32 or 64.
    std::uint8_t regionBricks = 1u << DefaultRegionBricksLog2;
    std::uint8_t narrowBand = 4;
    std::uint8_t coarseClamp = 4;
    std::uint64_t inputHash = 0;
    std::uint64_t settingsHash = 0;

    // R^3 entries, cell (x, y, z) at x + R * (y + R * z): a brick slot or a uniform value.
    std::vector<std::uint32_t> brickIndex;
    // R^3 entries: Chebyshev distance in bricks to the nearest allocated brick, 0 exactly at allocated bricks, saturating at
    // coarseClamp + 1. Bricks of neighbouring regions count, so values may be below what this region alone implies, never above.
    std::vector<std::uint8_t> coarseDistance;
    // Per slot: the packed cell (x | y << 8 | z << 16). Redundant with brickIndex; checked against it.
    std::vector<std::uint32_t> brickCells;
    std::vector<BrickMask> occupancy;
    std::vector<BrickMask> solid;
    // Per slot: ApronBrickVoxelCount values; apron voxel (ax, ay, az) in [-1, 8] at (ax + 1) + 10 * ((ay + 1) + 10 * (az + 1)).
    std::vector<std::int8_t> distance;

    std::uint32_t GetBrickCount() const noexcept
    {
        return static_cast<std::uint32_t>(brickCells.size());
    }

    bool operator==(const RegionData&) const = default;
};

constexpr std::uint32_t PackBrickCell(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept
{
    return x | (y << 8) | (z << 16);
}

constexpr int BrickVoxelBit(int x, int y, int z) noexcept
{
    return x + BrickVoxels * (y + BrickVoxels * z);
}

constexpr bool TestMask(const BrickMask& mask, int x, int y, int z) noexcept
{
    const int bit = BrickVoxelBit(x, y, z);
    return ((mask[bit >> 6] >> (bit & 63)) & 1u) != 0;
}

constexpr int ApronIndex(int x, int y, int z) noexcept
{
    return (x + 1) + ApronBrickVoxels * ((y + 1) + ApronBrickVoxels * (z + 1));
}

// The smallest |q| a voxel that is not surface-occupied may store, and the largest an occupied one may: an unoccupied voxel
// centre is at least half a voxel from every triangle, an occupied one at most half a diagonal. Rounding slack is one step.
int MinimumUnoccupiedDistance(int narrowBand) noexcept;
int MaximumOccupiedDistance(int narrowBand) noexcept;

// Everything Decode requires of the contents. Empty when valid, otherwise the first violation.
std::string ValidateRegion(const RegionData& data);

// Throws std::invalid_argument for data ValidateRegion rejects, so nothing Decode would refuse is ever written.
std::vector<std::byte> EncodeRegion(const RegionData& data);

DecodeResult DecodeRegion(std::span<const std::byte> bytes, RegionData& data);
}
