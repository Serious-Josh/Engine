#include "assets/CookedScene.h"
#include "Scene/FileSceneRuntime.h"

#include "Scene/SceneAssetResolver.h"
#include "Scene/FileSceneAssetCatalog.h"
#include "Scene/SceneDocumentAdapter.h"

#include <utility>
#include <atomic>
#include <type_traits>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <limits>
#include "assets/AssetID.h"
#include "Renderer/Mesh.h"

namespace
{
    std::atomic<std::uint64_t> nextRuntimeIdentity{1};
}

FileSceneRuntime::FileSceneRuntime()
    : m_instanceIdentity(nextRuntimeIdentity.fetch_add(1, std::memory_order_relaxed))
{
}

bool NormalizeSceneObjectName(std::string& name, std::string* errorMessage)
{
    const auto whitespace = [](unsigned char character) { return std::isspace(character) != 0; };
    const auto first = std::find_if_not(name.begin(), name.end(), whitespace);
    const auto last = std::find_if_not(name.rbegin(), name.rend(), whitespace).base();

    if(first >= last)
    {
        if(errorMessage)
        {
            *errorMessage = "Object name cannot be empty or whitespace-only.";
        }

        return false;
    }

    name = std::string(first, last);
    return true;
}

bool FileSceneRuntime::ReorderObjects(const SceneObjectOrderIdentity& identity,
    const std::vector<std::string>& expectedOrder, const std::vector<std::string>& replacementOrder,
    std::string* errorMessage)
{
    const auto fail = [errorMessage](const char* message)
    {
        if(errorMessage)
        {
            *errorMessage = message;
        }

        return false;
    };

    if(HasPreparedScene() || m_materialAssignment)
    {
        return fail("Reordering is blocked by pending scene preparation or material assignment.");
    }

    SceneHierarchySnapshot snapshot;

    if(!BuildHierarchySnapshot(snapshot, errorMessage))
    {
        return false;
    }

    if(identity.sceneID != m_fileScene.GetDocument().id || identity.runtimeIdentity != m_instanceIdentity ||
        identity.structureRevision != m_scene.GetStructureRevision())
    {
        return fail("Hierarchy reorder belongs to a stale scene runtime.");
    }

    if(expectedOrder.size() != snapshot.entries.size() || replacementOrder.size() != expectedOrder.size())
    {
        return fail("Hierarchy reorder object count changed; scene state was preserved.");
    }

    std::unordered_map<std::string, size_t> indices;
    indices.reserve(expectedOrder.size());

    for(size_t index = 0; index < expectedOrder.size(); ++index)
    {
        if(expectedOrder[index] != snapshot.entries[index].objectID)
        {
            return fail("Hierarchy order changed outside this command; scene state was preserved.");
        }

        indices.emplace(expectedOrder[index], index);
    }

    const auto& objects = m_fileScene.GetDocument().objects;
    std::vector<SceneObjectDocument> reordered;
    reordered.reserve(objects.size());
    std::vector<bool> visited(objects.size(), false);

    for(const auto& id : replacementOrder)
    {
        const auto found = indices.find(id);

        if(found == indices.end() || visited[found->second])
        {
            return fail("Hierarchy reorder must contain each persistent object ID exactly once.");
        }

        visited[found->second] = true;
        reordered.push_back(objects[found->second]);
    }

    if(expectedOrder == replacementOrder)
    {
        return true;
    }

    // Only document order changes. Runtime objects, mappings, and revision stay intact.
    m_fileScene.EditDocument().objects.swap(reordered);
    return true;
}

bool FileSceneRuntime::SetObjectPhysics(const SceneObjectMetadata& selection,
    const std::optional<Physics::PhysicsComponent>& expected,
    const std::optional<Physics::PhysicsComponent>& replacement, std::string* errorMessage)
{
    const auto fail = [&](const char* message)
    {
        if(errorMessage) *errorMessage = message;
        return false;
    };
    if(errorMessage) errorMessage->clear();
    if(m_archiveScene || HasPreparedScene() || m_materialAssignment || m_activePresentationAssociation)
        return fail("Physics authoring requires an idle editable scene.");
    if(replacement && !Physics::ValidatePhysicsComponent(*replacement, errorMessage)) return false;
    const auto current = GetObjectMetadata(selection.objectID, errorMessage);
    if(!current) return false;
    if(current->sceneID != selection.sceneID || current->runtimeIdentity != selection.runtimeIdentity ||
        current->structureRevision != selection.structureRevision || current->runtimeIndex != selection.runtimeIndex)
        return fail("Physics target belongs to a stale scene selection.");
    const auto& objects = m_fileScene.GetDocument().objects;
    const auto found = std::find_if(objects.begin(), objects.end(),
        [&](const SceneObjectDocument& object) { return object.id == selection.objectID; });
    if(found == objects.end() || found->physics != expected)
        return fail("Physics component changed since inspection; edit rejected.");
    if(expected == replacement) return true;
    const size_t index = static_cast<size_t>(found - objects.begin());
    // Validation precedes the dirty-marking commit. No runtime or presentation fields are touched.
    m_fileScene.EditDocument().objects[index].physics = replacement;
    return true;
}

bool FileSceneRuntime::RenameObject(const SceneObjectMetadata& selection, std::string_view expectedName,
    std::string name, std::string* errorMessage, SceneObjectNamePolicy policy)
{
    const auto fail = [errorMessage](const char* message)
    {
        if(errorMessage)
        {
            *errorMessage = message;
        }

        return false;
    };

    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(HasPreparedScene() || m_materialAssignment)
    {
        return fail("Finish or cancel pending scene preparation and material assignment before renaming.");
    }

    if(policy == SceneObjectNamePolicy::TrimmedInput && !NormalizeSceneObjectName(name, errorMessage))
    {
        return false;
    }

    const auto current = GetObjectMetadata(selection.objectID, errorMessage);

    if(!current)
    {
        return false;
    }

    if(current->sceneID != selection.sceneID || current->runtimeIdentity != selection.runtimeIdentity ||
        current->structureRevision != selection.structureRevision || current->runtimeIndex != selection.runtimeIndex ||
        current->name != expectedName)
    {
        return fail("Rename target or expected name is stale; scene state was preserved.");
    }

    if(current->name == name)
    {
        return true;
    }

    const auto& objects = m_fileScene.GetDocument().objects;
    const auto found = std::find_if(objects.begin(), objects.end(),
        [&](const SceneObjectDocument& object) { return object.id == selection.objectID; });

    if(found == objects.end())
    {
        return fail("Rename target is absent from the scene document.");
    }

    const size_t index = static_cast<size_t>(found - objects.begin());

    // All validation and string allocation precede the dirty-marking commit point.
    m_fileScene.EditDocument().objects[index].name.swap(name);
    return true;
}

