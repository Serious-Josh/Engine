#pragma once

#include "RenderTarget.h"

class DescriptorHeap;

class GBuffer
{
    public:
        GBuffer() = default;

        GBuffer(const GBuffer&) = delete;
        GBuffer& operator=(const GBuffer&) = delete;

        bool Initialize(ID3D12Device* device, unsigned int width, unsigned int height, DescriptorHeap& rtvHeap, DescriptorHeap& resourceHeap);
        
        bool Resize(ID3D12Device* device, unsigned int width, unsigned int height, DescriptorHeap& rtvHeap, DescriptorHeap& resourceHeap);

        void Shutdown();

        RenderTarget& GetAlbedo();
        RenderTarget& GetNormal();

        const RenderTarget& GetAlbedo() const;
        const RenderTarget& GetNormal() const;
    
    private:
        RenderTarget m_albedo;
        RenderTarget m_normal;
};