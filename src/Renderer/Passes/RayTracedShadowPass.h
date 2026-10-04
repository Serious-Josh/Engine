#pragma once

#include "Renderer/RayTracing/RayTracingPipeline.h"
#include "Renderer/Texture.h"

class DescriptorHeap;
class GraphicsDevice;
class RayTracingSceneResources;
class ShaderLibrary;

class RayTracedShadowPass
{
    public:
        bool Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height);
        void Execute(
            ID3D12GraphicsCommandList4* commandList,
            DescriptorHeap& descriptorHeap,
            const RayTracingSceneResources& rayTracingScene,
            D3D12_GPU_VIRTUAL_ADDRESS shadowConstantsAddress,
            unsigned int width,
            unsigned int height);
        void Shutdown();

        bool Resize(ID3D12Device* device, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height);

        Texture& GetVisibilityTexture();
        const Texture& GetVisibilityTexture() const;

        unsigned int GetAccumulatedFrameCount() const;
        void ResetHistory();

        unsigned int GetPreviousDepthIndex() const;
        unsigned int GetOutputDepthIndex() const;    
        unsigned int GetPreviousHistoryLengthIndex() const;
    unsigned int GetOutputHistoryLengthIndex() const;  

    private:
        Texture m_historyTextures[2];
        Texture m_depthHistoryTextures[2];
        Texture m_historyLengthTextures[2];
        RayTracingPipeline m_pipeline;

        unsigned int m_historyReadIndex = 0;
        unsigned int m_accumulatedFrameCount = 0;
};