#pragma once

#include "Renderer/Pipeline.h"

#include <climits>
#include <d3d12.h>

class DescriptorHeap;
class ShaderLibrary;
class Texture;

class GGXAverageAlbedoPass
{
    public:
        bool Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary, DescriptorHeap& rtvHeap, Texture& texture);
        void Execute(ID3D12GraphicsCommandList* commandList, Texture& texture);
        void Shutdown();

    private:
        Pipeline m_pipeline;

        DescriptorHeap* m_rtvHeap = nullptr;
        unsigned int m_rtvIndex = UINT_MAX;

        unsigned int m_width = 0;
        unsigned int m_height = 0;
};