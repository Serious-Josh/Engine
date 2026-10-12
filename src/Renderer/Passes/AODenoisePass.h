#pragma once

#include "Renderer/Texture.h"
#include "Renderer/ComputePipeline.h"

#include "Math/Matrix4.h"

class DescriptorHeap;
class GraphicsDevice;
class ShaderLibrary;
class GBuffer;
class UploadBuffer;

struct AODenoiseConstants
{
    Matrix4 inverseViewProjection;

    unsigned int inputAOIndex = 0;
    unsigned int depthIndex = 0;
    unsigned int surfaceIndex = 0;
    unsigned int outputAOIndex = 0;

    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int stepWidth = 1;
    unsigned int padding = 0;
};

static_assert(sizeof(AODenoiseConstants) == 96);

class AODenoisePass
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

    void Shutdown();

    void Execute(
        ID3D12GraphicsCommandList* commandList,
        Texture& accumulatedAO,
        GBuffer& gBuffer,
        UploadBuffer& uploadBuffer,
        const Matrix4& inverseViewProjection,
        unsigned int width,
        unsigned int height);

    Texture& GetFilteredAOTexture();

    private:
    ComputePipeline m_pipeline;
    Texture m_filteredAO[2];
    unsigned int m_outputIndex = 0;
};
