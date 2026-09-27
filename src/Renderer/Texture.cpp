#include "Texture.h"
#include "DescriptorHeap.h"

#include <cstring>
#include <utility>

Texture::Texture(Texture&& other) noexcept
    : m_resource(other.m_resource)
    , m_width(other.m_width)
    , m_height(other.m_height)
    , m_initialData(std::move(other.m_initialData))
{
    other.m_resource = nullptr;
    other.m_width = 0;
    other.m_height = 0;
}

Texture& Texture::operator=(Texture&& other) noexcept
{
    if(this != &other)
    {
        Shutdown();

        m_resource = other.m_resource;
        m_width = other.m_width;
        m_height = other.m_height;
        m_initialData = std::move(other.m_initialData);

        other.m_resource = nullptr;
        other.m_width = 0;
        other.m_height = 0;
    }

    return *this;
}

bool Texture::Initialize(ID3D12Device* device, const void* data, unsigned int width, unsigned int height)
{
    if(!device || !data || width == 0 || height == 0){
        return false;
    }

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT result = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_resource));

    if(FAILED(result)){
        return false;
    }

    m_width = width;
    m_height = height;

    m_initialData.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    std::memcpy(m_initialData.data(), data, m_initialData.size());

    return true;
}

void Texture::Shutdown()
{
    if(m_resource){
        m_resource->Release();
        m_resource = nullptr;
    }

    m_width = 0;
    m_height = 0;
    m_initialData.clear();
}

bool Texture::CreateSRV(ID3D12Device* device, DescriptorHeap& descriptorHeap, unsigned int descriptorIndex)
{
    if(!m_resource){
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC description{};
    description.Format = m_resource->GetDesc().Format;
    description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    description.Texture2D.MipLevels = m_resource->GetDesc().MipLevels;

    device->CreateShaderResourceView(m_resource, &description, descriptorHeap.GetCPUHandle(descriptorIndex));

    return true;
}

ID3D12Resource* Texture::GetResource() const
{
    return m_resource;
}

const void* Texture::GetInitialData() const
{
    return m_initialData.data();
}

size_t Texture::GetInitialDataSize() const
{
    return m_initialData.size();
}