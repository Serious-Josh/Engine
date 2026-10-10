#pragma once

#include "Renderer/Texture.h"
#include "Renderer/ComputePipeline.h"
#include "Math/Matrix4.h"

class GraphicsDevice;
class ShaderLibrary;
class DescriptorHeap;
class UploadBuffer;
class GBuffer;

struct GISignedCorrectionConstants
{
    Matrix4 inverseViewProjection;
    Matrix4 previousViewProjection;
    Matrix4 previousInverseViewProjection;
    unsigned int correctionIndex;
    unsigned int previousHistoryIndex;
    unsigned int outputHistoryIndex;
    unsigned int depthIndex;
    unsigned int normalIndex;
    unsigned int previousDepthIndex;
    unsigned int previousNormalIndex;
    unsigned int historyValid;
    unsigned int fullWidth;
    unsigned int fullHeight;
    unsigned int giWidth;
    unsigned int giHeight;
};
static_assert(sizeof(GISignedCorrectionConstants) == 240);

struct GISignedReconstructConstants
{
    unsigned int historyIndex;
    unsigned int depthIndex;
    unsigned int normalIndex;
    unsigned int baselineIndex;
    unsigned int productionGIIndex;
    unsigned int outputIndex;
    unsigned int fullWidth;
    unsigned int fullHeight;
    unsigned int giWidth;
    unsigned int giHeight;
    unsigned int padding[2];
};
static_assert(sizeof(GISignedReconstructConstants) == 48);

class GISignedCorrectionPass
{
public:
    bool Initialize(
        GraphicsDevice& device,
        ShaderLibrary& shaders,
        DescriptorHeap& heap,
        unsigned int width,
        unsigned int height);
    bool Resize(
        GraphicsDevice& device,
        DescriptorHeap& heap,
        unsigned int width,
        unsigned int height);
    bool Execute(
        ID3D12GraphicsCommandList* commandList,
        UploadBuffer& uploadBuffer,
        GBuffer& gBuffer,
        const Matrix4& view,
        const Matrix4& projection);

    void ResetHistory();

    bool Reconstruct(ID3D12GraphicsCommandList* commandList,
        UploadBuffer& uploadBuffer,
        GBuffer& gBuffer,
        Texture& baseline,
        Texture& productionGI);

    void Shutdown();

    Texture& GetSignedCorrectionTexture() { return m_correction; }
    Texture& GetHistoryTexture() { return m_history[m_readIndex]; }
    Texture& GetReconstructedTexture() { return m_reconstructed; }

private:
    bool CreateTextures(GraphicsDevice& device, DescriptorHeap& heap, unsigned int width, unsigned int height);

    Texture m_correction;
    Texture m_history[2];
    Texture m_reconstructed;
    Texture m_previousDepth;
    Texture m_previousNormal;

    Matrix4 m_previousView;
    Matrix4 m_previousProjection;
    Matrix4 m_previousViewProjection;
    Matrix4 m_previousInverseViewProjection;

    ComputePipeline m_pipeline;
    ComputePipeline m_reconstructPipeline;

    unsigned int m_width = 0;
    unsigned int m_height = 0;
    unsigned int m_readIndex = 0;

    bool m_historyValid = false;
};
