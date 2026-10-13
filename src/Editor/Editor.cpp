#include "assets/ProjectAssetRename.h"
#include "assets/ProjectAssetDeletion.h"
#include "Core/Log.h"
#include "Editor.h"
#include "Debug/DebugUI.h"

#include "Math/Math.h"

#include "DemoContent/IDemoScene.h"
#include "DemoContent/SphereGridScene.h"
#include "DemoContent/StressTestScene.h"
#include "DemoContent/DemoSceneManager.h"
#include "DemoContent/FileBackedDemoScene.h"

#include "Project/ProjectLocator.h"
#include "Project/ProjectPaths.h"

#include "Renderer/Mesh.h"

#include "Scene/DirectionalLight.h"
#include "Scene/Scene.h"

#include "imgui.h"
#include "ImGuizmo.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace
{
    struct PickingRay
    {
        Vector3 origin;
        Vector3 direction;

        float maxDistance = 0.0f;
    };

    Vector3 TransformPoint(
        const Matrix4& matrix,
        const Vector3& point)
    {
        return {matrix.m[0][0] * point.x + matrix.m[0][1] * point.y + matrix.m[0][2] * point.z + matrix.m[0][3],
                matrix.m[1][0] * point.x + matrix.m[1][1] * point.y + matrix.m[1][2] * point.z + matrix.m[1][3],
                matrix.m[2][0] * point.x + matrix.m[2][1] * point.y + matrix.m[2][2] * point.z + matrix.m[2][3]};
    }

    Vector3 TransformDirection(
        const Matrix4& matrix,
        const Vector3& direction)
    {
        return {matrix.m[0][0] * direction.x + matrix.m[0][1] * direction.y + matrix.m[0][2] * direction.z,
                matrix.m[1][0] * direction.x + matrix.m[1][1] * direction.y + matrix.m[1][2] * direction.z,
                matrix.m[2][0] * direction.x + matrix.m[2][1] * direction.y + matrix.m[2][2] * direction.z};
    }

    bool UnprojectPoint(
        const Matrix4& inverseViewProjection,
        float x,
        float y,
        float z,
        Vector3& result)
    {
        const float w = inverseViewProjection.m[3][0] * x + inverseViewProjection.m[3][1] * y +
                        inverseViewProjection.m[3][2] * z + inverseViewProjection.m[3][3];

        if (!std::isfinite(w) || std::abs(w) < 0.000001f)
        {
            return false;
        }

        const float inverseW = 1.0f / w;

        result = {(inverseViewProjection.m[0][0] * x + inverseViewProjection.m[0][1] * y +
                   inverseViewProjection.m[0][2] * z + inverseViewProjection.m[0][3]) *
                      inverseW,
                  (inverseViewProjection.m[1][0] * x + inverseViewProjection.m[1][1] * y +
                   inverseViewProjection.m[1][2] * z + inverseViewProjection.m[1][3]) *
                      inverseW,
                  (inverseViewProjection.m[2][0] * x + inverseViewProjection.m[2][1] * y +
                   inverseViewProjection.m[2][2] * z + inverseViewProjection.m[2][3]) *
                      inverseW};

        return std::isfinite(result.x) && std::isfinite(result.y) && std::isfinite(result.z);
    }

    bool BuildPickingRay(
        const Camera& camera,
        float mouseX,
        float mouseY,
        float viewportWidth,
        float viewportHeight,
        PickingRay& ray)
    {
        if (viewportWidth <= 0.0f || viewportHeight <= 0.0f || !std::isfinite(mouseX) || !std::isfinite(mouseY))
        {
            return false;
        }

        const float x = 2.0f * mouseX / viewportWidth - 1.0f;
        const float y = 1.0f - 2.0f * mouseY / viewportHeight;

        const Matrix4 viewProjection = camera.GetProjectionMatrix() * camera.GetViewMatrix();
        const Matrix4 inverseViewProjection = viewProjection.Inverse();

        Vector3 nearPoint;
        Vector3 farPoint;

        if (!UnprojectPoint(inverseViewProjection, x, y, 0.0f, nearPoint) ||
            !UnprojectPoint(inverseViewProjection, x, y, 1.0f, farPoint))
        {
            return false;
        }

        const Vector3 difference = farPoint - nearPoint;
        const float distance = difference.Length();

        if (!std::isfinite(distance) || distance <= 0.000001f)
        {
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
        if (!bounds.valid)
        {
            return false;
        }

        float tMin = 0.0f;

        float tMax = maxDistance;

        const float origins[3] = {origin.x, origin.y, origin.z};

        const float directions[3] = {direction.x, direction.y, direction.z};
        const float minimums[3] = {bounds.min.x, bounds.min.y, bounds.min.z};
        const float maximums[3] = {bounds.max.x, bounds.max.y, bounds.max.z};

        for (int axis = 0; axis < 3; ++axis)
        {
            if (std::abs(directions[axis]) < 0.00000001f)
            {
                if (origins[axis] < minimums[axis] || origins[axis] > maximums[axis])
                {
                    return false;
                }

                continue;
            }

            const float inverseDirection = 1.0f / directions[axis];

            float t1 = (minimums[axis] - origins[axis]) * inverseDirection;

            float t2 = (maximums[axis] - origins[axis]) * inverseDirection;

            if (t1 > t2)
            {
                (std::swap)(t1, t2);
            }

            tMin = (std::max)(tMin, t1);

            tMax = (std::min)(tMax, t2);

            if (tMin > tMax)
            {
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
        if (!object.mesh || !object.mesh->GetBounds().valid)
        {
            return false;
        }

        // Reject singular transforms before calling Matrix4::Inverse(),
        // which otherwise silently returns identity.
        const Vector3& scale = object.transform.scale;

        if (!std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z) ||
            std::abs(scale.x * scale.y * scale.z) < 0.000001f)
        {
            return false;
        }

        const Matrix4 inverseModel = object.transform.GetModelMatrix().Inverse();

        const Vector3 localOrigin = TransformPoint(inverseModel, ray.origin);

        // Do not normalize: preserve the world-ray parameter t.

        const Vector3 localDirection = TransformDirection(inverseModel, ray.direction);

        return IntersectAABB(
            localOrigin,
            localDirection,
            object.mesh->GetBounds(), ray.maxDistance, hitDistance);
    }

    size_t PickObject(
        const Scene& scene,
        float mouseX,
        float mouseY,
        float viewportWidth,
        float viewportHeight)
    {
        PickingRay ray{};

        if (!BuildPickingRay(scene.GetCamera(), mouseX, mouseY, viewportWidth, viewportHeight, ray))
        {
            return (std::numeric_limits<size_t>::max)();
        }

        const auto& objects = scene.GetRenderObjects();

        size_t closestObject = (std::numeric_limits<size_t>::max)();

        float closestDistance = ray.maxDistance;

        for (size_t i = 0; i < objects.size(); ++i)
        {
            float hitDistance = 0.0f;

            if (IntersectObject(ray, objects[i], hitDistance) && hitDistance <= closestDistance)
            {
                closestDistance = hitDistance;
                closestObject = i;
            }
        }

        return closestObject;
    }
}

bool Editor::RequestApplicationClose()
{
    if(m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending || m_fileRequest)
    {
        return false;
    }

    if(!m_inspectorPanel.HasUnsavedChanges())
    {
        return true;
    }

    m_materialLeaveAction = MaterialLeaveAction::Close;
    return false;
}

bool Editor::TakeApplicationCloseApproval()
{
    return std::exchange(m_applicationCloseApproved, false);
}

std::optional<Editor::FileRequest> Editor::TakeFileRequest()
{
    if(m_fileRequest && m_fileRequest->type != FileRequestType::Save && m_inspectorPanel.HasUnsavedChanges())
    {
        if(m_materialLeaveAction == MaterialLeaveAction::None)
        {
            m_materialLeaveAction = MaterialLeaveAction::Scene;
        }

        return std::nullopt;
    }

    if(m_materialLeaveAction != MaterialLeaveAction::None)
    {
        return std::nullopt;
    }

    return std::exchange(
        m_fileRequest,
        std::nullopt);
}

std::optional<AssetBrowser::MaterialCreationRequest> Editor::TakeMaterialCreationRequest()
{
    return m_assetBrowser.TakeMaterialCreationRequest();
}

void Editor::OnMaterialCreationCompleted(
    const std::filesystem::path& relativePath,
    const std::string& error)
{
    m_assetBrowser.OnMaterialCreationCompleted(relativePath, error);
}

bool Editor::HasFileRequest(FileRequestType type) const noexcept
{
    return m_fileRequest && m_fileRequest->type == type;
}

void Editor::DrawMainMenu(
    const IDemoScene& demoScene,
    DemoSceneManager& sceneManager,
    DebugUI& debugUI)
{
    if (!ImGui::BeginMainMenuBar())
    {
        return;
    }

    if (ImGui::BeginMenu("File"))
    {
        ImGui::BeginDisabled(m_sceneOperationPending || m_fileRequest.has_value() || m_materialLeaveAction != MaterialLeaveAction::None);

        if (ImGui::MenuItem("New"))
        {
            m_fileRequest = FileRequest{FileRequestType::New, {}};
        }

        const bool fileBacked = dynamic_cast<const FileBackedDemoScene*>(&demoScene) != nullptr;
        ImGui::BeginDisabled(!fileBacked);

        if (ImGui::MenuItem("Save", "Ctrl+S"))
        {
            m_fileRequest = FileRequest{FileRequestType::Save, {}};
        }

        ImGui::EndDisabled();

        if (ImGui::BeginMenu("Load"))
        {
            const ProjectPaths paths(ProjectLocator::GetDefaultProjectRoot());
            std::vector<std::filesystem::path> scenes;
            std::error_code error;
            std::filesystem::directory_iterator iterator(
                paths.GetScenesDirectory(),
                error);
            const std::filesystem::directory_iterator end;

            while (!error && iterator != end)
            {
                std::error_code entryError;

                if (iterator->is_regular_file(entryError) && !entryError && iterator->path().extension() == ".scene")
                {
                    scenes.push_back(iterator->path());
                }

                iterator.increment(error);
            }

            std::sort(scenes.begin(), scenes.end(), [](const auto& left, const auto& right)
            {
                return left.filename().native() < right.filename().native();
            });

            for (const auto& scene : scenes)
            {
                const std::string filename = scene.filename().string();

                if (ImGui::MenuItem(filename.c_str()))
                {
                    m_fileRequest = FileRequest{FileRequestType::Load, scene};
                }
            }

            if (error)
            {
                ImGui::TextDisabled("Scenes directory unavailable or incomplete");
            }
            else if (scenes.empty())
            {
                ImGui::TextDisabled("No scene files found");
            }

            ImGui::EndMenu();
        }

        ImGui::EndDisabled();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Editor"))
    {
        ImGui::MenuItem("Scene Inspector", nullptr, &m_showEditorPanel);
        ImGui::BeginDisabled(m_materialLeaveAction != MaterialLeaveAction::None);

        if(ImGui::MenuItem("Inspector", nullptr, m_workspace.InspectorVisible()))
        {
            if(m_workspace.InspectorVisible() && m_inspectorPanel.HasUnsavedChanges())
            {
                m_materialLeaveAction = MaterialLeaveAction::Hide;
            }
            else
            {
                m_workspace.InspectorVisible() = !m_workspace.InspectorVisible();
            }
        }

        ImGui::EndDisabled();
        ImGui::MenuItem("Asset Browser", nullptr, &m_workspace.AssetBrowserVisible());
        ImGui::Separator();

        const std::string_view activeSceneName = sceneManager.GetActiveSceneName();

        ImGui::BeginDisabled(m_sceneOperationPending || m_fileRequest.has_value() || m_materialLeaveAction != MaterialLeaveAction::None);

        if (ImGui::BeginCombo("Demo Scene", activeSceneName.data()))
        {
            for (const DemoSceneEntry& entry : sceneManager.GetAvailableScenes())
            {
                const bool isSelected = entry.name == activeSceneName;

                if (ImGui::Selectable(entry.name.c_str(), isSelected))
                {
                    m_fileRequest = FileRequest{FileRequestType::Demo, {}, entry.name};
                }

                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }

            ImGui::EndCombo();
        }

        ImGui::EndDisabled();

        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Debug"))
    {
        debugUI.DrawMenu();
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

void Editor::DrawSceneViewport(
    std::uint64_t sceneTextureHandle,
    unsigned int sceneWidth,
    unsigned int sceneHeight)
{
    m_viewportImage = {};
    m_viewportHovered = false;
    const auto& rectangle = m_workspace.GetSceneViewport();

    if(rectangle.size.x <= 0.0f || rectangle.size.y <= 0.0f)
    {
        return;
    }

    ImGui::SetNextWindowPos(rectangle.position, ImGuiCond_Always);
    ImGui::SetNextWindowSize(rectangle.size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing;

    if(ImGui::Begin("Scene Viewport", nullptr, flags))
    {
        EditorWorkspace::KeepCurrentWindowBehindFloatingUI();
        if(sceneTextureHandle != 0 && sceneWidth != 0 && sceneHeight != 0)
        {
            const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
            const ImVec2 logicalSize(static_cast<float>(sceneWidth) / framebufferScale.x,
                                     static_cast<float>(sceneHeight) / framebufferScale.y);
            const float scale = (std::min)(rectangle.size.x / logicalSize.x, rectangle.size.y / logicalSize.y);
            const ImVec2 size(logicalSize.x * scale, logicalSize.y * scale);
            const ImVec2 position(rectangle.position.x + (rectangle.size.x - size.x) * 0.5f,
                                  rectangle.position.y + (rectangle.size.y - size.y) * 0.5f);
            m_viewportImage = {position, size};
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const bool inside = mouse.x >= position.x && mouse.y >= position.y &&
                mouse.x < position.x + size.x && mouse.y < position.y + size.y;
            m_viewportHovered = inside && !m_workspace.IsSplitterDragging() &&
                !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) &&
                !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);

            ImGui::GetWindowDrawList()->AddImage(
                ImTextureRef(static_cast<ImTextureID>(sceneTextureHandle)),
                position,
                ImVec2(position.x + size.x, position.y + size.y));
        }
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

const EditorWorkspaceRect& Editor::PrepareWorkspace(
    IDemoScene& demoScene,
    DemoSceneManager& sceneManager,
    DebugUI& debugUI)
{
    DrawMainMenu(demoScene, sceneManager, debugUI);
    m_workspace.Update();
    return m_workspace.GetSceneViewport();
}

void Editor::Draw(
    IDemoScene& demoScene,
    DemoSceneManager& sceneManager,
    DebugUI& debugUI,
    std::uint64_t sceneTextureHandle,
    unsigned int sceneWidth,
    unsigned int sceneHeight,
    float deltaTime,
    const ProjectPaths& projectPaths,
    const ProjectAssetRegistry& assetRegistry,
    const std::optional<MaterialPreviewImage>& materialPreview)
{

    (void)debugUI;
    m_assetBrowser.SetThumbnailProject(projectPaths, assetRegistry);
    ImGui::BeginDisabled(m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending);

    if(m_workspace.AssetBrowserVisible())
    {
        m_assetBrowser.Draw(m_workspace.GetAssetBrowser());
    }
    else
    {
        m_assetBrowser.PauseMaterialThumbnails();
    }

    ImGui::EndDisabled();

    if(m_materialLeaveAction == MaterialLeaveAction::None &&
       m_assetBrowser.GetSelection() != m_inspectedAssetSelection)
    {
        if(m_inspectorPanel.HasUnsavedChanges())
        {
            m_pendingAssetSelection = m_assetBrowser.GetSelection();
            m_materialLeaveAction = MaterialLeaveAction::Selection;
        }
        else
        {
            m_inspectedAssetSelection = m_assetBrowser.GetSelection();
            m_inspectorPanel.LoadSelection(m_inspectedAssetSelection, projectPaths, assetRegistry);
        }
    }

    const bool disableInspector = m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending;

    // This ImGui version's color picker restores alpha whenever a disabled scope exists,
    // including BeginDisabled(false), whose alpha backup may never have been initialized.
    if(disableInspector)
    {
        ImGui::BeginDisabled();
    }

    if(m_workspace.InspectorVisible())
    {
        m_inspectorPanel.Draw(
            m_workspace.GetInspector(), projectPaths, assetRegistry,
            materialPreview,
            m_materialLeaveAction != MaterialLeaveAction::None);
    }

    if(disableInspector)
    {
        ImGui::EndDisabled();
    }

    if(m_inspectorPanel.TakeRefreshRequest() && m_materialLeaveAction == MaterialLeaveAction::None)
    {
        if(m_inspectorPanel.HasUnsavedChanges())
        {
            m_materialLeaveAction = MaterialLeaveAction::Refresh;
        }
        else
        {
            m_inspectorPanel.LoadSelection(m_inspectedAssetSelection, projectPaths, assetRegistry);
        }
    }

    if(m_materialLeaveAction != MaterialLeaveAction::None)
    {
        const auto decision = m_inspectorPanel.DrawUnsavedConfirmation(projectPaths, assetRegistry);

        if(decision == InspectorPanel::LeaveDecision::Proceed)
        {
            switch(m_materialLeaveAction)
            {
            case MaterialLeaveAction::Selection:
                m_inspectedAssetSelection = m_pendingAssetSelection;
                m_inspectorPanel.LoadSelection(m_inspectedAssetSelection, projectPaths, assetRegistry);
                break;
            case MaterialLeaveAction::Refresh:
                m_inspectorPanel.LoadSelection(m_inspectedAssetSelection, projectPaths, assetRegistry);
                break;
            case MaterialLeaveAction::Hide:
                m_workspace.InspectorVisible() = false;
                break;
            case MaterialLeaveAction::Close:
                m_applicationCloseApproved = true;
                break;
            default:
                break;
            }

            m_materialLeaveAction = MaterialLeaveAction::None;
        }
        else if(decision == InspectorPanel::LeaveDecision::Cancel)
        {
            if(m_materialLeaveAction == MaterialLeaveAction::Selection)
            {
                m_assetBrowser.RestoreSelection(m_inspectedAssetSelection);
            }

            if(m_materialLeaveAction == MaterialLeaveAction::Scene)
            {
                m_fileRequest.reset();
            }

            m_materialLeaveAction = MaterialLeaveAction::None;
        }
    }

    if(const auto savedSource = m_inspectorPanel.TakeSavedMaterialSource())
    {
        m_assetBrowser.InvalidateMaterialSource(*savedSource);
    }

    m_assetBrowser.DrawRenameModal(m_inspectorPanel.HasUnsavedChanges() ||
        m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending);

    m_assetBrowser.DrawDeletionConfirmation(m_inspectorPanel.HasUnsavedChanges() ||
        m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending);

    DrawSceneViewport(sceneTextureHandle, sceneWidth, sceneHeight);

    const ImGuiIO& io = ImGui::GetIO();

    if(ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        m_viewportFocused = m_viewportHovered;
    }

    if(io.AppFocusLost || m_sceneOperationPending || m_viewportImage.size.x <= 0.0f || m_workspace.IsSplitterDragging())
    {
        m_viewportFocused = false;
        m_cameraLooking = false;
    }

    if(m_viewportFocused && !io.WantTextInput && !ImGuizmo::IsUsing() && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
    {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false))
        {
            m_gizmoMode = GizmoMode::Translate;
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_E, false))
        {
            m_gizmoMode = GizmoMode::Rotate;
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_R, false))
        {
            m_gizmoMode = GizmoMode::Scale;
        }
    }

    Scene& scene = demoScene.GetScene();
    const std::string activeSceneName(sceneManager.GetActiveSceneName());
    const uint64_t structureRevision = scene.GetStructureRevision();

    if (m_selectionSceneName != activeSceneName || m_selectionStructureRevision != structureRevision)
    {
        ClearTransformUndoHistory();

        m_selectedObject = InvalidObjectSelection;
        m_selectionSceneName = activeSceneName;
        m_selectionStructureRevision = structureRevision;
    }

    const Scene& constScene = scene;
    const auto& objects = constScene.GetRenderObjects();

    if (m_selectedObject >= objects.size())
    {
        m_selectedObject = InvalidObjectSelection;
    }

    if (m_showEditorPanel)
    {
        if (ImGui::Begin("Scene Inspector", &m_showEditorPanel))
        {
            if (auto* sphereGridScene = dynamic_cast<SphereGridScene*>(&demoScene))
            {
                DrawSphereGridControls(*sphereGridScene);

                ImGui::Separator();
            }

            if (auto* stressTestScene = dynamic_cast<StressTestScene*>(&demoScene))
            {
                DrawStressTestControls(*stressTestScene);

                ImGui::Separator();
            }

            DrawObjectControls(demoScene);

            ImGui::Separator();

            DrawCamera(demoScene);

            ImGui::Separator();

            DrawDirectionalLight(demoScene);

        }

        ImGui::End();
    }

    // Save is an application shortcut even when a property input owns keyboard
    // focus. Do not gate it on WantCaptureKeyboard or consume text-edit input.

    if (!m_sceneOperationPending && !m_fileRequest && m_materialLeaveAction == MaterialLeaveAction::None &&
        dynamic_cast<const FileBackedDemoScene*>(&demoScene))
    {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            m_fileRequest = FileRequest{FileRequestType::Save, {}};
        }
    }

    // Projection is driven by the actual render extent, including after Inspector controls.
    scene.GetCamera().SetAspectRatio(static_cast<float>(sceneWidth) / static_cast<float>(sceneHeight));

    HandleTransformUndo(demoScene);
    HandleTransformRedo(demoScene);

    UpdateCameraLook(demoScene, m_viewportImage.size.x, m_viewportImage.size.y);
    UpdateCameraMovement(demoScene, deltaTime);
    DrawTransformGizmo(demoScene, m_viewportImage.size.x, m_viewportImage.size.y);
    HandleViewportSelection(demoScene, m_viewportImage.size.x, m_viewportImage.size.y);
}

void Editor::DrawSphereGridControls(SphereGridScene& demoScene)
{
    if (!ImGui::CollapsingHeader("Diagnostics", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    const char* modeNames[] = {"None", "Rotate Light", "Move Sphere", "Orbit Camera"};

    int selectedMode = static_cast<int>(demoScene.GetUpdateMode());

    if (ImGui::Combo("Update Mode", &selectedMode, modeNames, IM_ARRAYSIZE(modeNames)))
    {
        demoScene.SetUpdateMode(static_cast<DemoUpdateMode>(selectedMode));
    }

    DemoUpdateMode mode = demoScene.GetUpdateMode();

    if (mode == DemoUpdateMode::RotateLight)
    {
        float speed = demoScene.GetLightRotationSpeed();

        if (ImGui::DragFloat("Rotation Speed", &speed, 0.05f, 0.0f, 10.0f, "%.2f"))
        {
            demoScene.SetLightRotationSpeed(speed);
        }

        float verticalAngleDegrees = demoScene.GetLightVerticalAngle() * (180.0f / 3.14159265358979323846f);

        if (ImGui::SliderFloat("Light Angle", &verticalAngleDegrees, 0.0f, 90.0f, "%.1f deg"))
        {
            demoScene.SetLightVerticalAngle(ToRadians(verticalAngleDegrees));
        }
    }
    else if (mode == DemoUpdateMode::OrbitCamera)
    {
        float speed = demoScene.GetCameraOrbitSpeed();

        if (ImGui::DragFloat("Orbit Speed", &speed, 0.05f, 0.0f, 10.0f, "%.2f"))
        {
            demoScene.SetCameraOrbitSpeed(speed);
        }
    }
}

void Editor::DrawStressTestControls(StressTestScene& demoScene)
{
    if (!ImGui::CollapsingHeader("Stress Test", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    constexpr unsigned int gridSizes[] = {10, 20, 30, 40, 50};

    constexpr const char* gridLabels[] = {"10 x 10", "20 x 20", "30 x 30", "40 x 40", "50 x 50"};

    const unsigned int currentGridSize = demoScene.GetGridSize();

    int selectedIndex = 0;

    for (int i = 0; i < IM_ARRAYSIZE(gridSizes); ++i)
    {
        if (gridSizes[i] == currentGridSize)
        {
            selectedIndex = i;
            break;
        }
    }

    if (ImGui::Combo("Grid Size", &selectedIndex, gridLabels, IM_ARRAYSIZE(gridLabels)))
    {
        demoScene.SetGridSize(gridSizes[selectedIndex]);
    }

    ImGui::Text("Sphere instances: %u", currentGridSize * currentGridSize);
}

void Editor::DrawDirectionalLight(IDemoScene& demoScene)
{
    if (!ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    DirectionalLight& light = demoScene.GetScene().GetDirectionalLight();

    ImGui::DragFloat3("Direction", &light.direction.x, 0.01f, -1.0f, 1.0f);

    ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0.0f, 100.0f);

    ImGui::DragFloat("Ambient Strength", &light.ambientStrength, 0.01f, 0.0f, 1.0f);

    float angularRadiusDegrees = light.angularRadius * (180.0f / 3.14159265358979323846f);

    if (ImGui::DragFloat("Angular Radius", &angularRadiusDegrees, 0.01f, 0.0f, 90.0f, "%.2f deg"))
    {
        light.angularRadius = ToRadians(angularRadiusDegrees);
    }

    if (ImGui::Button("Default##DirectionalLight"))
    {
        demoScene.ResetDirectionalLight();
    }
}

void Editor::DrawObjectControls(
    IDemoScene& demoScene)
{
    Scene& scene = demoScene.GetScene();

    const auto& objects = scene.GetRenderObjects();

    if (!ImGui::CollapsingHeader("Objects", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    const std::string preview =
        m_selectedObject == InvalidObjectSelection ? "None" : "Object " + std::to_string(m_selectedObject);

    if (ImGui::BeginCombo("Selected Object", preview.c_str()))
    {
        const bool noneSelected = m_selectedObject == InvalidObjectSelection;

        if (ImGui::Selectable("None", noneSelected))
        {
            m_selectedObject = InvalidObjectSelection;
        }

        for (size_t i = 0; i < objects.size(); ++i)
        {
            const std::string label = "Object " + std::to_string(i);

            const bool selected = m_selectedObject == i;

            if (ImGui::Selectable(label.c_str(), selected))
            {
                m_selectedObject = i;
            }

            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    ImGui::Text("Objects: %zu", objects.size());

    if (m_selectedObject == InvalidObjectSelection)
    {
        return;
    }

    Transform transform = objects[m_selectedObject].transform;

    bool transformChanged = false;

    const Transform originalTransform = transform;

    bool fieldActivated = false;

    bool fieldDeactivated = false;

    ImGui::Separator();
    ImGui::PushID("SelectedObjectTransform");

    transformChanged |= ImGui::DragFloat3("Position", &transform.position.x, 0.05f);

    fieldActivated |= ImGui::IsItemActivated();
    fieldDeactivated |= ImGui::IsItemDeactivatedAfterEdit();

    float rotationDegrees[3] = {ToDegrees(transform.rotation.x), ToDegrees(transform.rotation.y),
                                ToDegrees(transform.rotation.z)};

    if (ImGui::DragFloat3("Rotation", rotationDegrees, 0.05))
    {
        transform.rotation =
            Vector3{ToRadians(rotationDegrees[0]), ToRadians(rotationDegrees[1]), ToRadians(rotationDegrees[2])};

        transformChanged = true;
    }

    fieldActivated |= ImGui::IsItemActivated();
    fieldDeactivated |= ImGui::IsItemDeactivatedAfterEdit();

    if (ImGui::DragFloat3("Scale", &transform.scale.x, 0.01f, 0.01f, 100.0f))
    {
        transform.scale.x = (std::max)(0.01f, transform.scale.x);
        transform.scale.y = (std::max)(0.01f, transform.scale.y);
        transform.scale.z = (std::max)(0.01f, transform.scale.z);

        transformChanged = true;
    }

    fieldActivated |= ImGui::IsItemActivated();
    fieldDeactivated |= ImGui::IsItemDeactivatedAfterEdit();

    if (fieldActivated && !m_transformFieldEditing)
    {
        m_transformFieldEditing = true;
        m_transformFieldUndoObject = m_selectedObject;
        m_transformFieldUndoStart = originalTransform;
    }

    ImGui::PopID();

    if (transformChanged)
    {
        scene.SetRenderObjectTransform(m_selectedObject, transform);
    }

    if (fieldDeactivated && m_transformFieldEditing)
    {
        const auto& currentObjects = static_cast<const Scene&>(scene).GetRenderObjects();

        if (m_transformFieldUndoObject < currentObjects.size())
        {
            const Transform& current = currentObjects[m_transformFieldUndoObject].transform;

            const Transform& previous = m_transformFieldUndoStart;

            const bool changed =

                current.position.x != previous.position.x || current.position.y != previous.position.y ||

                current.position.z != previous.position.z || current.rotation.x != previous.rotation.x ||

                current.rotation.y != previous.rotation.y || current.rotation.z != previous.rotation.z ||
                current.scale.x != previous.scale.x || current.scale.y != previous.scale.y ||
                current.scale.z != previous.scale.z;

            if (changed)
            {
                m_transformRedoHistory.clear();
                m_transformUndoHistory.push_back({m_transformFieldUndoObject, previous});
            }
        }

        m_transformFieldEditing = false;
        m_transformFieldUndoObject = InvalidObjectSelection;
    }
}

void Editor::DrawTransformGizmo(
    IDemoScene& demoScene,
    float viewportWidth,
    float viewportHeight)
{
    if (m_selectedObject == InvalidObjectSelection || viewportWidth <= 0.0f || viewportHeight <= 0.0f)
    {
        return;
    }

    Scene& scene = demoScene.GetScene();

    const Scene& constScene = scene;
    const auto& objects = constScene.GetRenderObjects();

    if (m_selectedObject >= objects.size())
    {
        return;
    }

    Transform transform = objects[m_selectedObject].transform;

    const Camera& camera = scene.GetCamera();

    Matrix4 view = camera.GetViewMatrix().Transpose();

    Matrix4 projection = camera.GetProjectionMatrix().Transpose();
    Matrix4 model = transform.GetModelMatrix().Transpose();

    // Append to the image's window: separate root windows have independent display ordering.
    // The first Begin in DrawSceneViewport owns placement, sizing and style for this frame.
    const ImGuiWindowFlags viewportFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::Begin("Scene Viewport", nullptr, viewportFlags);

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGui::GetWindowDrawList()->PushClipRect(m_viewportImage.position,
        ImVec2(m_viewportImage.position.x + viewportWidth, m_viewportImage.position.y + viewportHeight), true);
    ImGuizmo::SetRect(m_viewportImage.position.x, m_viewportImage.position.y, viewportWidth, viewportHeight);
    const bool allowGizmo = !ImGui::GetIO().AppFocusLost && !m_sceneOperationPending &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && !m_cameraLooking;
    ImGuizmo::Enable(allowGizmo && !m_workspace.IsSplitterDragging() && (m_viewportHovered || ImGuizmo::IsUsing()));

    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;

    switch (m_gizmoMode)
    {
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
    ImGui::GetWindowDrawList()->PopClipRect();

    ImGui::End();

    const bool gizmoUsing = ImGuizmo::IsUsing();

    if (gizmoUsing && !m_gizmoWasUsing)
    {
        m_gizmoUndoObject = m_selectedObject;
        m_gizmoUndoStart = transform;
    }

    if (gizmoUsing)
    {
        float translation[3];
        float rotationDegrees[3];
        float scale[3];

        ImGuizmo::DecomposeMatrixToComponents(&model.m[0][0], translation, rotationDegrees, scale);

        bool changed = false;

        switch (m_gizmoMode)
        {
        case GizmoMode::Translate:
        {
            const Vector3 newPosition{translation[0], translation[1], translation[2]};

            if (newPosition.x != transform.position.x || newPosition.y != transform.position.y ||
                newPosition.z != transform.position.z)
            {
                transform.position = newPosition;

                changed = true;
            }

            break;
        }

        case GizmoMode::Rotate:
        {
            const Vector3 newRotation{ToRadians(rotationDegrees[0]), ToRadians(rotationDegrees[1]),
                                      ToRadians(rotationDegrees[2])};

            transform.rotation = newRotation;
            changed = true;
            break;
        }

        case GizmoMode::Scale:
        {
            transform.scale =
                Vector3{(std::max)(0.01f, scale[0]), (std::max)(0.01f, scale[1]), (std::max)(0.01f, scale[2])};

            changed = true;
            break;
        }
        }

        if (changed)
        {
            scene.SetRenderObjectTransform(m_selectedObject, transform);
        }
    }

    if (!gizmoUsing && m_gizmoWasUsing && m_gizmoUndoObject != InvalidObjectSelection)
    {
        const auto& currentObjects = static_cast<const Scene&>(scene).GetRenderObjects();

        if (m_gizmoUndoObject < currentObjects.size())
        {
            const Transform& current = currentObjects[m_gizmoUndoObject].transform;

            const Transform& previous = m_gizmoUndoStart;

            const bool changed =
                current.position.x != previous.position.x || current.position.y != previous.position.y ||

                current.position.z != previous.position.z || current.rotation.x != previous.rotation.x ||

                current.rotation.y != previous.rotation.y || current.rotation.z != previous.rotation.z ||

                current.scale.x != previous.scale.x || current.scale.y != previous.scale.y ||

                current.scale.z != previous.scale.z;

            if (changed)
            {
                m_transformRedoHistory.clear();
                m_transformUndoHistory.push_back({m_gizmoUndoObject, previous});
            }
        }

        m_gizmoUndoObject = InvalidObjectSelection;
    }

    m_gizmoWasUsing = gizmoUsing;
}

void Editor::HandleViewportSelection(
    IDemoScene& demoScene,
    float viewportWidth,
    float viewportHeight)
{
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();

    if(!m_viewportHovered || m_sceneOperationPending || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) ||
        ImGuizmo::IsUsing())
    {
        return;
    }

    if (m_selectedObject != InvalidObjectSelection && ImGuizmo::IsOver())
    {
        return;
    }

    const ImVec2 mouse(io.MousePos.x - m_viewportImage.position.x, io.MousePos.y - m_viewportImage.position.y);

    if (mouse.x < 0.0f || mouse.y < 0.0f || mouse.x >= viewportWidth || mouse.y >= viewportHeight)
    {
        return;
    }

    const Scene& scene = demoScene.GetScene();

    m_selectedObject = PickObject(scene, mouse.x, mouse.y, viewportWidth, viewportHeight);
}

void Editor::ClearTransformUndoHistory()
{
    m_transformUndoHistory.clear();
    m_transformRedoHistory.clear();

    m_gizmoWasUsing = false;
    m_gizmoUndoObject = InvalidObjectSelection;

    m_transformFieldEditing = false;
    m_transformFieldUndoObject = InvalidObjectSelection;
}

void Editor::HandleTransformUndo(IDemoScene& demoScene)
{
    const ImGuiIO& io = ImGui::GetIO();

    if (!io.KeyCtrl || !ImGui::IsKeyPressed(ImGuiKey_Z, false) || io.KeyShift || io.WantTextInput ||
        ImGui::IsAnyItemActive())
    {
        return;
    }

    if (ImGuizmo::IsUsing() || m_cameraLooking || m_transformFieldEditing ||
        m_transformUndoHistory.empty())
    {
        return;
    }

    Scene& scene = demoScene.GetScene();

    const TransformUndoEntry entry = m_transformUndoHistory.back();
    m_transformUndoHistory.pop_back();

    if (entry.objectIndex >= scene.GetRenderObjects().size())
    {
        return;
    }

    const auto& objects = static_cast<const Scene&>(scene).GetRenderObjects();

    const Transform currentTransform = objects[entry.objectIndex].transform;

    m_transformRedoHistory.push_back({entry.objectIndex, currentTransform});

    scene.SetRenderObjectTransform(entry.objectIndex, entry.previousTransform);
}

void Editor::HandleTransformRedo(IDemoScene& demoScene)
{
    const ImGuiIO& io = ImGui::GetIO();

    const bool redoPressed = io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Y, false) ||
                                            (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)));

    if (!redoPressed || io.WantTextInput || ImGui::IsAnyItemActive() || ImGuizmo::IsUsing())
    {
        return;
    }

    if (m_cameraLooking ||
        m_transformFieldEditing || m_transformRedoHistory.empty())
    {
        return;
    }

    Scene& scene = demoScene.GetScene();

    const TransformUndoEntry entry = m_transformRedoHistory.back();
    m_transformRedoHistory.pop_back();

    const auto& objects = static_cast<const Scene&>(scene).GetRenderObjects();

    if (entry.objectIndex >= objects.size())
    {
        return;
    }

    const Transform currentTransform = objects[entry.objectIndex].transform;

    m_transformUndoHistory.push_back({entry.objectIndex, currentTransform});

    scene.SetRenderObjectTransform(entry.objectIndex, entry.previousTransform);
}

void Editor::UpdateCameraLook(
    IDemoScene& demoScene,
    float viewportWidth,
    float viewportHeight)
{
    const ImGuiIO& io = ImGui::GetIO();

    (void)viewportWidth;
    (void)viewportHeight;

    if(!ImGui::IsMouseDown(ImGuiMouseButton_Right) || io.AppFocusLost || !m_viewportFocused ||
       ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
    {
        if (m_cameraLooking)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        }

        m_cameraLooking = false;
        return;
    }

    if (!m_cameraLooking)
    {
        if(!m_viewportHovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Right) || m_sceneOperationPending)
        {
            return;
        }

        if (ImGuizmo::IsUsing())
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

    const Vector3 direction{std::sin(m_cameraYaw) * std::cos(m_cameraPitch), std::sin(m_cameraPitch),
                            std::cos(m_cameraYaw) * std::cos(m_cameraPitch)};

    Camera& camera = demoScene.GetScene().GetCamera();

    camera.SetTarget(camera.GetPosition() + direction);
}

void Editor::UpdateCameraMovement(
    IDemoScene& demoScene,
    float deltaTime)
{
    if (!m_cameraLooking || deltaTime <= 0.0f)
    {
        return;
    }

    Camera& camera = demoScene.GetScene().GetCamera();

    const Vector3 forward{std::sin(m_cameraYaw) * std::cos(m_cameraPitch), std::sin(m_cameraPitch),
                          std::cos(m_cameraYaw) * std::cos(m_cameraPitch)};

    const Vector3 right{std::cos(m_cameraYaw), 0.0f, -std::sin(m_cameraYaw)};

    const Vector3 up{0.0f, 1.0f, 0.0f};

    Vector3 movement{0.0f, 0.0f, 0.0f};

    if (ImGui::IsKeyDown(ImGuiKey_W))
    {
        movement = movement + forward;
    }

    if (ImGui::IsKeyDown(ImGuiKey_S))
    {
        movement = movement - forward;
    }

    if (ImGui::IsKeyDown(ImGuiKey_D))
    {
        movement = movement + right;
    }

    if (ImGui::IsKeyDown(ImGuiKey_A))
    {
        movement = movement - right;
    }

    if (ImGui::IsKeyDown(ImGuiKey_E) || ImGui::IsKeyDown(ImGuiKey_Space))
    {
        movement = movement + up;
    }

    if (ImGui::IsKeyDown(ImGuiKey_Q))
    {
        movement = movement - up;
    }

    const float length = movement.Length();

    if (length <= 0.000001f)
    {
        return;
    }

    movement = movement * (1.0f / length);

    constexpr float normalSpeed = 5.0f;
    constexpr float fastSpeed = 15.0f;

    const bool shiftHelp = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);

    const float speed = shiftHelp ? fastSpeed : normalSpeed;

    const Vector3 offset = movement * (speed * deltaTime);

    camera.SetPosition(camera.GetPosition() + offset);

    camera.SetTarget(camera.GetTarget() + offset);
}

void Editor::DrawCamera(IDemoScene& demoScene)
{
    if (!ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    Camera& camera = demoScene.GetScene().GetCamera();

    Vector3 position = camera.GetPosition();

    if (ImGui::DragFloat3("Position", &position.x, 0.05f))
    {
        camera.SetPosition(position);
    }

    Vector3 target = camera.GetTarget();

    if (ImGui::DragFloat3("Target", &target.x, 0.05f))
    {
        camera.SetTarget(target);
    }

    if (ImGui::Button("Default##Camera"))
    {
        demoScene.ResetCamera();
    }
}

void Editor::SetSceneOperationPending(bool pending) noexcept
{
    m_sceneOperationPending = pending;
}

Editor::SceneChangeDecision Editor::DrawSceneChangeConfirmation(const std::string& error)
{
    constexpr const char* title = "Unsaved scene changes";

    if (!ImGui::IsPopupOpen(title))
    {
        ImGui::OpenPopup(title);
    }

    SceneChangeDecision decision = SceneChangeDecision::None;

    bool confirmationOpen = true;

    if (ImGui::BeginPopupModal(title, &confirmationOpen, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Save changes before leaving this scene?");

        if (!error.empty())
        {
            ImGui::TextWrapped("%s", error.c_str());
        }

        if (ImGui::Button("Save"))
        {
            decision = SceneChangeDecision::Save;
        }

        ImGui::SameLine();

        if (ImGui::Button("Discard"))
        {
            decision = SceneChangeDecision::Discard;
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel"))
        {
            decision = SceneChangeDecision::Cancel;
        }

        // Save failure is handled by Engine and reopens the modal next frame.

        if (decision != SceneChangeDecision::None)
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if(!confirmationOpen)
    {
        decision = SceneChangeDecision::Cancel;
    }

    return decision;
}

void Editor::SuspendViewportInteraction()
{
    m_cameraLooking = false;
    m_viewportFocused = false;
    m_viewportHovered = false;
    m_viewportImage = {};
    ImGuizmo::Enable(false);
}

void Editor::OnSceneActivated()
{
    m_cameraLooking = false;
    m_viewportFocused = false;

    // Different files may share a display name and structure revision.
    ClearTransformUndoHistory();
    m_selectedObject = InvalidObjectSelection;
    m_selectionSceneName.clear();
    m_selectionStructureRevision = 0;
}

void Editor::ImportFileDrop(
    const std::vector<std::filesystem::path>& files,
    float screenX,
    float screenY,
    const ProjectPaths& paths,
    ProjectAssetRegistry& registry)
{
    if(m_sceneOperationPending || m_materialLeaveAction != MaterialLeaveAction::None)
    {
        Log::Info("Texture drop rejected while an editor operation is pending.");
        return;
    }

    m_assetBrowser.ImportFileDrop(files, screenX, screenY, paths, registry);
}

void Editor::ProcessAssetDeletion(const ProjectPaths& paths, ProjectAssetRegistry& registry)
{
    const auto request = m_assetBrowser.TakeDeletionRequest();

    if(!request)
    {
        return;
    }

    std::string error;

    if(m_inspectorPanel.HasUnsavedChanges() || m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending)
    {
        error = "Deletion rejected while editor changes or a scene operation are pending.";
    }
    else
    {
        ProjectAssetDeletion::RemoveFile(paths, registry, *request, error);
    }

    m_assetBrowser.OnDeletionCompleted(*request, error);

    if(error.empty())
    {
        Log::Info("Deleted project file: " + request->string());
    }
    else
    {
        Log::Error("Project file deletion failed: " + error);
    }

    if(error.empty() && m_inspectedAssetSelection == *request)
    {
        m_inspectedAssetSelection.clear();
        m_pendingAssetSelection.clear();
        m_inspectorPanel.LoadSelection({}, paths, registry);
    }
}

void Editor::ProcessAssetRename(
    const ProjectPaths& paths,
    ProjectAssetRegistry& registry,
    const std::filesystem::path& activeScenePath)
{
    const auto request = m_assetBrowser.TakeRenameRequest();

    if(!request)
    {
        return;
    }

    std::string error;
    std::filesystem::path destination;

    if(m_inspectorPanel.HasUnsavedChanges() || m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending)
    {
        error = "Rename rejected while editor changes or a scene operation are pending.";
    }
    else if(!activeScenePath.empty() &&
        _wcsicmp(std::filesystem::absolute(request->source).lexically_normal().c_str(),
            std::filesystem::absolute(activeScenePath).lexically_normal().c_str()) == 0)
    {
        error = "The active scene retains its Save path. Open another scene before renaming this file.";
    }
    else
    {
        ProjectAssetRename::RenameFile(paths, registry, request->source, request->stem, destination, error);
    }

    m_assetBrowser.OnRenameCompleted(request->source, destination, error);

    if(!error.empty())
    {
        Log::Error("Project file rename failed: " + error);
    }
    else
    {
        Log::Info("Renamed project file: " + request->source.string() + " -> " + destination.string());

        if(m_inspectedAssetSelection == request->source && request->source != destination)
        {
            m_inspectedAssetSelection = destination;
            m_pendingAssetSelection.clear();
            m_inspectorPanel.LoadSelection(destination, paths, registry);
        }
    }
}
