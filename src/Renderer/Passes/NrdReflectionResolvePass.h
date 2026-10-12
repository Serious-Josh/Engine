#pragma once

#include "Math/Matrix4.h"
#include "Renderer/Texture.h"
#include "Renderer/ComputePipeline.h"
#include <cstddef>

class GraphicsDevice;
class DescriptorHeap;
class ShaderLibrary;
class GBuffer;
class UploadBuffer;

struct NrdReflectionResolveConstants
{
    Matrix4 inverseViewProjection;
    Matrix4 view;

    unsigned int depthIndex;
    unsigned int materialIndex;
    unsigned int surfaceIndex;
    unsigned int materialBufferIndex;

    unsigned int nrdOutputIndex;
    unsigned int outputIndex;
    unsigned int width;
    unsigned int height;
    unsigned int geometricNormalIndex;
    unsigned int padding[3];
};

static_assert(sizeof(NrdReflectionResolveConstants) == 176);
static_assert(offsetof(NrdReflectionResolveConstants, geometricNormalIndex) == 160);
static_assert(offsetof(NrdReflectionResolveConstants, padding) == 164);

class NrdReflectionResolvePass
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
        UploadBuffer& uploadBuffer,
        const Matrix4& inverseViewProjection,
        const Matrix4& view,
        unsigned int materialBufferIndex,
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
