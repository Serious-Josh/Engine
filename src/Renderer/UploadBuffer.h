#pragma once

#include <d3d12.h>
#include <cstddef>

class UploadBuffer
{
    public:
        UploadBuffer() = default;

        UploadBuffer(const UploadBuffer&) = delete;
        UploadBuffer& operator=(const UploadBuffer&) = delete;

        bool Initialize(ID3D12Device* device, std::size_t capacityPerFrame, unsigned int frameCount);

        void Shutdown();

        void Reset(unsigned int frameIndex);

        D3D12_GPU_VIRTUAL_ADDRESS Allocate(const void* data, std::size_t size, std::size_t alignment);

    private:
        ID3D12Resource* m_resource = nullptr;

        unsigned char* m_mappedData = nullptr;

        std::size_t m_capacityPerFrame = 0;
        std::size_t m_frameOffset = 0;
        std::size_t m_offset = 0;

        unsigned int m_frameCount = 0;
};