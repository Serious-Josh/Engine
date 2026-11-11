#pragma once

// VX.1: the world voxel lattice. See Documentation/Voxel/VoxelArchitecture.md, section 2.
//
// Voxel size at level L is exactly 2^(baseExponent + L) metres, so world-to-lattice conversion is an exact power-of-two scale
// followed by floor. Every lattice step (voxel -> brick -> region, level L -> L + 1) is a floor division by a power of two,
// which is an arithmetic right shift on signed integers (defined behaviour since C++20). Never use `/` on lattice coordinates:
// it truncates towards zero, so -1 / 8 would land in brick 0 instead of brick -1.
//
// Regions at every level are aligned to the same world origin, so a level L + 1 region covers exactly 2^3 level L regions,
// a coarse brick exactly 2^3 fine bricks and a coarse voxel exactly 2^3 fine voxels.

#include <array>
#include <cmath>
#include <compare>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>

namespace Voxel
{
inline constexpr int BrickVoxelsLog2 = 3;
inline constexpr int BrickVoxels = 1 << BrickVoxelsLog2;
inline constexpr int BrickVoxelCount = BrickVoxels * BrickVoxels * BrickVoxels;
// Stored distance bricks carry a one-voxel apron on every side, so trilinear filtering never leaves the brick.
inline constexpr int ApronBrickVoxels = BrickVoxels + 2;
inline constexpr int ApronBrickVoxelCount = ApronBrickVoxels * ApronBrickVoxels * ApronBrickVoxels;

// Voxel size at level 0 is 2^baseExponent metres: 7.8 mm to 16 m.
inline constexpr int MinimumBaseExponent = -7;
inline constexpr int MaximumBaseExponent = 4;
inline constexpr int DefaultBaseExponent = -3;
inline constexpr int MaximumLevel = 15;

// Bricks per region axis is 2^log2: 16, 32 or 64.
inline constexpr int MinimumRegionBricksLog2 = 4;
inline constexpr int MaximumRegionBricksLog2 = 6;
inline constexpr int DefaultRegionBricksLog2 = 5;

// Region coordinates at every level lie in [-RegionCoordinateLimit, RegionCoordinateLimit - 1]: 21-bit signed.
inline constexpr std::int64_t RegionCoordinateLimit = std::int64_t{1} << 20;
inline constexpr int RegionCoordinateBits = 21;

// Voxel coordinates must fit int32.
inline constexpr std::int64_t MinimumVoxelCoordinate = (std::numeric_limits<std::int32_t>::min)();
inline constexpr std::int64_t MaximumVoxelCoordinate = (std::numeric_limits<std::int32_t>::max)();

enum class LatticeError
{
    NonFinite,
    OutOfRange,
    InvalidSettings
};

struct Coordinate
{
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;

