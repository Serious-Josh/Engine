#include "Editor.h"

#include "Math/Math.h"

#include "DemoContent/IDemoScene.h"
#include "DemoContent/SphereGridScene.h"
#include "DemoContent/StressTestScene.h"
#include "DemoContent/DemoSceneManager.h"

#include "Renderer/Mesh.h"

#include "Scene/DirectionalLight.h"
#include "Scene/Scene.h"

#include "imgui.h"
#include "ImGuizmo.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    struct PickingRay
    {
        Vector3 origin;
        Vector3 direction;
        float maxDistance = 0.0f;
    };

    Vector3 TransformPoint(const Matrix4& matrix, const Vector3& point)
    {
        return {
            matrix.m[0][0] * point.x + matrix.m[0][1] * point.y +
                matrix.m[0][2] * point.z + matrix.m[0][3],

            matrix.m[1][0] * point.x + matrix.m[1][1] * point.y +
                matrix.m[1][2] * point.z + matrix.m[1][3],

            matrix.m[2][0] * point.x + matrix.m[2][1] * point.y +
                matrix.m[2][2] * point.z + matrix.m[2][3]
        };
    }

    Vector3 TransformDirection(const Matrix4& matrix, const Vector3& direction)
    {
        return {
            matrix.m[0][0] * direction.x + matrix.m[0][1] * direction.y +
                matrix.m[0][2] * direction.z,

            matrix.m[1][0] * direction.x + matrix.m[1][1] * direction.y +
                matrix.m[1][2] * direction.z,

            matrix.m[2][0] * direction.x + matrix.m[2][1] * direction.y +
                matrix.m[2][2] * direction.z
        };
    }

    bool UnprojectPoint(
        const Matrix4& inverseViewProjection,
        float x, float y, float z,
        Vector3& result)
    {
        const float w =
            inverseViewProjection.m[3][0] * x +
            inverseViewProjection.m[3][1] * y +
            inverseViewProjection.m[3][2] * z +
            inverseViewProjection.m[3][3];

        if(!std::isfinite(w) || std::abs(w) < 0.000001f){
            return false;
        }

        const float inverseW = 1.0f / w;

        result = {
            (inverseViewProjection.m[0][0] * x +
             inverseViewProjection.m[0][1] * y +
             inverseViewProjection.m[0][2] * z +
             inverseViewProjection.m[0][3]) * inverseW,

            (inverseViewProjection.m[1][0] * x +
             inverseViewProjection.m[1][1] * y +
             inverseViewProjection.m[1][2] * z +
             inverseViewProjection.m[1][3]) * inverseW,

            (inverseViewProjection.m[2][0] * x +
             inverseViewProjection.m[2][1] * y +
             inverseViewProjection.m[2][2] * z +
             inverseViewProjection.m[2][3]) * inverseW
        };

        return std::isfinite(result.x) &&
               std::isfinite(result.y) &&
               std::isfinite(result.z);
    }

    bool BuildPickingRay(
        const Camera& camera,
        float mouseX, float mouseY,
        float viewportWidth, float viewportHeight,
        PickingRay& ray)
    {
        if(viewportWidth <= 0.0f || viewportHeight <= 0.0f ||
           !std::isfinite(mouseX) || !std::isfinite(mouseY)){
            return false;
        }

        const float x = 2.0f * mouseX / viewportWidth - 1.0f;
        const float y = 1.0f - 2.0f * mouseY / viewportHeight;

        const Matrix4 viewProjection = camera.GetProjectionMatrix() * camera.GetViewMatrix();
        const Matrix4 inverseViewProjection = viewProjection.Inverse();

        Vector3 nearPoint;
        Vector3 farPoint;

        if(!UnprojectPoint(inverseViewProjection, x, y, 0.0f, nearPoint) ||
           !UnprojectPoint(inverseViewProjection, x, y, 1.0f, farPoint)){
            return false;
        }

        const Vector3 difference = farPoint - nearPoint;
        const float distance = difference.Length();

        if(!std::isfinite(distance) || distance <= 0.000001f){
            return false;
        }

        ray.origin = nearPoint;
        ray.direction = difference * (1.0f / distance);
        ray.maxDistance = distance;

        return true;
    }

    bool IntersectAABB(
        const Vector3& origin,
        const Vector3& direction,
        const MeshBounds& bounds,
        float maxDistance,
        float& hitDistance)
    {
        if(!bounds.valid){
            return false;
        }

        float tMin = 0.0f;
        float tMax = maxDistance;

        const float origins[3] = {origin.x, origin.y, origin.z};
        const float directions[3] = {direction.x, direction.y, direction.z};
        const float minimums[3] = {bounds.min.x, bounds.min.y, bounds.min.z};
        const float maximums[3] = {bounds.max.x, bounds.max.y, bounds.max.z};

        for(int axis = 0; axis < 3; ++axis)
        {
            if(std::abs(directions[axis]) < 0.00000001f)
            {
                if(origins[axis] < minimums[axis] ||
                   origins[axis] > maximums[axis]){
                    return false;
                }

                continue;
            }

            const float inverseDirection = 1.0f / directions[axis];

            float t1 = (minimums[axis] - origins[axis]) * inverseDirection;
            float t2 = (maximums[axis] - origins[axis]) * inverseDirection;

            if(t1 > t2){
                (std::swap)(t1, t2);
            }

            tMin = (std::max)(tMin, t1);
            tMax = (std::min)(tMax, t2);

            if(tMin > tMax){
                return false;
            }
        }

        hitDistance = tMin;
        return true;
    }

    bool IntersectObject(
        const PickingRay& ray,
        const RenderObject& object,
        float& hitDistance)
    {
        if(!object.mesh || !object.mesh->GetBounds().valid){
            return false;
        }

        // Reject singular transforms before calling Matrix4::Inverse(),
        // which otherwise silently returns identity.
        const Vector3& scale = object.transform.scale;

        if(!std::isfinite(scale.x) ||
           !std::isfinite(scale.y) ||
           !std::isfinite(scale.z) ||
           std::abs(scale.x * scale.y * scale.z) < 0.000001f){
            return false;
        }

        const Matrix4 inverseModel = object.transform.GetModelMatrix().Inverse();

        const Vector3 localOrigin = TransformPoint(inverseModel, ray.origin);

        // Do not normalize: preserve the world-ray parameter t.
        const Vector3 localDirection = TransformDirection(inverseModel, ray.direction);

        return IntersectAABB(
            localOrigin,
            localDirection,
            object.mesh->GetBounds(),
            ray.maxDistance,
            hitDistance);
    }

    size_t PickObject(
        const Scene& scene,
        float mouseX,
        float mouseY,
        float viewportWidth,
        float viewportHeight)
    {
        PickingRay ray{};

        if(!BuildPickingRay(scene.GetCamera(), mouseX, mouseY, viewportWidth, viewportHeight, ray)){
            return (std::numeric_limits<size_t>::max)();
        }

        const auto& objects = scene.GetRenderObjects();

        size_t closestObject = (std::numeric_limits<size_t>::max)();
        float closestDistance = ray.maxDistance;

        for(size_t i = 0; i < objects.size(); ++i){
            float hitDistance = 0.0f;

            if(IntersectObject(ray, objects[i], hitDistance) && hitDistance <= closestDistance){
                closestDistance = hitDistance;
                closestObject = i;
            }
        }

        return closestObject;
    }
}

