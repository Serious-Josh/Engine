#pragma once

#include "Renderer/Texture.h"
#include "Renderer/ComputePipeline.h"

class GraphicsDevice;
class ShaderLibrary;
class DescriptorHeap;
class UploadBuffer;
class GBuffer;

struct GISpatialDenoiseConstants
{
    unsigned int inputIndex;
    unsigned int outputIndex;
    unsigned int depthIndex;
    unsigned int normalIndex;

    unsigned int width;
    unsigned int height;
    unsigned int stepWidth;
    float depthSigma;

    float normalPower;
    unsigned int padding0;
    unsigned int padding1;
    unsigned int padding2;
};

static_assert(sizeof(GISpatialDenoiseConstants) == 48);

class GISpatialDenoisePass
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
        Texture& inputGI,
        UploadBuffer& uploadBuffer,
        unsigned int width,
        unsigned int height);

    void Shutdown();

    Texture& GetOutputTexture();
    const Texture& GetOutputTexture() const;

private:
    bool CreateTextures(
        GraphicsDevice& graphicsDevice,
        DescriptorHeap& descriptorHeap,
        unsigned int width,
        unsigned int height);

    Texture m_pingTexture;
    Texture m_pongTexture;

    ComputePipeline m_pipeline;
};