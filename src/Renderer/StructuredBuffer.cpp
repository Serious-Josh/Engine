#include "StructuredBuffer.h"
#include "DescriptorHeap.h"
#include "ResourceUploader.h"

#include <climits>
#include <limits>

bool StructuredBuffer::Initialize(
    ID3D12Device* device,
    ResourceUploader& uploader,
    const void* data,
    size_t elementSize,
    unsigned int elementCount,
    DescriptorHeap& descriptorHeap)
{
    if(!data || !InitializeStorage(device, elementSize, elementCount, descriptorHeap))
    {
        return false;
    }

    if(!uploader.UploadBuffer(m_resource, data, elementSize * elementCount,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
    {
        Shutdown();
        return false;
    }

    return true;
}

bool StructuredBuffer::InitializeStorage(ID3D12Device* device, size_t elementSize,
    unsigned int elementCount, DescriptorHeap& descriptorHeap)
{
    if(!device || m_resource || elementSize == 0 || elementSize > UINT_MAX || elementCount == 0 ||
       elementSize > (std::numeric_limits<size_t>::max)() / elementCount)
    {
        return false;
    }

    const size_t bufferSize = elementSize * elementCount;

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resourceDescription{};
    resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDescription.Width = bufferSize;
    resourceDescription.Height = 1;

    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;
    resourceDescription.Format = DXGI_FORMAT_UNKNOWN;
    resourceDescription.SampleDesc.Count = 1;

    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT result = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_resource));

    if (FAILED(result))
    {
        return false;
    }

    m_descriptorIndex = descriptorHeap.Allocate();

    if (m_descriptorIndex == UINT_MAX)
    {
        m_resource->Release();
        m_resource = nullptr;
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription{};

    srvDescription.Format = DXGI_FORMAT_UNKNOWN;
    srvDescription.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDescription.Buffer.FirstElement = 0;

    srvDescription.Buffer.NumElements = elementCount;
    srvDescription.Buffer.StructureByteStride = static_cast<unsigned int>(elementSize);
    srvDescription.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    device->CreateShaderResourceView(m_resource, &srvDescription, descriptorHeap.GetCPUHandle(m_descriptorIndex));

    m_descriptorHeap = &descriptorHeap;

    return true;
}

void StructuredBuffer::Shutdown()
{
    if (m_descriptorHeap && m_descriptorIndex != UINT_MAX)
    {
        m_descriptorHeap->Free(m_descriptorIndex);
    }

    if (m_resource)
    {
        m_resource->Release();

        m_resource = nullptr;
    }

    m_descriptorHeap = nullptr;
    m_descriptorIndex = UINT_MAX;
}

ID3D12Resource* StructuredBuffer::GetResource() const
{
    return m_resource;
}

unsigned int StructuredBuffer::GetDescriptorIndex() const
{
    return m_descriptorIndex;
}
