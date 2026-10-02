#include "GBuffer.h"
#include "DescriptorHeap.h"

bool GBuffer::Initialize(ID3D12Device* device, unsigned int width, unsigned int height, DescriptorHeap& rtvHeap, DescriptorHeap& dsvHeap, DescriptorHeap& resourceHeap)
{
    if(!m_material.Initialize(
        device,
        width,
        height,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        rtvHeap,
        resourceHeap))
    {
        return false;
    }

    if(!m_surface.Initialize(
        device,
        width,
        height,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        rtvHeap,
        resourceHeap))
    {
        m_material.Shutdown();
        return false;
    }

    if(!m_depth.Initialize(device, width, height, dsvHeap, resourceHeap)){
        m_surface.Shutdown();
        m_material.Shutdown();
        return false;
    }

    return true;
}

bool GBuffer::Resize(ID3D12Device* device, unsigned int width, unsigned int height, DescriptorHeap& rtvHeap, DescriptorHeap& dsvHeap, DescriptorHeap& resourceHeap)
{
    Shutdown();

    return Initialize(device, width, height, rtvHeap, dsvHeap, resourceHeap);
}

void GBuffer::Shutdown()
{
    m_depth.Shutdown();
    m_surface.Shutdown();
    m_material.Shutdown();
}

RenderTarget& GBuffer::GetMaterial()
{
    return m_material;
}

RenderTarget& GBuffer::GetSurface()
{
    return m_surface;
}

DepthBuffer& GBuffer::GetDepth()
{
    return m_depth;
}

const RenderTarget& GBuffer::GetMaterial() const
{
    return m_material;
}

const RenderTarget& GBuffer::GetSurface() const
{
    return m_surface;
}

const DepthBuffer& GBuffer::GetDepth() const
{
    return m_depth;
}