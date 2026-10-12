#pragma once

#include "Renderer/Pipeline.h"

#include <vector>
#include <d3d12.h>

class DescriptorHeap;
class ShaderLibrary;
class Texture;

class EnvironmentMipPass
{
    public:
    bool Initialize(
      ID3D12Device* device,
      ShaderLibrary& shaderLibrary,
      DescriptorHeap& rtvHeap,
      Texture& texture);

    void Execute(
        ID3D12GraphicsCommandList* commandList,
        Texture& texture);

    void Shutdown();

    private:
    Pipeline m_pipeline;

    DescriptorHeap* m_rtvHeap = nullptr;

    std::vector<unsigned int> m_rtvIndices{};

    unsigned int m_width = 0;
    unsigned int m_height = 0;
    unsigned int m_mipLevels = 0;
};
