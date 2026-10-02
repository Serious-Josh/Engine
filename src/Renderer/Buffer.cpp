#include "Buffer.h"

#include "ResourceUploader.h"

Buffer::Buffer(Buffer&& other) noexcept
    : m_resource(other.m_resource)
{
    other.m_resource = nullptr;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept
{
    if(this != &other)
    {
        Shutdown();

        m_resource = other.m_resource;
        other.m_resource = nullptr;
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
        D3D12_RESOURCE_STATE_COPY_DEST,
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

void Buffer::Shutdown()
{
    if(m_resource){
        m_resource->Release();
        m_resource = nullptr;
    }
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