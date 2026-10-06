#include "RenderTarget.h"
#include "DescriptorHeap.h"


RenderTarget::RenderTarget(RenderTarget&& other) noexcept
    : m_resource(other.m_resource)
    , m_rtvHeap(other.m_rtvHeap)
    , m_resourceHeap(other.m_resourceHeap)
    , m_rtvIndex(other.m_rtvIndex)
    , m_srvIndex(other.m_srvIndex)
    , m_width(other.m_width)
    , m_height(other.m_height)
    , m_format(other.m_format)
    , m_state(other.m_state)
{
    other.m_resource = nullptr;
    other.m_rtvHeap = nullptr;
    other.m_resourceHeap = nullptr;
    other.m_rtvIndex = UINT_MAX;
    other.m_srvIndex = UINT_MAX;
    other.m_width = 0;
    other.m_height = 0;
    other.m_format = DXGI_FORMAT_UNKNOWN;
    other.m_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
}

RenderTarget& RenderTarget::operator=(RenderTarget&& other) noexcept
{
    if(this != &other)
    {
        Shutdown();

        m_resource = other.m_resource;
        m_rtvHeap = other.m_rtvHeap;
        m_resourceHeap = other.m_resourceHeap;
        m_rtvIndex = other.m_rtvIndex;
        m_srvIndex = other.m_srvIndex;
        m_width = other.m_width;
        m_height = other.m_height;
        m_format = other.m_format;
        m_state = other.m_state;

        other.m_resource = nullptr;
        other.m_rtvHeap = nullptr;
        other.m_resourceHeap = nullptr;
        other.m_rtvIndex = UINT_MAX;
        other.m_srvIndex = UINT_MAX;
        other.m_width = 0;
        other.m_height = 0;
        other.m_format = DXGI_FORMAT_UNKNOWN;
        other.m_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    return *this;
}

bool RenderTarget::Initialize(ID3D12Device* device, unsigned int width, unsigned int height, DXGI_FORMAT format, DescriptorHeap& rtvHeap, DescriptorHeap& resourceHeap)
{
    if(!device || width == 0 || height == 0){
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
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = format;
    clearValue.Color[0] = 0.0f;
    clearValue.Color[1] = 0.0f;
    clearValue.Color[2] = 0.0f;
    clearValue.Color[3] = 1.0f;

    HRESULT result = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue, IID_PPV_ARGS(&m_resource));

    if(FAILED(result)){
        return false;
    }

    m_state = D3D12_RESOURCE_STATE_RENDER_TARGET;

    m_rtvIndex = rtvHeap.Allocate();

    if(m_rtvIndex == UINT_MAX){
        Shutdown();
        return false;
    }

    m_srvIndex = resourceHeap.Allocate();

    if(m_srvIndex == UINT_MAX){
        m_rtvHeap = &rtvHeap;
        Shutdown();
        return false;
    }

    m_rtvHeap = &rtvHeap;
    m_resourceHeap = &resourceHeap;

    D3D12_RENDER_TARGET_VIEW_DESC rtvDescription{};
    rtvDescription.Format = format;
    rtvDescription.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

    device->CreateRenderTargetView(m_resource, &rtvDescription, rtvHeap.GetCPUHandle(m_rtvIndex));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription{};
    srvDescription.Format = format;
    srvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDescription.Texture2D.MipLevels = 1;

    device->CreateShaderResourceView(m_resource, &srvDescription, resourceHeap.GetCPUHandle(m_srvIndex));

    m_width = width;
    m_height = height;
    m_format = format;

    return true;
}

void RenderTarget::Transition(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES newState)
{
    if(!m_resource || !commandList || m_state == newState){
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = m_state;
    barrier.Transition.StateAfter = newState;

    commandList->ResourceBarrier(1, &barrier);

    m_state = newState;
}

void RenderTarget::Shutdown()
{
    if(m_rtvHeap && m_rtvIndex != UINT_MAX){
        m_rtvHeap->Free(m_rtvIndex);
    }

    if(m_resourceHeap && m_srvIndex != UINT_MAX){
        m_resourceHeap->Free(m_srvIndex);
    }

    if(m_resource){
        m_resource->Release();
        m_resource = nullptr;
    }

    m_rtvHeap = nullptr;
    m_resourceHeap = nullptr;

    m_rtvIndex = UINT_MAX;
    m_srvIndex = UINT_MAX;

    m_width = 0;
    m_height = 0;
    m_format = DXGI_FORMAT_UNKNOWN;
    m_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
}

ID3D12Resource* RenderTarget::GetResource() const
{
    return m_resource;
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderTarget::GetRTV() const
{
    if(!m_rtvHeap || m_rtvIndex == UINT_MAX)
    {
        return {};
    }

    return m_rtvHeap->GetCPUHandle(m_rtvIndex);
}

unsigned int RenderTarget::GetSRVIndex() const
{
    return m_srvIndex;
}

unsigned int RenderTarget::GetWidth() const
{
    return m_width;
}

unsigned int RenderTarget::GetHeight() const
{
    return m_height;
}

DXGI_FORMAT RenderTarget::GetFormat() const
{
    return m_format;
}