#include "assets/ProjectAssetRename.h"
#include "assets/ProjectAssetDeletion.h"
#include "assets/ProjectAssetResolver.h"
#include "Core/Log.h"
#include "Editor.h"
#include "ViewportCameraInput.h"
#include "SceneViewportLayout.h"
#include "GroupTransform.h"
#include "ScenePrimitiveCreation.h"
#include "ViewportPicking.h"
#include "InspectorNumericDrag.h"
#include "Debug/DebugUI.h"

#include "Math/Math.h"

#include "DemoContent/IDemoScene.h"
#include "DemoContent/SphereGridScene.h"
#include "DemoContent/StressTestScene.h"
#include "DemoContent/DemoSceneManager.h"
#include "DemoContent/FileBackedDemoScene.h"

#include "Project/ProjectContext.h"
#include "Project/ProjectPaths.h"

#include "Renderer/Mesh.h"

#include "Scene/DirectionalLight.h"
#include "Scene/Scene.h"

#include "imgui.h"
#include <type_traits>
#include "imgui_internal.h"
#include "ImGuizmo.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <utility>
#include <unordered_map>

namespace
{
    bool SameHistoryTransform(const Transform& left, const Transform& right);
    float ViewportCameraSpeed()
    {
        const bool fast = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
        return fast ? 15.0f : 5.0f;
    }

    using PickingRay = ViewportPicking::Ray;

    using ViewportPicking::BuildPickingRay;

    bool IsFinitePosition(const Vector3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    using RaycastVector = ViewportPicking::DoubleVector;

    bool RaycastObject(const PickingRay& ray, const RenderObject& object,
        const ImportedMesh& geometry, double& nearestDistance)
    {
        // The scene owns immutable CPU arrays for this resident mesh. No pointer
        // escapes this synchronous main-thread query or a scene mutation boundary.
        if(!object.mesh || object.mesh->GetVertexCount() != geometry.vertices.size() ||
            object.mesh->GetIndexCount() != geometry.indices.size())
        {
            return false;
        }
        return ViewportPicking::IntersectMesh(ray, object.GetPresentationTransform().currentModel, object.mesh->GetBounds(),
            geometry, object.mesh->IsIndexed(), object.mesh->GetMetadata().submeshes, nearestDistance);
    }

    bool IntersectPlacementPlane(const PickingRay& ray, const Vector3& point, const Vector3& normal,
        double& distance)
    {
        const double denominator = RaycastVector(normal).Dot(RaycastVector(ray.direction));

        if(!std::isfinite(denominator) || std::abs(denominator) < 1e-8)
        {
            return false;
        }

        distance = (RaycastVector(point) - RaycastVector(ray.origin)).Dot(RaycastVector(normal)) / denominator;
        return std::isfinite(distance) && distance > 0 && distance <= ray.maxDistance;
    }

    bool FindMeshDropPosition(const FileBackedDemoScene& owner, float mouseX, float mouseY,
        float width, float height, Vector3& position)
    {
        const auto& scene = owner.GetScene();
        const auto& camera = scene.GetCamera();
        const Vector3 forward = (camera.GetTarget() - camera.GetPosition()).Normalized();

        if(!IsFinitePosition(forward) || forward.Length() < 0.5f ||
            Vector3::Cross({0, 1, 0}, forward).Length() < 1e-6f)
        {
            return false;
        }

        PickingRay ray;

        if(!BuildPickingRay(camera, mouseX, mouseY, width, height, ray))
        {
            return false;
        }

        double distance = ray.maxDistance;
        bool hit = false;

        for(const auto& object : scene.GetRenderObjects())
        {
            const auto* geometry = object.mesh ? owner.GetMeshGeometry(*object.mesh) : nullptr;

            if(geometry && RaycastObject(ray, object, *geometry, distance))
            {
                hit = true;
            }
        }

        // Camera::GetViewMatrix establishes +Y up. Ground is the XZ plane at
        // y=0. Last fallback is perpendicular to view, 10 engine units ahead.
        if(!hit && !IntersectPlacementPlane(ray, {}, {0, 1, 0}, distance) &&
            !IntersectPlacementPlane(ray, camera.GetPosition() + forward * 10.0f, forward, distance))
        {
            return false;
        }

        position = {static_cast<float>(ray.origin.x + double(ray.direction.x) * distance),
            static_cast<float>(ray.origin.y + double(ray.direction.y) * distance),
            static_cast<float>(ray.origin.z + double(ray.direction.z) * distance)};
        return IsFinitePosition(position) &&
            RaycastVector(position - camera.GetPosition()).Dot(RaycastVector(forward)) > 0;
    }

    size_t PickObject(
        const IDemoScene& owner,
        float mouseX,
        float mouseY,
        float viewportWidth,
        float viewportHeight)
    {
        const auto& scene = owner.GetScene();
        PickingRay ray{};

        if (!BuildPickingRay(scene.GetCamera(), mouseX, mouseY, viewportWidth, viewportHeight, ray))
        {
            return (std::numeric_limits<size_t>::max)();
        }

        const auto& objects = scene.GetRenderObjects();

        return ViewportPicking::PickNearest(objects.size(), ray.maxDistance, [&](size_t index, double& nearest)
        {
            const auto& object = objects[index];
            const auto* geometry = object.mesh ? owner.GetMeshGeometry(*object.mesh) : nullptr;
            // Pending/GPU-only geometry is skipped, never promoted from an AABB hit.
            return geometry && RaycastObject(ray, object, *geometry, nearest);
        });
    }
}

bool Editor::RequestApplicationClose()
{
    QueueFileRequest(FileRequest{FileRequestType::Exit, {}});
    return false;
}

bool Editor::QueueFileRequest(FileRequest request)
{
    if(m_sceneOperationPending || m_fileRequest || m_materialLeaveAction != MaterialLeaveAction::None ||
        IsPhysicsEditingLocked())
    {
        ReportLifecycleMessage("Finish the pending operation and stop physics before leaving the scene.");
        return false;
    }

    m_fileRequest = std::move(request);
    return true;
}

void Editor::ReportLifecycleMessage(std::string message)
{
    m_lifecycleMessage = std::move(message);
}

bool Editor::TakeApplicationCloseApproval()
{
    return std::exchange(m_applicationCloseApproved, false);
}

std::optional<Editor::FileRequest> Editor::TakeFileRequest()
{
    if(m_fileRequest && (m_fileRequest->type != FileRequestType::Save && m_fileRequest->type != FileRequestType::SaveAs) &&
        m_inspectorPanel.HasUnsavedChanges())
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

void Editor::DrawLifecycleWindows(const IDemoScene& demoScene)
{
    DrawProjectSettings(demoScene);
    DrawFileAssociation();
    DrawProjectOwnershipLost();

    if(!m_lifecycleMessage.empty())
    {
        bool visible = true;
        ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);

        if(ImGui::Begin("Scene and project status", &visible, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::PushTextWrapPos(490);
            ImGui::TextUnformatted(m_lifecycleMessage.c_str());
            ImGui::PopTextWrapPos();

            if(ImGui::Button("Dismiss"))
            {
                visible = false;
            }
        }

        ImGui::End();

        if(!visible)
        {
            m_lifecycleMessage.clear();
        }
    }
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

    if(ImGui::BeginMenu("File"))
    {
        ImGui::BeginDisabled(m_sceneOperationPending || m_fileRequest.has_value() ||
            m_materialLeaveAction != MaterialLeaveAction::None || IsPhysicsEditingLocked());

        if(ImGui::MenuItem("New Scene"))
        {
            QueueFileRequest(FileRequest{FileRequestType::New, {}});
        }

        if(ImGui::MenuItem("Open Scene..."))
        {
            QueueFileRequest(FileRequest{FileRequestType::Load, {}});
        }

        const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&demoScene);
        ImGui::BeginDisabled(!owner || owner->IsArchiveActivation());

        if(ImGui::MenuItem("Save Scene", "Ctrl+S"))
        {
            QueueFileRequest(FileRequest{FileRequestType::Save, {}});
        }

        if(ImGui::MenuItem("Save Scene As..."))
        {
            QueueFileRequest(FileRequest{FileRequestType::SaveAs, {}});
        }

        ImGui::EndDisabled();
        ImGui::Separator();

        if(ImGui::MenuItem("New Project..."))
        {
            QueueFileRequest(FileRequest{FileRequestType::NewProject, {}});
        }

        if(ImGui::MenuItem("Open Project..."))
        {
            QueueFileRequest(FileRequest{FileRequestType::OpenProject, {}});
        }

        if(ImGui::MenuItem("Project Settings"))
        {
            m_showProjectSettings = true;
            m_startupSceneCandidatesListed = false;
        }

        ImGui::Separator();

        if(ImGui::MenuItem("Exit"))
        {
            RequestApplicationClose();
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
        ImGui::MenuItem("Hierarchy", nullptr, &m_workspace.HierarchyVisible());
        ImGui::MenuItem("Asset Browser", nullptr, &m_workspace.AssetBrowserVisible());
        ImGui::Separator();

        if(ImGui::MenuItem("Windows File Association..."))
        {
            m_showFileAssociation = true;
            m_fileAssociationStatusRead = false;
        }

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
                    QueueFileRequest(FileRequest{FileRequestType::Demo, {}, entry.name});
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

    if(ImGui::BeginMenu("Build"))
    {
        if(ImGui::MenuItem("Build")) m_showBuildGame=true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Debug"))
    {
        debugUI.DrawMenu();
        ImGui::EndMenu();
    }

    DrawPhysicsControls();
    ImGui::EndMainMenuBar();
}

void Editor::DrawResolutionInfo(const ImVec2& position) const
{
    const ViewportResolutionInfo& info = m_viewportResolutionInfo;

    if(info.outputWidth == 0 || info.outputHeight == 0)
    {
        return;
    }

    // The effective scale is reported per axis as the renderer chose the extents; equal axes print once.
    const double scaleX = 100.0 * info.renderWidth / info.outputWidth;
    const double scaleY = 100.0 * info.renderHeight / info.outputHeight;
    char lines[4][96]{};
    std::snprintf(lines[0], sizeof(lines[0]), "Output   %u x %u", info.outputWidth, info.outputHeight);
    std::snprintf(lines[1], sizeof(lines[1]), "Render   %u x %u", info.renderWidth, info.renderHeight);

    if(std::abs(scaleX - scaleY) < 0.05)
    {
        std::snprintf(lines[2], sizeof(lines[2]), "Scale    %.1f%%", scaleX);
    }
    else
    {
        std::snprintf(lines[2], sizeof(lines[2]), "Scale    %.1f%% x %.1f%%", scaleX, scaleY);
    }

    std::snprintf(lines[3], sizeof(lines[3]), "Mode     %s", info.mode);

    float width = 0.0f;

    for(const char* line : lines)
    {
        width = (std::max)(width, ImGui::CalcTextSize(line).x);
    }

    const float padding = 6.0f;
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(position, ImVec2(position.x + width + 2.0f * padding,
        position.y + 4.0f * lineHeight + 2.0f * padding - ImGui::GetStyle().ItemSpacing.y), IM_COL32(0, 0, 0, 150), 4.0f);

    for(int line = 0; line < 4; ++line)
    {
        drawList->AddText(ImVec2(position.x + padding, position.y + padding + line * lineHeight),
            IM_COL32(230, 230, 230, 255), lines[line]);
    }
}

void Editor::DrawSceneViewportToolbar(const EditorWorkspaceRect& toolbarRectangle)
{
    if(toolbarRectangle.size.x <= 0.0f || toolbarRectangle.size.y <= 0.0f)
    {
        return;
    }

    ImGui::SetNextWindowPos(toolbarRectangle.position, ImGuiCond_Always);
    ImGui::SetNextWindowSize(toolbarRectangle.size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoScrollWithMouse;
    if(ImGui::Begin("Scene Viewport Toolbar", nullptr, flags))
    {
        EditorWorkspace::KeepCurrentWindowBehindFloatingUI();
        const char* overlaysLabel = "Overlays";
        const float buttonWidth = ImGui::CalcTextSize(overlaysLabel).x + ImGui::GetFrameHeight() +
            ImGui::GetStyle().FramePadding.x * 2.0f;
        const float overlaysPositionX = (std::max)(0.0f, toolbarRectangle.size.x - buttonWidth);

        DrawGridSnapControls(overlaysPositionX - ImGui::GetStyle().ItemSpacing.x);

        ImGui::SameLine();
        ImGui::SetCursorPosX(overlaysPositionX);
        ImGui::SetNextItemWidth(buttonWidth);
        if(ImGui::BeginCombo("##SceneOverlays", overlaysLabel))
        {
            // This display preference stays editable while authored physics mutations are locked.
            ImGui::Checkbox("Physics Colliders", &m_showPhysicsColliders);
            ImGui::Checkbox("Camera Frusta", &m_showCameraFrusta);
            ImGui::Checkbox("Light Wireframes", &m_showLightWireframes);
            ImGui::Checkbox("Camera Icons", &m_showCameraIcons);
            ImGui::Checkbox("Light Icons", &m_showLightIcons);
            ImGui::Checkbox("Grid", &m_showViewportGrid);
            ImGui::Checkbox("Resolution Info", &m_showResolutionInfo);
            ImGui::EndCombo();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
}

namespace
{
    // Numeric snap-interval entry. Each valid keystroke applies immediately; an invalid draft
    // is reported while editing and reverted afterwards, so the applied value is never corrupted.
    void DrawSnapIntervalInput(const char* label, const char* inputID, float& draft, float& applied)
    {
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        ImGui::InputFloat(inputID, &draft, 0.0f, 0.0f, "%.4g");
        const bool inputActive = ImGui::IsItemActive();

        if(ViewportGridSnap::IsValidInterval(draft))
        {
            applied = draft;
        }
        else if(inputActive)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "Interval must be finite and greater than zero.");
        }
        else
        {
            draft = applied;
        }
    }
}

void Editor::DrawGridSnapControls(float rightEdge)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const char* toggleLabel = "Snap##ViewportSnapToggle";
    const float toggleWidth = ImGui::CalcTextSize(toggleLabel, nullptr, true).x + style.FramePadding.x * 2.0f;
    const float dropdownWidth = ImGui::GetFrameHeight();
    ImGui::SetCursorPosX((std::max)(0.0f, rightEdge - toggleWidth - dropdownWidth));

    // One master toggle drives translation and rotation snapping. It borrows the theme's
    // pressed colours so the enabled state reads as held down.
    const bool snapWasEnabled = m_snapEnabled;

    if(snapWasEnabled)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, style.Colors[ImGuiCol_ButtonActive]);
    }

    if(ImGui::Button(toggleLabel))
    {
        m_snapEnabled = !m_snapEnabled;
    }

    if(snapWasEnabled)
    {
        ImGui::PopStyleColor(2);
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Gizmo snapping %s (move %.4g map units, rotate %.4g degrees)",
            m_snapEnabled ? "on" : "off", m_translationSnapInterval, m_rotationSnapIntervalDegrees);
    }

    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetNextItemWidth(dropdownWidth);

    // Editing either interval never changes the master enabled state.
    if(ImGui::BeginCombo("##ViewportSnapSettings", nullptr, ImGuiComboFlags_NoPreview))
    {
        DrawSnapIntervalInput("Translation interval (map units)", "##TranslationSnapInterval",
            m_translationSnapIntervalDraft, m_translationSnapInterval);
        DrawSnapIntervalInput("Rotation interval (degrees)", "##RotationSnapInterval",
            m_rotationSnapIntervalDraft, m_rotationSnapIntervalDegrees);

        ImGui::Separator();
        ImGui::TextDisabled("Snap is %s. Applies to viewport gizmo drags only.", m_snapEnabled ? "on" : "off");
        ImGui::EndCombo();
    }
}

Vector3 Editor::SnapGizmoTranslation(const Vector3& dragStart, const Vector3& continuousPosition) const
{
    if(!m_snapEnabled || !ViewportGridSnap::IsValidInterval(m_translationSnapInterval))
    {
        return continuousPosition;
    }

    return ViewportGridSnap::SnapTranslation(dragStart, continuousPosition, m_translationSnapInterval);
}

void Editor::UpdateGizmoRotationDrag(bool gizmoUsing, const Matrix4& gizmoModel, const Vector3& authoredRotation)
{
    if(!gizmoUsing || m_gizmoMode != GizmoMode::Rotate)
    {
        m_gizmoRotationDrag.reset();
        return;
    }

    // gizmoModel is in ImGuizmo's transposed layout; the tracker uses Engine's convention.
    const Matrix4 continuousRotation = ViewportGridSnap::RotationFromModel(gizmoModel.Transpose());

    if(!m_gizmoWasUsing || !m_gizmoRotationDrag)
    {
        // authoredRotation is still the drag-start value on the gesture's first frame.
        m_gizmoRotationDrag.emplace();
        m_gizmoRotationDrag->startRotation = authoredRotation;
        m_gizmoRotationDrag->tracker.Begin(Matrix4::Rotation(authoredRotation), continuousRotation);
    }
    else
    {
        m_gizmoRotationDrag->tracker.Update(continuousRotation);
    }

    m_gizmoRotationDrag->continuousModel = gizmoModel;
}

ViewportPreferences Editor::GetViewportPreferences() const
{
    ViewportPreferences preferences;
    preferences.snapEnabled = m_snapEnabled;
    preferences.translationSnapInterval = m_translationSnapInterval;
    preferences.rotationSnapIntervalDegrees = m_rotationSnapIntervalDegrees;
    preferences.gridVisible = m_showViewportGrid;
    return preferences;
}

void Editor::ApplyViewportPreferences(const ViewportPreferences& preferences)
{
    m_snapEnabled = preferences.snapEnabled;
    m_showViewportGrid = preferences.gridVisible;

    if(ViewportGridSnap::IsValidInterval(preferences.translationSnapInterval))
    {
        m_translationSnapInterval = preferences.translationSnapInterval;
    }

    if(ViewportGridSnap::IsValidInterval(preferences.rotationSnapIntervalDegrees))
    {
        m_rotationSnapIntervalDegrees = preferences.rotationSnapIntervalDegrees;
    }

    m_translationSnapIntervalDraft = m_translationSnapInterval;
    m_rotationSnapIntervalDraft = m_rotationSnapIntervalDegrees;
}

std::optional<float> Editor::GetViewportGridSpacing() const
{
    // The grid is an editing aid: hidden while Play/Pause simulates, restored on Stop. The
    // translation interval only ever holds a validated value, so invalid drafts keep the grid.
    if(!m_showViewportGrid || IsPhysicsEditingLocked())
    {
        return std::nullopt;
    }

    return m_translationSnapInterval;
}

bool Editor::IsRotationSnapActive() const
{
    return m_snapEnabled && ViewportGridSnap::IsValidInterval(m_rotationSnapIntervalDegrees);
}

Vector3 Editor::ResolveGizmoRotation(const Vector3& continuousRotationRadians) const
{
    if(!m_gizmoRotationDrag || !IsRotationSnapActive())
    {
        return continuousRotationRadians;
    }

    const auto& tracker = m_gizmoRotationDrag->tracker;

    // No snapped step yet: keep the authored start exactly rather than a re-decomposed copy.
    if(tracker.SnappedDeltaDegrees(m_rotationSnapIntervalDegrees) == 0.0)
    {
        return m_gizmoRotationDrag->startRotation;
    }

    return GroupTransform::Euler(tracker.SnappedRotation(m_rotationSnapIntervalDegrees));
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
            m_viewportImage = FitSceneViewportImage(m_viewportContentRegion, logicalSize);
            const ImVec2 position = m_viewportImage.position;
            const ImVec2 size = m_viewportImage.size;
            // Toolbar pixels are outside this authoritative image rectangle. Popups and
            // other windows still block scene input through the existing hover policy.
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const bool inside = mouse.x >= position.x && mouse.y >= position.y &&
                mouse.x < position.x + size.x && mouse.y < position.y + size.y;
            m_viewportHovered = inside && !m_workspace.IsSplitterDragging() &&
                !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) &&
                !ImGui::IsAnyItemActive() && !ImGui::GetDragDropPayload() &&
                !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);

            ImGui::GetWindowDrawList()->AddImage(
                ImTextureRef(static_cast<ImTextureID>(sceneTextureHandle)),
                position,
                ImVec2(position.x + size.x, position.y + size.y));

            if(!m_meshCreationStatus.empty())
            {
                ImGui::GetWindowDrawList()->AddText(ImVec2(position.x + 12.0f, position.y + 12.0f),
                    IM_COL32(240, 220, 170, 255), m_meshCreationStatus.c_str());
            }

            if(!m_sceneObjectDeletionStatus.empty())
            {
                const float offset = m_meshCreationStatus.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing();
                ImGui::GetWindowDrawList()->AddText(ImVec2(position.x + 12.0f, position.y + 12.0f + offset),
                    IM_COL32(240, 220, 170, 255), m_sceneObjectDeletionStatus.c_str());
            }

            if(!m_sceneHistoryStatus.empty())
            {
                const int lines = static_cast<int>(!m_meshCreationStatus.empty()) +
                    static_cast<int>(!m_sceneObjectDeletionStatus.empty());
                ImGui::GetWindowDrawList()->AddText(ImVec2(position.x + 12.0f,
                    position.y + 12.0f + lines * ImGui::GetTextLineHeightWithSpacing()),
                    IM_COL32(240, 220, 170, 255), m_sceneHistoryStatus.c_str());
            }
            if(!m_materialDropStatus.empty())
            {
                ImGui::GetWindowDrawList()->AddText(ImVec2(position.x + 12.0f,
                    position.y + size.y - ImGui::GetTextLineHeightWithSpacing() - 12.0f),
                    IM_COL32(240, 220, 170, 255), m_materialDropStatus.c_str());
            }

            if(m_showResolutionInfo)
            {
                // Below any status lines at the top-left; drawn into this input-less window, so it never takes input.
                const int statusLines = static_cast<int>(!m_meshCreationStatus.empty()) +
                    static_cast<int>(!m_sceneObjectDeletionStatus.empty()) + static_cast<int>(!m_sceneHistoryStatus.empty());
                DrawResolutionInfo(ImVec2(position.x + 8.0f,
                    position.y + 8.0f + statusLines * ImGui::GetTextLineHeightWithSpacing()));
            }
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
    ViewportCameraInput::ReserveLeftButton(m_cameraLooking, m_panLeftHeld);
    DrawMainMenu(demoScene, sceneManager, debugUI);
    DrawLifecycleWindows(demoScene);

    // Save is application-wide even when an input owns keyboard focus. The Engine applies
    // the same eligibility and persistence path as File -> Save Scene.
    if(ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) &&
        dynamic_cast<const FileBackedDemoScene*>(&demoScene))
    {
        QueueFileRequest(FileRequest{FileRequestType::Save, {}});
    }

    m_workspace.Update();
    const auto regions = CalculateSceneViewportRegions(m_workspace.GetSceneViewport(), ImGui::GetFrameHeight());
    m_viewportContentRegion = regions.imageArea;
    DrawSceneViewportToolbar(regions.toolbar);
    return m_viewportContentRegion;
}

