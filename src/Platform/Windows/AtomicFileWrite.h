#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace AtomicFileWrite
{
    // Writes contents to a temporary file in the destination's directory, flushes it, then
    // replaces the destination with MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH), the pattern
    // the asset registry and scene serializers already use. On any failure the temporary file
    // is removed and an existing destination is left exactly as it was.
    bool Write(
        const std::filesystem::path& destination,
        std::string_view contents,
        std::string& errorMessage);
}
