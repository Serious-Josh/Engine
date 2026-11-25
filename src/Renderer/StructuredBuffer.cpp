#include "StructuredBuffer.h"
#include "Debug/MemoryRegistry.h"
#include "Renderer/TextureMemoryAllocator.h"
#include "DescriptorHeap.h"
#include "ResourceUploader.h"

#include <climits>
#include <limits>
#include <cstring>
#include <utility>

void StructuredBuffer::Swap(StructuredBuffer& other) noexcept
{
    std::swap(m_resource, other.m_resource);
    std::swap(m_descriptorHeap, other.m_descriptorHeap);
    std::swap(m_descriptorIndex, other.m_descriptorIndex);
    std::swap(m_uavDescriptorIndex, other.m_uavDescriptorIndex);
    std::swap(m_elementSize, other.m_elementSize);
    std::swap(m_elementCount, other.m_elementCount);
}

bool StructuredBuffer::InitializeReadOnlyUpload(ID3D12Device* device, const void* data,
    size_t elementSize, unsigned int elementCount, DescriptorHeap& descriptorHeap)
{
    if(!device || !data || m_resource || !elementSize || elementSize > D3D12_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES ||
        elementSize % 4 != 0 || !elementCount ||
        elementSize > (std::numeric_limits<size_t>::max)() / elementCount)
        return false;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = elementSize * elementCount;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_resource))))
    {
        return false;
    }

    MemoryRegistry::TrackResource(m_resource);

    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, 0};
    if(FAILED(m_resource->Map(0, &readRange, &mapped)))
    {
        Shutdown();
        return false;
    }
    std::memcpy(mapped, data, elementSize * elementCount);
    m_resource->Unmap(0, nullptr);
    m_descriptorIndex = descriptorHeap.Allocate();
    if(m_descriptorIndex == UINT_MAX)
    {
        Shutdown();
        return false;
    }
    m_descriptorHeap = &descriptorHeap;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Buffer.NumElements = elementCount;
    view.Buffer.StructureByteStride = static_cast<UINT>(elementSize);
    device->CreateShaderResourceView(m_resource, &view, descriptorHeap.GetCPUHandle(m_descriptorIndex));
    return true;
}