bool FileSceneRuntime::BuildHierarchySnapshot(
    SceneHierarchySnapshot& snapshot, std::string* errorMessage) const
{
    const auto fail = [errorMessage](const char* message)
    {
        if(errorMessage)
        {
            *errorMessage = message;
        }

        return false;
    };

    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(!m_fileScene.HasDocument())
    {
        return fail("No scene document is loaded.");
    }

    const auto& document = m_fileScene.GetDocument();
    if(m_archiveScene && !SceneDocumentsEqual(document,m_archiveScene->metadata.document))
        return fail("Immutable packaged scene metadata changed; reload the archive scene.");
    const auto& objects = m_scene.GetRenderObjects();
    const auto& mapping = m_objectMapping;

    if(!ValidateSceneDocument(document, errorMessage))
    {
        return false;
    }

    if(mapping.scene != &m_scene || mapping.structureRevision != m_scene.GetStructureRevision() ||
        mapping.committedObjects.size() != objects.size() || mapping.idToRuntimeIndex.size() != objects.size() ||
        mapping.bindingRevisions.size() != objects.size() ||
        document.objects.size() != objects.size())
    {
        return fail("Scene hierarchy mapping is stale.");
    }

    SceneHierarchySnapshot candidate;
    candidate.sceneName = document.name;
    candidate.entries.reserve(document.objects.size());
    std::vector<bool> visited(objects.size(), false);

    for(const auto& object : document.objects)
    {
        const auto found = mapping.idToRuntimeIndex.find(object.id);

        if(found == mapping.idToRuntimeIndex.end() || found->second >= objects.size() || visited[found->second])
        {
            return fail("Scene hierarchy object mapping is invalid.");
        }

        const auto index = found->second;
        const auto& runtime = objects[index];
        const auto& committed = mapping.committedObjects[index];

        if(!MatchesCommittedSceneObjectBindings(object, runtime, committed))
        {
            return fail("Scene hierarchy asset bindings are stale.");
        }

        visited[index] = true;
        candidate.entries.push_back({object.id, object.name, index});
    }

    snapshot = std::move(candidate);
    return true;
}

std::optional<SceneObjectMetadata> FileSceneRuntime::GetObjectMetadata(
    RenderObjectID runtimeIndex,
    std::string* errorMessage) const
{
    if(errorMessage)
    {
        errorMessage->clear();
    }

    const auto fail = [errorMessage](const char* message) -> std::optional<SceneObjectMetadata>
    {
        if(errorMessage)
        {
            *errorMessage = message;
        }

        return std::nullopt;
    };

    if(!m_fileScene.HasDocument())
    {
        return fail("No file-backed scene document is loaded.");
    }

    const auto& document = m_fileScene.GetDocument();
    if(m_archiveScene && !SceneDocumentsEqual(document,m_archiveScene->metadata.document))
        return fail("Immutable packaged scene metadata changed; reload the archive scene.");
    const auto& objects = m_scene.GetRenderObjects();
    const auto& mapping = m_objectMapping;

    if(mapping.scene != &m_scene || mapping.structureRevision != m_scene.GetStructureRevision())
    {
        return fail("Scene object metadata mapping is stale or belongs to another scene.");
    }

    if(runtimeIndex >= objects.size())
    {
        return fail("Selected runtime object index is out of range.");
    }

    if(document.objects.size() != objects.size() || mapping.idToRuntimeIndex.size() != objects.size() ||
       mapping.committedObjects.size() != objects.size() || mapping.bindingRevisions.size() != objects.size())
    {
        return fail("Scene object metadata count does not match the committed mapping.");
    }

    if(!ValidateSceneDocument(document, errorMessage))
    {
        return std::nullopt;
    }

    std::vector<bool> visited(objects.size(), false);
    const SceneObjectDocument* selected = nullptr;

    for(const auto& object : document.objects)
    {
        const auto found = mapping.idToRuntimeIndex.find(object.id);

        if(found == mapping.idToRuntimeIndex.end() || found->second >= objects.size() || visited[found->second])
        {
            return fail("Scene document object has an invalid or ambiguous runtime mapping.");
        }

        const auto index = found->second;
        visited[index] = true;

        if(!MatchesCommittedSceneObjectBindings(object, objects[index], mapping.committedObjects[index]))
        {
            return fail("Runtime asset bindings no longer match the committed scene objects.");
        }

        if(index == runtimeIndex)
        {
            selected = &object;
        }
    }

    if(!selected)
    {
        return fail("Selected runtime object has no document metadata.");
    }

    return SceneObjectMetadata{selected->name, selected->meshReference, selected->materialReference,
        selected->id, document.id, runtimeIndex, m_instanceIdentity, mapping.structureRevision,
        selected->materialSlotBindings, mapping.bindingRevisions[runtimeIndex]};
}

namespace
{
    bool AssignmentError(std::string* error, std::string_view message)
    {
        if(error)
        {
            *error = message;
        }

        return false;
    }

    bool SameFileState(const FileScene& left, const FileScene& right)
    {
        return left.HasDocument() == right.HasDocument() && left.IsDirty() == right.IsDirty() &&
            left.GetPath() == right.GetPath() && SceneDocumentsEqual(left.GetDocument(), right.GetDocument());
    }

    void ApplyFallbackMaterial(RenderObject& object, Material* material,
        const std::map<std::string, std::string>& explicitBindings) noexcept
    {
        object.material = material;

        for(auto& [slotID, effectiveMaterial] : object.slotMaterials)
        {
            if(!explicitBindings.contains(slotID))
            {
                effectiveMaterial = material;
            }
        }
    }

