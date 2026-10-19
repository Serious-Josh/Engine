#include "assets/ProjectScriptAssets.h"
#include "Project/ProjectPaths.h"
#include "assets/ProjectAssetRegistry.h"
#include "assets/AssetRegistrySerializer.h"
#include "Platform/Windows/WindowsText.h"

#include <Windows.h>
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace ProjectScriptAssets
{
namespace
{
namespace fs = std::filesystem;

std::string UTF8(const fs::path& path)
{
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

void Require(bool condition, const char* message)
{
    if(!condition)
    {
        throw std::runtime_error(message);
    }
}

struct HandleCloser
{
    void operator()(void* handle) const noexcept
    {
        CloseHandle(handle);
    }
};

class PinnedSources
{
public:
    explicit PinnedSources(const ProjectPaths& paths) : m_root(paths.GetRoot())
    {
        Require(m_root.is_absolute(), "Script project root must be absolute.");
        fs::path prefix;

        for(const auto& part : m_root)
        {
            prefix /= part;

            if(prefix.has_root_directory())
            {
                Pin(prefix, true);
            }
        }
    }

    ~PinnedSources() = default;

    PinnedSources(const PinnedSources&) = delete;
    PinnedSources& operator=(const PinnedSources&) = delete;

    void PinDirectory(const fs::path& directory)
    {
        PinCanonical(directory.lexically_relative(m_root), true);
    }

    std::string Read(std::string_view name, std::size_t limit)
    {
        const auto handle = PinCanonical(fs::path(WindowsText::FromUTF8(name)), false);
        LARGE_INTEGER size{};
        Require(GetFileSizeEx(handle, &size) && size.QuadPart >= 0 &&
            static_cast<unsigned long long>(size.QuadPart) <= limit, "Script source exceeds its byte limit.");
        std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        Require(ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
            read == bytes.size(), "Cannot read pinned script source.");
        return bytes;
    }

    void Inspect(const fs::path& relative)
    {
        PinCanonical(relative, false);
    }

private:
    HANDLE Pin(const fs::path& path, bool directory)
    {
        const auto handle = CreateFileW(path.c_str(), directory ? FILE_READ_ATTRIBUTES : GENERIC_READ,
            directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
        Require(handle != INVALID_HANDLE_VALUE, "Cannot pin script path (missing, locked or unreadable).");

        std::unique_ptr<void, HandleCloser> owner(handle);
        m_handles.push_back(std::move(owner));

        BY_HANDLE_FILE_INFORMATION info{};
        Require(GetFileInformationByHandle(handle, &info) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory &&
            (directory || info.nNumberOfLinks == 1), "Script links/reparse points and nonordinary paths are forbidden.");

        if(!directory)
        {
            const auto identity = std::tuple{info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
            Require(m_files.insert(identity).second, "Ambiguous script file identity alias.");
        }

        return handle;
    }

    HANDLE PinCanonical(const fs::path& relative, bool directory)
    {
        Require(!relative.empty() && !relative.has_root_path(), "Script path must be project-relative.");
        fs::path path = m_root;
        HANDLE handle = INVALID_HANDLE_VALUE;
        std::size_t depth = 0;

        for(auto part = relative.begin(); part != relative.end(); ++part)
        {
            Require(++depth <= 64 && *part != "." && *part != "..", "Script path depth/traversal rejected.");
            std::size_t matches = 0;
            std::size_t visited = 0;
            fs::path actual;

            for(const auto& entry : fs::directory_iterator(path))
            {
                Require(++visited <= 16384, "Script path directory entry limit exceeded.");
                if(ProjectAssetRegistry::PathsEqual(entry.path().filename(), *part))
                {
                    ++matches;
                    actual = entry.path().filename();
                }
            }

            Require(matches == 1 && actual == *part, "Missing or ambiguous/case-aliased script path.");
            path /= *part;
            auto next = part;
            ++next;
            handle = Pin(path, next != relative.end() || directory);
        }

        return handle;
    }

    fs::path m_root;
    std::vector<std::unique_ptr<void, HandleCloser>> m_handles;
    std::set<std::tuple<DWORD, DWORD, DWORD>> m_files;
};

Scripting::SourceReader Reader(PinnedSources& pins, const ProjectAssetRegistry& registry, std::size_t limit)
{
    return [&pins, &registry, limit](std::string_view name)
    {
        const auto* record = registry.FindByPath(fs::path(WindowsText::FromUTF8(name)));
        Require(record && record->type == AssetType::Script && UTF8(record->relativePath) == name,
            "Source is not an exact registered project Script asset; Refresh script discovery first.");
        return Scripting::SourceSection{std::string(name), record->id.ToReference(), pins.Read(name, limit)};
    };
}

Scripting::Result AttributeRoot(Scripting::Result result, const AssetID& root)
{
    if(result.snapshot && result.snapshot->rootAsset.empty())
    {
        auto snapshot = std::make_shared<Scripting::SourceSnapshot>(*result.snapshot);
        snapshot->rootAsset = root.ToReference();
        snapshot->revision = Scripting::SourceClosureRevision(*snapshot);
        result.snapshot = std::move(snapshot);
    }

    for(auto& diagnostic : result.diagnostics)
    {
        if(diagnostic.assetReference.empty())
        {
            diagnostic.assetReference = root.ToReference();
        }

        if(result.snapshot)
        {
            diagnostic.sourceRevision = result.snapshot->revision;
        }
    }

    return result;
}
}

bool Reconcile(const ProjectPaths& paths, ProjectAssetRegistry& registry, std::string& error)
{
    error.clear();

    try
    {
        PinnedSources pins(paths);
        const auto scripts = paths.GetScriptsDirectory();

        const auto content = scripts.parent_path();
        const DWORD contentAttributes = GetFileAttributesW(content.c_str());

        if(contentAttributes == INVALID_FILE_ATTRIBUTES)
        {
            Require(GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND,
                "Cannot inspect script content directory.");
            return true;
        }

        pins.PinDirectory(content);
        const DWORD scriptAttributes = GetFileAttributesW(scripts.c_str());

        if(scriptAttributes == INVALID_FILE_ATTRIBUTES)
        {
            Require(GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND,
                "Cannot inspect script source directory.");
            return true;
        }

        pins.PinDirectory(scripts);
        std::vector<fs::path> files;
        std::size_t visited = 0;

        for(const auto& entry : fs::recursive_directory_iterator(scripts))
        {
            Require(++visited <= 16384, "Script discovery entry limit exceeded.");
            const DWORD attributes = GetFileAttributesW(entry.path().c_str());
            Require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
                "Script discovery rejects reparse points.");

            if(attributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                pins.PinDirectory(entry.path());
            }
            else if(ProjectAssetRegistry::PathsEqual(entry.path().extension(), ".as"))
            {
                const auto relative = entry.path().lexically_relative(paths.GetRoot());
                Scripting::CanonicalScriptSection(UTF8(relative));
                pins.Inspect(relative);
                files.push_back(relative);
                Require(files.size() <= 4096, "Script discovery asset limit exceeded.");
            }
        }

        std::sort(files.begin(), files.end(), [](const auto& left, const auto& right)
        {
            return UTF8(left) < UTF8(right);
        });
        ProjectAssetRegistry candidate = registry;
        bool changed = false;

        for(const auto& path : files)
        {
            if(const auto* existing = candidate.FindByPath(path))
            {
                Require(existing->type == AssetType::Script && existing->relativePath == path,
                    "Script path conflicts with existing asset identity/canonical spelling.");
                continue;
            }

            const auto id = AssetID::Generate();
            Require(id && candidate.Register({*id, AssetType::Script, path}), "Cannot register script asset identity.");
            changed = true;
        }

        if(changed)
        {
            Require(AssetRegistrySerializer::SaveToFile(candidate, paths.GetAssetRegistryPath(), &error),
                "Cannot publish script asset registry.");
            registry = std::move(candidate);
        }

        return true;
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

Scripting::Result Validate(Scripting::ScriptHost& host, const ProjectPaths& paths,
    const ProjectAssetRegistry& registry, const AssetID& root, Scripting::SourceLimits limits)
{
    std::string rootSection = "Content/Scripts/unavailable.as";

    try
    {
        const auto* record = registry.Find(root);
        Require(record && record->type == AssetType::Script, "Root is not a registered Script asset.");
        rootSection = UTF8(record->relativePath);
        PinnedSources pins(paths);
        return AttributeRoot(host.ValidateSources(rootSection, Reader(pins, registry, limits.sectionBytes), limits), root);
    }
    catch(const std::exception& exception)
    {
        // Invalidate a previously executable AS.2 module even on admission failure.
        auto result = host.ValidateSources(rootSection, [&](std::string_view) -> Scripting::SourceSection
        {
            throw std::runtime_error(exception.what());
        }, limits);

        return AttributeRoot(std::move(result), root);
    }
}

bool IsCurrent(const Scripting::SourceSnapshot& snapshot, const ProjectPaths& paths,
    const ProjectAssetRegistry& registry, std::string& error)
{
    error.clear();

    try
    {
        Require(snapshot.complete, "An incomplete source closure is not current.");
        Require(Scripting::SourceClosureRevision(snapshot) == snapshot.revision, "Invalid snapshot content identity.");
        PinnedSources pins(paths);
        auto read = Reader(pins, registry, 1024 * 1024);
        auto current = snapshot;

        for(auto& section : current.sections)
        {
            section = read(section.name);
        }

        Require(Scripting::SourceClosureRevision(current) == snapshot.revision, "Script source revision is stale.");
        return true;
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}
}
