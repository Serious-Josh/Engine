#include "Editor.h"
#include "SceneObjectDuplication.h"
#include "Scene/SceneDocumentAdapter.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "imgui.h"
#include <utility>
#include <exception>
void Editor::HandleSceneObjectDuplication(IDemoScene& scene)
{
    if (IsPhysicsEditingLocked())
    {
        return;
    }

    const auto& io = ImGui::GetIO();
    const bool focused = (m_viewportFocused && !io.WantCaptureKeyboard) || m_hierarchyPanel.IsFocused();
    if(!io.KeyCtrl || io.KeyShift || io.KeyAlt || !ImGui::IsKeyPressed(ImGuiKey_D,false) ||
        !focused || HistoryReplayBlocked())
    {
        return;
    }
    const auto* owner=dynamic_cast<const FileBackedDemoScene*>(&scene);
    if(!owner || owner->IsArchiveActivation() || !m_historySession ||
        !CanPreserveSceneHistory(scene,m_historySession->projectIdentity,m_historyEpoch) ||
        m_selectionStructureRevision != scene.GetScene().GetStructureRevision())
    {
        return;
    }
    const auto selected=owner->GetObjectMetadata(m_selectedObject);
    if(!selected)
    {
        return;
    }

    std::string error;

    if(!QueueSceneObjectDuplication(scene, *selected, error))
    {
        ReportSceneHistoryFailure(error);
    }
}

bool Editor::QueueSceneObjectDuplication(IDemoScene& scene, const SceneObjectMetadata& target, std::string& error)
{
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);

    if(!owner || owner->IsArchiveActivation() || !m_historySession || IsPhysicsEditingLocked() ||
        !SceneObjectDeletionMatches(scene, target) ||
        !CanPreserveSceneHistory(scene, m_historySession->projectIdentity, m_historyEpoch) ||
        HasPendingSceneHistoryOperation() || m_sceneOperationPending || HasBlockingObjectCreation())
    {
        error = "Duplication requires a stopped, current scene without pending edits.";
        return false;
    }

    try
    {
        HistoryReplayRequest request;
        request.hierarchy.emplace();
        std::string duplicateRoot;

        if(!CaptureSceneDocument(owner->m_runtime->GetFileScene().GetDocument(), owner->GetScene(),
            owner->m_runtime->GetObjectMapping(), request.hierarchy->before, &error) ||
            !SceneHierarchy::DuplicateSubtree(request.hierarchy->before, target.objectID,
                request.hierarchy->after, duplicateRoot, error, &request.hierarchy->bindingSources))
        {
            return false;
        }

        const auto root = std::find_if(request.hierarchy->after.objects.begin(), request.hierarchy->after.objects.end(),
            [&](const auto& object) { return object.id == duplicateRoot; });
        request.object = *root;
        request.insertionIndex = static_cast<size_t>(root - request.hierarchy->after.objects.begin());
        request.bindingSourceID = target.objectID;
        request.identity = {AllocateSceneHistoryOperationID(), m_historyEpoch, false, duplicateRoot};
        request.membership = true;

        if(!StageHierarchyHistory(*request.hierarchy, target.objectID, duplicateRoot,
            request.identity.operationID, error))
        {
            return false;
        }

        m_duplicationRequest = std::move(request);
        m_sceneHistoryStatus = "Duplicating scene object...";
        return true;
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}
std::optional<Editor::HistoryReplayRequest> Editor::TakeSceneDuplicationRequest()
{
    return std::exchange(m_duplicationRequest, std::nullopt);
}
