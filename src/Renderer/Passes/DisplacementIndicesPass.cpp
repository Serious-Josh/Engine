#include "DisplacementIndicesPass.h"

#include "Renderer/GraphicsDevice.h"
#include "Renderer/Mesh.h"
#include "Renderer/UploadBuffer.h"
#include "Renderer/Shader/ShaderLibrary.h"

#include <limits>

namespace
{
    constexpr D3D12_RESOURCE_STATES IndexReadState =
        D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}

bool DisplacementIndicesPass::Initialize(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary)
{
    ShaderDescription shaderDescription{};
    shaderDescription.path = "DisplacementIndices.hlsl";
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

bool DisplacementIndicesPass::Execute(
    ID3D12GraphicsCommandList* commandList,
    UploadBuffer& uploadBuffer,
    Mesh& mesh,
    std::uint32_t patchesPerAxis,
    std::uint32_t subdivisionsPerPatch,
    std::uint32_t patchBufferIndex)
{
    constexpr std::uint64_t maxUint32 = (std::numeric_limits<std::uint32_t>::max)();
    constexpr std::uint64_t patchStride = 32;
    constexpr std::uint64_t indexStride = sizeof(std::uint32_t);
    constexpr std::uint64_t vertexStride = 48;
    constexpr std::uint64_t threadsPerGroup = 64;

    const std::uint32_t indexBufferIndex = mesh.GetIndexBufferUAVDescriptorIndex();

    if (!commandList ||
        patchesPerAxis == 0 ||
        subdivisionsPerPatch < 2 ||
        (subdivisionsPerPatch & (subdivisionsPerPatch - 1u)) != 0)
    {
        return false;
    }

    if (patchBufferIndex == maxUint32 ||
        indexBufferIndex == maxUint32 ||
        mesh.GetIndexFormat() != DXGI_FORMAT_R32_UINT)
    {
        return false;
    }

    // Products of two uint32 values fit in uint64. Bound them before
    // squaring the grid or converting element counts into byte sizes.
    const std::uint64_t patchCount = std::uint64_t{patchesPerAxis} * patchesPerAxis;
    const std::uint64_t gridResolution = std::uint64_t{patchesPerAxis} * subdivisionsPerPatch;

    if (patchCount > maxUint32 / patchStride || gridResolution >= maxUint32)
    {
        return false;
    }

    const std::uint64_t groupCount = (patchCount + threadsPerGroup - 1) / threadsPerGroup;

    if (groupCount > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION)
    {
        return false;
    }

    // Match the shader's grid bound, including its 48-byte vertex stride.

    const std::uint64_t rowStride = gridResolution + 1;

    if (rowStride > (maxUint32 / vertexStride) / rowStride)
    {
        return false;
    }

    const std::uint64_t cellCount = gridResolution * gridResolution;
    const std::uint64_t maxRawIndexCount = maxUint32 / indexStride;

    if (cellCount > maxRawIndexCount / 6)
    {
        return false;
    }

    // Full-resolution ordinary cells provide a conservative capacity bound
    // for every supported LOD and stitching pattern.

    const std::uint64_t maxIndexCount = cellCount * 6;
    const std::uint64_t requiredBytes = maxIndexCount * indexStride;
    ID3D12Resource* indexResource = mesh.GetIndexBufferResource();

    if (!indexResource || maxIndexCount > mesh.GetIndexCapacity())
    {
        return false;
    }

    const D3D12_RESOURCE_DESC indexDescription = indexResource->GetDesc();

    if (indexDescription.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||

        (indexDescription.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0 ||
        indexDescription.Width < requiredBytes || indexDescription.Width > maxUint32)
    {
        return false;
    }

    DisplacementIndicesConstants constants{};
    constants.patchesPerAxis = patchesPerAxis;
    constants.subdivisionsPerPatch = subdivisionsPerPatch;

    constants.patchBufferIndex = patchBufferIndex;
    constants.indexBufferIndex = indexBufferIndex;

    const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress =
        uploadBuffer.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if (constantsAddress == 0)
    {
        return false;
    }

    mesh.TransitionIndexBuffer(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    m_pipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, constantsAddress);

    commandList->Dispatch(static_cast<UINT>(groupCount), 1, 1);

    mesh.TransitionIndexBuffer(commandList, IndexReadState);
    return true;
}

void DisplacementIndicesPass::Shutdown()
{
    m_pipeline.Shutdown();
}
