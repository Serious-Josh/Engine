#pragma once

#include "Camera.h"
#include "DirectionalLight.h"
#include "Renderer/RenderObject.h"

#include <cstddef>
#include <vector>

using RenderObjectID = std::size_t;

class Scene
{
    public:
        Scene() = default;

        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;

        RenderObjectID AddRenderObject(const RenderObject& object);

        void SetRenderObjectTransform(RenderObjectID id, const Transform& transform);

        const std::vector<RenderObjectID>& GetChangedTransforms() const;
        void ClearChangedTransforms();

        std::vector<RenderObject>& GetRenderObjects();
        const std::vector<RenderObject>& GetRenderObjects() const;

        Camera& GetCamera();
        const Camera& GetCamera() const;

        DirectionalLight& GetDirectionalLight();
        const DirectionalLight& GetDirectionalLight() const;

    private:
        std::vector<RenderObject> m_renderObjects;
        std::vector<RenderObjectID> m_changedTransforms;
        std::vector<bool> m_transformDirty;

        Camera m_camera;
        DirectionalLight m_directionalLight;
};