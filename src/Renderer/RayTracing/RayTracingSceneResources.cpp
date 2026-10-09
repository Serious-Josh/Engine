#include "RayTracingSceneResources.h"

#include "Renderer/Mesh.h"
#include "Renderer/RayTracing/RayTracingAccelerationStructure.h"
#include "Renderer/RenderObject.h"
#include "Scene/Scene.h"
#include "Renderer/Material.h"
#include "Renderer/ShaderData.h"

#include <cstring>
#include <unordered_map>
#include <vector>

bool RayTracingSceneResources::Initialize(ID3D12Device5* device, ID3D12GraphicsCommandList4* commandList, ResourceUploader& uploader, DescriptorHeap& descriptorHeap, const Scene& scene)
{
    if(!device || !commandList){
        return false;
    }

    const std::vector<RenderObject>& renderObjects = scene.GetRenderObjects();

    for(const RenderObject& renderObject : renderObjects){
        if(!renderObject.mesh){
            continue;
        }

        if(m_meshToBottomLevel.contains(renderObject.mesh)){
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

        m_meshToBottomLevel.emplace(renderObject.mesh, bottomLevelIndex);

        m_bottomLevels.push_back(std::move(bottomLevel));
    }

    if(m_bottomLevels.empty()){
        return false;
    }

    m_instances.clear();
    m_instances.reserve(renderObjects.size());

    m_renderObjectToInstance.clear();
    m_renderObjectToInstance.resize(renderObjects.size(), InvalidInstanceIndex);

    std::vector<RayTracingInstanceData> instanceData(renderObjects.size());

    for(unsigned int i = 0; i < renderObjects.size(); ++i){
        const RenderObject& renderObject = renderObjects[i];

        if(!renderObject.mesh){
            continue;
        }

        const auto bottomLevelIt = m_meshToBottomLevel.find(renderObject.mesh);

        if(bottomLevelIt == m_meshToBottomLevel.end()){
            return false;
        }

        if(!renderObject.material){
            return false;
        }

        unsigned int normalOffset = UINT_MAX;
        unsigned int uvOffset = UINT_MAX;
        unsigned int tangentOffset = UINT_MAX;

        for(const VertexAttribute& attribute : renderObject.mesh->GetVertexLayout().GetAttributes()){
            if(std::strcmp(attribute.semantic, "NORMAL") == 0 && attribute.semanticIndex == 0 && attribute.type == VertexAttributeType::Float3){
                normalOffset = attribute.offset;
            }

            if(std::strcmp(attribute.semantic, "TEXCOORD") == 0 && attribute.semanticIndex == 0 && attribute.type == VertexAttributeType::Float2){
                uvOffset = attribute.offset;
            }

            if (std::strcmp(attribute.semantic, "TANGENT") == 0 && attribute.semanticIndex == 0 && attribute.type == VertexAttributeType::Float4)
            {
                tangentOffset = attribute.offset;
            }
        }

        if(normalOffset == UINT_MAX || uvOffset == UINT_MAX){
            return false;
        }

        RayTracingInstanceData gpuInstance{};
        gpuInstance.vertexBufferIndex = renderObject.mesh->GetVertexBufferDescriptorIndex();
        gpuInstance.indexBufferIndex = renderObject.mesh->IsIndexed() ? renderObject.mesh->GetIndexBufferDescriptorIndex() : UINT_MAX;
        gpuInstance.materialIndex = renderObject.material->gpuIndex;
        gpuInstance.vertexStride = renderObject.mesh->GetVertexStride();
        gpuInstance.normalOffset = normalOffset;
        gpuInstance.uvOffset = uvOffset;
        gpuInstance.tangentOffset = tangentOffset;
        gpuInstance.indexed = renderObject.mesh->IsIndexed() ? 1u : 0u;

        instanceData[i] = gpuInstance;

        RayTracingInstanceDescription instanceDescription{};
        instanceDescription.transform = renderObject.transform.GetModelMatrix();
        instanceDescription.instanceID = i;
        instanceDescription.mask = 0xFF;
        instanceDescription.bottomLevelIndex = bottomLevelIt->second;
        
        const std::size_t instanceIndex = m_instances.size();

        m_renderObjectToInstance[i] = instanceIndex;

        m_instances.push_back(instanceDescription);
    }

    if(!m_instanceDataBuffer.Initialize(device, uploader, instanceData.data(), sizeof(RayTracingInstanceData), static_cast<unsigned int>(instanceData.size()), descriptorHeap)){
        return false;
    }

    RayTracingTopLevelDescription topLevelDescription{};
    topLevelDescription.instances = m_instances;
    topLevelDescription.buildPreference = RayTracingAccelerationStructureBuildPreference::FastTrace;

    m_bottomLevelPointers.clear();
    m_bottomLevelPointers.reserve(m_bottomLevels.size());

    for(const auto& bottomLevel : m_bottomLevels){
        m_bottomLevelPointers.push_back(bottomLevel.get());
    }

    if(!m_topLevel.Initialize(device, commandList, topLevelDescription, m_bottomLevelPointers)){
        return false;
    }

    return true;
}

bool RayTracingSceneResources::Update(ID3D12GraphicsCommandList4* commandList, unsigned int frameIndex, const Scene& scene)
{
    if(!commandList){
        return false;
    }

    const std::vector<RenderObject>& renderObjects = scene.GetRenderObjects();
    const std::vector<RenderObjectID>& changedTransforms = scene.GetChangedTransforms();

    if(renderObjects.size() != m_renderObjectToInstance.size())
    {
        return false;
    }

    std::vector<std::size_t> changedInstanceIndices; changedInstanceIndices.reserve(changedTransforms.size());

    for(RenderObjectID renderObjectID : changedTransforms){
        if(renderObjectID >= renderObjects.size()){
            return false;
        }

        const std::size_t instanceIndex = m_renderObjectToInstance[renderObjectID];

        if(instanceIndex == InvalidInstanceIndex){
            continue;
        }

        if(instanceIndex >= m_instances.size()){
            return false;
        }

        m_instances[instanceIndex].transform = renderObjects[renderObjectID].transform.GetModelMatrix();

        changedInstanceIndices.push_back(instanceIndex);
    }

    return m_topLevel.Update(commandList, frameIndex, m_instances, changedInstanceIndices, m_bottomLevelPointers);
}

void RayTracingSceneResources::Shutdown()
{
    m_topLevel.Shutdown();
    
    for(auto& bottomLevel : m_bottomLevels){
        bottomLevel->Shutdown();
    }

    m_instanceDataBuffer.Shutdown();
    m_instances.clear();
    m_renderObjectToInstance.clear();
    m_bottomLevelPointers.clear();
    m_bottomLevels.clear();
    m_meshToBottomLevel.clear();
}

const TopLevelAccelerationStructure& RayTracingSceneResources::GetTopLevel() const
{
    return m_topLevel;
}

unsigned int RayTracingSceneResources::GetInstanceDataDescriptorIndex() const
{
    return m_instanceDataBuffer.GetDescriptorIndex();
}

std::size_t RayTracingSceneResources::GetInstanceCount() const
{
    return m_instances.size();
}