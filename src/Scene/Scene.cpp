#include "Scene.h"

#include <algorithm>
#include <cmath>

RenderObjectID Scene::AddRenderObject(const RenderObject& object)
{
    const RenderObjectID id = m_renderObjects.size();

    m_renderObjects.push_back(object);
    m_renderObjects.back().previousTransform = object.transform;

    m_transformDirty.push_back(false);

    ++m_structureRevision;

    return id;
}

void Scene::ClearRenderObjects()
{
    m_renderObjects.clear();
    m_changedTransforms.clear();
    m_transformDirty.clear();

    ++m_structureRevision;
}

void Scene::ReplaceRenderObjects(std::vector<RenderObject> objects)
{
    std::vector<bool> dirty(objects.size(), false);

    for(auto& object : objects)
    {
        object.previousTransform = object.transform;
    }

    m_renderObjects.swap(objects);
    m_transformDirty.swap(dirty);
    m_changedTransforms.clear();
    ++m_structureRevision;
}

void Scene::SetRenderObjectTransform(
    RenderObjectID id,
    const Transform& transform)
{
    if (id >= m_renderObjects.size())
    {
        return;
    }

    m_renderObjects[id].transform = transform;

    if (!m_transformDirty[id])
    {
        m_transformDirty[id] = true;
        m_changedTransforms.push_back(id);
    }
}

bool Scene::SetRenderObjectTransforms(std::span<const RenderObjectTransformUpdate> updates)
{
    const auto finite = [](const Vector3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    for(size_t index = 0; index < updates.size(); ++index)
    {
        const auto& update = updates[index];
        if(update.id >= m_renderObjects.size() || !finite(update.transform.position) ||
            !finite(update.transform.rotation) || !finite(update.transform.scale))
        {
            return false;
        }
        for(size_t previous = 0; previous < index; ++previous)
        {
            if(updates[previous].id == update.id) return false;
        }
    }
    m_changedTransforms.reserve(m_changedTransforms.size() + updates.size());
    for(const auto& update : updates)
    {
        SetRenderObjectTransform(update.id, update.transform);
    }
    return true;
}

const std::vector<RenderObjectID>& Scene::GetChangedTransforms() const
{
    return m_changedTransforms;
}

void Scene::ClearChangedTransforms()
{
    for (RenderObjectID id : m_changedTransforms)
    {
        m_renderObjects[id].previousTransform = m_renderObjects[id].transform;

        m_transformDirty[id] = false;
    }

    m_changedTransforms.clear();
}

uint64_t Scene::GetStructureRevision() const
{
    return m_structureRevision;
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

bool Scene::HasRenderableMeshes() const noexcept
{
    return std::any_of(m_renderObjects.begin(), m_renderObjects.end(), [](const RenderObject& object)
    {
        return object.mesh != nullptr;
    });
}
