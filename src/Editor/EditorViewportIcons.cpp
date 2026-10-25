#include "Editor.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Scene/Scene.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"
#include <algorithm>

void Editor::BuildViewportIcons(const IDemoScene& demoScene, const EditorIcons& icons)
{
    m_viewportIcons.clear();
    m_viewportIconFrame = -1;
    m_viewportIconScene = nullptr;
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&demoScene);

    if(!owner || !owner->m_runtime || !owner->m_runtime->GetFileScene().HasDocument() ||
        m_viewportImage.size.x <= 0 || m_viewportImage.size.y <= 0 ||
        (!m_showCameraIcons && !m_showLightIcons))
    {
        return;
    }

    const auto& scene = demoScene.GetScene();
    const auto& mapping = owner->m_runtime->GetObjectMapping();
    const auto& objects = scene.GetRenderObjects();

    if(mapping.scene != &scene || mapping.structureRevision != scene.GetStructureRevision())
    {
        return;
    }

    const auto& camera = scene.GetCamera();
    const auto forward = camera.GetTarget() - camera.GetPosition();
    const auto right = Vector3::Cross(Vector3{0, 1, 0}, forward.Normalized());

    if(!std::isfinite(forward.Length()) || forward.Length() <= 0.000001f ||
        !std::isfinite(right.Length()) || right.Length() <= 0.000001f ||
        !std::isfinite(camera.GetNearPlane()) || !std::isfinite(camera.GetFarPlane()) ||
        camera.GetNearPlane() <= 0 || camera.GetFarPlane() <= camera.GetNearPlane() ||
        !std::isfinite(camera.GetVerticalFov()) || camera.GetVerticalFov() <= 0 ||
        camera.GetVerticalFov() >= 3.14159265f || !std::isfinite(camera.GetAspectRatio()) ||
        camera.GetAspectRatio() <= 0)
    {
        return;
    }

    const auto projection = camera.GetProjectionMatrix() * camera.GetViewMatrix();

    for(const auto& object : owner->m_runtime->GetFileScene().GetDocument().objects)
    {
        const bool showCamera = object.camera && m_showCameraIcons;
        const bool showLight = object.light && m_showLightIcons;

        if(!showCamera && !showLight)
        {
            continue;
        }

        const auto found = mapping.idToRuntimeIndex.find(object.id);

        if(found == mapping.idToRuntimeIndex.end() || found->second >= objects.size())
        {
            continue;
        }

        const auto model = objects[found->second].GetPresentationTransform().currentModel;
        const auto anchor = ViewportIcons::WorldAnchor(model);

        if(!anchor)
        {
            continue;
        }

        ViewportIcons::Icon candidate;
        candidate.objectIndex = found->second;
        candidate.artwork = SceneObjectIcon(showCamera, showLight ? &*object.light : nullptr);
        const float screenSize = ViewportIcons::ScreenSize(camera.GetPosition(), *anchor, ImGui::GetFontSize());

        if(ViewportIcons::Project(projection, *anchor, m_viewportImage.position, m_viewportImage.size,
            icons.GetSize(candidate.artwork, screenSize), candidate))
        {
            m_viewportIcons.push_back(candidate);
        }
    }

    std::sort(m_viewportIcons.begin(), m_viewportIcons.end(), ViewportIcons::Behind);
    m_viewportIconScene = &scene;
    m_viewportIconRuntime = owner->GetRuntimeIdentity();
    m_viewportIconRevision = scene.GetStructureRevision();
    m_viewportIconFrame = ImGui::GetFrameCount();
}

std::optional<size_t> Editor::PickViewportIcon(const IDemoScene& scene, ImVec2 mouse) const
{
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);

    if(m_viewportIconFrame != ImGui::GetFrameCount() || m_viewportIconScene != &scene.GetScene() ||
        !owner || m_viewportIconRuntime != owner->GetRuntimeIdentity() ||
        m_viewportIconRevision != scene.GetScene().GetStructureRevision())
    {
        return std::nullopt;
    }

    return ViewportIcons::Pick(m_viewportIcons, mouse);
}

void Editor::DrawViewportTools(IDemoScene& scene, const EditorIcons& icons, bool viewportWasFocused)
{
    if(m_viewportImage.size.x <= 0 || m_viewportImage.size.y <= 0)
    {
        m_viewportGizmoInteractive = false;
        m_viewportIcons.clear();
        m_viewportIconFrame = -1;
        m_viewportIconScene = nullptr;
        return;
    }

    // Keep the original input-less viewport window and layer gizmos above the icons.
    // No ImGui item, input capture, or renderer target is introduced by the overlay.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::Begin("Scene Viewport", nullptr, flags);
    auto* draw = ImGui::GetWindowDrawList();
    auto& layers = m_viewportIconLayers;
    layers.Split(draw, 2);
    layers.SetCurrentChannel(draw, 1);
    ImGui::End();

    DrawTransformGizmo(scene, m_viewportImage.size.x, m_viewportImage.size.y);
    // Gizmo manipulation may update an anchor, so rebuild after it, on every frame.
    BuildViewportIcons(scene, icons);
    HandleViewportSelection(scene, m_viewportImage.size.x, m_viewportImage.size.y, viewportWasFocused);

    ImGui::Begin("Scene Viewport", nullptr, flags);
    layers.SetCurrentChannel(draw, 0);
    const ImVec2 end(m_viewportImage.position.x + m_viewportImage.size.x,
        m_viewportImage.position.y + m_viewportImage.size.y);
    draw->PushClipRect(m_viewportImage.position, end, true);
    const auto& io = ImGui::GetIO();
    const bool allowHover = m_viewportHovered && !m_sceneOperationPending && !m_cameraLooking &&
        !m_panLeftHeld && !m_marqueeGesture && !ImGuizmo::IsUsing() &&
        !(m_viewportGizmoInteractive && m_selectedObject != InvalidObjectSelection && ImGuizmo::IsOver()) &&
        !io.MouseDown[ImGuiMouseButton_Right] && !io.MouseDown[ImGuiMouseButton_Middle] &&
        !ImGui::GetDragDropPayload() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    const auto hovered = allowHover ? PickViewportIcon(scene, io.MousePos) : std::nullopt;
    const float outline = (std::max)(1.0f, ImGui::GetFontSize() / 13.0f);

    for(const auto& icon : m_viewportIcons)
    {
        const ImVec2 size(icon.maximum.x - icon.minimum.x, icon.maximum.y - icon.minimum.y);
        const bool selected = icon.objectIndex == m_selectedObject ||
            std::find(m_selectedObjects.begin(), m_selectedObjects.end(), icon.objectIndex) != m_selectedObjects.end();
        const bool over = hovered && *hovered == icon.objectIndex;
        const ImU32 tint = selected ? IM_COL32(255, 205, 90, 255) :
            over ? IM_COL32(120, 220, 255, 255) : IM_COL32(235, 235, 235, 240);

        // A narrow dark halo keeps the light/tinted outline legible on bright scene pixels.
        for(const auto offset : {ImVec2(-outline, 0), ImVec2(outline, 0), ImVec2(0, -outline), ImVec2(0, outline)})
        {
            icons.Draw(*draw, icon.artwork, ImVec2(icon.minimum.x + offset.x, icon.minimum.y + offset.y),
                size, IM_COL32(10, 10, 10, 230));
        }

        icons.Draw(*draw, icon.artwork, icon.minimum, size, tint);
    }

    draw->PopClipRect();
    layers.Merge(draw);
    ImGui::End();
}
