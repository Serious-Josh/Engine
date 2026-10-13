#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class ProjectPaths;

// CPU-only, existing-file replacements. Call before loading dependent project data.
// Bytes are opaque: callers retain responsibility for document validation.
namespace AssetReconciliationTransaction
{
    struct Replacement
    {
        std::filesystem::path relativePath;
        std::string bytes;
        std::optional<std::string> expectedOriginal;
    };

    bool Commit(
        const ProjectPaths& paths,
        const std::vector<Replacement>& replacements,
        std::string& error);

    // No commit marker: rollback. Durable commit marker: retain replacements.
    // Failure preserves recovery files and must block project loading.
    bool Recover(const ProjectPaths& paths, std::string& error);

    bool ReadExisting(
        const ProjectPaths& paths,
        const std::filesystem::path& relativePath,
        std::string& bytes,
        std::string& error);
}
