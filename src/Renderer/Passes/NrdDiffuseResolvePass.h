#pragma once

#include "Renderer/Texture.h"
#include "Renderer/ComputePipeline.h"

class GraphicsDevice;
class DescriptorHeap;
class ShaderLibrary;
class UploadBuffer;

struct NrdDiffuseResolveConstants
{
    unsigned int nrdOutputIndex;
    unsigned int outputIndex;
    unsigned int width;
    unsigned int height;

    unsigned int viewZIndex;
    float denoisingRange;
    unsigned int depthIndex;
    unsigned int normalIndex;
};

static_assert(sizeof(NrdDiffuseResolveConstants) == 32);

class GBuffer;

class NrdDiffuseResolvePass
{
    public:
    bool Initialize(
      GraphicsDevice& graphicsDevice,
      ShaderLibrary& shaderLibrary,
      DescriptorHeap& descriptorHeap,
      unsigned int width,
      unsigned int height);

    bool Resize(
        GraphicsDevice& graphicsDevice,
        DescriptorHeap& descriptorHeap,
        unsigned int width,
        unsigned int height);

    void Execute(
        ID3D12GraphicsCommandList* commandList,
        GBuffer& gBuffer,
        Texture& nrdOutput,
        Texture& viewZ,
        UploadBuffer& uploadBuffer,
        unsigned int width,
        unsigned int height,
        float denoisingRange);

    void Shutdown();

    Texture& GetOutputTexture();
    const Texture& GetOutputTexture() const;

    private:
    bool CreateTexture(
      GraphicsDevice& graphicsDevice,
      DescriptorHeap& descriptorHeap,
      unsigned int width,
      unsigned int height);

    Texture m_outputTexture;

    ComputePipeline m_pipeline;
};
