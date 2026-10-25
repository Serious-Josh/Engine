#pragma once

#include "DepthBuffer.h"
#include "RenderTarget.h"

class DescriptorHeap;

class GBuffer
{
    public:
    // The material target (and its previous-frame history copies): linear base colour in RGB, stored sRGB-encoded by the
    // hardware on write and decoded on every SRV read (8-bit sRGB keeps the precision of the sRGB8 base-colour textures);
    // alpha is the perceptual roughness, which sRGB formats store as linear UNORM. Every pipeline writing the G-buffer
    // declares this format for render target 0 (FOUNDATION.4).
    static constexpr DXGI_FORMAT MaterialFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

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
