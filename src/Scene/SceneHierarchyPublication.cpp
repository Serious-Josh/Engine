#include "Scene/SceneDocumentAdapter.h"
#include "Scene/FileSceneRuntime.h"
#include "Scene/SceneHierarchyEdits.h"

#include <algorithm>
#include <string_view>
#include <unordered_map>

namespace
{
    bool Fail(std::string* errorMessage, const char* message)
    {
        if(errorMessage)
        {
            *errorMessage = message;
        }

        return false;
    }

    // The validated forest must describe exactly the current document IDs and parents.
    bool ForestMatchesObjects(const SceneHierarchy::Forest& forest, std::span<const SceneObjectDocument> objects)
    {
        if(forest.objectIDs.size() != objects.size())
        {
            return false;
        }

        for(size_t index = 0; index < objects.size(); ++index)
        {
            if(forest.objectIDs[index] != objects[index].id || forest.parentIDs[index] != objects[index].parentID)
            {
                return false;
            }
        }

        return true;
    }

    bool HasEdges(const SceneHierarchy::Forest& forest)
    {
        for(const auto& parent : forest.parents)
        {
            if(parent)
            {
                return true;
            }
        }

        return false;
    }

    // Converts document indices to runtime slots through persistent IDs; document position is
    // never assumed to equal the render-object index. `slotOf` returns count for an unknown ID.
    template<typename SlotLookup>
    bool MapForestToSlots(const SceneHierarchy::Forest& forest, std::uint64_t revision, size_t count,
        SlotLookup slotOf, SceneHierarchyTopology& topology, std::string* errorMessage)
    {
        if(forest.objectIDs.size() != count)
        {
            return Fail(errorMessage, "Hierarchy object count does not match the runtime slots.");
        }

        std::vector<RenderObjectID> slots(count);
        std::vector<bool> visited(count, false);

        for(size_t index = 0; index < count; ++index)
        {
            const size_t slot = slotOf(forest.objectIDs[index]);

            if(slot >= count || visited[slot])
            {
                return Fail(errorMessage, "Hierarchy object IDs do not map one-to-one onto runtime slots.");
            }

            visited[slot] = true;
            slots[index] = slot;
        }

        SceneHierarchyTopology candidate;
        candidate.parentSlots.resize(count);
        candidate.parentBeforeChild.reserve(count);
        candidate.hierarchyRevision = revision;

        for(size_t index = 0; index < count; ++index)
        {
            if(forest.parents[index])
            {
                candidate.parentSlots[slots[index]] = slots[*forest.parents[index]];
            }
        }

        for(const size_t index : forest.parentBeforeChild)
        {
            candidate.parentBeforeChild.push_back(slots[index]);
        }

        topology = std::move(candidate);
        return true;
    }
}

bool FileSceneRuntime::PrepareCommitHierarchy(SceneHierarchy::State& state,
    std::span<const SceneObjectDocument> objects, std::span<const std::string_view> slotObjectIDs,
    SceneHierarchyTopology& topology, std::string* errorMessage)
{
    if(!state.Update(objects, errorMessage))
    {
        return false;
    }

    // A flat forest installs no topology, so flat scenes keep the original presentation path.
    if(!HasEdges(state.GetForest()))
    {
        topology = {};
        return true;
    }

    std::unordered_map<std::string_view, size_t> slots;
    slots.reserve(slotObjectIDs.size());

    for(size_t slot = 0; slot < slotObjectIDs.size(); ++slot)
    {
        if(!slots.emplace(slotObjectIDs[slot], slot).second)
        {
            return Fail(errorMessage, "Prepared runtime slots contain a duplicate object ID.");
        }
    }

    const size_t count = slotObjectIDs.size();
    const auto slotOf = [&](const std::string& id)
    {
        const auto found = slots.find(id);
        return found == slots.end() ? count : found->second;
    };

    return MapForestToSlots(state.GetForest(), state.GetRevision(), count, slotOf, topology, errorMessage);
}

bool FileSceneRuntime::BuildHierarchyTopology(const SceneHierarchy::State& state,
    std::span<const SceneObjectDocument> objects, SceneHierarchyTopology& topology, std::string* errorMessage) const
{
    const auto& forest = state.GetForest();
    const auto& mapping = m_objectMapping;
    const size_t count = m_scene.GetRenderObjects().size();

    if(state.GetRevision() == 0 || !ForestMatchesObjects(forest, objects))
    {
        return Fail(errorMessage, "Hierarchy state does not describe the current scene document.");
    }

    if(mapping.scene != &m_scene || mapping.structureRevision != m_scene.GetStructureRevision() ||
        mapping.idToRuntimeIndex.size() != count || objects.size() != count)
    {
        return Fail(errorMessage, "Hierarchy runtime mapping is stale.");
    }

    if(!HasEdges(forest))
    {
        topology = {};
        topology.parentSlots.resize(count);

        for(RenderObjectID slot = 0; slot < count; ++slot)
        {
            topology.parentBeforeChild.push_back(slot);
        }

        topology.hierarchyRevision = state.GetRevision();
        return true;
    }

    const auto slotOf = [&](const std::string& id)
    {
        const auto found = mapping.idToRuntimeIndex.find(id);
        return found == mapping.idToRuntimeIndex.end() ? count : found->second;
    };

    return MapForestToSlots(forest, state.GetRevision(), count, slotOf, topology, errorMessage);
}

