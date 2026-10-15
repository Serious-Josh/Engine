#pragma once

#include "Scene/Camera.h"
#include "Scene/DirectionalLight.h"
#include "Renderer/RenderObject.h"

#include <cstddef>
#include <cstdint>
#include <vector>
#include <array>
#include <span>
#include <string>

using RenderObjectID = std::size_t;

struct RenderObjectTransformUpdate
{
    RenderObjectID id;
    Transform transform;
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

    void SetRenderObjectTransform(
        RenderObjectID id,
        const Transform& transform);

    // Validate and allocate dirty-list capacity before changing any object.
    bool SetRenderObjectTransforms(std::span<const RenderObjectTransformUpdate> updates);

    const std::vector<RenderObjectID>& GetChangedTransforms() const;
    void ClearChangedTransforms();

    uint64_t GetStructureRevision() const;

    std::vector<RenderObject>& GetRenderObjects();
    const std::vector<RenderObject>& GetRenderObjects() const;

    bool HasRenderableMeshes() const noexcept;

    Camera& GetCamera();
    const Camera& GetCamera() const;

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
    std::string m_environmentReference;
    std::vector<RenderObject> m_renderObjects;

    std::vector<RenderObjectID> m_changedTransforms;

    std::vector<bool> m_transformDirty;

    uint64_t m_structureRevision = 0;

    Camera m_camera;

    DirectionalLight m_directionalLight;

    // Temporary camera-selected displacement LOD diagnostic.
    std::array<std::uint32_t, kDisplacementPatchCount> m_displacementPatchLODs{};
};
