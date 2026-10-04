#include "Texture.h"
#include "DescriptorHeap.h"

#include <cstring>
#include <climits>
#include <utility>

Texture::Texture(Texture&& other) noexcept
    : m_resource(other.m_resource)
    , m_width(other.m_width)
    , m_height(other.m_height)
    , m_srvIndex(other.m_srvIndex)
    , m_uavIndex(other.m_uavIndex)
    , m_descriptorHeap(other.m_descriptorHeap)
    , m_initialData(std::move(other.m_initialData))
    , m_format(other.m_format)
    , m_bytesPerPixel(other.m_bytesPerPixel)
    , m_subresourceStates(std::move(other.m_subresourceStates))
{
    other.m_resource = nullptr;
    other.m_width = 0;
    other.m_height = 0;
    other.m_srvIndex = UINT_MAX;
    other.m_uavIndex = UINT_MAX;
    other.m_descriptorHeap = nullptr;
    other.m_format = DXGI_FORMAT_UNKNOWN;
    other.m_bytesPerPixel = 0;
    other.m_initialData.clear();
    other.m_subresourceStates.clear();
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
        m_srvIndex = other.m_srvIndex;
        m_uavIndex = other.m_uavIndex;
        m_descriptorHeap = other.m_descriptorHeap;
        m_format = other.m_format;
        m_bytesPerPixel = other.m_bytesPerPixel;
        m_subresourceStates = std::move(other.m_subresourceStates);

        other.m_srvIndex = UINT_MAX;
        other.m_uavIndex = UINT_MAX;
        other.m_descriptorHeap = nullptr;
        other.m_resource = nullptr;
        other.m_width = 0;
        other.m_height = 0;
        other.m_format = DXGI_FORMAT_UNKNOWN;
        other.m_bytesPerPixel = 0;
        other.m_initialData.clear();
        other.m_subresourceStates.clear();
    }

    return *this;
}

bool Texture::Initialize(ID3D12Device* device, const void* data, unsigned int width, unsigned int height, DXGI_FORMAT format, size_t bytesPerPixel)
{
    if(!device || !data || width == 0 || height == 0){
        return false;
    }

    if(format == DXGI_FORMAT_UNKNOWN || bytesPerPixel == 0){
        return false;
    }

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT result = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_resource));

    if(FAILED(result)){
        return false;
    }

    m_subresourceStates.assign(
        1,
        D3D12_RESOURCE_STATE_COPY_DEST
    );

    m_width = width;
    m_height = height;
    m_format = format;
    m_bytesPerPixel = bytesPerPixel;

    m_initialData.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * bytesPerPixel);
    std::memcpy(m_initialData.data(), data, m_initialData.size());

    return true;
}

bool Texture::Initialize(ID3D12Device* device, unsigned int width, unsigned int height, unsigned int arraySize, unsigned int mipLevels, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initialState)
{
    if(!device || width == 0 || height == 0 || arraySize == 0 || mipLevels == 0){
        return false;
    }

    if(format == DXGI_FORMAT_UNKNOWN){
        return false;
    }

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = static_cast<UINT16>(arraySize);
    description.MipLevels = static_cast<UINT16>(mipLevels);
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = flags;

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT result = device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &description,
        initialState,
        nullptr,
        IID_PPV_ARGS(&m_resource)
    );

    if(FAILED(result)){
        return false;
    }

    const unsigned int subresourceCount =
        arraySize * mipLevels;

    m_subresourceStates.assign(
        subresourceCount,
        initialState
    );

    m_width = width;
    m_height = height;
    m_format = format;
    m_bytesPerPixel = 0;
    m_initialData.clear();

    return true;
}

void Texture::Shutdown()
{
    if(m_descriptorHeap && m_srvIndex != UINT_MAX){
        m_descriptorHeap->Free(m_srvIndex);
        m_srvIndex = UINT_MAX;
    }

    if(m_descriptorHeap && m_uavIndex != UINT_MAX){
        m_descriptorHeap->Free(m_uavIndex);
        m_uavIndex = UINT_MAX;
    }

    if(m_resource){
        m_resource->Release();
        m_resource = nullptr;
    }

    m_subresourceStates.clear();
    m_srvIndex = UINT_MAX;
    m_descriptorHeap = nullptr;
    m_width = 0;
    m_height = 0;
    m_format = DXGI_FORMAT_UNKNOWN;
    m_bytesPerPixel = 0;
    m_initialData.clear();
}

