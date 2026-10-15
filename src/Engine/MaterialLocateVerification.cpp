#include "Engine/Engine.h"
#ifndef NDEBUG
#include "Editor/EditorWorkspace.h"
#include "assets/ProjectAssetResolver.h"
#include "Scene/SceneDocumentAdapter.h"
#include "Core/Log.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <fstream>
#include <stdexcept>
namespace
{
void CheckLocate(bool value,const std::string& message) { if(!value) throw std::runtime_error("Material locate: "+message); }
}
class MaterialLocateVerification
{
public:
static void Run(Engine& engine)
{
    const auto root=std::filesystem::absolute("build/material-locate-e2/Project");
    std::filesystem::create_directories(root/"Materials/Nested");
    ProjectPaths paths(root); ProjectAssetRegistry registry;
    const auto id=*AssetID::Generate();
    const auto target=root/"Materials/Nested/ZTarget.material";
    { std::ofstream f(target); f << "{}"; }
    CheckLocate(registry.Register({id,AssetType::Material,"Materials/Nested/ZTarget.material"}),"Fixture registration failed.");
    for(int i=0;i<80;++i) { std::ofstream f(root/"Materials/Nested"/("A"+std::to_string(i)+".txt")); f << "fixture"; }
    auto& editor=engine.m_editor;
    // Initialize backend display/font state once; subsequent synthetic input frames
    // bypass Win32 polling so the real desktop cursor cannot overwrite test input.
    engine.m_renderer.GetDebugUI().BeginFrame();
    ImGui::EndFrame();
    AssetBrowser browser; browser.ConfigureMaterialThumbnails(engine.m_renderer);
    browser.m_root=root; browser.m_directory=root; browser.SetThumbnailProject(paths,registry);
    const auto selection=root/"retained.selection";
    browser.RestoreSelection(selection);
    InspectorPanel inspector;
    auto& active=engine.m_demoSceneManager.GetActiveScene();
    if(active.GetScene().GetRenderObjects().size()>1)
    {
        editor.SelectSceneObject(active,0);
        editor.SelectSceneObject(active,1,true);
        editor.m_inspectorTarget=Editor::InspectorTarget::SceneObject;
    }
    const auto selectedObjects=editor.m_selectedObjects;
    const auto selected=editor.m_selectedObject;
    const auto ordered=editor.m_orderedSelection;
    const auto targetKind=editor.m_inspectorTarget;
    const auto cursor=editor.m_historyCursor;
    const auto before=engine.m_demoSceneManager.GetActiveScene().GetScene().GetStructureRevision();
    auto item=[&](bool click) {
        auto& io=ImGui::GetIO(); io.ConfigInputTrickleEventQueue=false;
        io.AddMousePosEvent(90,90); io.AddMouseButtonEvent(ImGuiMouseButton_Right,click);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(20,20)); ImGui::SetNextWindowSize(ImVec2(250,150));
        ImGui::Begin("Locate fixture",nullptr,ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetCursorScreenPos(ImVec2(60,70)); ImGui::Button("Assigned material",ImVec2(150,40));
        inspector.HandleMaterialLocateItem(id.ToReference());
        ImGui::End(); ImGui::EndFrame();
    };
    item(false); item(false); item(true);
    const auto request=inspector.TakeMaterialLocateRequest();
    CheckLocate(request && *request==id.ToReference(),"Right-click did not emit the assigned stable identity.");
    CheckLocate(!inspector.TakeSlotAssignmentRequest(),"Right-click emitted a material assignment.");
    std::string error;
    CheckLocate(browser.RevealMaterial(id,paths,registry,error),error);
    CheckLocate(browser.m_directory==target.parent_path() && browser.GetNavigationHighlight()==id &&
        browser.GetSelection()==selection && !browser.TakeSelectionIntent(),"Reveal changed selection or failed navigation/highlight.");
    auto drawBrowser=[&]() {
        auto& io=ImGui::GetIO(); io.AddMouseButtonEvent(ImGuiMouseButton_Right,false);
        ImGui::NewFrame();
        browser.Draw({ImVec2(0,0),ImVec2(700,250)});
        ImGui::EndFrame();
    };
    drawBrowser(); drawBrowser(); drawBrowser();
    bool scrolled=false;
    for(auto* window:ImGui::GetCurrentContext()->Windows)
        if(std::string_view(window->Name).find("ContentGrid")!=std::string_view::npos && window->Scroll.y>0) scrolled=true;
    CheckLocate(scrolled && !browser.m_revealPending,"Target did not request/receive scrolling into view.");
    CheckLocate(browser.RevealMaterial(id,paths,registry,error) && browser.m_revealPending,"Same-directory reveal was not repeated.");
    const auto previousDirectory=browser.m_directory;
    const auto missing=*AssetID::Generate();
    CheckLocate(!browser.RevealMaterial(missing,paths,registry,error) && !error.empty() &&
        browser.m_directory==previousDirectory && browser.GetNavigationHighlight()==id,"Unregistered target changed navigation.");
    std::filesystem::remove(target);
    CheckLocate(!browser.RevealMaterial(id,paths,registry,error) && browser.m_directory==previousDirectory &&
        browser.GetSelection()==selection,"Missing material changed navigation/selection.");
    // Re-register the SAME identity at its current folder, without retaining a slot path.
    ProjectAssetRegistry moved;
    std::filesystem::create_directories(root/"Materials/Moved");
    const auto destination=root/"Materials/Moved/ZTarget.material";
    { std::ofstream f(destination); f << "{}"; }
    CheckLocate(moved.Register({id,AssetType::Material,"Materials/Moved/ZTarget.material"}),"Moved fixture registration failed.");
    browser.SetThumbnailProject(paths,moved);
    CheckLocate(browser.RevealMaterial(id,paths,moved,error) && browser.m_directory==destination.parent_path() &&
        browser.GetSelection()==selection,"Moved material did not resolve current directory.");
    browser.Navigate(root);
    CheckLocate(!browser.GetNavigationHighlight(),"User navigation did not clear highlight.");
    // Exercise Editor request consumption, including showing the existing hidden panel.
    const auto oldSelection=editor.m_assetBrowser.GetSelection();
    // Use a value copy: GetAssets may return temporary vectors.
    const auto materials=engine.m_projectAssetRegistry.GetAssets(AssetType::Material);
    if(!materials.empty())
    {
        editor.m_workspace.AssetBrowserVisible()=false;
        editor.m_inspectorPanel.m_materialLocateRequest=materials.front().id.ToReference();
        editor.ProcessMaterialLocateRequest(ProjectPaths(engine.m_editor.m_assetBrowser.m_root),engine.m_projectAssetRegistry);
        CheckLocate(editor.m_workspace.AssetBrowserVisible() && editor.m_assetBrowser.GetSelection()==oldSelection &&
            !editor.m_assetBrowser.TakeSelectionIntent(),"Editor reveal selected an asset or failed to show panel.");
    }
    CheckLocate(editor.m_selectedObject==selected && editor.m_orderedSelection.size()==ordered.size() && editor.m_selectedObjects==selectedObjects &&
        editor.m_inspectorTarget==targetKind && editor.m_historyCursor==cursor &&
        engine.m_demoSceneManager.GetActiveScene().GetScene().GetStructureRevision()==before,
        "Locate changed scene selection, inspection or history.");
    Log::Info("Assigned material locate passed: right-click identity request, separate stable-ID highlight, scroll/reveal, same-folder repeat, selection preservation, moved identity, missing/unregistered rejection and existing panel visibility.");
}
};
bool Engine::VerifyMaterialLocate()
{
    ImGui::GetIO().IniFilename=nullptr;
    MaterialLocateVerification::Run(*this);
    return true;
}
#endif
