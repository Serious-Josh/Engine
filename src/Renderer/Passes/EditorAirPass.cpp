#include "Renderer/Passes/EditorAirPass.h"

#include "Renderer/DepthBuffer.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/UploadBuffer.h"

#include <algorithm>
#include <cmath>

namespace
{
struct EditorAirConstants
{
    Matrix4 viewProjection;
    AirFieldBinding field;
    AirVectorParameters parameters;
    // AIR.4D D3: the split pass's list and arguments (UAV descriptors).
    std::uint32_t splitListIndex = 0;
    std::uint32_t splitArgumentsIndex = 0;
    std::uint32_t padding[2] = {0, 0};
};

// The split pass's arguments: D3D12_DRAW_ARGUMENTS (2 vertices, the list's length in instances, 0, 0).
constexpr std::size_t SplitArgumentBytes = sizeof(D3D12_DRAW_ARGUMENTS);

static_assert(sizeof(EditorAirConstants) == EditorAirPass::ConstantCount * sizeof(float));

std::int32_t FloorDivide(std::int64_t value, std::int64_t divisor)
{
    const std::int64_t quotient = value / divisor;
    return static_cast<std::int32_t>((value % divisor != 0 && value < 0) ? quotient - 1 : quotient);
}
}

AirVectorParameters MakeAirVectorParameters(const AirFieldBinding& field, const AirVisualizationRequest& request)
{
    AirVectorParameters parameters;
    // AIR.4D D3: anchors are per cell, so their blocks are at least a cell; each block draws both pieces.
    const bool anchors = request.placement == AirVectorPlacement::PieceAnchors;
    const int exponent = std::clamp(request.spacingExponent, anchors ? 0 : -2, 3);
    parameters.quarterCells = 1u << (exponent + 2);
    parameters.lengthScale = request.lengthScale;
    parameters.maximumLength = request.maximumLength;
    parameters.colorMaximumSpeed = request.colorMaximumSpeed;
    parameters.minimumSpeed = request.minimumSpeed;
    parameters.placement = static_cast<std::uint32_t>(request.placement);
    parameters.vectorsPerBlock = anchors ? 2u : 1u;
    parameters.ambientVelocity[0] = request.ambientVelocity.x;
    parameters.ambientVelocity[1] = request.ambientVelocity.y;
    parameters.ambientVelocity[2] = request.ambientVelocity.z;

    // The blocks holding a cell of the window: cells scale to blocks by 2^-exponent (exact in integers either way).
    std::int64_t windowFirst[3];
    std::int64_t windowBlocks[3];

    for(std::size_t axis = 0; axis < 3; ++axis)
    {
        const std::int64_t first = field.minimum[axis];
        const std::int64_t end = first + static_cast<std::int64_t>(field.dimensions[axis]);
        windowFirst[axis] = exponent >= 0 ? FloorDivide(first, std::int64_t{1} << exponent) : first << -exponent;
        const std::int64_t last = exponent >= 0 ? FloorDivide(end - 1, std::int64_t{1} << exponent) : (end << -exponent) - 1;
        windowBlocks[axis] = last - windowFirst[axis] + 1;
    }

    // The largest cube edge (clamped to the window per axis) whose box fits the budget; every vector of a block counts.
    const std::uint64_t vectors = std::clamp(request.maximumVectors, 1u, EditorAirPass::MaximumVectors);
    const std::uint64_t budget = (std::max)(std::uint64_t{1}, vectors / parameters.vectorsPerBlock);
    const auto boxCount = [&](std::int64_t edge)
    {
        std::uint64_t count = 1;

        for(const std::int64_t blocks : windowBlocks)
        {
            count *= static_cast<std::uint64_t>((std::min)(edge, blocks));
        }

        return count;
    };
    std::int64_t low = 1;
    std::int64_t high = (std::max)({windowBlocks[0], windowBlocks[1], windowBlocks[2]});

    while(low < high)
    {
        const std::int64_t middle = (low + high + 1) / 2;

        if(boxCount(middle) <= budget)
        {
            low = middle;
        }
        else
        {
            high = middle - 1;
        }
    }

    // Centred on the focus's block, slid back inside the window.
    const double blockSize = static_cast<double>(field.cellSize) * parameters.quarterCells * 0.25;
    const float focus[3] = {request.focus.x, request.focus.y, request.focus.z};

    for(std::size_t axis = 0; axis < 3; ++axis)
    {
        const std::int64_t extent = (std::min)(low, windowBlocks[axis]);
        const double focusBlock = std::isfinite(focus[axis]) ? std::floor(focus[axis] / blockSize) : 0.0;
        const double centred = std::clamp(focusBlock - static_cast<double>(extent / 2), static_cast<double>(windowFirst[axis]),
            static_cast<double>(windowFirst[axis] + windowBlocks[axis] - extent));
        parameters.firstBlock[axis] = static_cast<std::int32_t>(centred);
        parameters.blocks[axis] = static_cast<std::uint32_t>(extent);
    }

    return parameters;
}

