#pragma once

#include "Renderer/RayTracing/RayTracingPipeline.h"
#include "Renderer/Texture.h"

class DescriptorHeap;
class GraphicsDevice;
class RayTracingSceneResources;
class ShaderLibrary;

class RayTracedReflectionPass
{
    public:
        bool Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height);
        void Execute(ID3D12GraphicsCommandList4* commandList, DescriptorHeap& descriptorHeap, const RayTracingSceneResources& rayTracingScene, D3D12_GPU_VIRTUAL_ADDRESS reflectionConstantsAddress, D3D12_GPU_VIRTUAL_ADDRESS lightConstantsAddress, unsigned int width, unsigned int height);
        bool Resize(ID3D12Device* device, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height);
        void Shutdown();

        Texture& GetHitDistanceTexture();
        const Texture& GetHitDistanceTexture() const;
        Texture& GetRawReflectionTexture();
        const Texture& GetRawReflectionTexture() const;


    private:
        Texture m_rawReflectionTexture;
        Texture m_hitDistanceTexture;

        RayTracingPipeline m_pipeline;
};