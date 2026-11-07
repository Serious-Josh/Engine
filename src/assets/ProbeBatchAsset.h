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

// M7: the generated probes of one Probe Volume, in Content/Audio/Probes/<uuid>.probebatch. A scene stores only the reference, so
// no binary data reaches its JSON. The same bytes are a Resource entry in a packaged game's archive.
//
// Layout, little-endian: "ENGPROBE", u32 version, u32 rejected, u64 fingerprint, u32 probe count, u32 ambiguous, u64 batch size,
// count * {f32 x, y, z, radius}, the batch (iplProbeBatchSave's bytes, opaque here), u64 FNV-1a of everything before it. Decoding
// checks all of it before any byte reaches Steam Audio, because iplProbeBatchLoad is not safe on malformed data.
namespace ProbeBatchAssets
{
inline constexpr std::uint32_t FormatVersion = 1;
inline constexpr std::uint32_t PackagedPayloadVersion = FormatVersion;
// Guards hostile sizes only: this is what one archive entry can hold, and no limit on what a project may generate.
inline constexpr std::size_t MaximumBytes = std::size_t{1} << 31;
inline constexpr std::string_view Extension = ".probebatch";

struct Data
{
    // What the probes were generated from (ProbeVolumes::Fingerprint). Regenerate when it no longer matches the scene.
    std::uint64_t fingerprint = 0;
    // Engine-space x, y, z, radius: for drawing and diagnostics, without loading Steam Audio.
    std::vector<std::array<float, 4>> probes;
    std::vector<std::byte> batch;
    // What the inside-solid filter dropped and what it could not classify and kept. Files from before the filter hold zeros here.
    std::uint32_t rejectedProbes = 0;
    std::uint32_t ambiguousProbes = 0;

    bool operator==(const Data&) const = default;
};

std::vector<std::byte> Encode(const Data& data);
bool Decode(std::span<const std::byte> bytes, Data& data, std::string* error = nullptr);

bool LoadFromFile(const std::filesystem::path& path, Data& data, std::string* error = nullptr);
std::optional<Data> Resolve(std::string_view reference, const ProjectPaths& paths, const ProjectAssetRegistry& registry, std::string* error = nullptr);

// Writes `encoded` whole or not at all, as a new asset, and returns its reference. Stored probes are immutable: undo, other scenes and
// saved files may all still reference the previous asset, which stays until ProbeAssetCleanup confirms that nothing does.
std::optional<std::string> Store(const ProjectPaths& paths, ProjectAssetRegistry& registry, std::span<const std::byte> encoded, std::string* error = nullptr);

// Removes the file and its registration, without asking what references it: only ProbeAssetCleanup decides that. A reference that is
// already gone is success.
bool Remove(const ProjectPaths& paths, ProjectAssetRegistry& registry, std::string_view reference, std::string* error = nullptr);
}
