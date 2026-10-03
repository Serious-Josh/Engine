#pragma once

#include "Renderer/Pipeline.h"

#include <array>
#include <climits>
#include <d3d12.h>

class DescriptorHeap;
class ShaderLibrary;
class Texture;

class DiffuseIrradiancePass
{
    public:
        bool Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary, DescriptorHeap& rtvHeap, Texture& irradianceCubemap);
        void Execute(ID3D12GraphicsCommandList* commandList, Texture& sourceTexture, Texture& irradianceCubemap);
        void Shutdown();

    private:
        Pipeline m_pipeline;

        DescriptorHeap* m_rtvHeap = nullptr;

        std::array<unsigned int, 6> m_rtvIndices{};

        unsigned int m_size = 0;
};