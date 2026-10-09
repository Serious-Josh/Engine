#pragma once

#include "Renderer/Texture.h"

#include "Renderer/RayTracing/RayTracingPipeline.h"

class DescriptorHeap;
class GraphicsDevice;
class ShaderLibrary;
class RayTracingSceneResources;

class RayTracedAOPass
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
            ID3D12GraphicsCommandList4* commandList,
            DescriptorHeap& descriptorHeap,
            const RayTracingSceneResources& rayTracingScene,
            D3D12_GPU_VIRTUAL_ADDRESS aoConstantsAddress,
            unsigned int width,
            unsigned int height);

        Texture& GetRawAOTexture();
        const Texture& GetRawAOTexture() const;

    private:
            bool CreateTextures(
                ID3D12Device* device,
                DescriptorHeap& descriptorHeap,
                unsigned int width,
                unsigned int height);

            Texture m_rawAOTexture;
            RayTracingPipeline m_pipeline;
};