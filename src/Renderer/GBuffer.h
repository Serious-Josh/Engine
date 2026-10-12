#pragma once

#include "DepthBuffer.h"
#include "RenderTarget.h"

class DescriptorHeap;

class GBuffer
{
    public:
    GBuffer() = default;

    GBuffer(const GBuffer&) = delete;
    GBuffer& operator=(const GBuffer&) = delete;

    bool Initialize(
        ID3D12Device* device,
        unsigned int width,
        unsigned int height,
        DescriptorHeap& rtvHeap,
        DescriptorHeap& dsvHeap,
        DescriptorHeap& resourceHeap);

    bool Resize(
        ID3D12Device* device,
        unsigned int width,
        unsigned int height,
        DescriptorHeap& rtvHeap,
        DescriptorHeap& dsvHeap,
        DescriptorHeap& resourceHeap);

    void Shutdown();

    RenderTarget& GetMaterial();
    RenderTarget& GetSurface();
    RenderTarget& GetGeometricNormal();
    DepthBuffer& GetDepth();
    RenderTarget& GetMotion();

    const RenderTarget& GetMaterial() const;
    const RenderTarget& GetSurface() const;
    const RenderTarget& GetGeometricNormal() const;
    const DepthBuffer& GetDepth() const;
    const RenderTarget& GetMotion() const;

    private:
    RenderTarget m_material;
    RenderTarget m_surface;
    RenderTarget m_geometricNormal;
    RenderTarget m_motion;

    DepthBuffer m_depth;
};
