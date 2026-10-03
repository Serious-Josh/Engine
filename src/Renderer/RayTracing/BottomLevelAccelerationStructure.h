#pragma once

#include <d3d12.h>

class Mesh;

class BottomLevelAccelerationStructure
{
    public:
        BottomLevelAccelerationStructure() = default;

        BottomLevelAccelerationStructure(
            const BottomLevelAccelerationStructure&) = delete;
        BottomLevelAccelerationStructure& operator=(
            const BottomLevelAccelerationStructure&) = delete;

        bool Initialize(ID3D12Device5* device, ID3D12GraphicsCommandList4* commandList, const Mesh& mesh);
        void Shutdown();

        D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const;

    private:
            ID3D12Resource* m_resource = nullptr;
            ID3D12Resource* m_scratchResource = nullptr;
};