    bool MatchesSlotAssignment(const RenderObject& object, Mesh* mesh, Material* fallback,
        const std::map<std::string, Material*>& expectedSlots, const std::string& slotID)
    {
        if (object.mesh != mesh || object.material != fallback || object.slotMaterials != expectedSlots)
        {
            return false;
        }

        if (!mesh)
        {
            return true; // Pending slots are separately validated against catalog evidence.
        }

        const auto& slots = mesh->GetMetadata().materialSlots;
        return std::any_of(slots.begin(), slots.end(), [&](const auto& slot) { return slot.id == slotID; });
    }

}

std::optional<SceneObjectMetadata> FileSceneRuntime::GetObjectMetadata(
    std::string_view objectID, std::string* errorMessage) const
{
    const auto found = m_objectMapping.idToRuntimeIndex.find(std::string(objectID));

    if(found == m_objectMapping.idToRuntimeIndex.end())
    {
        AssignmentError(errorMessage, "Scene object persistent ID is not present in the current runtime mapping.");
        return std::nullopt;
    }

    return GetObjectMetadata(found->second, errorMessage);
}

std::optional<MaterialAssignmentToken> FileSceneRuntime::PrepareMaterialAssignment(
    const SceneObjectMetadata& selection, std::string materialReference, std::string* errorMessage)
{
    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(m_materialAssignment || HasPreparedScene() || m_nextMaterialAssignment == 0)
    {
        AssignmentError(errorMessage, "Another scene preparation/assignment is pending, or transaction identities exhausted.");
        return std::nullopt;
    }

    const auto metadata = GetObjectMetadata(selection.runtimeIndex, errorMessage);

    if(!metadata || metadata->sceneID != selection.sceneID || metadata->objectID != selection.objectID ||
       metadata->runtimeIdentity != selection.runtimeIdentity || metadata->structureRevision != selection.structureRevision ||
       metadata->bindingRevision != selection.bindingRevision || metadata->meshReference != selection.meshReference ||
       metadata->materialReference != selection.materialReference || metadata->materialSlotBindings != selection.materialSlotBindings)
    {
        AssignmentError(errorMessage, "Material assignment selection is stale or belongs to another scene object.");
        return std::nullopt;
    }

    if(materialReference != "builtin:default" && !AssetID::ParseReference(materialReference))
    {
        AssignmentError(errorMessage, "Material assignment requires builtin:default or a canonical asset reference.");
        return std::nullopt;
    }

    MaterialAssignment prepared;
    prepared.selection = *metadata;
    prepared.previousMaterial = m_scene.GetRenderObjects()[selection.runtimeIndex].material;
    prepared.original = m_fileScene;
    prepared.candidate = m_fileScene;

    if(!CaptureSceneDocument(m_fileScene.GetDocument(), m_scene, m_objectMapping, prepared.liveBefore, errorMessage))
    {
        return std::nullopt;
    }

    const bool noOp = metadata->materialReference == materialReference ||
        (metadata->materialReference.empty() && materialReference == "builtin:default");

    for(const auto& object : prepared.liveBefore.objects)
    {
        if(object.id == metadata->objectID)
        {
            prepared.slotBindings = object.materialSlotBindings;
            break;
        }
    }

    if(!noOp)
    {
        SceneDocument candidate = prepared.liveBefore;

        for(auto& object : candidate.objects)
        {
            if(object.id == metadata->objectID)
            {
                object.materialReference = materialReference;
            }
        }

        if(!ValidateSceneDocument(candidate, errorMessage))
        {
            return std::nullopt;
        }

        prepared.candidate.EditDocument() = std::move(candidate);
    }

    prepared.token = {m_instanceIdentity, m_nextMaterialAssignment++, noOp};
    m_materialAssignment = std::move(prepared);
    return m_materialAssignment->token;
}

