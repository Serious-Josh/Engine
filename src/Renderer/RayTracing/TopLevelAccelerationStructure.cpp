#include "TopLevelAccelerationStructure.h"

#include "BottomLevelAccelerationStructure.h"
#include "Scene/Scene.h"
#include "Renderer/RenderObject.h"
#include "Math/Matrix4.h"

#include <cstring>
#include <vector>

bool TopLevelAccelerationStructure::Initialize(
    ID3D12Device5* device,
    ID3D12GraphicsCommandList4* commandList,
    const Scene& scene,
    const BottomLevelAccelerationStructure& blas)
{
    if(!device || !commandList || blas.GetGPUAddress() == 0){
        return false;
    }

    const std::vector<RenderObject>& renderObjects = scene.GetRenderObjects();

    if(renderObjects.empty()){
        return false;
    }

    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescriptions(
        renderObjects.size());

    for(std::size_t i = 0; i < renderObjects.size(); ++i){
        const RenderObject& object = renderObjects[i];

        D3D12_RAYTRACING_INSTANCE_DESC& instance = instanceDescriptions[i];

        const Matrix4 modelMatrix = object.transform.GetModelMatrix();

        for(std::size_t row = 0; row < 3; ++row){
            for(std::size_t column = 0; column < 4; ++column){
                instance.Transform[row][column] = modelMatrix[row][column];
            }
        }

        instance.InstanceID = static_cast<UINT>(i);
        instance.InstanceMask = 0xFF;
        instance.InstanceContributionToHitGroupIndex = 0;
        instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        instance.AccelerationStructure = blas.GetGPUAddress();
    }

    const UINT64 instanceBufferSize =
        sizeof(D3D12_RAYTRACING_INSTANCE_DESC) *
        instanceDescriptions.size();

    D3D12_HEAP_PROPERTIES uploadHeapProperties{};
    uploadHeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC instanceBufferDescription{};
    instanceBufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    instanceBufferDescription.Width = instanceBufferSize;
    instanceBufferDescription.Height = 1;
    instanceBufferDescription.DepthOrArraySize = 1;
    instanceBufferDescription.MipLevels = 1;
    instanceBufferDescription.Format = DXGI_FORMAT_UNKNOWN;
    instanceBufferDescription.SampleDesc.Count = 1;
    instanceBufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT result = device->CreateCommittedResource(
        &uploadHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &instanceBufferDescription,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_instanceBuffer));

    if(FAILED(result)){
        Shutdown();
        return false;
    }

    void* mappedData = nullptr;

    result = m_instanceBuffer->Map(0, nullptr, &mappedData);

    if(FAILED(result)){
        Shutdown();
        return false;
    }

    std::memcpy(
        mappedData,
        instanceDescriptions.data(),
        static_cast<std::size_t>(instanceBufferSize));

    m_instanceBuffer->Unmap(0, nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS buildInputs{};
    buildInputs.Type =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;

    buildInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    buildInputs.NumDescs = static_cast<UINT>(instanceDescriptions.size());
    buildInputs.InstanceDescs = m_instanceBuffer->GetGPUVirtualAddress();
    buildInputs.Flags =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo{};

    device->GetRaytracingAccelerationStructurePrebuildInfo(
        &buildInputs,
        &prebuildInfo);

    if(prebuildInfo.ResultDataMaxSizeInBytes == 0 ||
       prebuildInfo.ScratchDataSizeInBytes == 0)
    {
        Shutdown();
        return false;
    }

    D3D12_HEAP_PROPERTIES defaultHeapProperties{};
    defaultHeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

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

    result = device->CreateCommittedResource(
        &defaultHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDescription,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&m_scratchResource));

    if(FAILED(result)){
        Shutdown();
        return false;
    }

    resourceDescription.Width = prebuildInfo.ResultDataMaxSizeInBytes;

    result = device->CreateCommittedResource(
        &defaultHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDescription,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        nullptr,
        IID_PPV_ARGS(&m_resource));

    if(FAILED(result)){
        Shutdown();
        return false;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDescription{};
    buildDescription.Inputs = buildInputs;
    buildDescription.ScratchAccelerationStructureData =
        m_scratchResource->GetGPUVirtualAddress();

    buildDescription.DestAccelerationStructureData =
        m_resource->GetGPUVirtualAddress();

    commandList->BuildRaytracingAccelerationStructure(
        &buildDescription,
        0,
        nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = m_resource;

    commandList->ResourceBarrier(1, &barrier);

    return true;
}

void TopLevelAccelerationStructure::Shutdown()
{
    if(m_instanceBuffer){
        m_instanceBuffer->Release();
        m_instanceBuffer = nullptr;
    }

    if(m_scratchResource){
        m_scratchResource->Release();
        m_scratchResource = nullptr;
    }

    if(m_resource){
        m_resource->Release();
        m_resource = nullptr;
    }
}