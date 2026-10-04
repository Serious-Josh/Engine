#pragma once

#include "Renderer/RayTracing/BottomLevelAccelerationStructure.h"
#include "Renderer/RayTracing/TopLevelAccelerationStructure.h"
#include "Renderer/RayTracing/RayTracingPipeline.h"
#include "Renderer/Texture.h"

#include <d3d12.h>

class DescriptorHeap;
class GraphicsDevice;
class Mesh;
class Scene;
class ShaderLibrary;
class RenderTarget;

class RayTracingTestPass
{
    public:
        bool Initialize(
            GraphicsDevice& graphicsDevice,
            ID3D12GraphicsCommandList4* commandList,
            ShaderLibrary& shaderLibrary,
            DescriptorHeap& descriptorHeap,
            const Mesh& mesh,
            const Scene& scene,
            unsigned int width,
            unsigned int height);
        void Execute(
            ID3D12GraphicsCommandList4* commandList,
            DescriptorHeap& descriptorHeap,
            D3D12_GPU_VIRTUAL_ADDRESS reconstructionConstantsAddress,
            RenderTarget& sceneColor,
            unsigned int width,
            unsigned int height);
        void Shutdown();

    private:
        Texture m_output;

        BottomLevelAccelerationStructure m_blas;
        TopLevelAccelerationStructure m_tlas;
        RayTracingPipeline m_pipeline;
};