bool Editor::RenameSessionMatches(const IDemoScene& scene, const SceneHistorySession& session) const
{
    return m_historySession && session.epoch == m_historySession->epoch &&
        session.projectIdentity == m_historySession->projectIdentity &&
        session.scenePath == m_historySession->scenePath && session.sceneIdentity == m_historySession->sceneIdentity &&
        CanPreserveSceneHistory(scene, session.projectIdentity, session.epoch);
}

bool Editor::RenameIntentMatches(const IDemoScene& scene) const
{
    if(!m_renameIntent || !RenameSessionMatches(scene, m_renameIntent->session))
    {
        return false;
    }

    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
    const auto& target = m_renameIntent->target;
    const auto current = owner ? owner->GetObjectMetadata(target.objectID) : std::nullopt;

    return current && current->runtimeIdentity == target.runtimeIdentity && current->sceneID == target.sceneID &&
        current->structureRevision == target.structureRevision && current->runtimeIndex == target.runtimeIndex &&
        current->name == target.name;
}

void Editor::CancelObjectRename()
{
    m_renameIntent.reset();
    m_renameStatus.clear();
    m_hierarchyPanel.CancelRename();
}

void Editor::QueueObjectRename(const IDemoScene& scene, const SceneObjectMetadata& target)
{
    if (IsPhysicsEditingLocked())
    {
        return;
    }

    if(!m_historySession || !SceneObjectDeletionMatches(scene, target) || m_materialAssignmentPending ||
        !RenameSessionMatches(scene, *m_historySession))
    {
        return;
    }

    RenameIntent intent{*m_historySession, target};
    m_hierarchyPanel.BeginRename(target.objectID, target.name);
    m_renameIntent = std::move(intent);

    m_renameStatus.clear();
    m_workspace.HierarchyVisible() = true;
}

void Editor::CommitObjectRename(IDemoScene& scene, std::string name)
{
    if (IsPhysicsEditingLocked())
    {
        return;
    }

    if(!RenameIntentMatches(scene) || HasPendingSceneHistoryOperation())
    {
        m_renameStatus = "Rename target belongs to a stale scene or pending operation.";
        return;
    }

    if(name == m_renameIntent->target.name)
    {
        m_renameStatus.clear();
        return;
    }

    if(!NormalizeSceneObjectName(name, &m_renameStatus))
    {
        return;
    }

    const auto& intent = *m_renameIntent;

    if(name == intent.target.name)
    {
        m_renameStatus.clear();
        return;
    }

    try
    {
        auto rename = std::make_unique<RenameObject>(RenameObject{
            intent.session, intent.target.objectID, intent.target.name, std::move(name)});
        size_t payloadBytes = sizeof(SceneHistoryEntry) + sizeof(RenameObject);
        payloadBytes += rename->objectID.capacity() + rename->before.capacity() + rename->after.capacity() + 3;
        payloadBytes += rename->session.sceneIdentity.capacity() + 1;
        payloadBytes += (rename->session.projectIdentity.native().capacity() +
            rename->session.scenePath.native().capacity() + 2) * sizeof(std::filesystem::path::value_type);

        if(payloadBytes > kSceneHistoryPayloadBytes)
        {
            m_renameStatus = "Rename exceeds the 4 MiB scene history payload limit.";
            return;
        }

        SceneHistoryEntry entry{std::move(rename), intent.session.epoch, payloadBytes};
        const auto& command = *std::get<std::unique_ptr<RenameObject>>(entry.command);
        m_sceneHistory.reserve(kSceneHistoryCapacity);
        auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

        if(!owner || !owner->RenameObject(intent.target, command.before, command.after, &m_renameStatus))
        {
            return;
        }

        // The document commit succeeded. Reserved, noexcept history publication follows.
        AppendSceneHistoryEntry(std::move(entry));
        m_renameStatus.clear();
    }
    catch(const std::exception& exception)
    {
        m_renameStatus = std::string("Rename could not be staged: ") + exception.what();
    }
}

void Editor::CommitObjectReorder(IDemoScene& scene, const HierarchyReorderRequest& request)
{
    if (IsPhysicsEditingLocked())
    {
        return;
    }

    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner || !m_historySession || !RenameSessionMatches(scene, *m_historySession) ||
        request.identity.sessionEpoch != m_historySession->epoch ||
        request.identity.runtimeIdentity != owner->GetRuntimeIdentity() ||
        request.identity.structureRevision != scene.GetScene().GetStructureRevision() ||
        HasPendingSceneHistoryOperation() || HasBlockingObjectCreation() || m_sceneOperationPending ||
        m_fileRequest || m_sceneObjectDeletionPending || m_sceneObjectDeletionRequest ||
        m_materialAssignmentRequest || m_meshCreationRequest || m_renameIntent || ImGui::GetIO().AppFocusLost)
    {
        m_reorderStatus = "Hierarchy drop rejected: scene identity or pending operations changed.";
        return;
    }

    try
    {
        SceneHierarchySnapshot snapshot;

        if(!owner->BuildHierarchySnapshot(snapshot, &m_reorderStatus))
        {
            return;
        }

        auto reorder = std::make_unique<ReorderObjects>();
        reorder->session = *m_historySession;
        reorder->before.reserve(snapshot.entries.size());

        for(const auto& entry : snapshot.entries)
        {
            reorder->before.push_back(entry.objectID);
        }

        reorder->after = reorder->before;
        const auto source = std::find(reorder->after.begin(), reorder->after.end(), request.sourceID);
        const auto destination = std::find(reorder->after.begin(), reorder->after.end(), request.destinationID);

        if(source == reorder->after.end() || destination == reorder->after.end())
        {
            m_reorderStatus = "Hierarchy drop target no longer exists.";
            return;
        }

        if(source == destination)
        {
            m_reorderStatus.clear();
            return;
        }

        reorder->after.erase(source);
        const auto insertion = std::find(reorder->after.begin(), reorder->after.end(), request.destinationID);
        reorder->after.insert(request.insertAfter ? insertion + 1 : insertion, request.sourceID);

        if(reorder->before == reorder->after)
        {
            m_reorderStatus.clear();
            return;
        }

        size_t payloadBytes = sizeof(SceneHistoryEntry) + sizeof(ReorderObjects);
        payloadBytes += (reorder->before.capacity() + reorder->after.capacity()) * sizeof(std::string);

        for(const auto* order : {&reorder->before, &reorder->after})
        {
            for(const auto& id : *order)
            {
                payloadBytes += id.capacity() + 1;
            }
        }

        payloadBytes += reorder->session.sceneIdentity.capacity() + 1;
        payloadBytes += (reorder->session.projectIdentity.native().capacity() +
            reorder->session.scenePath.native().capacity() + 2) * sizeof(std::filesystem::path::value_type);

        if(payloadBytes > kSceneHistoryPayloadBytes)
        {
            m_reorderStatus = "Hierarchy reorder exceeds the 4 MiB scene history payload limit.";
            return;
        }

        SceneHistoryEntry entry{std::move(reorder), m_historySession->epoch, payloadBytes};
        const auto& command = *std::get<std::unique_ptr<ReorderObjects>>(entry.command);
        const SceneObjectOrderIdentity identity{std::string(owner->GetPersistentSceneID()),
            owner->GetRuntimeIdentity(), scene.GetScene().GetStructureRevision()};
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(!owner->ReorderObjects(identity, command.before, command.after, &m_reorderStatus))
        {
            return;
        }

        AppendSceneHistoryEntry(std::move(entry));
        m_reorderStatus.clear();
    }
    catch(const std::exception& exception)
    {
        m_reorderStatus = std::string("Hierarchy reorder could not be staged: ") + exception.what();
    }
}

void Editor::DrawHierarchy(IDemoScene& demoScene, const std::string& activeSceneName)
{
    const auto& scene = demoScene.GetScene();
    SceneHierarchySnapshot snapshot;
    snapshot.sceneName = activeSceneName;
    std::string error;

    if(const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&demoScene))
    {
        owner->BuildHierarchySnapshot(snapshot, &error);
    }
    else
    {
        const auto& objects = scene.GetRenderObjects();
        snapshot.entries.reserve(objects.size());

        for(size_t index = 0; index < objects.size(); ++index)
        {
            snapshot.entries.push_back({{}, "Object " + std::to_string(index), index});
        }
    }

    const std::optional<size_t> selected = m_selectedObject == InvalidObjectSelection ?
        std::nullopt : std::optional<size_t>{m_selectedObject};
    m_hierarchyPanel.ObserveSelection(snapshot, selected, m_historyEpoch, &scene);

    if(!m_workspace.HierarchyVisible())
    {
        CancelObjectRename();
        return;
    }

    const bool blocked = m_sceneOperationPending || m_fileRequest || m_materialLeaveAction != MaterialLeaveAction::None ||
        HasPendingSceneHistoryOperation() || m_sceneObjectDeletionPending || m_sceneObjectDeletionRequest ||
        m_materialAssignmentPending || m_materialAssignmentRequest || m_gizmoWasUsing ||
        m_workspace.IsSplitterDragging() || ImGuizmo::IsUsing() || m_transformFieldEditing;

    if(m_renameIntent && (IsPhysicsEditingLocked() || blocked || m_inspectorPanel.HasUnsavedChanges() || !RenameIntentMatches(demoScene)))
    {
        CancelObjectRename();
    }

    if(error.empty())
    {
        error = m_renameStatus.empty() ? m_reorderStatus : m_renameStatus;
    }

    const auto* fileScene = dynamic_cast<const FileBackedDemoScene*>(&demoScene);
    const bool allowReorder = !IsPhysicsEditingLocked() && !blocked && fileScene && m_historySession &&
        RenameSessionMatches(demoScene, *m_historySession) && !m_inspectorPanel.HasUnsavedChanges() &&
        !m_renameIntent && !ImGui::GetIO().AppFocusLost &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const HierarchyDragIdentity dragIdentity{m_historyEpoch,
        fileScene ? fileScene->GetRuntimeIdentity() : 0, scene.GetStructureRevision(),
        fileScene ? fileScene->m_runtime->GetHierarchyRevision() : 0};

    ImGui::BeginDisabled(blocked);
    std::unordered_map<std::string_view, EditorIcon> objectIcons;

    if(fileScene && fileScene->m_runtime->GetFileScene().HasDocument())
    {
        const auto& objects = fileScene->m_runtime->GetFileScene().GetDocument().objects;
        objectIcons.reserve(objects.size());

        for(const auto& object : objects)
        {
            objectIcons.emplace(object.id, SceneObjectIcon(object.camera.has_value(), object.light ? &*object.light : nullptr));
        }
    }

    const auto clicked = m_hierarchyPanel.Draw(m_workspace.GetHierarchy(), snapshot, selected, &scene, error,
        dragIdentity, allowReorder,
        !IsPhysicsEditingLocked() && !blocked && fileScene && !fileScene->IsArchiveActivation() &&
        m_historySession && RenameSessionMatches(demoScene,*m_historySession) &&
        !m_inspectorPanel.HasUnsavedChanges() && !m_renameIntent, m_selectedObjects,
        [&](const HierarchyDropRequest& request, std::string& reason)
        {
            SceneHierarchyEdit preview;
            return PrepareHierarchyDrop(demoScene, request, preview, reason);
        }, [&](std::string_view id)
        {
            const auto found = objectIcons.find(id);
            return found == objectIcons.end() ? EditorIcon::Box : found->second;
        });
    ImGui::EndDisabled();

    if(auto primitive=m_hierarchyPanel.TakePrimitiveRequest())
    {
        if(!IsPhysicsEditingLocked() && !blocked && fileScene && !fileScene->IsArchiveActivation() &&
            m_historySession && RenameSessionMatches(demoScene,*m_historySession) && !HasBlockingObjectCreation())
            m_primitiveCreationRequest=PrimitiveCreationRequest{*primitive,fileScene->GetRuntimeIdentity(),m_historyEpoch};
    }

    // Same eligibility as primitive creation; Engine re-checks everything before staging.
    if(m_hierarchyPanel.TakeEmptyObjectRequest())
    {
        const bool canCreate = !IsPhysicsEditingLocked() && !blocked && fileScene && !fileScene->IsArchiveActivation() &&
            m_historySession && RenameSessionMatches(demoScene, *m_historySession) && !HasBlockingObjectCreation();

        if(canCreate)
        {
            m_emptyObjectCreationRequest = EmptyObjectCreationRequest{fileScene->GetRuntimeIdentity(), m_historyEpoch};
        }
    }

    if(m_hierarchyPanel.TakeCameraObjectRequest())
    {
        const bool canCreate = !IsPhysicsEditingLocked() && !blocked && fileScene && !fileScene->IsArchiveActivation() &&
            m_historySession && RenameSessionMatches(demoScene, *m_historySession) && !HasBlockingObjectCreation();

        if(canCreate)
        {
            m_cameraObjectCreationRequest = CameraObjectCreationRequest{fileScene->GetRuntimeIdentity(), m_historyEpoch};
        }
    }

    if(const auto lightType = m_hierarchyPanel.TakeLightObjectRequest())
    {
        const bool canCreate = !IsPhysicsEditingLocked() && !blocked && fileScene && !fileScene->IsArchiveActivation() &&
            m_historySession && RenameSessionMatches(demoScene, *m_historySession) && !HasBlockingObjectCreation();

        if(canCreate)
        {
            m_lightObjectCreationRequest =
                LightObjectCreationRequest{*lightType, fileScene->GetRuntimeIdentity(), m_historyEpoch};
        }
    }

    if(auto drop = m_hierarchyPanel.TakeDropRequest())
    {
        if(allowReorder)
        {
            try
            {
                if(SubmitHierarchyDrop(demoScene, *drop, m_reorderStatus))
                {
                    m_reorderStatus.clear();
                }
            }
            catch(const std::exception& exception)
            {
                m_reorderStatus = exception.what();
            }
        }
    }

    if(auto objectID = m_hierarchyPanel.TakeContextRequest())
    {
        const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&demoScene);
        const auto target = owner ? owner->GetObjectMetadata(*objectID) : std::nullopt;

        if(!IsPhysicsEditingLocked() && !blocked && target)
        {
            OpenSceneObjectContext(demoScene, *target);
        }
    }

    if(auto name = m_hierarchyPanel.TakeRenameCommit())
    {
        if(!IsPhysicsEditingLocked() && !blocked)
        {
            CommitObjectRename(demoScene, std::move(*name));
        }
    }

    if(m_renameIntent && !m_hierarchyPanel.IsRenaming())
    {
        m_renameIntent.reset();
    }

    if(!blocked && clicked && *clicked < scene.GetRenderObjects().size())
    {
        const auto& io = ImGui::GetIO();
        const bool toggleSelectionMembership = io.KeyShift || io.KeyCtrl;
        SelectSceneObject(demoScene, *clicked, toggleSelectionMembership);
        m_objectInspectionRequested = true;
    }
}

void Editor::ActivateObjectInspection(
    IDemoScene& demoScene, const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    DismissAddComponentPopup();
    DismissTriggerManagementPopup();
    DismissControllerManagementPopup();
    DismissCameraManagementPopup();
    DismissLightManagementPopup();
    m_objectInspectionIdentity.reset();
    m_transformEditStatus.clear();
    m_physicsInspection.reset();
    m_triggerInspection.reset();
    m_controllerInspection.reset();
    m_cameraInspection.reset();
    m_lightInspection.reset();
    m_inspectorTarget = InspectorTarget::None;
    m_inspectorPanel.LoadSelection({}, paths, registry);
    const auto& scene = static_cast<const IDemoScene&>(demoScene).GetScene();

    if(m_selectedObject >= scene.GetRenderObjects().size())
    {
        return;
    }

    ObjectInspectionIdentity identity{&scene, scene.GetStructureRevision()};

    if(const auto* fileScene = dynamic_cast<const FileBackedDemoScene*>(&demoScene))
    {
        const auto metadata = fileScene->GetObjectMetadata(m_selectedObject);

        if(!metadata)
        {
            return;
        }

        identity.runtimeIdentity = metadata->runtimeIdentity;
        identity.objectID = metadata->objectID;
        identity.sceneID = metadata->sceneID;
    }

    m_objectInspectionIdentity = std::move(identity);
    m_inspectorTarget = InspectorTarget::SceneObject;
}

