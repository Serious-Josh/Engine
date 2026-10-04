#pragma once

#include "Renderer/Pipeline.h"

#include <d3d12.h>

class ShaderLibrary;
class RenderTarget;
class GBuffer;
class StructuredBuffer;
class Texture;

class DeferredLightingPass
{
    public:
        bool Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary);
        void Execute(ID3D12GraphicsCommandList* commandList,
            GBuffer& gBuffer,
            StructuredBuffer& materialBuffer,
            Texture& environmentTexture,
            Texture& environmentalCubemap,
            Texture& diffuseIrradiance,
            Texture& brdfIntegrationLUT,
            Texture& ggxAverageAlbedoLUT,
            Texture& ggxMultiscatterIrradiance,
            Texture& shadowVisiblity,
            RenderTarget& sceneColor,
            D3D12_GPU_VIRTUAL_ADDRESS reconstructionConstantsAddress,
            D3D12_GPU_VIRTUAL_ADDRESS lightConstantsAddress
        );

        void Shutdown();

    private:
        Pipeline m_pipeline;
};