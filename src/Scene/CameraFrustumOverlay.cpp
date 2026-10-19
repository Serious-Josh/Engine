#include "Scene/FileSceneRuntime.h"
#include "Scene/RenderView.h"
#include "Renderer/Renderer.h"

#include <algorithm>
#include <thread>

bool FileSceneRuntime::CaptureEditorCameraFrusta(const Renderer& renderer, EditorColliderOverlay& overlay,
    std::uint32_t viewportWidth, std::uint32_t viewportHeight, std::span<const size_t> selectedRuntimeIndices,
    std::vector<std::string>& rejected, std::string& error) const
{
    rejected.clear();
    error.clear();

    // Editor-only, owner-thread, current-renderer capture, as for collider overlays.
    const bool ownsActiveRendererScene = renderer.m_sceneResourceScene == &m_scene &&
        renderer.m_sceneResourceLifetime != 0;

    if(m_archiveScene || std::this_thread::get_id() != m_ownerThread || !ownsActiveRendererScene)
    {
        error = "Camera frusta require the active editable scene.";
        return false;
    }

    if(viewportWidth == 0 || viewportHeight == 0)
    {
        error = "Camera frusta require a nonzero Scene viewport extent.";
        return false;
    }

    if(overlay.scene == nullptr)
    {
        overlay.scene = &m_scene;
        overlay.structureRevision = m_scene.GetStructureRevision();
        overlay.rendererLifetime = renderer.m_sceneResourceLifetime;
    }
    else if(!overlay.Matches(m_scene, renderer.m_sceneResourceLifetime))
    {
        error = "Camera frusta cannot join an overlay captured for another scene.";
        return false;
    }

    const auto& objects = m_scene.GetRenderObjects();
    const auto& mapping = m_objectMapping.idToRuntimeIndex;
    std::vector<ColliderOverlayMarker> frusta;
    // Selected frusta (any style) draw last, so with equal depth they stay on top of coincident ones.
    std::vector<ColliderOverlayMarker> selectedFrusta;

    // Document order is display order only; identity is the stable object ID mapping.
    for(const auto& object : m_fileScene.GetDocument().objects)
    {
        if(!object.camera)
        {
            continue;
        }

        const auto found = mapping.find(object.id);

        if(found == mapping.end() || found->second >= objects.size())
        {
            rejected.push_back(object.name);
            continue;
        }

        Matrix4 model;
        const Matrix4 world = objects[found->second].GetPresentationTransform().currentModel;

        if(!BuildCameraFrustumOverlayModel(world, *object.camera, viewportWidth, viewportHeight, model))
        {
            rejected.push_back(object.name);
            continue;
        }

        const bool selected = std::find(selectedRuntimeIndices.begin(), selectedRuntimeIndices.end(),
            found->second) != selectedRuntimeIndices.end();
        auto role = selected ? ColliderOverlayRole::CameraFrustumSelected : ColliderOverlayRole::CameraFrustum;

        if(!object.camera->enabled)
        {
            role = ColliderOverlayRole::CameraFrustumDisabled;
        }

        (selected ? selectedFrusta : frusta).push_back({model, ColliderOverlayShape::Box, role, 0.0f});
    }

    frusta.insert(frusta.end(), selectedFrusta.begin(), selectedFrusta.end());

    if(frusta.size() > EditorColliderOverlay::MaxColliders - (std::min)(overlay.GetColliderCount(),
        EditorColliderOverlay::MaxColliders))
    {
        error = "Camera frusta exceed the Editor overlay capacity.";
        return false;
    }

    overlay.cameraFrusta = std::move(frusta);
    return true;
}
