#pragma once

#include "Renderer/ComputePipeline.h"

#include <cstddef>
#include <cstdint>

class GraphicsDevice;
class ShaderLibrary;
class UploadBuffer;
class Texture;
class Mesh;

struct DisplacementVerticesConstants
{
    std::uint32_t patchesPerAxis;
    std::uint32_t subdivisionsPerPatch;
    std::uint32_t heightmapWidth;
    std::uint32_t heightmapHeight;

    float displacementScale;
    float displacementMidpoint;

    std::uint32_t heightmapIndex;
    std::uint32_t vertexBufferIndex;
};

static_assert(sizeof(DisplacementVerticesConstants) == 32);
static_assert(offsetof(DisplacementVerticesConstants, displacementScale) == 16);
static_assert(offsetof(DisplacementVerticesConstants, heightmapIndex) == 24);

class DisplacementVerticesPass
{
    public:
    bool Initialize(
      GraphicsDevice& graphicsDevice,
      ShaderLibrary& shaderLibrary);

    bool Execute(
        ID3D12GraphicsCommandList* commandList,
        Mesh& mesh,
        Texture& heightmap,
        UploadBuffer& uploadBuffer,
        std::uint32_t patchesPerAxis,
        std::uint32_t subdivisionsPerPatch,
        float displacementScale,
        float displacementMidpoint);

    void Shutdown();

    private:
    ComputePipeline m_pipeline;
};
