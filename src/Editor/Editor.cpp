#include "Editor.h"

#include "DemoContent/IDemoScene.h"
#include "DemoContent/SphereGridScene.h"
#include "Scene/DirectionalLight.h"
#include "Scene/Scene.h"
#include "DemoContent/DemoSceneManager.h"

#include "imgui.h"

void Editor::Draw(IDemoScene& demoScene, DemoSceneManager& sceneManager, SphereGridScene* sphereGridScene)
{
    ImGui::Begin("Editor");

    const char* sceneNames[] =
    {
        "Sphere Grid",
        "Mirror Test"
    };

    int selectedScene = static_cast<int>(sceneManager.GetActiveSceneId());

    if(ImGui::Combo("Demo Scene", &selectedScene, sceneNames, IM_ARRAYSIZE(sceneNames)))
    {
        sceneManager.RequestScene(static_cast<DemoSceneId>(selectedScene));
    }

    ImGui::Separator();

    if(sphereGridScene)
    {
        DrawSphereGridControls(*sphereGridScene);

        ImGui::Separator();
    }

    DrawCamera(demoScene);

    ImGui::Separator();

    DrawDirectionalLight(demoScene);

    ImGui::End();
}

void Editor::DrawSphereGridControls(SphereGridScene& demoScene)
{
    if(!ImGui::CollapsingHeader("Diagnostics", ImGuiTreeNodeFlags_DefaultOpen)){
        return;
    }

    const char* modeNames[] =
    {
        "None",
        "Rotate Light",
        "Move Sphere",
        "Orbit Camera"
    };

    int selectedMode = static_cast<int>(demoScene.GetUpdateMode());

    if(ImGui::Combo("Update Mode", &selectedMode, modeNames, IM_ARRAYSIZE(modeNames))){
        demoScene.SetUpdateMode(static_cast<DemoUpdateMode>(selectedMode));
    }

    DemoUpdateMode mode = demoScene.GetUpdateMode();

    if(mode == DemoUpdateMode::RotateLight)
    {
        float speed =
            demoScene.GetLightRotationSpeed();

        if(ImGui::DragFloat(
            "Rotation Speed",
            &speed,
            0.05f,
            0.0f,
            10.0f,
            "%.2f"))
        {
            demoScene.SetLightRotationSpeed(speed);
        }

        float verticalAngleDegrees =
            demoScene.GetLightVerticalAngle() *
            (180.0f / 3.14159265358979323846f);

        if(ImGui::SliderFloat(
            "Light Angle",
            &verticalAngleDegrees,
            0.0f,
            90.0f,
            "%.1f deg"))
        {
            demoScene.SetLightVerticalAngle(
                ToRadians(verticalAngleDegrees));
        }
    }
    else if(mode == DemoUpdateMode::OrbitCamera)
    {
        float speed = demoScene.GetCameraOrbitSpeed();

        if(ImGui::DragFloat("Orbit Speed", &speed, 0.05f, 0.0f, 10.0f, "%.2f"))
        {
            demoScene.SetCameraOrbitSpeed(speed);
        }
    }
}

void Editor::DrawDirectionalLight(IDemoScene& demoScene)
{
    if(!ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen)){
        return;
    }

    DirectionalLight& light = demoScene.GetScene().GetDirectionalLight();

    ImGui::DragFloat3("Direction", &light.direction.x, 0.01f, -1.0f, 1.0f);
    ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0.0f, 100.0f);
    ImGui::DragFloat("Ambient Strength", &light.ambientStrength, 0.01f, 0.0f, 1.0f);
    
    float angularRadiusDegrees = light.angularRadius * (180.0f / 3.14159265358979323846f);

    if(ImGui::DragFloat("Angular Radius", &angularRadiusDegrees, 0.01f, 0.0f, 90.0f, "%.2f deg")){
        light.angularRadius = ToRadians(angularRadiusDegrees);
    }

    if(ImGui::Button("Default##DirectionalLight")){
        demoScene.ResetDirectionalLight();
    }
}

void Editor::DrawCamera(IDemoScene& demoScene)
{
    if(!ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    Camera& camera = demoScene.GetScene().GetCamera();

    Vector3 position = camera.GetPosition();

    if(ImGui::DragFloat3("Position", &position.x, 0.05f))
    {
        camera.SetPosition(position);
    }

    Vector3 target = camera.GetTarget();

    if(ImGui::DragFloat3("Target", &target.x, 0.05f))
    {
        camera.SetTarget(target);
    }

    if(ImGui::Button("Default##Camera")){
        demoScene.ResetCamera();
    }
}