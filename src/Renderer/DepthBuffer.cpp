#include "DepthBuffer.h"
#include "DescriptorHeap.h"


bool DepthBuffer::Initialize(ID3D12Device* device, unsigned int width, unsigned int height, DescriptorHeap& dsvHeap, DescriptorHeap& resourceHeap)
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

    // Typeless so the same resource can be viewed as
    // D32_FLOAT for depth testing and R32_FLOAT in shaders.
    description.Format = DXGI_FORMAT_R32_TYPELESS;

    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    HRESULT result = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(&m_resource));

    if(FAILED(result)){
        return false;
    }

    m_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    m_dsvIndex = dsvHeap.Allocate();

    if(m_dsvIndex == UINT_MAX){
        Shutdown();
        return false;
    }

    m_dsvHeap = &dsvHeap;
    m_srvIndex = resourceHeap.Allocate();

    if(m_srvIndex == UINT_MAX){
        Shutdown();
        return false;
    }

    m_resourceHeap = &resourceHeap;

    D3D12_DEPTH_STENCIL_VIEW_DESC  dsvDescription{};
    dsvDescription.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDescription.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDescription.Flags = D3D12_DSV_FLAG_NONE;

    device->CreateDepthStencilView(m_resource, &dsvDescription, dsvHeap.GetCPUHandle(m_dsvIndex));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription{};
    srvDescription.Format = DXGI_FORMAT_R32_FLOAT;
    srvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDescription.Texture2D.MipLevels = 1;

    device->CreateShaderResourceView(m_resource, &srvDescription, resourceHeap.GetCPUHandle(m_srvIndex));

    m_width = width;
    m_height = height;

    return true;
}

void DepthBuffer::Transition(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES newState)
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

void DepthBuffer::Shutdown()
{
    if(m_dsvHeap && m_dsvIndex != UINT_MAX){
        m_dsvHeap->Free(m_dsvIndex);
    }

    if(m_resourceHeap && m_srvIndex != UINT_MAX){
        m_resourceHeap->Free(m_srvIndex);
    }

    if(m_resource){
        m_resource->Release();
        m_resource = nullptr;
    }

    m_dsvHeap = nullptr;
    m_resourceHeap = nullptr;

    m_dsvIndex = UINT_MAX;
    m_srvIndex = UINT_MAX;

    m_width = 0;
    m_height = 0;

    m_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
}

ID3D12Resource* DepthBuffer::GetResource() const
{
    return m_resource;
}

D3D12_CPU_DESCRIPTOR_HANDLE DepthBuffer::GetDSV() const
{
    if(!m_dsvHeap || m_dsvIndex == UINT_MAX){
        return {};
    }

    return m_dsvHeap->GetCPUHandle(m_dsvIndex);
}

unsigned int DepthBuffer::GetSRVIndex() const
{
    return m_srvIndex;
}

unsigned int DepthBuffer::GetWidth() const
{
    return m_width;
}

unsigned int DepthBuffer::GetHeight() const
{
    return m_height;
}
