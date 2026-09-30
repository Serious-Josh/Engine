#pragma once

#include "Camera.h"
#include "DirectionalLight.h"
#include "Renderer/RenderObject.h"

#include <vector>

class Scene
{
    public:
        Scene() = default;

        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;

        void AddRenderObject(const RenderObject& object);

        std::vector<RenderObject>& GetRenderObjects();
        const std::vector<RenderObject>& GetRenderObjects() const;

        Camera& GetCamera();
        const Camera& GetCamera() const;

        DirectionalLight& GetDirectionalLight();
        const DirectionalLight& GetDirectionalLight() const;

    private:
        std::vector<RenderObject> m_renderObjects;

        Camera m_camera;
        DirectionalLight m_directionalLight;
};