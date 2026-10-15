#include "Engine/Engine.h"
#ifndef NDEBUG
#include "Editor/SceneObjectDuplication.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Scene/SceneDocumentAdapter.h"
#include "Scene/SceneSerializer.h"
#include "assets/CookedScene.h"
#include "Core/Log.h"
#include "imgui.h"
#include <chrono>
#include <algorithm>
#include <thread>
#include <stdexcept>
namespace
{
void CheckDuplicate(bool value,const std::string& error) { if(!value) throw std::runtime_error("Duplication: "+error); }
bool SameObject(const SceneObjectDocument& a,const SceneObjectDocument& b)
{ SceneDocument x,y; x.objects={a}; y.objects={b}; return SceneDocumentsEqual(x,y); }
}
class SceneDuplicationVerification
{
public:
static void Run(Engine& engine,FileBackedDemoScene& owner)
{
    ImGui::GetIO().ConfigInputTrickleEventQueue=false;
    auto& editor=engine.m_editor; auto& renderer=engine.m_renderer;
    // Settle initial native messages and viewport target sizing before measuring
    // ordinary membership operations; resizing has its own existing drain.
    engine.Frame(); engine.Frame();
    std::string error;
    const ProjectPaths paths(owner.GetProjectRoot());
    editor.UpdateSceneHistorySession(owner,paths,std::string(engine.m_demoSceneManager.GetActiveSceneName()));
    editor.PublishRestoredObjectSelection(owner,owner.GetObjectMetadata(std::size_t{0}),
        std::string(engine.m_demoSceneManager.GetActiveSceneName()),false);
    editor.SelectSceneObject(owner,1);
    editor.SelectSceneObject(owner,0,true);
    const auto original=owner.m_runtime->GetFileScene().GetDocument().objects.front();
    CheckDuplicate(original.name=="Cat","Fixture first object must be named Cat.");
    const auto originalMesh=owner.GetScene().GetRenderObjects()[0].mesh;
    const auto originalSlots=owner.GetScene().GetRenderObjects()[0].slotMaterials;
    const auto blasCount=renderer.m_rayTracingScene.m_blasRegistry.bottomLevels.size();
    const auto drains=renderer.m_commandContext.GetGPUDrainCount();
    SceneDocument names;
    for(const auto& name:{"Cat","Cat (2)","Cat (3)","Cat (toy)"}) { SceneObjectDocument object; object.name=name; names.objects.push_back(object); }
    CheckDuplicate(UniqueDuplicateObjectName("Cat (2)",names)=="Cat (4)","Numbered base naming failed.");
    names.objects.erase(names.objects.begin()+1);
    CheckDuplicate(UniqueDuplicateObjectName("Cat",names)=="Cat (2)" &&
        UniqueDuplicateObjectName("Cat (toy)",names)=="Cat (toy) (2)","Naming holes or unrelated parentheses failed.");
    // Full arbitrary authored maps are copied by the same helper (not inferred from runtime slots).
    auto shortcut=[&](ImGuiKey key,bool down,bool text=false,bool sendEvent=true) {
        auto& io=ImGui::GetIO(); io.AddKeyEvent(ImGuiMod_Ctrl,true); if(sendEvent) io.AddKeyEvent(key,down);
        renderer.GetDebugUI().BeginFrame();
        editor.PrepareWorkspace(owner,engine.m_demoSceneManager,renderer.GetDebugUI());
        io.KeyCtrl=true;
        io.WantCaptureKeyboard=false; io.WantTextInput=text; editor.m_viewportFocused=true;
        editor.m_viewportImage.size=ImVec2(640,480);
        if(key==ImGuiKey_D)
        {
            editor.HandleSceneObjectDuplication(owner);
        }
        else if(key==ImGuiKey_Z) editor.HandleTransformUndo(owner);
        else if(key==ImGuiKey_Y) editor.HandleTransformRedo(owner);
        io.WantTextInput=false;
        RendererOptions options; options.selectedObject=editor.GetSelectedObject();
        CheckDuplicate(renderer.Render(owner.GetScene(),options)!=Renderer::FrameRenderResult::Error,"Shortcut frame failed.");
    };
    auto settle=[&]() {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
        // Consume requests and publish through the normal editor Frame path.
        if(editor.m_duplicationRequest || editor.m_historyReplayRequest) engine.Frame();
        while(engine.m_pendingEditorMembership)
        {
            CheckDuplicate(std::chrono::steady_clock::now()<deadline,"Membership operation timed out.");
            engine.Frame(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    auto snapshot=[&](const std::string& id) { SceneObjectDocument value; std::size_t index=0;
        CheckDuplicate(owner.m_runtime->CaptureObjectDeletionSnapshot(id,value,index,&error),error); return value; };
    const auto count=owner.GetScene().GetRenderObjects().size();
    shortcut(ImGuiKey_D,true,true);
    CheckDuplicate(!editor.m_duplicationRequest,"Text editing triggered duplication.");
    shortcut(ImGuiKey_D,false);
    shortcut(ImGuiKey_D,true);
    CheckDuplicate(editor.m_duplicationRequest.has_value(),"Ctrl+D did not queue a duplicate.");
    const auto first=editor.m_duplicationRequest->object;
    CheckDuplicate(first.id!=original.id && first.name=="Cat (3)" &&
        owner.GetScene().GetRenderObjects().size()==count,"Duplicate published before acceptance or naming failed.");
    auto expected=original; expected.id=first.id; expected.name=first.name;
    CheckDuplicate(SameObject(first,expected),"Mesh, all authored material slots or exact transform changed.");
    const auto cursor=editor.m_historyCursor;
    shortcut(ImGuiKey_Z,true); // Pending duplication must lock history replay.
    CheckDuplicate(editor.m_historyCursor==cursor && !editor.m_historyReplayRequest,"Undo raced pending duplication.");
    shortcut(ImGuiKey_Z,false);
    settle();
    CheckDuplicate(editor.m_historyCursor==cursor+1 && editor.GetSelectedObject()==1 &&
        editor.m_objectInspectionIdentity && editor.m_objectInspectionIdentity->objectID==first.id,
        "Duplicate hierarchy/viewport/gizmo primary selection or history failed.");
    CheckDuplicate(owner.GetScene().GetRenderObjects()[1].mesh==originalMesh &&
        owner.GetScene().GetRenderObjects()[1].slotMaterials==originalSlots && SameObject(snapshot(original.id),original),
        "Underlying asset bindings were cloned/changed or original mutated.");
    shortcut(ImGuiKey_D,true,false,false); // No new key-down event while held.
    CheckDuplicate(!editor.m_duplicationRequest && owner.GetScene().GetRenderObjects().size()==count+1,"Held Ctrl+D repeated.");
    shortcut(ImGuiKey_D,false); shortcut(ImGuiKey_D,true); settle();
    const auto second=owner.m_runtime->GetFileScene().GetDocument().objects[2];
    CheckDuplicate(second.name=="Cat (4)" && second.id!=first.id,"Repeated duplication identity/name sequence failed.");
    shortcut(ImGuiKey_D,false);
    auto history=[&](ImGuiKey key) { shortcut(key,true); settle(); shortcut(key,false); };
    history(ImGuiKey_Z); CheckDuplicate(!owner.GetObjectMetadata(second.id) && editor.GetSelectedObject()==1,"Undo second duplicate failed.");
    history(ImGuiKey_Z); CheckDuplicate(!owner.GetObjectMetadata(first.id) && editor.GetSelectedObject()==0,"Undo first duplicate failed.");
    history(ImGuiKey_Y); CheckDuplicate(SameObject(snapshot(first.id),first) && editor.GetSelectedObject()==1,"Redo changed duplicate identity or authored state.");
    history(ImGuiKey_Y); CheckDuplicate(SameObject(snapshot(second.id),second) && editor.GetSelectedObject()==2,"Redo numbered duplicate failed.");
    // Ordinary single deletion and its undo use the same coordinator.
    const auto selected=owner.GetObjectMetadata(editor.GetSelectedObject());
    CheckDuplicate(selected && engine.BeginEditorMembershipDeletion(*selected,error),error);
    settle(); CheckDuplicate(!owner.GetObjectMetadata(second.id),"Ordinary deletion failed.");
    history(ImGuiKey_Z); CheckDuplicate(SameObject(snapshot(second.id),second),"Deletion undo restoration failed.");
    CheckDuplicate(renderer.m_rayTracingScene.m_blasRegistry.bottomLevels.size()==blasCount &&
        renderer.m_commandContext.GetGPUDrainCount()==drains,"Duplication/deletion/undo/redo drain count "+std::to_string(drains)+" -> "+std::to_string(renderer.m_commandContext.GetGPUDrainCount())+
        "; BLAS count "+std::to_string(blasCount)+" -> "+std::to_string(renderer.m_rayTracingScene.m_blasRegistry.bottomLevels.size()));
    CheckDuplicate(owner.m_runtime->Save(&error),error);
    SceneDocument saved;
    CheckDuplicate(LoadSceneDocument(owner.GetScenePath(),saved,&error),error);
    const auto found=std::find_if(saved.objects.begin(),saved.objects.end(),[&](const auto& object) { return object.id==second.id; });
    CheckDuplicate(found!=saved.objects.end() && SameObject(*found,second),"Save/reload lost duplicate authored state.");
    const auto output=std::filesystem::absolute("build/duplication-e1b/Game");
    CheckDuplicate(engine.StartBuildGame({"E1B Game",output,false},error),error);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(120);
    while(engine.m_buildGame.Poll().busy)
    { CheckDuplicate(std::chrono::steady_clock::now()<deadline,"Build Game timed out."); engine.Frame(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    CheckDuplicate(engine.m_buildGame.Poll().success,engine.m_buildGame.Poll().message);
    const auto archive=CookedScene::OpenArchive(output/"Game.toc",output/"Game.eap",BuildGame::SceneIdentity(saved.id));
    const auto cooked=std::find_if(archive->metadata.document.objects.begin(),archive->metadata.document.objects.end(),[&](const auto& object) { return object.id==second.id; });
    CheckDuplicate(cooked!=archive->metadata.document.objects.end() && SameObject(*cooked,second),"Build Game lost duplicate references or transform.");
    Log::Info("Ctrl+D passed: naming/collisions, authored state, shared bindings/BLAS, primary selection/gizmo identity, pending undo lock, held-key suppression, stable undo/redo, non-draining deletion/restoration, save/reload and Build Game archive; zero membership GPU drains.");
}
static void RunPending(Engine& engine,FileBackedDemoScene& owner)
{
    auto& editor=engine.m_editor; auto& renderer=engine.m_renderer;
    editor.UpdateSceneHistorySession(owner,ProjectPaths(owner.GetProjectRoot()),std::string(engine.m_demoSceneManager.GetActiveSceneName()));
    editor.PublishRestoredObjectSelection(owner,owner.GetObjectMetadata(std::size_t{0}),
        std::string(engine.m_demoSceneManager.GetActiveSceneName()),false);
    const auto count=owner.GetScene().GetRenderObjects().size();
    CheckDuplicate(!owner.GetScene().HasRenderableMeshes(),"Expected metadata-only pending geometry.");
    const auto drains=renderer.m_commandContext.GetGPUDrainCount();
    auto key=[&](ImGuiKey code,bool down) {
        auto& io=ImGui::GetIO(); io.AddKeyEvent(code,down); renderer.GetDebugUI().BeginFrame(); io.KeyCtrl=true;
        io.WantCaptureKeyboard=false; editor.m_viewportFocused=true; editor.m_viewportImage.size=ImVec2(640,480);
        if(code==ImGuiKey_D) editor.HandleSceneObjectDuplication(owner); else editor.HandleTransformUndo(owner);
        CheckDuplicate(renderer.Render(owner.GetScene(),RendererOptions{})!=Renderer::FrameRenderResult::Error,"Pending frame failed.");
    };
    editor.m_selectedObject=Editor::InvalidObjectSelection;
    key(ImGuiKey_D,true); CheckDuplicate(!editor.m_duplicationRequest,"No-selection shortcut created an object."); key(ImGuiKey_D,false);
    editor.SelectSceneObject(owner,0);
    key(ImGuiKey_D,true);
    auto request=editor.TakeSceneDuplicationRequest(); CheckDuplicate(request.has_value(),"Pending geometry duplicate was not queued.");
    const auto id=request->object.id; std::string error;
    CheckDuplicate(engine.BeginEditorMembership(*request,false,error),error);
    auto settle=[&]() {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(engine.m_pendingEditorMembership)
        {
            CheckDuplicate(std::chrono::steady_clock::now()<deadline,"Pending geometry transaction timed out.");
            CheckDuplicate(engine.AdvanceEditorMembership(),"Pending geometry publication failed.");
            renderer.GetDebugUI().BeginFrame();
            CheckDuplicate(renderer.Render(owner.GetScene(),RendererOptions{})!=Renderer::FrameRenderResult::Error,"Pending geometry render failed.");
        }
    };
    settle();
    CheckDuplicate(owner.GetScene().GetRenderObjects().size()==count+1 && owner.GetObjectMetadata(id) &&
        !owner.GetScene().GetRenderObjects()[1].mesh,"Pending duplicate lost its authored identity or cloned geometry.");
    key(ImGuiKey_D,false); key(ImGuiKey_Z,true);
    auto undo=editor.TakeSceneHistoryReplayRequest(); CheckDuplicate(undo.has_value(),"Pending duplicate undo missing.");
    CheckDuplicate(engine.BeginEditorMembership(*undo,true,error),error); settle(); key(ImGuiKey_Z,false);
    CheckDuplicate(owner.GetScene().GetRenderObjects().size()==count && !owner.GetObjectMetadata(id) &&
        renderer.m_commandContext.GetGPUDrainCount()==drains,"Pending duplicate/undo drained or failed.");
    Log::Info("Metadata-only reload preserved saved duplicates; no-selection and pending-mesh duplication/undo passed with zero GPU drains.");
}
};
bool Engine::VerifySceneDuplication(const std::filesystem::path& path)
{
    ImGui::GetIO().IniFilename=nullptr;
    std::string error;
    CheckDuplicate(m_demoSceneManager.RequestSynchronousFileSceneDiagnostic(std::filesystem::absolute(path),&error),error);
    CheckDuplicate(ActivateRequestedFileScene(),"Cannot activate duplication fixture.");
    auto* owner=dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    CheckDuplicate(owner && !owner->IsArchiveActivation(),"Expected loose editor scene.");
    CancelMeshResidencyAcceptance(); m_automaticMeshResidency={}; m_automaticResidencyIntent=0;
    SceneDuplicationVerification::Run(*this,*owner);
    const auto saved=owner->m_runtime->GetFileScene().GetDocument();
    CheckDuplicate(m_demoSceneManager.RequestMetadataOnlyFileScene(std::filesystem::absolute(path),&error),error);
    CheckDuplicate(ActivateRequestedFileScene(),"Saved scene did not reload.");
    auto* reloaded=dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    CheckDuplicate(reloaded && SceneDocumentsEqual(reloaded->m_runtime->GetFileScene().GetDocument(),saved),"Real scene reload changed duplicate authored state.");
    SceneDuplicationVerification::RunPending(*this,*reloaded);
    return true;
}
#endif
