#pragma once

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

// M8.2: the baked reflection layers of one Probe Volume, in Content/Audio/Probes/<uuid>.bakedreflections. Immutable once
// stored: a rebake writes a new asset, and the previous one stays until ProbeAssetCleanup confirms nothing references it.
// The probe batch inside holds the probes it was baked from, so the source .probebatch is never modified or needed at run time.
//
// Layout, little-endian:
//   "ENGBAKED", u32 format version, u32 Steam Audio version (major << 16 | minor << 8 | patch)
//   settings: u32 rays, u32 diffuse samples, u32 bounces, f32 irradiance minimum, u32 order, f32 duration
//   source probes: u64 probe fingerprint, u64 probe positions hash, u32 reference length, reference bytes
//   u32 probe count, u32 layer count, u64 batch size
//   probe count * {f32 x, y, z, radius}
//   layer count * {u32 kind, f32 centre x, y, z, f32 radius, u32 source id length, source id bytes,
//                  u64 probes hash, u64 geometry hash, u64 materials hash, u64 settings hash, u64 fingerprint,
//                  u64 layer data size}
//   batch (iplProbeBatchSave's bytes, opaque here), u64 FNV-1a of everything before it
// Decoding checks every size, count and relationship, and the checksum, before any byte reaches Steam Audio:
// iplProbeBatchLoad is not safe on malformed data.
namespace BakedReflectionsAssets
{
inline constexpr std::uint32_t FormatVersion = 1;
inline constexpr std::uint32_t PackagedPayloadVersion = FormatVersion;
// The SDK this format's batches come from; SteamAudioProbes.cpp asserts it against phonon_version.h.
inline constexpr std::uint32_t SteamAudioVersion = (4u << 16) | (8u << 8) | 1u;
// The simulator's reflection order and duration, which every bake uses so its layers match what plays them. SteamAudioProbes.cpp
// asserts them against SteamAudioReflectionMaxOrder and SteamAudioReflectionMaxDuration.
inline constexpr std::uint32_t SimulatorOrder = 1;
inline constexpr float SimulatorDuration = 1.0f;
inline constexpr std::size_t MaximumBytes = std::size_t{1} << 31;
inline constexpr std::size_t MaximumLayers = 4096;
inline constexpr std::size_t MaximumTextBytes = 512;
inline constexpr std::string_view Extension = ".bakedreflections";

// Matches IPLBakedDataVariation for the two layers Engine bakes.
enum class LayerKind : std::uint32_t
{
    // Listener-centric: source and listener at each probe. Its identifier sphere is always all zeros (M8.1: the SDK
    // compares the sphere even for this variation).
    Reverb = 0,
    // Source-centric: one static Audio Source at the identifier's centre, listener at each probe.
    StaticSource = 1
};

// The settings a bake ran with. Order and duration follow the simulator; the rest are the Probe Volume's.
struct BakeParameters
{
    std::uint32_t rays = 0;
    std::uint32_t diffuseSamples = 0;
    std::uint32_t bounces = 0;
    float irradianceMinimumDistance = 0.0f;
    std::uint32_t order = 0;
    float duration = 0.0f;

    bool operator==(const BakeParameters&) const = default;
};

struct Layer
{
    LayerKind kind = LayerKind::Reverb;
    // The identifier exactly as baked, Steam Audio space. Kept bit for bit: the SDK matches identifiers by exact float
    // equality (M8.1), so a recomputed centre would find no data.
    std::array<float, 3> centre{};
    float radius = 0.0f;
    // StaticSource only: the Audio Source's object id. Empty for Reverb.
    std::string sourceObjectId;
    // What the layer was baked from, kept apart so a stale layer can say why (ReflectionBakes).
    std::uint64_t probesHash = 0;
    std::uint64_t geometryHash = 0;
    std::uint64_t materialsHash = 0;
    std::uint64_t settingsHash = 0;
    // Combines the above with the identifier and the format and SDK versions.
    std::uint64_t fingerprint = 0;
    // iplProbeBatchGetDataSize for this layer. Never zero: an empty or partial layer is never published.
    std::uint64_t dataBytes = 0;

    bool operator==(const Layer&) const = default;
};

struct Data
{
    std::uint32_t steamAudioVersion = SteamAudioVersion;
    BakeParameters parameters;
    // The probe asset this was baked from: its reference, its M7 fingerprint, and ReflectionBakes::HashProbes of it, which every
    // layer's probesHash must equal.
    std::string sourceProbeBatch;
    std::uint64_t sourceProbeFingerprint = 0;
    std::uint64_t sourceProbesHash = 0;
    // Engine-space x, y, z, radius, identical to the source probe asset's.
    std::vector<std::array<float, 4>> probes;
    std::vector<Layer> layers;
    std::vector<std::byte> batch;

    bool operator==(const Data&) const = default;
};

enum class DecodeError
{
    None,
    // Not this format, or the wrong size.
    NotBakedReflections,
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

// Removes the file and its registration without asking what references it: only ProbeAssetCleanup decides that.
bool Remove(const ProjectPaths& paths, ProjectAssetRegistry& registry, std::string_view reference, std::string* error = nullptr);

// FNV-1a over the probe values, as both formats store them. A layer baked from these probes records it.
std::uint64_t HashProbes(std::span<const std::array<float, 4>> probes) noexcept;
}
