#include "assets/ProjectSceneMigration.h"

#include "Project/ProjectPaths.h"
#include "Scene/SceneAssetIdentity.h"
#include "Scene/SceneDocumentAdapter.h"
#include "Scene/SceneSerializer.h"
#include "assets/AssetReconciliationTransaction.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/ProjectAssetRegistry.h"
#include "assets/ProjectSceneAssets.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>

namespace
{
    namespace fs = std::filesystem;
    using ProjectSceneAssets::SceneFile;
    using ProjectSceneAssets::SceneFileStatus;

    // AssetReconciliationTransaction's limits, checked before anything is written so an oversized project fails cleanly.
    static_assert(AssetReconciliationTransaction::MaximumFileBytes == MaximumSceneSourceBytes,
        "The largest admitted scene file must be savable in one transaction");
    constexpr std::size_t MaximumTargets = AssetReconciliationTransaction::MaximumTargets;
    constexpr std::size_t MaximumFileBytes = AssetReconciliationTransaction::MaximumFileBytes;
    constexpr std::size_t MaximumTransactionBytes = AssetReconciliationTransaction::MaximumTransactionBytes;
    const fs::path RegistryFile = "Assets.registry.json";

    std::string UTF8(const fs::path& path)
    {
        const auto text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }

    struct FileHandle
    {
        HANDLE value = INVALID_HANDLE_VALUE;
        explicit FileHandle(HANDLE handle) : value(handle) {}
        ~FileHandle()
        {
            if(value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value);
            }
        }
        FileHandle(const FileHandle&) = delete;
        FileHandle& operator=(const FileHandle&) = delete;
    };

    // Writes a new file (never replacing one), flushes it to disk, then reads it back and requires identical bytes.
    void WriteVerifiedFile(const fs::path& path, const std::string& bytes)
    {
        {
            FileHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));

            if(file.value == INVALID_HANDLE_VALUE)
            {
                throw std::runtime_error("Cannot create backup file " + UTF8(path) + " (Windows error " +
                    std::to_string(GetLastError()) + ").");
            }

            DWORD written = 0;

            if(WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) == FALSE ||
                written != bytes.size() || FlushFileBuffers(file.value) == FALSE)
            {
                throw std::runtime_error("Cannot write backup file " + UTF8(path) + ".");
            }
        }

        FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        std::string readBack(bytes.size(), '\0');
        DWORD read = 0;
        LARGE_INTEGER size{};

        if(file.value == INVALID_HANDLE_VALUE || GetFileSizeEx(file.value, &size) == FALSE ||
            static_cast<std::uint64_t>(size.QuadPart) != bytes.size() ||
            (!bytes.empty() && (ReadFile(file.value, readBack.data(), static_cast<DWORD>(readBack.size()), &read, nullptr) == FALSE ||
                read != readBack.size() || readBack != bytes)))
        {
            throw std::runtime_error("Backup verification failed for " + UTF8(path) + ".");
        }
    }

    void CreateNewDirectory(const fs::path& path, bool mayExist)
    {
        if(CreateDirectoryW(path.c_str(), nullptr) == FALSE)
        {
            const DWORD error = GetLastError();

            if(!(mayExist && error == ERROR_ALREADY_EXISTS))
            {
                throw std::runtime_error("Cannot create backup directory " + UTF8(path) + " (Windows error " +
                    std::to_string(error) + ").");
            }
        }
    }

    std::string BackupRunName()
    {
        SYSTEMTIME time{};
        GetSystemTime(&time);
        char stamp[32]{};
        std::snprintf(stamp, sizeof(stamp), "%04u%02u%02uT%02u%02u%02uZ", time.wYear, time.wMonth, time.wDay, time.wHour,
            time.wMinute, time.wSecond);
        const auto unique = AssetID::Generate();

        if(!unique)
        {
            throw std::runtime_error("Cannot generate a backup directory name.");
        }

        return std::string(stamp) + "-" + unique->ToString().substr(0, 8);
    }

    // The scene JSON without the fields migration changes, to tell a verbatim relabel from a historical representation upgrade.
    nlohmann::json Comparable(const std::string& bytes)
    {
        auto json = nlohmann::json::parse(bytes);
        json.erase("schemaVersion");
        json.erase("kind");
        json.erase("id");
        return json;
    }

    std::vector<std::string> EntityIDs(const nlohmann::json& json)
    {
        std::vector<std::string> ids;

        for(const auto& object : json.at("objects"))
        {
            ids.push_back(object.at("id").get<std::string>());
        }

        return ids;
    }

    struct Prepared
    {
        const SceneFile* file = nullptr;
        std::string bytes;
        ProjectSceneMigration::MigratedScene record;
    };

    // Converts one legacy scene in memory and proves the result before anything is staged. Returns a problem, or empty.
    std::string Prepare(const SceneFile& file, Prepared& prepared)
    {
        SceneDocument legacy;
        std::string error;

        if(!ParseSceneDocument(file.bytes, legacy, &error))
        {
            return "cannot be read: " + error;
        }

        SceneDocument migrated = legacy;
        migrated.kind = SceneKind::Local;
        migrated.id = SceneAssetIdentity(legacy.id).ToString();
        migrated.schemaVersion = SceneDocument::kSchemaVersion;
        std::string bytes;

        if(!SerializeSceneDocument(migrated, bytes, &error))
        {
            return "cannot be written in schema " + std::to_string(SceneDocument::kSchemaVersion) + ": " + error;
        }

        SceneDocument readBack;

        if(!ParseSceneDocument(bytes, readBack, &error))
        {
            return "the migrated document does not read back: " + error;
        }

        if(!SceneDocumentsEqual(migrated, readBack) || readBack.kind != SceneKind::Local || readBack.id != migrated.id)
        {
            return "the migrated document does not read back unchanged";
        }

        // Content: apart from schema version, kind and id, the migrated document is the document the upgrade chain produced.
        SceneDocument restored = readBack;
        restored.schemaVersion = legacy.schemaVersion;
        restored.kind = legacy.kind;
        restored.id = legacy.id;

        if(!SceneDocumentsEqual(restored, legacy))
        {
            return "the migrated document differs from the loaded scene beyond its schema version, kind and id";
        }

        // Entity identities, checked against the original file independently of the reader.
        const auto original = nlohmann::json::parse(file.bytes);
        const auto written = nlohmann::json::parse(bytes);

        if(EntityIDs(original) != EntityIDs(written))
        {
            return "entity identities or order changed";
        }

        prepared.file = &file;
        prepared.bytes = std::move(bytes);
        prepared.record.relativePath = file.relativePath;
        prepared.record.fromSchema = file.schemaVersion;
        prepared.record.oldID = legacy.id;
        prepared.record.newID = migrated.id;
        prepared.record.representationUpgraded = Comparable(file.bytes) != Comparable(prepared.bytes);
        return {};
    }

    bool ReadWholeFile(const fs::path& path, std::string& bytes)
    {
        std::ifstream file(path, std::ios::binary);

        if(!file)
        {
            return false;
        }

        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return !file.bad();
    }

    // Records a backup set's outcome once (CREATE_NEW: an existing outcome is never replaced).
    void WriteOutcome(const fs::path& run, const std::string& outcome, const std::string& detail)
    {
        const nlohmann::json record{{"version", 1}, {"outcome", outcome}, {"detail", detail}};
        WriteVerifiedFile(run / ProjectSceneMigration::BackupOutcomeFile, record.dump(2) + "\n");
    }

    // Best effort after a failure: the set is resolved again at the next run if this cannot be written.
    void TryWriteOutcome(const fs::path& run, const std::string& outcome, const std::string& detail,
        ProjectSceneMigration::Report& report)
    {
        try
        {
            WriteOutcome(run, outcome, detail);
        }
        catch(const std::exception& exception)
        {
            report.backupResolutions.push_back(UTF8(run.filename()) + ": outcome '" + outcome + "' not recorded (" +
                exception.what() + "); it is resolved at the next project open.");
        }
    }

    // Gives every backup set left without an outcome by an interrupted attempt the outcome the project's state establishes.
    // Runs only with no pending transaction, so the project is in its recovered, consistent state.
    void ResolveInterruptedBackups(const ProjectPaths& paths, ProjectSceneMigration::Report& report)
    {
        const fs::path root = paths.GetRoot();
        const fs::path migrations = root / ProjectSceneAssets::MigrationDirectory;
        std::error_code error;

        if(!fs::is_directory(migrations, error))
        {
            return;
        }

        if(fs::exists(root / ProjectSceneAssets::ReconciliationDirectory, error))
        {
            report.backupResolutions.push_back("Backup outcomes not resolved: a project transaction is pending recovery.");
            return;
        }

        std::vector<fs::path> runs;

        for(const auto& entry : fs::directory_iterator(migrations, error))
        {
            if(entry.is_directory() && !entry.is_symlink() && !fs::exists(entry.path() / ProjectSceneMigration::BackupOutcomeFile))
            {
                runs.push_back(entry.path());
            }
        }

        std::sort(runs.begin(), runs.end());

        for(const auto& run : runs)
        {
            const std::string name = UTF8(run.filename());

            try
            {
                std::string manifestBytes;

                if(!ReadWholeFile(run / "migration.json", manifestBytes))
                {
                    WriteOutcome(run, "backup-incomplete", "Backup preparation stopped before its manifest; no migration was attempted.");
                    report.backupResolutions.push_back(name + ": backup-incomplete (no manifest).");
                    continue;
                }

                const auto manifest = nlohmann::json::parse(manifestBytes);
                bool original = true;
                bool migrated = true;

                for(const auto& scene : manifest.at("scenes"))
                {
                    const fs::path path = root / fs::u8path(scene.at("path").get<std::string>());
                    std::string current;
                    std::string backup;
                    const bool present = ReadWholeFile(path, current);
                    original = original && present && ReadWholeFile(run / fs::u8path(scene.at("backup").get<std::string>()), backup) &&
                        current == backup;

                    SceneDocument document;
                    std::uint64_t schema = 0;
                    migrated = migrated && present && ParseSceneDocument(current, document, nullptr, &schema) &&
                        schema >= SceneDocument::kSceneKindSchemaVersion && document.id == scene.at("newID").get<std::string>();
                }

                if(original)
                {
                    WriteOutcome(run, "not-applied", "Resolved after an interrupted attempt: the project holds the original scenes.");
                    report.backupResolutions.push_back(name + ": not-applied (interrupted attempt; originals in place).");
                }
                else if(migrated)
                {
                    WriteOutcome(run, "committed", "Resolved after an interrupted attempt: the project holds the migrated scenes.");
                    report.backupResolutions.push_back(name + ": committed (interrupted after commit).");
                }
                else
                {
                    report.backupResolutions.push_back(name + ": outcome undetermined (scenes changed or missing since the attempt); "
                        "left unmarked.");
                }
            }
            catch(const std::exception& exception)
            {
                report.backupResolutions.push_back(name + ": outcome not resolved: " + exception.what());
            }
        }
    }

    void WriteBackups(const ProjectPaths& paths, const std::vector<Prepared>& scenes, const std::string& registryBytes,
        ProjectSceneMigration::Report& report)
    {
        const fs::path migrationRoot = paths.GetRoot() / ProjectSceneAssets::MigrationDirectory;
        CreateNewDirectory(migrationRoot, true);
        const fs::path relativeRun = fs::path(ProjectSceneAssets::MigrationDirectory) / BackupRunName();
        const fs::path run = paths.GetRoot() / relativeRun;
        // A new, uniquely named directory: an earlier backup is never overwritten.
        CreateNewDirectory(run, false);
        report.backupDirectory = relativeRun;
        nlohmann::json manifest{{"version", 1}, {"schemaVersion", SceneDocument::kSchemaVersion}, {"scenes", nlohmann::json::array()}};

        for(std::size_t index = 0; index < scenes.size(); ++index)
        {
            const auto& scene = scenes[index];
            fs::path backup = scene.record.relativePath;
            backup += ".scene-backup";
            fs::create_directories((run / backup).parent_path());
            WriteVerifiedFile(run / backup, scene.file->bytes);
            report.scenes[index].backup = relativeRun / backup;
            manifest["scenes"].push_back({{"path", UTF8(scene.record.relativePath)}, {"backup", UTF8(backup)},
                {"fromSchema", scene.record.fromSchema}, {"oldID", scene.record.oldID}, {"newID", scene.record.newID},
                {"bytes", scene.file->bytes.size()}, {"representationUpgraded", scene.record.representationUpgraded}});
        }

        WriteVerifiedFile(run / "Assets.registry.json.backup", registryBytes);
        manifest["registryBackup"] = "Assets.registry.json.backup";
        WriteVerifiedFile(run / "migration.json", manifest.dump(2) + "\n");
    }
}

