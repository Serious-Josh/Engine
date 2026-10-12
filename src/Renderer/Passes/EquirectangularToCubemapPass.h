#pragma once

#include "Renderer/Pipeline.h"

#include <vector>
#include <d3d12.h>

class DescriptorHeap;
class ShaderLibrary;
class Texture;

class EquirectangularToCubemapPass
{
    public:
    bool Initialize(
      ID3D12Device* device,
      ShaderLibrary& shaderLibrary,
      DescriptorHeap& rtvHeap,
      Texture& cubemap);
    void Execute(
        ID3D12GraphicsCommandList* commandList,
        Texture& sourceTexture,
        Texture& cubemap);
    void Shutdown();

    private:
    Pipeline m_pipeline;

    DescriptorHeap* m_rtvHeap = nullptr;

    std::vector<unsigned int> m_rtvIndices{};

    unsigned int m_size = 0;
    unsigned int m_mipLevels = 0;
};