std::optional<MaterialAssignmentToken> FileSceneRuntime::PrepareMaterialSlotAssignment(
    const SceneObjectMetadata& selection, std::string slotID, const std::optional<AssetID>& materialID,
    Material& effectiveMaterial, std::string* errorMessage, FileSceneAssetCatalog* catalog)
{
    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(m_materialAssignment || HasPreparedScene() || m_nextMaterialAssignment == 0)
    {
        AssignmentError(errorMessage, "Another scene preparation/assignment is pending, or transaction identities exhausted.");
        return std::nullopt;
    }

    const auto metadata = GetObjectMetadata(selection.objectID, errorMessage);

    if(!metadata || metadata->runtimeIndex != selection.runtimeIndex || metadata->sceneID != selection.sceneID ||
        metadata->runtimeIdentity != selection.runtimeIdentity || metadata->structureRevision != selection.structureRevision ||
        metadata->meshReference != selection.meshReference || metadata->materialReference != selection.materialReference ||
        metadata->materialSlotBindings != selection.materialSlotBindings || metadata->bindingRevision != selection.bindingRevision)
    {
        AssignmentError(errorMessage, "Material slot assignment object, mesh, or bindings are stale.");
        return std::nullopt;
    }

    const auto& object = m_scene.GetRenderObjects()[metadata->runtimeIndex];
    std::string validation;

    std::optional<MeshResidencyCandidate> pendingSource;

    if (!object.mesh)
    {
        Mesh* retained = catalog ? catalog->FindRetainedMesh(metadata->meshReference) : nullptr;

        if (!retained)
        {
            AssignmentError(errorMessage, "Material slots await verified mesh metadata; authored overrides are retained.");
            return std::nullopt;
        }

        pendingSource = catalog->DescribeMeshResidencyCandidate(metadata->meshReference, *retained, errorMessage);

        if (!pendingSource)
        {
            return std::nullopt;
        }
    }

    const Mesh* slotMesh = object.mesh ? object.mesh : pendingSource->mesh;

    if (!object.material || !ValidateMeshMetadata(slotMesh->GetMetadata(), slotMesh->GetIndexCount(), validation))
    {
        AssignmentError(errorMessage, "Material slot assignment requires verified mesh-slot metadata. " + validation);
        return std::nullopt;
    }

    const auto& slots = slotMesh->GetMetadata().materialSlots;
    const auto slotExists = [&](const std::string& id)
    {
        return std::any_of(slots.begin(), slots.end(), [&](const auto& slot) { return slot.id == id; });
    };

    if(!slotExists(slotID) || (object.mesh && object.slotMaterials.size() != slots.size()) ||
        (!object.mesh && object.slotMaterials.size() != metadata->materialSlotBindings.size()))
    {
        AssignmentError(errorMessage, "Unknown mesh slot or incomplete effective runtime material mapping: " + slotID);
        return std::nullopt;
    }

    for(const auto& slot : slots)
    {
        if (!object.mesh && !metadata->materialSlotBindings.contains(slot.id))
        {
            continue;
        }

        const auto binding = object.slotMaterials.find(slot.id);

        if(binding == object.slotMaterials.end() || !binding->second)
        {
            AssignmentError(errorMessage, "A mesh slot has no resolved runtime material: " + slot.id);
            return std::nullopt;
        }
    }

    for(const auto& [id, reference] : metadata->materialSlotBindings)
    {
        if(!slotExists(id) || !AssetID::ParseReference(reference))
        {
            AssignmentError(errorMessage, "Existing material-slot bindings contain an unknown slot or invalid material reference.");
            return std::nullopt;
        }
    }

    if(!materialID && &effectiveMaterial != object.material)
    {
        AssignmentError(errorMessage, "Clearing a slot override must restore the object's existing fallback material.");
        return std::nullopt;
    }

    const auto previousBinding = metadata->materialSlotBindings.find(slotID);
    const bool noOp = materialID ? previousBinding != metadata->materialSlotBindings.end() &&
        previousBinding->second == materialID->ToReference() : previousBinding == metadata->materialSlotBindings.end();

    if(noOp && &effectiveMaterial != object.GetSlotMaterial(slotID))
    {
        AssignmentError(errorMessage, "An unchanged slot binding must retain its existing runtime material.");
        return std::nullopt;
    }

    MaterialAssignment prepared;
    prepared.selection = *metadata;
    prepared.original = m_fileScene;
    prepared.candidate = m_fileScene;

    prepared.slotID = std::move(slotID);
    prepared.expectedMesh = object.mesh;
    prepared.previousMaterial = object.material;
    prepared.stagedMaterial = &effectiveMaterial;

    prepared.originalSlots = object.slotMaterials;
    prepared.stagedRuntimeSlots = object.slotMaterials;
    if (object.mesh || materialID)
    {
        prepared.stagedRuntimeSlots.insert_or_assign(*prepared.slotID, &effectiveMaterial);
    }
    else
    {
        prepared.stagedRuntimeSlots.erase(*prepared.slotID);
    }

    prepared.stagedCommittedSlots = prepared.stagedRuntimeSlots;
    prepared.expectedAppliedSlots = prepared.stagedRuntimeSlots;
    prepared.pendingSlotSource = std::move(pendingSource);
    prepared.pendingSlotCatalog = catalog;

    if(!CaptureSceneDocument(m_fileScene.GetDocument(), m_scene, m_objectMapping, prepared.liveBefore, errorMessage))
    {
        return std::nullopt;
    }

    if(!noOp)
    {
        SceneDocument candidate = prepared.liveBefore;
        const auto target = std::find_if(candidate.objects.begin(), candidate.objects.end(),
            [&](const auto& entry) { return entry.id == metadata->objectID; });

        if(target == candidate.objects.end())
        {
            AssignmentError(errorMessage, "Persistent object ID disappeared during material-slot preparation.");
            return std::nullopt;
        }

        if(materialID)
        {
            target->materialSlotBindings[*prepared.slotID] = materialID->ToReference();
        }
        else
        {
            target->materialSlotBindings.erase(*prepared.slotID);
        }

        if(!ValidateSceneDocument(candidate, errorMessage))
        {
            return std::nullopt;
        }

        prepared.candidate.EditDocument() = std::move(candidate);
    }

    prepared.token = {m_instanceIdentity, m_nextMaterialAssignment++, noOp};
    // Optional construction may allocate map sentinels. Failure leaves the optional
    // disengaged and the published scene untouched; Engine reports preparation failure.
    m_materialAssignment = std::move(prepared);
    return m_materialAssignment->token;
}

Material* FileSceneRuntime::GetPreparedMaterialAssignmentMaterial(const MaterialAssignmentToken& token) const noexcept
{
    return m_materialAssignment && m_materialAssignment->token == token ? m_materialAssignment->stagedMaterial : nullptr;
}