std::optional<SceneObjectInspectorView> Editor::ResolveObjectInspection(
    const IDemoScene& demoScene, const ProjectPaths& paths, const ProjectAssetRegistry& registry) const
{
    const auto& scene = demoScene.GetScene();
    const auto& objects = scene.GetRenderObjects();

    if(!m_objectInspectionIdentity || m_objectInspectionIdentity->scene != &scene ||
       m_objectInspectionIdentity->revision != scene.GetStructureRevision() || m_selectedObject >= objects.size())
    {
        return std::nullopt;
    }

    const auto& object = objects[m_selectedObject];
    SceneObjectInspectorView view{"Object " + std::to_string(m_selectedObject), object.transform,
        object.mesh ? "Runtime mesh (document reference unavailable)" : "No mesh",
        "Runtime material (document reference unavailable)"};
    const auto* fileScene = dynamic_cast<const FileBackedDemoScene*>(&demoScene);

    if(object.mesh)
    {
        for(const auto& slot : object.mesh->GetMetadata().materialSlots)
        {
            view.materialSlots.push_back({slot.id, slot.name, "Runtime material", {}, false,
                object.GetSlotMaterial(slot.id) != nullptr});
        }
    }

    if(!fileScene)
    {
        if(m_selectedObjects.size() > 1)
            view.name += " (active; " + std::to_string(m_selectedObjects.size()) + " selected)";
        return view;
    }

    const auto metadata = fileScene->GetObjectMetadata(m_selectedObject);

    if(!metadata || metadata->runtimeIdentity != m_objectInspectionIdentity->runtimeIdentity ||
       metadata->structureRevision != m_objectInspectionIdentity->revision ||
       metadata->objectID != m_objectInspectionIdentity->objectID || metadata->sceneID != m_objectInspectionIdentity->sceneID)
    {
        return std::nullopt;
    }

    const auto label = [&](const std::string& reference, AssetType type) -> std::string
    {
        if(reference.empty())
        {
            return type == AssetType::Mesh ? "No mesh" : "Default Material";
        }

        if(reference == "builtin:sphere" && type == AssetType::Mesh)
        {
            return "Sphere";
        }

        if(reference == "builtin:default" && type == AssetType::Material)
        {
            return "Default Material";
        }

        const auto id = AssetID::ParseReference(reference);
        const auto* record = id ? registry.Find(*id) : nullptr;

        if(!record || record->type != type)
        {
            return type == AssetType::Mesh ? "Unavailable mesh reference" : "Unavailable material reference";
        }

        const auto path = record->relativePath.generic_string();
        return ProjectAssetResolver::Resolve(paths, registry, *id).status == AssetAvailability::Available ?
            record->relativePath.stem().generic_string() : "Unavailable: " + path;
    };

    view.metadata = metadata;
    view.name = metadata->name;
    if(m_selectedObjects.size() > 1)
    {
        view.name += " (active; " + std::to_string(m_selectedObjects.size()) + " selected)";
    }
    view.mesh = label(metadata->meshReference, AssetType::Mesh);
    view.material = label(metadata->materialReference, AssetType::Material);
    view.meshReference = metadata->meshReference;
    view.meshResidency = metadata->GetMeshResidency(object);
    view.materialSlotMetadataAvailable = object.mesh != nullptr;

    if (!object.mesh && !metadata->meshReference.empty())
    {
        if (const auto* slots = fileScene->GetPendingSlotMetadata(metadata->meshReference))
        {
            view.materialSlotMetadataAvailable = true;

            for (const auto& slot : slots->materialSlots)
            {
                view.materialSlots.push_back({slot.id, slot.name, "Pending material", {}, false,
                    object.GetSlotMaterial(slot.id) != nullptr});
            }
        }
    }


    for(auto& slot : view.materialSlots)
    {
        const auto binding = metadata->materialSlotBindings.find(slot.id);
        slot.explicitBinding = binding != metadata->materialSlotBindings.end();
        slot.materialReference = slot.explicitBinding ? binding->second : metadata->materialReference;
        slot.materialName = label(slot.materialReference, AssetType::Material);
    }

    return view;
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

    m_hierarchyPanel.SetIcons(debugUI.GetEditorIcons());
    m_assetBrowser.SetIcons(debugUI.GetEditorIcons());
    m_inspectorPanel.SetIcons(debugUI.GetEditorIcons());
    Scene& scene = demoScene.GetScene();
    const std::string activeSceneName(sceneManager.GetActiveSceneName());
    const auto* dragScene = dynamic_cast<const FileBackedDemoScene*>(&demoScene);
    const auto dragRuntimeIdentity = dragScene ? dragScene->GetRuntimeIdentity() : 0;

    if(dragRuntimeIdentity != m_meshDragRuntimeIdentity)
    {
        m_assetBrowser.InvalidateMeshDragIdentity();
        m_meshDragRuntimeIdentity = dragRuntimeIdentity;
    }

    UpdateSceneHistorySession(demoScene, projectPaths, activeSceneName);

    const uint64_t structureRevision = scene.GetStructureRevision();

    if (m_selectionSceneName != activeSceneName || m_selectionStructureRevision != structureRevision ||
        m_selectionScene != &scene || m_selectionRuntimeIdentity != dragRuntimeIdentity)
    {
        ResetTransformGestures();

        const bool preserveIDs = m_selectionScene == &scene && m_selectionSceneName == activeSceneName &&
            m_selectionRuntimeIdentity == dragRuntimeIdentity && dragScene;
        if(!preserveIDs)
        {
            m_orderedSelection.clear();
            m_selectedObjects.clear();
            m_selectedObject = InvalidObjectSelection;
        }
        m_objectInspectionRequested = true;
        m_selectionSceneName = activeSceneName;
        m_selectionStructureRevision = structureRevision;

        m_selectionScene = &scene;
        m_selectionRuntimeIdentity = dragRuntimeIdentity;
    }

    const Scene& constScene = scene;
    const auto& objects = constScene.GetRenderObjects();

    if (m_selectedObject >= objects.size())
    {
        m_selectedObject = InvalidObjectSelection;
    }

    RefreshSceneSelection(demoScene);
    DrawHierarchy(demoScene, activeSceneName);
    if(m_selectionStructureRevision != scene.GetStructureRevision())
    {
        ResetTransformGestures();
        m_selectionStructureRevision = scene.GetStructureRevision();
        RefreshSceneSelection(demoScene);
        m_objectInspectionRequested = true;
    }

    (void)debugUI;
    m_assetBrowser.SetThumbnailProject(projectPaths, assetRegistry);
    ImGui::BeginDisabled(IsPhysicsEditingLocked() || m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending);

    if(m_workspace.AssetBrowserVisible())
    {
        m_assetBrowser.Draw(m_workspace.GetAssetBrowser(), m_activeProject,
            dynamic_cast<const FileBackedDemoScene*>(&demoScene));
    }
    else
    {
        m_assetBrowser.PauseMaterialThumbnails();
    }

    ImGui::EndDisabled();

    if(const auto scenePath = m_assetBrowser.TakeSceneOpenRequest())
    {
        QueueFileRequest(FileRequest{FileRequestType::Load, *scenePath});
    }

    const bool assetSelectionIntent = m_assetBrowser.TakeSelectionIntent();

    const bool assetTargetChanged = m_inspectorTarget != InspectorTarget::Asset ||
        m_assetBrowser.GetSelection() != m_inspectedAssetSelection;

    if(!m_fileRequest && !m_sceneOperationPending && m_materialLeaveAction == MaterialLeaveAction::None && assetTargetChanged &&
       (assetSelectionIntent || (m_inspectorTarget == InspectorTarget::Asset &&
        m_assetBrowser.GetSelection() != m_inspectedAssetSelection)))
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
            m_inspectorTarget = m_inspectedAssetSelection.empty() ? InspectorTarget::None : InspectorTarget::Asset;
            m_objectInspectionIdentity.reset();
        }

        m_objectInspectionRequested = false;
    }

    if(m_objectInspectionRequested && m_materialLeaveAction == MaterialLeaveAction::None)
    {
        m_objectInspectionRequested = false;

        if(m_inspectorPanel.HasUnsavedChanges())
        {
            m_materialLeaveAction = MaterialLeaveAction::ObjectSelection;
        }
        else
        {
            ActivateObjectInspection(demoScene, projectPaths, assetRegistry);
        }
    }

    const auto objectView = m_inspectorTarget == InspectorTarget::SceneObject ?
        ResolveObjectInspection(demoScene, projectPaths, assetRegistry) : std::nullopt;

    if(m_inspectorTarget == InspectorTarget::SceneObject && !objectView)
    {
        m_inspectorTarget = InspectorTarget::None;
        m_objectInspectionIdentity.reset();
    }

    // A disappearing control may never receive IsItemDeactivatedAfterEdit. Finalize
    // against its original object, never against the newly selected object.
    if(m_transformFieldEditing && (m_transformFieldUndoObject != m_selectedObject ||
       m_transformFieldScene != &scene || m_transformFieldRevision != scene.GetStructureRevision() ||
       !objectView || !m_workspace.InspectorVisible() || m_materialLeaveAction != MaterialLeaveAction::None ||
       m_sceneOperationPending))
    {
        FinishTransformFieldEdit(demoScene);
    }

    const bool disableInspector = IsPhysicsEditingLocked() || m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending ||
        m_pendingMaterialBinding.has_value();

    ValidateAddComponentPopup(demoScene);
    ValidateTriggerManagementPopup(demoScene);
    ValidateControllerManagementPopup(demoScene);

    // This ImGui version's color picker restores alpha whenever a disabled scope exists,
    // including BeginDisabled(false), whose alpha backup may never have been initialized.
    if(disableInspector)
    {
        ImGui::BeginDisabled();
    }

    if(m_materialAssignmentSelection && (!objectView || !objectView->metadata ||
       objectView->metadata->runtimeIdentity != m_materialAssignmentSelection->runtimeIdentity ||
       objectView->metadata->objectID != m_materialAssignmentSelection->objectID ||
       objectView->metadata->structureRevision != m_materialAssignmentSelection->structureRevision))
    {
        m_materialAssignmentStatus.clear();
    }

    if(objectView || !m_workspace.InspectorVisible() || disableInspector)
    {
        m_inspectorPanel.DismissTexturePicker();
    }

    if(m_workspace.InspectorVisible())
    {
        if(objectView)
        {
            // The controller section re-offers its object to the drive toggle every frame.
            m_characterDriveCandidate.reset();
            m_inspectorPanel.DrawSceneObject(m_workspace.GetInspector(), *objectView, [this, &demoScene]()
            {
                DrawSelectedObjectTransformControls(demoScene);
            }, projectPaths, assetRegistry, m_materialAssignmentPending, m_materialAssignmentStatus,
            [this, &demoScene]()
            {
                DrawObjectPhysicsControls(demoScene);
                DrawObjectTriggerColliderControls(demoScene);
                DrawObjectCharacterControllerControls(demoScene);
                DrawObjectCameraControls(demoScene);
                DrawObjectLightControls(demoScene);
                DrawAddComponentControls(demoScene);
            });

            if(!m_materialAssignmentPending && !m_inspectorPanel.GetSlotPickerStatus().empty())
            {
                m_materialAssignmentSelection = objectView->metadata;
                m_materialAssignmentStatus = m_inspectorPanel.GetSlotPickerStatus();
            }

            if(auto request = m_inspectorPanel.TakeSlotAssignmentRequest())
            {
                m_materialAssignmentSelection = request->selection;
                m_materialAssignmentSlotID = request->slotID;
                m_materialAssignmentRequest = MaterialAssignmentRequest{
                    request->selection, {}, request->slotID, request->materialID};

                m_materialAssignmentPending = true;
                m_materialAssignmentStatus = "Pending material-slot assignment.";
            }
        }
        else
        {
            m_inspectorPanel.Draw(
                m_workspace.GetInspector(), projectPaths, assetRegistry,
                materialPreview,
                m_materialLeaveAction != MaterialLeaveAction::None);
        }
    }

    if(disableInspector)
    {
        ImGui::EndDisabled();
    }

    if(!objectView || !m_workspace.InspectorVisible())
    {
        m_physicsInspection.reset();
        m_triggerInspection.reset();
        m_controllerInspection.reset();
        m_cameraInspection.reset();
        m_lightInspection.reset();
        m_inspectorPanel.ReleaseSceneObjectPreview();
        m_inspectorPanel.DismissSceneMaterialPicker();
    }

    ProcessMaterialLocateRequest(projectPaths,assetRegistry);

    if(m_inspectorPanel.TakeRefreshRequest() && m_materialLeaveAction == MaterialLeaveAction::None &&
       m_inspectorTarget == InspectorTarget::Asset)
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

    DrawMaterialLeaveConfirmation(demoScene, projectPaths, assetRegistry);

    for(auto& working : m_inspectorPanel.TakeWorkingMaterialRevisions(
        m_workspace.InspectorVisible() && m_inspectorTarget == InspectorTarget::Asset))
    {
        const auto* fileScene = dynamic_cast<const FileBackedDemoScene*>(&demoScene);
        const auto versions = fileScene ? fileScene->GetMaterialVersions(working.projectIdentity, working.assetID) : std::nullopt;

        if(versions && !versions->versions.empty())
        {
            m_workingMaterialRevisions.push_back({std::move(working), *versions});
        }
    }

    for(auto& saved : m_inspectorPanel.TakeSavedMaterialRevisions())
    {
        // Bind the owned receipt to this scene instance, never to an object selection.
        const auto* fileScene = dynamic_cast<const FileBackedDemoScene*>(&demoScene);

        if(fileScene)
        {
            const auto versions = fileScene->GetMaterialVersions(saved.projectIdentity, saved.assetID);

            if(versions && !versions->versions.empty())
            {
                m_savedMaterialRevisions.push_back({std::move(saved), versions->catalogIdentity,
                    fileScene->GetRuntimeIdentity(), scene.GetStructureRevision()});
            }
        }
    }

    if(const auto savedSource = m_inspectorPanel.TakeSavedMaterialSource())
    {
        m_assetBrowser.InvalidateMaterialSource(*savedSource);
    }

    m_assetBrowser.DrawRenameModal(IsPhysicsEditingLocked() || m_inspectorPanel.HasUnsavedChanges() ||
        m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending);

    m_assetBrowser.DrawDeletionConfirmation(IsPhysicsEditingLocked() || m_inspectorPanel.HasUnsavedChanges() ||
        m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending);

    DrawBuildGame(demoScene);
    DrawSceneViewport(sceneTextureHandle, sceneWidth, sceneHeight);
    DrawMeshDropTarget(demoScene, projectPaths, assetRegistry);
    DrawMaterialDropTarget(demoScene, projectPaths, assetRegistry);

    const ImGuiIO& io = ImGui::GetIO();

    const bool viewportWasFocused = m_viewportFocused;
    m_viewportFocused = ViewportCameraInput::UpdateFocus(m_viewportFocused, m_cameraLooking, m_viewportHovered);

    if(io.AppFocusLost || m_sceneOperationPending || m_viewportImage.size.x <= 0.0f || m_workspace.IsSplitterDragging())
    {
        m_viewportFocused = false;
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

    // Projection is driven by the actual render extent, including after Inspector controls.
    scene.GetCamera().SetAspectRatio(static_cast<float>(sceneWidth) / static_cast<float>(sceneHeight));

    HandleSceneObjectDuplication(demoScene);
    HandleTransformUndo(demoScene);
    HandleTransformRedo(demoScene);

    UpdateCameraLook(demoScene, m_viewportImage.size.x, m_viewportImage.size.y);
    UpdateCameraMovement(demoScene, deltaTime);
    DrawViewportTools(demoScene, debugUI.GetEditorIcons(), viewportWasFocused);
    DrawSceneObjectDeletionControls(demoScene);
    UpdateCharacterDriveInput(demoScene);
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

    // LIGHT.1 migration: Directional Light components replace this legacy scene sun entirely.
    const auto* fileScene = dynamic_cast<const FileBackedDemoScene*>(&demoScene);

    if(fileScene && fileScene->m_runtime->GetFileScene().HasDocument())
    {
        const auto& objects = fileScene->m_runtime->GetFileScene().GetDocument().objects;
        const bool overridden = std::any_of(objects.begin(), objects.end(), [](const SceneObjectDocument& object)
        {
            return object.light && object.light->type == LightType::Directional;
        });

        if(overridden)
        {
            ImGui::TextDisabled("Inactive: this scene's Directional Light components replace the legacy sun.");
        }
    }

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
            SelectSceneObject(demoScene, InvalidObjectSelection);
            m_objectInspectionRequested = true;
        }

        for (size_t i = 0; i < objects.size(); ++i)
        {
            const std::string label = "Object " + std::to_string(i);

            const bool selected = m_selectedObject == i;

            if (ImGui::Selectable(label.c_str(), selected))
            {
                SelectSceneObject(demoScene, i);
                m_objectInspectionRequested = true;
            }

            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    ImGui::Text("Objects: %zu", objects.size());

}

void Editor::DrawSelectedObjectTransformControls(IDemoScene& demoScene)
{
    Scene& scene = demoScene.GetScene();
    const auto& objects = static_cast<const Scene&>(scene).GetRenderObjects();

    if(m_selectedObject >= objects.size())
    {
        return;
    }

    ImGui::BeginDisabled(IsPhysicsEditingLocked());
    Transform transform = objects[m_selectedObject].transform;

    bool transformChanged = false;

    const Transform originalTransform = transform;

    bool fieldActivated = false;

    bool fieldDeactivated = false;

    ImGui::TextUnformatted("Transform");
    ImGui::PushID(&scene);
    ImGui::PushID(static_cast<int>(m_selectedObject));
    ImGui::PushID("SelectedObjectTransform");

    transformChanged |= ImGui::DragFloat3("Position", &transform.position.x, 0.05f);

    fieldActivated |= ImGui::IsItemActivated();
    fieldDeactivated |= ImGui::IsItemDeactivatedAfterEdit();

    float rotationDegrees[3] = {ToDegrees(transform.rotation.x), ToDegrees(transform.rotation.y),
                                ToDegrees(transform.rotation.z)};

    if(ImGui::DragFloat3("Rotation", rotationDegrees, 0.05))
    {
        transform.rotation =
            Vector3{ToRadians(rotationDegrees[0]), ToRadians(rotationDegrees[1]), ToRadians(rotationDegrees[2])};

        transformChanged = true;
    }

    fieldActivated |= ImGui::IsItemActivated();
    fieldDeactivated |= ImGui::IsItemDeactivatedAfterEdit();

    if(ImGui::DragFloat3("Scale", &transform.scale.x, 0.01f, 0.01f, 100.0f))
    {
        transform.scale.x = (std::max)(0.01f, transform.scale.x);
        transform.scale.y = (std::max)(0.01f, transform.scale.y);
        transform.scale.z = (std::max)(0.01f, transform.scale.z);

        transformChanged = true;
    }

    fieldActivated |= ImGui::IsItemActivated();
    fieldDeactivated |= ImGui::IsItemDeactivatedAfterEdit();

    if(fieldActivated && !m_transformFieldEditing)
    {
        m_transformFieldEditing = true;
        m_transformFieldUndoObject = m_selectedObject;
        m_transformFieldUndoStart = originalTransform;
        m_transformFieldScene = &scene;

        m_transformFieldRevision = scene.GetStructureRevision();
        m_fieldHistoryTarget = CaptureHistoryTarget(demoScene, m_selectedObject);
        m_fieldHistoryEpoch = m_historyEpoch;
    }

    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();

    const auto finite = [](const Vector3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };

    if(!IsPhysicsEditingLocked() && m_fieldHistoryTarget && transformChanged && finite(transform.position) && finite(transform.rotation) && finite(transform.scale))
    {
        ApplyObjectTransformEdit(demoScene, m_selectedObject, transform);
    }

    if(fieldDeactivated && m_transformFieldEditing)
    {
        FinishTransformFieldEdit(demoScene);
    }

    if(!m_transformEditStatus.empty())
    {
        ImGui::TextWrapped("%s", m_transformEditStatus.c_str());
    }

    ImGui::EndDisabled();
}

void Editor::FinishTransformFieldEdit(IDemoScene& demoScene)
{
    Scene& scene = demoScene.GetScene();

    if(m_transformFieldEditing && m_fieldHistoryTarget && m_transformFieldScene == &scene &&
        m_transformFieldRevision == scene.GetStructureRevision())
    {
        const auto index = ResolveHistoryTarget(demoScene, *m_fieldHistoryTarget);

        if(index)
        {
            RecordTransformEdit(std::move(*m_fieldHistoryTarget), m_fieldHistoryEpoch, m_transformFieldUndoStart,
                static_cast<const Scene&>(scene).GetRenderObjects()[*index].transform);
        }
    }

    m_transformFieldEditing = false;
    m_transformFieldUndoObject = InvalidObjectSelection;
    m_transformFieldScene = nullptr;
    m_transformFieldRevision = 0;

    m_fieldHistoryTarget.reset();
}

