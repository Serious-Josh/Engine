#pragma once

#include "Scene/SceneDocument.h"
#include "assets/AssetID.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class ProjectPaths;
class ProjectAssetRegistry;

// W1.2: discovery of .scene documents as AssetType::Scene records. A scene's asset identity is its document id, so discovery
// never generates identities: it reads them. It never modifies a scene file.
namespace ProjectSceneAssets
{
    // Project-root directories that hold transaction and migration storage; never scanned.
    inline constexpr const char* ReconciliationDirectory = ".asset-reconciliation";
    inline constexpr const char* MigrationDirectory = ".scene-migration";

    enum class SceneFileStatus
    {
        // Schema 28 or later and valid: kind and canonical GUID id.
        Current,
        // A supported historical schema (1-27) that loads; it needs migration before it is registered.
        Legacy,
        // Not a loadable scene document (unreadable, malformed, invalid content).
        Invalid,
        // A schema newer than this executable supports.
        UnsupportedFuture
    };

    struct SceneFile
    {
        std::filesystem::path relativePath;
        SceneFileStatus status = SceneFileStatus::Invalid;
        std::string bytes;
        std::uint32_t schemaVersion = 0;
        // The id as written: a canonical GUID for Current scenes, possibly a legacy id for Legacy scenes.
        std::string documentID;
        // Current: the document id. Legacy: the identity migration will give it (SceneAssetIdentity).
        std::optional<AssetID> assetID;
        SceneKind kind = SceneKind::Local;
        std::string problem;
    };

    struct Scan
    {
        // Sorted by relative path (generic UTF-8), so results are deterministic.
        std::vector<SceneFile> files;
        // Linked (reparse-point) directories and files that were not followed.
        std::vector<std::filesystem::path> skippedLinks;
    };

    // Read-only scan of every .scene file below the project root.
    bool ScanProject(const ProjectPaths& paths, Scan& scan, std::string& error);

    struct Report
    {
        std::size_t registered = 0;
        std::size_t moved = 0;
        std::size_t removed = 0;
        std::size_t unchanged = 0;
        // Human-readable findings, one per affected file or identity.
        std::vector<std::string> invalid;
        std::vector<std::string> unsupported;
        std::vector<std::string> duplicates;
        std::vector<std::string> legacy;
        std::vector<std::string> conflicts;
        std::vector<std::string> retained;

        bool HasProblems() const
        {
            return !invalid.empty() || !unsupported.empty() || !duplicates.empty() || !conflicts.empty();
        }
    };

    // Applies a scan to `registry` (pure; no filesystem access). Registers each Current scene with a unique id, updates the path
    // of a registered scene found elsewhere (an external move), and removes a scene record only when its file is gone and its id
    // is found nowhere else. Records whose file still exists but is invalid, legacy or a duplicate are retained. Two or more files
    // claiming one id are reported and none of them changes the registry. Fails only on an internal registry error.
    bool Apply(const Scan& scan, ProjectAssetRegistry& registry, Report& report, std::string& error);

    // ScanProject + Apply, then publishes the registry atomically when it changed. Problems are reported, not errors; `registry`
    // is replaced only after a successful publication.
    bool Reconcile(const ProjectPaths& paths, ProjectAssetRegistry& registry, Report& report, std::string& error);
}