    bool operator==(const Coordinate&) const = default;
};

constexpr bool IsValidBaseExponent(int exponent) noexcept
{
    return exponent >= MinimumBaseExponent && exponent <= MaximumBaseExponent;
}

constexpr bool IsValidLevel(int level) noexcept
{
    return level >= 0 && level <= MaximumLevel;
}

constexpr bool IsValidRegionBricksLog2(int log2) noexcept
{
    return log2 >= MinimumRegionBricksLog2 && log2 <= MaximumRegionBricksLog2;
}

// The log2 of a valid bricks-per-axis value (16, 32, 64), or nullopt.
constexpr std::optional<int> RegionBricksLog2(std::uint32_t regionBricks) noexcept
{
    for(int log2 = MinimumRegionBricksLog2; log2 <= MaximumRegionBricksLog2; ++log2)
    {
        if(regionBricks == (std::uint32_t{1} << log2))
        {
            return log2;
        }
    }

    return std::nullopt;
}

// Floor division by 2^shift. Arithmetic right shift rounds towards negative infinity.
constexpr std::int64_t FloorShift(std::int64_t value, int shift) noexcept
{
    return value >> shift;
}

constexpr Coordinate FloorShift(const Coordinate& value, int shift) noexcept
{
    return {FloorShift(value.x, shift), FloorShift(value.y, shift), FloorShift(value.z, shift)};
}

constexpr bool IsValidVoxelCoordinate(std::int64_t value) noexcept
{
    return value >= MinimumVoxelCoordinate && value <= MaximumVoxelCoordinate;
}

constexpr bool IsValidRegionCoordinate(std::int64_t value) noexcept
{
    return value >= -RegionCoordinateLimit && value < RegionCoordinateLimit;
}

// The voxel containing world coordinate `world` at voxel size 2^sizeExponent metres: floor of the exact quotient. -0.0 lies in
// voxel 0, and any negative value, however small, in voxel -1 or below.
inline std::expected<std::int64_t, LatticeError> WorldToVoxel(double world, int sizeExponent) noexcept
{
    if(!std::isfinite(world))
    {
        return std::unexpected(LatticeError::NonFinite);
    }

    // Scaling by a power of two is exact unless the result is subnormal. Such a result has magnitude far below one, so only
    // its sign matters, and underflow to -0.0 must still land a negative input in voxel -1.
    double scaled = std::floor(std::ldexp(world, -sizeExponent));

    if(world < 0.0 && scaled == 0.0)
    {
        scaled = -1.0;
    }

    // Compare as doubles before converting: the conversion itself is undefined outside the int64 range.
    if(scaled < static_cast<double>(MinimumVoxelCoordinate) || scaled > static_cast<double>(MaximumVoxelCoordinate))
    {
        return std::unexpected(LatticeError::OutOfRange);
    }

    return static_cast<std::int64_t>(scaled);
}

inline std::expected<Coordinate, LatticeError> WorldToVoxel(const std::array<double, 3>& world, int sizeExponent) noexcept
{
    Coordinate voxel;
    std::int64_t* axes[3] = {&voxel.x, &voxel.y, &voxel.z};

    for(int axis = 0; axis < 3; ++axis)
    {
        const auto value = WorldToVoxel(world[axis], sizeExponent);

        if(!value)
        {
            return std::unexpected(value.error());
        }

        *axes[axis] = *value;
    }

    return voxel;
}

// The world position of a voxel's minimum corner. Exact: voxel coordinates fit int32, so they and the power-of-two scale are
// represented exactly in a double.
inline double VoxelToWorld(std::int64_t voxel, int sizeExponent) noexcept
{
    return std::ldexp(static_cast<double>(voxel), sizeExponent);
}

constexpr Coordinate VoxelToBrick(const Coordinate& voxel) noexcept
{
    return FloorShift(voxel, BrickVoxelsLog2);
}

constexpr Coordinate BrickToRegion(const Coordinate& brick, int regionBricksLog2) noexcept
{
    return FloorShift(brick, regionBricksLog2);
}

constexpr Coordinate VoxelToRegion(const Coordinate& voxel, int regionBricksLog2) noexcept
{
    return FloorShift(voxel, BrickVoxelsLog2 + regionBricksLog2);
}

// A voxel's position within its region: always in [0, 8 * 2^regionBricksLog2).
constexpr Coordinate VoxelLocalToRegion(const Coordinate& voxel, int regionBricksLog2) noexcept
{
    const std::int64_t mask = (std::int64_t{1} << (BrickVoxelsLog2 + regionBricksLog2)) - 1;
    return {voxel.x & mask, voxel.y & mask, voxel.z & mask};
}

// The level L + 1 voxel, brick or region containing a level L one.
constexpr Coordinate ParentCoordinate(const Coordinate& value) noexcept
{
    return FloorShift(value, 1);
}

// One region at one level. Ordered by level, then x, y, z, which is also the order of PackRegionCoordinates.
struct RegionKey
{
    std::uint8_t level = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;

    auto operator<=>(const RegionKey&) const = default;
};

constexpr bool IsValidRegionKey(const RegionKey& key) noexcept
{
    return IsValidLevel(key.level) && IsValidRegionCoordinate(key.x) && IsValidRegionCoordinate(key.y) &&
        IsValidRegionCoordinate(key.z);
}

// Packs a key's coordinates, each biased by 2^20 into 21 bits: x in bits 42-62, y in 21-41, z in 0-20. Bit 63 is always zero.
// The level is not packed (3 x 21 + 8 bits do not fit a u64); formats store it beside the packed value. Packed values of one
// level order exactly as the coordinates do lexicographically.
constexpr std::optional<std::uint64_t> PackRegionCoordinates(const RegionKey& key) noexcept
{
    if(!IsValidRegionKey(key))
    {
        return std::nullopt;
    }

    const auto bias = [](std::int32_t value) {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(value) + RegionCoordinateLimit);
    };

