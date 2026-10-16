#include "Editor.h"
#include "ViewportMarquee.h"
#include "DemoContent/IDemoScene.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"

void Editor::SetViewportPublicationIdentity(std::uint64_t rendererLifetime, std::uint64_t bindingEpoch)
{
    if(rendererLifetime != m_viewportRendererLifetime || bindingEpoch != m_viewportBindingEpoch)
    {
        CancelViewportMarquee();
    }
    m_viewportRendererLifetime = rendererLifetime;
    m_viewportBindingEpoch = bindingEpoch;
}

void Editor::CancelViewportMarquee()
{
    // Committed selection never changed during preview, so cancellation needs no restoration writes.
    m_marqueeGesture.reset();
}

bool Editor::ViewportMarqueeMatches(const IDemoScene& scene) const
{
    if(!m_marqueeGesture)
    {
        return false;
    }
    const auto& gesture = *m_marqueeGesture;
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
    const auto runtimeIdentity = owner ? owner->GetRuntimeIdentity() : 0;
    const bool sameScene = gesture.sceneIdentity == &scene.GetScene() &&
        gesture.structureRevision == scene.GetScene().GetStructureRevision() && gesture.runtimeIdentity == runtimeIdentity;
    const bool samePublication = gesture.rendererLifetime == m_viewportRendererLifetime &&
        gesture.bindingEpoch == m_viewportBindingEpoch && m_viewportRendererLifetime != 0;
    const bool sameImage = gesture.image.position.x == m_viewportImage.position.x &&
        gesture.image.position.y == m_viewportImage.position.y && gesture.image.size.x == m_viewportImage.size.x &&
        gesture.image.size.y == m_viewportImage.size.y;
    return sameScene && samePublication && sameImage;
}

void Editor::RefreshViewportMarqueePreview(const IDemoScene& scene)
{
    if(!ViewportMarqueeMatches(scene))
    {
        CancelViewportMarquee();
        return;
    }
    auto& gesture = *m_marqueeGesture;
    if(!gesture.active)
    {
        return;
    }
    const auto& renderScene = scene.GetScene();
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
    const Matrix4 viewProjection = renderScene.GetCamera().GetProjectionMatrix() * renderScene.GetCamera().GetViewMatrix();
    const auto rectangle = ViewportMarquee::Normalize(gesture.anchor, gesture.cursor);
    gesture.candidates.clear();
    const auto& objects = renderScene.GetRenderObjects();
    if(gesture.objectIdentities.size() != objects.size())
    {
        CancelViewportMarquee();
        return;
    }
    // Metadata validates the complete document/map once per preview, not once per hit.
    if(owner && !objects.empty() && !owner->GetObjectMetadata(std::size_t{0}))
    {
        CancelViewportMarquee();
        return;
    }
    for(std::size_t index = 0; index < objects.size(); ++index)
    {
        const auto& object = objects[index];
        if(!object.mesh || object.materialsDeferred || !scene.GetMeshGeometry(*object.mesh))
        {
            continue;
        }
        ViewportMarquee::Rectangle projectedBounds;
        try
        {
            const Matrix4 clipFromLocal = viewProjection * object.GetPresentationTransform().currentModel;
            if(!ViewportMarquee::ProjectBounds(object.mesh->GetBounds(), clipFromLocal, gesture.image.size, projectedBounds) ||
                !ViewportMarquee::Overlaps(rectangle, projectedBounds)) continue;
        }
        catch(const std::exception&)
        {
            continue;
        }
        gesture.candidates.push_back(gesture.objectIdentities[index]);
    }
    ViewportMarquee::CombineSelection(gesture.originalSelection, gesture.candidates, gesture.mode, gesture.previewSelection);
    gesture.previewIndices.clear();
    for(const auto& identity : gesture.previewSelection)
    {
        if(owner)
        {
            const auto found = gesture.persistentIndices.find(identity.objectID);
            if(found == gesture.persistentIndices.end())
            {
                CancelViewportMarquee();
                return;
            }
            gesture.previewIndices.push_back(found->second);
        }
        else
        {
            if(identity.proceduralIndex >= objects.size())
            {
                CancelViewportMarquee();
                return;
            }
            gesture.previewIndices.push_back(identity.proceduralIndex);
        }
    }
}

const std::vector<std::size_t>* Editor::GetViewportMarqueePreview(const IDemoScene& scene) const
{
    if(!ViewportMarqueeMatches(scene) || !m_marqueeGesture->active)
    {
        return nullptr;
    }
    return &m_marqueeGesture->previewIndices;
}

