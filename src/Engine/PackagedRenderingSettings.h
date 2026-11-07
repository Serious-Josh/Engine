#pragma once

#include "Project/ProjectSettings.h"
#include "assets/AssetID.h"
#include "assets/EngineAssetPak.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// PS.4: the project's saved rendering settings in a packaged game. Build Game cooks them into one
// fixed-ID Resource entry of the game archive; packaged preflight admits it before the renderer starts.
//
// The payload is a ProjectSettings.json document (PS.1 format, stable serialized enum names, both mode
// sections), so the packaged runtime parses and validates it exactly as the Editor loads the project file.
// Archives built before PS.4 have no entry and render the default Legacy configuration, as they always did.
namespace PackagedRenderingSettings
{
    inline AssetID ID() { return *AssetID::Parse("00000000-0000-0000-0000-000000000037"); }
    // The archive entry's payload version. The document inside carries its own PS.1 format version.
    inline constexpr std::uint32_t PayloadVersion = 1;
    inline constexpr std::size_t MaximumPayloadBytes = 1024 * 1024;

    // Build Game's source policy. Only the last successfully saved settings are cooked, never a draft or
    // session override. Missing (never saved) cooks the defaults the Editor renders; a file whose values were
    // repaired on load, a malformed, newer-version or unreadable file is refused with an actionable error,
    // so a build never packages a configuration the user did not save.
    bool SelectBuildSource(const LoadedProjectSettings& saved, ProjectSettings& source, std::string& error);

    std::vector<std::byte> Encode(const ProjectSettings& settings);

    enum class Source
    {
        // No entry: an archive built before PS.4.
        Missing,
        Cooked,
        // A valid entry whose values were repaired by validation.
        Recovered,
        // An unreadable or invalid entry: defaults.
        Malformed
    };

    std::string_view Describe(Source source);

    struct Loaded
    {
        RenderingSettings rendering;
        // Steam Audio's saved settings travel in the same document. Missing or malformed: off.
        AudioSettings audio;
        Source source = Source::Missing;
        std::vector<std::string> warnings;
    };

    // An entry written by a newer Engine: a compatibility error, never a silent fallback.
    class UnsupportedVersion : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    // Throws UnsupportedVersion; a malformed document yields defaults with warnings.
    Loaded Decode(std::span<const std::byte> payload);

    // Packaged preflight: the entry (null when absent) and a reader of its bytes. Never writes anything.
    Loaded Admit(const EngineAssetPak::Entry* entry, const std::function<std::vector<std::byte>()>& read);
}
