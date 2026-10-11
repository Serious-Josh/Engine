#include "BottomLevelAccelerationStructure.h"
#include "Core/Log.h"

#include "Renderer/Mesh.h"

#include <string>
#include <vector>
#include <utility>

bool BottomLevelAccelerationStructure::Initialize(ID3D12Device5* device, ID3D12GraphicsCommandList4* commandList, const RayTracingBottomLevelDescription& description)
{
    if(!device || !commandList || description.geometries.empty()){
        return false;
    }

    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescriptions;
    geometryDescriptions.reserve(description.geometries.size());

    for(const RayTracingGeometryDescription& geometry : description.geometries){
        if(!geometry.mesh || !geometry.mesh->IsIndexed()){
            return false;
        }

        const Mesh& mesh = *geometry.mesh;

        D3D12_RAYTRACING_GEOMETRY_DESC geometryDescription{};
        geometryDescription.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometryDescription.Flags = geometry.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        geometryDescription.Triangles.VertexBuffer.StartAddress = mesh.GetVertexBufferAddress();
        geometryDescription.Triangles.VertexBuffer.StrideInBytes = mesh.GetVertexStride();
        geometryDescription.Triangles.VertexCount = static_cast<UINT>(mesh.GetVertexCount());
        geometryDescription.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometryDescription.Triangles.IndexBuffer = mesh.GetIndexBufferAddress();
        geometryDescription.Triangles.IndexCount = static_cast<UINT>(mesh.GetIndexCount());
        geometryDescription.Triangles.IndexFormat = mesh.GetIndexFormat();
        geometryDescriptions.push_back(geometryDescription);
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS buildInputs;
    buildInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    buildInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    buildInputs.NumDescs = static_cast<UINT>(geometryDescriptions.size());
    buildInputs.pGeometryDescs = geometryDescriptions.data();
    buildInputs.Flags = description.buildPreference == RayTracingAccelerationStructureBuildPreference::FastBuild ?
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD
        : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;


    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo{};

    device->GetRaytracingAccelerationStructurePrebuildInfo(&buildInputs, &prebuildInfo);

    if(prebuildInfo.ResultDataMaxSizeInBytes == 0 || prebuildInfo.ScratchDataSizeInBytes == 0){
        return false;
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resourceDescription{};
    resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDescription.Height = 1;
    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;
    resourceDescription.Format = DXGI_FORMAT_UNKNOWN;
    resourceDescription.SampleDesc.Count = 1;
    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    resourceDescription.Width = prebuildInfo.ScratchDataSizeInBytes;

    HRESULT result = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_scratchResource));

    if(FAILED(result)){
        Log::Error("Failed to allocate BLAS scratch resource. HRESULT: " + std::to_string(result));
        Shutdown();
        return false;
    }

    resourceDescription.Width = prebuildInfo.ResultDataMaxSizeInBytes;

    result = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&m_resource));

    if(FAILED(result)){
        Log::Error("Failed to allocate BLAS result resource. HRESULT: " + std::to_string(result));
        Shutdown();
        return false;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDescription{};
    buildDescription.Inputs = buildInputs;
    buildDescription.ScratchAccelerationStructureData = m_scratchResource->GetGPUVirtualAddress();
    buildDescription.DestAccelerationStructureData = m_resource->GetGPUVirtualAddress();

    commandList->BuildRaytracingAccelerationStructure(&buildDescription, 0, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = m_resource;

    commandList->ResourceBarrier(1, &barrier);

    return true;
}

bool BottomLevelAccelerationStructure::Rebuild(
    ID3D12Device5* device,
    ID3D12GraphicsCommandList4* commandList,
    const RayTracingBottomLevelDescription& description)
{
    if(!device || !commandList || !m_resource)
    {
        return false;
    }

    BottomLevelAccelerationStructure replacement;

    if(!replacement.Initialize(device, commandList, description))
    {
        return false;
    }

    std::swap(m_resource, replacement.m_resource);
    std::swap(m_scratchResource, replacement.m_scratchResource);

    replacement.Shutdown();

    return true;
}

void BottomLevelAccelerationStructure::Shutdown()
{
    if(m_scratchResource){
        m_scratchResource->Release();
        m_scratchResource = nullptr;
    }

    if(m_resource){
        m_resource->Release();
        m_resource = nullptr;
    }
}

D3D12_GPU_VIRTUAL_ADDRESS BottomLevelAccelerationStructure::GetGPUAddress() const
{
    if(!m_resource){
        return 0;
    }

    return m_resource->GetGPUVirtualAddress();
}