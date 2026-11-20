#include "assets/ProjectSceneAssets.h"

#include "Project/ProjectPaths.h"
#include "Scene/SceneAssetIdentity.h"
#include "Scene/SceneDocumentCache.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/ProjectAssetRegistry.h"

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace
{
    namespace fs = std::filesystem;

    // The scene source bound (M3.5; also the reconciliation transaction's per-file limit); a larger file is reported as invalid
    // rather than read.
    constexpr std::uintmax_t MaximumSceneBytes = MaximumSceneSourceBytes;

    std::string UTF8(const fs::path& path)
    {
        const auto text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }

    bool IsExcludedDirectory(const fs::path& relative)
    {
        // Only the project-root storage directories are excluded; a folder with the same name elsewhere is ordinary content.
        if(std::distance(relative.begin(), relative.end()) != 1)
        {
            return false;
        }

        return ProjectAssetRegistry::PathsEqual(relative, ProjectSceneAssets::ReconciliationDirectory) ||
            ProjectAssetRegistry::PathsEqual(relative, ProjectSceneAssets::MigrationDirectory);
    }

    void Classify(ProjectSceneAssets::SceneFile& file, const fs::path& absolutePath)
    {
        using ProjectSceneAssets::SceneFileStatus;
        // One parse per content (W6.0): the reader reports the stored schema version even when the document is refused.
        SceneDocumentCache::Parsed parsedDocument;
        std::string error;
        const bool parsed = SceneDocumentCache::Parse(absolutePath, file.bytes, parsedDocument, &error, "ProjectSceneAssets");
        const std::uint64_t schema = parsedDocument.storedSchemaVersion;

        if(schema > SceneDocument::kSchemaVersion)
        {
            file.status = SceneFileStatus::UnsupportedFuture;
            file.schemaVersion = schema > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(schema);
            file.problem = "scene schema " + std::to_string(schema) + " is newer than this Engine supports (" +
                std::to_string(SceneDocument::kSchemaVersion) + ")";
            return;
        }

        if(!parsed)
        {
            file.status = SceneFileStatus::Invalid;
            file.problem = error;
            return;
        }

        file.schemaVersion = static_cast<std::uint32_t>(schema);

        file.documentID = parsedDocument.document->id;
        file.kind = parsedDocument.document->kind;

        if(file.schemaVersion >= SceneDocument::kSceneKindSchemaVersion)
        {
            // The schema-28 reader already requires a canonical GUID.
            file.status = SceneFileStatus::Current;
            file.assetID = AssetID::Parse(parsedDocument.document->id);
            return;
        }

        file.status = SceneFileStatus::Legacy;
        file.assetID = SceneAssetIdentity(parsedDocument.document->id);
    }
}

