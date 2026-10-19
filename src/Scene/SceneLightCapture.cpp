#include "Scene/FileSceneRuntime.h"
#include "Scene/SceneLighting.h"
#include "Renderer/Renderer.h"

#include <algorithm>
#include <thread>

bool FileSceneRuntime::CaptureSceneLighting(SceneLightingSnapshot& snapshot, std::string& error) const
{
    error.clear();

    if(std::this_thread::get_id() != m_ownerThread)
    {
        error = "Scene lighting must be captured on the scene owner thread.";
        return false;
    }

    const auto& objects = m_scene.GetRenderObjects();
    const auto& mapping = m_objectMapping.idToRuntimeIndex;

    // The accepted presentation world covers the hierarchy and accepted physics poses, and is
    // the same world the frame's meshes render with.
    const auto worldOf = [&](const SceneObjectDocument& object) -> std::optional<Matrix4>
    {
        const auto found = mapping.find(object.id);

        if(found == mapping.end() || found->second >= objects.size())
        {
            return std::nullopt;
        }

        return objects[found->second].GetPresentationTransform().currentModel;
    };

    std::string buildError;

    if(!BuildSceneLightingSnapshot(m_fileScene.GetDocument(), worldOf, snapshot, &buildError))
    {
        error = buildError;
        return false;
    }

    return true;
}

bool FileSceneRuntime::CaptureEditorLightWireframes(const Renderer& renderer, EditorColliderOverlay& overlay,
    std::span<const size_t> selectedRuntimeIndices, std::vector<std::string>& rejected, std::string& error) const
{
    rejected.clear();
    error.clear();

    // Editor-only, owner-thread, current-renderer capture, as for collider overlays and frusta.
    const bool ownsActiveRendererScene = renderer.m_sceneResourceScene == &m_scene &&
        renderer.m_sceneResourceLifetime != 0;

    if(m_archiveScene || std::this_thread::get_id() != m_ownerThread || !ownsActiveRendererScene)
    {
        error = "Light wireframes require the active editable scene.";
        return false;
    }

    if(overlay.scene != nullptr && !overlay.Matches(m_scene, renderer.m_sceneResourceLifetime))
    {
        error = "Light wireframes cannot join an overlay captured for another scene.";
        return false;
    }

    const auto& objects = m_scene.GetRenderObjects();
    const auto& mapping = m_objectMapping.idToRuntimeIndex;
    std::vector<ColliderOverlayMarker> wireframes;
    // Selected wireframes (any style) draw last, so with equal depth they stay on top.
    std::vector<ColliderOverlayMarker> selectedWireframes;

    for(const auto& object : m_fileScene.GetDocument().objects)
    {
        if(!object.light)
        {
            continue;
        }

        const LightComponent& light = *object.light;
        const auto found = mapping.find(object.id);

        if(found == mapping.end() || found->second >= objects.size() || !ValidateLightComponent(light))
        {
            rejected.push_back(object.name);
            continue;
        }

        const Matrix4 world = objects[found->second].GetPresentationTransform().currentModel;
        const bool selected = std::find(selectedRuntimeIndices.begin(), selectedRuntimeIndices.end(),
            found->second) != selectedRuntimeIndices.end();
        auto role = selected ? ColliderOverlayRole::LightSelected : ColliderOverlayRole::Light;

        if(!light.enabled)
        {
            role = ColliderOverlayRole::LightDisabled;
        }

        auto& target = selected ? selectedWireframes : wireframes;

        // The same placement rules as lighting, so a Light that cannot illuminate never draws.
        Vector3 position;
        Vector3 direction;

        if(!ResolveLightWorldPlacement(world, light.type, position, direction))
        {
            rejected.push_back(object.name);
            continue;
        }

        if(light.type == LightType::Point)
        {
            target.push_back({BuildPointLightWireframeModel(position, light.range), ColliderOverlayShape::Sphere, role, 0.0f});
            continue;
        }

        Matrix4 rigidWorld;

        if(!BuildLightRigidWorld(world, rigidWorld))
        {
            rejected.push_back(object.name);
            continue;
        }

        if(light.type == LightType::Directional)
        {
            target.push_back({BuildDirectionalLightArrowsModel(rigidWorld), ColliderOverlayShape::Arrows, role, 0.0f});
            continue;
        }

        target.push_back({BuildSpotLightConeModel(rigidWorld, light.range, light.outerConeAngle),
            ColliderOverlayShape::Cone, role, 0.0f});

        // The inner cone is drawn only when it is a distinct, visible cone inside the outer one.
        if(light.innerConeAngle > 0.0f && light.innerConeAngle < light.outerConeAngle)
        {
            target.push_back({BuildSpotLightConeModel(rigidWorld, light.range, light.innerConeAngle),
                ColliderOverlayShape::Cone, role, 0.0f});
        }
    }

    wireframes.insert(wireframes.end(), selectedWireframes.begin(), selectedWireframes.end());

    // A scene without drawable Lights never creates an overlay of its own.
    if(wireframes.empty())
    {
        return true;
    }

    if(wireframes.size() > EditorColliderOverlay::MaxColliders - (std::min)(overlay.GetColliderCount(),
        EditorColliderOverlay::MaxColliders))
    {
        error = "Light wireframes exceed the Editor overlay capacity.";
        return false;
    }

    if(overlay.scene == nullptr)
    {
        overlay.scene = &m_scene;
        overlay.structureRevision = m_scene.GetStructureRevision();
        overlay.rendererLifetime = renderer.m_sceneResourceLifetime;
    }

    overlay.lightWireframes = std::move(wireframes);
    return true;
}
