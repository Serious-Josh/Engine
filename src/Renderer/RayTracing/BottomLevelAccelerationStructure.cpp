#include "BottomLevelAccelerationStructure.h"

#include "Renderer/Mesh.h"

bool BottomLevelAccelerationStructure::Initialize(ID3D12Device5* device, ID3D12GraphicsCommandList4* commandList, const Mesh& mesh)
{
    if(!device || !commandList || !mesh.IsIndexed()){
        return false;
    }

    D3D12_RAYTRACING_GEOMETRY_DESC geometryDescription{};

    geometryDescription.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometryDescription.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometryDescription.Triangles.VertexBuffer.StartAddress = mesh.GetVertexBufferAddress();
    geometryDescription.Triangles.VertexBuffer.StrideInBytes = mesh.GetVertexStride();
    geometryDescription.Triangles.VertexCount = static_cast<UINT>(mesh.GetVertexCount());
    geometryDescription.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometryDescription.Triangles.IndexBuffer = mesh.GetIndexBufferAddress();
    geometryDescription.Triangles.IndexCount = static_cast<UINT>(mesh.GetIndexCount());
    geometryDescription.Triangles.IndexFormat = mesh.GetIndexFormat();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS buildInputs{};
    buildInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    buildInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    buildInputs.NumDescs = 1;
    buildInputs.pGeometryDescs = &geometryDescription;
    buildInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;


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

    HRESULT result = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_scratchResource));

    if(FAILED(result)){
        Shutdown();
        return false;
    }

    resourceDescription.Width = prebuildInfo.ResultDataMaxSizeInBytes;

    result = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&m_resource));

    if(FAILED(result)){
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