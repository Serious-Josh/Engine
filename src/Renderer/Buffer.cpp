#include "Buffer.h"
#include "DescriptorHeap.h"
#include "ResourceUploader.h"

#include <cstdint>

Buffer::Buffer(Buffer&& other) noexcept
    : m_resource(other.m_resource),
      m_descriptorHeap(other.m_descriptorHeap),
      m_descriptorIndex(other.m_descriptorIndex)
{
    other.m_resource = nullptr;
    other.m_descriptorHeap = nullptr;
    other.m_descriptorIndex = UINT_MAX;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept
{
    if(this != &other)
    {
        Shutdown();

        m_resource = other.m_resource;
        m_descriptorHeap = other.m_descriptorHeap;
        m_descriptorIndex = other.m_descriptorIndex;
        other.m_resource = nullptr;
        other.m_descriptorHeap = nullptr;
        other.m_descriptorIndex = UINT_MAX;
    }

    return *this;
}

bool Buffer::Initialize(
    ID3D12Device* device,
    ResourceUploader& uploader,
    const void* data,
    size_t size,
    D3D12_RESOURCE_STATES finalState
)
{
    if(!device || !data || size == 0){
        return false;
    }

    D3D12_HEAP_PROPERTIES heapProperties{};

    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resourceDescription{};

    resourceDescription.Dimension =
        D3D12_RESOURCE_DIMENSION_BUFFER;

    resourceDescription.Width = size;
    resourceDescription.Height = 1;
    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;
    resourceDescription.Format = DXGI_FORMAT_UNKNOWN;
    resourceDescription.SampleDesc.Count = 1;
    resourceDescription.Layout =
        D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT result = device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDescription,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&m_resource)
    );

    if(FAILED(result))
    {
        return false;
    }

    if(!uploader.UploadBuffer(m_resource, data, size, finalState)){
        Shutdown();
        return false;
    }

    return true;
}

bool Buffer::PrepareReplacement(
            ID3D12Device* device,
            ResourceUploader& uploader,
            const void* data,
            size_t size,
            D3D12_RESOURCE_STATES finalState,
            Buffer& destination) const
{
    if(!device || !data || size == 0 ||
       !m_resource || !m_descriptorHeap ||
       m_descriptorIndex == UINT_MAX ||
       size % sizeof(std::uint32_t) != 0 ||
       size / sizeof(std::uint32_t) > UINT_MAX ||
       &destination == this ||
       destination.m_resource ||
       destination.m_descriptorHeap ||
       destination.m_descriptorIndex != UINT_MAX)
    {
        return false;
    }

    return destination.Initialize(
        device,
        uploader,
        data,
        size,
        finalState);
}

bool Buffer::CommitReplacement(
    ID3D12Device* device,
    Buffer& prepared)
{
    if(!device ||
       &prepared == this ||
       !m_resource ||
       !m_descriptorHeap ||
       m_descriptorIndex == UINT_MAX ||
       !prepared.m_resource ||
       prepared.m_descriptorHeap ||
       prepared.m_descriptorIndex != UINT_MAX)
    {
        return false;
    }

    const D3D12_RESOURCE_DESC description =
        prepared.m_resource->GetDesc();

    if(description.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
       description.Width == 0 ||
       description.Width % sizeof(std::uint32_t) != 0 ||
       description.Width / sizeof(std::uint32_t) > UINT_MAX)
    {
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription{};
    srvDescription.Format = DXGI_FORMAT_R32_TYPELESS;
    srvDescription.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDescription.Buffer.FirstElement = 0;
    srvDescription.Buffer.NumElements = static_cast<UINT>(description.Width / sizeof(std::uint32_t));
    srvDescription.Buffer.StructureByteStride = 0;
    srvDescription.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;

    // The caller must ensure that all GPU work using
    // the old resource and descriptor has completed.
    device->CreateShaderResourceView(
        prepared.m_resource,
        &srvDescription,
        m_descriptorHeap->GetCPUHandle(m_descriptorIndex));

    ID3D12Resource* previous = m_resource;

    m_resource = prepared.m_resource;
    prepared.m_resource = nullptr;

    previous->Release();

    return true;
}

bool Buffer::ReplaceData(
    ID3D12Device* device,
    ResourceUploader& uploader,
    const void* data,
    size_t size,
    D3D12_RESOURCE_STATES finalState)
{
    if(!device || !data || size == 0 ||
       !m_resource || !m_descriptorHeap ||
       m_descriptorIndex == UINT_MAX ||
       size % sizeof(std::uint32_t) != 0 ||
       size / sizeof(std::uint32_t) > UINT_MAX)
    {
        return false;
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resourceDescription{};
    resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDescription.Width = size;
    resourceDescription.Height = 1;
    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;
    resourceDescription.Format = DXGI_FORMAT_UNKNOWN;
    resourceDescription.SampleDesc.Count = 1;
    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* replacement = nullptr;

    const HRESULT result = device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDescription,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&replacement));

    if(FAILED(result))
    {
        return false;
    }

    if(!uploader.UploadBuffer(
        replacement, data, size, finalState))
    {
        replacement->Release();
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription{};
    srvDescription.Format = DXGI_FORMAT_R32_TYPELESS;
    srvDescription.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDescription.Buffer.FirstElement = 0;
    srvDescription.Buffer.NumElements = static_cast<UINT>(size / sizeof(std::uint32_t));
    srvDescription.Buffer.StructureByteStride = 0;
    srvDescription.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;

    // Caller must have completed all GPU work referencing
    // the previous resource and its descriptor.
    device->CreateShaderResourceView(
        replacement,
        &srvDescription,
        m_descriptorHeap->GetCPUHandle(m_descriptorIndex));

    ID3D12Resource* previous = m_resource;
    m_resource = replacement;
    previous->Release();

    return true;
}

bool Buffer::CreateRawSRV(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    if(!device || !m_resource){
        return false;
    }

    m_descriptorIndex = descriptorHeap.Allocate();

    if(m_descriptorIndex == UINT_MAX){
        return false;
    }

    const D3D12_RESOURCE_DESC resourceDescription = m_resource->GetDesc();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription{};
    srvDescription.Format = DXGI_FORMAT_R32_TYPELESS;
    srvDescription.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDescription.Buffer.FirstElement = 0;
    srvDescription.Buffer.NumElements = static_cast<UINT>(resourceDescription.Width / 4);
    srvDescription.Buffer.StructureByteStride = 0;
    srvDescription.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;

    device->CreateShaderResourceView(m_resource, &srvDescription, descriptorHeap.GetCPUHandle(m_descriptorIndex));

    m_descriptorHeap = &descriptorHeap;

    return true;
}

void Buffer::Shutdown()
{
    if(m_descriptorHeap && m_descriptorIndex != UINT_MAX)
    {
        m_descriptorHeap->Free(
            m_descriptorIndex
        );
    }

    if(m_resource){
        m_resource->Release();
        m_resource = nullptr;
    }

    m_descriptorHeap = nullptr;
    m_descriptorIndex = UINT_MAX;
}

D3D12_GPU_VIRTUAL_ADDRESS Buffer::GetGPUAddress() const
{
    if(!m_resource){
        return 0;
    }

    return m_resource->GetGPUVirtualAddress();
}

ID3D12Resource* Buffer::GetResource() const
{
    return m_resource;
}

unsigned int Buffer::GetDescriptorIndex() const
{
    return m_descriptorIndex;
}