bool FileSceneRuntime::CommitMaterialAssignment(
    const MaterialAssignmentToken& token, Material& retainedSceneMaterial, std::string* errorMessage)
{
    if (m_scene.HasPhysicsPresentation())
        return AssignmentError(errorMessage, "Clear physics presentation before legacy material assignment; use coordinated publication to retain overrides.");
    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(!m_materialAssignment || m_materialAssignment->token != token || m_materialAssignment->applied)
    {
        return AssignmentError(errorMessage, "Material assignment token is invalid or already applied.");
    }

    auto& assignment = *m_materialAssignment;
    const auto metadata = GetObjectMetadata(assignment.selection.runtimeIndex, errorMessage);
    SceneDocument live;

    if(!metadata || metadata->runtimeIdentity != token.runtimeIdentity || metadata->sceneID != assignment.selection.sceneID ||
       metadata->objectID != assignment.selection.objectID || metadata->structureRevision != assignment.selection.structureRevision ||
       metadata->bindingRevision != assignment.selection.bindingRevision ||
       !SameFileState(m_fileScene, assignment.original) ||
       !CaptureSceneDocument(m_fileScene.GetDocument(), m_scene, m_objectMapping, live, errorMessage) ||
       !SceneDocumentsEqual(live, assignment.liveBefore))
    {
        return AssignmentError(errorMessage, "Scene/document/live state changed after material assignment preparation.");
    }

    const auto index = assignment.selection.runtimeIndex;

    if(assignment.slotID && (metadata->meshReference != assignment.selection.meshReference ||
        &retainedSceneMaterial != assignment.stagedMaterial ||
        !MatchesSlotAssignment(m_scene.GetRenderObjects()[index], assignment.expectedMesh, assignment.previousMaterial,
            assignment.originalSlots, *assignment.slotID)))
    {
        return AssignmentError(errorMessage, "Prepared material slot, mesh identity, or retained material changed before commit.");
    }

    std::optional<MeshResidencySourceLock> slotSource;

    if (assignment.pendingSlotSource)
    {
        if (!assignment.pendingSlotCatalog)
        {
            return AssignmentError(errorMessage, "Pending slot metadata catalog lifetime is unavailable.");
        }

        slotSource.emplace(assignment.pendingSlotCatalog->ValidateMeshResidencyCandidate(
            *assignment.pendingSlotSource, metadata->meshReference, errorMessage));

        if (!*slotSource)
        {
            return false;
        }
    }

    if(token.noOp)
    {
        Material* expected = assignment.slotID ? assignment.stagedMaterial : assignment.previousMaterial;

        if(&retainedSceneMaterial != expected)
        {
            return AssignmentError(errorMessage, "A no-op assignment cannot replace the runtime material.");
        }
    }
    else
    {
        // Reserve an epoch for this edit and a possible rollback (ABA protection).
        if(m_objectMapping.bindingRevisions[index] > (std::numeric_limits<std::uint64_t>::max)() - 2)
        {
            return AssignmentError(errorMessage, "Object binding revision identities are exhausted.");
        }

        // Complete fallible copies before publication. FileScene moves preserve exact dirty/path state.
        FileScene candidate = assignment.candidate;
        static_assert(std::is_nothrow_move_assignable_v<FileScene>);
        m_fileScene = std::move(candidate);

        if(assignment.slotID)
        {
            // Both maps were allocated during preparation. Swaps publish without allocation
            // and retain the exact original maps for a nonallocating checked rollback.
            static_assert(std::is_nothrow_swappable_v<std::map<std::string, Material*>>);
            m_scene.GetRenderObjects()[index].slotMaterials.swap(assignment.stagedRuntimeSlots);
            m_objectMapping.committedObjects[index].slotMaterials.swap(assignment.stagedCommittedSlots);
        }
        else
        {
            ApplyFallbackMaterial(m_scene.GetRenderObjects()[index], &retainedSceneMaterial, assignment.slotBindings);
            ApplyFallbackMaterial(m_objectMapping.committedObjects[index], &retainedSceneMaterial, assignment.slotBindings);
        }

        ++m_objectMapping.bindingRevisions[index];
    }

    assignment.appliedMaterial = &retainedSceneMaterial;
    assignment.applied = true;
    return true;
}

bool FileSceneRuntime::RollbackMaterialAssignment(const MaterialAssignmentToken& token, std::string* errorMessage)
{
    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(!m_materialAssignment || m_materialAssignment->token != token || !m_materialAssignment->applied)
    {
        return AssignmentError(errorMessage, "Rollback requires the exact applied material assignment.");
    }

    auto& assignment = *m_materialAssignment;
    const auto metadata = GetObjectMetadata(assignment.selection.runtimeIndex, errorMessage);
    SceneDocument live;
    const auto& expectedLive = token.noOp ? assignment.liveBefore : assignment.candidate.GetDocument();

    if(!metadata || metadata->runtimeIdentity != token.runtimeIdentity || metadata->sceneID != assignment.selection.sceneID ||
       metadata->objectID != assignment.selection.objectID || metadata->structureRevision != assignment.selection.structureRevision ||
       m_scene.GetRenderObjects()[assignment.selection.runtimeIndex].material !=
           (assignment.slotID ? assignment.previousMaterial : assignment.appliedMaterial) ||
       !SameFileState(m_fileScene, assignment.candidate) ||
       !CaptureSceneDocument(m_fileScene.GetDocument(), m_scene, m_objectMapping, live, errorMessage) ||
       !SceneDocumentsEqual(live, expectedLive))
    {
        return AssignmentError(errorMessage, "Rollback rejected: scene/document/live state changed after assignment.");
    }

    const auto index = assignment.selection.runtimeIndex;

    if(assignment.slotID && (metadata->meshReference != assignment.selection.meshReference ||
        !MatchesSlotAssignment(m_scene.GetRenderObjects()[index], assignment.expectedMesh, assignment.previousMaterial,
            token.noOp ? assignment.originalSlots : assignment.expectedAppliedSlots, *assignment.slotID)))
    {
        return AssignmentError(errorMessage, "Rollback rejected: mesh or complete runtime slot mapping changed after assignment.");
    }

    if(!token.noOp && m_objectMapping.bindingRevisions[index] == (std::numeric_limits<std::uint64_t>::max)())
    {
        return AssignmentError(errorMessage, "Object binding revision identities are exhausted before rollback.");
    }

    m_fileScene = std::move(assignment.original);

    if(assignment.slotID)
    {
        m_scene.GetRenderObjects()[index].slotMaterials.swap(assignment.stagedRuntimeSlots);
        m_objectMapping.committedObjects[index].slotMaterials.swap(assignment.stagedCommittedSlots);
    }
    else
    {
        ApplyFallbackMaterial(m_scene.GetRenderObjects()[index], assignment.previousMaterial, assignment.slotBindings);
        ApplyFallbackMaterial(m_objectMapping.committedObjects[index], assignment.previousMaterial, assignment.slotBindings);
    }

    if(!token.noOp)
    {
        ++m_objectMapping.bindingRevisions[index];
    }

    m_materialAssignment.reset();
    return true;
}

bool FileSceneRuntime::FinishMaterialAssignment(const MaterialAssignmentToken& token, std::string* errorMessage)
{
    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(!m_materialAssignment || m_materialAssignment->token != token || !m_materialAssignment->applied)
    {
        return AssignmentError(errorMessage, "Finishing requires the exact applied assignment; use Cancel before commit.");
    }

    const auto& selection = m_materialAssignment->selection;
    const auto metadata = GetObjectMetadata(selection.runtimeIndex, errorMessage);

    if(!metadata || metadata->runtimeIdentity != selection.runtimeIdentity ||
       metadata->structureRevision != selection.structureRevision || metadata->sceneID != selection.sceneID ||
       metadata->objectID != selection.objectID)
    {
        return AssignmentError(errorMessage, "Cannot finish an assignment after unexpected scene identity/structure changes.");
    }

    const auto& assignment = *m_materialAssignment;

    if(assignment.slotID && (metadata->meshReference != selection.meshReference ||
        !SameFileState(m_fileScene, assignment.candidate) ||
        !MatchesSlotAssignment(m_scene.GetRenderObjects()[selection.runtimeIndex], assignment.expectedMesh,
            assignment.previousMaterial, token.noOp ? assignment.originalSlots : assignment.expectedAppliedSlots, *assignment.slotID)))
    {
        return AssignmentError(errorMessage, "Cannot finish after the committed document, mesh, or slot mapping changed.");
    }

    m_materialAssignment.reset();
    return true;
}

