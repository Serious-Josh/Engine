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
        // W2: a new file (the target must not exist; no expected original). Rollback removes it again, and only if it holds
        // exactly the staged bytes.
        bool create = false;
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

#if defined(_DEBUG)
    // W1.3 diagnostics (Debug builds only): simulates process death at one stage of the next Commit or Recover. The interrupted
    // call returns false and leaves storage exactly as a crash would (no rollback, no cleanup), so the following Recover is what
    // must restore a consistent project. `index` selects the target for the per-target stages. One-shot.
    enum class FaultStage
    {
        None,
        BeforeJournal,
        AfterJournal,
        DuringRetirement,
        DuringInstallation,
        BeforeCommitMarker,
        AfterCommitMarker,
        DuringRecovery
    };

    void InjectFaultForTesting(FaultStage stage, std::size_t index = 0);
#endif
}