std::uint32_t AirVectorCount(const AirVectorParameters& parameters)
{
    const std::uint64_t blocks = std::uint64_t{parameters.blocks[0]} * parameters.blocks[1] * parameters.blocks[2];
    const std::uint64_t count = blocks * parameters.vectorsPerBlock;
    return static_cast<std::uint32_t>((std::min)(count, std::uint64_t{EditorAirPass::MaximumVectors}));
}

bool EditorAirPass::Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary)
{
    m_device = device;
    const Shader* vertexShader = shaderLibrary.Load({"EditorAir.hlsl", "VSMain", ShaderStage::Vertex});
    const Shader* pixelShader = shaderLibrary.Load({"EditorAir.hlsl", "PSMain", ShaderStage::Pixel});
    const Shader* splitVertexShader = shaderLibrary.Load({"EditorAirSplit.hlsl", "VSSplit", ShaderStage::Vertex});
    const Shader* resetShader = shaderLibrary.Load({"EditorAir.hlsl", "CSResetSplit", ShaderStage::Compute});
    const Shader* classifyShader = shaderLibrary.Load({"EditorAir.hlsl", "CSClassify", ShaderStage::Compute});
    const Shader* cellVertexShader = shaderLibrary.Load({"EditorAirCells.hlsl", "VSMain", ShaderStage::Vertex});
    const Shader* cellPixelShader = shaderLibrary.Load({"EditorAirCells.hlsl", "PSMain", ShaderStage::Pixel});
    const Shader* pieceVertexShader = shaderLibrary.Load({"EditorAirPieces.hlsl", "VSMain", ShaderStage::Vertex});
    const Shader* piecePixelShader = shaderLibrary.Load({"EditorAirPieces.hlsl", "PSMain", ShaderStage::Pixel});

    if(!vertexShader || !pixelShader || !cellVertexShader || !cellPixelShader || !pieceVertexShader || !piecePixelShader ||
        !splitVertexShader || !resetShader || !classifyShader)
    {
        return false;
    }

    PipelineDescription description;
    description.vertexShader = vertexShader;
    description.pixelShader = pixelShader;
    description.renderTargets.count = 1;
    description.renderTargets.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    // Occluded by the scene; contributes nothing to its depth.
    description.depthEnabled = true;
    description.depthFormat = DXGI_FORMAT_D32_FLOAT;
    description.depthComparison = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    description.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.rootParameters.push_back({RootParameterType::Constants, 0, ConstantCount, D3D12_SHADER_VISIBILITY_VERTEX});

    // The cells: translucent triangles, otherwise the same depth rules.
    PipelineDescription cells = description;
    cells.vertexShader = cellVertexShader;
    cells.pixelShader = cellPixelShader;
    cells.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    cells.alphaBlend = true;
    cells.rootParameters = {{RootParameterType::Constants, 0, CellConstantCount, D3D12_SHADER_VISIBILITY_VERTEX}};

    // The pieces: the same, with constants too large for root constants.
    PipelineDescription pieces = cells;
    pieces.vertexShader = pieceVertexShader;
    pieces.pixelShader = piecePixelShader;
    RootParameterDescription constants{};
    constants.type = RootParameterType::ConstantBuffer;
    constants.shaderRegister = 0;
    constants.visibility = D3D12_SHADER_VISIBILITY_VERTEX;
    pieces.rootParameters = {constants};

    // AIR.4D D3: the split pass draws like the lean one; its list is reset and filled by compute shaders with the same constants.
    PipelineDescription split = description;
    split.vertexShader = splitVertexShader;
    ComputePipelineDescription reset{};
    reset.computeShader = resetShader;
    reset.rootParameters.push_back({RootParameterType::Constants, 0, ConstantCount, D3D12_SHADER_VISIBILITY_ALL});
    ComputePipelineDescription classify = reset;
    classify.computeShader = classifyShader;

    D3D12_INDIRECT_ARGUMENT_DESC draw{};
    draw.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC signature{};
    signature.ByteStride = static_cast<UINT>(SplitArgumentBytes);
    signature.NumArgumentDescs = 1;
    signature.pArgumentDescs = &draw;

    return m_pipeline.Initialize(device, description) && m_cellPipeline.Initialize(device, cells) && m_piecePipeline.Initialize(device, pieces) &&
        m_splitPipeline.Initialize(device, split) && m_resetSplitPipeline.Initialize(device, reset) &&
        m_classifyPipeline.Initialize(device, classify) &&
        SUCCEEDED(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&m_splitSignature)));
}

