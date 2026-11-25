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
    // shaderPath: the pass's shader file, or a variant that includes it with other sample counts (the physical sky's).
    bool Initialize(
      ID3D12Device* device,
      ShaderLibrary& shaderLibrary,
      DescriptorHeap& rtvHeap,
      Texture& cubemap,
      const char* shaderPath = "EquirectangularToCubemap.hlsl");
    void Execute(
        ID3D12GraphicsCommandList* commandList,
        Texture& sourceTexture,
        Texture& cubemap);
    // S0.5: rows [rowBegin, rowEnd) of one face of one mip, as Execute draws them (the scissor narrows the same draw, so
    // every texel is computed exactly as in Execute). The subresource is a render target while drawn and is shader-readable
    // after its last row.
    void RecordRows(ID3D12GraphicsCommandList* commandList, Texture& sourceTexture, Texture& cubemap, unsigned int mip,
        unsigned int face, unsigned int rowBegin, unsigned int rowEnd);
    // Physical sky S4: mips [firstMip, endMip) of every face, as Execute draws them, with the transitions before and after all the
    // draws (so they overlap, unlike face-by-face RecordRows). The subresources are shader-readable after.
    void RecordMips(ID3D12GraphicsCommandList* commandList, Texture& sourceTexture, Texture& cubemap, unsigned int firstMip,
        unsigned int endMip);
    unsigned int GetSize() const { return m_size; }
    unsigned int GetMipLevels() const { return m_mipLevels; }
    void Shutdown();

    private:
    Pipeline m_pipeline;

    DescriptorHeap* m_rtvHeap = nullptr;

    std::vector<unsigned int> m_rtvIndices{};

    unsigned int m_size = 0;
    unsigned int m_mipLevels = 0;
};
