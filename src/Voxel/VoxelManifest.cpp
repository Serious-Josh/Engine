#include "Voxel/VoxelManifest.h"

#include "Voxel/VoxelBinary.h"

#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace Voxel
{
namespace
{
    using namespace ManifestFormat;

    constexpr std::array<char, 8> Magic{'E', 'N', 'G', 'V', 'O', 'X', 'M', 'F'};
    constexpr std::array<std::int32_t, 3> EmptyMinimum{0, 0, 0};
    constexpr std::array<std::int32_t, 3> EmptyMaximum{-1, -1, -1};

    // References are opaque here, but must be printable ASCII without spaces: they are asset references, never paths.
    bool IsValidReference(std::string_view reference) noexcept
    {
        return !reference.empty() && reference.size() <= MaximumReferenceBytes &&
            std::all_of(reference.begin(), reference.end(), [](char c) { return c > 0x20 && c < 0x7F; });
    }

    bool InExtent(const RegionKey& key, const ManifestData& data) noexcept
    {
        const std::int32_t coordinates[3] = {key.x, key.y, key.z};

        for(int axis = 0; axis < 3; ++axis)
        {
            if(coordinates[axis] < data.minimumRegion[axis] || coordinates[axis] > data.maximumRegion[axis])
            {
                return false;
            }
        }

        return true;
    }

    std::string ValidateEntry(const ManifestEntry& entry, const ManifestData& data)
    {
        if(!IsValidRegionKey(entry.key) || entry.key.level >= data.levelsBaked)
        {
            return "Manifest entry key or level is out of range.";
        }

        if(!InExtent(entry.key, data))
        {
            return "Manifest entry lies outside the baked extent.";
        }

        switch(entry.kind)
        {
        case RegionKind::Populated:
        {
            const std::uint64_t cells = std::uint64_t{data.regionBricks} * data.regionBricks * data.regionBricks;

            if(entry.brickCount == 0 || entry.brickCount > cells)
            {
                return "Populated manifest entry has an invalid brick count.";
            }

            if(entry.byteSize < RegionFormat::EncodedBytes(data.regionBricks, entry.brickCount) ||
                entry.byteSize > RegionFormat::MaximumBytes)
            {
                return "Populated manifest entry has an invalid byte size.";
            }

            if(!IsValidReference(entry.reference))
            {
                return "Populated manifest entry has an invalid reference.";
            }

            return {};
        }
        case RegionKind::UniformInside:
            if(entry.brickCount != 0 || entry.byteSize != 0 || !entry.reference.empty())
            {
                return "Uniform Inside manifest entry carries region data.";
            }

            return {};
        }

        return "Manifest entry kind is unknown.";
    }

    std::uint64_t EntryBytes(const ManifestEntry& entry) noexcept
    {
        return MinimumEntryBytes + entry.reference.size();
    }
}

void SortManifestEntries(ManifestData& data)
{
    std::sort(data.entries.begin(), data.entries.end(),
        [](const ManifestEntry& left, const ManifestEntry& right) { return left.key < right.key; });
}

std::string ValidateManifest(const ManifestData& data)
{
    if(!IsValidBaseExponent(data.baseExponent) || !RegionBricksLog2(data.regionBricks) ||
        data.narrowBand < RegionFormat::MinimumNarrowBand || data.narrowBand > RegionFormat::MaximumNarrowBand ||
        data.coarseClamp < RegionFormat::MinimumCoarseClamp || data.coarseClamp > RegionFormat::MaximumCoarseClamp)
    {
        return "Manifest settings are out of range.";
    }

    if(data.levelsBaked != SupportedLevelsBaked)
    {
        return "Manifest bakes an unsupported number of levels.";
    }

    if(data.entries.size() > MaximumEntries)
    {
        return "Manifest has too many entries.";
    }

    const bool empty = data.minimumRegion == EmptyMinimum && data.maximumRegion == EmptyMaximum;

    if(empty)
    {
        if(!data.entries.empty())
        {
            return "Manifest with an empty extent has entries.";
        }
    }
    else
    {
        for(int axis = 0; axis < 3; ++axis)
        {
            if(!IsValidRegionCoordinate(data.minimumRegion[axis]) || !IsValidRegionCoordinate(data.maximumRegion[axis]) ||
                data.minimumRegion[axis] > data.maximumRegion[axis])
            {
                return "Manifest extent is invalid.";
            }
        }
    }

    std::vector<std::string_view> references;

    for(std::size_t index = 0; index < data.entries.size(); ++index)
    {
        const ManifestEntry& entry = data.entries[index];

        if(std::string error = ValidateEntry(entry, data); !error.empty())
        {
            return error;
        }

        if(index > 0 && !(data.entries[index - 1].key < entry.key))
        {
            return "Manifest entries are not strictly ascending.";
        }

        if(entry.kind == RegionKind::Populated)
        {
            references.push_back(entry.reference);
        }
    }

    std::sort(references.begin(), references.end());

    if(std::adjacent_find(references.begin(), references.end()) != references.end())
    {
        return "Manifest references one region asset twice.";
    }

    return {};
}

std::vector<std::byte> EncodeManifest(const ManifestData& data)
{
    if(const std::string error = ValidateManifest(data); !error.empty())
    {
        throw std::invalid_argument("Voxel manifest is invalid: " + error);
    }

    std::uint64_t total = HeaderBytes + Binary::ChecksumBytes;

    for(const ManifestEntry& entry : data.entries)
    {
        total += EntryBytes(entry);
    }

    if(total > MaximumBytes)
    {
        throw std::length_error("Voxel manifest exceeds the maximum file size.");
    }

    Binary::Writer writer(static_cast<std::size_t>(total));
    writer.Bytes(Magic.data(), Magic.size());
    writer.U32(FormatVersion);
    writer.I8(data.baseExponent);
    writer.U8(BrickVoxels);
    writer.U8(data.regionBricks);
    writer.U8(data.narrowBand);
    writer.U8(data.coarseClamp);
    writer.U8(data.levelsBaked);
    writer.U16(0);
    writer.U64(data.participationHash);
    writer.U64(data.settingsHash);

    for(const auto* bound : {&data.minimumRegion, &data.maximumRegion})
    {
        for(const std::int32_t value : *bound)
        {
            writer.I32(value);
        }
    }

    writer.U32(static_cast<std::uint32_t>(data.entries.size()));

    for(const ManifestEntry& entry : data.entries)
    {
        writer.U8(entry.key.level);
        writer.U8(static_cast<std::uint8_t>(entry.kind));
        writer.U16(0);
        writer.U64(*PackRegionCoordinates(entry.key));
        writer.U64(entry.inputHash);
        writer.U32(entry.brickCount);
        writer.U64(entry.byteSize);
        writer.U32(static_cast<std::uint32_t>(entry.reference.size()));
        writer.Bytes(entry.reference.data(), entry.reference.size());
    }

    return writer.Finish();
}

DecodeResult DecodeManifest(std::span<const std::byte> bytes, ManifestData& data)
{
    const auto fail = [](DecodeError error, std::string message) { return DecodeResult{error, std::move(message)}; };

    if(bytes.size() > MaximumBytes)
    {
        return fail(DecodeError::TooLarge, "Voxel manifest exceeds the maximum file size.");
    }

    if(bytes.size() < HeaderBytes + Binary::ChecksumBytes || std::memcmp(bytes.data(), Magic.data(), Magic.size()) != 0)
    {
        return fail(DecodeError::NotVoxelFormat, "Not a voxel manifest.");
    }

    if(Binary::LoadU32(bytes, Magic.size()) != FormatVersion)
    {
        return fail(DecodeError::UnsupportedVersion, "Unsupported voxel manifest format version.");
    }

    const auto body = bytes.first(bytes.size() - Binary::ChecksumBytes);

    if(Binary::LoadU64(bytes, body.size()) != Binary::Fnv1a(body))
    {
        return fail(DecodeError::Corrupt, "Voxel manifest checksum mismatch.");
    }

    Binary::Reader reader(body);
    reader.Take(Magic.size() + 4);

    ManifestData decoded;
    decoded.baseExponent = reader.I8();
    const std::uint8_t brickSize = reader.U8();
    decoded.regionBricks = reader.U8();
    decoded.narrowBand = reader.U8();
    decoded.coarseClamp = reader.U8();
    decoded.levelsBaked = reader.U8();
    const std::uint16_t reserved = reader.U16();
    decoded.participationHash = reader.U64();
    decoded.settingsHash = reader.U64();

    for(auto* bound : {&decoded.minimumRegion, &decoded.maximumRegion})
    {
        for(std::int32_t& value : *bound)
        {
            value = reader.I32();
        }
    }

    const std::uint32_t count = reader.U32();

    if(!reader.ok || brickSize != BrickVoxels || reserved != 0)
    {
        return fail(DecodeError::Invalid, "Voxel manifest header is invalid.");
    }

    // Bounds the reservation below by what the input can actually hold.
    if(count > MaximumEntries || std::uint64_t{count} * MinimumEntryBytes > reader.Remaining())
    {
        return fail(DecodeError::Invalid, "Voxel manifest entry count is out of range.");
    }

    decoded.entries.reserve(count);

    for(std::uint32_t index = 0; index < count; ++index)
    {
        const std::uint8_t level = reader.U8();
        const std::uint8_t kind = reader.U8();
        const std::uint16_t entryReserved = reader.U16();
        const std::uint64_t packed = reader.U64();
        ManifestEntry entry;
        entry.inputHash = reader.U64();
        entry.brickCount = reader.U32();
        entry.byteSize = reader.U64();
        const std::uint32_t referenceLength = reader.U32();

        const auto key = UnpackRegionCoordinates(level, packed);

        if(!reader.ok || !key || entryReserved != 0 || kind > static_cast<std::uint8_t>(RegionKind::UniformInside) ||
            referenceLength > MaximumReferenceBytes)
        {
            return fail(DecodeError::Invalid, "Voxel manifest entry " + std::to_string(index) + " is invalid.");
        }

        const auto reference = reader.Take(referenceLength);

        if(!reader.ok)
        {
            return fail(DecodeError::Invalid, "Voxel manifest entry " + std::to_string(index) + " is truncated.");
        }

        entry.key = *key;
        entry.kind = static_cast<RegionKind>(kind);
        entry.reference.assign(reinterpret_cast<const char*>(reference.data()), reference.size());
        decoded.entries.push_back(std::move(entry));
    }

    if(reader.Remaining() != 0)
    {
        return fail(DecodeError::Invalid, "Voxel manifest has trailing bytes.");
    }

    if(std::string error = ValidateManifest(decoded); !error.empty())
    {
        return fail(DecodeError::Invalid, std::move(error));
    }

    data = std::move(decoded);
    return {};
}
}
