#pragma once

#include <d3d12.h>
#include <cstddef>
#include <vector>

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

    // Bytes allocated in the current frame (its ring region plus any overflow pages), and the ring region's capacity.
    std::size_t GetFrameBytesUsed() const noexcept
    {
        return m_offset - m_frameOffset + m_overflowBytesUsed;
    }

    // Overflow pages currently retained across all frame slots (high-water mark, reused, released at Shutdown).
    std::size_t GetOverflowBytesRetained() const noexcept;

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
    // An upload page a frame slot uses once its ring region is full. Owned by that slot: reused from the slot's next Reset,
    // which runs only after the slot's previous submission completed, so no page is rewritten while the GPU reads it.
    struct OverflowPage
    {
        ID3D12Resource* resource = nullptr;
        unsigned char* mappedData = nullptr;
        std::size_t capacity = 0;
    };

    // Bounds overflow per frame slot, so a runaway caller fails its allocation instead of growing without limit.
    static constexpr std::size_t MaxOverflowBytesPerFrame = 64u * 1024u * 1024u;

    D3D12_GPU_VIRTUAL_ADDRESS AllocateOverflow(const void* data, std::size_t size, std::size_t alignment);

    ID3D12Device* m_device = nullptr;
    ID3D12Resource* m_resource = nullptr;

    unsigned char* m_mappedData = nullptr;

    std::size_t m_capacityPerFrame = 0;
    std::size_t m_frameOffset = 0;
    std::size_t m_offset = 0;

    unsigned int m_frameCount = 0;

    std::vector<std::vector<OverflowPage>> m_overflowPages; // Per frame slot.
    unsigned int m_frameIndex = 0;
    std::size_t m_overflowPage = 0;
    std::size_t m_overflowOffset = 0;
    std::size_t m_overflowBytesUsed = 0;
};
