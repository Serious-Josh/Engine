#pragma once

#include <d3d12.h>
#include <cstddef>

class DescriptorHeap;

class UploadBuffer
{
    public:
    UploadBuffer() = default;

    UploadBuffer(const UploadBuffer&) = delete;
    UploadBuffer& operator=(const UploadBuffer&) = delete;

    bool Initialize(
        ID3D12Device* device,
        std::size_t capacityPerFrame,
        unsigned int frameCount);

    void Shutdown();
    void Swap(UploadBuffer& other) noexcept;

    void Reset(unsigned int frameIndex);

    // Bytes allocated in the current frame's region, and its capacity.
    std::size_t GetFrameBytesUsed() const noexcept
    {
        return m_offset - m_frameOffset;
    }

    std::size_t GetCapacityPerFrame() const noexcept
    {
        return m_capacityPerFrame;
    }

    D3D12_GPU_VIRTUAL_ADDRESS Allocate(
        const void* data,
        std::size_t size,
        std::size_t alignment);

    ID3D12Resource* GetResource() const
    {
        return m_resource;
    };

    bool CreateRawSRV(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        D3D12_GPU_VIRTUAL_ADDRESS gpuAddress,
        std::size_t size,
        unsigned int& descriptorIndex) const;

    private:
    ID3D12Resource* m_resource = nullptr;

    unsigned char* m_mappedData = nullptr;

    std::size_t m_capacityPerFrame = 0;
    std::size_t m_frameOffset = 0;
    std::size_t m_offset = 0;

    unsigned int m_frameCount = 0;
};
