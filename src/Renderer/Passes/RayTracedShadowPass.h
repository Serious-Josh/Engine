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

    private:
        Texture m_visibilityTexture;
        RayTracingPipeline m_pipeline;
};