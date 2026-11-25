#pragma once

#include <d3d12.h>
#include <cstddef>
#include <climits>

class DescriptorHeap;
class ResourceUploader;

class StructuredBuffer
{
    public:
    StructuredBuffer() = default;

    StructuredBuffer(const StructuredBuffer&) = delete;
    StructuredBuffer& operator=(const StructuredBuffer&) = delete;

    bool Initialize(
        ID3D12Device* device,
        ResourceUploader& uploader,
        const void* data,
        size_t elementSize,
        unsigned int elementCount,
        DescriptorHeap& descriptorHeap);
    // Allocates COMMON-state storage only. Caller owns recording, upload and GPU lifetime.
    bool InitializeStorage(ID3D12Device* device, size_t elementSize, unsigned int elementCount,
        DescriptorHeap& descriptorHeap);
    // Immutable small lookup table. No commands, upload submission or GPU wait.
    bool InitializeReadOnlyUpload(ID3D12Device* device, const void* data, size_t elementSize,
        unsigned int elementCount, DescriptorHeap& descriptorHeap);
    // S0.5: InitializeStorage in two steps. CreateStorageResource touches only the device (any thread; the memory registry
    // entry is deferred); CreateStorageView registers the resource and creates its SRV on the main thread.
    bool CreateStorageResource(ID3D12Device* device, size_t elementSize, unsigned int elementCount);
    bool CreateStorageView(ID3D12Device* device, DescriptorHeap& descriptorHeap);
    // Physical sky S1: COMMON-state storage the GPU writes (ALLOW_UNORDERED_ACCESS), with an SRV and a UAV of the same elements.
    // Always a committed resource: its owner (SkyAtmosphere) allocates it once and keeps it until shutdown. The caller records
    // every barrier and owns the GPU lifetime.
    bool InitializeUnorderedStorage(ID3D12Device* device, size_t elementSize, unsigned int elementCount,
        DescriptorHeap& descriptorHeap);
    void Swap(StructuredBuffer& other) noexcept;
    void Shutdown();

    ID3D12Resource* GetResource() const;
    unsigned int GetDescriptorIndex() const;
    // UINT_MAX unless created by InitializeUnorderedStorage.
    unsigned int GetUAVDescriptorIndex() const;

    private:
    ID3D12Resource* m_resource = nullptr;

    DescriptorHeap* m_descriptorHeap = nullptr;
    unsigned int m_descriptorIndex = UINT_MAX;
    unsigned int m_uavDescriptorIndex = UINT_MAX;
    size_t m_elementSize = 0;
    unsigned int m_elementCount = 0;
};
