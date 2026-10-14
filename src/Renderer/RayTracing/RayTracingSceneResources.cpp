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
#include <atomic>
#include <algorithm>
#include <limits>
#include "Core/Log.h"

namespace
{
    std::uint64_t AllocateSceneRetirementLifetime()
    {
        static std::atomic<std::uint64_t> next{1};
        auto value = next.load(std::memory_order_relaxed);
        while (value != UINT64_MAX)
        {
            if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            {
                return value;
            }
        }

        return 0;
    }
} // namespace

bool RayTracingSceneResources::Initialize(
    ID3D12Device5* device,
    ID3D12GraphicsCommandList4* commandList,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const Scene& scene)
{
    m_retirementLifetime = 0;

    if (!device || !commandList)
    {
        return false;
    }

    const auto lifetime = AllocateSceneRetirementLifetime();

    if (lifetime == 0)
    {
        return false;
    }

    const std::vector<RenderObject>& renderObjects = scene.GetRenderObjects();

    for (const RenderObject& renderObject : renderObjects)
    {
        if (!renderObject.mesh)
        {
            continue;
        }

        if (m_meshToBottomLevel.contains(renderObject.mesh))
        {
            continue;
        }

        RayTracingBottomLevelDescription bottomLevelDescription{};

        RayTracingGeometryDescription geometryDescription{};
        geometryDescription.mesh = renderObject.mesh;
        geometryDescription.opaque = true;

        bottomLevelDescription.geometries.push_back(geometryDescription);
        bottomLevelDescription.buildPreference = RayTracingAccelerationStructureBuildPreference::FastTrace;

        auto bottomLevel = std::make_unique<BottomLevelAccelerationStructure>();

        if (!bottomLevel->Initialize(device, commandList, bottomLevelDescription))
        {
            return false;
        }

        const std::size_t bottomLevelIndex = m_bottomLevels.size();

        m_meshToBottomLevel.emplace(renderObject.mesh, bottomLevelIndex);

        m_bottomLevels.push_back(std::move(bottomLevel));
    }

    if (m_bottomLevels.empty())
    {
        return false;
    }

    m_instances.clear();

    m_instances.reserve(renderObjects.size());

    m_renderObjectToInstance.clear();

    m_renderObjectToInstance.resize(renderObjects.size(), InvalidInstanceIndex);

    std::vector<RayTracingInstanceData> instanceData(renderObjects.size());

    if(!InitializeMaterialSlots(device, uploader, descriptorHeap, scene, instanceData))
    {
        return false;
    }

    for (unsigned int i = 0; i < renderObjects.size(); ++i)
    {
        const RenderObject& renderObject = renderObjects[i];

        if (!renderObject.mesh)
        {
            continue;
        }

        const auto bottomLevelIt = m_meshToBottomLevel.find(renderObject.mesh);

        if (bottomLevelIt == m_meshToBottomLevel.end())
        {
            return false;
        }

        if (!renderObject.material)
        {
            return false;
        }

        unsigned int normalOffset = UINT_MAX;

        unsigned int uvOffset = UINT_MAX;
        unsigned int tangentOffset = UINT_MAX;

        for (const VertexAttribute& attribute : renderObject.mesh->GetVertexLayout().GetAttributes())
        {
            if (std::strcmp(attribute.semantic, "NORMAL") == 0 && attribute.semanticIndex == 0 &&

                attribute.type == VertexAttributeType::Float3)
            {
                normalOffset = attribute.offset;
            }

            if (std::strcmp(attribute.semantic, "TEXCOORD") == 0 && attribute.semanticIndex == 0 &&

                attribute.type == VertexAttributeType::Float2)
            {
                uvOffset = attribute.offset;
            }

            if (std::strcmp(attribute.semantic, "TANGENT") == 0 && attribute.semanticIndex == 0 &&

                attribute.type == VertexAttributeType::Float4)
            {
                tangentOffset = attribute.offset;
            }
        }

        if (normalOffset == UINT_MAX || uvOffset == UINT_MAX)
        {
            return false;
        }

        RayTracingInstanceData gpuInstance = instanceData[i];

        gpuInstance.vertexBufferIndex = renderObject.mesh->GetVertexBufferDescriptorIndex();
        gpuInstance.indexBufferIndex =

            renderObject.mesh->IsIndexed() ? renderObject.mesh->GetIndexBufferDescriptorIndex() : UINT_MAX;
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

    if (!m_instanceDataBuffer.Initialize(device, uploader, instanceData.data(), sizeof(RayTracingInstanceData),
                                         static_cast<unsigned int>(instanceData.size()), descriptorHeap))
    {
        return false;
    }

    RayTracingTopLevelDescription topLevelDescription{};
    topLevelDescription.instances = m_instances;
    topLevelDescription.buildPreference = RayTracingAccelerationStructureBuildPreference::FastTrace;

    m_bottomLevelPointers.clear();
    m_bottomLevelPointers.reserve(m_bottomLevels.size());

    for (const auto& bottomLevel : m_bottomLevels)
    {
        m_bottomLevelPointers.push_back(bottomLevel.get());
    }

    if (!m_topLevel.Initialize(device, commandList, topLevelDescription, m_bottomLevelPointers))
    {
        return false;
    }

    m_retirementLifetime = lifetime;
    return true;
}

bool RayTracingSceneResources::Update(
    ID3D12GraphicsCommandList4* commandList,
    unsigned int frameIndex,
    const Scene& scene)
{
    if (!commandList)
    {
        return false;
    }

    const std::vector<RenderObject>& renderObjects = scene.GetRenderObjects();

    const std::vector<RenderObjectID>& changedTransforms = scene.GetChangedTransforms();

    if (renderObjects.size() != m_renderObjectToInstance.size())
    {
        return false;
    }

    std::vector<std::size_t> changedInstanceIndices;
    changedInstanceIndices.reserve(changedTransforms.size());

    for (RenderObjectID renderObjectID : changedTransforms)
    {
        if (renderObjectID >= renderObjects.size())
        {
            return false;
        }

        const std::size_t instanceIndex = m_renderObjectToInstance[renderObjectID];

        if (instanceIndex == InvalidInstanceIndex)
        {
            continue;
        }

        if (instanceIndex >= m_instances.size())
        {
            return false;
        }

        m_instances[instanceIndex].transform = renderObjects[renderObjectID].transform.GetModelMatrix();

        changedInstanceIndices.push_back(instanceIndex);
    }

    return m_topLevel.Update(commandList, frameIndex, m_instances, changedInstanceIndices, m_bottomLevelPointers);
}

bool RayTracingSceneResources::UpdateTransformsAndRefreshTLASReferences(
    ID3D12GraphicsCommandList4* commandList,
    unsigned int frameIndex,
    const Scene& scene)
{
    if (!commandList || m_instances.empty() || m_bottomLevelPointers.empty())
    {
        return false;
    }

    const auto& objects = scene.GetRenderObjects();

    if (objects.size() != m_renderObjectToInstance.size())
    {
        return false;
    }

    for (RenderObjectID id : scene.GetChangedTransforms())
    {
        if (id >= objects.size())
        {

            return false;
        }
    }

    // Read all transforms, including instances not in the dirty list. Validate
    // registration before publishing any scene-level instance changes.

    auto instances = m_instances;

    std::vector<std::size_t> allInstanceIndices;
    allInstanceIndices.reserve(instances.size());

    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
    {
        const RenderObject& object = objects[objectIndex];
        const std::size_t instanceIndex = m_renderObjectToInstance[objectIndex];

        if (instanceIndex == InvalidInstanceIndex)
        {
            if (object.mesh)
            {
                return false; // Structural change requires scene reconstruction.
            }

            continue;
        }

        if (!object.mesh || instanceIndex >= instances.size() || instances[instanceIndex].instanceID != objectIndex)
        {
            return false;
        }

        const auto registered = m_meshToBottomLevel.find(object.mesh);

        const std::size_t bottomLevelIndex = instances[instanceIndex].bottomLevelIndex;

        if (registered == m_meshToBottomLevel.end() ||
            registered->second != bottomLevelIndex ||
            bottomLevelIndex >= m_bottomLevelPointers.size() ||
            !m_bottomLevelPointers[bottomLevelIndex])
        {
            return false;
        }

        if (m_bottomLevelPointers[bottomLevelIndex]->GetGPUAddress() == 0)
        {
            return false;
        }

        instances[instanceIndex].transform = object.transform.GetModelMatrix();
        allInstanceIndices.push_back(instanceIndex);
    }

    if (allInstanceIndices.size() != instances.size())
    {
        return false;
    }

    // TLAS Update resolves current BLAS addresses for every supplied instance,
    // propagates pending entries to both slots, and maps only the selected slot
    // once before recording its single build and existing UAV barrier.

    m_instances.swap(instances);
    return m_topLevel.Update(commandList, frameIndex, m_instances, allInstanceIndices, m_bottomLevelPointers);
}

bool RayTracingSceneResources::RebuildMeshBLAS(
    ID3D12Device5* device,
    ID3D12GraphicsCommandList4* commandList,
    const Mesh* mesh)
{
    return RebuildMeshBLASInternal(device, commandList, mesh, std::nullopt);
}

bool RayTracingSceneResources::RebuildMeshBLAS(
    ID3D12Device5* device,
    ID3D12GraphicsCommandList4* commandList,
    const Mesh* mesh,
    const RayTracingGeometryCounts& counts)
{
    return RebuildMeshBLASInternal(device, commandList, mesh, counts);
}

bool RayTracingSceneResources::RebuildMeshBLASInternal(
    ID3D12Device5* device,
    ID3D12GraphicsCommandList4* commandList,
    const Mesh* mesh,
    const std::optional<RayTracingGeometryCounts>& countOverride)
{
    if (!device || !commandList || !mesh)
    {
        return false;
    }

    const auto it = m_meshToBottomLevel.find(mesh);

    if (it == m_meshToBottomLevel.end())
    {
        return false;
    }

    const std::size_t bottomLevelIndex = it->second;

    if (bottomLevelIndex >= m_bottomLevels.size() || !m_bottomLevels[bottomLevelIndex])
    {
        return false;
    }

    RayTracingBottomLevelDescription description{};

    RayTracingGeometryDescription geometry{};
    geometry.mesh = mesh;
    geometry.opaque = true;
    geometry.countOverride = countOverride;

    description.geometries.push_back(geometry);

    description.buildPreference = RayTracingAccelerationStructureBuildPreference::FastTrace;

    return m_bottomLevels[bottomLevelIndex]->Rebuild(device, commandList, description);
}

bool RayTracingSceneResources::RefreshTLASReferences(
    ID3D12GraphicsCommandList4* commandList,
    unsigned int frameIndex)
{
    if (!commandList || m_instances.empty() || m_bottomLevelPointers.empty())
    {
        return false;
    }

    return m_topLevel.RefreshBLASReferences(commandList, frameIndex, m_instances, m_bottomLevelPointers);
}

void RayTracingSceneResources::ReleaseRetiredBLASResources()
{
    for (const auto& bottomLevel : m_bottomLevels)
    {
        if (bottomLevel)
        {
            bottomLevel->ReleaseRetiredResources();
        }
    }
}

SceneBLASRetirementBatchIdentity RayTracingSceneResources::GetExclusiveRetirementBatch(const Mesh* mesh) const
{
    if (!mesh || m_retirementLifetime == 0)
    {
        return {};
    }

    const auto registered = m_meshToBottomLevel.find(mesh);

    if (registered == m_meshToBottomLevel.end() || registered->second >= m_bottomLevels.size())
    {
        return {};
    }

    const auto* expected = m_bottomLevels[registered->second].get();

    if (!expected)
    {
        return {};
    }

    for (const auto& bottomLevel : m_bottomLevels)
    {
        if (!bottomLevel || (bottomLevel.get() != expected && bottomLevel->GetRetirementBatchCount() != 0))
        {
            return {};
        }
    }

    const auto batch = expected->GetExclusiveRetirementBatch();

    if (!expected->ValidateRetirementBatch(batch))
    {
        return {};
    }

    return {m_retirementLifetime, batch};
}

bool RayTracingSceneResources::ValidateRetirementBatch(

    const Mesh* mesh,
    SceneBLASRetirementBatchIdentity identity) const
{
    const auto current = GetExclusiveRetirementBatch(mesh);

    return identity.sceneLifetime != 0 && identity.sceneLifetime == current.sceneLifetime &&

           identity.blas.ownerLifetime != 0 && identity.blas.ownerLifetime == current.blas.ownerLifetime &&

           identity.blas.batchID != 0 && identity.blas.batchID == current.blas.batchID;
}

bool RayTracingSceneResources::ReleaseRetirementBatch(
    const Mesh* mesh,
    SceneBLASRetirementBatchIdentity identity,
    bool gpuCompletionConfirmed,
    bool referencesSafe)
{
    if (!gpuCompletionConfirmed || !referencesSafe || !ValidateRetirementBatch(mesh, identity))
    {
        return false;
    }

    const auto registered = m_meshToBottomLevel.find(mesh);

    return m_bottomLevels[registered->second]->ReleaseRetirementBatch(identity.blas, gpuCompletionConfirmed,
                                                                      referencesSafe);
}

bool RayTracingSceneResources::TryGetTLASGeometryEvidence(
    const Mesh* mesh,
    SceneBLASRetirementBatchIdentity batch,
    std::uint64_t geometryVersion,
    std::uint64_t recordingGeneration,
    unsigned int frameSlot,
    TLASGeometryRecordingEvidence& evidence) const
{
    if (!geometryVersion ||
        !recordingGeneration ||
        frameSlot >= 2 ||
        frameSlot != m_topLevel.GetRecordingSlot())
    {
        return false;
    }

    if (!ValidateRetirementBatch(mesh, batch))
    {
        return false;
    }

    const auto registered = m_meshToBottomLevel.find(mesh);
    TLASGeometryRecordingEvidence candidate{batch,
                                            geometryVersion,
                                            recordingGeneration,
                                            m_topLevel.GetRecordingIdentity(),
                                            m_bottomLevels[registered->second]->GetGPUAddress(),
                                            frameSlot};

    if (!ValidateTLASGeometryEvidence(mesh, candidate))
    {
        return false;
    }

    evidence = candidate;
    return true;
}

bool RayTracingSceneResources::ValidateTLASGeometryEvidence(
    const Mesh* mesh,
    const TLASGeometryRecordingEvidence& evidence) const
{
    if (!evidence.geometryVersion ||
        !evidence.recordingGeneration ||
        evidence.frameSlot >= 2 ||
        evidence.frameSlot != m_topLevel.GetRecordingSlot())
    {
        return false;
    }

    if (!ValidateRetirementBatch(mesh, evidence.retirement))
    {
        return false;
    }

    const auto registered = m_meshToBottomLevel.find(mesh);

    if (evidence.blasAddress == 0 || evidence.blasAddress != m_bottomLevels[registered->second]->GetGPUAddress())
    {
        return false;
    }

    bool found = false;

    for (size_t instanceIndex = 0; instanceIndex < m_instances.size(); ++instanceIndex)
    {
        if (m_instances[instanceIndex].bottomLevelIndex == registered->second)
        {
            found = true;

            if (!m_topLevel.RecordedInstanceUsesBLAS(evidence.tlasRecording, instanceIndex, evidence.blasAddress))
            {
                return false;
            }
        }
    }

    return found;
}

void RayTracingSceneResources::Shutdown()
{
    m_retirementLifetime = 0;
    m_topLevel.Shutdown();

    for (auto& bottomLevel : m_bottomLevels)
    {
        bottomLevel->Shutdown();
    }

    m_instanceDataBuffer.Shutdown();
    m_triangleSlots.Shutdown();
    m_slotMaterials.Shutdown();
    m_slotGeometry.clear();
    m_materialBindings.clear();
    m_slotMaterialIndices.clear();
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

bool RayTracingSceneResources::InitializeMaterialSlots(ID3D12Device5* device, ResourceUploader& uploader,
    DescriptorHeap& heap, const Scene& scene, std::vector<RayTracingInstanceData>& instances)
{
    const auto& objects = scene.GetRenderObjects();
    std::vector<unsigned int> triangles{0};
    std::vector<unsigned int> materials;
    std::unordered_map<const Mesh*, unsigned int> triangleOffsets;
    m_materialBindings = objects;
    m_slotMaterialIndices.resize(objects.size());

    for(size_t i = 0; i < objects.size(); ++i)
    {
        const auto& object = objects[i];

        if(!object.mesh)
        {
            continue;
        }

        const auto& metadata = object.mesh->GetMetadata();
        const size_t indexCount = object.mesh->IsIndexed() ? object.mesh->GetIndexCount() : object.mesh->GetVertexCount();
        std::string error;

        if(!ValidateMeshMetadata(metadata, indexCount, error))
        {
            Log::Error("Invalid DXR mesh slot metadata: " + error);
            return false;
        }

        if(metadata.materialSlots.size() > UINT_MAX - materials.size())
        {
            Log::Error("DXR material-slot table exceeds 32-bit addressing.");
            return false;
        }

        auto& instance = instances[i];
        instance.slotMaterialOffset = static_cast<unsigned int>(materials.size());
        instance.triangleSlotBufferIndex = UINT_MAX;
        m_slotGeometry.emplace(object.mesh, metadata);

        for(const auto& slot : metadata.materialSlots)
        {
            const auto* material = object.GetSlotMaterial(slot.id);

            if(!material || !material->pipeline || !material->texture)
            {
                Log::Error("DXR object has an unresolved material slot: " + slot.id);
                return false;
            }

            materials.push_back(material->gpuIndex);
            m_slotMaterialIndices[i].push_back(material->gpuIndex);
        }

        instance.materialIndex = materials[instance.slotMaterialOffset];

        if(metadata.materialSlots.size() == 1)
        {
            continue; // Dynamic single-slot geometry needs no primitive lookup.
        }

        auto offset = triangleOffsets.find(object.mesh);

        if(offset == triangleOffsets.end())
        {
            const size_t triangleCount = indexCount / 3;

            if(triangleCount > UINT_MAX - triangles.size())
            {
                Log::Error("DXR triangle-slot table exceeds 32-bit addressing.");
                return false;
            }

            const auto firstTriangle = static_cast<unsigned int>(triangles.size());
            triangles.resize(triangles.size() + triangleCount);

            for(const auto& submesh : metadata.submeshes)
            {
                const auto begin = triangles.begin() + firstTriangle + submesh.firstIndex / 3;
                std::fill_n(begin, submesh.indexCount / 3, submesh.materialSlot);
            }

            offset = triangleOffsets.emplace(object.mesh, firstTriangle).first;
        }

        instance.triangleSlotOffset = offset->second;
        instance.triangleSlotBufferIndex = 0; // Patched after successful GPU allocation.
    }

    if(materials.empty() ||
        !m_triangleSlots.Initialize(device, uploader, triangles.data(), sizeof(unsigned int),
            static_cast<unsigned int>(triangles.size()), heap) ||
        !m_slotMaterials.Initialize(device, uploader, materials.data(), sizeof(unsigned int),
            static_cast<unsigned int>(materials.size()), heap))
    {
        Log::Error("Failed to allocate DXR material-slot lookup buffers.");
        return false;
    }

    for(auto& instance : instances)
    {
        instance.slotMaterialBufferIndex = m_slotMaterials.GetDescriptorIndex();

        if(instance.triangleSlotBufferIndex != UINT_MAX)
        {
            instance.triangleSlotBufferIndex = m_triangleSlots.GetDescriptorIndex();
        }
    }

    return true;
}

bool RayTracingSceneResources::ValidateMaterialBindings(const Scene& scene) const
{
    const auto& objects = scene.GetRenderObjects();

    if(!scene.HasRenderableMeshes() && m_instances.empty())
    {
        return true;
    }

    if(objects.size() != m_materialBindings.size())
    {
        return false;
    }

    for(size_t i = 0; i < objects.size(); ++i)
    {
        const auto& object = objects[i];
        const auto& previous = m_materialBindings[i];

        if(object.mesh != previous.mesh || object.material != previous.material ||
            object.slotMaterials != previous.slotMaterials)
        {
            return false;
        }

        if(!object.mesh)
        {
            continue;
        }

        const auto& current = object.mesh->GetMetadata();
        const auto& original = m_slotGeometry.at(object.mesh);

        if(current.materialSlots != original.materialSlots ||
            (current.materialSlots.size() > 1 && current.submeshes != original.submeshes))
        {
            return false;
        }

        if(current.materialSlots.size() == 1)
        {
            const auto count = object.mesh->IsIndexed() ? object.mesh->GetIndexCount() : object.mesh->GetVertexCount();

            if(current.submeshes.size() != 1 || current.submeshes.front().firstIndex != 0 ||
                current.submeshes.front().materialSlot != 0 || current.submeshes.front().indexCount != count ||
                count == 0 || count % 3 != 0)
            {
                return false;
            }
        }

        for(size_t slot = 0; slot < current.materialSlots.size(); ++slot)
        {
            const auto* material = object.GetSlotMaterial(current.materialSlots[slot].id);

            if(!material || material->gpuIndex != m_slotMaterialIndices[i][slot])
            {
                return false;
            }
        }
    }

    return true;
}
