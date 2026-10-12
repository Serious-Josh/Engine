#include "TopLevelAccelerationStructure.h"
#include "Core/Log.h"
#include "BottomLevelAccelerationStructure.h"
#include "Math/Matrix4.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    uint64_t AllocateTLASRecordingIdentity()
    {
        static std::atomic<uint64_t> next{1};
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

bool TopLevelAccelerationStructure::Initialize(
    ID3D12Device5* device,
    ID3D12GraphicsCommandList4* commandList,
    const RayTracingTopLevelDescription& description,
    const std::vector<const BottomLevelAccelerationStructure*>& bottomLevelStructures)
{
    m_recordingIdentity = 0;
    m_recordingSlot = UINT_MAX;

    if (!device || !commandList || description.instances.empty() || bottomLevelStructures.empty())
    {
        return false;
    }

    m_instanceDescriptions.clear();

    m_instanceDescriptions.resize(description.instances.size());

    for (std::size_t i = 0; i < description.instances.size(); ++i)
    {
        const RayTracingInstanceDescription& sourceInstance = description.instances[i];

        if (sourceInstance.bottomLevelIndex >= bottomLevelStructures.size())
        {
            return false;
        }

        const BottomLevelAccelerationStructure* bottomLevel = bottomLevelStructures[sourceInstance.bottomLevelIndex];

        if (!bottomLevel || bottomLevel->GetGPUAddress() == 0)
        {
            return false;
        }

        D3D12_RAYTRACING_INSTANCE_DESC& instance = m_instanceDescriptions[i];

        for (std::size_t row = 0; row < 3; ++row)
        {
            for (std::size_t column = 0; column < 4; ++column)
            {
                instance.Transform[row][column] = sourceInstance.transform[row][column];
            }
        }

        instance.InstanceID = sourceInstance.instanceID;

        instance.InstanceMask = sourceInstance.mask;

        instance.InstanceContributionToHitGroupIndex = 0;
        instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;

        instance.AccelerationStructure = bottomLevel->GetGPUAddress();
    }

    const UINT64 instanceBufferSize = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * m_instanceDescriptions.size();

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

    for (unsigned int i = 0; i < FrameCount; ++i)
    {
        HRESULT result = device->CreateCommittedResource(&uploadHeapProperties, D3D12_HEAP_FLAG_NONE,
                                                         &instanceBufferDescription, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                         nullptr, IID_PPV_ARGS(&m_instanceBuffers[i]));

        if (FAILED(result))
        {
            Log::Error("Failed to create initial TLAS instance buffer. HRESULT: " + std::to_string(result));
            Shutdown();
            return false;
        }
    }

    for (unsigned int frameIndex = 0; frameIndex < FrameCount; ++frameIndex)
    {
        void* mappedData = nullptr;

        HRESULT result = m_instanceBuffers[frameIndex]->Map(0, nullptr, &mappedData);

        if (FAILED(result))
        {
            Log::Error("Failed to map initial TLAS instance buffer. HRESULT: " + std::to_string(result));
            Shutdown();
            return false;
        }

        std::memcpy(mappedData, m_instanceDescriptions.data(), static_cast<std::size_t>(instanceBufferSize));

        m_instanceBuffers[frameIndex]->Unmap(0, nullptr);
    }

    for (unsigned int frameIndex = 0; frameIndex < FrameCount; ++frameIndex)
    {
        m_pendingInstanceUpdates[frameIndex].clear();

        m_pendingInstanceUpdates[frameIndex].resize(m_instanceDescriptions.size(), false);
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS buildInputs{};
    buildInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;

    buildInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    buildInputs.NumDescs = static_cast<UINT>(m_instanceDescriptions.size());
    buildInputs.InstanceDescs = m_instanceBuffers[0]->GetGPUVirtualAddress();
    buildInputs.Flags = description.buildPreference == RayTracingAccelerationStructureBuildPreference::FastBuild
                            ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD
                            : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    buildInputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo{};

    device->GetRaytracingAccelerationStructurePrebuildInfo(&buildInputs, &prebuildInfo);

    if (prebuildInfo.ResultDataMaxSizeInBytes == 0 || prebuildInfo.ScratchDataSizeInBytes == 0 ||

        prebuildInfo.UpdateScratchDataSizeInBytes == 0)
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

    resourceDescription.Width =
        (std::max)(prebuildInfo.ScratchDataSizeInBytes, prebuildInfo.UpdateScratchDataSizeInBytes);
    HRESULT result =
        device->CreateCommittedResource(&defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_scratchResource));

    if (FAILED(result))
    {
        Log::Error("Failed to allocate TLAS scratch resource. HRESULT: " + std::to_string(result));
        Shutdown();
        return false;
    }

    resourceDescription.Width = prebuildInfo.ResultDataMaxSizeInBytes;

    result = device->CreateCommittedResource(&defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription,
                                             D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
                                             IID_PPV_ARGS(&m_resource));

    if (FAILED(result))
    {
        Log::Error("Failed to allocate TLAS result resource. HRESULT: " + std::to_string(result));
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

    m_instanceCount = description.instances.size();
    m_buildFlags = buildInputs.Flags;

    return true;
}

bool TopLevelAccelerationStructure::Update(
    ID3D12GraphicsCommandList4* commandList,
    unsigned int frameIndex,
    const std::vector<RayTracingInstanceDescription>& instances,
    const std::vector<std::size_t>& changedInstanceIndices,
    const std::vector<const BottomLevelAccelerationStructure*>& bottomLevelStructures)
{
    m_recordingIdentity = 0;
    m_recordingSlot = UINT_MAX;
    const auto recordingIdentity = AllocateTLASRecordingIdentity();

    if (recordingIdentity == 0)
    {
        return false;
    }

    if (frameIndex >= FrameCount ||
        !commandList ||
        !m_resource ||
        !m_scratchResource)
    {
        return false;
    }

    if (!m_instanceBuffers[frameIndex] ||
        instances.size() != m_instanceCount ||
        bottomLevelStructures.empty())
    {
        return false;
    }

    // Update the persistent native DXR descriptors for the instances whose scene transforms changed.

    for (std::size_t instanceIndex : changedInstanceIndices)
    {
        if (instanceIndex >= instances.size() || instanceIndex >= m_instanceDescriptions.size())
        {
            return false;
        }

        const RayTracingInstanceDescription& sourceInstance = instances[instanceIndex];

        if (sourceInstance.bottomLevelIndex >= bottomLevelStructures.size())
        {
            return false;
        }

        const BottomLevelAccelerationStructure* bottomLevel = bottomLevelStructures[sourceInstance.bottomLevelIndex];

        if (!bottomLevel || bottomLevel->GetGPUAddress() == 0)
        {
            return false;
        }

        D3D12_RAYTRACING_INSTANCE_DESC& instance = m_instanceDescriptions[instanceIndex];

        for (std::size_t row = 0; row < 3; ++row)
        {
            for (std::size_t column = 0; column < 4; ++column)
            {
                instance.Transform[row][column] = sourceInstance.transform[row][column];
            }
        }

        instance.InstanceID = sourceInstance.instanceID;

        instance.InstanceMask = sourceInstance.mask;
        instance.InstanceContributionToHitGroupIndex = 0;
        instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;

        instance.AccelerationStructure = bottomLevel->GetGPUAddress();

        for (unsigned int pendingFrame = 0; pendingFrame < FrameCount; ++pendingFrame)
        {
            if (!m_pendingInstanceUpdates[pendingFrame][instanceIndex])
            {
                m_pendingInstanceUpdates[pendingFrame][instanceIndex] = true;

                m_pendingInstanceIndices[pendingFrame].push_back(instanceIndex);
            }
        }
    }

    void* mappedData = nullptr;

    HRESULT result = m_instanceBuffers[frameIndex]->Map(0, nullptr, &mappedData);

    if (FAILED(result))
    {
        Log::Error("Failed to map TLAS instance buffer for update. HRESULT: " + std::to_string(result));
        return false;
    }

    auto* mappedInstances = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(mappedData);

    for (std::size_t instanceIndex : m_pendingInstanceIndices[frameIndex])
    {
        mappedInstances[instanceIndex] = m_instanceDescriptions[instanceIndex];

        m_pendingInstanceUpdates[frameIndex][instanceIndex] = false;
    }

    m_pendingInstanceIndices[frameIndex].clear();

    m_instanceBuffers[frameIndex]->Unmap(0, nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS buildInputs{};

    buildInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    buildInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

    buildInputs.NumDescs = static_cast<UINT>(m_instanceDescriptions.size());
    buildInputs.InstanceDescs = m_instanceBuffers[frameIndex]->GetGPUVirtualAddress();

    buildInputs.Flags = m_buildFlags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDescription{};

    buildDescription.Inputs = buildInputs;
    buildDescription.SourceAccelerationStructureData = m_resource->GetGPUVirtualAddress();
    buildDescription.DestAccelerationStructureData = m_resource->GetGPUVirtualAddress();
    buildDescription.ScratchAccelerationStructureData = m_scratchResource->GetGPUVirtualAddress();

    commandList->BuildRaytracingAccelerationStructure(&buildDescription, 0, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = m_resource;

    commandList->ResourceBarrier(1, &barrier);

    m_recordingIdentity = recordingIdentity;
    m_recordingSlot = frameIndex;
    return true;
}

bool TopLevelAccelerationStructure::RecordedInstanceUsesBLAS(
    uint64_t recordingIdentity,
    size_t instanceIndex,
    D3D12_GPU_VIRTUAL_ADDRESS address) const
{
    // Successful Update flushed this slot before recording Build. Its pending
    // list must still be empty and the queried descriptor must remain current.

    if (recordingIdentity == 0 || recordingIdentity != m_recordingIdentity || m_recordingSlot >= FrameCount)
    {
        return false;
    }

    if (!m_resource || !m_instanceBuffers[m_recordingSlot] || address == 0)
    {
        return false;
    }

    if (instanceIndex >= m_instanceDescriptions.size() ||

        instanceIndex >= m_pendingInstanceUpdates[m_recordingSlot].size())
    {
        return false;
    }

    if (!m_pendingInstanceIndices[m_recordingSlot].empty() || m_pendingInstanceUpdates[m_recordingSlot][instanceIndex])
    {
        return false;
    }

    return m_instanceDescriptions[instanceIndex].AccelerationStructure == address;
}

bool TopLevelAccelerationStructure::RefreshBLASReferences(
    ID3D12GraphicsCommandList4* commandList,
    unsigned int frameIndex,
    const std::vector<RayTracingInstanceDescription>& instances,
    const std::vector<const BottomLevelAccelerationStructure*>& bottomLevelStructures)
{
    if (!commandList || frameIndex >= FrameCount || instances.size() != m_instanceCount)
    {
        return false;
    }

    std::vector<std::size_t> changedInstanceIndices;
    changedInstanceIndices.reserve(instances.size());

    for (std::size_t i = 0; i < instances.size(); ++i)
    {
        changedInstanceIndices.push_back(i);
    }

    return Update(commandList, frameIndex, instances, changedInstanceIndices, bottomLevelStructures);
}

void TopLevelAccelerationStructure::Shutdown()
{
    m_recordingIdentity = 0;
    m_recordingSlot = UINT_MAX;

    for (unsigned int i = 0; i < FrameCount; ++i)
    {
        if (m_instanceBuffers[i])
        {
            m_instanceBuffers[i]->Release();
            m_instanceBuffers[i] = nullptr;
        }
    }

    if (m_scratchResource)
    {
        m_scratchResource->Release();
        m_scratchResource = nullptr;
    }

    if (m_resource)
    {
        m_resource->Release();

        m_resource = nullptr;
    }

    m_instanceDescriptions.clear();

    for (unsigned int frameIndex = 0; frameIndex < FrameCount; ++frameIndex)
    {
        m_pendingInstanceUpdates[frameIndex].clear();

        m_pendingInstanceIndices[frameIndex].clear();
    }

    m_instanceCount = 0;

    m_buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
}

D3D12_GPU_VIRTUAL_ADDRESS TopLevelAccelerationStructure::GetGPUAddress() const
{
    return m_resource ? m_resource->GetGPUVirtualAddress() : 0;
}
