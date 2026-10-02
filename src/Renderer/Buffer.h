#pragma once

#include <d3d12.h>

class ResourceUploader;

class Buffer
{
    public:
        Buffer() = default;

        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

        Buffer(Buffer&& other) noexcept;
        Buffer& operator=(Buffer&& other) noexcept;

        bool Initialize(ID3D12Device* device, ResourceUploader& uploader, const void* data, size_t size, D3D12_RESOURCE_STATES finalState);
        void Shutdown();

        D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const;
        ID3D12Resource* GetResource() const;

    private:
        ID3D12Resource* m_resource = nullptr;
};