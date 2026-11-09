#include "Editor.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Scene/Scene.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"
#include <algorithm>

namespace
{
    // Screen position of a world point in front of the camera; nullopt behind it or for non-finite input.
    std::optional<ImVec2> ProjectPoint(const Matrix4& viewProjection, const Vector3& point, ImVec2 origin, ImVec2 size)
    {
        float clip[4]{};

        for(size_t row = 0; row < 4; ++row)
        {
            clip[row] = viewProjection.m[row][0] * point.x + viewProjection.m[row][1] * point.y +
                viewProjection.m[row][2] * point.z + viewProjection.m[row][3];
        }

        if(!std::isfinite(clip[0]) || !std::isfinite(clip[1]) || !std::isfinite(clip[3]) || clip[3] <= 0.000001f)
        {
            return std::nullopt;
        }

        return ImVec2(origin.x + (clip[0] / clip[3] + 1.0f) * 0.5f * size.x,
            origin.y + (1.0f - clip[1] / clip[3]) * 0.5f * size.y);
    }
}

void Editor::BuildViewportIcons(const IDemoScene& demoScene, const EditorIcons& icons)
{
    m_viewportIcons.clear();
    m_viewportListenerFacings.clear();
    m_viewportActiveListener.reset();
    m_viewportIconFrame = -1;
    m_viewportIconScene = nullptr;
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&demoScene);

    if(!owner || !owner->m_runtime || !owner->m_runtime->GetFileScene().HasDocument() ||
        m_viewportImage.size.x <= 0 || m_viewportImage.size.y <= 0 ||
        (!m_showCameraIcons && !m_showLightIcons && !m_showAudioIcons))
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
    const auto& documentObjects = owner->m_runtime->GetFileScene().GetDocument().objects;
    const auto* activeListener = FindActiveAudioListener(documentObjects);

    for(const auto& object : documentObjects)
    {
        const bool showCamera = object.camera && m_showCameraIcons;
        const bool showLight = object.light && m_showLightIcons;
        const bool showSource = object.audioSource && m_showAudioIcons;
        const bool showListener = object.audioListener && m_showAudioIcons;

        if(!showCamera && !showLight && !showSource && !showListener)
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

        const float screenSize = ViewportIcons::ScreenSize(camera.GetPosition(), *anchor, ImGui::GetFontSize());
        // Audio icons beside a camera/light icon are offset so both stay visible and pickable.
        float offset = 0;
        const auto addIcon = [&](EditorIcon artwork)
        {
            ViewportIcons::Icon candidate;
            candidate.objectIndex = found->second;
            candidate.artwork = artwork;

            if(!ViewportIcons::Project(projection, *anchor, m_viewportImage.position, m_viewportImage.size,
                icons.GetSize(artwork, screenSize), candidate))
            {
                return false;
            }

            candidate.minimum.x += offset;
            candidate.maximum.x += offset;
            candidate.pickable = candidate.pickable && candidate.maximum.x <= m_viewportImage.position.x + m_viewportImage.size.x;
            m_viewportIcons.push_back(candidate);
            offset += (candidate.maximum.x - candidate.minimum.x) * 0.9f;
            return true;
        };

        if(showCamera || showLight)
        {
            addIcon(SceneObjectIcon(showCamera, showLight ? &*object.light : nullptr));
        }

        if(showSource)
        {
            addIcon(EditorIcon::AudioSource);
        }

        if(showListener && addIcon(EditorIcon::AudioListener))
        {
            const auto& icon = m_viewportIcons.back();

            if(&object == activeListener)
            {
                m_viewportActiveListener = icon.objectIndex;
            }

            const Vector3 facing{model.m[0][2], model.m[1][2], model.m[2][2]};
            const float length = facing.Length();

            if(std::isfinite(length) && length > 0.000001f)
            {
                // A fixed 1 m facing line from the object's position along its presented forward (+Z).
                const Vector3 tip = *anchor + facing * (1.0f / length);
                const auto start = ProjectPoint(projection, *anchor, m_viewportImage.position, m_viewportImage.size);
                const auto end = ProjectPoint(projection, tip, m_viewportImage.position, m_viewportImage.size);

                if(start && end)
                {
                    // The line starts under its icon, which may sit beside a camera or light icon.
                    const float shift = (icon.minimum.x + icon.maximum.x) * 0.5f - start->x;
                    m_viewportListenerFacings.push_back({ImVec2(start->x + shift, start->y),
                        ImVec2(end->x + shift, end->y),
                        &object == activeListener});
                }
            }
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
    // Game Play hides selection, gizmos and icons; picking cannot select through the game view.
    if(m_viewportImage.size.x <= 0 || m_viewportImage.size.y <= 0 || IsGamePlaying())
    {
        m_viewportGizmoInteractive = false;
        m_viewportIcons.clear();
        m_viewportListenerFacings.clear();
        m_viewportActiveListener.reset();
        m_viewportIconFrame = -1;
        m_viewportIconScene = nullptr;

        // M9.6: the pathing overlay is what Play is for, so it stays over the game view when asked for. Display only, no input.
        const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);

        if(owner && owner->m_runtime && owner->m_runtime->GetFileScene().HasDocument() && m_viewportImage.size.x > 0 && m_viewportImage.size.y > 0)
        {
            ImDrawList* draw = ImGui::GetForegroundDrawList();
            const ImVec2 end(m_viewportImage.position.x + m_viewportImage.size.x, m_viewportImage.position.y + m_viewportImage.size.y);
            draw->PushClipRect(m_viewportImage.position, end, true);
            const auto& camera = scene.GetScene().GetCamera();
            DrawPathingOverlay(*owner, *draw, camera.GetProjectionMatrix() * camera.GetViewMatrix(), m_viewportImage.position, m_viewportImage.size);
            draw->PopClipRect();
        }
        else if(m_audioSystem)
        {
            m_audioSystem->SetPathingVisualization(false);
        }

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
    {
        const auto& camera = scene.GetScene().GetCamera();
        const Matrix4 viewProjection = camera.GetProjectionMatrix() * camera.GetViewMatrix();
        DrawProbeVolumeOverlay(scene, *draw, viewProjection, m_viewportImage.position, m_viewportImage.size);
    }

    // Listener facing lines under the icons; the active listener is green, others grey.
    for(const auto& facing : m_viewportListenerFacings)
    {
        draw->AddLine(facing.start, facing.end, IM_COL32(10, 10, 10, 200), outline * 4.0f);
        draw->AddLine(facing.start, facing.end,
            facing.active ? IM_COL32(110, 230, 110, 255) : IM_COL32(160, 160, 160, 220), outline * 2.0f);
    }

    for(const auto& icon : m_viewportIcons)
    {
        const ImVec2 size(icon.maximum.x - icon.minimum.x, icon.maximum.y - icon.minimum.y);
        const bool selected = icon.objectIndex == m_selectedObject ||
            std::find(m_selectedObjects.begin(), m_selectedObjects.end(), icon.objectIndex) != m_selectedObjects.end();
        const bool over = hovered && *hovered == icon.objectIndex;
        const bool activeListener = icon.artwork == EditorIcon::AudioListener && m_viewportActiveListener == icon.objectIndex;
        const ImU32 tint = selected ? IM_COL32(255, 205, 90, 255) :
            over ? IM_COL32(120, 220, 255, 255) :
            activeListener ? IM_COL32(110, 230, 110, 255) : IM_COL32(235, 235, 235, 240);

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
