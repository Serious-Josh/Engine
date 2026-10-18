#include "Engine/Engine.h"
#include "Editor/ScenePrimitiveCreation.h"
#include "Project/ProjectPaths.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Core/Log.h"
#include "Scene/SceneDocumentAdapter.h"
#include <exception>
#include <string>
#include <new>
#include <algorithm>

bool Engine::BeginEditorMembership(Editor::HistoryReplayRequest request, bool replay, std::string& error,
    std::optional<SceneObjectMetadata> deletion)
{
    if (PhysicsBlocksSceneEdits())
    {
        error = "Stop physics before changing scene membership or replaying history.";
        return false;
    }

    try
    {
        const auto identity = GetSceneObjectRestorationIdentity();
        auto* owner = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());

        if(!identity || !owner || owner->IsArchiveActivation() || m_pendingEditorMembership ||
            m_pendingSceneObjectCreation || m_pendingSceneObjectDeletion || m_pendingSceneObjectRestoration ||
            m_pendingMaterialAssignment || m_pendingEditorSceneOperation || m_demoSceneManager.HasPendingSceneChange() ||
            identity->historyEpoch != request.identity.epoch ||
            !m_editor.CanPreserveSceneHistory(*owner, identity->projectIdentity, identity->historyEpoch) ||
            (replay && !m_editor.MatchesPendingHistoryReplay(request.identity)))
        {
            error = "Membership edit belongs to a stale scene/history session or conflicts with another operation.";
            return false;
        }

        if(replay && request.remove && !request.hierarchy)
        {
            SceneObjectDocument current;
            size_t index = 0;

            if(!owner->m_runtime->CaptureObjectDeletionSnapshot(request.object.id, current, index, &error))
            {
                return false;
            }

            SceneDocument actual;
            SceneDocument expected;
            actual.objects = {current};
            expected.objects = {request.object};

            if(index != request.insertionIndex || !SceneDocumentsEqual(actual, expected))
            {
                error = "Undo/redo membership target changed outside its history command.";
                return false;
            }
        }

        if(request.hierarchy)
        {
            SceneDocument current;

            if(!CaptureSceneDocument(owner->m_runtime->GetFileScene().GetDocument(), owner->GetScene(),
                owner->m_runtime->GetObjectMapping(), current, &error) ||
                request.hierarchy->before.id != current.id || request.hierarchy->after.id != current.id ||
                !SceneHierarchy::SameObjects(request.hierarchy->before, current) ||
                !ValidateSceneDocument(request.hierarchy->after, &error))
            {
                error = "Hierarchy transaction expected state changed or candidate is invalid: " + error;
                return false;
            }

            bool sameMembership = current.objects.size() == request.hierarchy->after.objects.size();

            for(const auto& object : request.hierarchy->after.objects)
            {
                sameMembership = sameMembership && owner->m_runtime->GetObjectMapping().idToRuntimeIndex.contains(object.id);
            }

            if(sameMembership)
            {
                // Reuse the renderer's existing authored-edit eligibility gate. It covers
                // recording/submission uncertainty and pending displacement or recovery.
                if(!m_renderer.CanEditPendingMaterialBindings(owner->GetScene()))
                {
                    error = "Renderer is not ready for a hierarchy-only commit; scene unchanged.";
                    return false;
                }

                // No GPU membership work: stage selection/name before the last fallible CPU commit.
                auto selection = request.identity.selectionID ? owner->GetObjectMetadata(*request.identity.selectionID) : std::nullopt;
                auto sceneName = std::string(m_demoSceneManager.GetActiveSceneName());

                if(!owner->m_runtime->CommitHierarchyEdit(request.hierarchy->before, request.hierarchy->after, &error))
                {
                    return false;
                }

                m_editor.PublishRestoredObjectSelection(*owner, std::move(selection), std::move(sceneName), false);

                if(replay)
                {
                    m_editor.CompleteSceneHistoryReplay(request.identity, true, {});
                }
                else
                {
                    m_editor.CompleteObjectDeletionHistory(request.identity.operationID, request.identity.epoch, true, {});
                }

                m_editor.SetSceneOperationPending(false);
                return true;
            }
        }

        EditorMembershipOperation operation{std::move(request), *identity, replay, false, std::move(deletion), {},
            std::string(m_demoSceneManager.GetActiveSceneName())};
        // Cancel acceptance only; retain catalog geometry/BLAS and reusable CPU cache.
        CancelMeshResidencyAcceptance();
        m_pendingEditorMembership = std::move(operation);
        return true;
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}
bool Engine::BeginEditorMembershipDeletion(const SceneObjectMetadata& target,std::string& error)
{
    try
    {
        auto* owner = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
        const auto current = owner ? owner->GetObjectMetadata(target.objectID, &error) : std::nullopt;

        if(!current || current->runtimeIdentity != target.runtimeIdentity || current->structureRevision != target.structureRevision)
        {
            error = "Deletion target belongs to a stale scene.";
            return false;
        }

        Editor::HistoryReplayRequest request;
        request.hierarchy.emplace();

        if(!CaptureSceneDocument(owner->m_runtime->GetFileScene().GetDocument(), owner->GetScene(),
            owner->m_runtime->GetObjectMapping(), request.hierarchy->before, &error) ||
            !SceneHierarchy::DeleteSubtree(request.hierarchy->before, target.objectID, request.hierarchy->after, error))
        {
            return false;
        }

        const auto before = owner->GetObjectMetadata(m_editor.GetSelectedObject());
        const auto selectionBefore = before ? std::optional<std::string>(before->objectID) : std::nullopt;
        auto selectionAfter = selectionBefore;

        if(selectionAfter && std::none_of(request.hierarchy->after.objects.begin(), request.hierarchy->after.objects.end(),
            [&](const auto& object) { return object.id == *selectionAfter; }))
        {
            selectionAfter.reset();
        }

        request.identity = {m_editor.AllocateSceneHistoryOperationID(), m_editor.GetSceneHistoryEpoch(), false, selectionAfter};
        request.membership = true;
        request.remove = true;
        // Allocate the completion payload before history is staged, so a failed
        // metadata copy cannot strand a pending history command.
        std::optional<SceneObjectMetadata> deletion = target;

        if(!m_editor.StageHierarchyHistory(*request.hierarchy, selectionBefore, selectionAfter, request.identity.operationID, error))
        {
            return false;
        }

        const auto operationID = request.identity.operationID;
        const auto epoch = request.identity.epoch;

        if(BeginEditorMembership(std::move(request), false, error, std::move(deletion)))
        {
            return true;
        }

        m_editor.CompleteObjectDeletionHistory(operationID, epoch, false, error);
        return false;
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

void Engine::CancelEditorMembership(const std::string& error)
{
    m_editorMembership.Cancel();
    if(!m_pendingEditorMembership) return;
    auto operation=std::move(*m_pendingEditorMembership); m_pendingEditorMembership.reset();
    if(operation.replay) m_editor.CompleteSceneHistoryReplay(operation.request.identity,false,error);
    else m_editor.CompleteObjectDeletionHistory(operation.request.identity.operationID,operation.request.identity.epoch,false,error);
    if(operation.deletion)
        if(auto* owner=m_demoSceneManager.TryGetActiveScene()) m_editor.OnSceneObjectDeletionCompleted(*owner,*operation.deletion,false,error);
    m_editor.SetSceneOperationPending(false);
}
bool Engine::AdvanceEditorMembership()
{
    if(!m_pendingEditorMembership) return true;
    try
    {
        auto& operation=*m_pendingEditorMembership;
        auto* owner=dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
        const auto identity=GetSceneObjectRestorationIdentity();
        if(!owner || !identity || *identity!=operation.identity || m_demoSceneManager.HasPendingSceneChange() ||
            !m_editor.CanPreserveSceneHistory(*owner,identity->projectIdentity,identity->historyEpoch) ||
            (operation.replay && !m_editor.MatchesPendingHistoryReplay(operation.request.identity)))
        { CancelEditorMembership("Scene/history changed before membership publication."); return true; }
        using Status=SceneMembershipPublication::Status;
        if(!operation.prepared)
        {
            const auto prepare=[&]
            {
                if(operation.request.hierarchy)
                {
                    SceneDocument current;
                    std::string error;

                    if(!CaptureSceneDocument(owner->m_runtime->GetFileScene().GetDocument(), owner->GetScene(),
                        owner->m_runtime->GetObjectMapping(), current, &error) ||
                        !SceneHierarchy::SameObjects(operation.request.hierarchy->before, current))
                    {
                        return SceneMembershipPublication::Result{Status::Stale, "Hierarchy expected state changed."};
                    }

                    return m_editorMembership.PrepareDocument(m_renderer, *owner->m_runtime, owner->m_assets,
                        operation.request.hierarchy->after, 0, operation.request.hierarchy->bindingSources);
                }

                return operation.request.remove ?
                    m_editorMembership.PrepareRemoval(m_renderer,*owner->m_runtime,owner->m_assets,operation.request.object.id,0) :
                    m_editorMembership.PrepareInsertion(m_renderer,*owner->m_runtime,owner->m_assets,operation.request.object,
                        operation.request.insertionIndex,0,
                        operation.request.bindingSourceID ? std::string_view(*operation.request.bindingSourceID) : std::string_view{});
            };
            auto result=prepare();

            // Descriptor exhaustion is not a deferral: retrying unchanged every frame cannot
            // succeed. Reclaim idle Editor thumbnail descriptors once at this main-thread
            // boundary and retry immediately; if capacity is still short, cancel. Preparation
            // never touched the active scene, so cancellation leaves scene and history coherent.
            if(result.status==Status::ResourceExhausted && !operation.descriptorReclaimAttempted)
            {
                operation.descriptorReclaimAttempted=true;
                const auto reclaimed=m_renderer.ReclaimIdleEditorDescriptors();
                Log::Warning("Scene membership needs descriptors; reclaimed " + std::to_string(reclaimed) +
                    " idle Editor thumbnail descriptors and retrying once.");

                if(reclaimed)
                {
                    result=prepare();
                }
            }

            if(result.status==Status::ResourceExhausted)
            {
                const auto statistics=m_renderer.GetResourceDescriptorStatistics();
                CancelEditorMembership(result.message + " The scene was not changed (" +
                    std::to_string(statistics.allocated) + " of " + std::to_string(statistics.capacity) +
                    " descriptors in use).");
                return true;
            }

            if(result.status==Status::Retry) return true;
            if(result.status!=Status::Ready)
            { const bool safe=result.status!=Status::Failed; CancelEditorMembership(result.message); return safe; }
            operation.prepared=true;
            if(operation.request.identity.selectionID)
                operation.selection=m_editorMembership.GetPreparedSelection(*operation.request.identity.selectionID);
        }
        std::optional<std::size_t> mapped;
        const auto selection=operation.request.identity.selectionID ?
            std::optional<std::string_view>(*operation.request.identity.selectionID) : std::nullopt;
        auto result=m_editorMembership.PublishAtBoundary(m_renderer,owner->m_runtime,owner->m_assets,selection,mapped);
        if(result.status==Status::Retry) return true;
        if(result.status!=Status::Ready)
        { const bool safe=result.status!=Status::Failed; CancelEditorMembership(result.message); return safe; }
        // The coordinator published CPU/GPU order together. All selection payloads
        // and history storage were allocated before that point.
        auto completed=std::move(operation); m_pendingEditorMembership.reset();
        m_renderedSceneStructureRevision=owner->GetScene().GetStructureRevision();
        m_editor.PublishRestoredObjectSelection(*owner,std::move(completed.selection),std::move(completed.sceneName),false);
        if(completed.replay) m_editor.CompleteSceneHistoryReplay(completed.request.identity,true,{});
        else m_editor.CompleteObjectDeletionHistory(completed.request.identity.operationID,completed.request.identity.epoch,true,{});
        if(completed.deletion) m_editor.OnSceneObjectDeletionCompleted(*owner,*completed.deletion,true,{});
        m_editor.SetSceneOperationPending(false);
        ContinueAutomaticMeshResidencyAfterReplacement(completed.identity.runtimeIdentity);
        return true;
    }
    catch(const std::bad_alloc&) { CancelEditorMembership("Membership allocation failed; scene unchanged."); return true; }
    catch(const std::exception& e) { CancelEditorMembership(e.what()); return false; }
}


bool Engine::BeginPrimitiveCreation(const Editor::PrimitiveCreationRequest& intent, std::string& error)
{
    const auto preparePrimitive = [&](const std::filesystem::path& project, const SceneDocument& document,
        SceneObjectDocument& object, std::string& prepareError)
    {
        std::string reference;

        if(!PrimitiveMeshAssets::Ensure(ProjectPaths(project), m_projectAssetRegistry, intent.primitive, reference, prepareError))
        {
            return false;
        }

        object = PreparePrimitiveObject(document, intent.primitive, reference);
        return true;
    };

    return BeginObjectCreation(intent.runtimeIdentity, intent.historyEpoch, "Primitive", preparePrimitive, error);
}

bool Engine::BeginEmptyObjectCreation(const Editor::EmptyObjectCreationRequest& intent, std::string& error)
{
    // No asset work: an Empty Object references no mesh or material.
    const auto prepareEmpty = [](const std::filesystem::path&, const SceneDocument& document,
        SceneObjectDocument& object, std::string&)
    {
        object = PrepareEmptyObject(document);
        return true;
    };

    return BeginObjectCreation(intent.runtimeIdentity, intent.historyEpoch, "Empty Object", prepareEmpty, error);
}

bool Engine::BeginObjectCreation(std::uint64_t runtimeIdentity, std::uint64_t historyEpoch, const char* operationName,
    const PrepareCreatedObject& prepareObject, std::string& error)
{
    error.clear();
    try
    {
        auto* owner = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
        if(PhysicsBlocksSceneEdits() || m_editor.IsPhysicsEditingLocked() || m_state != EngineState::Running ||
            !owner || owner->IsArchiveActivation() || owner->GetRuntimeIdentity() != runtimeIdentity ||
            historyEpoch != m_editor.GetSceneHistoryEpoch() || m_pendingEditorMembership ||
            m_pendingSceneObjectCreation || m_pendingSceneObjectDeletion || m_pendingSceneObjectRestoration ||
            m_pendingMaterialAssignment || m_pendingEditorSceneOperation || m_demoSceneManager.HasPendingSceneChange() ||
            m_editor.HasBlockingObjectCreation() || m_editor.HasBlockingMaterialOperation() ||
            m_editor.HasPendingSceneHistoryOperation() || owner->m_objectMutationCandidate ||
            owner->m_runtime->HasPreparedScene() || owner->m_runtime->HasMaterialAssignment())
        {
            error = std::string(operationName) +
                " creation requires a stopped editable scene without pending operations or transform gestures.";
            return false;
        }
        const auto project = std::filesystem::canonical(owner->m_projectRoot);
        if(!m_editor.CanPreserveSceneHistory(*owner, project, historyEpoch))
        {
            error = std::string(operationName) + " creation belongs to a stale scene history session.";
            return false;
        }
        SceneDocument document;
        if(!CaptureSceneDocument(owner->m_runtime->GetFileScene().GetDocument(), owner->GetScene(),
            owner->m_runtime->GetObjectMapping(), document, &error)) return false;

        // The new object is appended, so existing scene and render-object indices are unchanged.
        Editor::HistoryReplayRequest request;

        if(!prepareObject(project, document, request.object, error))
        {
            return false;
        }

        request.insertionIndex = document.objects.size();
        request.membership = true;
        request.identity = {m_editor.AllocateSceneHistoryOperationID(), historyEpoch, false, request.object.id};
        const auto previous = owner->GetObjectMetadata(m_editor.GetSelectedObject());
        const auto selectionBefore = previous ? std::optional<std::string>{previous->objectID} : std::nullopt;
        request.hierarchy = SceneHierarchyEdit{document, document};
        request.hierarchy->after.objects.push_back(request.object);

        if(!m_editor.StageHierarchyHistory(*request.hierarchy, selectionBefore, request.object.id,
            request.identity.operationID, error))
        {
            return false;
        }

        const auto operationID = request.identity.operationID;
        if(!BeginEditorMembership(std::move(request), false, error))
        {
            m_editor.CompleteObjectDeletionHistory(operationID, historyEpoch, false, error);
            return false;
        }
        m_editor.SetSceneOperationPending(true);
        return true;
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}