void Editor::DrawViewportMarquee() const
{
    const auto& gesture = *m_marqueeGesture;
    const auto rectangle = ViewportMarquee::Normalize(gesture.anchor, gesture.cursor);
    const ImVec2 minimum(gesture.image.position.x + rectangle.minimum.x, gesture.image.position.y + rectangle.minimum.y);
    const ImVec2 maximum(gesture.image.position.x + rectangle.maximum.x, gesture.image.position.y + rectangle.maximum.y);
    // Append to the image window so floating panels and toolbar remain above the rectangle.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::Begin("Scene Viewport", nullptr, flags);
    auto* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(gesture.image.position,
        ImVec2(gesture.image.position.x + gesture.image.size.x, gesture.image.position.y + gesture.image.size.y), true);
    drawList->AddRectFilled(minimum, maximum, IM_COL32(55, 130, 255, 45));
    drawList->AddRect(minimum, maximum, IM_COL32(70, 155, 255, 230));
    drawList->PopClipRect();
    ImGui::End();
}

bool Editor::UpdateViewportMarquee(IDemoScene& scene)
{
    if(!m_marqueeGesture)
    {
        return false;
    }
    const auto& io = ImGui::GetIO();
    const bool navigationOwnsInput = m_cameraLooking || m_panLeftHeld ||
        ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    const bool uiOwnsInput = ImGui::GetDragDropPayload() || ImGui::IsAnyItemActive() ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool conflictingInput = io.AppFocusLost || !ImGui::IsMousePosValid() || m_sceneOperationPending ||
        m_workspace.IsSplitterDragging() || navigationOwnsInput || uiOwnsInput || ImGuizmo::IsUsing();
    if(conflictingInput || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || !ViewportMarqueeMatches(scene) ||
        (!io.MouseDown[ImGuiMouseButton_Left] && !io.MouseReleased[ImGuiMouseButton_Left]))
    {
        CancelViewportMarquee();
        return true;
    }

    auto& gesture = *m_marqueeGesture;
    gesture.cursor = {io.MousePos.x - gesture.image.position.x, io.MousePos.y - gesture.image.position.y};
    if(!gesture.active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, io.MouseDragThreshold))
    {
        gesture.active = true;
    }
    if(gesture.active)
    {
        ImGui::SetKeyOwner(ImGuiKey_MouseLeft, ImHashStr("ViewportMarquee"), ImGuiInputFlags_LockThisFrame);
        RefreshViewportMarqueePreview(scene);
        if(!m_marqueeGesture)
        {
            return true;
        }
    }
    if(io.MouseReleased[ImGuiMouseButton_Left])
    {
        if(gesture.active)
        {
            m_orderedSelection = std::move(gesture.previewSelection);
            m_selectedObject = InvalidObjectSelection;
            RefreshSceneSelection(scene);
            m_objectInspectionRequested = true;
        }
        else if(!gesture.preserveEmptyClick)
        {
            SelectSceneObject(scene, InvalidObjectSelection);
        }
        CancelViewportMarquee();
        return true;
    }
    if(gesture.active)
    {
        DrawViewportMarquee();
    }
    return true;
}

void Editor::StartViewportMarquee(const IDemoScene& scene, ImVec2 imageMouse)
{
    ViewportMarquee::Gesture gesture;
    gesture.anchor = imageMouse;
    gesture.cursor = imageMouse;
    gesture.image = m_viewportImage;
    gesture.sceneIdentity = &scene.GetScene();
    gesture.structureRevision = scene.GetScene().GetStructureRevision();
    const auto& objects = scene.GetScene().GetRenderObjects();
    gesture.objectIdentities.resize(objects.size());
    if(const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene))
    {
        gesture.runtimeIdentity = owner->GetRuntimeIdentity();
        SceneHierarchySnapshot snapshot;
        if(!owner->BuildHierarchySnapshot(snapshot))
        {
            return;
        }
        for(const auto& entry : snapshot.entries)
        {
            if(!entry.runtimeIndex || *entry.runtimeIndex >= objects.size())
            {
                return;
            }
            gesture.objectIdentities[*entry.runtimeIndex] = {entry.objectID};
            gesture.persistentIndices.emplace(entry.objectID, *entry.runtimeIndex);
        }
    }
    else
    {
        for(std::size_t index = 0; index < objects.size(); ++index)
        {
            gesture.objectIdentities[index].proceduralIndex = index;
        }
    }
    gesture.rendererLifetime = m_viewportRendererLifetime;
    gesture.bindingEpoch = m_viewportBindingEpoch;
    const auto& io = ImGui::GetIO();
    if(io.KeyCtrl)
    {
        gesture.mode = ViewportMarquee::SelectionMode::Toggle;
    }
    else if(io.KeyShift)
    {
        gesture.mode = ViewportMarquee::SelectionMode::Add;
    }
    gesture.preserveEmptyClick = io.KeyShift;
    gesture.originalSelection = m_orderedSelection;
    m_marqueeGesture = std::move(gesture);
}