void Editor::SelectSceneObject(const IDemoScene& demoScene, size_t index, bool toggle)
{
    if(ImGuizmo::IsUsing() || m_gizmoWasUsing) return;
    std::optional<SelectedObjectIdentity> picked;
    if(index < demoScene.GetScene().GetRenderObjects().size())
    {
        SelectedObjectIdentity identity;
        if(const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&demoScene))
        {
            const auto metadata = owner->GetObjectMetadata(index);
            if(!metadata) return;
            identity.objectID = metadata->objectID;
        }
        else
        {
            identity.proceduralIndex = index;
        }
        picked = std::move(identity);
    }
    SceneSelection::Select(m_orderedSelection, std::move(picked), toggle);
    // An empty ordered list must not be repopulated from the old active index.
    m_selectedObject = InvalidObjectSelection;
    RefreshSceneSelection(demoScene);
    m_objectInspectionRequested = true;
}

void Editor::RefreshSceneSelection(const IDemoScene& demoScene)
{
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&demoScene);
    const auto& objects = demoScene.GetScene().GetRenderObjects();
    const auto previous = m_selectedObject;
    // Compatibility with checked engine publication of a new single selection.
    if(m_orderedSelection.empty() && m_selectedObject < objects.size())
    {
        if(owner)
        {
            if(const auto metadata = owner->GetObjectMetadata(m_selectedObject))
                m_orderedSelection.push_back({metadata->objectID});
        }
        else m_orderedSelection.push_back({{}, m_selectedObject});
    }
    m_selectedObjects.clear();
    m_selectedObjects.reserve(m_orderedSelection.size());
    std::erase_if(m_orderedSelection, [&](const SelectedObjectIdentity& identity)
    {
        if(owner)
        {
            const auto metadata = owner->GetObjectMetadata(identity.objectID);
            if(!metadata || metadata->runtimeIndex >= objects.size()) return true;
            m_selectedObjects.push_back(metadata->runtimeIndex);
        }
        else
        {
            if(identity.objectID.size() || identity.proceduralIndex >= objects.size()) return true;
            m_selectedObjects.push_back(identity.proceduralIndex);
        }
        return false;
    });
    m_selectedObject = m_selectedObjects.empty() ? InvalidObjectSelection : m_selectedObjects.back();
    if(m_selectedObject != previous) m_objectInspectionRequested = true;
}

void Editor::UpdateGroupGizmo(IDemoScene& demoScene, bool usingGizmo, const Transform& anchorAfter)
{
    if(usingGizmo && !m_gizmoWasUsing)
    {
        try
        {
            auto command = std::make_unique<GroupTransformEdit>();
            command->mode = m_gizmoMode;
            command->edits.reserve(m_selectedObjects.size());
            size_t bytes = sizeof(SceneHistoryEntry) + sizeof(GroupTransformEdit) +
                m_selectedObjects.size() * sizeof(TransformEdit);
            const auto& objects = static_cast<const Scene&>(demoScene.GetScene()).GetRenderObjects();
            for(const auto index : m_selectedObjects)
            {
                auto target = CaptureHistoryTarget(demoScene, index);
                if(!target) { m_gizmoWasUsing = true; return; }
                if(const auto* persistent = std::get_if<PersistentHistoryTarget>(&*target))
                    bytes += persistent->objectID.capacity() + 1;
                command->edits.push_back({std::move(*target), objects[index].transform, objects[index].transform});
            }
            if(bytes > kSceneHistoryPayloadBytes || HasPendingSceneHistoryOperation())
            {
                ReportSceneHistoryFailure("Group transform exceeds history capacity or another operation is pending.");
                m_gizmoWasUsing = true;
                return;
            }
            m_sceneHistory.reserve(kSceneHistoryCapacity);
            m_gizmoHistoryEpoch = m_historyEpoch;
            m_gizmoGroupPayloadBytes = bytes;
            m_gizmoGroupEdit = std::move(command);
        }
        catch(const std::exception& exception)
        {
            ReportSceneHistoryFailure(exception.what());
            m_gizmoWasUsing = true;
            return;
        }
    }

    if(usingGizmo && m_gizmoGroupEdit)
    {
        try
        {
            std::vector<Transform> before, after;
            std::vector<RenderObjectTransformUpdate> updates;
            for(const auto& edit : m_gizmoGroupEdit->edits) before.push_back(edit.before);
            const auto mode = m_gizmoGroupEdit->mode == GizmoMode::Translate ? GroupTransform::Mode::Translate :
                m_gizmoGroupEdit->mode == GizmoMode::Rotate ? GroupTransform::Mode::Rotate : GroupTransform::Mode::Scale;
            std::string error;
            if(!GroupTransform::Calculate(mode, before.back(), anchorAfter, before, after, error))
            {
                if(m_sceneHistoryStatus != "Scene history: " + error) ReportSceneHistoryFailure(error);
            }
            else
            {
                for(size_t index = 0; index < after.size(); ++index)
                {
                    const auto resolved = ResolveHistoryTarget(demoScene, m_gizmoGroupEdit->edits[index].target);
                    if(!resolved) { m_gizmoWasUsing = true; return; }
                    updates.push_back({*resolved, after[index]});
                }
                if(!ApplyObjectTransformEdits(demoScene, updates))
                    ReportSceneHistoryFailure("Invalid group transform; no objects were changed.");
                else
                {
                    for(size_t index = 0; index < after.size(); ++index)
                        m_gizmoGroupEdit->edits[index].after = after[index];
                    m_sceneHistoryStatus.clear();
                }
            }
        }
        catch(const std::exception& exception) { ReportSceneHistoryFailure(exception.what()); }
    }
    if(!usingGizmo && m_gizmoWasUsing && m_gizmoGroupEdit)
    {
        const bool changed = std::any_of(m_gizmoGroupEdit->edits.begin(), m_gizmoGroupEdit->edits.end(),
            [](const TransformEdit& edit) { return !SameHistoryTransform(edit.before, edit.after); });
        if(changed && m_historySession && m_historySession->epoch == m_gizmoHistoryEpoch)
        {
            TruncateHistoryRedo();
            AppendSceneHistoryEntry({std::move(m_gizmoGroupEdit), m_gizmoHistoryEpoch, m_gizmoGroupPayloadBytes});
        }
        m_gizmoGroupEdit.reset();
    }
    m_gizmoWasUsing = usingGizmo;
    if(usingGizmo && !m_sceneHistoryStatus.empty()) ImGui::SetTooltip("%s", m_sceneHistoryStatus.c_str());
}

void Editor::DrawTransformGizmo(
    IDemoScene& demoScene,
    float viewportWidth,
    float viewportHeight)
{
    m_viewportGizmoInteractive = false;

    if (m_selectedObject == InvalidObjectSelection || viewportWidth <= 0.0f || viewportHeight <= 0.0f)
    {
        return;
    }

    Scene& scene = demoScene.GetScene();

    if (IsPhysicsEditingLocked() || scene.HasPhysicsPresentation())
    {
        // Block the whole selection, including mixed groups and existing drags.
        // Never decompose a simulated model into an authored history entry.
        ImGuizmo::Enable(false);
        m_gizmoGroupEdit.reset();
        m_gizmoWasUsing = false;
        m_gizmoUndoObject = InvalidObjectSelection;
        m_gizmoHistoryTarget.reset();
        m_gizmoRotationDrag.reset();
        return;
    }

    const Scene& constScene = scene;
    const auto& objects = constScene.GetRenderObjects();

    if (m_selectedObject >= objects.size())
    {
        return;
    }

    // Parented scenes load from schema 9, but a world-space gizmo edit of a child needs the
    // H.5 world-to-local conversion; writing a decomposed world matrix as its local TRS would
    // move it. Block the whole gesture as for physics locks; Inspector local edits remain.
    const bool selectionHasParent = constScene.HasHierarchyParent(m_selectedObject) ||
        std::any_of(m_selectedObjects.begin(), m_selectedObjects.end(), [&](size_t selected)
        {
            return constScene.HasHierarchyParent(selected);
        });

    if (selectionHasParent)
    {
        ImGuizmo::Enable(false);
        m_gizmoGroupEdit.reset();
        m_gizmoWasUsing = false;
        m_gizmoUndoObject = InvalidObjectSelection;
        m_gizmoHistoryTarget.reset();
        m_gizmoRotationDrag.reset();
        return;
    }

    Transform transform = objects[m_selectedObject].transform;

    const Camera& camera = scene.GetCamera();

    Matrix4 view = camera.GetViewMatrix().Transpose();

    Matrix4 projection = camera.GetProjectionMatrix().Transpose();
    // Placement reads presentation; manipulation/history below still edit authored Transform.
    Matrix4 model = objects[m_selectedObject].GetPresentationTransform().currentModel.Transpose();

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
    const bool allowGizmo = !m_marqueeGesture && !ImGui::GetIO().AppFocusLost && !m_sceneOperationPending && !HasPendingSceneHistoryOperation() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && !m_cameraLooking && !m_panLeftHeld &&
        !ImGui::GetIO().MouseDown[ImGuiMouseButton_Right] &&
        !ImGui::GetDragDropPayload();
    m_viewportGizmoInteractive = allowGizmo && !m_workspace.IsSplitterDragging() &&
        (m_viewportHovered || ImGuizmo::IsUsing());
    ImGuizmo::Enable(m_viewportGizmoInteractive);

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

    // ImGuizmo rotates incrementally from the matrix it is given. While a rotation drag is in
    // progress give it its own previous continuous output, never the snapped authored rotation,
    // so quantized results cannot feed back into the drag.
    if(m_gizmoRotationDrag && m_gizmoWasUsing && m_gizmoMode == GizmoMode::Rotate)
    {
        model = m_gizmoRotationDrag->continuousModel;
    }

    ImGuizmo::Manipulate(&view.m[0][0], &projection.m[0][0], operation, ImGuizmo::WORLD, &model.m[0][0]);
    ImGui::GetWindowDrawList()->PopClipRect();

    ImGui::End();

    const bool gizmoUsing = ImGuizmo::IsUsing();
    UpdateGizmoRotationDrag(gizmoUsing, model, transform.rotation);

    if(m_selectedObjects.size() > 1 || m_gizmoGroupEdit)
    {
        float position[3], rotation[3], scale[3];
        ImGuizmo::DecomposeMatrixToComponents(&model.m[0][0], position, rotation, scale);
        Transform anchorAfter{{position[0], position[1], position[2]},
            {ToRadians(rotation[0]), ToRadians(rotation[1]), ToRadians(rotation[2])},
            {(std::max)(0.01f, scale[0]), (std::max)(0.01f, scale[1]), (std::max)(0.01f, scale[2])}};

        if(gizmoUsing && m_gizmoMode == GizmoMode::Translate)
        {
            // Only the active (anchor) object lands on the grid; the group keeps its relative
            // offsets. Before the first group frame the anchor's authored value is the start.
            const Vector3 anchorDragStart = m_gizmoGroupEdit ?
                m_gizmoGroupEdit->edits.back().before.position :
                transform.position;
            anchorAfter.position = SnapGizmoTranslation(anchorDragStart, anchorAfter.position);
        }
        else if(gizmoUsing && m_gizmoMode == GizmoMode::Rotate)
        {
            // The anchor's twist snaps; GroupTransform then applies the same world rotation to
            // every member around the anchor, preserving the existing pivot and spacing.
            anchorAfter.rotation = ResolveGizmoRotation(anchorAfter.rotation);
        }

        UpdateGroupGizmo(demoScene, gizmoUsing, anchorAfter);
        return;
    }

    if (gizmoUsing && !m_gizmoWasUsing)
    {
        m_gizmoUndoObject = m_selectedObject;
        m_gizmoUndoStart = transform;
        m_gizmoHistoryTarget = CaptureHistoryTarget(demoScene, m_selectedObject);
        m_gizmoHistoryEpoch = m_historyEpoch;
    }

    if(gizmoUsing && m_gizmoHistoryTarget)
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
            // ImGuizmo reports a continuous drag position derived from the cursor, not from the
            // snapped value fed back each frame, so snapping from the drag start cannot drift.
            const Vector3 continuousPosition{translation[0], translation[1], translation[2]};
            const Vector3 newPosition = SnapGizmoTranslation(m_gizmoUndoStart.position, continuousPosition);

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

            transform.rotation = ResolveGizmoRotation(newRotation);
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
            ApplyObjectTransformEdit(demoScene, m_selectedObject, transform);
        }
    }

    if(gizmoUsing && !m_transformEditStatus.empty())
    {
        ImGui::SetTooltip("%s", m_transformEditStatus.c_str());
    }

    if(!gizmoUsing && m_gizmoWasUsing && m_gizmoHistoryTarget)
    {
        const auto index = ResolveHistoryTarget(demoScene, *m_gizmoHistoryTarget);

        if(index)
        {
            RecordTransformEdit(std::move(*m_gizmoHistoryTarget), m_gizmoHistoryEpoch, m_gizmoUndoStart,
                static_cast<const Scene&>(scene).GetRenderObjects()[*index].transform);
        }

        m_gizmoUndoObject = InvalidObjectSelection;
        m_gizmoHistoryTarget.reset();
    }

    m_gizmoWasUsing = gizmoUsing;
}

void Editor::HandleViewportSelection(
    IDemoScene& demoScene,
    float viewportWidth,
    float viewportHeight,
    bool viewportWasFocused)
{
    if(UpdateViewportMarquee(demoScene))
    {
        return;
    }
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();

    if(!m_viewportHovered || m_sceneOperationPending || m_cameraLooking || m_panLeftHeld ||
        ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) ||
        ImGuizmo::IsUsing())
    {
        return;
    }

    if (m_viewportGizmoInteractive && m_selectedObject != InvalidObjectSelection && ImGuizmo::IsOver())
    {
        return;
    }

    const ImVec2 mouse(io.MousePos.x - m_viewportImage.position.x, io.MousePos.y - m_viewportImage.position.y);

    if (mouse.x < 0.0f || mouse.y < 0.0f || mouse.x >= viewportWidth || mouse.y >= viewportHeight)
    {
        return;
    }

    // While driving, an unmodified click that acquires keyboard focus must not also
    // replace the controller selection. Once focused, picking and marquee work normally.
    if(m_characterDrive && m_physicsState == PhysicsState::Playing && !viewportWasFocused &&
        m_viewportFocused && !io.KeyShift && !io.KeyCtrl && !io.KeyAlt)
    {
        return;
    }

    const auto icon = PickViewportIcon(demoScene, io.MousePos);
    const auto picked = icon ? *icon : PickObject(demoScene, mouse.x, mouse.y, viewportWidth, viewportHeight);
    if(picked != InvalidObjectSelection)
    {
        const bool toggleSelectionMembership = io.KeyShift || io.KeyCtrl;
        SelectSceneObject(demoScene, picked, toggleSelectionMembership);
    }
    else if(m_viewportRendererLifetime != 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Middle))
    {
        StartViewportMarquee(demoScene, mouse);
    }
}

void Editor::ResetTransformGestures()
{
    CancelViewportMarquee();
    m_gizmoGroupEdit.reset();
    m_gizmoWasUsing = false;
    m_gizmoUndoObject = InvalidObjectSelection;
    m_gizmoHistoryTarget.reset();
    m_gizmoRotationDrag.reset();

    m_transformFieldEditing = false;
    m_transformFieldUndoObject = InvalidObjectSelection;
    m_transformFieldScene = nullptr;
    m_transformFieldRevision = 0;

    m_fieldHistoryTarget.reset();
}

void Editor::HandleTransformUndo(IDemoScene& demoScene)
{
    const auto& io = ImGui::GetIO();

    if(io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false) && !HistoryReplayBlocked())
    {
        ReplaySceneHistory(demoScene, false);
    }
}

void Editor::HandleTransformRedo(IDemoScene& demoScene)
{
    const auto& io = ImGui::GetIO();
    const bool pressed = io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Y, false) ||
        (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)));

    if(pressed && !HistoryReplayBlocked())
    {
        ReplaySceneHistory(demoScene, true);
    }
}

void Editor::UpdateCameraLook(
    IDemoScene& demoScene,
    float viewportWidth,
    float viewportHeight)
{
    const ImGuiIO& io = ImGui::GetIO();

    (void)viewportWidth;
    (void)viewportHeight;

    // Use the same ImGui button state even when LMB is reserved from other widgets.
    const bool leftDown = io.MouseDown[ImGuiMouseButton_Left];
    const bool rightDown = io.MouseDown[ImGuiMouseButton_Right];
    m_panLeftHeld = m_panLeftHeld && leftDown;

    const bool interrupted = io.AppFocusLost || (rightDown && !ViewportCameraInput::HasCapture()) ||
       m_sceneOperationPending || m_workspace.IsSplitterDragging() ||
       m_viewportImage.size.x <= 0.0f || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    if(!rightDown || interrupted || (!m_cameraLooking && !m_viewportFocused))
    {
        if (m_cameraLooking)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        }

        m_cameraLooking = false;
        m_cameraPanning = false;
        if(interrupted)
        {
            m_viewportFocused = false;
            m_viewportRightGesture = false;
            m_viewportRightObject.reset();
        }
        return;
    }

    if (!m_cameraLooking)
    {
        const bool panChord = leftDown && rightDown;
        if(!m_viewportHovered || (!io.MouseClicked[ImGuiMouseButton_Right] && !panChord) || m_sceneOperationPending)
        {
            return;
        }

        if ((!panChord && (ImGuizmo::IsUsing() || m_gizmoWasUsing)) || ImGui::IsAnyItemActive() ||
            ImGui::GetDragDropPayload() || io.WantTextInput)
        {
            return;
        }

        if(panChord && (ImGuizmo::IsUsing() || m_gizmoWasUsing))
        {
            // A chord cancels the in-progress authored gesture, rather than
            // publishing its last mouse delta or recording a partial undo edit.
            if(m_gizmoHistoryEpoch == m_historyEpoch)
            {
                if(m_gizmoGroupEdit)
                {
                    std::vector<RenderObjectTransformUpdate> updates;
                    for(const auto& edit : m_gizmoGroupEdit->edits)
                        if(const auto index = ResolveHistoryTarget(demoScene, edit.target))
                            updates.push_back({*index, edit.before});
                    ApplyObjectTransformEdits(demoScene, updates);
                }
                else if(m_gizmoHistoryTarget)
                {
                    if(const auto index = ResolveHistoryTarget(demoScene, *m_gizmoHistoryTarget))
                        ApplyObjectTransformEdit(demoScene, *index, m_gizmoUndoStart);
                }
            }
            ImGuizmo::Enable(false);
            m_gizmoGroupEdit.reset();
            m_gizmoHistoryTarget.reset();
            m_gizmoUndoObject = InvalidObjectSelection;
            m_gizmoRotationDrag.reset();
            m_gizmoWasUsing = false;
        }

        Camera& camera = demoScene.GetScene().GetCamera();

        const Vector3 direction = camera.GetTarget() - camera.GetPosition();
        const float horizontalLength = std::sqrt(direction.x * direction.x + direction.z * direction.z);

        m_cameraYaw = std::atan2(direction.x, direction.z);

        m_cameraPitch = std::atan2(direction.y, horizontalLength);

        CaptureViewportRightClick(demoScene);
        m_cameraLooking = true;

        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }

    ImGui::SetMouseCursor(ImGuiMouseCursor_None);

    if(leftDown != m_cameraPanning)
    {
        m_cameraPanning = leftDown;
        m_panLeftHeld = leftDown;
        // A two-button chord is navigation even without motion: suppress RMB menus.
        // Discard this frame's delta, which may precede the second press/release.
        m_viewportRightDragged = true;
        m_viewportRightObject.reset();
        return;
    }

    if(m_cameraPanning)
    {
        Camera& camera = demoScene.GetScene().GetCamera();
        const Vector3 forward = (camera.GetTarget() - camera.GetPosition()).Normalized();
        // Match LookAt's basis: world up establishes right, then derive local up.
        const Vector3 right = Vector3::Cross(Vector3{0.0f, 1.0f, 0.0f}, forward).Normalized();
        const Vector3 up = Vector3::Cross(forward, right);
        // Mouse deltas are displacement, so do not multiply by frame time.
        constexpr float panSensitivity = 0.002f;
        const Vector3 offset = (right * io.MouseDelta.x - up * io.MouseDelta.y) *
            (ViewportCameraSpeed() * panSensitivity);
        const Vector3 position = camera.GetPosition();
        const Vector3 target = camera.GetTarget();
        camera.SetPosition(position + offset);
        camera.SetTarget(target + offset);
        return;
    }

    ImVec2 delta = io.MouseDelta;

    if(!m_viewportRightDragged)
    {
        if(!ImGui::IsMouseDragging(ImGuiMouseButton_Right))
        {
            return;
        }

        // Apply the accumulated motion once the gesture becomes camera navigation.
        // Motion on the initial press can precede the click and must not rotate the camera.
        delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right, 0.0f);
        m_viewportRightDragged = true;
    }

    constexpr float sensitivity = 0.003f;
    m_cameraYaw += delta.x * sensitivity;
    m_cameraPitch -= delta.y * sensitivity;

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
    if (!m_cameraLooking || m_cameraPanning || deltaTime <= 0.0f)
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

    m_viewportRightDragged = true;
    movement = movement * (1.0f / length);

    const float speed = ViewportCameraSpeed();

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

