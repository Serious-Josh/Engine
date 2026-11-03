#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Scripting
{
// First line of every generated as.predefined. An existing file without it was not written by the exporter and is
// never replaced.
constexpr std::string_view GeneratedDeclarationsMarker = "// as.predefined: Engine's AngelScript API";

enum class DeclarationFileStatus
{
    Written,
    Unchanged,
    Refused,
    Failed
};

// Writes generated declarations to `path`, which must be named exactly as.predefined in an existing directory.
// Identical contents are left untouched (no timestamp change). The new text goes to a sibling temporary file that
// then replaces the target, so a failed write never leaves a truncated file behind. `error` explains Refused/Failed.
DeclarationFileStatus WriteDeclarationFile(const std::filesystem::path& path, std::string_view text,
    std::string& error);
}
