#pragma once

#include "../Texture.h"
#include "../ComputePipeline.h"
#include "../UploadBuffer.h"
#include "Math/Matrix4.h"

class DescriptorHeap;
class GraphicsDevice;
class ShaderLibrary;
class GBuffer;

struct ReflectionTemporalConstants
{
    Matrix4 inverseViewProjection;
    Matrix4 previousViewProjection;
    Matrix4 previousInverseViewProjection;

    unsigned int rawReflectionIndex;
    unsigned int depthIndex;
    unsigned int surfaceIndex;
    unsigned int historyIndex;

    unsigned int previousDepthIndex;
    unsigned int outputDepthIndex;
    unsigned int previousHistoryLengthIndex;
    unsigned int outputHistoryLengthIndex;

    unsigned int outputReflectionIndex;
    unsigned int width;
    unsigned int height;
    unsigned int accumulatedFrameCount;
};

static_assert(sizeof(ReflectionTemporalConstants) == 240);

class RayTracedReflectionTemporalPass
{
    public:
    bool Initialize(
      GraphicsDevice& graphicsDevice,
      ShaderLibrary& shaderLibrary,
      DescriptorHeap& descriptorHeap,
      unsigned int width,
      unsigned int height);

    bool Resize(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        unsigned int width,
        unsigned int height);

    void Shutdown();

    void Execute(
        ID3D12GraphicsCommandList* commandList,
        Texture& rawReflection,
        GBuffer& gBuffer,
        UploadBuffer& uploadBuffer,
        const Matrix4& inverseViewProjection,
        const Matrix4& previousViewProjection,
        const Matrix4& previousInverseViewProjection,
        unsigned int width,
        unsigned int height);

    Texture& GetReflectionTexture();
    const Texture& GetReflectionTexture() const;

    unsigned int GetAccumulatedFrameCount() const;
    void ResetHistory();

    private:
    ComputePipeline m_pipeline;

    Texture m_historyTextures[2];
    Texture m_depthHistoryTextures[2];
    Texture m_historyLengthTextures[2];

    unsigned int m_historyReadIndex = 0;
    unsigned int m_accumulatedFrameCount = 0;
};
