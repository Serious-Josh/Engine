#pragma once

#include "RayTracingAccelerationStructure.h"

#include <d3d12.h>
#include <vector>

class BottomLevelAccelerationStructure;

class TopLevelAccelerationStructure
{
    public:
        TopLevelAccelerationStructure() = default;

        TopLevelAccelerationStructure(
            const TopLevelAccelerationStructure&) = delete;
        TopLevelAccelerationStructure& operator=(
            const TopLevelAccelerationStructure&) = delete;

        bool Initialize(ID3D12Device5* device, ID3D12GraphicsCommandList4* commandList, const RayTracingTopLevelDescription& description, const std::vector<const BottomLevelAccelerationStructure*>& bottomLevelStructures);
        bool Update(
            ID3D12GraphicsCommandList4* commandList,
            unsigned int frameIndex,
            const std::vector<RayTracingInstanceDescription>& instances,
            const std::vector<std::size_t>& changedInstanceIndices,
            const std::vector<const BottomLevelAccelerationStructure*>& bottomLevelStructures);

        bool RefreshBLASReferences(
            ID3D12GraphicsCommandList4* commandList,
            unsigned int frameIndex,
            const std::vector<RayTracingInstanceDescription>& instances,
            const std::vector<const BottomLevelAccelerationStructure*>& bottomLevelStructures);

        void Shutdown();

        D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const;

    private:
            ID3D12Resource* m_resource = nullptr;
            ID3D12Resource* m_scratchResource = nullptr;
            
            static constexpr unsigned int FrameCount = 2;
            ID3D12Resource* m_instanceBuffers[FrameCount]{};

            std::size_t m_instanceCount = 0;
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS m_buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;

            std::vector<D3D12_RAYTRACING_INSTANCE_DESC> m_instanceDescriptions;
            std::vector<bool> m_pendingInstanceUpdates[FrameCount];
            std::vector<std::size_t> m_pendingInstanceIndices[FrameCount];
};