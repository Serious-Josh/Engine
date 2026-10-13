#pragma once

#include "Scene/Camera.h"
#include "Scene/DirectionalLight.h"
#include "Renderer/RenderObject.h"

#include <cstddef>
#include <cstdint>
#include <vector>
#include <array>

using RenderObjectID = std::size_t;

class Scene
{
    public:
    Scene() = default;

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    RenderObjectID AddRenderObject(const RenderObject& object);

    void ClearRenderObjects();

    void SetRenderObjectTransform(
        RenderObjectID id,
        const Transform& transform);

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
    std::vector<RenderObject> m_renderObjects;

    std::vector<RenderObjectID> m_changedTransforms;

    std::vector<bool> m_transformDirty;

    uint64_t m_structureRevision = 0;

    Camera m_camera;

    DirectionalLight m_directionalLight;

    // Temporary camera-selected displacement LOD diagnostic.
    std::array<std::uint32_t, kDisplacementPatchCount> m_displacementPatchLODs{};
};
