#pragma once

#include <d3d12.h>
#include <cstddef>
#include <cstdint>
#include <vector>

class DescriptorHeap
{
    public:
    // Low-overhead accounting; reading it never logs or allocates.
    struct Statistics
    {
        unsigned int capacity = 0;
        size_t allocated = 0;
        size_t available = 0;
        size_t peakAllocated = 0;
        std::uint64_t failedAllocations = 0;
    };

    DescriptorHeap() = default;

    DescriptorHeap(const DescriptorHeap&) = delete;
    DescriptorHeap& operator=(const DescriptorHeap&) = delete;

    DescriptorHeap(DescriptorHeap&& other) noexcept;
    DescriptorHeap& operator=(DescriptorHeap&& other) noexcept;

    bool Initialize(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        unsigned int capacity,
        bool shaderVisible);

    void Shutdown();

    unsigned int Allocate();
    void Free(unsigned int index);
    size_t GetAvailableCount() const noexcept { return m_freeIndices.size(); }
    unsigned int GetCapacity() const noexcept { return m_capacity; }

    // True when `count` descriptors can be allocated now. Lets transactions check capacity
    // before allocating anything, so exhaustion is detected without a partial allocation.
    bool CanAllocate(size_t count) const noexcept { return m_freeIndices.size() >= count; }

    Statistics GetStatistics() const noexcept;

    D3D12_CPU_DESCRIPTOR_HANDLE GetCPUHandle(unsigned int index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetGPUHandle(unsigned int index) const;

    ID3D12DescriptorHeap* GetHeap() const;

    unsigned int GetIndex(D3D12_CPU_DESCRIPTOR_HANDLE handle) const;

    private:
    ID3D12DescriptorHeap* m_heap = nullptr;

    D3D12_DESCRIPTOR_HEAP_TYPE m_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

    unsigned int m_descriptorSize = 0;
    unsigned int m_capacity = 0;
    std::vector<unsigned int> m_freeIndices;

    bool m_shaderVisible = false;

    size_t m_peakAllocated = 0;
    std::uint64_t m_failedAllocations = 0;

    // Exhaustion is logged once per episode; a Free ends the episode.
    bool m_exhaustionReported = false;
};
