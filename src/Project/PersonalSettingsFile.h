#pragma once

#include <filesystem>
#include <string>

// Shared load/save policy for personal Editor state (global preferences and per-project
// workspace). Personal state is always optional: no outcome here may stop a project opening.
namespace PersonalSettingsFile
{
    enum class Status
    {
        Missing,            // First run: defaults, file created on the first save.
        Loaded,
        Malformed,          // Defaults; the bad file is kept as <name>.invalid before replacement.
        IdentityMismatch,   // Workspace copied from another project: ignored, then replaced.
        UnsupportedVersion, // Written by a newer Engine: defaults, and never overwritten.
        Unreadable          // Access denied or I/O error: defaults, and never overwritten.
    };

    const char* Describe(Status status);

    // Whether this session may replace the file. A newer-version or unreadable file is left alone
    // so personal state from another Engine version, or a temporarily locked file, is not lost.
    bool MayReplace(Status status);

    // Reads at most 1 MiB. Returns Missing when the file does not exist.
    Status Read(const std::filesystem::path& path, std::string& contents, std::string& errorMessage);

    // Moves a malformed or foreign file aside to <name>.invalid (replacing an older one) so the
    // next save does not destroy it silently. Missing source is not an error.
    bool PreserveInvalid(const std::filesystem::path& path, std::string& errorMessage);

    // Replaces the file atomically, first preserving it aside when status requires.
    bool Save(
        const std::filesystem::path& path,
        Status loadedStatus,
        const std::string& contents,
        std::string& errorMessage);
}
