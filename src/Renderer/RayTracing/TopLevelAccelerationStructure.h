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
        void Shutdown();

        D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const;

    private:
            ID3D12Resource* m_resource = nullptr;
            ID3D12Resource* m_scratchResource = nullptr;
            ID3D12Resource* m_instanceBuffer = nullptr;
};