void Editor::DrawMaterialLeaveConfirmation(IDemoScene& demoScene, const ProjectPaths& projectPaths,
    const ProjectAssetRegistry& assetRegistry)
{
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
                m_inspectorTarget = m_inspectedAssetSelection.empty() ? InspectorTarget::None : InspectorTarget::Asset;
                m_objectInspectionIdentity.reset();
                break;
            case MaterialLeaveAction::ObjectSelection:
                ActivateObjectInspection(demoScene, projectPaths, assetRegistry);
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

        if(ImGui::Button("Don't Save"))
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
    CancelViewportMarquee();
    m_viewportRightGesture = false;
    m_viewportRightObject.reset();
    m_sceneContextObject.reset();
    m_openSceneContext = false;

    m_cameraLooking = false;
    m_cameraPanning = false;
    m_panLeftHeld = false;
    m_viewportFocused = false;
    m_viewportHovered = false;
    m_viewportImage = {};
    ImGuizmo::Enable(false);
}

void Editor::OnSceneActivated()
{
    InvalidateSceneFiles();
    ClearSceneHistory();
    ResetSceneInteractions();
}

void Editor::ResetSceneInteractions()
{
    DismissAddComponentPopup();
    DismissTriggerManagementPopup();
    DismissControllerManagementPopup();
    DismissCameraManagementPopup();
    DismissLightManagementPopup();
    m_triggerInspection.reset();
    m_controllerInspection.reset();
    m_cameraInspection.reset();
    m_lightInspection.reset();
    m_lightColorGestureActive = false;
    m_triggerEditStatus.clear();
    m_controllerEditStatus.clear();
    m_cameraEditStatus.clear();
    m_lightEditStatus.clear();
    m_transformEditStatus.clear();
    m_pendingMaterialBinding.reset();
    m_viewportMaterialRequest.reset();
    m_materialDropHover.reset();
    m_materialDropStatus.clear();
    m_orderedSelection.clear();
    m_selectedObjects.clear();
    CancelObjectRename();
    m_viewportRightGesture = false;
    m_viewportRightObject.reset();
    m_sceneContextObject.reset();
    m_openSceneContext = false;

    m_sceneObjectDeletionRequest.reset();
    m_sceneObjectDeletionPending.reset();
    m_sceneObjectDeletionStatusObject.reset();
    m_sceneObjectDeletionStatus.clear();

    m_assetBrowser.InvalidateMeshDragIdentity();
    m_meshCreationRequest.reset();
    m_workingMaterialRevisions.clear();
    m_inspectorPanel.ResetWorkingMaterialNotifications();
    m_savedMaterialRevisions.clear();
    m_materialAssignmentRequest.reset();
    m_materialAssignmentSelection.reset();
    m_materialAssignmentSlotID.reset();
    m_inspectorPanel.DismissSceneMaterialPicker();
    m_materialAssignmentPending = false;
    m_materialAssignmentStatus.clear();
    m_objectInspectionRequested = false;
    m_objectInspectionIdentity.reset();

    if(m_inspectorTarget == InspectorTarget::SceneObject)
    {
        m_inspectorTarget = InspectorTarget::None;
    }

    if(m_materialLeaveAction == MaterialLeaveAction::ObjectSelection)
    {
        m_materialLeaveAction = MaterialLeaveAction::None;
    }

    m_cameraLooking = false;
    m_cameraPanning = false;
    m_panLeftHeld = false;
    m_viewportFocused = false;

    // Different files may share a display name and structure revision.
    ResetTransformGestures();
    m_selectedObject = InvalidObjectSelection;
    m_selectionSceneName.clear();

    m_selectionScene = nullptr;
    m_selectionRuntimeIdentity = 0;
    m_selectionStructureRevision = 0;
}

void Editor::ImportFileDrop(
    const std::vector<std::filesystem::path>& files,
    float screenX,
    float screenY,
    const ProjectPaths& paths,
    ProjectAssetRegistry& registry)
{
    if (IsPhysicsEditingLocked())
    {
        return;
    }

    if(m_sceneOperationPending || m_materialLeaveAction != MaterialLeaveAction::None)
    {
        Log::Info("Texture drop rejected while an editor operation is pending.");
        return;
    }

    m_assetBrowser.ImportFileDrop(files, screenX, screenY, paths, registry);
}

void Editor::ProcessAssetDeletion(const ProjectPaths& paths, ProjectAssetRegistry& registry)
{
    if (IsPhysicsEditingLocked())
    {
        return;
    }

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
    else if(ProjectAssetRegistry::PathsEqual(request->extension(), ".scene"))
    {
        error = "Scene deletion requires a coordinated lifecycle workflow and is not available in the Asset Browser.";
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

        if(m_inspectorTarget == InspectorTarget::Asset)
        {
            m_inspectorPanel.LoadSelection({}, paths, registry);
            m_inspectorTarget = InspectorTarget::None;
        }
    }
}

void Editor::ProcessAssetRename(
    const ProjectPaths& paths,
    ProjectAssetRegistry& registry,
    const std::filesystem::path& activeScenePath)
{
    if (IsPhysicsEditingLocked())
    {
        return;
    }

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
    else if(ProjectAssetRegistry::PathsEqual(request->source.extension(), ".scene"))
    {
        error = "Scene renaming requires a coordinated lifecycle workflow and is not available in the Asset Browser.";
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

            if(m_inspectorTarget == InspectorTarget::Asset)
            {
                m_inspectorPanel.LoadSelection(destination, paths, registry);
            }
        }
    }
}

void Editor::ProcessAssetMove(const ProjectPaths& paths, ProjectAssetRegistry& registry)
{
    if (IsPhysicsEditingLocked())
    {
        return;
    }

    const auto request = m_assetBrowser.TakeMoveRequest();
    if(!request)
    {
        return;
    }
    std::string error;
    std::filesystem::path destination;
    if(m_inspectorPanel.HasUnsavedChanges() || m_materialLeaveAction != MaterialLeaveAction::None || m_sceneOperationPending)
    {
        error = "Move rejected while editor changes or a scene operation are pending.";
    }
    else if(ProjectAssetRegistry::PathsEqual(request->source.extension(), ".scene"))
    {
        error = "Scene moving requires a coordinated lifecycle workflow and is not available in the Asset Browser.";
    }
    else
    {
        ProjectAssetRename::MoveAssetFile(paths, registry, request->asset.id, request->source,
            request->directory, destination, error);
    }
    m_assetBrowser.OnMoveCompleted(*request, destination, error);
    if(!error.empty())
    {
        Log::Error("Project asset move failed: " + error);
        return;
    }
    if(m_inspectedAssetSelection == request->source)
    {
        m_inspectedAssetSelection = destination;
        m_pendingAssetSelection.clear();
        if(m_inspectorTarget == InspectorTarget::Asset)
        {
            m_inspectorPanel.LoadSelection(destination, paths, registry);
        }
    }
}

std::optional<Editor::MaterialAssignmentRequest> Editor::TakeMaterialAssignmentRequest()
{
    auto request = std::move(m_materialAssignmentRequest);
    m_materialAssignmentRequest.reset();
    return request;
}

void Editor::OnMaterialAssignmentCompleted(const SceneObjectMetadata& selection, bool succeeded, std::string message)
{
    if(m_materialAssignmentSelection && m_materialAssignmentSelection->runtimeIdentity == selection.runtimeIdentity &&
       m_materialAssignmentSelection->objectID == selection.objectID &&
       m_materialAssignmentSelection->structureRevision == selection.structureRevision)
    {
        m_materialAssignmentPending = false;
        m_materialAssignmentStatus = (succeeded ? "Assigned: " : "Not assigned: ") + std::move(message);

        if(m_materialAssignmentSlotID)
        {
            m_inspectorPanel.OnSlotAssignmentCompleted(selection, *m_materialAssignmentSlotID,
                succeeded, m_materialAssignmentStatus);
        }
    }
}

std::vector<Editor::SceneMaterialSavedRevision> Editor::TakeSavedMaterialRevisions()
{
    return std::exchange(m_savedMaterialRevisions, {});
}

std::vector<Editor::SceneMaterialWorkingRevision> Editor::TakeWorkingMaterialRevisions()
{
    return std::exchange(m_workingMaterialRevisions, {});
}

void Editor::PublishCreatedObjectSelection(
    const Scene& scene, SceneObjectMetadata metadata, std::string activeSceneName) noexcept
{
    // Metadata was resolved through the candidate's persistent-ID mapping before
    // renderer initialization; no allocation or fallible lookup follows publication.
    OnSceneActivated();
    m_selectedObject = metadata.runtimeIndex;
    m_selectionScene = &scene;
    m_selectionRuntimeIdentity = metadata.runtimeIdentity;

    m_selectionSceneName = std::move(activeSceneName);
    m_selectionStructureRevision = scene.GetStructureRevision();

    ObjectInspectionIdentity identity{&scene, scene.GetStructureRevision()};
    identity.runtimeIdentity = metadata.runtimeIdentity;
    identity.objectID = std::move(metadata.objectID);
    identity.sceneID = std::move(metadata.sceneID);

    m_objectInspectionIdentity = std::move(identity);
    m_inspectorTarget = InspectorTarget::SceneObject;
    m_objectInspectionRequested = true;
}

