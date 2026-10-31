#include "GBuffer.h"
#include "DescriptorHeap.h"

bool GBuffer::Initialize(
    ID3D12Device* device,
    unsigned int width,
    unsigned int height,
    DescriptorHeap& rtvHeap,
    DescriptorHeap& dsvHeap,
    DescriptorHeap& resourceHeap)
{
    if (!m_material.Initialize(device, width, height, MaterialFormat, rtvHeap, resourceHeap,
                               {1.0f, 0.0f, 1.0f, 1.0f}))
    {
        return false;
    }

    if (!m_surface.Initialize(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, rtvHeap, resourceHeap,
                              {0.0f, 1.0f, 0.0f, 1.0f}))
    {
        m_material.Shutdown();
        return false;
    }

    if (!m_motion.Initialize(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, rtvHeap, resourceHeap,
                             {0.0f, 0.0f, 0.0f, 0.0f}))
    {
        m_surface.Shutdown();
        m_material.Shutdown();
        return false;
    }

    if (!m_geometricNormal.Initialize(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, rtvHeap, resourceHeap,
                                      {0.0f, 1.0f, 0.0f, 0.0f}))
    {
        m_motion.Shutdown();
        m_surface.Shutdown();
        m_material.Shutdown();
        return false;
    }

    if (!m_depth.Initialize(device, width, height, dsvHeap, resourceHeap))
    {
        m_motion.Shutdown();
        m_geometricNormal.Shutdown();
        m_surface.Shutdown();
        m_material.Shutdown();
        return false;
    }

    m_depth.GetResource()->SetName(L"GBuffer Depth");

    m_material.GetResource()->SetName(L"GBuffer Material");
    m_surface.GetResource()->SetName(L"GBuffer Surface");
    m_geometricNormal.GetResource()->SetName(L"GBuffer Geometric Normal");
    m_motion.GetResource()->SetName(L"GBuffer Motion");

    if (!m_identity.Initialize(device, width, height, IdentityFormat, rtvHeap, resourceHeap, {0.0f, 0.0f, 0.0f, 0.0f}))
    {
        Shutdown();
        return false;
    }

    m_identity.GetResource()->SetName(L"GBuffer Raster Identity");

    return true;
}

bool GBuffer::Resize(
    ID3D12Device* device,
    unsigned int width,
    unsigned int height,
    DescriptorHeap& rtvHeap,
    DescriptorHeap& dsvHeap,
    DescriptorHeap& resourceHeap)
{
    Shutdown();

    return Initialize(device, width, height, rtvHeap, dsvHeap, resourceHeap);
}

void GBuffer::Shutdown()
{
    m_identity.Shutdown();
    m_depth.Shutdown();
    m_motion.Shutdown();
    m_geometricNormal.Shutdown();
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

RenderTarget& GBuffer::GetMotion()
{
    return m_motion;
}

const RenderTarget& GBuffer::GetMotion() const
{
    return m_motion;
}

RenderTarget& GBuffer::GetGeometricNormal()
{
    return m_geometricNormal;
}

const RenderTarget& GBuffer::GetGeometricNormal() const
{
    return m_geometricNormal;
}
