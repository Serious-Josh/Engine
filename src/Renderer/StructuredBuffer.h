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

        bool Initialize(ID3D12Device* device, ResourceUploader& uploader, const void* data, size_t elementSize, unsigned int elementCount, DescriptorHeap& descriptorHeap);
        void Shutdown();

        ID3D12Resource* GetResource() const;
        unsigned int GetDescriptorIndex() const;

    private:
        ID3D12Resource* m_resource = nullptr;

        DescriptorHeap* m_descriptorHeap = nullptr;
        unsigned int m_descriptorIndex = UINT_MAX;
};