bool EditorAirPass::EnsureSplitResources(DescriptorHeap& descriptorHeap)
{
    if(m_splitResourcesReady)
    {
        return true;
    }

    // Capacity for every vector the largest budget allows, so the list never overflows.
    m_splitResourcesReady = m_device != nullptr && m_splitList.InitializeGPUWritable(m_device, SplitListBytes) &&
        m_splitList.CreateRawUAV(m_device, descriptorHeap) && m_splitArguments.InitializeGPUWritable(m_device, SplitArgumentBytes) &&
        m_splitArguments.CreateRawUAV(m_device, descriptorHeap);

    if(!m_splitResourcesReady)
    {
        m_splitList.Shutdown();
        m_splitArguments.Shutdown();
    }

    return m_splitResourcesReady;
}

void EditorAirPass::ReleaseSplitResources()
{
    m_splitList.Shutdown();
    m_splitArguments.Shutdown();
    m_splitResourcesReady = false;
}

void EditorAirPass::Shutdown()
{
    ReleaseSplitResources();
    m_splitSignature.Reset();
    m_classifyPipeline.Shutdown();
    m_resetSplitPipeline.Shutdown();
    m_splitPipeline.Shutdown();
    m_piecePipeline.Shutdown();
    m_cellPipeline.Shutdown();
    m_pipeline.Shutdown();
}

bool EditorAirPass::Begin(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, RenderTarget& viewportColor,
    DepthBuffer& viewportDepth, const Pipeline& pipeline, D3D12_RESOURCE_STATES& previousDepthState) const
{
    if(viewportColor.GetWidth() != viewportDepth.GetWidth() || viewportColor.GetHeight() != viewportDepth.GetHeight() || !pipeline.IsInitialized())
    {
        return false;
    }

    // Read-only depth: the presented depth stays exactly as the scene left it.
    previousDepthState = viewportDepth.GetState();
    viewportDepth.Transition(commandList, D3D12_RESOURCE_STATE_DEPTH_READ);
    const D3D12_CPU_DESCRIPTOR_HANDLE colorTarget = viewportColor.GetRTV();
    const D3D12_CPU_DESCRIPTOR_HANDLE readOnlyDepth = viewportDepth.GetReadOnlyDSV();
    commandList->OMSetRenderTargets(1, &colorTarget, FALSE, &readOnlyDepth);

    // The shaders read the air field through the bindless heap.
    ID3D12DescriptorHeap* heaps[]{descriptorHeap.GetHeap()};
    commandList->SetDescriptorHeaps(1, heaps);
    pipeline.Bind(commandList);
    return true;
}

void EditorAirPass::End(ID3D12GraphicsCommandList* commandList, RenderTarget& viewportColor, DepthBuffer& viewportDepth,
    D3D12_RESOURCE_STATES previousDepthState) const
{
    const D3D12_CPU_DESCRIPTOR_HANDLE colorTarget = viewportColor.GetRTV();
    commandList->OMSetRenderTargets(1, &colorTarget, FALSE, nullptr);
    viewportDepth.Transition(commandList, previousDepthState);
}

