#pragma once

#include <d3d12.h>
#include <cstddef>

class UploadBuffer
{
    public:
        UploadBuffer() = default;

        UploadBuffer(const UploadBuffer&) = delete;
        UploadBuffer& operator=(const UploadBuffer&) = delete;

        bool Initialize(ID3D12Device* device, std::size_t capacity);

        void Shutdown();

        void Reset();

        D3D12_GPU_VIRTUAL_ADDRESS Allocate(const void* data, std::size_t size, std::size_t alignment);

    private:
        ID3D12Resource* m_resource = nullptr;

        unsigned char* m_mappedData = nullptr;

        std::size_t m_capacity = 0;
        std::size_t m_offset = 0;
};