namespace ProjectSceneAssets
{
bool ScanProject(const ProjectPaths& paths, Scan& scan, std::string& error)
{
    error.clear();
    Scan result;

    try
    {
        const fs::path root = paths.GetRoot();
        fs::recursive_directory_iterator iterator(root);

        for(auto entry = fs::begin(iterator); entry != fs::end(iterator); ++entry)
        {
            const auto relative = entry->path().lexically_relative(root);
            const DWORD attributes = GetFileAttributesW(entry->path().c_str());

            if(attributes == INVALID_FILE_ATTRIBUTES)
            {
                throw std::runtime_error("Cannot inspect project entry: " + UTF8(relative));
            }

            if(attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                // Never follow links: a linked directory may lead outside the project or back into it.
                result.skippedLinks.push_back(relative);
                iterator.disable_recursion_pending();
                continue;
            }

            if(attributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if(IsExcludedDirectory(relative))
                {
                    iterator.disable_recursion_pending();
                }

                continue;
            }

            if(!ProjectAssetRegistry::PathsEqual(relative.extension(), ".scene"))
            {
                continue;
            }

            SceneFile file;
            file.relativePath = relative;
            std::error_code sizeError;
            const auto size = fs::file_size(entry->path(), sizeError);

            if(sizeError || size > MaximumSceneBytes)
            {
                file.status = SceneFileStatus::Invalid;
                file.problem = sizeError ? "cannot read file size: " + sizeError.message() : "file exceeds the 1 GiB scene limit";
                result.files.push_back(std::move(file));
                continue;
            }

            std::ifstream stream(entry->path(), std::ios::binary);
            std::ostringstream bytes;
            bytes << stream.rdbuf();

            if(!stream.good() && !stream.eof())
            {
                file.status = SceneFileStatus::Invalid;
                file.problem = "cannot read file";
                result.files.push_back(std::move(file));
                continue;
            }

            file.bytes = bytes.str();
            Classify(file, entry->path());
            result.files.push_back(std::move(file));
        }
    }
    catch(const std::exception& exception)
    {
        error = std::string("Scene discovery failed: ") + exception.what();
        return false;
    }

    std::sort(result.files.begin(), result.files.end(), [](const SceneFile& left, const SceneFile& right)
    {
        return UTF8(left.relativePath) < UTF8(right.relativePath);
    });
    scan = std::move(result);
    return true;
}

bool Apply(const Scan& scan, ProjectAssetRegistry& registry, Report& report, std::string& error)
{
    error.clear();
    ProjectAssetRegistry candidate = registry;

    // Every identity claimed by a scene file (current or legacy), with the files claiming it.
    std::map<std::string, std::vector<const SceneFile*>> claims;

    for(const auto& file : scan.files)
    {
        const std::string path = UTF8(file.relativePath);

        switch(file.status)
        {
        case SceneFileStatus::Invalid:
            report.invalid.push_back(path + ": " + file.problem);
            break;
        case SceneFileStatus::UnsupportedFuture:
            report.unsupported.push_back(path + ": " + file.problem);
            break;
        case SceneFileStatus::Legacy:
            report.legacy.push_back(path + ": schema " + std::to_string(file.schemaVersion) + ", id '" + file.documentID +
                "' -> " + file.assetID->ToString());
            claims[file.assetID->ToString()].push_back(&file);
            break;
        case SceneFileStatus::Current:
            claims[file.assetID->ToString()].push_back(&file);
            break;
        }
    }

    const auto fileExists = [&](const fs::path& relative)
    {
        for(const auto& file : scan.files)
        {
            if(ProjectAssetRegistry::PathsEqual(file.relativePath, relative))
            {
                return true;
            }
        }

        return false;
    };

    // Unique current scenes, the only ones registered. A legacy file sharing the id makes it ambiguous too.
    std::map<std::string, const SceneFile*> unique;

    for(const auto& [identity, files] : claims)
    {
        if(files.size() > 1)
        {
            std::string message = identity + " is claimed by";

            for(const auto* file : files)
            {
                message += " " + UTF8(file->relativePath) + (file->status == SceneFileStatus::Legacy ? " (legacy)" : "");
            }

            report.duplicates.push_back(message + "; none of them is registered until the conflict is resolved.");
            continue;
        }

        if(files.front()->status == SceneFileStatus::Current)
        {
            unique[identity] = files.front();
        }
    }

    // Stale scene records first, so a path freed by a removed record can be registered below.
    for(const auto& record : candidate.GetAssets(AssetType::Scene))
    {
        const std::string identity = record.id.ToString();

        if(unique.contains(identity))
        {
            continue;
        }

        if(claims.contains(identity) || fileExists(record.relativePath))
        {
            // The id is still claimed (a duplicate or legacy file), or its file exists but is not a usable current scene:
            // malformed is not deleted.
            report.retained.push_back(record.id.ToReference() + " (" + UTF8(record.relativePath) +
                "): retained; its file is not a current, uniquely identified scene.");
            continue;
        }

        if(!candidate.Unregister(record.id))
        {
            error = "Cannot remove the scene record " + record.id.ToReference() + ".";
            return false;
        }

        ++report.removed;
    }

    for(const auto& [identity, file] : unique)
    {
        const auto id = *file->assetID;

        if(const auto* existing = candidate.Find(id))
        {
            if(existing->type != AssetType::Scene)
            {
                report.conflicts.push_back(UTF8(file->relativePath) + ": its id " + identity +
                    " already identifies a different kind of asset (" + UTF8(existing->relativePath) + ").");
                continue;
            }

            if(ProjectAssetRegistry::PathsEqual(existing->relativePath, file->relativePath))
            {
                ++report.unchanged;
                continue;
            }

            if(fileExists(existing->relativePath))
            {
                // The registered path still holds a scene file that no longer claims this id; it was replaced, not moved.
                report.conflicts.push_back(UTF8(existing->relativePath) + " is registered as " + identity +
                    " but no longer has that id, which " + UTF8(file->relativePath) + " now has.");
                continue;
            }

            if(const auto* occupant = candidate.FindByPath(file->relativePath); occupant && !(occupant->id == id))
            {
                report.conflicts.push_back(UTF8(file->relativePath) + " is registered to another asset " +
                    occupant->id.ToReference() + ".");
                continue;
            }

            if(!candidate.UpdatePath(id, file->relativePath))
            {
                error = "Cannot update the scene record " + id.ToReference() + ".";
                return false;
            }

            ++report.moved;
            continue;
        }

        if(const auto* occupant = candidate.FindByPath(file->relativePath))
        {
            report.conflicts.push_back(UTF8(file->relativePath) + " is registered to another asset " +
                occupant->id.ToReference() + ".");
            continue;
        }

        if(!candidate.Register({id, AssetType::Scene, file->relativePath}))
        {
            report.conflicts.push_back(UTF8(file->relativePath) + ": not a valid scene asset path.");
            continue;
        }

        ++report.registered;
    }

    registry = std::move(candidate);
    return true;
}

bool Reconcile(const ProjectPaths& paths, ProjectAssetRegistry& registry, Report& report, std::string& error)
{
    Scan scan;

    if(!ScanProject(paths, scan, error))
    {
        return false;
    }

    ProjectAssetRegistry candidate = registry;

    if(!Apply(scan, candidate, report, error))
    {
        return false;
    }

    if(AssetRegistrySerializer::Serialize(candidate) == AssetRegistrySerializer::Serialize(registry))
    {
        return true;
    }

    if(!AssetRegistrySerializer::SaveToFile(candidate, paths.GetAssetRegistryPath(), &error))
    {
        error = "Cannot publish the scene asset registry: " + error;
        return false;
    }

    registry = std::move(candidate);
    return true;
}
}
