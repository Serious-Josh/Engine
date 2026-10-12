#pragma once

#include <climits>
#include <d3d12.h>

class DescriptorHeap;
class ResourceUploader;

class Buffer
{
    public:
    Buffer() = default;

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    bool Initialize(
        ID3D12Device* device,
        ResourceUploader& uploader,
        const void* data,
        size_t size,
        D3D12_RESOURCE_STATES finalState);

    bool InitializeGPUWritable(
        ID3D12Device* device,
        size_t capacityBytes);

    // Allocates once in COMMON and synchronously uploads the active prefix.
    // finalState applies within the upload submission; completed buffer state
    // is COMMON after decay, permitting subsequent read-state promotion.
    bool InitializeGPUWritableSeeded(
        ID3D12Device* device,
        ResourceUploader& uploader,
        const void* data,
        size_t size,
        size_t capacityBytes,
        D3D12_RESOURCE_STATES finalState);

    bool ReplaceData(
        ID3D12Device* device,
        ResourceUploader& uploader,
        const void* data,
        size_t size,
        D3D12_RESOURCE_STATES finalState);

    bool PrepareReplacement(
        ID3D12Device* device,
        ResourceUploader& uploader,
        const void* data,
        size_t size,
        D3D12_RESOURCE_STATES finalState,
        Buffer& destination) const;

    bool CommitReplacement(
        ID3D12Device* device,
        Buffer& prepared);

    bool CreateRawSRV(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap);
    bool CreateRawUAV(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap);
    void Shutdown();

    D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const;
    ID3D12Resource* GetResource() const;

    D3D12_RESOURCE_STATES GetResourceState() const;

    // CPU tracking only. Use only after discarding unsubmitted barriers.
    void RestoreStateAfterAbort(D3D12_RESOURCE_STATES state)
    {
        m_resourceState = state;
    }

    void Transition(
        ID3D12GraphicsCommandList* commandList,
        D3D12_RESOURCE_STATES newState);

    unsigned int GetDescriptorIndex() const;
    unsigned int GetUAVDescriptorIndex() const;

    private:
    bool AllocateGPUWritable(
      ID3D12Device* device,
      size_t capacityBytes,
      D3D12_RESOURCE_STATES initialState);
    ID3D12Resource* m_resource = nullptr;

    DescriptorHeap* m_descriptorHeap = nullptr;
    unsigned int m_descriptorIndex = UINT_MAX;
    unsigned int m_uavDescriptorIndex = UINT_MAX;

    D3D12_RESOURCE_STATES m_resourceState = D3D12_RESOURCE_STATE_COMMON;
};
