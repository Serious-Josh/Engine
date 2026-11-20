#include "Scene/FileSceneRuntime.h"
#include "Scene/SceneLighting.h"
#include "Renderer/Renderer.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <thread>

static_assert(EditorColliderOverlay::MaxColliders == 2 * MaximumSceneObjects,
    "The Editor overlay holds two optional colliders per object at the scene bound");

std::shared_ptr<const SceneLightingSnapshot> FileSceneRuntime::CaptureSceneLightingShared(std::string& error) const
{
    error.clear();

    if(std::this_thread::get_id() != m_ownerThread)
    {
        error = "Scene lighting must be captured on the scene owner thread.";
        return nullptr;
    }

    constexpr RenderObjectID Unmapped = (std::numeric_limits<RenderObjectID>::max)();
    const auto& objects = m_scene.GetRenderObjects();
    const auto& mapping = m_objectMapping.idToRuntimeIndex;
    const SceneDocument& document = m_fileScene.GetDocument();
    const MetadataValidationPass::Fingerprint storage = CaptureMetadataFingerprint();
    const std::uint64_t editRevision = m_fileScene.GetEditRevision();
    const std::uint64_t hierarchyRevision = m_scene.GetHierarchyRevision();
    const SceneLightingSettings& lightingSettings = m_scene.GetLightingSettings();

    // Reuse: the same document storage and content, hierarchy and settings, and every Light presented exactly where the
    // snapshot resolved it (physics poses, gameplay locals and hierarchy propagation all reach the presentation world).
    if(m_lightingCache && m_lightingCache->storage == storage && m_lightingCache->editRevision == editRevision &&
        m_lightingCache->hierarchyRevision == hierarchyRevision && m_lightingCache->lightingSettings == lightingSettings)
    {
        bool unchanged = true;

        for(const auto& [slot, world] : m_lightingCache->lightWorlds)
        {
            if(slot == Unmapped)
            {
                continue;
            }

            if(slot >= objects.size())
            {
                unchanged = false;
                break;
            }

            const Matrix4 current = objects[slot].GetPresentationTransform().currentModel;

            if(std::memcmp(&current, &world, sizeof(Matrix4)) != 0)
            {
                unchanged = false;
                break;
            }
        }

        if(unchanged)
        {
            return m_lightingCache->snapshot;
        }
    }

    auto snapshot = std::make_shared<SceneLightingSnapshot>();

    if(!CaptureSceneLighting(*snapshot, error))
    {
        m_lightingCache.reset();
        return nullptr;
    }

    static std::atomic<std::uint64_t> nextRevision{0};
    snapshot->revision = ++nextRevision;

    LightingCache cache;
    cache.storage = storage;
    cache.editRevision = editRevision;
    cache.hierarchyRevision = hierarchyRevision;
    cache.lightingSettings = lightingSettings;
    cache.snapshot = snapshot;

    for(const auto& object : document.objects)
    {
        if(!object.light)
        {
            continue;
        }

        const auto found = mapping.find(object.id);

        if(found == mapping.end() || found->second >= objects.size())
        {
            cache.lightWorlds.emplace_back(Unmapped, Matrix4::Identity());
            continue;
        }

        cache.lightWorlds.emplace_back(found->second, objects[found->second].GetPresentationTransform().currentModel);
    }

    m_lightingCache = std::move(cache);
    return snapshot;
}

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

    snapshot.lightingSettings = m_scene.GetLightingSettings();
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
    const auto& documentObjects = m_fileScene.GetDocument().objects;
    std::vector<ColliderOverlayMarker> wireframes;
    // Selected wireframes (any style) draw last, so with equal depth they stay on top.
    std::vector<ColliderOverlayMarker> selectedWireframes;
    const auto isSelected = [&](RenderObjectID slot)
    {
        return std::find(selectedRuntimeIndices.begin(), selectedRuntimeIndices.end(), slot) != selectedRuntimeIndices.end();
    };

    // M3.5: with more Lights than LightWireframeBudget, only the selected Lights and the nearest LightWireframeBudget others
    // (to the viewport camera of the last recorded frame) are drawn, rather than tens of thousands of overlays nobody can
    // read. A cheap first pass ranks Lights by distance; the loop below then builds wireframes for the chosen ones only.
    // Below the budget every Light is drawn exactly as before.
    constexpr std::size_t LightWireframeBudget = 1024;
    std::vector<bool> chosen;
    std::size_t lightCount = 0;

    for(const auto& object : documentObjects)
    {
        lightCount += object.light ? 1u : 0u;
    }

    if(lightCount > LightWireframeBudget)
    {
        // The camera position of a world-to-view matrix [R | t]: -R^T t.
        const Matrix4& view = renderer.GetRecordedView();
        const Vector3 camera{
            -(view.m[0][0] * view.m[0][3] + view.m[1][0] * view.m[1][3] + view.m[2][0] * view.m[2][3]),
            -(view.m[0][1] * view.m[0][3] + view.m[1][1] * view.m[1][3] + view.m[2][1] * view.m[2][3]),
            -(view.m[0][2] * view.m[0][3] + view.m[1][2] * view.m[1][3] + view.m[2][2] * view.m[2][3])};
        std::vector<std::pair<float, std::size_t>> distances;
        distances.reserve(lightCount);

        for(std::size_t index = 0; index < documentObjects.size(); ++index)
        {
            const auto found = documentObjects[index].light ? mapping.find(documentObjects[index].id) : mapping.end();

            if(found == mapping.end() || found->second >= objects.size() || isSelected(found->second))
            {
                continue;
            }

            const Matrix4 world = objects[found->second].GetPresentationTransform().currentModel;
            const Vector3 offset{world.m[0][3] - camera.x, world.m[1][3] - camera.y, world.m[2][3] - camera.z};
            distances.emplace_back(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z, index);
        }

        chosen.assign(documentObjects.size(), false);
        const std::size_t kept = (std::min)(LightWireframeBudget, distances.size());
        std::nth_element(distances.begin(), distances.begin() + static_cast<std::ptrdiff_t>(kept), distances.end());

        for(std::size_t index = 0; index < kept; ++index)
        {
            chosen[distances[index].second] = true;
        }
    }

    for(std::size_t index = 0; index < documentObjects.size(); ++index)
    {
        const SceneObjectDocument& object = documentObjects[index];

        if(!object.light)
        {
            continue;
        }

        const LightComponent& light = *object.light;
        const auto found = mapping.find(object.id);
        const bool selected = found != mapping.end() && isSelected(found->second);

        // Not drawn (beyond the budget and not selected): not checked either.
        if(!chosen.empty() && !chosen[index] && !selected)
        {
            continue;
        }

        if(found == mapping.end() || found->second >= objects.size() || !ValidateLightComponent(light))
        {
            rejected.push_back(object.name);
            continue;
        }

        const Matrix4 world = objects[found->second].GetPresentationTransform().currentModel;
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