bool FileSceneRuntime::CancelMaterialAssignment(const MaterialAssignmentToken& token, std::string* errorMessage)
{
    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(!m_materialAssignment || m_materialAssignment->token != token || m_materialAssignment->applied)
    {
        return AssignmentError(errorMessage, "Cancel requires the exact uncommitted assignment; use checked rollback after commit.");
    }

    m_materialAssignment.reset();
    return true;
}

bool FileSceneRuntime::Prepare(
    SceneAssetResolver& resolver,
    std::string* errorMessage)
{
    if(m_materialAssignment)
    {
        return AssignmentError(errorMessage, "Finish/cancel/rollback the material assignment before scene lifecycle operations.");
    }

    if (!m_fileScene.HasDocument())
    {
        if (errorMessage)
        {
            *errorMessage = "No file-backed scene document is loaded.";
        }

        return false;
    }

    PreparedScene prepared;

    if (!PrepareScene(
            m_fileScene.GetDocument(),
            resolver,
            prepared,
            errorMessage))
    {
        return false;
    }

    m_preparedMetadata.reset();
    CancelMeshResidency();
    m_preparedScene = std::move(prepared);
    return true;
}

bool FileSceneRuntime::Commit(std::string* errorMessage)
{
    if(m_materialAssignment)
    {
        return AssignmentError(errorMessage, "Finish/cancel/rollback the material assignment before scene lifecycle operations.");
    }

    if (!m_preparedScene)
    {
        if (errorMessage)
        {
            *errorMessage = "No prepared scene is available.";
        }

        return false;
    }

    if (!CommitPreparedScene(
            *m_preparedScene,
            m_scene,
            m_objectMapping,
            errorMessage))
    {
        return false;
    }

    DiscardPreparedScene();
    return true;
}

bool FileSceneRuntime::LoadArchive(std::shared_ptr<const CookedScene::ArchiveScene> source, std::string* error)
{
    if(m_objectMapping.scene || m_materialAssignment || !source || !source->ValidateReferences(error))
        return AssignmentError(error,"Archive load requires an inactive runtime and a validated source.");
    if(!m_fileScene.LoadDocument(source->metadata.document,error)) return false;
    DiscardPreparedScene(); m_objectMapping = {}; m_archiveScene = std::move(source); return true;
}

bool FileSceneRuntime::PrepareMetadata(FileSceneAssetCatalog& catalog, std::string* error)
{
    if(m_materialAssignment || !m_fileScene.HasDocument())
    {
        return AssignmentError(error, "Metadata preparation requires an open document without a material assignment.");
    }

    PreparedMetadataScene prepared;

    if(!PrepareSceneMetadata(m_fileScene.GetDocument(), catalog, prepared, error))
    {
        return false;
    }

    m_preparedScene.reset();
    CancelMeshResidency();
    m_preparedMetadata = std::move(prepared);
    return true;
}

bool FileSceneRuntime::CommitMetadata(FileSceneAssetCatalog& catalog, std::string* error)
{
    if(m_materialAssignment || !m_preparedMetadata || !m_fileScene.HasDocument())
    {
        return AssignmentError(error, "Metadata commit requires a prepared metadata scene without a material assignment.");
    }

    if(!SceneDocumentsEqual(m_fileScene.GetDocument(), m_preparedMetadata->document))
    {
        return AssignmentError(error, "Authored document changed after metadata preparation.");
    }

    // Resolve/validate current registry and containment again before any scene
    // mutation. A changed material borrow cannot silently replace the prepared one.
    PreparedMetadataScene verified;

    if(!PrepareSceneMetadata(m_fileScene.GetDocument(), catalog, verified, error))
    {
        return false;
    }

    for(size_t index = 0; index < verified.objects.size(); ++index)
    {
        if(!MatchesCommittedSceneObjectBindings(verified.document.objects[index],
            verified.objects[index].renderObject, m_preparedMetadata->objects[index].renderObject))
        {
            return AssignmentError(error, "Prepared metadata material bindings changed before commit.");
        }
    }

    if(!CommitPreparedMetadataScene(verified, m_scene, m_objectMapping, error))
    {
        return false;
    }

    DiscardPreparedScene();
    return true;
}

bool FileSceneRuntime::New(
    std::string sceneID,
    std::string sceneName)
{
    if(m_materialAssignment || m_archiveScene)
    {
        return false;
    }

    if (!m_fileScene.New(
            std::move(sceneID),
            std::move(sceneName)))
    {
        return false;
    }

    DiscardPreparedScene();
    m_objectMapping = {};
    if (m_scene.HasPhysicsPresentation()) m_scene.CommitPresentation({}, false);
    m_activePresentationAssociation.reset();
    return true;
}

bool FileSceneRuntime::Open(
    const std::filesystem::path& path,
    std::string* errorMessage)
{
    if(m_materialAssignment)
    {
        return AssignmentError(errorMessage, "Finish/cancel/rollback the material assignment before scene lifecycle operations.");
    }

    if(m_archiveScene) return AssignmentError(errorMessage,"Archive runtime cannot open a loose scene.");
    if (!m_fileScene.Open(path, errorMessage))
    {
        return false;
    }

    DiscardPreparedScene();
    m_objectMapping = {};
    if (m_scene.HasPhysicsPresentation()) m_scene.CommitPresentation({}, false);
    m_activePresentationAssociation.reset();
    return true;
}

SceneDocument& FileSceneRuntime::EditDocument()
{
    if(m_archiveScene) throw std::logic_error("Packaged scenes are immutable.");
    DiscardPreparedScene();
    return m_fileScene.EditDocument();
}

bool FileSceneRuntime::Save(std::string* errorMessage)
{
    return SaveAs(m_fileScene.GetPath(), errorMessage);
}

