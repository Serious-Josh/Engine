#pragma once

#include "../Texture.h"
#include "../ComputePipeline.h"
#include "Math/Matrix4.h"
#include "../UploadBuffer.h"

class DescriptorHeap;
class GraphicsDevice;
class ShaderLibrary;
class GBuffer;

struct DenoiseConstants
{
    Matrix4 inverseViewProjection;

    unsigned int visibilityIndex;
    unsigned int depthIndex;
    unsigned int surfaceIndex;
    unsigned int outputIndex;

    unsigned int width;
    unsigned int height;
    unsigned int stepWidth;
    unsigned int padding;
};

static_assert(sizeof(DenoiseConstants) == 96);

class ShadowDenoisePass
{
    public:
        bool Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height);
        void Resize(GraphicsDevice& graphicsDevice, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height);
        void Shutdown();

        void Execute(ID3D12GraphicsCommandList* commandList, Texture& inputVisibility, GBuffer& gBuffer, UploadBuffer& uploadBuffer, const Matrix4& inverseViewProjection, unsigned int width, unsigned int height);

        Texture& GetFilteredVisibilityTexture();

    private:
        ComputePipeline m_pipeline;
        Texture m_filteredVisibility[2];
        unsigned int m_outputIndex = 0;
};