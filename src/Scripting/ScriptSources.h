#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Scripting
{
struct SourceLimits
{
    std::size_t sectionBytes = 1024 * 1024;
    std::size_t totalBytes = 8 * 1024 * 1024;
    std::size_t sections = 64;
    std::size_t depth = 16;
};

struct SourceSection
{
    std::string name;
    std::string assetReference;
    std::string bytes;
};

struct SourceSnapshot
{
    std::string rootSection;
    std::string rootAsset;
    // Sorted canonical section names; immutable once published through Result.
    std::vector<SourceSection> sections;
    std::string revision;
    bool complete = false;
};

// A reader supplies bytes only for an exact authorized name or throws. No default IO.
using SourceReader = std::function<SourceSection(std::string_view)>;

std::string CanonicalScriptSection(std::string_view name);
std::string ResolveScriptInclude(std::string_view from, std::string_view include);
std::string SourceClosureRevision(const SourceSnapshot& snapshot);
bool ScriptNamesEqual(std::string_view left, std::string_view right);

// Archive entry (EngineAssetPak Resource, keyed by the root Script asset ID) holding one encoded snapshot.
inline constexpr std::uint32_t PackagedSnapshotVersion = 1;
inline constexpr std::size_t PackagedSnapshotBytes = 10 * 1024 * 1024;

// AS.5 packaged form of a complete source closure (Build Game). Decoding enforces the closure
// limits; the include policy and revision are rechecked when the snapshot is built.
std::vector<std::byte> EncodeSnapshot(const SourceSnapshot& snapshot);
SourceSnapshot DecodeSnapshot(std::span<const std::byte> bytes, SourceLimits limits = {});
}