bool FileSceneRuntime::SaveAs(
    const std::filesystem::path& path,
    std::string* errorMessage)
{
    if(m_archiveScene) return AssignmentError(errorMessage,"Archive scenes are read-only; no loose scene save path.");
    if(m_materialAssignment)
    {
        return AssignmentError(errorMessage, "Finish/cancel/rollback the material assignment before scene lifecycle operations.");
    }

    SceneDocument candidate;

    if (!m_fileScene.HasDocument() ||
        !CaptureSceneDocument(m_fileScene.GetDocument(), m_scene, m_objectMapping, candidate, errorMessage))
    {
        return false;
    }

    return m_fileScene.SaveCaptured(std::move(candidate), path, errorMessage);
}

bool FileSceneRuntime::CheckUnsavedChanges(
    bool& dirty,
    std::string* errorMessage) const
{
    SceneDocument candidate;

    if (!m_fileScene.HasDocument() ||
        !CaptureSceneDocument(m_fileScene.GetDocument(), m_scene, m_objectMapping, candidate, errorMessage))
    {
        return false;
    }

    dirty = m_fileScene.IsDirty() || !SceneDocumentsEqual(m_fileScene.GetDocument(), candidate);
    return true;
}

std::unique_ptr<FileSceneRuntime> FileSceneRuntime::PrepareObjectCreation(
    SceneAssetResolver& resolver, std::string meshReference, const Transform& transform,
    std::string materialReference, std::string name, std::string& objectID,
    std::string* errorMessage) const
{
    objectID.clear();

    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(!m_fileScene.HasDocument() || m_materialAssignment || HasPreparedScene())
    {
        AssignmentError(errorMessage, "Object creation requires a committed scene without pending preparation/assignment.");
        return nullptr;
    }

    if(!AssetID::ParseReference(meshReference) ||
        (materialReference != "builtin:default" && !AssetID::ParseReference(materialReference)))
    {
        AssignmentError(errorMessage, "Creation requires a canonical mesh asset reference and a valid material reference.");
        return nullptr;
    }

    SceneDocument document;

    if(!CaptureSceneDocument(m_fileScene.GetDocument(), m_scene, m_objectMapping, document, errorMessage))
    {
        return nullptr;
    }

    SceneObjectDocument added;
    added.name = name.empty() ? "Object" : std::move(name);
    added.transform = transform;
    added.meshReference = std::move(meshReference);
    added.materialReference = std::move(materialReference);

    for(unsigned int attempt = 0; attempt < 8; ++attempt)
    {
        const auto id = AssetID::Generate();

        if(!id)
        {
            break;
        }

        const bool collision = std::any_of(document.objects.begin(), document.objects.end(),
            [&](const SceneObjectDocument& object) { return object.id == id->ToString(); });

        if(!collision)
        {
            added.id = id->ToString();
            break;
        }
    }

    if(added.id.empty())
    {
        AssignmentError(errorMessage, "Cannot generate a unique persistent scene-object ID after bounded retries.");
        return nullptr;
    }

    document.objects.push_back(added);

    if(!ValidateSceneDocument(document, errorMessage))
    {
        return nullptr;
    }

    // Resolve only the new object. Re-resolving existing material references could
    // discard assigned material versions or live working parameter changes.
    SceneDocument addition = document;
    addition.objects = {added};
    std::vector<ResolvedSceneObject> resolved;

    if (auto* catalog = dynamic_cast<FileSceneAssetCatalog*>(&resolver))
    {
        ResolvedSceneObject object;

        if (!catalog->PrepareEditorObject(addition.objects.front(), object, errorMessage))
        {
            return nullptr;
        }

        resolved.push_back(std::move(object));
    }
    else if (!ResolveSceneObjects(addition, resolver, resolved, errorMessage))
    {
        return nullptr;
    }

    PreparedScene prepared;
    prepared.environmentReference = document.environmentReference;
    prepared.camera = document.camera;
    prepared.directionalLight = document.directionalLight;
    prepared.objects.reserve(document.objects.size());

    for(const auto& object : m_fileScene.GetDocument().objects)
    {
        const auto index = m_objectMapping.idToRuntimeIndex.at(object.id);
        prepared.objects.push_back({object.id, object.name, m_scene.GetRenderObjects()[index],
            object.meshReference.empty()});
    }

    prepared.objects.push_back(std::move(resolved.front()));
    auto candidate = std::make_unique<FileSceneRuntime>();
    candidate->m_fileScene = m_fileScene;
    candidate->m_fileScene.EditDocument() = std::move(document);

    if (auto* owner = dynamic_cast<FileSceneAssetCatalog*>(&resolver); owner &&
        !owner->ValidateEditorBindings(candidate->m_fileScene.GetDocument(), prepared, errorMessage))
    {
        return nullptr;
    }

    if(!CommitPreparedEditorScene(prepared, candidate->m_fileScene.GetDocument(),
        candidate->m_scene, candidate->m_objectMapping, errorMessage))
    {
        return nullptr;
    }

    // Capture preserves the document's saved aspect ratio. Keep the live viewport
    // aspect in the runtime so insertion does not momentarily change projection.
    candidate->m_scene.GetCamera() = m_scene.GetCamera();
    objectID = std::move(added.id);
    return candidate;
}

