#pragma once

#include <d3d12.h>

class Buffer
{
    public:
        bool Initialize(ID3D12Device* device, const void* data, size_t size);
        void Shutdown();

        D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const;
        ID3D12Resource* GetResource() const;

    private:
        ID3D12Resource* m_resource = nullptr;
};