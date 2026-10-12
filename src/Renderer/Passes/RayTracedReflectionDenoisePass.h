#pragma once

#include "../Texture.h"
#include "../ComputePipeline.h"
#include "Math/Matrix4.h"
#include "../UploadBuffer.h"

class DescriptorHeap;
class GraphicsDevice;
class ShaderLibrary;
class GBuffer;

struct ReflectionDenoiseConstants
{
    Matrix4 inverseViewProjection;

    unsigned int reflectionIndex;
    unsigned int depthIndex;
    unsigned int surfaceIndex;
    unsigned int materialIndex;

    unsigned int outputIndex;
    unsigned int width;
    unsigned int height;
    unsigned int stepWidth;

    unsigned int hitDistanceIndex;
    unsigned int padding[3];
};

static_assert(sizeof(ReflectionDenoiseConstants) == 112);

class RayTracedReflectionDenoisePass
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
        Texture& inputReflection,
        Texture& hitDistance,
        GBuffer& gBuffer,
        UploadBuffer& uploadBuffer,
        const Matrix4& inverseViewProjection,
        unsigned int width,
        unsigned int height);

    Texture& GetFilteredReflectionTexture();

    private:
    ComputePipeline m_pipeline;
    Texture m_filteredReflection[2];
    unsigned int m_outputIndex = 0;
};