namespace ProjectSceneMigration
{
bool Run(const ProjectPaths& paths, ProjectAssetRegistry& registry, Report& report, std::string& error)
{
    error.clear();
    report = {};

    try
    {
        ResolveInterruptedBackups(paths, report);
        ProjectSceneAssets::Scan scan;

        if(!ProjectSceneAssets::ScanProject(paths, scan, error))
        {
            return false;
        }

        std::vector<const SceneFile*> legacy;

        for(const auto& file : scan.files)
        {
            if(file.status == SceneFileStatus::Legacy)
            {
                legacy.push_back(&file);
            }
        }

        if(legacy.empty())
        {
            return true;
        }

        report.required = true;

        // Preflight: every problem in the project blocks the whole migration; collect them all.
        std::map<std::string, std::vector<std::string>> identities;

        for(const auto& file : scan.files)
        {
            const std::string path = UTF8(file.relativePath);

            if(file.status == SceneFileStatus::Invalid)
            {
                report.blocking.push_back(path + ": invalid scene: " + file.problem);
            }
            else if(file.status == SceneFileStatus::UnsupportedFuture)
            {
                report.blocking.push_back(path + ": unsupported scene schema: " + file.problem);
            }
            else
            {
                identities[file.assetID->ToString()].push_back(path);
            }
        }

        for(const auto& [identity, files] : identities)
        {
            if(files.size() > 1)
            {
                std::string message = "duplicate scene identity " + identity + " (after migration) in:";

                for(const auto& file : files)
                {
                    message += " " + file;
                }

                report.blocking.push_back(message);
            }

            if(const auto* record = registry.Find(*AssetID::Parse(identity)); record && record->type != AssetType::Scene)
            {
                report.blocking.push_back(files.front() + ": its identity " + identity + " already identifies " +
                    UTF8(record->relativePath));
            }
        }

        std::vector<Prepared> prepared(legacy.size());

        for(std::size_t index = 0; index < legacy.size(); ++index)
        {
            const std::string problem = Prepare(*legacy[index], prepared[index]);

            if(!problem.empty())
            {
                report.blocking.push_back(UTF8(legacy[index]->relativePath) + ": " + problem);
            }
        }

        // The registry on disk must be the registry the project loaded; it is replaced in the same transaction.
        std::string registryBytes;

        if(!AssetReconciliationTransaction::ReadExisting(paths, RegistryFile, registryBytes, error))
        {
            report.blocking.push_back("Assets.registry.json: " + error);
            error.clear();
        }
        else
        {
            ProjectAssetRegistry onDisk;
            std::string registryError;

            if(!AssetRegistrySerializer::Deserialize(nlohmann::json::parse(registryBytes), onDisk, &registryError) ||
                AssetRegistrySerializer::Serialize(onDisk) != AssetRegistrySerializer::Serialize(registry))
            {
                report.blocking.push_back("Assets.registry.json changed after the project loaded it.");
            }
        }

        // The registry the migrated project implies: every scene (migrated ones under their new identity) as a current scene.
        ProjectAssetRegistry candidate = registry;

        if(report.blocking.empty())
        {
            ProjectSceneAssets::Scan migratedScan = scan;
            std::size_t next = 0;

            for(auto& file : migratedScan.files)
            {
                if(file.status == SceneFileStatus::Legacy)
                {
                    file.status = SceneFileStatus::Current;
                    file.bytes = prepared[next].bytes;
                    file.documentID = prepared[next].record.newID;
                    file.assetID = AssetID::Parse(prepared[next].record.newID);
                    ++next;
                }
            }

            ProjectSceneAssets::Report sceneReport;

            if(!ProjectSceneAssets::Apply(migratedScan, candidate, sceneReport, error))
            {
                return false;
            }

            for(const auto& problem : sceneReport.conflicts)
            {
                report.blocking.push_back("registry conflict: " + problem);
            }

            for(const auto& problem : sceneReport.duplicates)
            {
                report.blocking.push_back("registry conflict: " + problem);
            }
        }

        // Transaction limits, before any backup is written.
        const std::string candidateBytes = AssetRegistrySerializer::ToBytes(candidate);
        std::size_t total = registryBytes.size() + candidateBytes.size();

        for(std::size_t index = 0; index < legacy.size(); ++index)
        {
            total += legacy[index]->bytes.size() + prepared[index].bytes.size();

            if(prepared[index].bytes.size() > MaximumFileBytes)
            {
                report.blocking.push_back(UTF8(legacy[index]->relativePath) + ": the migrated scene exceeds the 64 MiB transaction file limit.");
            }
        }

        report.transactionBytes = total;

        if(legacy.size() + 1 > MaximumTargets)
        {
            report.blocking.push_back(std::to_string(legacy.size()) + " legacy scenes exceed the migration transaction limit of " +
                std::to_string(MaximumTargets - 1) + "; migrate the project in an Engine build with a larger limit.");
        }

        if(total > MaximumTransactionBytes)
        {
            report.blocking.push_back("The migration (" + std::to_string(total / (1024 * 1024)) +
                " MiB of original and migrated data) exceeds the 256 MiB transaction limit.");
        }

        if(!report.blocking.empty())
        {
            return false;
        }

        for(auto& scene : prepared)
        {
            report.scenes.push_back(scene.record);
        }

        // Exact original bytes, verified on disk, before any original is replaced. A backup failure stops the migration.
        WriteBackups(paths, prepared, registryBytes, report);

        std::vector<AssetReconciliationTransaction::Replacement> replacements;

        for(std::size_t index = 0; index < prepared.size(); ++index)
        {
            replacements.push_back({legacy[index]->relativePath, prepared[index].bytes, legacy[index]->bytes});
        }

        replacements.push_back({RegistryFile, candidateBytes, registryBytes});

        const fs::path run = paths.GetRoot() / report.backupDirectory;

        if(!AssetReconciliationTransaction::Commit(paths, replacements, error))
        {
            error = "Scene migration was not committed: " + error;

            // Commit rolled back: the originals are in place. Pending recovery storage means the outcome is not yet known; the
            // next run resolves it after recovery.
            if(!fs::exists(paths.GetRoot() / ProjectSceneAssets::ReconciliationDirectory))
            {
                TryWriteOutcome(run, "not-applied", "The migration transaction did not commit: " + error, report);
            }

            return false;
        }

        registry = std::move(candidate);
        report.completed = true;
        // The migration is committed whether or not this marker is written; a missing marker is resolved at the next run.
        TryWriteOutcome(run, "committed", "Migrated " + std::to_string(report.scenes.size()) + " scenes.", report);
        return true;
    }
    catch(const std::exception& exception)
    {
        error = std::string("Scene migration failed: ") + exception.what();

        // A failure after the backup directory exists (backup preparation itself, or staging) never replaced an original.
        if(!report.backupDirectory.empty() && !fs::exists(paths.GetRoot() / ProjectSceneAssets::ReconciliationDirectory))
        {
            const fs::path run = paths.GetRoot() / report.backupDirectory;
            const bool manifest = fs::exists(run / "migration.json");
            TryWriteOutcome(run, manifest ? "not-applied" : "backup-incomplete", error, report);
        }

        return false;
    }
}
}
