#pragma once

#include "Scene/Camera.h"
#include "Scene/DirectionalLight.h"
#include "Scene/SceneLightingSettings.h"
#include "Renderer/RenderObject.h"

#include <cstddef>
#include <cstdint>
#include <vector>
#include <array>
#include <optional>
#include <span>
#include <string>

using RenderObjectID = std::size_t;

struct RenderObjectTransformUpdate
{
    RenderObjectID id;
    Transform transform;
};

// Runtime-slot form of a validated SceneHierarchy::Forest. Built by FileSceneRuntime from
// the object-ID mapping; slots are never document positions.
struct SceneHierarchyTopology
{
    std::vector<std::optional<RenderObjectID>> parentSlots;
    std::vector<RenderObjectID> parentBeforeChild;
    std::uint64_t hierarchyRevision = 0;
};

class Scene
{
    public:
    Scene() = default;
    const std::string& GetEnvironmentReference() const noexcept { return m_environmentReference; }
    void SetEnvironmentReference(std::string value) { m_environmentReference=std::move(value); }

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    RenderObjectID AddRenderObject(const RenderObject& object);

    void ClearRenderObjects();

    // Inactive scenes only, after checked renderer release. Allocates dirty flags
    // before mutation, then replaces CPU object storage without further allocation.
    void ReplaceRenderObjects(std::vector<RenderObject> objects);

    // Same contract plus a hierarchy topology for the new slots. The topology is validated and
    // every authored world evaluated before any state changes; rejection leaves the scene intact.
    // History initializes to the new worlds.
    bool ReplaceRenderObjects(std::vector<RenderObject> objects, SceneHierarchyTopology topology,
        std::string* error);

    void SetRenderObjectTransform(
        RenderObjectID id,
        const Transform& transform);

    // Validate and allocate dirty-list capacity before changing any object.
    bool SetRenderObjectTransforms(std::span<const RenderObjectTransformUpdate> updates);

    const std::vector<RenderObjectID>& GetChangedTransforms() const;
    void ClearChangedTransforms();

    bool HasPhysicsPresentation() const noexcept { return m_physicsPresentationActive; }
    const std::vector<RenderObjectID>& GetChangedPresentationTransforms() const noexcept
    {
        return m_changedPresentationTransforms;
    }

    uint64_t GetStructureRevision() const;

    // Revision of the installed hierarchy topology; 0 when the scene is flat.
    std::uint64_t GetHierarchyRevision() const noexcept
    {
        return m_hierarchy.hierarchyRevision;
    }

    bool HasHierarchy() const noexcept
    {
        return !m_hierarchy.parentSlots.empty();
    }

    // True when the installed hierarchy gives this slot a parent.
    bool HasHierarchyParent(RenderObjectID id) const noexcept
    {
        return id < m_hierarchy.parentSlots.size() && m_hierarchy.parentSlots[id].has_value();
    }

    std::vector<RenderObject>& GetRenderObjects();
    const std::vector<RenderObject>& GetRenderObjects() const;

    bool HasRenderableMeshes() const noexcept;

    Camera& GetCamera();
    const Camera& GetCamera() const;

    bool UsesObjectLighting() const noexcept { return !m_directionalLight.has_value(); }
    const SceneLightingSettings& GetLightingSettings() const noexcept { return m_lightingSettings; }
    void SetLightingSettings(SceneLightingSettings settings) noexcept
    {
        m_lightingSettings = settings;
        m_directionalLight.reset();
    }
    // Procedural-demo compatibility only; file scenes discard this storage on commit.
    DirectionalLight& GetDirectionalLight();
    const DirectionalLight& GetDirectionalLight() const;

    static constexpr std::size_t kDisplacementPatchCount = 64;

    void SetDisplacementPatchLODs(const std::array<std::uint32_t, kDisplacementPatchCount>& patchLODs)
    {
        m_displacementPatchLODs = patchLODs;
    }

    const std::array<std::uint32_t, kDisplacementPatchCount>& GetDisplacementPatchLODs() const
    {
        return m_displacementPatchLODs;
    }

    private:
    friend class FileSceneRuntime;
#if defined(ENGINE_HAS_VERIFICATION)
    friend class PresentationTransformVerification;
    friend class ColliderVisualizationVerification;
    friend class MarqueeSelectionVerification;
    friend class SceneHierarchyVerification;
    friend class ViewportGridSnapVerification;
#endif
    // Preparation allocates before acceptance and, for hierarchy scenes, proves that every
    // descendant world under these poses is valid. Commit only copies value types.
    bool PreparePresentationCommit(std::span<const std::optional<Physics::Pose>> poses = {});
    void CommitPresentation(std::span<const std::optional<Physics::Pose>> poses, bool active) noexcept;

    // Installs a topology after evaluating every world under it. Rejection leaves the
    // committed topology, matrices, history and dirty lists unchanged. initializeHistory
    // sets previous = current, as for a newly committed scene.
    bool InstallHierarchy(SceneHierarchyTopology topology, bool initializeHistory, std::string* error,
        std::span<const RenderObjectTransformUpdate> updates = {});

    // Detaches every parented object. Its last presented world stays as previous history
    // until the next successful render. Capacity was reserved by InstallHierarchy.
    void ClearHierarchy() noexcept;

    // Where each candidate local transform and pose comes from during evaluation.
    struct HierarchyInputs
    {
        const std::vector<const Transform*>* localOverrides = nullptr;
        std::span<const std::optional<Physics::Pose>> poses;
        bool useCommittedPoses = true;
    };

    // Checks that the order is a parent-before-child permutation of every slot.
    static bool ValidateHierarchyTopology(const SceneHierarchyTopology& topology, size_t count,
        std::vector<bool>& hasChildren, bool& hasEdges, std::string* error);

    // Computes every slot's world into `worlds` in parent-before-child order. Reads only its
    // arguments, so candidates can be evaluated before they replace committed state.
    static bool EvaluateHierarchy(std::span<const RenderObject> objects, const SceneHierarchyTopology& topology,
        const std::vector<bool>& hasChildren, const HierarchyInputs& inputs, std::vector<Matrix4>& worlds,
        std::string* error) noexcept;

    // Validates both the authored candidate and, while poses are presented, the rendered one.
    bool EvaluateHierarchyEdit(std::string* error) noexcept;

    // Copies m_hierarchyWorlds into parented objects and marks changed slots dirty.
    void CommitHierarchyWorlds(bool initializeHistory) noexcept;
    void MarkTransformDirty(RenderObjectID id) noexcept;

    SceneHierarchyTopology m_hierarchy;
    std::vector<Matrix4> m_hierarchyWorlds;
    std::vector<const Transform*> m_hierarchyLocalOverrides;
    std::vector<bool> m_hierarchyHasChildren;
    std::vector<RenderObjectID> m_changedPresentationTransforms;
    bool m_physicsPresentationActive = false;
    std::string m_environmentReference;
    std::vector<RenderObject> m_renderObjects;

    std::vector<RenderObjectID> m_changedTransforms;

    std::vector<bool> m_transformDirty;

    uint64_t m_structureRevision = 0;

    Camera m_camera;

    std::optional<DirectionalLight> m_directionalLight{DirectionalLight{}};
    SceneLightingSettings m_lightingSettings;

    // Temporary camera-selected displacement LOD diagnostic.
    std::array<std::uint32_t, kDisplacementPatchCount> m_displacementPatchLODs{};
};
