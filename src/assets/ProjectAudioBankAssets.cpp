#include "assets/ProjectAudioBankAssets.h"
#include "Project/ProjectPaths.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/ProjectAssetRegistry.h"

#include <Windows.h>
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace ProjectAudioBankAssets
{
namespace
{
namespace fs = std::filesystem;

constexpr std::size_t MaximumDirectoryEntries = 16384;
constexpr std::size_t MaximumBanks = 4096;

void Require(bool condition, const char* message)
{
    if(!condition)
    {
        throw std::runtime_error(message);
    }
}

std::string UTF8(const fs::path& path)
{
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

// False when the directory does not exist. A reparse point (junction or symbolic link) is rejected: discovered
// identities must name ordinary project-owned files.
bool InspectDirectory(const fs::path& directory)
{
    const DWORD attributes = GetFileAttributesW(directory.c_str());

    if(attributes == INVALID_FILE_ATTRIBUTES)
    {
        const DWORD error = GetLastError();
        Require(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND, "Cannot inspect the audio bank directory.");
        return false;
    }

    Require((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
        "Audio bank discovery requires ordinary Content/Audio/Banks directories (no links or files in their place).");
    return true;
}

std::vector<fs::path> FindBankFiles(const ProjectPaths& paths)
{
    const fs::path banks = paths.GetAudioBanksDirectory();
    std::vector<fs::path> files;

    for(const auto& directory : {banks.parent_path().parent_path(), banks.parent_path(), banks})
    {
        if(!InspectDirectory(directory))
        {
            return files;
        }
    }

    std::size_t visited = 0;

    for(const auto& entry : fs::recursive_directory_iterator(banks))
    {
        Require(++visited <= MaximumDirectoryEntries, "Audio bank discovery entry limit exceeded.");
        const DWORD attributes = GetFileAttributesW(entry.path().c_str());
        Require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "Audio bank discovery rejects reparse points.");

        if((attributes & FILE_ATTRIBUTE_DIRECTORY) || !ProjectAssetRegistry::PathsEqual(entry.path().extension(), ".bank"))
        {
            continue;
        }

        files.push_back(entry.path().lexically_relative(paths.GetRoot()));
        Require(files.size() <= MaximumBanks, "Audio bank discovery asset limit exceeded.");
    }

    // Stable registration order, independent of directory enumeration order.
    std::sort(files.begin(), files.end(), [](const fs::path& left, const fs::path& right)
    {
        return UTF8(left) < UTF8(right);
    });

    return files;
}
}

bool Reconcile(const ProjectPaths& paths, ProjectAssetRegistry& registry, ReconcileReport& report, std::string& error)
{
    error.clear();
    report = {};

    try
    {
        const std::vector<fs::path> files = FindBankFiles(paths);
        ProjectAssetRegistry candidate = registry;
        bool changed = false;

        for(const fs::path& path : files)
        {
            if(const AssetRecord* existing = candidate.FindByPath(path))
            {
                Require(existing->type == AssetType::AudioBank && existing->relativePath == path,
                    "Audio bank path conflicts with an existing asset identity or its canonical spelling.");
                continue;
            }

            const auto id = AssetID::Generate();
            Require(id.has_value(), "Cannot generate an audio bank asset identity.");

            const AssetRecord record{*id, AssetType::AudioBank, path};
            Require(ValidateAssetRecord(record),
                "Audio bank file name is not a valid project path (reserved or unsupported characters).");
            Require(candidate.Register(record), "Cannot register audio bank asset identity.");
            changed = true;
        }

        if(changed)
        {
            Require(AssetRegistrySerializer::SaveToFile(candidate, paths.GetAssetRegistryPath(), &error),
                "Cannot publish the audio bank asset registry.");
            registry = std::move(candidate);
        }

        for(const AssetRecord& record : registry.GetAssets(AssetType::AudioBank))
        {
            ++report.registered;
            const bool present = std::any_of(files.begin(), files.end(), [&record](const fs::path& path)
            {
                return path == record.relativePath;
            });

            if(present)
            {
                ++report.available;
            }
            else
            {
                ++report.missing;
            }
        }

        return true;
    }
    catch(const std::exception& exception)
    {
        if(error.empty())
        {
            error = exception.what();
        }
        else
        {
            error = std::string(exception.what()) + " " + error;
        }

        return false;
    }
}
}
