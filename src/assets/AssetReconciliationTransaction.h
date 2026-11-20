#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class ProjectPaths;

// CPU-only, existing-file replacements. Call before loading dependent project data.
// Bytes are opaque: callers retain responsibility for document validation.
namespace AssetReconciliationTransaction
{
    // M3.5: per-file and aggregate byte limits. A file may be as large as the largest admitted scene source
    // (MaximumSceneSourceBytes, 1 GiB), so the largest scene can be saved; a transaction may hold two such files (a replacement
    // and the original it backs up) plus 256 MiB for everything else. They bound memory for actual files; nothing is
    // allocated to them. Callers that pre-check (ProjectSceneMigration) use these same values.
    inline constexpr std::size_t MaximumFileBytes = std::size_t{1} << 30;
    inline constexpr std::size_t MaximumTargets = 4096;
    inline constexpr std::size_t MaximumTransactionBytes = 2 * MaximumFileBytes + 256 * 1024 * 1024;

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