bool FileSceneRuntime::ValidateHierarchyAssociation(std::string* errorMessage) const
{
    // Nothing is installed before the first commit; there is no graph to be stale against.
    if(!m_objectMapping.scene)
    {
        return true;
    }

    if(!m_fileScene.HasDocument())
    {
        return Fail(errorMessage, "No scene document is loaded.");
    }

    // With no validated forest since the last replacement reset (direct commit fixtures), only a
    // flat document and a flat scene are consistent; any parent then came from a raw edit.
    const auto& objects = m_fileScene.GetDocument().objects;

    if(m_hierarchy.GetRevision() == 0 || m_hierarchy.GetForest().objectIDs.empty())
    {
        const bool flatDocument = std::all_of(objects.begin(), objects.end(), [](const SceneObjectDocument& object)
        {
            return object.parentID.empty();
        });

        if(!flatDocument || m_scene.HasHierarchy())
        {
            return Fail(errorMessage, "Scene document hierarchy changed outside a validated hierarchy commit.");
        }

        return true;
    }

    if(!ForestMatchesObjects(m_hierarchy.GetForest(), objects))
    {
        return Fail(errorMessage, "Scene document hierarchy changed outside a validated hierarchy commit.");
    }

    // A flat validated forest must have no installed topology; a nonflat one must match its revision.
    const bool expectsTopology = HasEdges(m_hierarchy.GetForest());

    if(expectsTopology != m_scene.HasHierarchy() ||
        (expectsTopology && m_scene.GetHierarchyRevision() != m_hierarchy.GetRevision()))
    {
        return Fail(errorMessage, "Installed hierarchy topology belongs to a stale hierarchy revision.");
    }

    return true;
}

#if defined(ENGINE_HAS_VERIFICATION)
bool FileSceneRuntime::CommitHierarchyForVerification(std::span<const std::string> parentIDs, std::string* errorMessage)
{
    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(m_archiveScene || HasPreparedScene() || m_materialAssignment || m_activePresentationAssociation ||
        !m_fileScene.HasDocument())
    {
        return Fail(errorMessage, "Hierarchy verification commits require an idle editable scene.");
    }

    // Build the complete candidate before touching the document, state or Scene.
    SceneDocument candidate = m_fileScene.GetDocument();

    if(parentIDs.size() != candidate.objects.size())
    {
        return Fail(errorMessage, "Hierarchy verification needs one parent ID per object.");
    }

    for(size_t index = 0; index < parentIDs.size(); ++index)
    {
        candidate.objects[index].parentID = parentIDs[index];
    }

    if(!ValidateSceneDocument(candidate, errorMessage))
    {
        return false;
    }

    auto state = m_hierarchy;

    if(!state.Update(candidate.objects, errorMessage))
    {
        return false;
    }

    SceneHierarchyTopology topology;

    if(!BuildHierarchyTopology(state, candidate.objects, topology, errorMessage))
    {
        return false;
    }

    // Last fallible step: evaluates every world, then commits matrices and dirty slots.
    if(!m_scene.InstallHierarchy(std::move(topology), false, errorMessage))
    {
        return false;
    }

    m_fileScene.EditDocument().objects.swap(candidate.objects);
    m_hierarchy = std::move(state);
    return true;
}
#endif

bool FileSceneRuntime::CommitHierarchyEdit(const SceneDocument& expected, SceneDocument replacement,
    std::string* errorMessage)
{
    if(m_archiveScene || HasPreparedScene() || m_materialAssignment || m_activePresentationAssociation ||
        m_scene.HasPhysicsPresentation() || !ValidateHierarchyAssociation(errorMessage))
    {
        return Fail(errorMessage, "Hierarchy edit requires an idle authored scene.");
    }

    SceneDocument current;

    if(!CaptureSceneDocument(m_fileScene.GetDocument(), m_scene, m_objectMapping, current, errorMessage) ||
        expected.id != current.id || replacement.id != current.id ||
        !SceneHierarchy::SameObjects(expected, current) || !ValidateSceneDocument(replacement, errorMessage))
    {
        return Fail(errorMessage, "Hierarchy expected state changed or candidate is invalid.");
    }

    // Reject component/asset/name/membership changes on this renderer-independent path.
    auto topologyOnly = current;
    topologyOnly.objects.clear();
    std::vector<RenderObjectTransformUpdate> updates;

    for(const auto& object : replacement.objects)
    {
        const auto found = std::find_if(current.objects.begin(), current.objects.end(),
            [&](const auto& entry) { return entry.id == object.id; });

        if(found == current.objects.end())
        {
            return Fail(errorMessage, "Membership edits require renderer publication.");
        }

        auto allowed = *found;
        allowed.parentID = object.parentID;
        allowed.transform = object.transform;
        topologyOnly.objects.push_back(std::move(allowed));
        updates.push_back({m_objectMapping.idToRuntimeIndex.at(object.id), object.transform});
    }

    if(current.objects.size() != replacement.objects.size() || !SceneHierarchy::SameObjects(topologyOnly, replacement))
    {
        return Fail(errorMessage, "Hierarchy-only commit cannot change components or asset bindings.");
    }

    auto state = m_hierarchy;
    SceneHierarchyTopology topology;

    if(!state.Update(replacement.objects, errorMessage) ||
        !BuildHierarchyTopology(state, replacement.objects, topology, errorMessage) ||
        !m_scene.InstallHierarchy(std::move(topology), false, errorMessage, updates))
    {
        return false;
    }

    m_fileScene.EditDocument().objects.swap(replacement.objects);
    m_hierarchy = std::move(state);
    return true;
}
