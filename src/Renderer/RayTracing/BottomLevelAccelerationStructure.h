#pragma once

#include "RayTracingAccelerationStructure.h"

#include <d3d12.h>

class BottomLevelAccelerationStructure
{
    public:
        BottomLevelAccelerationStructure() = default;

        BottomLevelAccelerationStructure(
            const BottomLevelAccelerationStructure&) = delete;
        BottomLevelAccelerationStructure& operator=(
            const BottomLevelAccelerationStructure&) = delete;

        bool Initialize(ID3D12Device5* device, ID3D12GraphicsCommandList4* commandList, const RayTracingBottomLevelDescription& description);
        void Shutdown();

        D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const;

    private:
            ID3D12Resource* m_resource = nullptr;
            ID3D12Resource* m_scratchResource = nullptr;
};