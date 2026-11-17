#include "Scene/CompositeWorld.h"

#include "Scene/LightComponent.h"
#include "Scene/SceneDocumentAdapter.h"
#include "Scene/SceneHierarchy.h"

#include <algorithm>
#include <unordered_set>

namespace
{
    bool Fail(std::string& error, std::string message)
    {
        error = std::move(message);
        return false;
    }

    std::string Describe(const CompositeWorldSource& source)
    {
        return "'" + source.document.name + "' (" + source.relativePath.generic_string() + ")";
    }

    std::size_t CountLights(const SceneDocument& document)
    {
        std::size_t count = 0;

        for(const auto& object : document.objects)
        {
            if(object.light)
            {
                ++count;
            }
        }

        return count;
    }

    bool HasDirectionalLight(const SceneDocument& document)
    {
        return std::any_of(document.objects.begin(), document.objects.end(), [](const SceneObjectDocument& object)
        {
            return object.light && object.light->type == LightType::Directional;
        });
    }

    // Appends one level's entities with namespaced ids. Parents must resolve inside the same document: a parent id that is not one
    // of the level's own entities is an unsupported cross-level reference (architecture §4.2). Document validation already
    // requires parents in the same document; this keeps the composite safe, with a specific message, if that rule ever loosens.
    bool AppendLevelEntities(CompositeWorld& composite, const CompositeWorldSource& source, CompositeWorld::EntityKind kind,
        const std::string& rootID, std::string& error)
    {
        std::unordered_set<std::string> ownIDs;

        for(const auto& object : source.document.objects)
        {
            ownIDs.insert(object.id);
        }

        for(const auto& object : source.document.objects)
        {
            SceneObjectDocument entity = object;
            entity.id = CompositeWorld::RuntimeID(source.document.id, object.id);

            if(!object.parentID.empty())
            {
                if(!ownIDs.contains(object.parentID))
                {
                    return Fail(error, "Entity '" + object.name + "' (" + object.id + ") in " + Describe(source) + " has parent '" +
                        object.parentID + "', which is not an entity of the same level; cross-level parenting is not supported.");
                }

                entity.parentID = CompositeWorld::RuntimeID(source.document.id, object.parentID);
            }
            else
            {
                entity.parentID = rootID;
                // A Local Level root parented to its placement node is still a root of its Local Level (physics root rule).
                entity.parentIsLevelNode = !rootID.empty();
            }

            composite.document.objects.push_back(std::move(entity));
            composite.entities.push_back({kind, source.document.id, object.id});
        }

        return true;
    }

    // B1: the first enabled Directional Light in composite order is the sun; the other enabled ones are disabled in the copy.
    // Main Cameras: the first keeps the designation.
    void ApplyFrameWidePolicies(CompositeWorld& composite)
    {
        for(std::size_t index = 0; index < composite.document.objects.size(); ++index)
        {
            auto& object = composite.document.objects[index];
            const auto& entity = composite.entities[index];
            const CompositeWorld::LightReference reference{entity.levelID, entity.originalID};
            const bool enabledDirectional = object.light && object.light->enabled && object.light->type == LightType::Directional;

            if(enabledDirectional && !composite.sun)
            {
                composite.sun = reference;
            }
            else if(enabledDirectional)
            {
                object.light->enabled = false;
                composite.suppressedDirectionalLights.push_back(reference);
            }

            if(object.camera && object.camera->main)
            {
                const bool firstMain = std::none_of(composite.document.objects.begin(), composite.document.objects.begin() + index,
                    [](const SceneObjectDocument& earlier)
                    {
                        return earlier.camera && earlier.camera->main;
                    });

                if(!firstMain)
                {
                    object.camera->main = false;
                    composite.suppressedMainCameras.push_back(reference);
                }
            }
        }
    }
}

std::string CompositeWorld::RuntimeID(std::string_view levelID, std::string_view originalID)
{
    std::string id;
    id.reserve(levelID.size() + 1 + originalID.size());
    id.append(levelID);
    id.push_back('/');
    id.append(originalID);
    return id;
}

