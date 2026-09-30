#include "UploadBuffer.h"

#include <cstring>

static std::size_t AlignUp(std::size_t value, std::size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

bool UploadBuffer::Initialize(ID3D12Device* device, std::size_t capacityPerFrame, unsigned int frameCount)
{
    if(!device || capacityPerFrame == 0 || frameCount == 0){
        return false;
    }

    const std::size_t totalCapacity = capacityPerFrame * frameCount;

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resourceDescription{};
    resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;

    resourceDescription.Width = totalCapacity;
    resourceDescription.Height = 1;
    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;
    resourceDescription.SampleDesc.Count = 1;
    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT result = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_resource));

    if(FAILED(result)){
        return false;
    }

    result = m_resource->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedData));

    if(FAILED(result)){
        m_resource->Release();
        m_resource = nullptr;

        return false;
    }

    m_capacityPerFrame = capacityPerFrame;
    m_frameCount = frameCount;
    m_frameOffset = 0;
    m_offset = 0;

    return true;
}

void UploadBuffer::Shutdown()
{
    if(m_resource){
        m_resource->Unmap(0, nullptr);
        m_resource->Release();
        m_resource = nullptr;
    }

    m_mappedData = nullptr;
    m_capacityPerFrame = 0;
    m_frameOffset = 0;
    m_offset = 0;
    m_frameCount = 0;
}

void UploadBuffer::Reset(unsigned int frameIndex)
{
    if(frameIndex >= m_frameCount){
        return;
    }

    m_frameOffset = m_capacityPerFrame * frameIndex;
    m_offset = m_frameOffset;
}

D3D12_GPU_VIRTUAL_ADDRESS UploadBuffer::Allocate(const void* data, std::size_t size, std::size_t alignment)
{
    if(!m_resource || !m_mappedData || !data || size == 0 || alignment == 0){
        return 0;
    }

    const std::size_t alignedOffset = AlignUp(m_offset, alignment);
    const std::size_t frameEnd = m_frameOffset + m_capacityPerFrame;

    if(alignedOffset + size > frameEnd){
        return 0;
    }

    std::memcpy(m_mappedData + alignedOffset, data, size);

    const D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = m_resource->GetGPUVirtualAddress() + alignedOffset;

    m_offset = alignedOffset + size;

    return gpuAddress;
}