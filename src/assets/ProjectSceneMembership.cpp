#include "assets/ProjectSceneMembership.h"

#include "Project/ProjectPaths.h"
#include "Scene/SceneEnvironmentResolution.h"
#include "Scene/SceneDocumentCache.h"
#include "Scene/SceneFileCreation.h"
#include "Scene/SceneSerializer.h"
#include "assets/AssetReconciliationTransaction.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/ProjectAssetRegistry.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace
{
    namespace fs = std::filesystem;
    using namespace ProjectSceneMembership;

    const fs::path RegistryFile = "Assets.registry.json";

    std::string UTF8(const fs::path& path)
    {
        const auto text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }

    void Require(bool condition, const std::string& message)
    {
        if(!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // A scene read for inspection: its exact bytes and the shared document they parse to (W6.0: parsed once per content).
    struct Read
    {
        fs::path relativePath;
        std::string bytes;
        std::shared_ptr<const SceneDocument> document;
    };

    // A scene read for an operation that changes it: its bytes are the transaction's expected original.
    struct Loaded
    {
        fs::path relativePath;
        std::string bytes;
        SceneDocument document;
    };

    Read ReadScene(const ProjectPaths& paths, const ProjectAssetRegistry& registry, const AssetID& id)
    {
        const auto* record = registry.Find(id);
        Require(record && record->type == AssetType::Scene, "Scene " + id.ToReference() + " is not a registered scene.");
        Read read;
        read.relativePath = record->relativePath;
        std::string error;
        Require(AssetReconciliationTransaction::ReadExisting(paths, record->relativePath, read.bytes, error),
            "Cannot read " + UTF8(record->relativePath) + ": " + error);
        SceneDocumentCache::Parsed parsed;
        Require(SceneDocumentCache::Parse(paths.GetRoot() / record->relativePath, read.bytes, parsed, &error, "ProjectSceneMembership"),
            UTF8(record->relativePath) + " is not a valid scene: " + error);
        read.document = std::move(parsed.document);
        Require(read.document->id == id.ToString(), UTF8(record->relativePath) + " no longer has the identity " + id.ToString() +
            " its registry record names.");
        return read;
    }

    Loaded Load(const ProjectPaths& paths, const ProjectAssetRegistry& registry, const AssetID& id)
    {
        Read read = ReadScene(paths, registry, id);
        return {std::move(read.relativePath), std::move(read.bytes), SceneDocumentCache::Copy(*read.document)};
    }

    std::string Describe(const SceneDocument& document, const fs::path& path)
    {
        return "'" + document.name + "' (" + UTF8(path) + ")";
    }

    // Every World of the project, read to check that a Local Level is listed nowhere else. Any unreadable scene makes the
    // check impossible, so it refuses the operation.
    std::vector<Read> LoadWorlds(const ProjectPaths& paths, const ProjectAssetRegistry& registry)
    {
        std::vector<Read> worlds;

        for(const auto& record : registry.GetAssets(AssetType::Scene))
        {
            Read read = ReadScene(paths, registry, record.id);

            if(read.document->kind == SceneKind::World)
            {
                worlds.push_back(std::move(read));
            }
        }

        return worlds;
    }

    // An unassigned Local Level that no World lists.
    void RequireUnassignedLocal(const Loaded& local, const std::vector<Read>& worlds)
    {
        Require(local.document.kind == SceneKind::Local, Describe(local.document, local.relativePath) + " is a World, not a Local Level.");
        Require(local.document.owningWorld.empty(), Describe(local.document, local.relativePath) + " already belongs to World " +
            local.document.owningWorld + "; unassign it first.");

        for(const auto& world : worlds)
        {
            for(const auto& member : world.document->members)
            {
                Require(member.level != local.document.id, "World " + Describe(*world.document, world.relativePath) + " lists " +
                    Describe(local.document, local.relativePath) + ", which does not name it as its owner; resolve the conflict first.");
            }
        }
    }

    const SceneMembership* FindMember(const SceneDocument& world, const std::string& level)
    {
        const auto found = std::find_if(world.members.begin(), world.members.end(), [&](const SceneMembership& member)
        {
            return member.level == level;
        });

        return found == world.members.end() ? nullptr : &*found;
    }

    // The World and Local Level agree in both directions.
    void RequireAgreement(const Loaded& world, const SceneDocument& local, const fs::path& localPath)
    {
        Require(world.document.kind == SceneKind::World, Describe(world.document, world.relativePath) + " is not a World.");
        Require(local.kind == SceneKind::Local, Describe(local, localPath) + " is not a Local Level.");
        const bool listed = FindMember(world.document, local.id) != nullptr;
        const bool owned = local.owningWorld == world.document.id;
        Require(listed && owned, "Membership disagrees: World " + Describe(world.document, world.relativePath) +
            (listed ? " lists " : " does not list ") + Describe(local, localPath) + ", whose owner is " +
            (local.owningWorld.empty() ? std::string("none") : local.owningWorld) + ". Nothing was changed.");
    }

    void RequireNewScenePath(const ProjectPaths& paths, const ProjectAssetRegistry& registry, const fs::path& relative)
    {
        Require(!relative.empty() && !relative.is_absolute() && ProjectAssetRegistry::PathsEqual(relative.extension(), ".scene"),
            "A new scene needs a project-relative .scene path.");

        for(const auto& part : relative)
        {
            Require(part != ".." && part != ".", "Unsafe scene path " + UTF8(relative) + ".");
        }

        Require(!fs::exists(paths.GetRoot() / relative), UTF8(relative) + " already exists.");
        Require(fs::is_directory((paths.GetRoot() / relative).parent_path()), "The folder for " + UTF8(relative) + " does not exist.");
        Require(registry.FindByPath(relative) == nullptr, UTF8(relative) + " is already registered to another asset.");
    }

    SceneDocument NewDocument(SceneKind kind, const std::string& name)
    {
        SceneDocument document;
        std::string error;
        Require(CreateCleanSceneDocument(document, &error), "Cannot create a scene: " + error);
        document.kind = kind;
        document.name = name.empty() ? std::string(kind == SceneKind::World ? "Untitled World" : "Untitled Local Level") : name;
        return document;
    }

    // The bytes to write, verified by reading them back; the read-back parse is kept for the document cache.
    std::string Serialize(const SceneDocument& document, SceneDocument& readBack, std::uint64_t& readBackSchema)
    {
        std::string bytes;
        std::string error;
        Require(SerializeSceneDocument(document, bytes, &error), "Cannot write '" + document.name + "': " + error);
        Require(ParseSceneDocument(bytes, readBack, &error, &readBackSchema), "'" + document.name + "' does not read back: " + error);
        return bytes;
    }

    // One prepared change of an operation.
    struct Change
    {
        fs::path relativePath;
        SceneDocument document;
        std::optional<std::string> expectedOriginal;
        bool create = false;
    };

    // Checks the post-operation documents against each other, then commits every change and the registry together.
    void Commit(const ProjectPaths& paths, ProjectAssetRegistry& registry, std::vector<Change>& changes,
        ProjectAssetRegistry candidate, Result& result)
    {
        // Both directions over the changed documents (unchanged ones were checked when read).
        std::map<std::string, const SceneDocument*> byID;

        for(const auto& change : changes)
        {
            byID[change.document.id] = &change.document;
        }

        for(const auto& change : changes)
        {
            const auto& document = change.document;

            if(document.kind == SceneKind::World)
            {
                Require(!document.members.empty(), "World '" + document.name + "' would have no Local Levels; a World must keep at least one.");

                for(const auto& member : document.members)
                {
                    if(const auto found = byID.find(member.level); found != byID.end())
                    {
                        Require(found->second->owningWorld == document.id, "Internal membership error: '" + found->second->name +
                            "' would not name World '" + document.name + "' as its owner.");
                    }
                }
            }
            else if(!document.owningWorld.empty())
            {
                if(const auto found = byID.find(document.owningWorld); found != byID.end())
                {
                    Require(FindMember(*found->second, document.id) != nullptr, "Internal membership error: World '" +
                        found->second->name + "' would not list '" + document.name + "'.");
                }
            }
        }

        std::vector<AssetReconciliationTransaction::Replacement> replacements;
        std::vector<SceneDocument> readBacks(changes.size());
        std::vector<std::uint64_t> readBackSchemas(changes.size());

        for(std::size_t index = 0; index < changes.size(); ++index)
        {
            const auto& change = changes[index];
            AssetReconciliationTransaction::Replacement replacement;
            replacement.relativePath = change.relativePath;
            replacement.bytes = Serialize(change.document, readBacks[index], readBackSchemas[index]);
            replacement.expectedOriginal = change.expectedOriginal;
            replacement.create = change.create;
            replacements.push_back(std::move(replacement));
            result.written.push_back(change.relativePath);
        }

        if(AssetRegistrySerializer::Serialize(candidate) != AssetRegistrySerializer::Serialize(registry))
        {
            std::string registryBytes;
            std::string error;
            Require(AssetReconciliationTransaction::ReadExisting(paths, RegistryFile, registryBytes, error),
                "Cannot read the asset registry: " + error);
            replacements.push_back({RegistryFile, AssetRegistrySerializer::ToBytes(candidate), registryBytes});
            result.written.push_back(RegistryFile);
        }

        std::string error;
        Require(AssetReconciliationTransaction::Commit(paths, replacements, error), "The change was not committed: " + error);
        registry = std::move(candidate);

        // The written bytes are on disk; their verified parse serves the next reader (a later edit on disk is different content).
        for(std::size_t index = 0; index < changes.size(); ++index)
        {
            SceneDocumentCache::Seed(paths.GetRoot() / replacements[index].relativePath, replacements[index].bytes,
                std::move(readBacks[index]), readBackSchemas[index]);
        }
    }

    void Register(ProjectAssetRegistry& candidate, const SceneDocument& document, const fs::path& relative)
    {
        Require(candidate.Register({*AssetID::Parse(document.id), AssetType::Scene, relative}),
            "Cannot register " + UTF8(relative) + " as a scene asset.");
    }

    template<typename Operation>
    bool Run(std::string& error, Operation&& operation)
    {
        error.clear();

        try
        {
            operation();
            return true;
        }
        catch(const std::exception& exception)
        {
            error = exception.what();
            return false;
        }
    }

    // A World's sky: the neutral default (empty), no environment, or a registered project texture. Anything else would leave
    // every inheriting Local Level with an unresolvable sky.
    void RequireEnvironmentReference(const ProjectAssetRegistry& registry, const std::string& reference)
    {
        if(reference.empty() || reference == SceneDocument::NoneEnvironmentReference)
        {
            return;
        }

        const auto id = AssetID::ParseReference(reference);
        const auto* record = id ? registry.Find(*id) : nullptr;
        Require(record && record->type == AssetType::Texture, "The World HDRI '" + reference + "' is not a project texture.");
    }

    template<typename Settings>
    bool SameOptional(const std::optional<Settings>& left, const std::optional<Settings>& right)
    {
        return left.has_value() == right.has_value() && (!left || *left == *right);
    }

    Vector3 Wind(const std::optional<AirSimulationSettings>& simulation)
    {
        return simulation.value_or(AirSimulationSettings{}).ambientVelocity;
    }

    // W3 Import Environment: a group is inherited from the World only where the Local Level's stored values equal the World's
    // exactly (no tolerance), so the Local Level's effective environment is unchanged.
    void InheritMatchingGroups(SceneDocument& local, const SceneDocument& world)
    {
        // Only switches an enabled group off; a group already inherited stays inherited (its effective value is the World's).
        auto& overrides = local.environmentOverrides;
        const bool sameSky = SceneDocumentSkyType(local) == SceneDocumentSkyType(world) &&
            SameOptional(local.physicalSky, world.physicalSky) &&
            local.environmentReference == world.environmentReference &&
            local.lightingSettings.environmentLuminanceScale == world.lightingSettings.environmentLuminanceScale &&
            local.lightingSettings.environmentRotationDegrees == world.lightingSettings.environmentRotationDegrees;
        overrides.sky = overrides.sky && !sameSky;
        overrides.exposure = overrides.exposure && local.lightingSettings.referenceEV100 != world.lightingSettings.referenceEV100;
        overrides.haze = overrides.haze && !SameOptional(local.environmentalHaze, world.environmentalHaze);
        overrides.mist = overrides.mist && !SameOptional(local.mistRendering, world.mistRendering);
        overrides.airEnvironment = overrides.airEnvironment && !SameOptional(local.airEnvironment, world.airEnvironment);
        const Vector3 localWind = Wind(local.airSimulation);
        const Vector3 worldWind = Wind(world.airSimulation);
        const bool sameWind = localWind.x == worldWind.x && localWind.y == worldWind.y && localWind.z == worldWind.z;
        overrides.wind = overrides.wind && !sameWind;
    }

    // The environment fields of two resolutions are equal (not where they came from).
    bool SameEnvironment(const EffectiveSceneEnvironment& left, const EffectiveSceneEnvironment& right)
    {
        return DescribeEnvironmentDifferences(left, right).empty();
    }
}

namespace
{
    // W2: the one routine that turns a Local Level document into an independent asset: a new scene GUID and new entity GUIDs,
    // the hierarchy (the only reference between entities) remapped to them, no World, everything else (content, settings, asset
    // references, baked-data references) unchanged. Used by Duplicate Local Level and by Save As.
    SceneDocument IndependentLocalCopy(const SceneDocument& source, const std::string& name)
    {
        Require(source.kind == SceneKind::Local, "Only Local Levels can be copied as independent assets.");
        SceneDocument copy = source;
        const auto id = AssetID::Generate();
        Require(id.has_value(), "Cannot generate a scene identity.");
        copy.id = id->ToString();
        copy.owningWorld.clear();
        copy.name = name;
        std::unordered_map<std::string, std::string> remap;

        for(auto& object : copy.objects)
        {
            const auto objectID = AssetID::Generate();
            Require(objectID.has_value(), "Cannot generate an entity identity.");
            Require(remap.emplace(object.id, objectID->ToString()).second, "Duplicate entity identity " + object.id + " in the source.");
            object.id = objectID->ToString();
        }

        for(auto& object : copy.objects)
        {
            if(!object.parentID.empty())
            {
                const auto parent = remap.find(object.parentID);
                Require(parent != remap.end(), "Entity '" + object.name + "' has a parent outside the scene; it cannot be copied safely.");
                object.parentID = parent->second;
            }
        }

        std::string validationError;
        Require(ValidateSceneDocument(copy, &validationError), "The copy is not valid: " + validationError);
        return copy;
    }

    // Creates the independent copy as a new file and registers it, in one transaction; an existing destination is refused.
    void CommitIndependentLocal(const ProjectPaths& paths, ProjectAssetRegistry& registry, const SceneDocument& source,
        const std::string& name, const fs::path& destination, Result& result)
    {
        RequireNewScenePath(paths, registry, destination);
        const SceneDocument copy = IndependentLocalCopy(source, name);
        ProjectAssetRegistry candidate = registry;
        Register(candidate, copy, destination);
        result.local = *AssetID::Parse(copy.id);
        std::vector<Change> changes{{destination, copy, std::nullopt, true}};
        Commit(paths, registry, changes, std::move(candidate), result);
    }
}

namespace ProjectSceneMembership
{
bool CreateWorld(const ProjectPaths& paths, ProjectAssetRegistry& registry, const CreateWorldRequest& request,
    Result& result, std::string& error)
{
    result = {};
    return Run(error, [&]
    {
        Require(request.existingLocal.has_value() != request.newLocal.has_value(),
            "A new World needs exactly one initial Local Level: an existing one or a new one.");
        RequireNewScenePath(paths, registry, request.world.relativePath);
        ProjectAssetRegistry candidate = registry;
        std::vector<Change> changes;

        Change local;

        if(request.existingLocal)
        {
            Loaded loaded = Load(paths, registry, *request.existingLocal);
            RequireUnassignedLocal(loaded, LoadWorlds(paths, registry));
            local = {loaded.relativePath, loaded.document, loaded.bytes, false};
        }
        else
        {
            RequireNewScenePath(paths, registry, request.newLocal->relativePath);
            Require(!ProjectAssetRegistry::PathsEqual(request.newLocal->relativePath, request.world.relativePath),
                "The World and its Local Level need different files.");
            local = {request.newLocal->relativePath, NewDocument(SceneKind::Local, request.newLocal->name), std::nullopt, true};
            Register(candidate, local.document, local.relativePath);
        }

        Change world{request.world.relativePath, NewDocument(SceneKind::World, request.world.name), std::nullopt, true};
        // The initial Local Level is unassigned: its effective environment is its own (or the Engine defaults).
        const EffectiveSceneEnvironment before = ResolveSceneEnvironment(local.document, nullptr);

        if(request.environment == EnvironmentSource::Import)
        {
            StoreEnvironment(world.document, before);
        }

        world.document.members = {SceneMembership{local.document.id, request.placement}};
        local.document.owningWorld = world.document.id;

        if(request.environment == EnvironmentSource::Import)
        {
            InheritMatchingGroups(local.document, world.document);
            Require(SameEnvironment(before, ResolveSceneEnvironment(local.document, &world.document)),
                "Internal environment error: Import Environment would change the Local Level's effective environment.");
        }

        result.environmentChanges = DescribeEnvironmentDifferences(before, ResolveSceneEnvironment(local.document, &world.document));
        Register(candidate, world.document, world.relativePath);
        result.world = *AssetID::Parse(world.document.id);
        result.local = *AssetID::Parse(local.document.id);
        changes.push_back(std::move(world));
        changes.push_back(std::move(local));
        Commit(paths, registry, changes, std::move(candidate), result);
    });
}

bool AssignLocal(const ProjectPaths& paths, ProjectAssetRegistry& registry, const AssetID& worldID, const AssetID& localID,
    const ScenePlacement& placement, Result& result, std::string& error)
{
    result = {};
    return Run(error, [&]
    {
        Loaded world = Load(paths, registry, worldID);
        Loaded local = Load(paths, registry, localID);
        Require(world.document.kind == SceneKind::World, Describe(world.document, world.relativePath) + " is not a World.");
        RequireUnassignedLocal(local, LoadWorlds(paths, registry));
        const EffectiveSceneEnvironment before = ResolveSceneEnvironment(local.document, nullptr);
        world.document.members.push_back({local.document.id, placement});
        local.document.owningWorld = world.document.id;
        // Overrides and stored values are kept; the groups the Local Level now inherits may look different (reported, not changed).
        result.environmentChanges = DescribeEnvironmentDifferences(before, ResolveSceneEnvironment(local.document, &world.document));
        result.world = worldID;
        result.local = localID;
        std::vector<Change> changes{{world.relativePath, world.document, world.bytes, false},
            {local.relativePath, local.document, local.bytes, false}};
        Commit(paths, registry, changes, registry, result);
    });
}

bool UpdateWorldEnvironment(const ProjectPaths& paths, ProjectAssetRegistry& registry, const AssetID& worldID,
    const EffectiveSceneEnvironment& expected, const EffectiveSceneEnvironment& environment, Result& result, std::string& error)
{
    result = {};
    return Run(error, [&]
    {
        Loaded world = Load(paths, registry, worldID);
        Require(world.document.kind == SceneKind::World, Describe(world.document, world.relativePath) + " is not a World.");
        // Stale or conflicting changes: the World must still hold the values the editor started from.
        Require(SameEnvironment(ResolveSceneEnvironment(world.document, nullptr), expected), "World '" + world.document.name +
            "' changed on disk since its environment was shown; reopen the World Membership window and edit again.");
        // A newly chosen sky must resolve; a stored one is kept as is, so other groups stay editable.
        if(environment.environmentReference != expected.environmentReference)
        {
            RequireEnvironmentReference(registry, environment.environmentReference);
        }

        StoreEnvironment(world.document, environment);
        result.world = worldID;
        // Only the World changes; Local Levels inherit through resolution (their documents are not rewritten).
        std::vector<Change> changes{{world.relativePath, world.document, world.bytes, false}};
        Commit(paths, registry, changes, registry, result);
    });
}

bool SaveSceneDocuments(const ProjectPaths& paths, ProjectAssetRegistry& registry, const std::vector<DocumentSave>& saves,
    Result& result, std::string& error)
{
    result = {};
    return Run(error, [&]
    {
        Require(!saves.empty(), "There is nothing to save.");
        std::vector<Change> changes;
        changes.reserve(saves.size());

        for(const auto& save : saves)
        {
            const auto* record = registry.FindByPath(save.relativePath);
            Require(record && record->type == AssetType::Scene && record->id.ToString() == save.document.id,
                UTF8(save.relativePath) + " is not the registered scene " + save.document.id + ".");
            changes.push_back({save.relativePath, save.document, save.expectedOriginal, false});
        }

        // The transaction compares every file with its expected original before writing any of them.
        Commit(paths, registry, changes, registry, result);
    });
}

bool CreateAndAssignLocal(const ProjectPaths& paths, ProjectAssetRegistry& registry, const AssetID& worldID, const NewScene& newLocal,
    const ScenePlacement& placement, Result& result, std::string& error)
{
    result = {};
    return Run(error, [&]
    {
        Loaded world = Load(paths, registry, worldID);
        Require(world.document.kind == SceneKind::World, Describe(world.document, world.relativePath) + " is not a World.");
        RequireNewScenePath(paths, registry, newLocal.relativePath);
        ProjectAssetRegistry candidate = registry;
        SceneDocument local = NewDocument(SceneKind::Local, newLocal.name);
        local.owningWorld = world.document.id;
        world.document.members.push_back({local.id, placement});
        Register(candidate, local, newLocal.relativePath);
        result.world = worldID;
        result.local = *AssetID::Parse(local.id);
        std::vector<Change> changes{{world.relativePath, world.document, world.bytes, false},
            {newLocal.relativePath, local, std::nullopt, true}};
        Commit(paths, registry, changes, std::move(candidate), result);
    });
}

bool UnassignLocal(const ProjectPaths& paths, ProjectAssetRegistry& registry, const AssetID& localID, Result& result, std::string& error)
{
    result = {};
    return Run(error, [&]
    {
        Loaded local = Load(paths, registry, localID);
        Require(local.document.kind == SceneKind::Local, Describe(local.document, local.relativePath) + " is not a Local Level.");
        Require(!local.document.owningWorld.empty(), Describe(local.document, local.relativePath) + " is not assigned to a World.");
        const auto worldID = AssetID::Parse(local.document.owningWorld);
        Require(worldID && registry.Find(*worldID), Describe(local.document, local.relativePath) + " names World " +
            local.document.owningWorld + ", which is not a registered scene; nothing was changed.");
        Loaded world = Load(paths, registry, *worldID);
        RequireAgreement(world, local.document, local.relativePath);
        Require(world.document.members.size() > 1, "'" + local.document.name + "' is the only Local Level of World '" +
            world.document.name + "'; a World must keep at least one. Assign another Local Level first.");
        // W3: keep the effective environment: store it in the Local Level and override every group, so it looks and simulates the
        // same without the World (the World's own entities were never part of a standalone preview).
        const EffectiveSceneEnvironment effective = ResolveSceneEnvironment(local.document, &world.document);
        std::erase_if(world.document.members, [&](const SceneMembership& member)
        {
            return member.level == local.document.id;
        });
        local.document.owningWorld.clear();
        StoreEnvironment(local.document, effective);
        local.document.environmentOverrides = SceneEnvironmentOverrides{};
        Require(SameEnvironment(effective, ResolveSceneEnvironment(local.document, nullptr)),
            "Internal environment error: unassignment would change the Local Level's effective environment.");
        result.world = *worldID;
        result.local = localID;
        std::vector<Change> changes{{world.relativePath, world.document, world.bytes, false},
            {local.relativePath, local.document, local.bytes, false}};
        Commit(paths, registry, changes, registry, result);
    });
}

bool UpdatePlacement(const ProjectPaths& paths, ProjectAssetRegistry& registry, const AssetID& worldID, const AssetID& localID,
    const ScenePlacement& placement, Result& result, std::string& error)
{
    result = {};
    return Run(error, [&]
    {
        Loaded world = Load(paths, registry, worldID);
        const Read local = ReadScene(paths, registry, localID);
        RequireAgreement(world, *local.document, local.relativePath);

        for(auto& member : world.document.members)
        {
            if(member.level == local.document->id)
            {
                member.placement = placement;
            }
        }

        result.world = worldID;
        result.local = localID;
        // Only the World changes; the Local Level's entities keep their own transforms.
        std::vector<Change> changes{{world.relativePath, world.document, world.bytes, false}};
        Commit(paths, registry, changes, registry, result);
    });
}

bool DuplicateLocal(const ProjectPaths& paths, ProjectAssetRegistry& registry, const AssetID& localID, const NewScene& duplicate,
    Result& result, std::string& error)
{
    result = {};
    return Run(error, [&]
    {
        const Read source = ReadScene(paths, registry, localID);
        Require(source.document->kind == SceneKind::Local, Describe(*source.document, source.relativePath) +
            " is a World; only Local Levels can be duplicated.");
        CommitIndependentLocal(paths, registry, *source.document,
            duplicate.name.empty() ? source.document->name + " (Copy)" : duplicate.name, duplicate.relativePath, result);
    });
}

bool SaveLocalAsNew(const ProjectPaths& paths, ProjectAssetRegistry& registry, const SceneDocument& snapshot,
    const std::filesystem::path& destination, Result& result, std::string& error)
{
    result = {};
    return Run(error, [&]
    {
        Require(snapshot.kind == SceneKind::Local, "Save As is not available for a World.");
        Require(snapshot.owningWorld.empty(), "Save As is not available for a Local Level assigned to a World: the copy would "
            "share its World membership. Use Duplicate Local Level in the Asset Browser.");
        CommitIndependentLocal(paths, registry, snapshot, snapshot.name, destination, result);
    });
}

bool ReadScenes(const ProjectPaths& paths, const ProjectAssetRegistry& registry, std::vector<SceneInfo>& scenes,
    std::vector<std::string>& problems)
{
    scenes.clear();

    for(const auto& record : registry.GetAssets(AssetType::Scene))
    {
        try
        {
            const Read read = ReadScene(paths, registry, record.id);
            const SceneDocument& document = *read.document;
            scenes.push_back({record.id, record.relativePath, document.kind, document.name, document.owningWorld, document.members,
                ResolveSceneEnvironment(document, nullptr)});
        }
        catch(const std::exception& exception)
        {
            problems.push_back(exception.what());
        }
    }

    return problems.empty();
}

std::vector<std::string> ValidateProject(const std::vector<SceneInfo>& scenes)
{
    std::vector<std::string> problems;
    std::map<std::string, const SceneInfo*> byID;
    std::map<std::string, std::vector<std::string>> listedBy;

    for(const auto& scene : scenes)
    {
        byID[scene.id.ToString()] = &scene;
    }

    for(const auto& scene : scenes)
    {
        const std::string label = "'" + scene.name + "' (" + UTF8(scene.relativePath) + ")";

        if(scene.kind == SceneKind::World)
        {
            if(scene.members.empty())
            {
                problems.push_back("World " + label + " has no Local Levels.");
            }

            for(const auto& member : scene.members)
            {
                listedBy[member.level].push_back(scene.id.ToString());
                const auto found = byID.find(member.level);

                if(found == byID.end())
                {
                    problems.push_back("World " + label + " lists " + member.level + ", which is not a registered scene.");
                }
                else if(found->second->kind != SceneKind::Local)
                {
                    problems.push_back("World " + label + " lists '" + found->second->name + "', which is not a Local Level.");
                }
                else if(found->second->owningWorld != scene.id.ToString())
                {
                    problems.push_back("World " + label + " lists '" + found->second->name + "', whose owner is " +
                        (found->second->owningWorld.empty() ? std::string("none") : found->second->owningWorld) + ".");
                }
            }
        }
        else if(!scene.owningWorld.empty())
        {
            const auto found = byID.find(scene.owningWorld);

            if(found == byID.end())
            {
                problems.push_back("Local Level " + label + " names World " + scene.owningWorld + ", which is not a registered scene.");
            }
            else if(found->second->kind != SceneKind::World)
            {
                problems.push_back("Local Level " + label + " names '" + found->second->name + "' as its World, which is not a World.");
            }
            else if(std::none_of(found->second->members.begin(), found->second->members.end(), [&](const SceneMembership& member)
                {
                    return member.level == scene.id.ToString();
                }))
            {
                problems.push_back("Local Level " + label + " names World '" + found->second->name + "', which does not list it.");
            }
        }
    }

    for(const auto& [level, worlds] : listedBy)
    {
        if(worlds.size() > 1)
        {
            problems.push_back("Local Level " + level + " is listed by " + std::to_string(worlds.size()) + " Worlds.");
        }
    }

    return problems;
}
}