std::unique_ptr<FileSceneRuntime> FileSceneRuntime::PrepareObjectDeletion(
    std::string_view objectID, std::string* errorMessage, FileSceneAssetCatalog* catalog) const
{
    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(!m_fileScene.HasDocument() || m_materialAssignment || HasPreparedScene())
    {
        AssignmentError(errorMessage, "Object deletion requires a committed scene without pending preparation/assignment.");
        return nullptr;
    }

    if(objectID.empty() || !GetObjectMetadata(objectID, errorMessage))
    {
        AssignmentError(errorMessage, "Object deletion requires a current persistent scene-object ID.");
        return nullptr;
    }

    SceneDocument document;

    if(!CaptureSceneDocument(m_fileScene.GetDocument(), m_scene, m_objectMapping, document, errorMessage))
    {
        return nullptr;
    }

    const auto removed = std::find_if(document.objects.begin(), document.objects.end(),
        [objectID](const SceneObjectDocument& object) { return object.id == objectID; });

    if(removed == document.objects.end())
    {
        AssignmentError(errorMessage, "The requested scene object no longer exists.");
        return nullptr;
    }

    document.objects.erase(removed);

    if(!ValidateSceneDocument(document, errorMessage))
    {
        return nullptr;
    }

    PreparedScene prepared;
    prepared.environmentReference = document.environmentReference;
    prepared.camera = document.camera;
    prepared.directionalLight = document.directionalLight;
    prepared.objects.reserve(document.objects.size());

    for(const auto& object : document.objects)
    {
        const auto index = m_objectMapping.idToRuntimeIndex.at(object.id);
        prepared.objects.push_back({object.id, object.name, m_scene.GetRenderObjects()[index],
            object.meshReference.empty()});
    }

    auto candidate = std::make_unique<FileSceneRuntime>();
    candidate->m_fileScene = m_fileScene;
    candidate->m_fileScene.EditDocument() = std::move(document);

    if (auto* owner = catalog; owner &&
        !owner->ValidateEditorBindings(candidate->m_fileScene.GetDocument(), prepared, errorMessage))
    {
        return nullptr;
    }

    if(!CommitPreparedEditorScene(prepared, candidate->m_fileScene.GetDocument(),
        candidate->m_scene, candidate->m_objectMapping, errorMessage))
    {
        return nullptr;
    }

    candidate->m_scene.GetCamera() = m_scene.GetCamera();
    return candidate;
}

std::unique_ptr<FileSceneRuntime> FileSceneRuntime::PrepareObjectRestoration(SceneAssetResolver& resolver,
    const SceneObjectDocument& restored, size_t insertionIndex, std::string* errorMessage) const
{
    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(!m_fileScene.HasDocument() || m_materialAssignment || HasPreparedScene())
    {
        AssignmentError(errorMessage, "Object restoration requires a committed scene without pending preparation/assignment.");
        return nullptr;
    }

    SceneDocument document;

    if(!CaptureSceneDocument(m_fileScene.GetDocument(), m_scene, m_objectMapping, document, errorMessage))
    {
        return nullptr;
    }

    if(restored.id.empty() || m_objectMapping.idToRuntimeIndex.contains(restored.id))
    {
        AssignmentError(errorMessage, "Restoration requires a nonempty persistent object ID that is absent from the scene.");
        return nullptr;
    }

    if(insertionIndex > document.objects.size())
    {
        AssignmentError(errorMessage, "Restoration document insertion index is out of range.");
        return nullptr;
    }

    document.objects.insert(document.objects.begin() + insertionIndex, restored);

    if(!ValidateSceneDocument(document, errorMessage))
    {
        return nullptr;
    }

    // Resolve only the restored instance. Surviving objects keep their exact live
    // catalog borrows, including previously assigned material versions and overrides.
    SceneDocument addition = document;
    addition.objects = {restored};
    std::vector<ResolvedSceneObject> resolved;

    if (auto* catalog = dynamic_cast<FileSceneAssetCatalog*>(&resolver))
    {
        ResolvedSceneObject object;

        if (!catalog->PrepareEditorObject(addition.objects.front(), object, errorMessage))
        {
            return nullptr;
        }

        resolved.push_back(std::move(object));
    }
    else if (!ResolveSceneObjects(addition, resolver, resolved, errorMessage))
    {
        return nullptr;
    }

    PreparedScene prepared;
    prepared.environmentReference = document.environmentReference;
    prepared.camera = document.camera;
    prepared.directionalLight = document.directionalLight;
    prepared.objects.reserve(document.objects.size());

    for(const auto& object : document.objects)
    {
        if(object.id == restored.id)
        {
            prepared.objects.push_back(std::move(resolved.front()));
            continue;
        }

        const auto index = m_objectMapping.idToRuntimeIndex.at(object.id);
        prepared.objects.push_back({object.id, object.name, m_scene.GetRenderObjects()[index],
            object.meshReference.empty()});
    }

    auto candidate = std::make_unique<FileSceneRuntime>();
    candidate->m_fileScene = m_fileScene;
    candidate->m_fileScene.EditDocument() = std::move(document);

    if (auto* owner = dynamic_cast<FileSceneAssetCatalog*>(&resolver); owner &&
        !owner->ValidateEditorBindings(candidate->m_fileScene.GetDocument(), prepared, errorMessage))
    {
        return nullptr;
    }

    if(!CommitPreparedEditorScene(prepared, candidate->m_fileScene.GetDocument(),
        candidate->m_scene, candidate->m_objectMapping, errorMessage))
    {
        return nullptr;
    }

    candidate->m_scene.GetCamera() = m_scene.GetCamera();
    candidate->m_scene.SetDisplacementPatchLODs(m_scene.GetDisplacementPatchLODs());
    return candidate;
}

bool FileSceneRuntime::CaptureObjectDeletionSnapshot(std::string_view objectID,
    SceneObjectDocument& object, size_t& documentIndex, std::string* errorMessage) const
{
    SceneDocument captured;

    if(!m_fileScene.HasDocument() ||
        !CaptureSceneDocument(m_fileScene.GetDocument(), m_scene, m_objectMapping, captured, errorMessage))
    {
        return false;
    }

    const auto found = std::find_if(captured.objects.begin(), captured.objects.end(),
        [objectID](const SceneObjectDocument& entry) { return entry.id == objectID; });

    if(found == captured.objects.end())
    {
        return AssignmentError(errorMessage, "Deletion snapshot target no longer exists.");
    }

    documentIndex = static_cast<size_t>(found - captured.objects.begin());
    object = *found;
    return true;
}

std::optional<PresentationTransform> FileSceneRuntime::GetObjectPresentation(
    const SceneObjectMetadata& identity, std::string* errorMessage) const
{
    const auto current = GetObjectMetadata(identity.objectID, errorMessage);
    if (!current)
    {
        return std::nullopt;
    }
    if (current->sceneID != identity.sceneID || current->runtimeIdentity != identity.runtimeIdentity ||
        current->structureRevision != identity.structureRevision || current->runtimeIndex != identity.runtimeIndex ||
        current->bindingRevision != identity.bindingRevision)
    {
        if (errorMessage)
        {
            *errorMessage = "Presentation read belongs to a stale scene object or binding.";
        }
        return std::nullopt;
    }

    return m_scene.GetRenderObjects()[current->runtimeIndex].GetPresentationTransform();
}