const CompositeWorld::Entity* CompositeWorld::Find(std::string_view runtimeID) const
{
    const auto found = runtimeIndex.find(std::string(runtimeID));
    return found == runtimeIndex.end() ? nullptr : &entities[found->second];
}

bool BuildCompositeWorld(const CompositeWorldSource& world, const std::vector<CompositeWorldSource>& locals, CompositeWorld& composite,
    std::string& error)
{
    // 1. The World and each member, validated on their own and against each other.
    std::string documentError;

    if(world.document.kind != SceneKind::World)
    {
        return Fail(error, Describe(world) + " is not a World.");
    }

    if(!ValidateSceneDocument(world.document, &documentError))
    {
        return Fail(error, "World " + Describe(world) + " is invalid: " + documentError);
    }

    std::unordered_map<std::string, const CompositeWorldSource*> byID;

    for(const auto& local : locals)
    {
        if(!byID.emplace(local.document.id, &local).second)
        {
            return Fail(error, "Local Level " + local.document.id + " was supplied twice.");
        }
    }

    std::unordered_set<std::string> listed;

    for(const auto& member : world.document.members)
    {
        if(!listed.insert(member.level).second)
        {
            return Fail(error, "World " + Describe(world) + " lists Local Level " + member.level + " more than once.");
        }

        const auto found = byID.find(member.level);

        if(found == byID.end())
        {
            return Fail(error, "World " + Describe(world) + " lists Local Level " + member.level + ", which could not be loaded.");
        }

        const auto& local = *found->second;

        if(local.document.kind != SceneKind::Local)
        {
            return Fail(error, Describe(local) + " is listed by World " + Describe(world) + " but is not a Local Level.");
        }

        if(local.document.owningWorld != world.document.id)
        {
            return Fail(error, "Local Level " + Describe(local) + " is listed by World " + Describe(world) + " but names " +
                (local.document.owningWorld.empty() ? std::string("no World") : "World " + local.document.owningWorld) + " as its owner.");
        }

        if(!ValidateSceneDocument(local.document, &documentError))
        {
            return Fail(error, "Local Level " + Describe(local) + " is invalid: " + documentError);
        }
    }

    if(locals.size() != listed.size())
    {
        return Fail(error, "A Local Level that World " + Describe(world) + " does not list was supplied.");
    }

    // 2. Light capacity: the composite is one renderer scene, so every level's Light components share it. The rule is the scene
    // rule (SceneDocument validation): the GPU table holds 64 enabled non-sun Lights and the sun has its own slot, so a scene may
    // hold 64 Light components, or 65 when one of them is a Directional Light. Disabled Lights count; they never reach the table,
    // which is why these bounds can never overflow it.
    std::size_t totalLights = CountLights(world.document);
    bool anyDirectional = HasDirectionalLight(world.document);
    std::string perLevel = "World " + Describe(world) + ": " + std::to_string(totalLights);

    for(const auto& member : world.document.members)
    {
        const auto& local = *byID.at(member.level);
        const std::size_t lights = CountLights(local.document);
        totalLights += lights;
        anyDirectional = anyDirectional || HasDirectionalLight(local.document);
        perLevel += "; " + Describe(local) + ": " + std::to_string(lights);
    }

    const std::size_t capacity = MaximumSceneLights + (anyDirectional ? 1u : 0u);

    if(totalLights > capacity)
    {
        return Fail(error, "World " + Describe(world) + " has " + std::to_string(totalLights) + " Light components across its levels; one " +
            "renderer scene holds at most " + std::to_string(MaximumSceneLights) + " plus one sun slot when a Directional Light exists (" +
            std::to_string(capacity) + " here). Per level: " + perLevel + ". Remove Lights or split the World.");
    }

    // 2b. Object capacity: a composite always has a hierarchy (its level nodes), and a hierarchical scene holds at most
    // SceneHierarchy::Policy::maxObjects objects (the cooked-scene admission limit). Every level's entities plus one node per
    // Local Level share it, so a World can exceed it even when each Local Level alone does not.
    std::size_t totalObjects = world.document.objects.size() + world.document.members.size();
    std::string perLevelObjects = "World " + Describe(world) + ": " + std::to_string(world.document.objects.size());

    for(const auto& member : world.document.members)
    {
        const auto& local = *byID.at(member.level);
        totalObjects += local.document.objects.size();
        perLevelObjects += "; " + Describe(local) + ": " + std::to_string(local.document.objects.size());
    }

    const std::size_t objectCapacity = SceneHierarchy::Policy{}.maxObjects;

    if(totalObjects > objectCapacity)
    {
        return Fail(error, "World " + Describe(world) + " would hold " + std::to_string(totalObjects) + " objects (its entities, its " +
            "Local Levels' entities and one node per Local Level); a scene with a hierarchy holds at most " +
            std::to_string(objectCapacity) + ". Per level: " + perLevelObjects + ". Split the World or reduce its content.");
    }

    // 3. The composite document: World values for every frame-wide setting (A1), World entities, then each member.
    CompositeWorld candidate;
    candidate.worldID = world.document.id;
    candidate.worldName = world.document.name;
    candidate.worldRelativePath = world.relativePath;
    candidate.worldSource = world;
    candidate.worldEnvironment = ResolveSceneEnvironment(world.document, nullptr);
    candidate.lightCount = totalLights;
    candidate.lightCapacity = capacity;

    SceneDocument& document = candidate.document;
    document = world.document;
    document.kind = SceneKind::Local;
    document.members.clear();
    document.owningWorld.clear();
    document.environmentOverrides = SceneEnvironmentOverrides{};
    document.objects.clear();

    if(!AppendLevelEntities(candidate, world, CompositeWorld::EntityKind::World, {}, error))
    {
        return false;
    }

    for(const auto& member : world.document.members)
    {
        const auto& local = *byID.at(member.level);
        SceneObjectDocument root;
        root.id = local.document.id;
        root.name = local.document.name + " (Local Level)";
        root.transform.position = member.placement.position;
        root.transform.rotation = member.placement.rotation;
        document.objects.push_back(std::move(root));
        candidate.entities.push_back({CompositeWorld::EntityKind::LevelRoot, local.document.id, {}});

        if(!AppendLevelEntities(candidate, local, CompositeWorld::EntityKind::Local, local.document.id, error))
        {
            return false;
        }

        CompositeWorld::Level level;
        level.id = local.document.id;
        level.name = local.document.name;
        level.relativePath = local.relativePath;
        level.placement = member.placement;
        level.environment = ResolveSceneEnvironment(local.document, &world.document);
        level.differencesFromWorld = DescribeEnvironmentDifferences(candidate.worldEnvironment, level.environment);
        level.entityCount = local.document.objects.size();
        level.lightCount = CountLights(local.document);
        candidate.levels.push_back(std::move(level));
        candidate.localSources.push_back(local);
    }

    ApplyFrameWidePolicies(candidate);

    for(std::size_t index = 0; index < document.objects.size(); ++index)
    {
        candidate.runtimeIndex.emplace(document.objects[index].id, index);
    }

    // 4. The composite must satisfy every rule of an ordinary scene (ids, hierarchy transform rules, components).
    if(!ValidateSceneDocument(document, &documentError))
    {
        return Fail(error, "World " + Describe(world) + " cannot be composed: " + documentError);
    }

    composite = std::move(candidate);
    return true;
}