void Texture::Transition(
    ID3D12GraphicsCommandList* commandList,
    D3D12_RESOURCE_STATES newState)
{
    if(!m_resource || !commandList){
        return;
    }

    for(unsigned int subresource = 0;
        subresource < m_subresourceStates.size();
        ++subresource)
    {
        Transition(
            commandList,
            newState,
            subresource
        );
    }
}

void Texture::Transition(
    ID3D12GraphicsCommandList* commandList,
    D3D12_RESOURCE_STATES newState,
    unsigned int subresource)
{
    if(!m_resource ||
       !commandList ||
       subresource >= m_subresourceStates.size())
    {
        return;
    }

    D3D12_RESOURCE_STATES& currentState =
        m_subresourceStates[subresource];

    if(currentState == newState){
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type =
        D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;

    barrier.Transition.pResource =
        m_resource;

    barrier.Transition.Subresource =
        subresource;

    barrier.Transition.StateBefore =
        currentState;

    barrier.Transition.StateAfter =
        newState;

    commandList->ResourceBarrier(
        1,
        &barrier
    );

    currentState = newState;
}

bool Texture::CreateSRV(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    if(!device || !m_resource){
        return false;
    }

    unsigned int descriptorIndex = descriptorHeap.Allocate();

    if(descriptorIndex == UINT_MAX){
        return false;
    }

    m_descriptorHeap = &descriptorHeap;

    D3D12_SHADER_RESOURCE_VIEW_DESC description{};
    description.Format = m_resource->GetDesc().Format;
    description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    description.Texture2D.MipLevels = m_resource->GetDesc().MipLevels;

    device->CreateShaderResourceView(m_resource, &description, descriptorHeap.GetCPUHandle(descriptorIndex));

    m_srvIndex = descriptorIndex;

    return true;
}

bool Texture::CreateCubeSRV(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    if(!device || !m_resource){
        return false;
    }

    D3D12_RESOURCE_DESC resourceDescription = m_resource->GetDesc();

    if(resourceDescription.DepthOrArraySize != 6){
        return false;
    }

    unsigned int descriptorIndex = descriptorHeap.Allocate();

    if(descriptorIndex == UINT_MAX){
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC description{};
    description.Format = resourceDescription.Format;
    description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    description.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    description.TextureCube.MostDetailedMip = 0;
    description.TextureCube.MipLevels =
        resourceDescription.MipLevels;
    description.TextureCube.ResourceMinLODClamp = 0.0f;

    device->CreateShaderResourceView(m_resource, &description, descriptorHeap.GetCPUHandle(descriptorIndex));

    m_descriptorHeap = &descriptorHeap;
    m_srvIndex = descriptorIndex;

    return true;
}

void Texture::SetInitialData(
    const void* data,
    size_t dataSize)
{
    if(!data || dataSize == 0){
        m_initialData.clear();
        return;
    }

    m_initialData.resize(dataSize);

    std::memcpy(m_initialData.data(), data, dataSize);
}

bool Texture::CreateUAV(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    if(!device || !m_resource){
        return false;
    }

    if(m_uavIndex != UINT_MAX){
        return false;
    }

    const D3D12_RESOURCE_DESC resourceDescription = m_resource->GetDesc();

    if((resourceDescription.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
    {
        return false;
    }

    const unsigned int descriptorIndex = descriptorHeap.Allocate();

    if(descriptorIndex == UINT_MAX){
        return false;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC description{};
    description.Format = resourceDescription.Format;
    description.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    description.Texture2D.MipSlice = 0;
    description.Texture2D.PlaneSlice = 0;

    device->CreateUnorderedAccessView(m_resource, nullptr, &description, descriptorHeap.GetCPUHandle(descriptorIndex));

    m_descriptorHeap = &descriptorHeap;
    m_uavIndex = descriptorIndex;

    return true;
}

unsigned int Texture::GetUAVIndex() const
{
    return m_uavIndex;
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

unsigned int Texture::GetDescriptorIndex() const
{
    return m_srvIndex;
}