void Editor::DrawMaterialDropTarget(IDemoScene& scene, const ProjectPaths& paths,
    const ProjectAssetRegistry& registry)
{
    if (IsPhysicsEditingLocked())
    {
        return;
    }

    m_materialDropHover.reset();
    const auto* dragging = ImGui::GetDragDropPayload();
    if(!dragging || !dragging->IsDataType(MaterialDragPayloadType) ||
        m_viewportImage.size.x <= 0.0f || m_viewportImage.size.y <= 0.0f)
        return;
    if(ImGui::GetIO().AppFocusLost || m_workspace.IsSplitterDragging() || ImGuizmo::IsUsing() ||
        m_gizmoWasUsing || m_cameraLooking || m_sceneOperationPending || m_fileRequest ||
        HasBlockingObjectCreation() || (HasPendingSceneHistoryOperation() && !m_pendingMaterialBinding) || m_meshCreationRequest ||
        m_materialAssignmentRequest ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        return;
    const ImRect image(m_viewportImage.position, ImVec2(m_viewportImage.position.x + m_viewportImage.size.x,
        m_viewportImage.position.y + m_viewportImage.size.y));
    const auto mouse = ImGui::GetIO().MousePos;
    if(!image.Contains(mouse)) return;

    ImGui::Begin("Scene Viewport");
    if(ImGui::BeginDragDropTargetViewport(ImGui::GetMainViewport(), &image))
    {
        const auto* payload = ImGui::AcceptDragDropPayload(MaterialDragPayloadType,
            ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
        if(payload)
        {
            std::string error;
            auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
            std::optional<AssetRecord> material;
            if(payload->Data && payload->DataSize == sizeof(MaterialDragPayload))
            {
                MaterialDragPayload value;
                std::memcpy(&value, payload->Data, sizeof(value));
                material = m_assetBrowser.ValidateMaterialDrag(value, paths, registry, error);
            }
            else error = "Invalid material drag payload.";
            std::optional<SceneObjectMetadata> target;
            if(material && owner)
            {
                const auto index = PickObject(scene, mouse.x - image.Min.x, mouse.y - image.Min.y,
                    image.GetWidth(), image.GetHeight());
                if(index != InvalidObjectSelection) target = owner->GetObjectMetadata(index);
                if(!target) error = "No resident, triangle-pickable object under the cursor.";
                else if(const auto* mesh = scene.GetScene().GetRenderObjects()[target->runtimeIndex].mesh;
                    !mesh || !ValidateMeshMetadata(mesh->GetMetadata(), mesh->IsIndexed() ? mesh->GetIndexCount() : mesh->GetVertexCount(), error))
                {
                    target.reset();
                    error = "Target has no compatible resident material-slot layout.";
                }
            }
            else if(material) error = "Material drops require a file-backed scene.";
            if(target) m_materialDropHover = target;
            if(!error.empty()) ImGui::SetTooltip("%s", error.c_str());
            if(payload->IsDelivery() && m_materialDropDeliveryFrame != ImGui::GetFrameCount())
            {
                m_materialDropDeliveryFrame = ImGui::GetFrameCount();
                if(target && material) QueueViewportMaterialDrop(scene, *target, material->id);
                else m_materialDropStatus = "Material drop rejected: " + error;
                m_materialDropHover.reset();
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::End();
}

void Editor::DrawMeshDropTarget(const IDemoScene& scene, const ProjectPaths& paths,
    const ProjectAssetRegistry& registry)
{
    if (IsPhysicsEditingLocked())
    {
        return;
    }

    const ImGuiPayload* dragging = ImGui::GetDragDropPayload();

    if(!dragging || !dragging->IsDataType(MeshDragPayloadType) ||
        m_viewportImage.size.x <= 0.0f || m_viewportImage.size.y <= 0.0f)
    {
        return;
    }

    if(ImGui::GetIO().AppFocusLost || m_workspace.IsSplitterDragging() || ImGuizmo::IsUsing() ||
        m_gizmoWasUsing || m_cameraLooking || m_sceneOperationPending || m_fileRequest ||
        HasBlockingObjectCreation() || HasPendingSceneHistoryOperation() || m_meshCreationRequest ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
    {
        return;
    }

    const ImRect image(m_viewportImage.position, ImVec2(
        m_viewportImage.position.x + m_viewportImage.size.x,
        m_viewportImage.position.y + m_viewportImage.size.y));
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    if(!image.Contains(mouse))
    {
        return;
    }

    // The viewport intentionally has NoInputs. This ImGui target accepts a
    // rectangle without requiring a hovered input window, preserving picking.
    ImGui::Begin("Scene Viewport");

    if(ImGui::BeginDragDropTargetViewport(ImGui::GetMainViewport(), &image))
    {
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(MeshDragPayloadType);

        if(payload && payload->IsDelivery() && m_meshDropDeliveryFrame != ImGui::GetFrameCount())
        {
            m_meshDropDeliveryFrame = ImGui::GetFrameCount();
            const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
            MeshDragPayload meshPayload;
            std::string error;
            std::optional<AssetRecord> record;

            if(!owner)
            {
                error = "Mesh drops require an active file-backed scene.";
            }
            else if(payload->DataSize != sizeof(MeshDragPayload) || !payload->Data)
            {
                error = "Mesh drop has an invalid payload size.";
            }
            else
            {
                std::memcpy(&meshPayload, payload->Data, sizeof(meshPayload));
                record = m_assetBrowser.ValidateMeshDrag(meshPayload, paths, registry, error);
            }

            Transform placement;

            if(record && !FindMeshDropPosition(*owner, mouse.x - image.Min.x, mouse.y - image.Min.y,
                image.GetWidth(), image.GetHeight(), placement.position))
            {
                record.reset();
                error = "No finite forward placement is available at this cursor position.";
            }

            if(record)
            {
                m_meshCreationRequest = MeshCreationRequest{record->id.ToReference(),
                    record->relativePath.stem().string(), placement, owner->GetRuntimeIdentity()};
                m_meshCreationStatus = "Creating mesh at cursor...";
            }
            else
            {
                OnMeshCreationCompleted(false, error);
            }
        }

        ImGui::EndDragDropTarget();
    }

    ImGui::End();
}

std::optional<Editor::MeshCreationRequest> Editor::TakeMeshCreationRequest()
{
    return std::exchange(m_meshCreationRequest, std::nullopt);
}

void Editor::OnMeshCreationCompleted(bool succeeded, const std::string& error)
{
    m_meshCreationStatus = succeeded ? "Mesh created at cursor. Save the scene to persist it." :
        "Mesh drop rejected: " + error;

    if(!succeeded)
    {
        Log::Warning(m_meshCreationStatus);
    }
}


bool Editor::SceneObjectDeletionBlocked() const
{
    if (IsPhysicsEditingLocked())
    {
        return true;
    }

    return HasPendingSceneHistoryOperation() || m_sceneOperationPending || m_sceneObjectDeletionPending || m_sceneObjectDeletionRequest ||
        m_fileRequest || m_meshCreationRequest || m_materialAssignmentRequest ||
        m_materialLeaveAction != MaterialLeaveAction::None || HasBlockingObjectCreation() ||
        m_workspace.IsSplitterDragging() || ImGui::GetDragDropPayload() || ImGui::GetIO().AppFocusLost ||
        m_viewportImage.size.x <= 0.0f || m_viewportImage.size.y <= 0.0f;
}

bool Editor::SceneObjectDeletionMatches(const IDemoScene& scene, const SceneObjectMetadata& selection) const
{
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
    const auto current = owner ? owner->GetObjectMetadata(selection.objectID) : std::nullopt;

    return current &&
        current->sceneID == selection.sceneID && current->runtimeIdentity == selection.runtimeIdentity &&
        current->structureRevision == selection.structureRevision && current->runtimeIndex == selection.runtimeIndex;
}

void Editor::CaptureViewportRightClick(const IDemoScene& scene)
{
    m_viewportRightObject.reset();
    m_viewportRightGesture = true;
    m_viewportRightDragged = false;

    if(SceneObjectDeletionBlocked() || ImGuizmo::IsOver() || ImGui::GetIO().WantTextInput)
    {
        return;
    }

    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);

    if(!owner)
    {
        return;
    }

    const auto mouse = ImGui::GetIO().MousePos;
    const auto index = PickObject(scene, mouse.x - m_viewportImage.position.x,
        mouse.y - m_viewportImage.position.y, m_viewportImage.size.x, m_viewportImage.size.y);

    if(index != InvalidObjectSelection)
    {
        m_viewportRightObject = owner->GetObjectMetadata(index);
    }
}

void Editor::FinishViewportRightClick(const IDemoScene& scene)
{
    if(!m_viewportRightGesture || !ImGui::IsMouseReleased(ImGuiMouseButton_Right))
    {
        return;
    }

    m_viewportRightGesture = false;
    auto selected = std::exchange(m_viewportRightObject, std::nullopt);
    const auto& io = ImGui::GetIO();
    const bool dragged = m_viewportRightDragged ||
        io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] >= io.MouseDragThreshold * io.MouseDragThreshold;

    if(!selected || dragged || !m_viewportHovered || SceneObjectDeletionBlocked() ||
        ImGuizmo::IsUsing() || ImGuizmo::IsOver() || ImGui::IsAnyItemActive() || io.WantTextInput ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        return;
    }

    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
    const auto current = owner ? owner->GetObjectMetadata(selected->objectID) : std::nullopt;

    if(!current || current->runtimeIdentity != selected->runtimeIdentity ||
        current->sceneID != selected->sceneID || current->structureRevision != selected->structureRevision)
    {
        return;
    }

    const auto index = PickObject(scene, io.MousePos.x - m_viewportImage.position.x,
        io.MousePos.y - m_viewportImage.position.y, m_viewportImage.size.x, m_viewportImage.size.y);

    if(index != current->runtimeIndex)
    {
        return;
    }

    OpenSceneObjectContext(scene, *current);
}

void Editor::OpenSceneObjectContext(const IDemoScene& scene, const SceneObjectMetadata& target)
{
    if(!m_historySession || !SceneObjectDeletionMatches(scene, target) ||
        !RenameSessionMatches(scene, *m_historySession))
    {
        return;
    }

    m_sceneContextObject = target;
    m_sceneContextEpoch = m_historySession->epoch;
    m_openSceneContext = true;

    SelectSceneObject(scene, target.runtimeIndex);
    m_objectInspectionRequested = true;
}

void Editor::QueueSceneObjectDeletion(const IDemoScene& scene, const SceneObjectMetadata& selection)
{
    if(SceneObjectDeletionBlocked() || !SceneObjectDeletionMatches(scene, selection))
    {
        return;
    }

    m_sceneObjectDeletionRequest = selection;
    m_sceneObjectDeletionPending = selection;
    m_sceneObjectDeletionStatusObject = selection;
    m_sceneObjectDeletionStatus = "Deleting scene object...";
}

void Editor::DrawSceneObjectDeletionControls(IDemoScene& scene)
{
    if(m_sceneObjectDeletionStatusObject && !SceneObjectDeletionMatches(scene, *m_sceneObjectDeletionStatusObject))
    {
        m_sceneObjectDeletionStatusObject.reset();
        m_sceneObjectDeletionStatus.clear();
    }

    const auto& io = ImGui::GetIO();

    if(io.AppFocusLost || !m_viewportFocused || SceneObjectDeletionBlocked())
    {
        m_viewportRightGesture = false;
        m_viewportRightObject.reset();
    }

    FinishViewportRightClick(scene);

    // Hierarchy keyboard navigation normally sets WantCaptureKeyboard itself.
    // Its focused window may own Delete, but active edits, gestures and popups may not.
    const ImGuiWindow* keyboardWindow = ImGui::GetCurrentContext()->NavWindow;
    const bool hierarchyHasKeyboardFocus = keyboardWindow &&
        keyboardWindow->RootWindow == ImGui::FindWindowByName("Hierarchy");
    const bool hierarchyKeyboardAllowed = m_workspace.HierarchyVisible() && m_hierarchyPanel.IsFocused() &&
        hierarchyHasKeyboardFocus && ImGui::TestKeyOwner(ImGuiKey_Delete, ImGuiKeyOwner_NoOwner) &&
        !m_hierarchyPanel.IsRenaming() && !m_openSceneContext && !HistoryReplayBlocked();
    const bool keyboardAllowed = (ViewportOwnsKeyboard() || hierarchyKeyboardAllowed) &&
        !SceneObjectDeletionBlocked();

    if(keyboardAllowed && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
        const auto selected = owner ? owner->GetObjectMetadata(m_selectedObject) : std::nullopt;

        if(selected && m_selectionStructureRevision == selected->structureRevision)
        {
            QueueSceneObjectDeletion(scene, *selected);
        }
    }

    DrawSceneObjectContextMenu(scene);
}

void Editor::DrawSceneObjectContextMenu(IDemoScene& scene)
{
    // Both entry points use this stable popup host and the same captured target.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::Begin("Scene Viewport", nullptr, flags);

    if(m_openSceneContext && m_materialLeaveAction == MaterialLeaveAction::None)
    {
        ImGui::OpenPopup("SceneObjectContext");
        m_openSceneContext = false;
    }

    if(ImGui::BeginPopup("SceneObjectContext"))
    {
        if(!m_sceneContextObject || !m_historySession || m_sceneContextEpoch != m_historySession->epoch ||
            !SceneObjectDeletionMatches(scene, *m_sceneContextObject))
        {
            ImGui::CloseCurrentPopup();
        }
        else
        {
            const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
            const auto current = owner->GetObjectMetadata(m_sceneContextObject->objectID);
            ImGui::TextUnformatted(current->name.c_str());
            ImGui::Separator();

            ImGui::BeginDisabled(SceneObjectDeletionBlocked());

            if(ImGui::MenuItem("Rename"))
            {
                QueueObjectRename(scene, *current);
            }

            if(ImGui::MenuItem("Create Empty Object"))
            {
                QueueHierarchyEmpty(scene, {}, m_reorderStatus);
            }

            if(ImGui::MenuItem("Create Child Empty Object"))
            {
                QueueHierarchyEmpty(scene, current->objectID, m_reorderStatus);
            }

            if(ImGui::MenuItem("Duplicate Object / Subtree"))
            {
                QueueSceneObjectDuplication(scene, *current, m_reorderStatus);
            }

            const auto& document = owner->m_runtime->GetFileScene().GetDocument();
            const auto authored = std::find_if(document.objects.begin(), document.objects.end(),
                [&](const auto& object) { return object.id == current->objectID; });

            if(authored != document.objects.end() && !authored->parentID.empty() && ImGui::BeginMenu("Move to Root"))
            {
                if(ImGui::MenuItem("Keep World"))
                {
                    QueueHierarchyReparent(scene, current->objectID, {}, true, {}, m_reorderStatus);
                }

                if(ImGui::MenuItem("Keep Local"))
                {
                    QueueHierarchyReparent(scene, current->objectID, {}, false, {}, m_reorderStatus);
                }

                ImGui::EndMenu();
            }

            if(ImGui::MenuItem("Delete Object / Subtree"))
            {
                QueueSceneObjectDeletion(scene, *current);
            }

            ImGui::EndDisabled();
        }

        ImGui::EndPopup();
    }

    if(!m_openSceneContext && !ImGui::IsPopupOpen("SceneObjectContext"))
    {
        m_sceneContextObject.reset();
    }

    ImGui::End();
}

std::optional<SceneObjectMetadata> Editor::TakeSceneObjectDeletionRequest()
{
    return std::exchange(m_sceneObjectDeletionRequest, std::nullopt);
}

void Editor::OnSceneObjectDeletionCompleted(const IDemoScene& scene, const SceneObjectMetadata& selection,
    bool succeeded, const std::string& error)
{
    if(!m_sceneObjectDeletionPending ||
        m_sceneObjectDeletionPending->objectID != selection.objectID ||
        m_sceneObjectDeletionPending->runtimeIdentity != selection.runtimeIdentity ||
        m_sceneObjectDeletionPending->structureRevision != selection.structureRevision ||
        m_sceneObjectDeletionPending->sceneID != selection.sceneID)
    {
        return;
    }

    m_sceneObjectDeletionPending.reset();

    if(succeeded || !SceneObjectDeletionMatches(scene, selection))
    {
        m_sceneObjectDeletionStatusObject.reset();
        m_sceneObjectDeletionStatus.clear();
        return;
    }

    m_sceneObjectDeletionStatusObject = selection;
    m_sceneObjectDeletionStatus = "Object deletion failed: " + error;
    Log::Warning(m_sceneObjectDeletionStatus);
}

namespace
{
    bool SameHistoryTransform(const Transform& left, const Transform& right)
    {
        const auto same = [](const Vector3& a, const Vector3& b)
        {
            return a.x == b.x && a.y == b.y && a.z == b.z;
        };
        return same(left.position, right.position) && same(left.rotation, right.rotation) &&
            same(left.scale, right.scale);
    }
}

void Editor::ClearSceneHistory()
{
    m_pendingMaterialBinding.reset();
    m_viewportMaterialRequest.reset();
    m_reorderStatus.clear();
    CancelObjectRename();
    // Session invalidation explicitly cancels requests; late results carry the old epoch.
    m_pendingHistoryReplay.reset();
    m_historyReplayRequest.reset();
    m_duplicationRequest.reset();
    m_stagedDeletionHistory.reset();

    m_sceneHistory.clear();
    m_historyCursor = 0;
    m_historyPayloadBytes = 0;
    m_historySession.reset();
    m_historyCurrentScenePath.clear();

    ++m_historyEpoch;
    m_historyObservedScene = nullptr;
    m_historyRuntimeIdentity = 0;
    m_historyStructureRevision = 0;

    m_sceneHistoryStatus.clear();
    ResetTransformGestures();
}

void Editor::ReportSceneHistoryFailure(const std::string& error)
{
    m_sceneHistoryStatus = "Scene history: " + error;
    Log::Warning(m_sceneHistoryStatus);
}

bool Editor::UpdateSceneHistorySession(const IDemoScene& demoScene, const ProjectPaths& paths,
    const std::string& name)
{
    std::error_code error;
    const auto project = std::filesystem::canonical(paths.GetRoot(), error);
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&demoScene);

    if(error || (owner && (owner->GetPersistentSceneID().empty() ||
        std::filesystem::canonical(owner->GetProjectRoot(), error) != project || error)))
    {
        ClearSceneHistory();
        ReportSceneHistoryFailure("Cannot establish the current project/scene identity; replay is disabled.");
        return false;
    }

    const std::string identity = owner ? std::string(owner->GetPersistentSceneID()) : name;
    const auto path = owner ? owner->GetScenePath().lexically_normal() : std::filesystem::path{};
    const auto runtime = owner ? owner->GetRuntimeIdentity() : 0;
    const auto& scene = demoScene.GetScene();
    const auto revision = scene.GetStructureRevision();
    const bool changed = m_historySession && (m_historySession->projectIdentity != project ||
        m_historySession->sceneIdentity != identity || m_historyCurrentScenePath != path ||
        m_historyObservedScene != &scene || m_historyRuntimeIdentity != runtime ||
        m_historyStructureRevision != revision);

    if(changed)
    {
        ClearSceneHistory();
    }

    if(!m_historySession)
    {
        if(m_historyEpoch == 0)
        {
            ++m_historyEpoch;
        }

        m_historySession = SceneHistorySession{project, path, identity, m_historyEpoch};
        m_historyCurrentScenePath = path;
        m_historyObservedScene = &scene;
        m_historyRuntimeIdentity = runtime;
        m_historyStructureRevision = revision;
    }

    return true;
}

bool Editor::RebindSceneHistoryAfterSaveAs(const FileBackedDemoScene& scene, const std::filesystem::path& previousPath)
{
    if(!m_historySession)
    {
        return m_sceneHistory.empty();
    }

    std::error_code error;
    const auto project = std::filesystem::canonical(scene.GetProjectRoot(), error);

    if(error || project != m_historySession->projectIdentity ||
        previousPath.lexically_normal() != m_historyCurrentScenePath ||
        m_historySession->sceneIdentity != scene.GetPersistentSceneID() ||
        m_historyRuntimeIdentity != scene.GetRuntimeIdentity() || m_historyObservedScene != &scene.GetScene() ||
        m_historyStructureRevision != scene.GetScene().GetStructureRevision())
    {
        ReportSceneHistoryFailure("Save As did not match the current history session.");
        return false;
    }

    m_historyCurrentScenePath = scene.GetScenePath().lexically_normal();
    return true;
}

bool Editor::RebindSceneHistoryAfterStructureChange(const IDemoScene& demoScene)
{
    // Precondition: a future supported structural command has completed its checked
    // Renderer publication. Never use this to acknowledge arbitrary structural edits.
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&demoScene);
    std::error_code error;
    const auto project = owner ? std::filesystem::canonical(owner->GetProjectRoot(), error) :
        std::filesystem::path{};

    if(!owner || !m_historySession || error || project != m_historySession->projectIdentity ||
        owner->GetPersistentSceneID() != m_historySession->sceneIdentity ||
        owner->GetScenePath().lexically_normal() != m_historyCurrentScenePath)
    {
        ReportSceneHistoryFailure("Structural publication does not belong to the current editing session.");
        return false;
    }

    m_historyObservedScene = &demoScene.GetScene();
    m_historyRuntimeIdentity = owner->GetRuntimeIdentity();
    m_historyStructureRevision = demoScene.GetScene().GetStructureRevision();
    return true;
}

std::optional<Editor::HistoryTarget> Editor::CaptureHistoryTarget(const IDemoScene& demoScene, size_t index)
{
    try
    {
        const auto& scene = demoScene.GetScene();

        if(!m_historySession || m_historyObservedScene != &scene ||
            m_historyStructureRevision != scene.GetStructureRevision() || index >= scene.GetRenderObjects().size())
        {
            ReportSceneHistoryFailure("Cannot record a transform against stale scene mappings.");
            return std::nullopt;
        }

        // Reserve before the first live change; completion moves the captured target.
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&demoScene))
        {
            const auto metadata = owner->GetObjectMetadata(index);

            if(!metadata || metadata->runtimeIdentity != m_historyRuntimeIdentity ||
                metadata->sceneID != m_historySession->sceneIdentity ||
                metadata->objectID.capacity() + sizeof(SceneHistoryEntry) + 1 > kSceneHistoryPayloadBytes)
            {
                ReportSceneHistoryFailure("Cannot prepare the edited object's persistent history target.");
                return std::nullopt;
            }

            return PersistentHistoryTarget{metadata->objectID};
        }

        return ProceduralHistoryTarget{index};
    }
    catch(const std::bad_alloc&)
    {
        return std::nullopt;
    }
}

std::optional<size_t> Editor::ResolveHistoryTarget(const IDemoScene& demoScene, const HistoryTarget& target)
{
    const auto& scene = demoScene.GetScene();
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&demoScene);

    if(!m_historySession || m_historySession->epoch != m_historyEpoch || m_historyObservedScene != &scene ||
        m_historyStructureRevision != scene.GetStructureRevision() ||
        (owner && m_historyRuntimeIdentity != owner->GetRuntimeIdentity()))
    {
        ReportSceneHistoryFailure("Scene bindings changed; the history cursor was preserved.");
        return std::nullopt;
    }

    if(const auto* persistent = std::get_if<PersistentHistoryTarget>(&target))
    {
        const auto metadata = owner ? owner->GetObjectMetadata(persistent->objectID) : std::nullopt;

        if(!metadata || metadata->sceneID != m_historySession->sceneIdentity)
        {
            ReportSceneHistoryFailure("Target object '" + persistent->objectID + "' cannot be resolved; cursor preserved.");
            return std::nullopt;
        }

        return metadata->runtimeIndex;
    }

    const auto index = std::get<ProceduralHistoryTarget>(target).objectIndex;

    if(owner || index >= scene.GetRenderObjects().size())
    {
        ReportSceneHistoryFailure("Procedural target cannot be resolved; the history cursor was preserved.");
        return std::nullopt;
    }

    return index;
}

void Editor::TruncateHistoryRedo()
{
    for(size_t index = m_historyCursor; index < m_sceneHistory.size(); ++index)
    {
        m_historyPayloadBytes -= m_sceneHistory[index].payloadBytes;
    }

    m_sceneHistory.erase(m_sceneHistory.begin() + m_historyCursor, m_sceneHistory.end());
}

void Editor::RecordTransformEdit(HistoryTarget target, std::uint64_t epoch,
    const Transform& before, const Transform& after)
{
    if(SameHistoryTransform(before, after))
    {
        return;
    }

    if(HasPendingSceneHistoryOperation())
    {
        ReportSceneHistoryFailure("A deferred history operation is pending; transform recording was rejected.");
        return;
    }

    if(!m_historySession || epoch != m_historySession->epoch)
    {
        ReportSceneHistoryFailure("Completed transform belongs to a different editing session.");
        return;
    }

    SceneHistoryEntry entry{TransformEdit{std::move(target), before, after}, epoch, sizeof(SceneHistoryEntry)};
    const auto& edit = std::get<TransformEdit>(entry.command);

    if(const auto* persistent = std::get_if<PersistentHistoryTarget>(&edit.target))
    {
        // Capacity accounts conservatively for retained string storage, including SSO.
        entry.payloadBytes += persistent->objectID.capacity() + 1;
    }

    m_sceneHistory.reserve(kSceneHistoryCapacity);
    TruncateHistoryRedo();

    if(entry.payloadBytes > kSceneHistoryPayloadBytes)
    {
        ReportSceneHistoryFailure("Transform command exceeds the 4 MiB history payload limit.");
        return;
    }

    AppendSceneHistoryEntry(std::move(entry));
}

void Editor::AppendSceneHistoryEntry(SceneHistoryEntry entry) noexcept
{
    static_assert(std::is_nothrow_move_constructible_v<SceneHistoryEntry>);
    static_assert(std::is_nothrow_move_assignable_v<SceneHistoryEntry>);

    TruncateHistoryRedo();

    while(m_historyCursor > 0 && (m_sceneHistory.size() >= kSceneHistoryCapacity ||
        m_historyPayloadBytes + entry.payloadBytes > kSceneHistoryPayloadBytes))
    {
        m_historyPayloadBytes -= m_sceneHistory.front().payloadBytes;
        m_sceneHistory.erase(m_sceneHistory.begin());
        --m_historyCursor;
    }

    m_historyPayloadBytes += entry.payloadBytes;
    m_sceneHistory.push_back(std::move(entry));
    m_historyCursor = m_sceneHistory.size();
    m_sceneHistoryStatus.clear();
}

