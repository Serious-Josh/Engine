#pragma once

// VX.1: a scene's voxel bake, the `.voxelmanifest` byte format. See Documentation/Voxel/VoxelArchitecture.md, section 3.4.
//
// The manifest lists the populated regions (each a `.voxelregion` asset reference) and the regions that are uniformly Inside (no
// asset). A region inside the extent but not listed is uniformly Outside; a region outside the extent is Unbaked.
//
// Layout, little-endian:
//   "ENGVOXMF", u32 format version
//   i8 base exponent, u8 brick size (8), u8 bricks per axis (R), u8 narrow band, u8 coarse clamp, u8 levels baked, u16 reserved (0)
//   u64 participation hash, u64 settings hash
//   i32 x 3 minimum region, i32 x 3 maximum region (inclusive; level 0). Empty extent: (0, 0, 0) and (-1, -1, -1), with no entries.
//   u32 entry count
//   entry count * {u8 level, u8 kind, u16 reserved (0), u64 packed region coordinates, u64 input hash, u32 brick count,
//                  u64 byte size, u32 reference length, reference bytes}
//   u64 FNV-1a of everything before it
// Entries are strictly ascending by (level, packed coordinates), which is also (level, x, y, z). V1 bakes level 0 only.
//
// Decode is all or nothing, and Encode writes only what Decode accepts.

#include "Voxel/VoxelRegion.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Voxel
{
namespace ManifestFormat
{
inline constexpr std::uint32_t FormatVersion = 1;
inline constexpr std::string_view Extension = ".voxelmanifest";
inline constexpr std::size_t MaximumEntries = std::size_t{1} << 22;
inline constexpr std::size_t MaximumReferenceBytes = 512;
// The cap on file size and on untrusted input size. A manifest is HeaderBytes + 8 + sum(MinimumEntryBytes + reference length), so the
// entry-count cap (2^22) is reachable only while references average at most 91 bytes ("asset:<uuid>" is 42); with longer
// references the size cap binds first and Encode throws std::length_error. Raised from 256 MiB in VX.3b; layout and version unchanged.
inline constexpr std::size_t MaximumBytes = std::size_t{512} << 20;
// V1 bakes level 0 only.
inline constexpr std::uint8_t SupportedLevelsBaked = 1;

inline constexpr std::size_t HeaderBytes = 64;
inline constexpr std::size_t MinimumEntryBytes = 36;
}

enum class RegionKind : std::uint8_t
{
    // A `.voxelregion` asset with at least one brick.
    Populated = 0,
    // Every voxel Inside a closed solid; no asset.
    UniformInside = 1
};

struct ManifestEntry
{
    RegionKey key;
    RegionKind kind = RegionKind::Populated;
    std::uint64_t inputHash = 0;
    // Populated only: the region asset's brick count and file size, and its reference ("asset:<uuid>").
    std::uint32_t brickCount = 0;
    std::uint64_t byteSize = 0;
    std::string reference;

    bool operator==(const ManifestEntry&) const = default;
};

struct ManifestData
{
    std::int8_t baseExponent = DefaultBaseExponent;
    std::uint8_t regionBricks = 1u << DefaultRegionBricksLog2;
    std::uint8_t narrowBand = 4;
    std::uint8_t coarseClamp = 4;
    std::uint8_t levelsBaked = ManifestFormat::SupportedLevelsBaked;
    std::uint64_t participationHash = 0;
    std::uint64_t settingsHash = 0;
    // Inclusive level 0 region bounds; EmptyExtent when nothing was baked.
    std::array<std::int32_t, 3> minimumRegion{0, 0, 0};
    std::array<std::int32_t, 3> maximumRegion{-1, -1, -1};
    std::vector<ManifestEntry> entries;

    bool operator==(const ManifestData&) const = default;
};

// Sorts entries into the canonical order Encode requires. Does not remove duplicates; validation rejects them.
void SortManifestEntries(ManifestData& data);

// Empty when valid, otherwise the first violation.
std::string ValidateManifest(const ManifestData& data);

// Throws std::invalid_argument for data ValidateManifest rejects, std::length_error above MaximumBytes.
std::vector<std::byte> EncodeManifest(const ManifestData& data);

DecodeResult DecodeManifest(std::span<const std::byte> bytes, ManifestData& data);
}