void Editor::Draw(IDemoScene& demoScene, DemoSceneManager& sceneManager, float viewportWidth, float viewportHeight)
{
    const ImGuiIO& io = ImGui::GetIO();

    if(!io.WantCaptureKeyboard && !ImGuizmo::IsUsing() && !ImGui::IsMouseDown(ImGuiMouseButton_Right)){
        if(ImGui::IsKeyPressed(ImGuiKey_W, false)){
            m_gizmoMode = GizmoMode::Translate;
        }
        else if(ImGui::IsKeyPressed(ImGuiKey_E, false)){
            m_gizmoMode = GizmoMode::Rotate;
        }
        else if(ImGui::IsKeyPressed(ImGuiKey_R, false)){
            m_gizmoMode = GizmoMode::Scale;
        }
    }

    ImGui::Begin("Editor");

    const std::string_view activeSceneName = sceneManager.GetActiveSceneName();

    if(ImGui::BeginCombo("Demo Scene", activeSceneName.data())){
        for(const DemoSceneEntry& entry : sceneManager.GetAvailableScenes()){
            const bool isSelected = entry.name == activeSceneName;

            if(ImGui::Selectable(entry.name.c_str(), isSelected)){
                sceneManager.RequestScene(entry.name);
            }

            if(isSelected){
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    ImGui::Separator();

    if(auto* sphereGridScene = dynamic_cast<SphereGridScene*>(&demoScene))
    {
        DrawSphereGridControls(*sphereGridScene);

        ImGui::Separator();
    }

    if(auto* stressTestScene = dynamic_cast<StressTestScene*>(&demoScene)){
        DrawStressTestControls(*stressTestScene);

        ImGui::Separator();
    }

    DrawObjectControls(demoScene, sceneManager);

    ImGui::Separator();

    DrawCamera(demoScene);

    ImGui::Separator();

    DrawDirectionalLight(demoScene);

    ImGui::End();

    UpdateCameraLook(demoScene, viewportWidth, viewportHeight);

    DrawTransformGizmo(demoScene, viewportWidth, viewportHeight);

    HandleViewportSelection(demoScene, viewportWidth, viewportHeight);
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

void Editor::DrawStressTestControls(StressTestScene& demoScene)
{
    if(!ImGui::CollapsingHeader(
        "Stress Test",
        ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    constexpr unsigned int gridSizes[] =
    {
        10, 20, 30, 40, 50
    };

    constexpr const char* gridLabels[] =
    {
        "10 x 10",
        "20 x 20",
        "30 x 30",
        "40 x 40",
        "50 x 50"
    };

    const unsigned int currentGridSize = demoScene.GetGridSize();

    int selectedIndex = 0;

    for(int i = 0; i < IM_ARRAYSIZE(gridSizes); ++i){
        if(gridSizes[i] == currentGridSize){
            selectedIndex = i;
            break;
        }
    }

    if(ImGui::Combo("Grid Size", &selectedIndex, gridLabels, IM_ARRAYSIZE(gridLabels))){
        demoScene.SetGridSize(gridSizes[selectedIndex]);
    }

    ImGui::Text("Sphere instances: %u", currentGridSize * currentGridSize);
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

void Editor::DrawObjectControls(IDemoScene& demoScene, DemoSceneManager& sceneManager)
{
    Scene& scene = demoScene.GetScene();

    const std::string activeSceneName(sceneManager.GetActiveSceneName());
    const uint64_t structureRevision = scene.GetStructureRevision();

    if(m_selectionSceneName != activeSceneName || m_selectionStructureRevision != structureRevision){
        m_selectedObject = InvalidObjectSelection;
        m_selectionSceneName = activeSceneName;
        m_selectionStructureRevision = structureRevision;
    }

    const Scene& constScene = scene;
    const auto& objects = constScene.GetRenderObjects();

    if(m_selectedObject >= objects.size()){
        m_selectedObject = InvalidObjectSelection;
    }

    if(!ImGui::CollapsingHeader("Objects", ImGuiTreeNodeFlags_DefaultOpen)){
        return;
    }

    const std::string preview = m_selectedObject == InvalidObjectSelection ? "None" : "Object " + std::to_string(m_selectedObject);

    if(ImGui::BeginCombo("Selected Object", preview.c_str())){
        const bool noneSelected = m_selectedObject == InvalidObjectSelection;

        if(ImGui::Selectable("None", noneSelected)){
            m_selectedObject = InvalidObjectSelection;
        }

        for(size_t i = 0; i < objects.size(); ++i){
            const std::string label = "Object " + std::to_string(i);

            const bool selected = m_selectedObject == i;

            if(ImGui::Selectable(label.c_str(), selected)){
                m_selectedObject = i;
            }

            if(selected){
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    ImGui::Text("Objects: %zu", objects.size());

    if(m_selectedObject == InvalidObjectSelection){
        return;
    }

    Transform transform = objects[m_selectedObject].transform;
    bool transformChanged = false;

    ImGui::Separator();
    ImGui::PushID("SelectedObjectTransform");

    transformChanged |= ImGui::DragFloat3("Position", &transform.position.x, 0.05f);

    float rotationDegrees[3] =
    {
        ToDegrees(transform.rotation.x),
        ToDegrees(transform.rotation.y),
        ToDegrees(transform.rotation.z)
    };

    if(ImGui::DragFloat3("Rotation", rotationDegrees, 0.05)){
        transform.rotation = Vector3{
            ToRadians(rotationDegrees[0]),
            ToRadians(rotationDegrees[1]),
            ToRadians(rotationDegrees[2])
        };

        transformChanged = true;
    }

    if(transformChanged |= ImGui::DragFloat3("Scale", &transform.scale.x, 0.01f, 0.01f, 100.0f)){
        transform.scale.x = (std::max)(0.01f, transform.scale.x);
        transform.scale.y = (std::max)(0.01f, transform.scale.y);
        transform.scale.z = (std::max)(0.01f, transform.scale.z);

        transformChanged = true;
    }

    ImGui::PopID();

    if(transformChanged){
        scene.SetRenderObjectTransform(m_selectedObject, transform);
    }
}

void Editor::DrawTransformGizmo(IDemoScene& demoScene, float viewportWidth, float viewportHeight)
{
    if(m_selectedObject == InvalidObjectSelection || viewportWidth <= 0.0f || viewportHeight <= 0.0f){
        return;
    }

    Scene& scene = demoScene.GetScene();

    const Scene& constScene = scene;
    const auto& objects = constScene.GetRenderObjects();

    if(m_selectedObject >= objects.size()){
        return;
    }

    Transform transform = objects[m_selectedObject].transform;

    const Camera& camera = scene.GetCamera();

    Matrix4 view = camera.GetViewMatrix().Transpose();
    Matrix4 projection = camera.GetProjectionMatrix().Transpose();
    Matrix4 model = transform.GetModelMatrix().Transpose();

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(viewportWidth, viewportHeight));

    constexpr ImGuiWindowFlags viewportFlags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoInputs;

    ImGui::Begin("##GizmoViewport", nullptr, viewportFlags);

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(0.0f, 0.0f, viewportWidth, viewportHeight);

    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;

    switch(m_gizmoMode){
        case GizmoMode::Translate:
            operation = ImGuizmo::TRANSLATE;
            break;
        case GizmoMode::Rotate:
            operation = ImGuizmo::ROTATE;
            break;
        case GizmoMode::Scale:
            operation = ImGuizmo::SCALE;
            break;
    }

    ImGuizmo::Manipulate(&view.m[0][0], &projection.m[0][0], operation, ImGuizmo::WORLD, &model.m[0][0]);

    ImGui::End();

    if(ImGuizmo::IsUsing())
    {
        float translation[3];
        float rotationDegrees[3];
        float scale[3];

        ImGuizmo::DecomposeMatrixToComponents(
            &model.m[0][0],
            translation,
            rotationDegrees,
            scale);

        bool changed = false;

        switch(m_gizmoMode)
        {
            case GizmoMode::Translate:
            {
                const Vector3 newPosition{
                    translation[0],
                    translation[1],
                    translation[2]
                };

                if(newPosition.x != transform.position.x ||
                newPosition.y != transform.position.y ||
                newPosition.z != transform.position.z)
                {
                    transform.position = newPosition;
                    changed = true;
                }

                break;
            }

            case GizmoMode::Rotate:
            {
                const Vector3 newRotation{
                    ToRadians(rotationDegrees[0]),
                    ToRadians(rotationDegrees[1]),
                    ToRadians(rotationDegrees[2])
                };

                transform.rotation = newRotation;
                changed = true;
                break;
            }

            case GizmoMode::Scale:
            {
                transform.scale = Vector3{
                    (std::max)(0.01f, scale[0]),
                    (std::max)(0.01f, scale[1]),
                    (std::max)(0.01f, scale[2])
                };

                changed = true;
                break;
            }
        }

        if(changed){
            scene.SetRenderObjectTransform(m_selectedObject, transform);
        }
    }
}

void Editor::HandleViewportSelection(IDemoScene& demoScene, float viewportWidth, float viewportHeight)
{
    if(!ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();

    if(io.WantCaptureMouse || ImGui::IsAnyItemActive() || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) || ImGuizmo::IsUsing()){
        return;
    }

    if(m_selectedObject != InvalidObjectSelection && ImGuizmo::IsOver()){
        return;
    }

    const ImVec2 mouse = io.MousePos;

    if(mouse.x < 0.0f || mouse.y < 0.0f ||
       mouse.x >= viewportWidth ||
       mouse.y >= viewportHeight)
    {
        return;
    }

    const Scene& scene = demoScene.GetScene();

    m_selectedObject = PickObject(scene, mouse.x, mouse.y, viewportWidth, viewportHeight);
}

void Editor::UpdateCameraLook(IDemoScene& demoScene, float viewportWidth, float viewportHeight)
{
    const ImGuiIO& io = ImGui::GetIO();

    if(!ImGui::IsMouseDown(ImGuiMouseButton_Right)){

        if(m_cameraLooking){
            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        }

        m_cameraLooking = false;
        return;
    }

    if(!m_cameraLooking){
        if(io.WantCaptureMouse ||
            io.MousePos.x < 0.0f ||
            io.MousePos.y < 0.0f ||
            io.MousePos.x >= viewportWidth ||
            io.MousePos.y >= viewportHeight ||
            ImGuizmo::IsUsing())
        {
            return;
        }

        Camera& camera = demoScene.GetScene().GetCamera();

        const Vector3 direction = camera.GetTarget() - camera.GetPosition();
        const float horizontalLength = std::sqrt(direction.x * direction.x + direction.z * direction.z);

        m_cameraYaw = std::atan2(direction.x, direction.z);
        m_cameraPitch = std::atan2(direction.y, horizontalLength);

        m_cameraLooking = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }

    ImGui::SetMouseCursor(ImGuiMouseCursor_None);

    constexpr float sensitivity = 0.003f;

    m_cameraYaw += io.MouseDelta.x * sensitivity;
    m_cameraPitch -= io.MouseDelta.y * sensitivity;

    m_cameraPitch = (std::clamp)(m_cameraPitch, -1.55334f, 1.55334f);

    const Vector3 direction{
        std::sin(m_cameraYaw) * std::cos(m_cameraPitch),
        std::sin(m_cameraPitch),
        std::cos(m_cameraYaw) * std::cos(m_cameraPitch)
    };

    Camera& camera = demoScene.GetScene().GetCamera();

    camera.SetTarget(camera.GetPosition() + direction);
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