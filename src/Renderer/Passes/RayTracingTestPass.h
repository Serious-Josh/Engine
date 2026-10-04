#pragma once

#include "Renderer/RayTracing/RayTracingPipeline.h"
#include "Renderer/Texture.h"

#include <d3d12.h>

class DescriptorHeap;
class GraphicsDevice;
class Mesh;
class Scene;
class ShaderLibrary;
class RenderTarget;
class RayTracingSceneResources;

class RayTracingTestPass
{
    public:
        bool Initialize(
            GraphicsDevice& graphicsDevice,
            ShaderLibrary& shaderLibrary,
            DescriptorHeap& descriptorHeap,
            unsigned int width,
            unsigned int height);
        void Execute(
            ID3D12GraphicsCommandList4* commandList,
            DescriptorHeap& descriptorHeap,
            const RayTracingSceneResources& rayTracingScene,
            D3D12_GPU_VIRTUAL_ADDRESS reconstructionConstantsAddress,
            RenderTarget& sceneColor,
            unsigned int width,
            unsigned int height);
        void Shutdown();

    private:
        Texture m_output;

        RayTracingPipeline m_pipeline;
};