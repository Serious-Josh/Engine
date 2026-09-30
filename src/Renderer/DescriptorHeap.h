#pragma once

#include <d3d12.h>
#include <vector>

class DescriptorHeap
{
    public:
        DescriptorHeap() = default;

        DescriptorHeap(const DescriptorHeap&) = delete;
        DescriptorHeap& operator=(const DescriptorHeap&) = delete;

        DescriptorHeap(DescriptorHeap&& other) noexcept;
        DescriptorHeap& operator=(DescriptorHeap&& other) noexcept;

        bool Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, unsigned int capacity, bool shaderVisible);

        void Shutdown();

        unsigned int Allocate();
        void Free(unsigned int index);

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
};