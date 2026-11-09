#pragma once

#include "Scene/AudioComponents.h"
#include "assets/AssetRecord.h"
#include "assets/ProjectAssetRegistry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class ProjectPaths;

// M9.4: the baked pathing layer of one Probe Volume, in Content/Audio/Probes/<uuid>.bakedpathing. Immutable once stored: a rebake writes a
// new asset, and the previous one stays until the reference is moved and nothing else references it. Separate from .bakedreflections, so
// pathing and reflections go stale independently (pathing ignores materials and reflection settings), and its batch is registered only
// with the pathing simulator: a batch without the REVERB layer beside one with it crashes reflection runs (M9.0).
// The probe batch inside holds the probes it was baked from, so the source .probebatch is not needed at run time.
//
// Layout, little-endian:
//   "ENGPATHB", u32 format version, u32 Steam Audio version (major << 16 | minor << 8 | patch)
//   settings: u32 samples, f32 radius, f32 threshold, f32 visibility range, f32 path range
//   source probes: u64 probe fingerprint, u64 probe positions hash, u32 reference length, reference bytes
//   u64 geometry hash, u64 settings hash, u64 fingerprint
//   u32 probe count, u64 layer data size, u64 batch size
//   probe count * {f32 x, y, z, radius}
//   batch (iplProbeBatchSave's bytes, opaque here), u64 FNV-1a of everything before it
// Decoding checks every size, count and relationship, and the checksum, before any byte reaches Steam Audio: iplProbeBatchLoad is not
// safe on malformed data.
namespace BakedPathingAssets
{
inline constexpr std::uint32_t FormatVersion = 1;
inline constexpr std::uint32_t PackagedPayloadVersion = FormatVersion;
// The SDK this format's batches come from; asserted against phonon_version.h where the baker is.
inline constexpr std::uint32_t SteamAudioVersion = (4u << 16) | (8u << 8) | 1u;
inline constexpr std::size_t MaximumBytes = std::size_t{1} << 31;
inline constexpr std::size_t MaximumTextBytes = 512;
inline constexpr std::string_view Extension = ".bakedpathing";

struct Data
{
    std::uint32_t steamAudioVersion = SteamAudioVersion;
    PathingBakeSettings settings;
    // The probe asset baked from: its reference, its M7 fingerprint, and BakedReflectionsAssets::HashProbes of its probes.
    std::string sourceProbeBatch;
    std::uint64_t sourceProbeFingerprint = 0;
    std::uint64_t sourceProbesHash = 0;
    // What the layer was baked from, kept apart so a stale asset can say why (PathingBakes).
    std::uint64_t geometryHash = 0;
    std::uint64_t settingsHash = 0;
    // Combines the above with the format and SDK versions.
    std::uint64_t fingerprint = 0;
    // Engine-space x, y, z, radius, identical to the source probe asset's: probe coverage at run time, without loading Steam Audio.
    std::vector<std::array<float, 4>> probes;
    // iplProbeBatchGetDataSize of the pathing layer. Never zero: an empty or partial layer is never published.
    std::uint64_t layerBytes = 0;
    std::vector<std::byte> batch;

    bool operator==(const Data&) const = default;
};

enum class DecodeError
{
    None,
    // Not this format, or the wrong size.
    NotBakedPathing,
    UnsupportedVersion,
    UnsupportedSteamAudioVersion,
    Corrupt,
    // Well formed, but its contents contradict each other or the rules above.
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

// Throws std::length_error when `data` breaks a limit that Decode would reject; never produces an asset Decode refuses.
std::vector<std::byte> Encode(const Data& data);
DecodeResult Decode(std::span<const std::byte> bytes, Data& data);

DecodeResult LoadFromFile(const std::filesystem::path& path, Data& data);

struct Resolution
{
    // Missing: no registered file to read. The rest are Decode's.
    bool missing = false;
    DecodeResult decode;
    std::optional<Data> data;
};

Resolution Resolve(std::string_view reference, const ProjectPaths& paths, const ProjectAssetRegistry& registry);

// Writes `encoded` whole or not at all, as a new asset, and returns its reference. The registry is unchanged on failure.
std::optional<std::string> Store(const ProjectPaths& paths, ProjectAssetRegistry& registry, std::span<const std::byte> encoded,
    std::string* error = nullptr);

// Removes the file and its registration without asking what references it. A reference that is already gone is success.
bool Remove(const ProjectPaths& paths, ProjectAssetRegistry& registry, std::string_view reference, std::string* error = nullptr);
}
