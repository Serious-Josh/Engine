#pragma once

#include "Renderer/ComputePipeline.h"

#include <cstdint>
#include <d3d12.h>

class Mesh;
class ShaderLibrary;
class UploadBuffer;
class GraphicsDevice;

struct DisplacementIndicesConstants
{
    std::uint32_t patchesPerAxis;
    std::uint32_t subdivisionsPerPatch;
    std::uint32_t patchBufferIndex;
    std::uint32_t indexBufferIndex;
};

static_assert(sizeof(DisplacementIndicesConstants) == 16);

class DisplacementIndicesPass
{
    public:
    bool Initialize(
      GraphicsDevice& graphicsDevice,
      ShaderLibrary& shaderLibrary);

    bool Execute(
        ID3D12GraphicsCommandList* commandList,
        UploadBuffer& uploadBuffer,
        Mesh& mesh,
        std::uint32_t patchesPerAxis,
        std::uint32_t subdivisionsPerPatch,
        std::uint32_t patchBufferIndex);

    void Shutdown();

    private:
    ComputePipeline m_pipeline;
};
