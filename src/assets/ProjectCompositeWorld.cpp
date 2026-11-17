#include "assets/ProjectCompositeWorld.h"

#include "Project/ProjectPaths.h"
#include "Scene/SceneSerializer.h"
#include "assets/AssetReconciliationTransaction.h"
#include "assets/ProjectAssetRegistry.h"
#include "assets/ProjectSceneMembership.h"

namespace ProjectCompositeWorld
{
    namespace
    {
        bool Fail(std::string& error, std::string message)
        {
            error = std::move(message);
            return false;
        }

        bool ReadSource(const ProjectPaths& paths, const std::filesystem::path& relativePath, CompositeWorldSource& source,
            std::string& error)
        {
            std::string loadError;

            // The exact bytes are kept as the save baseline: Save hands them to the transaction as the expected original, so a file
            // changed on disk since it was read is never overwritten.
            if(!AssetReconciliationTransaction::ReadExisting(paths, relativePath, source.bytes, loadError) ||
                !ParseSceneDocument(source.bytes, source.document, &loadError))
            {
                return Fail(error, "Cannot read " + relativePath.generic_string() + ": " + loadError);
            }

            source.relativePath = relativePath;
            return true;
        }
    }

    bool Load(const ProjectPaths& paths, const ProjectAssetRegistry& registry, const std::filesystem::path& worldPath,
        CompositeWorld& composite, std::string& error)
    {
        std::error_code relativeError;
        const auto worldRelative = std::filesystem::relative(worldPath, paths.GetRoot(), relativeError);

        if(relativeError || worldRelative.empty() || *worldRelative.begin() == "..")
        {
            return Fail(error, "The World " + worldPath.string() + " is not inside the project.");
        }

        CompositeWorldSource world;

        if(!ReadSource(paths, worldRelative, world, error))
        {
            return false;
        }

        // Project-wide view: each member must be a registered scene and no other World may list it.
        std::vector<ProjectSceneMembership::SceneInfo> scenes;
        std::vector<std::string> problems;

        // An unreadable scene elsewhere in the project does not block this World: members are read and validated individually
        // below, and the scan still returns every readable scene. (An unreadable *other* World cannot be checked for a double claim;
        // W2 membership transactions never create one.)
        ProjectSceneMembership::ReadScenes(paths, registry, scenes, problems);

        std::vector<CompositeWorldSource> locals;
        locals.reserve(world.document.members.size());

        for(const auto& member : world.document.members)
        {
            const auto id = AssetID::Parse(member.level);
            const auto* record = id ? registry.Find(*id) : nullptr;

            if(!record || record->type != AssetType::Scene)
            {
                return Fail(error, "World '" + world.document.name + "' lists Local Level " + member.level +
                    ", which is not a registered scene (missing or deleted).");
            }

            for(const auto& other : scenes)
            {
                const bool otherWorld = other.kind == SceneKind::World && other.id.ToString() != world.document.id;

                if(otherWorld && std::any_of(other.members.begin(), other.members.end(), [&](const SceneMembership& listed)
                    {
                        return listed.level == member.level;
                    }))
                {
                    return Fail(error, "Local Level " + member.level + " is listed by World '" + world.document.name + "' and by World '" +
                        other.name + "' (" + other.relativePath.generic_string() + ").");
                }
            }

            CompositeWorldSource local;

            if(!ReadSource(paths, record->relativePath, local, error))
            {
                return false;
            }

            locals.push_back(std::move(local));
        }

        if(!BuildCompositeWorld(world, locals, composite, error))
        {
            return false;
        }

        // Reported, never hidden: the members themselves were fully validated, but a World that could not be read may still list one.
        composite.unverifiedScenes = std::move(problems);
        return true;
    }
}