bool Editor::HistoryReplayBlocked() const
{
    if (IsPhysicsEditingLocked())
    {
        return true;
    }

    const auto& io = ImGui::GetIO();
    return m_marqueeGesture.has_value() || m_renameIntent.has_value() || HasPendingSceneHistoryOperation() || io.WantTextInput || io.AppFocusLost || ImGui::IsAnyItemActive() ||
        ImGuizmo::IsUsing() || m_gizmoWasUsing || m_transformFieldEditing || m_cameraLooking ||
        ImGui::IsMouseDown(ImGuiMouseButton_Right) || m_viewportRightGesture ||
        m_sceneOperationPending || m_sceneObjectDeletionPending || m_sceneObjectDeletionRequest ||
        m_materialAssignmentPending || m_materialAssignmentRequest || m_meshCreationRequest || m_fileRequest ||
        m_materialLeaveAction != MaterialLeaveAction::None || m_workspace.IsSplitterDragging() ||
        ImGui::GetDragDropPayload() ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

void Editor::ReplaySceneHistory(IDemoScene& demoScene, bool redo)
{
    if (IsPhysicsEditingLocked())
    {
        return;
    }

    if((redo && m_historyCursor == m_sceneHistory.size()) || (!redo && m_historyCursor == 0))
    {
        return;
    }

    const auto& entry = m_sceneHistory[redo ? m_historyCursor : m_historyCursor - 1];

    if(!m_historySession || entry.sessionEpoch != m_historySession->epoch)
    {
        ReportSceneHistoryFailure("Command belongs to another load session; cursor preserved.");
        return;
    }

    if(const auto* material = std::get_if<std::unique_ptr<MaterialBindingsEdit>>(&entry.command); material && *material)
    {
        try { QueueMaterialBindingReplay(demoScene, **material, redo); }
        catch(const std::exception& exception) { ReportSceneHistoryFailure(exception.what()); }
        return;
    }

    if(const auto* physics = std::get_if<std::unique_ptr<PhysicsComponentEdit>>(&entry.command); physics && *physics)
    {
        const auto& edit = **physics;
        auto* owner = dynamic_cast<FileBackedDemoScene*>(&demoScene);
        if(!owner || !RenameSessionMatches(demoScene, edit.session))
        {
            ReportSceneHistoryFailure("Physics replay belongs to a stale scene session.");
            return;
        }
        const auto selection = owner->GetObjectMetadata(edit.objectID);
        std::string error;
        if(!selection || !owner->SetObjectPhysics(*selection, redo ? edit.before : edit.after,
            redo ? edit.after : edit.before, &error))
        {
            ReportSceneHistoryFailure(error);
            return;
        }
        if(redo) ++m_historyCursor; else --m_historyCursor;
        m_sceneHistoryStatus.clear();
        return;
    }

    if(const auto* trigger = std::get_if<std::unique_ptr<TriggerColliderEdit>>(&entry.command); trigger && *trigger)
    {
        const auto& edit = **trigger;
        auto* owner = dynamic_cast<FileBackedDemoScene*>(&demoScene);

        if(!owner || !RenameSessionMatches(demoScene, edit.session))
        {
            ReportSceneHistoryFailure("Trigger Collider replay belongs to a stale scene session.");
            return;
        }

        const auto selection = owner->GetObjectMetadata(edit.objectID);
        std::string error;

        if(!selection || !owner->SetObjectTriggerCollider(*selection, redo ? edit.before : edit.after,
            redo ? edit.after : edit.before, &error))
        {
            ReportSceneHistoryFailure(error);
            return;
        }

        m_historyCursor = redo ? m_historyCursor + 1 : m_historyCursor - 1;
        m_triggerInspection.reset();
        ++m_triggerControlsRevision;
        m_sceneHistoryStatus.clear();
        return;
    }

    if(const auto* controller = std::get_if<std::unique_ptr<CharacterControllerEdit>>(&entry.command); controller && *controller)
    {
        const auto& edit = **controller;
        auto* owner = dynamic_cast<FileBackedDemoScene*>(&demoScene);

        if(!owner || !RenameSessionMatches(demoScene, edit.session))
        {
            ReportSceneHistoryFailure("Character Controller replay belongs to a stale scene session.");
            return;
        }

        const auto selection = owner->GetObjectMetadata(edit.objectID);
        std::string error;

        if(!selection || !owner->SetObjectCharacterController(*selection, redo ? edit.before : edit.after,
            redo ? edit.after : edit.before, &error))
        {
            ReportSceneHistoryFailure(error);
            return;
        }

        m_historyCursor = redo ? m_historyCursor + 1 : m_historyCursor - 1;
        m_controllerInspection.reset();
        ++m_controllerControlsRevision;
        m_sceneHistoryStatus.clear();
        return;
    }

    if(const auto* camera = std::get_if<std::unique_ptr<CameraEdit>>(&entry.command); camera && *camera)
    {
        const auto& edit = **camera;
        auto* owner = dynamic_cast<FileBackedDemoScene*>(&demoScene);

        if(!owner || !RenameSessionMatches(demoScene, edit.session))
        {
            ReportSceneHistoryFailure("Camera replay belongs to a stale scene session.");
            return;
        }

        std::string error;

        try
        {
            // Every affected Camera is restored by one expected-value-checked runtime edit.
            std::vector<SceneCameraChange> changes;
            changes.reserve(edit.changes.size());

            for(const auto& change : edit.changes)
            {
                const auto selection = owner->GetObjectMetadata(change.objectID, &error);

                if(!selection)
                {
                    ReportSceneHistoryFailure(error);
                    return;
                }

                changes.push_back({*selection, redo ? change.before : change.after, redo ? change.after : change.before});
            }

            if(!owner->SetObjectCameras(changes, &error))
            {
                ReportSceneHistoryFailure(error);
                return;
            }
        }
        catch(const std::exception& exception)
        {
            ReportSceneHistoryFailure(exception.what());
            return;
        }

        m_historyCursor = redo ? m_historyCursor + 1 : m_historyCursor - 1;
        m_cameraInspection.reset();
        ++m_cameraControlsRevision;
        m_sceneHistoryStatus.clear();
        return;
    }

    if(const auto* light = std::get_if<std::unique_ptr<LightEdit>>(&entry.command); light && *light)
    {
        const auto& edit = **light;
        auto* owner = dynamic_cast<FileBackedDemoScene*>(&demoScene);

        if(!owner || !RenameSessionMatches(demoScene, edit.session))
        {
            ReportSceneHistoryFailure("Light replay belongs to a stale scene session.");
            return;
        }

        std::string error;
        // Resolved by stable ID now; the runtime checks the expected value before restoring.
        const auto selection = owner->GetObjectMetadata(edit.objectID, &error);

        if(!selection || !owner->SetObjectLight(*selection, redo ? edit.before : edit.after,
            redo ? edit.after : edit.before, &error))
        {
            ReportSceneHistoryFailure(error);
            return;
        }

        m_historyCursor = redo ? m_historyCursor + 1 : m_historyCursor - 1;
        m_lightInspection.reset();
        ++m_lightControlsRevision;
        m_sceneHistoryStatus.clear();
        return;
    }

    const auto* ownedDeletion = std::get_if<std::unique_ptr<ObjectDeletion>>(&entry.command);

    if(ownedDeletion && *ownedDeletion)
    {
        const auto* deletion = ownedDeletion->get();
        const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&demoScene);

        if(!owner || !CanPreserveSceneHistory(*owner, deletion->session.projectIdentity, deletion->session.epoch))
        {
            ReportSceneHistoryFailure("Deletion replay does not belong to the current editing session.");
            return;
        }

        try
        {
            HistoryReplayIdentity identity{AllocateSceneHistoryOperationID(), m_historyEpoch, redo,
                redo ? deletion->selectionAfter : deletion->selectionBefore};
            // Copy the outbound payload before locking the cursor. No command addresses escape.
            HistoryReplayRequest request{identity, deletion->object, deletion->documentIndex};
            if(deletion->insertion)
            {
                request.bindingSourceID = deletion->selectionBefore;
            }
            request.membership = true;
            request.remove = deletion->insertion ? !redo : redo;

            if(deletion->hierarchy)
            {
                request.hierarchy = redo ? *deletion->hierarchy :
                    SceneHierarchyEdit{deletion->hierarchy->after, deletion->hierarchy->before};
                request.bindingSourceID.reset();
            }
            m_historyReplayRequest = std::move(request);
            m_pendingHistoryReplay = PendingHistoryReplay{std::move(identity), m_historyCursor};
        }
        catch(const std::exception& exception)
        {
            ReportSceneHistoryFailure(exception.what());
        }

        return;
    }

    const auto* ownedRename = std::get_if<std::unique_ptr<RenameObject>>(&entry.command);

    if(ownedRename && *ownedRename)
    {
        const auto& rename = **ownedRename;
        auto* owner = dynamic_cast<FileBackedDemoScene*>(&demoScene);

        if(!owner || !RenameSessionMatches(demoScene, rename.session))
        {
            ReportSceneHistoryFailure("Rename replay belongs to a stale scene session; cursor preserved.");
            return;
        }

        try
        {
            std::string error;
            const auto current = owner->GetObjectMetadata(rename.objectID, &error);
            const auto& expected = redo ? rename.before : rename.after;
            const auto& replacement = redo ? rename.after : rename.before;

            if(!current || !owner->RenameObject(*current, expected, replacement, &error,
                SceneObjectNamePolicy::ExactHistory))
            {
                ReportSceneHistoryFailure(error);
                return;
            }
        }
        catch(const std::exception& exception)
        {
            ReportSceneHistoryFailure(std::string("Rename replay failed: ") + exception.what());
            return;
        }

        m_historyCursor = redo ? m_historyCursor + 1 : m_historyCursor - 1;
        m_sceneHistoryStatus.clear();
        return;
    }

    const auto* ownedReorder = std::get_if<std::unique_ptr<ReorderObjects>>(&entry.command);

    if(ownedReorder && *ownedReorder)
    {
        const auto& reorder = **ownedReorder;
        auto* owner = dynamic_cast<FileBackedDemoScene*>(&demoScene);

        if(!owner || !RenameSessionMatches(demoScene, reorder.session))
        {
            ReportSceneHistoryFailure("Hierarchy reorder belongs to a stale scene session; cursor preserved.");
            return;
        }

        try
        {
            const SceneObjectOrderIdentity identity{std::string(owner->GetPersistentSceneID()),
                owner->GetRuntimeIdentity(), demoScene.GetScene().GetStructureRevision()};
            std::string error;

            if(!owner->ReorderObjects(identity, redo ? reorder.before : reorder.after,
                redo ? reorder.after : reorder.before, &error))
            {
                ReportSceneHistoryFailure(error);
                return;
            }
        }
        catch(const std::exception& exception)
        {
            ReportSceneHistoryFailure(std::string("Hierarchy reorder replay failed: ") + exception.what());
            return;
        }

        m_historyCursor = redo ? m_historyCursor + 1 : m_historyCursor - 1;
        m_sceneHistoryStatus.clear();
        return;
    }

    if(const auto* group = std::get_if<std::unique_ptr<GroupTransformEdit>>(&entry.command); group && *group)
    {
        try
        {
            std::vector<RenderObjectTransformUpdate> updates;
            updates.reserve((*group)->edits.size());
            for(const auto& edit : (*group)->edits)
            {
                const auto resolved = ResolveHistoryTarget(demoScene, edit.target);
                if(!resolved)
                {
                    return;
                }

                if(!SameHistoryTransform(demoScene.GetScene().GetRenderObjects()[*resolved].transform, redo ? edit.before : edit.after))
                {
                    ReportSceneHistoryFailure("Transform replay expected state changed; cursor preserved.");
                    return;
                }

                updates.push_back({*resolved, redo ? edit.after : edit.before});
            }
            if(!ApplyObjectTransformEdits(demoScene, updates))
            {
                ReportSceneHistoryFailure("Group replay validation failed; no objects changed and cursor preserved.");
                return;
            }
        }
        catch(const std::exception& exception) { ReportSceneHistoryFailure(exception.what()); return; }
        m_historyCursor = redo ? m_historyCursor + 1 : m_historyCursor - 1;
        m_sceneHistoryStatus.clear();
        return;
    }

    const auto* transformEdit = std::get_if<TransformEdit>(&entry.command);

    if(!transformEdit)
    {
        ReportSceneHistoryFailure("Unsupported scene history command; cursor preserved.");
        return;
    }

    const auto& edit = *transformEdit;
    const auto index = ResolveHistoryTarget(demoScene, edit.target);

    if(!index)
    {
        return;
    }

    if(!SameHistoryTransform(demoScene.GetScene().GetRenderObjects()[*index].transform, redo ? edit.before : edit.after))
    {
        ReportSceneHistoryFailure("Transform replay expected state changed; cursor preserved.");
        return;
    }

    if(!ApplyObjectTransformEdit(demoScene, *index, redo ? edit.after : edit.before))
    {
        ReportSceneHistoryFailure(m_transformEditStatus);
        return;
    }

    if(redo)
    {
        ++m_historyCursor;
    }
    else
    {
        --m_historyCursor;
    }

    m_sceneHistoryStatus.clear();
}

bool Editor::CanPreserveSceneHistory(const IDemoScene& demoScene,
    const std::filesystem::path& canonicalProject, std::uint64_t epoch) const
{
    if(epoch != m_historyEpoch)
    {
        return false;
    }

    if(!m_historySession)
    {
        return m_sceneHistory.empty();
    }

    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&demoScene);
    return owner && m_historySession->epoch == epoch &&
        m_historySession->projectIdentity == canonicalProject &&
        m_historySession->sceneIdentity == owner->GetPersistentSceneID() &&
        m_historyCurrentScenePath == owner->GetScenePath().lexically_normal() &&
        m_historyRuntimeIdentity == owner->GetRuntimeIdentity() &&
        m_historyObservedScene == &owner->GetScene() &&
        m_historyStructureRevision == owner->GetScene().GetStructureRevision();
}

void Editor::PublishRestoredObjectSelection(const FileBackedDemoScene& owner,
    std::optional<SceneObjectMetadata> selection, std::string activeSceneName, bool retainGroup) noexcept
{
    const auto& scene = owner.GetScene();
    m_historyObservedScene = &scene;
    m_historyRuntimeIdentity = owner.GetRuntimeIdentity();
    m_historyStructureRevision = scene.GetStructureRevision();

    auto retainedSelection = std::move(m_orderedSelection);
    m_inspectorPanel.DismissTexturePicker();
    m_inspectorPanel.ReleaseSceneObjectPreview();
    SuspendViewportInteraction();
    ResetSceneInteractions();
    if(retainGroup && retainedSelection.size() > 1) m_orderedSelection = std::move(retainedSelection);

    m_selectionSceneName = std::move(activeSceneName);
    m_selectionStructureRevision = scene.GetStructureRevision();
    m_selectionScene = &scene;
    m_selectionRuntimeIdentity = owner.GetRuntimeIdentity();

    if(!selection)
    {
        return;
    }

    m_selectedObject = selection->runtimeIndex;
    ObjectInspectionIdentity identity{&scene, scene.GetStructureRevision()};
    identity.runtimeIdentity = selection->runtimeIdentity;
    identity.objectID = std::move(selection->objectID);
    identity.sceneID = std::move(selection->sceneID);

    m_objectInspectionIdentity = std::move(identity);
    m_inspectorTarget = InspectorTarget::SceneObject;
    m_objectInspectionRequested = true;
}

bool Editor::HasPendingSceneHistoryOperation() const noexcept
{
    return m_pendingHistoryReplay.has_value() || m_stagedDeletionHistory.has_value() || m_pendingMaterialBinding.has_value();
}

bool Editor::MatchesPendingHistoryReplay(const HistoryReplayIdentity& identity) const noexcept
{
    return m_pendingHistoryReplay && identity.epoch == m_historyEpoch &&
        m_pendingHistoryReplay->identity.operationID == identity.operationID &&
        m_pendingHistoryReplay->identity.epoch == identity.epoch &&
        m_pendingHistoryReplay->identity.redo == identity.redo &&
        m_pendingHistoryReplay->identity.selectionID == identity.selectionID &&
        m_pendingHistoryReplay->cursor == m_historyCursor;
}

std::optional<Editor::HistoryReplayRequest> Editor::TakeSceneHistoryReplayRequest()
{
    return std::exchange(m_historyReplayRequest, std::nullopt);
}

size_t Editor::ObjectDeletionPayloadBytes(const ObjectDeletion& deletion)
{
    // Inline optional values (including Camera) are covered by sizeof(ObjectDeletion).
    const auto bytes = [](const std::string& text) { return text.capacity() + 1; };
    const auto& object = deletion.object;
    size_t size = sizeof(SceneHistoryEntry) + sizeof(ObjectDeletion) + bytes(object.id) + bytes(object.name) +
        bytes(object.meshReference) + bytes(object.materialReference);

    for(const auto& [slot, reference] : object.materialSlotBindings)
    {
        size += sizeof(std::pair<const std::string, std::string>) + 4 * sizeof(void*) + bytes(slot) + bytes(reference);
    }

    size += bytes(deletion.session.sceneIdentity) +
        (deletion.session.projectIdentity.native().capacity() + 1) * sizeof(std::filesystem::path::value_type) +
        (deletion.session.scenePath.native().capacity() + 1) * sizeof(std::filesystem::path::value_type);

    if(deletion.selectionBefore)
    {
        size += bytes(*deletion.selectionBefore);
    }

    if(deletion.selectionAfter)
    {
        size += bytes(*deletion.selectionAfter);
    }

    return size;
}

bool Editor::StageObjectDeletionHistory(SceneObjectDocument object, size_t documentIndex,
    std::optional<std::string> selectionBefore, std::optional<std::string> selectionAfter,
    std::uint64_t operationID, std::uint64_t epoch, bool replay, std::string& error, bool insertion)
{
    if(!m_historySession || epoch != m_historyEpoch || m_historySession->epoch != epoch)
    {
        error = "Deletion history cannot establish the current editing session.";
        return false;
    }

    if(replay)
    {
        if(!m_pendingHistoryReplay || m_pendingHistoryReplay->identity.operationID != operationID ||
            !m_pendingHistoryReplay->identity.redo || m_pendingHistoryReplay->identity.epoch != epoch ||
            m_pendingHistoryReplay->cursor != m_historyCursor || m_historyCursor >= m_sceneHistory.size())
        {
            error = "Redo deletion operation identity is stale.";
            return false;
        }

        const auto* owned = std::get_if<std::unique_ptr<ObjectDeletion>>(&m_sceneHistory[m_historyCursor].command);
        const auto* deletion = owned ? owned->get() : nullptr;

        if(!deletion || deletion->documentIndex != documentIndex || deletion->object.id != object.id ||
            deletion->object.name != object.name || deletion->object.parentID != object.parentID || !SameHistoryTransform(deletion->object.transform, object.transform) ||
            deletion->object.meshReference != object.meshReference ||
            deletion->object.materialReference != object.materialReference ||
            deletion->object.materialSlotBindings != object.materialSlotBindings ||
            deletion->object.physics != object.physics ||
            deletion->object.triggerCollider != object.triggerCollider ||
            deletion->object.characterController != object.characterController ||
            deletion->object.camera != object.camera ||
            deletion->object.light != object.light)
        {
            error = "Redo deletion target changed outside its history command; cursor preserved.";
            return false;
        }

        return true;
    }

    if(HasPendingSceneHistoryOperation())
    {
        error = "Another scene history operation is pending.";
        return false;
    }

    ObjectDeletion deletion{std::move(object), documentIndex, std::move(selectionBefore),
        std::move(selectionAfter), *m_historySession};
    deletion.insertion=insertion;
    const auto bytes = ObjectDeletionPayloadBytes(deletion);

    if(bytes > kSceneHistoryPayloadBytes)
    {
        error = "Deletion rejected: its undo snapshot exceeds the 4 MiB history payload limit.";
        ReportSceneHistoryFailure(error);
        return false;
    }

    m_sceneHistory.reserve(kSceneHistoryCapacity);
    m_stagedDeletionHistory = StagedDeletionHistory{
        SceneHistoryEntry{std::make_unique<ObjectDeletion>(std::move(deletion)), epoch, bytes}, operationID};
    return true;
}

void Editor::CompleteObjectDeletionHistory(std::uint64_t operationID, std::uint64_t epoch,
    bool succeeded, const std::string& error)
{
    if(m_pendingHistoryReplay && m_pendingHistoryReplay->identity.operationID == operationID &&
        m_pendingHistoryReplay->identity.epoch == epoch)
    {
        // The identity borrow remains valid until CompleteSceneHistoryReplay resets
        // pending state; it is not accessed afterward. No success-path allocation.
        CompleteSceneHistoryReplay(m_pendingHistoryReplay->identity, succeeded, error);
        return;
    }

    if(!m_stagedDeletionHistory || m_stagedDeletionHistory->operationID != operationID ||
        m_stagedDeletionHistory->entry.sessionEpoch != epoch || epoch != m_historyEpoch)
    {
        return;
    }

    if(succeeded)
    {
        auto entry = std::move(m_stagedDeletionHistory->entry);
        m_stagedDeletionHistory.reset();
        AppendSceneHistoryEntry(std::move(entry));
        return;
    }

    m_stagedDeletionHistory.reset();
    ReportSceneHistoryFailure(error);
}

void Editor::CompleteSceneHistoryReplay(const HistoryReplayIdentity& identity,
    bool succeeded, const std::string& error)
{
    if(!MatchesPendingHistoryReplay(identity))
    {
        return;
    }

    if(succeeded)
    {
        m_historyCursor = identity.redo ? m_historyCursor + 1 : m_historyCursor - 1;
        m_sceneHistoryStatus.clear();
    }
    else
    {
        ReportSceneHistoryFailure("Deferred undo/redo failed: " + error);
    }

    m_pendingHistoryReplay.reset();
    m_historyReplayRequest.reset();
}

void Editor::InvalidateSceneHistoryRedoAfterEdit() noexcept
{
    if(!HasPendingSceneHistoryOperation())
    {
        TruncateHistoryRedo();
    }
}

bool Editor::CommitObjectPhysics(IDemoScene& scene, const SceneObjectMetadata& selection,
    const std::optional<Physics::PhysicsComponent>& before,
    const std::optional<Physics::PhysicsComponent>& after)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    if(IsPhysicsEditingLocked() || !owner || !m_historySession ||
        !RenameSessionMatches(scene, *m_historySession) || HasPendingSceneHistoryOperation() ||
        m_sceneOperationPending || HasBlockingObjectCreation())
    {
        m_physicsEditStatus = "Physics authoring requires a stopped scene without pending edits.";
        return false;
    }
    if(after && !Physics::ValidatePhysicsComponent(*after, &m_physicsEditStatus)) return false;
    if(before == after) return true;
    try
    {
        auto command = std::make_unique<PhysicsComponentEdit>(PhysicsComponentEdit{
            *m_historySession, selection.objectID, before, after});
        const size_t bytes = sizeof(SceneHistoryEntry) + sizeof(PhysicsComponentEdit) +
            command->objectID.size() + command->session.sceneIdentity.size() +
            command->session.projectIdentity.native().size() * sizeof(std::filesystem::path::value_type) +
            command->session.scenePath.native().size() * sizeof(std::filesystem::path::value_type);
        if(bytes > kSceneHistoryPayloadBytes) return false;
        SceneHistoryEntry entry{std::move(command), m_historyEpoch, bytes};
        // Allocate history storage before mutating authored data; append is then noexcept.
        m_sceneHistory.reserve(kSceneHistoryCapacity);
        if(!owner->SetObjectPhysics(selection, before, after, &m_physicsEditStatus)) return false;
        AppendSceneHistoryEntry(std::move(entry));
        m_physicsEditStatus.clear();
        return true;
    }
    catch(const std::exception& exception)
    {
        m_physicsEditStatus = exception.what();
        return false;
    }
}

