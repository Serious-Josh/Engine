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

        // R32_FLOAT raw MIS-selected ray metadata: >0 geometry distance,
        // -1 environment miss, -2 no ray / invalid. Not a specular tracking guide.
        Texture& GetHitDistanceTexture();
        const Texture& GetHitDistanceTexture() const;
        Texture& GetRawReflectionTexture();
        const Texture& GetRawReflectionTexture() const;
        Texture& GetSpecularTrackingDistanceTexture();
        const Texture& GetSpecularTrackingDistanceTexture() const;

    private:
        Texture m_rawReflectionTexture;
        Texture m_hitDistanceTexture;
        Texture m_specularTrackingDistanceTexture;

        RayTracingPipeline m_pipeline;
};
