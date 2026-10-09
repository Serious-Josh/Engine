#pragma once

#include "Math/Matrix4.h"
#include "Renderer/Texture.h"
#include "Renderer/ComputePipeline.h"

class GraphicsDevice;
class DescriptorHeap;
class ShaderLibrary;
class GBuffer;
class UploadBuffer;

struct NrdReflectionPrepareConstants
{
    Matrix4 inverseViewProjection;
    Matrix4 view;

    unsigned int depthIndex;
    unsigned int materialIndex;
    unsigned int surfaceIndex;
    unsigned int motionInputIndex;

    unsigned int materialBufferIndex;
    unsigned int rawReflectionIndex;
    unsigned int trackingDistanceIndex;
    unsigned int width;

    unsigned int height;
    float hitDistanceA;
    float hitDistanceB;
    float hitDistanceC;

    float denoisingRange;
    unsigned int viewZOutputIndex;
    unsigned int motionOutputIndex;
    unsigned int normalRoughnessOutputIndex;

    unsigned int specRadianceHitDistOutputIndex;
    unsigned int rawGIIndex;
    unsigned int giHitDistanceIndex;
    unsigned int diffRadianceHitDistOutputIndex;
};

static_assert(sizeof(NrdReflectionPrepareConstants) == 208);

class NrdReflectionPreparePass
{
    public:
        bool Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height);
        bool Resize(GraphicsDevice& graphicsDevice, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height);
        void Execute(
            ID3D12GraphicsCommandList* commandList,
            GBuffer& gBuffer,
            Texture& rawReflection,
            Texture& trackingDistance,
            Texture& rawGI,
            Texture& giHitDistance,
            UploadBuffer& uploadBuffer,
            const Matrix4& inverseViewProjection,
            const Matrix4& view,
            unsigned int materialBufferIndex,
            unsigned int width,
            unsigned int height,
            float denoisingRange
        );
        void Shutdown();

        Texture& GetViewZTexture();
        const Texture& GetViewZTexture() const;

        Texture& GetMotionTexture();
        const Texture& GetMotionTexture() const;

        Texture& GetNormalRoughnessTexture();
        const Texture& GetNormalRoughnessTexture() const;

        Texture& GetSpecRadianceHitDistTexture();
        const Texture& GetSpecRadianceHitDistTexture() const;

        Texture& GetDiffRadianceHitDistTexture();
        const Texture& GetDiffRadianceHitDistTexture() const;

    private:
        bool CreateTextures(GraphicsDevice& graphicsDevice, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height);

        Texture m_viewZTexture;
        Texture m_motionTexture;
        Texture m_normalRoughnessTexture;
        Texture m_specRadianceHitDistTexture;
        Texture m_diffRadianceHitDistTexture;

        ComputePipeline m_pipeline;
};