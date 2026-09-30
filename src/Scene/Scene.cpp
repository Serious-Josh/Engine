#include "Scene.h"

void Scene::AddRenderObject(const RenderObject& object)
{
    m_renderObjects.push_back(object);
}

std::vector<RenderObject>& Scene::GetRenderObjects()
{
    return m_renderObjects;
}

const std::vector<RenderObject>& Scene::GetRenderObjects() const
{
    return m_renderObjects;
}

Camera& Scene::GetCamera()
{
    return m_camera;
}

const Camera& Scene::GetCamera() const
{
    return m_camera;
}

DirectionalLight& Scene::GetDirectionalLight()
{
    return m_directionalLight;
}

const DirectionalLight& Scene::GetDirectionalLight() const
{
    return m_directionalLight;
}