#include "Scene/FileSceneRuntime.h"

#include "Scene/SceneAssetResolver.h"
#include "Scene/SceneDocumentAdapter.h"

#include <utility>
#include <atomic>
#include <type_traits>
#include "assets/AssetID.h"

namespace
{
    std::atomic<std::uint64_t> nextRuntimeIdentity{1};
}

FileSceneRuntime::FileSceneRuntime()
    : m_instanceIdentity(nextRuntimeIdentity.fetch_add(1, std::memory_order_relaxed))
{
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
       mapping.committedObjects.size() != objects.size())
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

        if(objects[index].mesh != mapping.committedObjects[index].mesh ||
           objects[index].material != mapping.committedObjects[index].material)
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
        selected->id, document.id, runtimeIndex, m_instanceIdentity, mapping.structureRevision};
}

namespace
{
    bool AssignmentError(std::string* error, const char* message)
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
}

std::optional<MaterialAssignmentToken> FileSceneRuntime::PrepareMaterialAssignment(
    const SceneObjectMetadata& selection, std::string materialReference, std::string* errorMessage)
{
    if(errorMessage)
    {
        errorMessage->clear();
    }

    if(m_materialAssignment || m_preparedScene || m_nextMaterialAssignment == 0)
    {
        AssignmentError(errorMessage, "Another scene preparation/assignment is pending, or transaction identities exhausted.");
        return std::nullopt;
    }

    const auto metadata = GetObjectMetadata(selection.runtimeIndex, errorMessage);

    if(!metadata || metadata->sceneID != selection.sceneID || metadata->objectID != selection.objectID ||
       metadata->runtimeIdentity != selection.runtimeIdentity || metadata->structureRevision != selection.structureRevision)
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

bool FileSceneRuntime::CommitMaterialAssignment(
    const MaterialAssignmentToken& token, Material& retainedSceneMaterial, std::string* errorMessage)
{
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
       !SameFileState(m_fileScene, assignment.original) ||
       !CaptureSceneDocument(m_fileScene.GetDocument(), m_scene, m_objectMapping, live, errorMessage) ||
       !SceneDocumentsEqual(live, assignment.liveBefore))
    {
        return AssignmentError(errorMessage, "Scene/document/live state changed after material assignment preparation.");
    }

    const auto index = assignment.selection.runtimeIndex;

    if(token.noOp)
    {
        if(&retainedSceneMaterial != assignment.previousMaterial)
        {
            return AssignmentError(errorMessage, "A no-op assignment cannot replace the runtime material.");
        }
    }
    else
    {
        // Complete fallible copies before publication. FileScene moves preserve exact dirty/path state.
        FileScene candidate = assignment.candidate;
        static_assert(std::is_nothrow_move_assignable_v<FileScene>);
        m_fileScene = std::move(candidate);
        m_scene.GetRenderObjects()[index].material = &retainedSceneMaterial;
        m_objectMapping.committedObjects[index].material = &retainedSceneMaterial;
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
       m_scene.GetRenderObjects()[assignment.selection.runtimeIndex].material != assignment.appliedMaterial ||
       !SameFileState(m_fileScene, assignment.candidate) ||
       !CaptureSceneDocument(m_fileScene.GetDocument(), m_scene, m_objectMapping, live, errorMessage) ||
       !SceneDocumentsEqual(live, expectedLive))
    {
        return AssignmentError(errorMessage, "Rollback rejected: scene/document/live state changed after assignment.");
    }

    const auto index = assignment.selection.runtimeIndex;
    m_fileScene = std::move(assignment.original);
    m_scene.GetRenderObjects()[index].material = assignment.previousMaterial;
    m_objectMapping.committedObjects[index].material = assignment.previousMaterial;
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

    m_preparedScene.reset();
    return true;
}

bool FileSceneRuntime::New(
    std::string sceneID,
    std::string sceneName)
{
    if(m_materialAssignment)
    {
        return false;
    }

    if (!m_fileScene.New(
            std::move(sceneID),
            std::move(sceneName)))
    {
        return false;
    }

    m_preparedScene.reset();
    m_objectMapping = {};
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

    if (!m_fileScene.Open(path, errorMessage))
    {
        return false;
    }

    m_preparedScene.reset();
    m_objectMapping = {};
    return true;
}

SceneDocument& FileSceneRuntime::EditDocument() noexcept
{
    m_preparedScene.reset();
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