std::uint32_t EditorAirPass::Execute(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, RenderTarget& viewportColor,
    DepthBuffer& viewportDepth, const Matrix4& viewProjection, const AirFieldBinding& field, const AirVisualizationRequest& request)
{
    EditorAirConstants constants;
    constants.viewProjection = viewProjection;
    constants.field = field;
    constants.parameters = MakeAirVectorParameters(field, request);
    const std::uint32_t count = field.valid != 0 ? AirVectorCount(constants.parameters) : 0;
    // Piece anchors need no sampling, so only hashed points have a split pass.
    const bool hashed = request.placement == AirVectorPlacement::HashedPoints;

    if(count == 0 || viewportColor.GetWidth() != viewportDepth.GetWidth() || viewportColor.GetHeight() != viewportDepth.GetHeight() ||
        (hashed && !EnsureSplitResources(descriptorHeap)))
    {
        return 0;
    }

    if(hashed)
    {
        constants.splitListIndex = m_splitList.GetUAVDescriptorIndex();
        constants.splitArgumentsIndex = m_splitArguments.GetUAVDescriptorIndex();

        // The split list: emptied, then filled by one thread per instance; a barrier before the draws read it.
        ID3D12DescriptorHeap* heaps[]{descriptorHeap.GetHeap()};
        commandList->SetDescriptorHeaps(1, heaps);
        m_splitList.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_splitArguments.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_resetSplitPipeline.Bind(commandList);
        commandList->SetComputeRoot32BitConstants(0, ConstantCount, &constants, 0);
        commandList->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        commandList->ResourceBarrier(1, &barrier);
        m_classifyPipeline.Bind(commandList);
        commandList->SetComputeRoot32BitConstants(0, ConstantCount, &constants, 0);
        commandList->Dispatch((count + 63) / 64, 1, 1);
        commandList->ResourceBarrier(1, &barrier);
        m_splitArguments.Transition(commandList, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }

    D3D12_RESOURCE_STATES previousDepthState{};

    if(!Begin(commandList, descriptorHeap, viewportColor, viewportDepth, m_pipeline, previousDepthState))
    {
        return 0;
    }

    commandList->SetGraphicsRoot32BitConstants(0, ConstantCount, &constants, 0);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    commandList->DrawInstanced(2, count, 0, 0);

    if(hashed)
    {
        m_splitPipeline.Bind(commandList);
        commandList->SetGraphicsRoot32BitConstants(0, ConstantCount, &constants, 0);
        commandList->ExecuteIndirect(m_splitSignature.Get(), 1, m_splitArguments.GetResource(), 0, nullptr, 0);
    }

    End(commandList, viewportColor, viewportDepth, previousDepthState);

    if(hashed)
    {
        m_splitArguments.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    return count;
}

std::uint32_t EditorAirPass::ExecuteCells(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, RenderTarget& viewportColor,
    DepthBuffer& viewportDepth, const Matrix4& viewProjection, const AirFieldBinding& field, const AirCellRequest& request) const
{
    // The box of whole cells: the vectors' box at one-cell spacing.
    AirVisualizationRequest box;
    box.spacingExponent = 0;
    box.maximumVectors = request.maximumCells;
    box.focus = request.focus;
    const AirVectorParameters parameters = MakeAirVectorParameters(field, box);

    struct
    {
        Matrix4 viewProjection;
        AirFieldBinding field;
        std::int32_t firstCell[3];
        std::uint32_t showMask;
        std::uint32_t cells[3];
        std::uint32_t padding;
    } constants{viewProjection, field, {parameters.firstBlock[0], parameters.firstBlock[1], parameters.firstBlock[2]},
        (request.blocked ? 1u : 0u) | (request.unknown ? 2u : 0u), {parameters.blocks[0], parameters.blocks[1], parameters.blocks[2]}, 0};
    static_assert(sizeof(constants) == CellConstantCount * sizeof(float));

    const std::uint32_t count = field.valid != 0 && constants.showMask != 0 ? AirVectorCount(parameters) : 0;
    D3D12_RESOURCE_STATES previousDepthState{};

    if(count == 0 || !Begin(commandList, descriptorHeap, viewportColor, viewportDepth, m_cellPipeline, previousDepthState))
    {
        return 0;
    }

    commandList->SetGraphicsRoot32BitConstants(0, CellConstantCount, &constants, 0);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(36, count, 0, 0);
    End(commandList, viewportColor, viewportDepth, previousDepthState);
    return count;
}

std::uint32_t EditorAirPass::ExecutePieces(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, UploadBuffer& upload,
    RenderTarget& viewportColor, DepthBuffer& viewportDepth, const Matrix4& viewProjection, const AirFieldBinding& field,
    const VoxelWorldBinding& voxels, const AirCellRequest& request) const
{
    // The box of sub-cells: the vectors' box at quarter-cell spacing, whose blocks are exactly the sub-cells.
    AirVisualizationRequest box;
    box.spacingExponent = -2;
    box.maximumVectors = request.maximumSubcells;
    box.focus = request.focus;
    const AirVectorParameters parameters = MakeAirVectorParameters(field, box);

    struct
    {
        Matrix4 viewProjection;
        AirFieldBinding field;
        VoxelWorldBinding voxels;
        std::int32_t firstSubcell[3];
        std::int32_t subcellLevel;
        std::uint32_t subcells[3];
        std::uint32_t padding;
    } constants{viewProjection, field, voxels, {parameters.firstBlock[0], parameters.firstBlock[1], parameters.firstBlock[2]}, 0,
        {parameters.blocks[0], parameters.blocks[1], parameters.blocks[2]}, 0};
    static_assert(sizeof(constants) == 288);

    // A cell is 2^cellExponent metres and a voxel 2^baseExponent; a sub-cell is a quarter of a cell.
    constants.subcellLevel = static_cast<std::int32_t>(std::lround(std::log2(field.cellSize))) - 2 - voxels.baseExponent;
    const std::uint32_t count = field.valid != 0 && request.pieces ? AirVectorCount(parameters) : 0;
    D3D12_RESOURCE_STATES previousDepthState{};

    if(count == 0)
    {
        return 0;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS address = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(address == 0 || !Begin(commandList, descriptorHeap, viewportColor, viewportDepth, m_piecePipeline, previousDepthState))
    {
        return 0;
    }

    commandList->SetGraphicsRootConstantBufferView(0, address);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(36, count, 0, 0);
    End(commandList, viewportColor, viewportDepth, previousDepthState);
    return count;
}
