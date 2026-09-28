#include "GBuffer.h"
#include "DescriptorHeap.h"

bool GBuffer::Initialize(ID3D12Device* device, unsigned int width, unsigned int height, DescriptorHeap& rtvHeap, DescriptorHeap& dsvHeap, DescriptorHeap& resourceHeap)
{
    if(!m_albedo.Initialize(
        device,
        width,
        height,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        rtvHeap,
        resourceHeap))
    {
        return false;
    }

    if(!m_normal.Initialize(
        device,
        width,
        height,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        rtvHeap,
        resourceHeap))
    {
        m_albedo.Shutdown();
        return false;
    }

    if(!m_depth.Initialize(device, width, height, dsvHeap, resourceHeap)){
        m_normal.Shutdown();
        m_albedo.Shutdown();
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
    m_normal.Shutdown();
    m_albedo.Shutdown();
}

RenderTarget& GBuffer::GetAlbedo()
{
    return m_albedo;
}

RenderTarget& GBuffer::GetNormal()
{
    return m_normal;
}

DepthBuffer& GBuffer::GetDepth()
{
    return m_depth;
}

const RenderTarget& GBuffer::GetAlbedo() const
{
    return m_albedo;
}

const RenderTarget& GBuffer::GetNormal() const
{
    return m_normal;
}

const DepthBuffer& GBuffer::GetDepth() const
{
    return m_depth;
}