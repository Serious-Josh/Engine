#pragma once

#include <d3d12.h>
#include <vector>

class Scene;
class BottomLevelAccelerationStructure;

class TopLevelAccelerationStructure
{
    public:
        TopLevelAccelerationStructure() = default;

        TopLevelAccelerationStructure(
            const TopLevelAccelerationStructure&) = delete;
        TopLevelAccelerationStructure& operator=(
            const TopLevelAccelerationStructure&) = delete;

        bool Initialize(ID3D12Device5* device, ID3D12GraphicsCommandList4* commandList, const Scene& scene, const BottomLevelAccelerationStructure& blas);
        void Shutdown();

    private:
            ID3D12Resource* m_resource = nullptr;
            ID3D12Resource* m_scratchResource = nullptr;
            ID3D12Resource* m_instanceBuffer = nullptr;
};