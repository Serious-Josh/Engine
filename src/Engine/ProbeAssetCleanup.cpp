#include "Engine/ProbeAssetCleanup.h"

#include "Project/ProjectPaths.h"
#include "Scene/SceneSerializer.h"
#include "assets/ProbeBatchAsset.h"

#include <algorithm>

namespace ProbeAssetCleanup
{
Report Scan(const ProjectPaths& paths, const ProjectAssetRegistry& registry, const std::set<std::string>& liveReferences)
{
    Report report;
    std::set<std::string> referenced = liveReferences;
    std::set<std::filesystem::path> probeFiles;
    std::error_code error;
    std::filesystem::recursive_directory_iterator walker(paths.GetRoot(), std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;

    if(error)
    {
        report.reason = "the project folder cannot be read";
        return report;
    }

    for(; walker != end; walker.increment(error))
    {
        if(error)
        {
            report.reason = "the project folder cannot be read";
            return report;
        }

        const std::filesystem::path path = walker->path();

        if(ProjectAssetRegistry::PathsEqual(path.extension(), ".scene"))
        {
            SceneDocument document;
            std::string sceneError;

            if(!LoadSceneDocument(path, document, &sceneError))
            {
                report.reason = "the scene " + path.generic_string() + " cannot be read, so what it references is unknown: " + sceneError;
                return report;
            }

            ++report.scenesRead;

            for(const SceneObjectDocument& object : document.objects)
            {
                if(object.probeVolume && !object.probeVolume->probeBatch.empty())
                {
                    referenced.insert(object.probeVolume->probeBatch);
                }
            }
        }
        else if(ProjectAssetRegistry::PathsEqual(path.extension(), std::filesystem::path(ProbeBatchAssets::Extension)))
        {
            probeFiles.insert(path.lexically_normal());
        }
    }

    for(const AssetRecord& record : registry.GetAssets(AssetType::ProbeBatch))
    {
        const std::filesystem::path file = (paths.GetRoot() / record.relativePath).lexically_normal();
        probeFiles.erase(file);

        if(referenced.contains(record.id.ToReference()))
        {
            ++report.referenced;
            continue;
        }

        report.unused.push_back(record.id);
        std::error_code sizeError;
        const auto size = std::filesystem::file_size(file, sizeError);
        report.unusedBytes += sizeError ? 0 : size;
    }

    report.unregisteredFiles.assign(probeFiles.begin(), probeFiles.end());
    report.conclusive = true;
    return report;
}

std::size_t RemoveConfirmed(const ProjectPaths& paths, ProjectAssetRegistry& registry, const std::set<std::string>& liveReferences,
    const std::vector<AssetID>& confirmed, std::string& error)
{
    const Report fresh = Scan(paths, registry, liveReferences);

    if(!fresh.conclusive)
    {
        error = fresh.reason;
        return 0;
    }

    std::size_t removed = 0;

    for(const AssetID& id : fresh.unused)
    {
        if(std::find(confirmed.begin(), confirmed.end(), id) == confirmed.end())
        {
            continue;
        }

        if(!ProbeBatchAssets::Remove(paths, registry, id.ToReference(), &error))
        {
            return removed;
        }

        ++removed;
    }

    return removed;
}
}
