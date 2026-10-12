#pragma once

#include "Renderer/Texture.h"
#include "Renderer/ComputePipeline.h"

#include "Math/Matrix4.h"

class DescriptorHeap;
class GraphicsDevice;
class ShaderLibrary;
class GBuffer;
class UploadBuffer;

struct AOTemporalConstants
{
    Matrix4 inverseViewProjection;
    Matrix4 previousViewProjection;
    Matrix4 previousInverseViewProjection;

    unsigned int rawAOIndex = 0;
    unsigned int depthIndex = 0;
    unsigned int surfaceIndex = 0;
    unsigned int previousAOIndex = 0;

    unsigned int previousDepthIndex = 0;
    unsigned int previousHistoryLengthIndex = 0;
    unsigned int outputAOIndex = 0;
    unsigned int outputDepthIndex = 0;

    unsigned int outputHistoryLengthIndex = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int accumulatedFrameCount = 0;
};

static_assert(sizeof(AOTemporalConstants) == 240);

class AOTemporalPass
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

    void ResetHistory();

    void Execute(
        ID3D12GraphicsCommandList* commandList,
        Texture& rawAO,
        GBuffer& gBuffer,
        UploadBuffer& uploadBuffer,
        const Matrix4& inverseViewProjection,
        const Matrix4& previousViewProjection,
        const Matrix4& previousInverseViewProjection,
        unsigned int width,
        unsigned int height);

    Texture& GetAccumulatedAOTexture();

    private:
    ComputePipeline m_pipeline;

    Texture m_accumulatedAO[2];
    Texture m_historyDepth[2];
    Texture m_historyLength[2];

    unsigned int m_historyIndex = 0;
    unsigned int m_accumulatedFrameCount = 0;
};
