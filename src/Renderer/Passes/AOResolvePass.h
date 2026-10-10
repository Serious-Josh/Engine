#pragma once

#include "Renderer/Texture.h"
#include "Renderer/ComputePipeline.h"
#include "Renderer/DepthBuffer.h"
#include "Renderer/RenderTarget.h"

class GraphicsDevice;
class DescriptorHeap;
class ShaderLibrary;
class UploadBuffer;

struct AOResolveConstants
{
    unsigned int inputAOIndex = 0;
    unsigned int depthIndex = 0;
    unsigned int surfaceIndex = 0;
    unsigned int outputAOIndex = 0;

    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int padding0 = 0;
    unsigned int padding1 = 0;
};

static_assert(sizeof(AOResolveConstants) == 32);

class AOResolvePass
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
        Texture& filteredAO,
        DepthBuffer& depthTexture,
        RenderTarget& surfaceTexture,
        UploadBuffer& uploadBuffer,
        unsigned int width,
        unsigned int height);

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