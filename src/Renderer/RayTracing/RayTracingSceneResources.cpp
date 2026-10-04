#include "RayTracingSceneResources.h"

#include "Renderer/Mesh.h"
#include "Renderer/RayTracing/RayTracingAccelerationStructure.h"
#include "Renderer/RenderObject.h"
#include "Scene/Scene.h"

#include <unordered_map>
#include <vector>

bool RayTracingSceneResources::Initialize(ID3D12Device5* device, ID3D12GraphicsCommandList4* commandList, const Scene& scene)
{
    if(!device || !commandList){
        return false;
    }

    const std::vector<RenderObject>& renderObjects = scene.GetRenderObjects();
    std::unordered_map<const Mesh*, std::size_t> meshToBottomLevel;

    for(const RenderObject& renderObject : renderObjects){
        if(!renderObject.mesh){
            continue;
        }

        if(meshToBottomLevel.contains(renderObject.mesh)){
            continue;
        }

        RayTracingBottomLevelDescription bottomLevelDescription{};

        RayTracingGeometryDescription geometryDescription{};
        geometryDescription.mesh = renderObject.mesh;
        geometryDescription.opaque = true;

        bottomLevelDescription.geometries.push_back(geometryDescription);
        bottomLevelDescription.buildPreference = RayTracingAccelerationStructureBuildPreference::FastTrace;

        auto bottomLevel = std::make_unique<BottomLevelAccelerationStructure>();

        if(!bottomLevel->Initialize(device, commandList, bottomLevelDescription)){
            return false;
        }

        const std::size_t bottomLevelIndex = m_bottomLevels.size();

        meshToBottomLevel.emplace(renderObject.mesh, bottomLevelIndex);

        m_bottomLevels.push_back(std::move(bottomLevel));
    }

    if(m_bottomLevels.empty()){
        return false;
    }

    RayTracingTopLevelDescription topLevelDescription{};
    topLevelDescription.instances.reserve(renderObjects.size());

    for(unsigned int i = 0; i < renderObjects.size(); ++i){
        const RenderObject& renderObject = renderObjects[i];

        if(!renderObject.mesh){
            continue;
        }

        const auto bottomLevelIt = meshToBottomLevel.find(renderObject.mesh);

        if(bottomLevelIt == meshToBottomLevel.end()){
            return false;
        }

        RayTracingInstanceDescription instanceDescription{};
        instanceDescription.transform = renderObject.transform.GetModelMatrix();
        instanceDescription.instanceID = i;
        instanceDescription.mask = 0xFF;
        instanceDescription.bottomLevelIndex = bottomLevelIt->second;
        
        topLevelDescription.instances.push_back(instanceDescription);
    }

    topLevelDescription.buildPreference = RayTracingAccelerationStructureBuildPreference::FastTrace;

    std::vector<const BottomLevelAccelerationStructure*> bottomLevelStructures;

    bottomLevelStructures.reserve(m_bottomLevels.size());

    for(const auto& bottomLevel : m_bottomLevels){
        bottomLevelStructures.push_back(bottomLevel.get());
    }

    if(!m_topLevel.Initialize(device, commandList, topLevelDescription, bottomLevelStructures)){
        return false;
    }

    return true;
}

void RayTracingSceneResources::Shutdown()
{
    m_topLevel.Shutdown();
    
    for(auto& bottomLevel : m_bottomLevels){
        bottomLevel->Shutdown();
    }

    m_bottomLevels.clear();
}

const TopLevelAccelerationStructure& RayTracingSceneResources::GetTopLevel() const
{
    return m_topLevel;
}