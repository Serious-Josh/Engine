
#include "assets/ProjectAssetResolver.h"

#include <system_error>

namespace
{
    bool IsContainedWithin(
        const std::filesystem::path& child,
        const std::filesystem::path& directory)
    {
        const auto relative = child.lexically_relative(directory);

        if(relative.empty() || relative.is_absolute())
        {
            return false;
        }

        for(const auto& component : relative)
        {
            if(component == "..")
            {
                return false;
            }
        }

        return relative != ".";
    }

    std::filesystem::path GetCategoryDirectory(
        const ProjectPaths& paths,
        AssetType type)
    {
        switch(type){
            case AssetType::Material:
                return paths.GetMaterialsDirectory();

            case AssetType::Mesh:
                return paths.GetMeshesDirectory();
            case AssetType::Script:
                return paths.GetScriptsDirectory();

            case AssetType::Texture:
                // Textures may live anywhere below the project's asset root.
                return paths.GetRoot();

            default:
                return {};
        }
    }
}

AssetResolution ProjectAssetResolver::Resolve(
    const ProjectPaths& projectPaths,
    const ProjectAssetRegistry& registry,
    const AssetID& id)
{
    namespace fs = std::filesystem;

    const AssetRecord* record = registry.Find(id);

    if(!record)
    {
        return { AssetAvailability::NotRegistered, {} };
    }

    if(!ValidateAssetRecord(*record))
    {
        return { AssetAvailability::InvalidPath, {} };
    }

    const fs::path categoryDirectory = GetCategoryDirectory(projectPaths, record->type);

    if(categoryDirectory.empty() ||
       projectPaths.GetRoot().empty() ||
       !projectPaths.GetRoot().is_absolute())
    {
        return { AssetAvailability::InvalidPath, {} };
    }

    const fs::path candidate = projectPaths.GetRoot() / record->relativePath;

    std::error_code error;

    // Resolve existing directory aliases, including junctions.
    const fs::path resolvedRoot = fs::canonical(projectPaths.GetRoot(), error);

    if(error)
    {
        return { AssetAvailability::Unreadable, {} };
    }

    const fs::path resolvedCategory = fs::canonical(categoryDirectory, error);

    if(error)
    {
        return { AssetAvailability::Unreadable, {} };
    }

    // Texture resolution uses the root itself; other category directories
    // must still remain strictly inside it, including through junctions.
    const bool textureRoot = record->type == AssetType::Texture && resolvedCategory == resolvedRoot;

    if(!textureRoot && !IsContainedWithin(resolvedCategory, resolvedRoot))
    {
        return { AssetAvailability::InvalidPath, {} };
    }

    // weakly_canonical resolves existing path components even when
    // the final asset file does not exist.
    const fs::path resolvedCandidate = fs::weakly_canonical(candidate, error);

    if(error)
    {
        return { AssetAvailability::Unreadable, {} };
    }

    if(!IsContainedWithin(resolvedCandidate, resolvedCategory))
    {
        return { AssetAvailability::InvalidPath, {} };
    }

    const fs::file_status status = fs::status(resolvedCandidate, error);

    if(error == std::errc::no_such_file_or_directory)
    {
        return { AssetAvailability::Missing, {} };
    }

    if(error)
    {
        return { AssetAvailability::Unreadable, {} };
    }

    if(!fs::exists(status))
    {
        return { AssetAvailability::Missing, {} };
    }

    if(!fs::is_regular_file(status))
    {
        return { AssetAvailability::InvalidPath, {} };
    }

    return {
        AssetAvailability::Available,
        resolvedCandidate
    };
}
