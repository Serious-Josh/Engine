#include "Editor.h"
#include "SceneObjectDuplication.h"
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

    const auto& io=ImGui::GetIO();
    const bool focused=(m_viewportFocused && !io.WantCaptureKeyboard) || m_hierarchyPanel.IsFocused();
    if(!io.KeyCtrl || io.KeyShift || io.KeyAlt || !ImGui::IsKeyPressed(ImGuiKey_D,false) ||
        !focused || HistoryReplayBlocked()) return;
    const auto* owner=dynamic_cast<const FileBackedDemoScene*>(&scene);
    if(!owner || owner->IsArchiveActivation() || !m_historySession ||
        !CanPreserveSceneHistory(scene,m_historySession->projectIdentity,m_historyEpoch) ||
        m_selectionStructureRevision!=scene.GetScene().GetStructureRevision()) return;
    const auto selected=owner->GetObjectMetadata(m_selectedObject);
    if(!selected) return;
    try
    {
        HistoryReplayRequest request;
        std::string error;
        if(!PrepareSceneObjectDuplicate(*owner->m_runtime,selected->objectID,request.object,request.insertionIndex,error))
        { ReportSceneHistoryFailure(error); return; }
        request.identity={AllocateSceneHistoryOperationID(),m_historyEpoch,false,request.object.id};
        request.bindingSourceID=selected->objectID;
        request.membership=true;
        if(!StageObjectDeletionHistory(request.object,request.insertionIndex,selected->objectID,request.object.id,
            request.identity.operationID,m_historyEpoch,false,error,true)) { ReportSceneHistoryFailure(error); return; }
        m_duplicationRequest=std::move(request);
        m_sceneHistoryStatus="Duplicating scene object...";
    }
    catch(const std::exception& e) { ReportSceneHistoryFailure(e.what()); }
}
std::optional<Editor::HistoryReplayRequest> Editor::TakeSceneDuplicationRequest()
{ return std::exchange(m_duplicationRequest,std::nullopt); }
