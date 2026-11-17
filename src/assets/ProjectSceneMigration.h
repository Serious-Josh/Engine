#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class ProjectPaths;
class ProjectAssetRegistry;

// W1.3: migration of historical .scene documents (schemas 1-27) into schema-28 unassigned Local Levels.
//
// One project-wide AssetReconciliationTransaction replaces every legacy scene and the asset registry together, so after any
// interruption the transaction's recovery leaves either the complete original project or the complete migrated one. Each scene is
// read through the existing upgrade chain, given kind Local and the id SceneAssetIdentity(id) (an existing canonical GUID is kept),
// written in schema 28, read back and compared before anything is staged. Exact original bytes are first written to a retained
// backup under .scene-migration/. Nothing is written when any scene in the project is invalid, unsupported, or would share an
// identity: every such problem is reported and the project is left unchanged. No World is created.
namespace ProjectSceneMigration
{
    struct MigratedScene
    {
        std::filesystem::path relativePath;
        std::uint32_t fromSchema = 0;
        std::string oldID;
        std::string newID;
        // The upgrade chain changed the stored representation beyond schemaVersion, kind and id (historical upgrades such as
        // lighting units or collider scale), as opposed to a verbatim schema relabel.
        bool representationUpgraded = false;
        std::filesystem::path backup;
    };

    struct Report
    {
        // Legacy scenes were found.
        bool required = false;
        // The migration transaction committed.
        bool completed = false;
        std::vector<MigratedScene> scenes;
        // Every problem that blocks migration; nothing was written when non-empty.
        std::vector<std::string> blocking;
        // Retained backup directory (project-relative) of a completed or attempted migration.
        std::filesystem::path backupDirectory;
        std::size_t transactionBytes = 0;
        // Outcomes recorded for backup sets of earlier, interrupted attempts (see BackupOutcome), and sets whose outcome could
        // not be established from the project.
        std::vector<std::string> backupResolutions;
    };

    // Every backup set (.scene-migration/<run>/) ends with outcome.json, written once with CREATE_NEW and never replaced:
    //   "committed"          the migration transaction committed; the project holds the migrated scenes.
    //   "not-applied"        the transaction did not commit (blocked at commit or rolled back); the project holds the originals.
    //   "backup-incomplete"  backup preparation stopped before its manifest; never a migration.
    // A set without outcome.json belongs to an attempt interrupted by a crash. Run records its outcome from the project's actual
    // state (after transaction recovery): originals byte-identical to the backups -> not-applied; every listed scene at its
    // migrated id -> committed; anything else stays unmarked and is reported, never guessed.
    inline constexpr const char* BackupOutcomeFile = "outcome.json";

    // Migrates every legacy scene of the project. Returns true when there was nothing to migrate or the migration committed
    // (`registry` then holds the published registry); false when it was blocked (report.blocking) or failed (`error`), with the
    // project unchanged or restored by the transaction's recovery. Call after AssetReconciliationTransaction::Recover and with
    // the registry the project loaded.
    bool Run(const ProjectPaths& paths, ProjectAssetRegistry& registry, Report& report, std::string& error);
}