namespace
{
    // The level root as built, compared with `object` with the transform ignored (the transform is the editable placement).
    bool IsPristineLevelRoot(const CompositeWorld& composite, const SceneObjectDocument& object)
    {
        const auto built = composite.runtimeIndex.find(object.id);

        if(built == composite.runtimeIndex.end())
        {
            return false;
        }

        SceneDocument expected;
        expected.objects = {composite.document.objects[built->second]};
        SceneDocument actual;
        actual.objects = {object};
        actual.objects[0].transform = expected.objects[0].transform;
        return SceneDocumentsEqual(expected, actual);
    }

    // The source value of a field the composite policy changed, applied to the projected entity (the entity may have been deleted).
    void RestorePolicyFields(SceneDocument& projected, const SceneDocument& source, const std::string& levelID,
        const std::vector<CompositeWorld::LightReference>& suppressedLights, const std::vector<CompositeWorld::LightReference>& suppressedCameras)
    {
        const auto find = [](SceneDocument& document, const std::string& id) -> SceneObjectDocument*
        {
            const auto found = std::ranges::find(document.objects, id, &SceneObjectDocument::id);
            return found == document.objects.end() ? nullptr : &*found;
        };
        const auto findSource = [&source](const std::string& id) -> const SceneObjectDocument*
        {
            const auto found = std::ranges::find(source.objects, id, &SceneObjectDocument::id);
            return found == source.objects.end() ? nullptr : &*found;
        };

        for(const auto& light : suppressedLights)
        {
            auto* object = light.levelID == levelID ? find(projected, light.originalID) : nullptr;
            const auto* original = object ? findSource(light.originalID) : nullptr;

            if(object && original && object->light && original->light)
            {
                object->light->enabled = original->light->enabled;
            }
        }

        for(const auto& camera : suppressedCameras)
        {
            auto* object = camera.levelID == levelID ? find(projected, camera.originalID) : nullptr;
            const auto* original = object ? findSource(camera.originalID) : nullptr;

            if(object && original && object->camera && original->camera)
            {
                object->camera->main = original->camera->main;
            }
        }
    }
}

