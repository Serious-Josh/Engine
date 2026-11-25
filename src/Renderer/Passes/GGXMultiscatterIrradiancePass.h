#pragma once

#include "Renderer/Pipeline.h"

#include <climits>
#include <d3d12.h>
#include <vector>

class DescriptorHeap;
class ShaderLibrary;
class Texture;

class GGXMultiscatterIrradiancePass
{
    public:
    // shaderPath: the pass's shader file, or a variant that includes it with other sample counts (the physical sky's).
    bool Initialize(
      ID3D12Device* device,
      ShaderLibrary& shaderLibrary,
      DescriptorHeap& rtvHeap,
      Texture& irradianceCubemap,
      const char* shaderPath = "GGXMultiscatterIrradiance.hlsl");
    void Execute(
        ID3D12GraphicsCommandList* commandList,
        Texture& sourceTexture,
        Texture& brdfIntegrationLUT,
        Texture& irradianceCubemap);
    // S0.5: one face of one mip, exactly as Execute draws it.
    void RecordFace(ID3D12GraphicsCommandList* commandList, Texture& sourceTexture, Texture& brdfIntegrationLUT,
        Texture& irradianceCubemap, unsigned int mip, unsigned int face);
    unsigned int GetSize() const { return m_size; }
    unsigned int GetMipLevels() const { return m_mipLevels; }
    void Shutdown();

    private:
    Pipeline m_pipeline;

    DescriptorHeap* m_rtvHeap = nullptr;

    std::vector<unsigned int> m_rtvIndices;

    unsigned int m_size = 0;
    unsigned int m_mipLevels = 0;
};
