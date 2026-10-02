#pragma once

#include <d3d12.h>

class ResourceUploader
{
public:
    ResourceUploader() = default;
    ~ResourceUploader();

    ResourceUploader(const ResourceUploader&) = delete;
    ResourceUploader& operator=(const ResourceUploader&) = delete;

    bool Initialize(ID3D12Device* device);
    void Shutdown();

    // Destination must start in COPY_DEST; returns after the GPU copy completes.
    bool UploadBuffer(ID3D12Resource* destination, const void* data, size_t dataSize, D3D12_RESOURCE_STATES finalState);
    bool UploadTexture(ID3D12Resource* destination, const void* data, size_t dataSize);

private:
    bool FinishUpload(ID3D12Resource* uploadBuffer);

    ID3D12Device* m_device = nullptr;

    ID3D12CommandQueue* m_commandQueue = nullptr;
    ID3D12CommandAllocator* m_commandAllocator = nullptr;
    ID3D12GraphicsCommandList* m_commandList = nullptr;

    ID3D12Fence* m_fence = nullptr;
    unsigned long long m_fenceValue = 0;
    void* m_fenceEvent = nullptr;
};