bool ProjectCompositeDocuments(const CompositeWorld& composite, const SceneDocument& edited, CompositeWorldProjection& projection,
    std::string& error)
{
    // 1. Scene-level fields: the World keeps its identity and membership list (placements are filled from the roots below) and
    // takes the composite's frame-wide settings; each Local Level keeps every scene-level field of its baseline.
    CompositeWorldProjection candidate;
    candidate.world = edited;
    candidate.world.kind = SceneKind::World;
    candidate.world.id = composite.worldSource.document.id;
    candidate.world.name = composite.worldSource.document.name;
    candidate.world.schemaVersion = composite.worldSource.document.schemaVersion;
    candidate.world.members = composite.worldSource.document.members;
    candidate.world.owningWorld.clear();
    candidate.world.environmentOverrides = composite.worldSource.document.environmentOverrides;
    candidate.world.objects.clear();

    std::unordered_map<std::string, SceneDocument*> documents{{composite.worldID, &candidate.world}};
    candidate.locals.reserve(composite.localSources.size());

    for(const auto& source : composite.localSources)
    {
        candidate.locals.push_back(source.document);
        candidate.locals.back().objects.clear();
    }

    for(std::size_t index = 0; index < composite.levels.size(); ++index)
    {
        documents.emplace(composite.levels[index].id, &candidate.locals[index]);
    }

    // 2. Objects, in composite order (which keeps each level's document order): roots become placements, entities return home.
    for(const auto& object : edited.objects)
    {
        if(documents.contains(object.id) && object.id != composite.worldID)
        {
            // A level root is a placement only: anything else attached to it would have no document to be saved in.
            if(!IsPristineLevelRoot(composite, object))
            {
                error = "Local Level node '" + object.name + "' carries entity data (a component, mesh, parent or new name); a Local "
                    "Level node holds only its placement.";
                return false;
            }

            const auto& scale = object.transform.scale;

            if(scale.x != 1.0f || scale.y != 1.0f || scale.z != 1.0f)
            {
                error = "Local Level '" + object.name + "' has a scaled placement; Local Level placement has no scale.";
                return false;
            }

            auto member = std::ranges::find(candidate.world.members, object.id, &SceneMembership::level);

            if(member == candidate.world.members.end())
            {
                error = "Composite object " + object.id + " is a level root the World does not list.";
                return false;
            }

            member->placement.position = object.transform.position;
            member->placement.rotation = object.transform.rotation;
            continue;
        }

        const auto separator = object.id.find('/');
        const auto owner = separator == std::string::npos ? documents.end() : documents.find(object.id.substr(0, separator));

        if(owner == documents.end() || separator + 1 >= object.id.size())
        {
            error = "Composite object " + object.id + " belongs to no level of this World and cannot be saved.";
            return false;
        }

        const std::string levelID = owner->first;
        SceneObjectDocument entity = object;
        entity.id = object.id.substr(separator + 1);
        entity.parentIsLevelNode = false;

        if(object.parentID == levelID || object.parentID.empty())
        {
            entity.parentID.clear();
        }
        else if(object.parentID.starts_with(levelID + "/"))
        {
            entity.parentID = object.parentID.substr(levelID.size() + 1);
        }
        else
        {
            error = "Entity '" + object.name + "' has a parent in another level; cross-level parenting is not supported.";
            return false;
        }

        owner->second->objects.push_back(std::move(entity));
    }

    // 3. Fields the composite policy changed revert to their source values.
    RestorePolicyFields(candidate.world, composite.worldSource.document, composite.worldID, composite.suppressedDirectionalLights,
        composite.suppressedMainCameras);

    for(std::size_t index = 0; index < composite.levels.size(); ++index)
    {
        RestorePolicyFields(candidate.locals[index], composite.localSources[index].document, composite.levels[index].id,
            composite.suppressedDirectionalLights, composite.suppressedMainCameras);
    }

    projection = std::move(candidate);
    return true;
}

