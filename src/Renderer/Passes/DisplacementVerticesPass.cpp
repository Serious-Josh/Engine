
#include "DisplacementVerticesPass.h"

#include "Renderer/GraphicsDevice.h"
#include "Renderer/Mesh.h"
#include "Renderer/Texture.h"
#include "Renderer/UploadBuffer.h"
#include "Renderer/Shader/ShaderLibrary.h"

#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>

namespace
{
    constexpr D3D12_RESOURCE_STATES VertexReadState =
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // Initial integration contract:
    // A newly initialized GPU-writable mesh begins in COMMON.
    // After a successful dispatch, it ends in VertexReadState.
    //
    // This tracks one mesh per pass instance. It must be replaced
    // with per-resource state tracking before supporting multiple
    // meshes or other writers.
}

bool DisplacementVerticesPass::Initialize(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary)
{
    ShaderDescription shaderDescription{};
    shaderDescription.path = "DisplacementVertices.hlsl";
    shaderDescription.entryPoint = "CSMain";
    shaderDescription.stage = ShaderStage::Compute;

    Shader* computeShader = shaderLibrary.Load(shaderDescription);

    if (!computeShader)
    {
        return false;
    }

    ComputePipelineDescription pipelineDescription{};

    pipelineDescription.computeShader = computeShader;

    RootParameterDescription constants{};
    constants.type = RootParameterType::ConstantBuffer;
    constants.shaderRegister = 0;
    constants.visibility = D3D12_SHADER_VISIBILITY_ALL;

    pipelineDescription.rootParameters.push_back(constants);

    return m_pipeline.Initialize(graphicsDevice.GetDevice(), pipelineDescription);
}

bool DisplacementVerticesPass::Execute(
    ID3D12GraphicsCommandList* commandList,
    Mesh& mesh,
    Texture& heightmap,
    UploadBuffer& uploadBuffer,
    std::uint32_t patchesPerAxis,
    std::uint32_t subdivisionsPerPatch,
    float displacementScale,
    float displacementMidpoint)
{
    if (!commandList ||
        patchesPerAxis == 0 ||
        subdivisionsPerPatch == 0 ||
        !std::isfinite(displacementScale))
    {
        return false;
    }

    if (!std::isfinite(displacementMidpoint))
    {
        return false;
    }

    ID3D12Resource* heightmapResource = heightmap.GetResource();

    ID3D12Resource* vertexResource = mesh.GetVertexBufferResource();

    if (!heightmapResource || !vertexResource)
    {
        return false;
    }

    const D3D12_RESOURCE_DESC heightmapDescription = heightmapResource->GetDesc();

    if (heightmapDescription.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        heightmapDescription.Format != DXGI_FORMAT_R8_UNORM ||
        heightmapDescription.Width == 0 ||
        heightmapDescription.Width > UINT_MAX)
    {
        return false;
    }

    if (heightmapDescription.Height == 0 ||
        heightmapDescription.Height > UINT_MAX ||
        heightmapDescription.DepthOrArraySize != 1 ||
        heightmapDescription.MipLevels == 0)
    {
        return false;
    }

    if (heightmapDescription.SampleDesc.Count != 1 ||
        heightmap.GetDescriptorIndex() == UINT_MAX ||
        mesh.GetVertexBufferUAVDescriptorIndex() == UINT_MAX ||
        mesh.GetVertexStride() != 48)
    {
        return false;
    }

    const std::uint64_t gridResolution = std::uint64_t(patchesPerAxis) * subdivisionsPerPatch;

    const std::uint64_t side = gridResolution + 1;
    const std::uint64_t vertexCount = side * side;

    if (gridResolution == 0 ||
        gridResolution >= UINT_MAX ||
        side > (std::numeric_limits<UINT>::max)() / 48u ||
        vertexCount > mesh.GetVertexCapacity())
    {
        return false;
    }

    if (vertexCount > UINT_MAX / 48u)
    {
        return false;
    }

    const UINT groupCount = static_cast<UINT>((side + 7u) / 8u);

    if (groupCount > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION)
    {
        return false;
    }

    const D3D12_RESOURCE_DESC vertexDescription = vertexResource->GetDesc();

    if (vertexDescription.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||

        (vertexDescription.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0 ||
        vertexDescription.Width < vertexCount * 48u)
    {
        return false;
    }

    DisplacementVerticesConstants constants{};
    constants.patchesPerAxis = patchesPerAxis;
    constants.subdivisionsPerPatch = subdivisionsPerPatch;

    constants.heightmapWidth = static_cast<std::uint32_t>(heightmapDescription.Width);
    constants.heightmapHeight = static_cast<std::uint32_t>(heightmapDescription.Height);
    constants.displacementScale = displacementScale;
    constants.displacementMidpoint = displacementMidpoint;

    constants.heightmapIndex = heightmap.GetDescriptorIndex();
    constants.vertexBufferIndex = mesh.GetVertexBufferUAVDescriptorIndex();

    const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress =
        uploadBuffer.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if (constantsAddress == 0)
    {
        return false;
    }

    heightmap.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    mesh.TransitionVertexBuffer(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    m_pipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, constantsAddress);
    commandList->Dispatch(groupCount, groupCount, 1);

    mesh.TransitionVertexBuffer(commandList, VertexReadState);

    return true;
}

void DisplacementVerticesPass::Shutdown()
{
    m_pipeline.Shutdown();
}
