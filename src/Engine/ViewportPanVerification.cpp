#include "Engine/Engine.h"
#ifndef NDEBUG
#include "Editor/ViewportCameraInput.h"
#include "Core/Log.h"
#include "Project/ProjectLocator.h"
#include "Project/ProjectPaths.h"
#include "ImGuizmo.h"
#include <stdexcept>

namespace
{
void CheckPan(bool condition, const char* message)
{
    if(!condition) throw std::runtime_error(message);
}
bool Equal(const Vector3& a, const Vector3& b)
{
    return a.x==b.x && a.y==b.y && a.z==b.z;
}
bool Equal(const Transform& a, const Transform& b)
{
    return Equal(a.position,b.position) && Equal(a.rotation,b.rotation) && Equal(a.scale,b.scale);
}
}
class ViewportPanVerification
{
public:
static void Run(Engine& engine)
{
    auto& editor=engine.m_editor;
    auto& demo=engine.m_demoSceneManager.GetActiveScene();
    auto& scene=demo.GetScene();
    const auto& objects=static_cast<const Scene&>(scene).GetRenderObjects();
    CheckPan(!objects.empty(), "Diagnostic requires an object.");
    const auto original=objects[0].transform;
    CheckPan(editor.UpdateSceneHistorySession(demo,ProjectPaths(ProjectLocator::GetDefaultProjectRoot()),
        std::string(engine.m_demoSceneManager.GetActiveSceneName())),"Could not initialize diagnostic history session.");
    auto& io=ImGui::GetIO();
    io.IniFilename=nullptr;
    io.ConfigInputTrickleEventQueue=false;
    engine.m_renderer.GetDebugUI().BeginFrame(); ImGui::EndFrame();
    const auto window=static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
    float mouseX=300;
    auto button=[&](bool right,bool down) {
        SendMessageW(window,right ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP) :
            (down ? WM_LBUTTONDOWN : WM_LBUTTONUP),0,0);
    };
    auto frame=[&](bool move) {
        io.AddMousePosEvent(mouseX,300); if(move) mouseX+=10;
        ImGui::NewFrame(); ImGuizmo::BeginFrame();
        ViewportCameraInput::ReserveLeftButton(editor.m_cameraLooking,editor.m_panLeftHeld);
        editor.m_viewportHovered=true;
        editor.m_viewportFocused=ViewportCameraInput::UpdateFocus(editor.m_viewportFocused,
            editor.m_cameraLooking,true);
        editor.m_viewportImage={{0,0},{800,600}};
        editor.UpdateCameraLook(demo,800,600);
        editor.DrawTransformGizmo(demo,800,600);
        // Exercise picking suppression during the chord and its retained LMB tail.
        if(editor.m_cameraLooking || editor.m_panLeftHeld)
            editor.HandleViewportSelection(demo,800,600);
        editor.FinishViewportRightClick(demo);
        ImGui::EndFrame();
    };
    for(bool selected : {false,true})
    for(bool rightFirst : {false,true})
    for(bool rightReleaseFirst : {false,true})
    {
        editor.ResetTransformGestures();
        editor.SelectSceneObject(demo,selected ? 0 : Editor::InvalidObjectSelection);
        const auto selection=editor.m_selectedObject;
        const auto group=editor.m_selectedObjects;
        const auto history=editor.m_historyCursor;
        const auto historySize=editor.m_sceneHistory.size();
        frame(false);
        button(rightFirst,true); frame(false);
        if(selected && !rightFirst)
        {
            // Reproduce an already active single-object gizmo gesture. The chord
            // must restore this authored baseline without adding an undo command.
            editor.m_gizmoWasUsing=true;
            editor.m_gizmoUndoStart=original;
            editor.m_gizmoHistoryTarget=editor.CaptureHistoryTarget(demo,0);
            CheckPan(editor.m_gizmoHistoryTarget.has_value(),"Missing diagnostic gizmo target.");
            editor.m_gizmoHistoryEpoch=editor.m_historyEpoch;
            auto moved=original; moved.position.x+=1;
            scene.SetRenderObjectTransform(0,moved);
        }
        button(!rightFirst,true); frame(false);
        CheckPan(editor.m_cameraLooking && editor.m_cameraPanning,"Chord failed to acquire camera navigation.");
        const auto position=scene.GetCamera().GetPosition();
        for(int i=0;i<6;++i) frame(true);
        CheckPan(!Equal(position,scene.GetCamera().GetPosition()),"Panning did not move camera.");
        CheckPan(Equal(objects[0].transform,original),"Pan modified object transform or failed gesture rollback.");
        CheckPan(!ImGuizmo::IsUsing() && !editor.m_gizmoWasUsing,"Pan retained an active gizmo.");
        button(rightReleaseFirst,false); frame(true);
        CheckPan(ViewportCameraInput::HasCapture(),"First release lost native capture.");
        if(rightReleaseFirst)
        {
            CheckPan(!editor.m_cameraLooking && editor.m_panLeftHeld,"LMB tail was not reserved.");
            const auto tailPosition=scene.GetCamera().GetPosition();
            frame(true);
            CheckPan(Equal(tailPosition,scene.GetCamera().GetPosition()),"LMB tail moved camera.");
        }
        else CheckPan(editor.m_cameraLooking && !editor.m_cameraPanning,"RMB navigation did not resume.");
        button(!rightReleaseFirst,false); frame(false);
        CheckPan(!ViewportCameraInput::HasCapture() && !editor.m_cameraLooking && !editor.m_panLeftHeld,
            "Final release left capture or navigation stuck.");
        CheckPan(editor.m_selectedObject==selection && editor.m_selectedObjects==group,
            "Pan changed selection.");
        CheckPan(editor.m_historyCursor==history && editor.m_sceneHistory.size()==historySize,
            "Pan created undo history.");
        CheckPan(Equal(objects[0].transform,original),"Release changed object transform.");
    }
    Log::Info("Viewport pan passed: selected/unselected, both press and release orders, camera motion, gizmo gesture rollback, unchanged transforms/selection/history, native capture retention and final release.");
}
};
bool Engine::VerifyViewportPan()
{
    ViewportPanVerification::Run(*this);
    return true;
}
#endif
