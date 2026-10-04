#include "RayTracingSceneResources.h"

#include "Renderer/Mesh.h"
#include "Renderer/RayTracing/RayTracingAccelerationStructure.h"
#include "Renderer/RenderObject.h"
#include "Scene/Scene.h"

#include <vector>

bool RayTracingSceneResources::Initialize(ID3D12Device5* device, ID3D12GraphicsCommandList4* commandList, const Mesh& mesh, const Scene& scene)
{
    if(!device || !commandList){
        return false;
    }

    RayTracingBottomLevelDescription bottomLevelDescription{};

    RayTracingGeometryDescription geometryDescription{};
    geometryDescription.mesh = &mesh;
    geometryDescription.opaque = true;

    bottomLevelDescription.geometries.push_back(geometryDescription);
    bottomLevelDescription.buildPreference = RayTracingAccelerationStructureBuildPreference::FastTrace;

    if(!m_bottomLevel.Initialize(device, commandList, bottomLevelDescription)){
        return false;
    }

    RayTracingTopLevelDescription topLevelDescription{};

    const std::vector<RenderObject>& renderObjects = scene.GetRenderObjects();

    topLevelDescription.instances.reserve(renderObjects.size());

    for(unsigned int i = 0; i < renderObjects.size(); ++i){
        RayTracingInstanceDescription instanceDescription{};
        instanceDescription.transform = renderObjects[i].transform.GetModelMatrix();
        instanceDescription.instanceID = i;
        instanceDescription.mask = 0xFF;
        instanceDescription.bottomLevelIndex = 0;

        topLevelDescription.instances.push_back(instanceDescription);
    }

    topLevelDescription.buildPreference = RayTracingAccelerationStructureBuildPreference::FastTrace;

    const std::vector<const BottomLevelAccelerationStructure*> bottomLevelStructures = {&m_bottomLevel};

    if(!m_topLevel.Initialize(device, commandList, topLevelDescription, bottomLevelStructures)){
        return false;
    }

    return true;
}

void RayTracingSceneResources::Shutdown()
{
    m_topLevel.Shutdown();
    m_bottomLevel.Shutdown();
}

const TopLevelAccelerationStructure& RayTracingSceneResources::GetTopLevel() const
{
    return m_topLevel;
}