#include "assets/ProjectSceneAssets.h"

#include "Project/ProjectPaths.h"
#include "Scene/SceneAssetIdentity.h"
#include "Scene/SceneDocumentCache.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/ProjectAssetRegistry.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
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

    // Startup work (4.3): a persistent index of the classification of current scenes, keyed by the SHA-256 of the file's exact
    // bytes, so an unchanged project is not parsed on every open (the default project's 70 MiB of scenes took 17 s in Debug).
    // As SceneDocumentCache, timestamps and sizes are never trusted: every file is still read, and only identical content
    // reuses a classification. Only successful current-schema classifications are stored (legacy, invalid and future scenes
    // are always parsed). The index belongs to this executable's scene schema and reader: a different schema version or
    // IndexFormat discards it. Bump IndexFormat when the scene reader starts refusing documents it used to accept.
    // %LOCALAPPDATA%\Engine\SceneScanIndex\v1.txt; ENGINE_SCENE_SCAN_INDEX=0 disables it.
    namespace ScanIndex
    {
        constexpr std::uint32_t IndexFormat = 1;
        constexpr std::size_t MaximumEntries = 4096;

        struct Entry
        {
            std::uint32_t schemaVersion = 0;
            SceneKind kind = SceneKind::Local;
            std::string documentID;
            bool used = false;
        };

        struct State
        {
            std::mutex mutex;
            bool loaded = false;
            bool dirty = false;
            std::map<std::string, Entry> entries;
        };

        State& Get()
        {
            static State state;
            return state;
        }

        bool Enabled()
        {
            static const bool enabled = []()
            {
                char value[8]{};
                const DWORD length = GetEnvironmentVariableA("ENGINE_SCENE_SCAN_INDEX", value, sizeof(value));
                return !(length == 1 && value[0] == '0');
            }();
            return enabled;
        }

        std::optional<std::string> HashHex(std::string_view bytes)
        {
            std::array<unsigned char, 32> digest{};

            if(bytes.size() > (std::numeric_limits<ULONG>::max)() ||
                BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                    static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size())) < 0)
            {
                return std::nullopt;
            }

            static constexpr char digits[] = "0123456789abcdef";
            std::string text(64, '0');

            for(std::size_t index = 0; index < digest.size(); ++index)
            {
                text[index * 2] = digits[digest[index] >> 4];
                text[index * 2 + 1] = digits[digest[index] & 15];
            }

            return text;
        }

        fs::path FilePath()
        {
            std::wstring local(32768, L'\0');
            const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));

            if(length == 0 || length >= local.size())
            {
                return {};
            }

            local.resize(length);
            return fs::path(local) / "Engine" / "SceneScanIndex" / "v1.txt";
        }

        std::string Header()
        {
            return "EngineSceneScanIndex " + std::to_string(IndexFormat) + " schema " +
                std::to_string(SceneDocument::kSchemaVersion);
        }

        // Called with the state locked. A missing, unreadable or foreign index is simply empty.
        void Load(State& state)
        {
            state.loaded = true;
            const fs::path path = FilePath();

            if(path.empty())
            {
                return;
            }

            std::ifstream stream(path, std::ios::binary);
            std::string line;

            if(!stream || !std::getline(stream, line) || line != Header())
            {
                return;
            }

            while(std::getline(stream, line))
            {
                std::istringstream fields(line);
                std::string hash;
                std::uint32_t schema = 0;
                unsigned int kind = 0;
                std::string id;

                if(!(fields >> hash >> schema >> kind >> id) || hash.size() != 64 || kind > 1u ||
                    schema < SceneDocument::kSceneKindSchemaVersion || schema > SceneDocument::kSchemaVersion ||
                    !AssetID::Parse(id))
                {
                    continue;
                }

                state.entries[hash] = Entry{schema, static_cast<SceneKind>(kind), id, false};
            }
        }

        bool Find(const std::string& hash, Entry& found)
        {
            State& state = Get();
            const std::lock_guard lock(state.mutex);

            if(!state.loaded)
            {
                Load(state);
            }

            const auto entry = state.entries.find(hash);

            if(entry == state.entries.end())
            {
                return false;
            }

            entry->second.used = true;
            found = entry->second;
            return true;
        }

        void Store(const std::string& hash, std::uint32_t schema, SceneKind kind, const std::string& id)
        {
            State& state = Get();
            const std::lock_guard lock(state.mutex);
            state.entries[hash] = Entry{schema, kind, id, true};
            state.dirty = true;
        }

        // Writes the index once a scan added to it: entries used by this process first, then older ones up to the bound.
        // Written to a temporary file and moved into place, so a concurrent reader sees the old or the new index whole.
        void SaveIfChanged()
        {
            State& state = Get();
            const std::lock_guard lock(state.mutex);

            if(!state.dirty)
            {
                return;
            }

            state.dirty = false;
            const fs::path path = FilePath();

            if(path.empty())
            {
                return;
            }

            std::string text = Header() + "\n";
            std::size_t written = 0;

            for(const bool used : {true, false})
            {
                for(const auto& [hash, entry] : state.entries)
                {
                    if(entry.used != used || written >= MaximumEntries)
                    {
                        continue;
                    }

                    text += hash + " " + std::to_string(entry.schemaVersion) + " " +
                        std::to_string(static_cast<unsigned int>(entry.kind)) + " " + entry.documentID + "\n";
                    ++written;
                }
            }

            std::error_code error;
            fs::create_directories(path.parent_path(), error);
            const fs::path temporary = path.parent_path() / ("v1." + std::to_string(GetCurrentProcessId()) + ".tmp");

            {
                std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
                stream << text;
                stream.flush();

                if(!stream.good())
                {
                    stream.close();
                    fs::remove(temporary, error);
                    return;
                }
            }

            if(!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                fs::remove(temporary, error);
            }
        }
    }

    // The whole file in one read (an ostringstream copy of 58 MB costs seconds in Debug). False when it cannot be read, or
    // when its size changed since `size` was taken (the next scan reads it again).
    bool ReadWholeFile(const fs::path& path, std::uintmax_t size, std::string& bytes)
    {
        const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

        if(handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        bytes.resize(static_cast<std::size_t>(size));
        std::size_t total = 0;
        bool ok = true;

        while(total < bytes.size())
        {
            const DWORD request = static_cast<DWORD>((std::min<std::size_t>)(bytes.size() - total, std::size_t{1} << 30));
            DWORD read = 0;

            if(!ReadFile(handle, bytes.data() + total, request, &read, nullptr))
            {
                ok = false;
                break;
            }

            if(read == 0)
            {
                break;
            }

            total += read;
        }

        char extra = 0;
        DWORD tail = 0;
        ok = ok && total == bytes.size() && ReadFile(handle, &extra, 1, &tail, nullptr) && tail == 0;
        CloseHandle(handle);
        return ok;
    }

    void Classify(ProjectSceneAssets::SceneFile& file, const fs::path& absolutePath)
    {
        using ProjectSceneAssets::SceneFileStatus;
        std::optional<std::string> hash;

        if(ScanIndex::Enabled())
        {
            hash = ScanIndex::HashHex(file.bytes);
            ScanIndex::Entry entry;

            if(hash && ScanIndex::Find(*hash, entry))
            {
                file.schemaVersion = entry.schemaVersion;
                file.documentID = entry.documentID;
                file.kind = entry.kind;
                file.status = SceneFileStatus::Current;
                file.assetID = AssetID::Parse(entry.documentID);
                return;
            }
        }

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

            if(hash && file.assetID)
            {
                ScanIndex::Store(*hash, file.schemaVersion, file.kind, file.documentID);
            }

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

            if(!ReadWholeFile(entry->path(), size, file.bytes))
            {
                file.bytes.clear();
                file.status = SceneFileStatus::Invalid;
                file.problem = "cannot read file";
                result.files.push_back(std::move(file));
                continue;
            }

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

    if(ScanIndex::Enabled())
    {
        ScanIndex::SaveIfChanged();
    }

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