    return (bias(key.x) << (2 * RegionCoordinateBits)) | (bias(key.y) << RegionCoordinateBits) | bias(key.z);
}

constexpr std::optional<RegionKey> UnpackRegionCoordinates(std::uint8_t level, std::uint64_t packed) noexcept
{
    if(!IsValidLevel(level) || (packed >> (3 * RegionCoordinateBits)) != 0)
    {
        return std::nullopt;
    }

    constexpr std::uint64_t mask = (std::uint64_t{1} << RegionCoordinateBits) - 1;
    const auto unbias = [](std::uint64_t value) {
        return static_cast<std::int32_t>(static_cast<std::int64_t>(value) - RegionCoordinateLimit);
    };

    return RegionKey{level, unbias((packed >> (2 * RegionCoordinateBits)) & mask), unbias((packed >> RegionCoordinateBits) & mask),
        unbias(packed & mask)};
}

// The region containing `voxel` (a coordinate at `level`), range-checked.
constexpr std::expected<RegionKey, LatticeError> RegionForVoxel(int level, const Coordinate& voxel, int regionBricksLog2) noexcept
{
    if(!IsValidLevel(level) || !IsValidRegionBricksLog2(regionBricksLog2))
    {
        return std::unexpected(LatticeError::InvalidSettings);
    }

    if(!IsValidVoxelCoordinate(voxel.x) || !IsValidVoxelCoordinate(voxel.y) || !IsValidVoxelCoordinate(voxel.z))
    {
        return std::unexpected(LatticeError::OutOfRange);
    }

    const Coordinate region = VoxelToRegion(voxel, regionBricksLog2);

    if(!IsValidRegionCoordinate(region.x) || !IsValidRegionCoordinate(region.y) || !IsValidRegionCoordinate(region.z))
    {
        return std::unexpected(LatticeError::OutOfRange);
    }

    return RegionKey{static_cast<std::uint8_t>(level), static_cast<std::int32_t>(region.x), static_cast<std::int32_t>(region.y),
        static_cast<std::int32_t>(region.z)};
}

// The voxel coordinate (at the key's level) of a region's minimum corner. Fits int64 for every valid key.
constexpr Coordinate RegionOriginVoxel(const RegionKey& key, int regionBricksLog2) noexcept
{
    const int shift = BrickVoxelsLog2 + regionBricksLog2;
    return {static_cast<std::int64_t>(key.x) * (std::int64_t{1} << shift), static_cast<std::int64_t>(key.y) * (std::int64_t{1} << shift),
        static_cast<std::int64_t>(key.z) * (std::int64_t{1} << shift)};
}

constexpr std::expected<RegionKey, LatticeError> ParentRegion(const RegionKey& key) noexcept
{
    if(!IsValidRegionKey(key) || key.level >= MaximumLevel)
    {
        return std::unexpected(LatticeError::OutOfRange);
    }

    return RegionKey{static_cast<std::uint8_t>(key.level + 1), static_cast<std::int32_t>(FloorShift(key.x, 1)),
        static_cast<std::int32_t>(FloorShift(key.y, 1)), static_cast<std::int32_t>(FloorShift(key.z, 1))};
}

// Child `index` (bit 0: +x, bit 1: +y, bit 2: +z) of a level L > 0 region. Fails when the child lies outside the coordinate
// range, which only happens for parents at the range's edge.
constexpr std::expected<RegionKey, LatticeError> ChildRegion(const RegionKey& key, int index) noexcept
{
    if(!IsValidRegionKey(key) || key.level == 0 || index < 0 || index > 7)
    {
        return std::unexpected(LatticeError::OutOfRange);
    }

    const RegionKey child{static_cast<std::uint8_t>(key.level - 1), static_cast<std::int32_t>(std::int64_t{key.x} * 2 + (index & 1)),
        static_cast<std::int32_t>(std::int64_t{key.y} * 2 + ((index >> 1) & 1)),
        static_cast<std::int32_t>(std::int64_t{key.z} * 2 + ((index >> 2) & 1))};

    if(!IsValidRegionKey(child))
    {
        return std::unexpected(LatticeError::OutOfRange);
    }

    return child;
}
}
