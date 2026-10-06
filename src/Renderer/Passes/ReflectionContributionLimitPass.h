#pragma once
#include "Renderer/Texture.h"
#include "Renderer/ComputePipeline.h"
class GraphicsDevice;
class ShaderLibrary;
class DescriptorHeap;
class UploadBuffer;

// Experimental, stateless NRD-input-only luminance limiter.
class ReflectionContributionLimitPass
{
public:
    bool Initialize(GraphicsDevice&, ShaderLibrary&, DescriptorHeap&, unsigned int width, unsigned int height);
    bool Resize(GraphicsDevice&, DescriptorHeap&, unsigned int width, unsigned int height);
    bool Execute(ID3D12GraphicsCommandList*, Texture& raw, UploadBuffer&, unsigned int width, unsigned int height);
    void Shutdown();
    Texture& GetOutputTexture() { return m_output; }
private:
    bool CreateTexture(GraphicsDevice&, DescriptorHeap&, unsigned int width, unsigned int height);
    Texture m_output;
    ComputePipeline m_pipeline;
};