std::vector<bool> DirtyCompositeDocuments(const CompositeWorld& composite, const CompositeWorldProjection& projection)
{
    std::vector<bool> dirty;
    dirty.reserve(1 + projection.locals.size());
    dirty.push_back(!SceneDocumentsEqual(projection.world, composite.worldSource.document));

    for(std::size_t index = 0; index < projection.locals.size(); ++index)
    {
        dirty.push_back(!SceneDocumentsEqual(projection.locals[index], composite.localSources[index].document));
    }

    return dirty;
}

bool ValidateCompositeEdit(const CompositeWorld& composite, const SceneDocument& candidate, std::string& error)
{
    // Ownership: the candidate must still split into the World and its Local Levels.
    CompositeWorldProjection projection;

    if(!ProjectCompositeDocuments(composite, candidate, projection, error))
    {
        return false;
    }

    std::unordered_set<std::string> present;

    for(const auto& object : candidate.objects)
    {
        present.insert(object.id);

        // A Local Level's entity always hangs below its level node (directly or through its own parents): without it the entity
        // would lose its placement in the World.
        const bool localEntity = object.id.find('/') != std::string::npos && !object.id.starts_with(composite.worldID + "/");

        // The level-node marker (physics root rule) is exactly "my parent is a Local Level node"; a reparented entity loses it.
        const bool parentIsNode = std::ranges::any_of(composite.levels, [&](const CompositeWorld::Level& level) { return level.id == object.parentID; });

        if(object.parentIsLevelNode != parentIsNode)
        {
            error = "Entity '" + object.name + "' has an inconsistent Local Level root marker.";
            return false;
        }

        if(localEntity && object.parentID.empty())
        {
            error = "Entity '" + object.name + "' belongs to a Local Level and must stay inside it; move it within its Local Level.";
            return false;
        }
    }

    for(const auto& level : composite.levels)
    {
        if(!present.contains(level.id))
        {
            error = "Local Level node '" + level.name + "' cannot be deleted; remove the Local Level from the World instead.";
            return false;
        }
    }

    // Composite-policy fields stay as the policy set them (they are restored from their documents on save).
    const auto find = [&](const CompositeWorld::LightReference& reference) -> const SceneObjectDocument*
    {
        const auto found = std::ranges::find(candidate.objects, CompositeWorld::RuntimeID(reference.levelID, reference.originalID),
            &SceneObjectDocument::id);
        return found == candidate.objects.end() ? nullptr : &*found;
    };

    for(const auto& light : composite.suppressedDirectionalLights)
    {
        const auto* object = find(light);

        if(object && object->light && object->light->enabled)
        {
            error = "Directional Light '" + object->name + "' cannot be enabled in the World preview: the World has one sun. Edit it in its "
                "Local Level.";
            return false;
        }
    }

    for(const auto& camera : composite.suppressedMainCameras)
    {
        const auto* object = find(camera);

        if(object && object->camera && object->camera->main)
        {
            error = "Camera '" + object->name + "' cannot be the Main Camera in the World preview (one per scene). Edit it in its Local Level.";
            return false;
        }
    }

    return true;
}
