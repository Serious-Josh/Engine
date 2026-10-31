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

    // R.9I.3 P5D.2A raster identity (render target 4): (TLAS InstanceID + 1, TLAS primitive index) of the surviving
    // fragment. Cleared to (0, 0) = background; (InvalidIdentity, InvalidIdentity) = a draw with no exact TLAS mapping.
    // Always allocated with the G-buffer: D3D12 permits a null RTV only where the pipeline format is UNKNOWN, so a
    // caustics-only allocation would need a second set of G-buffer pipelines.
    static constexpr DXGI_FORMAT IdentityFormat = DXGI_FORMAT_R32G32_UINT;
    static constexpr unsigned int InvalidIdentity = 0xFFFFFFFFu;

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

    RenderTarget& GetIdentity() { return m_identity; }
    const RenderTarget& GetIdentity() const { return m_identity; }

    private:
    RenderTarget m_material;
    RenderTarget m_surface;
    RenderTarget m_geometricNormal;
    RenderTarget m_motion;

    DepthBuffer m_depth;

    RenderTarget m_identity;
};