bool StructuredBuffer::Initialize(
    ID3D12Device* device,
    ResourceUploader& uploader,
    const void* data,
    size_t elementSize,
    unsigned int elementCount,
    DescriptorHeap& descriptorHeap)
{
    {
        // Engine-owned memory: the upload below writes every byte and activates the buffer first (TextureMemoryAllocator.h).
        const TextureMemoryAllocator::ActivatedBufferScope placement;

        if(!data || !InitializeStorage(device, elementSize, elementCount, descriptorHeap))
        {
            return false;
        }
    }

    if(!uploader.UploadBuffer(m_resource, data, elementSize * elementCount,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
    {
        Shutdown();
        return false;
    }

    return true;
}

bool StructuredBuffer::CreateStorageResource(ID3D12Device* device, size_t elementSize, unsigned int elementCount)
{
    if(!device || m_resource || elementSize == 0 || elementSize > UINT_MAX || elementCount == 0 ||
       elementSize > (std::numeric_limits<size_t>::max)() / elementCount)
    {
        return false;
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resourceDescription{};
    resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDescription.Width = elementSize * elementCount;
    resourceDescription.Height = 1;
    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;
    resourceDescription.Format = DXGI_FORMAT_UNKNOWN;
    resourceDescription.SampleDesc.Count = 1;
    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // Engine-owned memory inside an activation scope or on the render thread (zero-filled when the allocator activates it;
    // TextureMemoryAllocator.h), committed otherwise.
    if (FAILED(TextureMemoryAllocator::CreateBuffer(device, resourceDescription, D3D12_RESOURCE_STATE_COMMON, &m_resource)) &&
        FAILED(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_resource))))
    {
        m_resource = nullptr;
        return false;
    }

    m_elementSize = elementSize;
    m_elementCount = elementCount;
    return true;
}

bool StructuredBuffer::CreateStorageView(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    if (!device || !m_resource || m_descriptorIndex != UINT_MAX)
    {
        return false;
    }

    MemoryRegistry::TrackResource(m_resource);
    m_descriptorIndex = descriptorHeap.Allocate();

    if (m_descriptorIndex == UINT_MAX)
    {
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription{};
    srvDescription.Format = DXGI_FORMAT_UNKNOWN;
    srvDescription.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDescription.Buffer.FirstElement = 0;
    srvDescription.Buffer.NumElements = m_elementCount;
    srvDescription.Buffer.StructureByteStride = static_cast<unsigned int>(m_elementSize);
    srvDescription.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    device->CreateShaderResourceView(m_resource, &srvDescription, descriptorHeap.GetCPUHandle(m_descriptorIndex));
    m_descriptorHeap = &descriptorHeap;
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

    // Engine-owned memory inside an activation scope or on the render thread (zero-filled when the allocator activates it;
    // TextureMemoryAllocator.h), committed otherwise.
    HRESULT result = TextureMemoryAllocator::CreateBuffer(device, resourceDescription, D3D12_RESOURCE_STATE_COMMON,
                                                          &m_resource);

    if (FAILED(result))
    {
        m_resource = nullptr;
        result = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_resource));
    }

    if (FAILED(result))
    {
        return false;
    }

    MemoryRegistry::TrackResource(m_resource);

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

bool StructuredBuffer::InitializeUnorderedStorage(ID3D12Device* device, size_t elementSize, unsigned int elementCount,
    DescriptorHeap& descriptorHeap)
{
    if(!device || m_resource || elementSize == 0 || elementSize > UINT_MAX || elementCount == 0 ||
       elementSize > (std::numeric_limits<size_t>::max)() / elementCount)
    {
        return false;
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resourceDescription{};
    resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDescription.Width = elementSize * elementCount;
    resourceDescription.Height = 1;
    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;
    resourceDescription.Format = DXGI_FORMAT_UNKNOWN;
    resourceDescription.SampleDesc.Count = 1;
    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // Engine-owned memory on the render thread (zero-filled when the allocator activates it) or for an owner that activates
    // it (TextureMemoryAllocator.h); committed otherwise.
    if(FAILED(TextureMemoryAllocator::CreateBuffer(device, resourceDescription, D3D12_RESOURCE_STATE_COMMON, &m_resource)) &&
        FAILED(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_resource))))
    {
        m_resource = nullptr;
        return false;
    }

    MemoryRegistry::TrackResource(m_resource);
    m_elementSize = elementSize;
    m_elementCount = elementCount;
    m_descriptorHeap = &descriptorHeap;
    m_descriptorIndex = descriptorHeap.Allocate();
    m_uavDescriptorIndex = descriptorHeap.Allocate();

    if(m_descriptorIndex == UINT_MAX || m_uavDescriptorIndex == UINT_MAX)
    {
        Shutdown();
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

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDescription{};
    uavDescription.Format = DXGI_FORMAT_UNKNOWN;
    uavDescription.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDescription.Buffer.FirstElement = 0;
    uavDescription.Buffer.NumElements = elementCount;
    uavDescription.Buffer.StructureByteStride = static_cast<unsigned int>(elementSize);
    uavDescription.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    device->CreateUnorderedAccessView(m_resource, nullptr, &uavDescription, descriptorHeap.GetCPUHandle(m_uavDescriptorIndex));
    return true;
}

void StructuredBuffer::Shutdown()
{
    if (m_descriptorHeap && m_descriptorIndex != UINT_MAX)
    {
        m_descriptorHeap->Free(m_descriptorIndex);
    }

    if (m_descriptorHeap && m_uavDescriptorIndex != UINT_MAX)
    {
        m_descriptorHeap->Free(m_uavDescriptorIndex);
    }

    if (m_resource)
    {
        m_resource->Release();

        m_resource = nullptr;
    }

    m_descriptorHeap = nullptr;
    m_descriptorIndex = UINT_MAX;
    m_uavDescriptorIndex = UINT_MAX;
}

ID3D12Resource* StructuredBuffer::GetResource() const
{
    return m_resource;
}

unsigned int StructuredBuffer::GetDescriptorIndex() const
{
    return m_descriptorIndex;
}

unsigned int StructuredBuffer::GetUAVDescriptorIndex() const
{
    return m_uavDescriptorIndex;
}