void Editor::DrawObjectPhysicsControls(IDemoScene& scene)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner)
    {
        return;
    }

    const auto selection = owner->GetObjectMetadata(m_selectedObject);
    SceneObjectDocument object;

    if(!selection || !owner->CapturePhysicsInspection(selection->objectID, object))
    {
        return;
    }

    // Meshless legacy objects keep access to their existing PhysicsComponent.
    if(object.meshReference.empty() && !object.physics)
    {
        return;
    }

    using Physics::BoxDimensions;
    using Physics::PhysicsComponent;

    // Numeric fields in draft-buffer order. Every shape's stored values persist while
    // inactive, so switching collider type never discards or derives dimensions.
    enum class PhysicsFieldShape
    {
        Box,
        Sphere,
        Capsule,
        Shared
    };

    struct PhysicsNumericField
    {
        const char* label;
        PhysicsFieldShape shape;
        InspectorDragSpec drag;
        float PhysicsComponent::* componentMember = nullptr;
        float BoxDimensions::* boxMember = nullptr;
        float Vector3::* offsetMember = nullptr;

        // Capsule local rotation is stored in radians (Transform convention) but edited
        // in degrees, like the object Rotation control.
        float Vector3::* rotationMember = nullptr;
    };

    // Drag sensitivities per pixel and drag-only limits. Limits stop a drag at the edge of
    // the valid range; typed values are not clamped, so ValidatePhysicsComponent still
    // rejects them with its existing message (for example a typed radius of 0).
    constexpr float largestFloat = (std::numeric_limits<float>::max)();
    static constexpr InspectorDragSpec lengthDrag{0.01f, 0.0f, largestFloat};
    static constexpr InspectorDragSpec offsetDrag{0.01f};
    static constexpr InspectorDragSpec angleDrag{0.25f};
    static constexpr InspectorDragSpec densityDrag{5.0f, 0.0f, largestFloat};
    static constexpr InspectorDragSpec frictionDrag{0.005f, 0.0f, largestFloat};
    static constexpr InspectorDragSpec restitutionDrag{0.005f, 0.0f, 1.0f};

    static constexpr std::array<PhysicsNumericField, 15> numericFields{{
        {"Width (X, m)", PhysicsFieldShape::Box, lengthDrag, nullptr, &BoxDimensions::width},
        {"Height (Y, m)", PhysicsFieldShape::Box, lengthDrag, nullptr, &BoxDimensions::height},
        {"Depth (Z, m)", PhysicsFieldShape::Box, lengthDrag, nullptr, &BoxDimensions::depth},
        {"Radius (m)##Sphere", PhysicsFieldShape::Sphere, lengthDrag, &PhysicsComponent::sphereRadius},
        {"Radius (m)##Capsule", PhysicsFieldShape::Capsule, lengthDrag, &PhysicsComponent::capsuleRadius},
        {"Cylinder Height (Y, m)", PhysicsFieldShape::Capsule, lengthDrag, &PhysicsComponent::capsuleCylinderHeight},
        {"Local Offset X (m)", PhysicsFieldShape::Shared, offsetDrag, nullptr, nullptr, &Vector3::x},
        {"Local Offset Y (m)", PhysicsFieldShape::Shared, offsetDrag, nullptr, nullptr, &Vector3::y},
        {"Local Offset Z (m)", PhysicsFieldShape::Shared, offsetDrag, nullptr, nullptr, &Vector3::z},
        {"Local Rotation X (deg)", PhysicsFieldShape::Capsule, angleDrag, nullptr, nullptr, nullptr, &Vector3::x},
        {"Local Rotation Y (deg)", PhysicsFieldShape::Capsule, angleDrag, nullptr, nullptr, nullptr, &Vector3::y},
        {"Local Rotation Z (deg)", PhysicsFieldShape::Capsule, angleDrag, nullptr, nullptr, nullptr, &Vector3::z},
        {"Density (kg/m3)", PhysicsFieldShape::Shared, densityDrag, &PhysicsComponent::density},
        {"Friction (>= 0)", PhysicsFieldShape::Shared, frictionDrag, &PhysicsComponent::friction},
        {"Restitution (Bounciness, 0..1)", PhysicsFieldShape::Shared, restitutionDrag, &PhysicsComponent::restitution}
    }};
    static_assert(numericFields.size() == std::tuple_size_v<decltype(m_physicsDragDrafts)>);

    const auto fieldValue = [](PhysicsComponent& component, const PhysicsNumericField& field) -> float&
    {
        if(field.boxMember)
        {
            return component.boxDimensions.*field.boxMember;
        }

        if(field.offsetMember)
        {
            return component.colliderLocalPositionOffset.*field.offsetMember;
        }

        if(field.rotationMember)
        {
            return component.capsuleLocalRotation.*field.rotationMember;
        }

        return component.*field.componentMember;
    };

    // Drafts show degrees for rotation fields and the stored value otherwise.
    const auto displayedValue = [&](PhysicsComponent component, const PhysicsNumericField& field)
    {
        const float storedValue = fieldValue(component, field);

        if(field.rotationMember)
        {
            return ToDegrees(storedValue);
        }

        return storedValue;
    };

    const auto storeDisplayedValue = [&](PhysicsComponent& component, const PhysicsNumericField& field, float value)
    {
        if(field.rotationMember)
        {
            fieldValue(component, field) = ToRadians(value);
            return;
        }

        fieldValue(component, field) = value;
    };

    const auto isFieldVisible = [](PhysicsFieldShape shape, Physics::ColliderType colliderType)
    {
        switch(shape)
        {
        case PhysicsFieldShape::Box:
            return colliderType == Physics::ColliderType::Box;

        case PhysicsFieldShape::Sphere:
            return colliderType == Physics::ColliderType::Sphere;

        case PhysicsFieldShape::Capsule:
            return colliderType == Physics::ColliderType::Capsule;

        default:
            return true;
        }
    };

    // Refresh drafts when the inspected object, history epoch or authored value changes.
    const bool changedTarget = !m_physicsInspection || m_physicsInspectionEpoch != m_historyEpoch ||
        m_physicsInspection->objectID != selection->objectID ||
        m_physicsInspection->runtimeIdentity != selection->runtimeIdentity ||
        m_physicsInspection->structureRevision != selection->structureRevision;
    const bool changedValue = m_physicsInspectedValue != object.physics;

    if(changedTarget || changedValue || IsPhysicsEditingLocked())
    {
        m_physicsInspection = selection;
        m_physicsInspectionEpoch = m_historyEpoch;
        m_physicsInspectedValue = object.physics;

        if(changedTarget)
        {
            m_physicsEditStatus.clear();
        }

        if(object.physics)
        {
            for(size_t index = 0; index < numericFields.size(); ++index)
            {
                m_physicsDragDrafts[index] = displayedValue(*object.physics, numericFields[index]);
            }
        }
    }

    // Widget IDs include the load epoch and stable object identity: drafts cannot migrate on selection.
    ImGui::PushID(static_cast<int>(m_historyEpoch));
    ImGui::PushID(selection->objectID.c_str());
    ImGui::Separator();
    ImGui::BeginDisabled(IsPhysicsEditingLocked() || HasPendingSceneHistoryOperation());

    bool enabled = object.physics.has_value();

    const bool controllerConflict = object.characterController.has_value() && !object.physics;
    ImGui::BeginDisabled(controllerConflict);

    if(ImGui::Checkbox("Physics", &enabled))
    {
        CommitObjectPhysics(scene, *selection, object.physics,
            enabled ? std::optional<PhysicsComponent>{PhysicsComponent{}} : std::nullopt);
    }

    ImGui::EndDisabled();

    if(controllerConflict)
    {
        ImGui::TextWrapped("Physics is unavailable while Character Controller is attached.");
    }

    if(object.physics)
    {
        auto candidate = *object.physics;
        int bodyType = static_cast<int>(candidate.bodyType);

        if(ImGui::Combo("Body Type", &bodyType, "Static\0Dynamic\0Kinematic\0"))
        {
            candidate.bodyType = static_cast<Physics::BodyType>(bodyType);
            CommitObjectPhysics(scene, *selection, object.physics, candidate);
        }

        // Combo indices equal the ColliderType values: Box 0, Sphere 1, Capsule 2.
        int colliderType = static_cast<int>(object.physics->colliderType);

        if(ImGui::Combo("Collider Type", &colliderType, "Box\0Sphere\0Capsule\0"))
        {
            auto edited = *object.physics;
            edited.colliderType = static_cast<Physics::ColliderType>(colliderType);
            CommitObjectPhysics(scene, *selection, object.physics, edited);
        }

        ImGui::TextWrapped("Full dimensions and local offset in meters; independent of visual scale.");

        if(object.physics->colliderType == Physics::ColliderType::Capsule)
        {
            ImGui::TextWrapped("Capsule axis is local +Y. Cylinder height excludes the rounded ends.");
        }

        for(size_t index = 0; index < numericFields.size(); ++index)
        {
            const PhysicsNumericField& field = numericFields[index];

            if(!isFieldVisible(field.shape, object.physics->colliderType))
            {
                continue;
            }

            if(field.componentMember == &PhysicsComponent::density &&
                object.physics->bodyType != Physics::BodyType::Dynamic)
            {
                continue;
            }

            // Dragging and typing change only the draft. A completed drag or typed entry is
            // committed once, so it creates one history entry; Escape restores the draft.
            float& draft = m_physicsDragDrafts[index];
            const InspectorDragResult result = InspectorDragFloatTransformWidth(field.label, draft, field.drag);

            if(result == InspectorDragResult::Committed)
            {
                if(!std::isfinite(draft))
                {
                    m_physicsEditStatus = "Enter a complete finite number; authored value was preserved.";
                }
                else
                {
                    auto edited = *object.physics;
                    storeDisplayedValue(edited, field, draft);
                    CommitObjectPhysics(scene, *selection, object.physics, edited);
                }
            }

            // Rejected and cancelled drafts are discarded, never written into the scene. A
            // successful commit refreshes every draft on the next frame.
            if(result == InspectorDragResult::Committed || result == InspectorDragResult::Cancelled)
            {
                draft = displayedValue(*object.physics, field);
            }
        }

        // Notification opt-in only: the collider stays solid either way.
        bool generateContactEvents = object.physics->generateContactEvents;

        if(ImGui::Checkbox("Generate Contact Events", &generateContactEvents))
        {
            auto edited = *object.physics;
            edited.generateContactEvents = generateContactEvents;
            CommitObjectPhysics(scene, *selection, object.physics, edited);
        }
    }

    if(!m_physicsEditStatus.empty())
    {
        ImGui::TextWrapped("%s", m_physicsEditStatus.c_str());
    }

    ImGui::EndDisabled();
    ImGui::PopID();
    ImGui::PopID();
}


std::optional<Editor::PrimitiveCreationRequest> Editor::TakePrimitiveCreationRequest()
{
    return std::exchange(m_primitiveCreationRequest,std::nullopt);
}

std::optional<Editor::EmptyObjectCreationRequest> Editor::TakeEmptyObjectCreationRequest()
{
    return std::exchange(m_emptyObjectCreationRequest, std::nullopt);
}

std::optional<Editor::CameraObjectCreationRequest> Editor::TakeCameraObjectCreationRequest()
{
    return std::exchange(m_cameraObjectCreationRequest, std::nullopt);
}

void Editor::OnCameraObjectCreationFailed(const std::string& error)
{
    m_meshCreationStatus = "Camera creation rejected: " + error;
    Log::Warning(m_meshCreationStatus);
}

std::optional<Editor::LightObjectCreationRequest> Editor::TakeLightObjectCreationRequest()
{
    return std::exchange(m_lightObjectCreationRequest, std::nullopt);
}

void Editor::OnLightObjectCreationFailed(const std::string& error)
{
    m_meshCreationStatus = "Light creation rejected: " + error;
    Log::Warning(m_meshCreationStatus);
}

void Editor::OnEmptyObjectCreationFailed(const std::string& error)
{
    // Shares the viewport creation status line with mesh and primitive creation.
    m_meshCreationStatus = "Empty Object creation rejected: " + error;
    Log::Warning(m_meshCreationStatus);
}

bool Editor::StageHierarchyHistory(const SceneHierarchyEdit& edit,
    std::optional<std::string> selectionBefore, std::optional<std::string> selectionAfter,
    std::uint64_t operationID, std::string& error)
{
    if(!m_historySession || m_historySession->epoch != m_historyEpoch || HasPendingSceneHistoryOperation() ||
        !ValidateSceneDocument(edit.before, &error) || !ValidateSceneDocument(edit.after, &error) ||
        edit.before.id != edit.after.id || SceneHierarchy::SameObjects(edit.before, edit.after))
    {
        error = "Hierarchy history requires a changed valid scene and an idle history session.";
        return false;
    }

    auto command = std::make_unique<ObjectDeletion>();
    command->session = *m_historySession;
    command->selectionBefore = std::move(selectionBefore);
    command->selectionAfter = std::move(selectionAfter);
    command->hierarchy = edit;
    size_t bytes = ObjectDeletionPayloadBytes(*command) + sizeof(SceneHierarchyEdit);

    for(const auto* document : {&command->hierarchy->before, &command->hierarchy->after})
    {
        // SceneObjectDocument includes inline Camera/physics optionals; only strings/maps
        // need separate dynamic-storage accounting below.
        bytes += sizeof(SceneDocument) + document->objects.capacity() * sizeof(SceneObjectDocument);
        bytes += document->id.capacity() + document->name.capacity() + document->environmentReference.capacity() + 3;

        for(const auto& object : document->objects)
        {
            bytes += object.id.capacity() + object.parentID.capacity() + object.name.capacity() +
                object.meshReference.capacity() + object.materialReference.capacity() + 5;

            for(const auto& [slot, reference] : object.materialSlotBindings)
            {
                bytes += sizeof(std::pair<const std::string, std::string>) + 4 * sizeof(void*) +
                    slot.capacity() + reference.capacity() + 2;
            }
        }
    }

    for(const auto& [duplicate, source] : command->hierarchy->bindingSources)
    {
        bytes += sizeof(std::pair<const std::string, std::string>) + 4 * sizeof(void*) +
            duplicate.capacity() + source.capacity() + 2;
    }

    if(bytes > kSceneHistoryPayloadBytes)
    {
        error = "Hierarchy edit exceeds the 4 MiB undo payload limit; scene unchanged.";
        return false;
    }

    m_sceneHistory.reserve(kSceneHistoryCapacity);
    m_stagedDeletionHistory = StagedDeletionHistory{
        SceneHistoryEntry{std::move(command), m_historyEpoch, bytes}, operationID};
    return true;
}

bool Editor::QueueHierarchyEdit(IDemoScene& scene, SceneHierarchyEdit edit,
    std::optional<std::string> selectionAfter, std::string& error)
{
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);

    if(IsPhysicsEditingLocked() || !owner || !m_historySession ||
        !CanPreserveSceneHistory(scene, m_historySession->projectIdentity, m_historyEpoch) ||
        HasPendingSceneHistoryOperation() || m_sceneOperationPending || HasBlockingObjectCreation() ||
        HasBlockingMaterialOperation() || m_transformFieldEditing || m_gizmoWasUsing)
    {
        error = "Hierarchy edit requires a stopped scene without pending operations or gestures.";
        return false;
    }

    if(SceneHierarchy::SameObjects(edit.before, edit.after))
    {
        return true;
    }

    const auto selected = owner->GetObjectMetadata(m_selectedObject);
    HistoryReplayRequest request;
    request.identity = {AllocateSceneHistoryOperationID(), m_historyEpoch, false, selectionAfter};
    request.membership = true;
    request.hierarchy = std::move(edit);

    if(!StageHierarchyHistory(*request.hierarchy, selected ? std::optional<std::string>(selected->objectID) : std::nullopt,
        selectionAfter, request.identity.operationID, error))
    {
        return false;
    }

    m_duplicationRequest = std::move(request);
    return true;
}

bool Editor::QueueHierarchyReparent(IDemoScene& scene, std::string_view objectID, std::string_view parentID,
    bool keepWorld, std::string_view beforeSibling, std::string& error)
{
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
    SceneHierarchyEdit edit;

    if(!owner || !CaptureSceneDocument(owner->m_runtime->GetFileScene().GetDocument(), owner->GetScene(),
        owner->m_runtime->GetObjectMapping(), edit.before, &error) ||
        !SceneHierarchy::Reparent(edit.before, objectID, parentID, keepWorld, beforeSibling, edit.after, error))
    {
        return false;
    }

    const auto selected = owner->GetObjectMetadata(m_selectedObject);
    return QueueHierarchyEdit(scene, std::move(edit),
        selected ? std::optional<std::string>(selected->objectID) : std::nullopt, error);
}

bool Editor::QueueHierarchyCreation(IDemoScene& scene, SceneObjectDocument object, std::string_view parentID,
    std::string& error)
{
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
    SceneHierarchyEdit edit;

    if(!owner || !CaptureSceneDocument(owner->m_runtime->GetFileScene().GetDocument(), owner->GetScene(),
        owner->m_runtime->GetObjectMapping(), edit.before, &error))
    {
        return false;
    }

    object.parentID = parentID;
    edit.after = edit.before;
    auto selection = object.id;
    edit.after.objects.push_back(std::move(object));
    return QueueHierarchyEdit(scene, std::move(edit), std::move(selection), error);
}

bool Editor::PrepareHierarchyDrop(IDemoScene& scene, const HierarchyDropRequest& request,
    SceneHierarchyEdit& edit, std::string& error)
{
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);

    if(!owner || owner->IsArchiveActivation() || IsPhysicsEditingLocked() || !m_historySession ||
        !CanPreserveSceneHistory(scene, m_historySession->projectIdentity, m_historyEpoch) ||
        request.identity != HierarchyDragIdentity{m_historyEpoch, owner->GetRuntimeIdentity(),
            scene.GetScene().GetStructureRevision(), owner->m_runtime->GetHierarchyRevision()} ||
        HasPendingSceneHistoryOperation() || m_sceneOperationPending || HasBlockingObjectCreation() ||
        m_fileRequest || m_sceneObjectDeletionPending || m_sceneObjectDeletionRequest ||
        m_materialAssignmentRequest || m_meshCreationRequest || m_renameIntent ||
        m_workspace.IsSplitterDragging() || ImGuizmo::IsUsing() || ImGui::GetIO().AppFocusLost)
    {
        error = "Drag belongs to a stale or busy scene.";
        return false;
    }

    SceneHierarchySnapshot snapshot;
    std::string parentID;
    std::string beforeSibling;

    if(!owner->BuildHierarchySnapshot(snapshot, &error) ||
        !ResolveHierarchyDrop(snapshot, request, parentID, beforeSibling, error) ||
        !CaptureSceneDocument(owner->m_runtime->GetFileScene().GetDocument(), owner->GetScene(),
            owner->m_runtime->GetObjectMapping(), edit.before, &error))
    {
        return false;
    }

    return SceneHierarchy::Reparent(edit.before, request.sourceID, parentID, request.keepWorld,
        beforeSibling, edit.after, error);
}

bool Editor::SubmitHierarchyDrop(IDemoScene& scene, const HierarchyDropRequest& request, std::string& error)
{
    SceneHierarchyEdit edit;

    if(!PrepareHierarchyDrop(scene, request, edit, error))
    {
        return false;
    }

    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
    const auto selected = owner->GetObjectMetadata(m_selectedObject);
    return QueueHierarchyEdit(scene, std::move(edit),
        selected ? std::optional<std::string>(selected->objectID) : std::nullopt, error);
}

bool Editor::QueueHierarchyEmpty(IDemoScene& scene, std::string_view parentID, std::string& error)
{
    try
    {
        const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
        SceneDocument document;

        if(!owner || !CaptureSceneDocument(owner->m_runtime->GetFileScene().GetDocument(), owner->GetScene(),
            owner->m_runtime->GetObjectMapping(), document, &error))
        {
            return false;
        }

        return QueueHierarchyCreation(scene, PrepareEmptyObject(document), parentID, error);
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}
