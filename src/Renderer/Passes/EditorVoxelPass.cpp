#include "Renderer/Passes/EditorVoxelPass.h"

#include "Renderer/DepthBuffer.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/Shader/ShaderLibrary.h"

#include <cmath>

namespace
{
struct VoxelDrawConstants
{
    Matrix4 clipFromLattice;
    std::uint32_t regionVoxels = 0;
};

static_assert(sizeof(VoxelDrawConstants) == EditorVoxelPass::ConstantCount * sizeof(float));

constexpr unsigned int VerticesPerCell = 36;
constexpr unsigned int VerticesPerBox = 24;

bool IsFinite(const Matrix4& matrix)
{
    for(const auto& row : matrix.m)
    {
        for(const float value : row)
        {
            if(!std::isfinite(value))
            {
                return false;
            }
        }
    }

    return true;
}

PipelineDescription OverlayPipeline(const Shader* vertexShader, const Shader* pixelShader)
{
    PipelineDescription description;
    description.vertexShader = vertexShader;
    description.pixelShader = pixelShader;
    description.renderTargets.count = 1;
    description.renderTargets.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.rootParameters.push_back({RootParameterType::Constants, 0, EditorVoxelPass::ConstantCount, D3D12_SHADER_VISIBILITY_VERTEX});
    description.rootParameters.push_back({RootParameterType::ShaderResource, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX});
    description.rootParameters.push_back({RootParameterType::ShaderResource, 1, 0, D3D12_SHADER_VISIBILITY_VERTEX});
    description.depthEnabled = true;
    description.depthFormat = DXGI_FORMAT_D32_FLOAT;
    description.depthComparison = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    return description;
}
}

bool EditorVoxelPass::Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary)
{
    const Shader* cellVertex = shaderLibrary.Load({"EditorVoxel.hlsl", "CellVS", ShaderStage::Vertex});
    const Shader* boxVertex = shaderLibrary.Load({"EditorVoxel.hlsl", "BoxVS", ShaderStage::Vertex});
    const Shader* pixel = shaderLibrary.Load({"EditorVoxel.hlsl", "OverlayPS", ShaderStage::Pixel});

    if(!cellVertex || !boxVertex || !pixel)
    {
        return false;
    }

    // Opaque cubes write depth, so nearer cubes hide farther ones whatever the instance order.
    PipelineDescription cell = OverlayPipeline(cellVertex, pixel);
    cell.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;

    // Boxes are lines over everything already drawn; they write no depth.
    PipelineDescription box = OverlayPipeline(boxVertex, pixel);
    box.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    box.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

    return m_cellPipeline.Initialize(device, cell) && m_boxPipeline.Initialize(device, box);
}

void EditorVoxelPass::Shutdown()
{
    // Buffer has no destructor: each one must be shut down to release its resource and descriptor.
    for(Retired& retired : m_retired)
    {
        retired.cells.Shutdown();
        retired.regions.Shutdown();
    }

    m_retired.clear();
    m_cells.Shutdown();
    m_regions.Shutdown();
    m_cellCount = 0;
    m_regionCount = 0;
    m_generation = 0;
    m_cellPipeline.Shutdown();
    m_boxPipeline.Shutdown();
}

std::uint64_t EditorVoxelPass::Publish(ID3D12Device* device, ResourceUploader& uploader, const EditorVoxelOverlay::Data& data,
    std::uint64_t lastSubmissionFence, std::string* error)
{
    const auto fail = [&](const char* message) -> std::uint64_t
    {
        if(error)
        {
            *error = message;
        }

        return 0;
    };

    if(data.cells.size() > EditorVoxelOverlay::MaximumCells || data.regions.size() > EditorVoxelOverlay::MaximumRegions)
    {
        return fail("The voxel overlay exceeds its cell or region limit.");
    }

    if(data.regions.empty() || data.regionVoxels == 0)
    {
        return fail("The voxel overlay has no regions.");
    }

    // Build the replacement completely before touching the current buffers: a failed upload leaves them current.
    Buffer cells;
    Buffer regions;
    const D3D12_RESOURCE_STATES readState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const std::size_t cellBytes = data.cells.size() * sizeof(EditorVoxelOverlay::Cell);
    const std::size_t regionBytes = data.regions.size() * sizeof(EditorVoxelOverlay::Region);

    if(!data.cells.empty() && !cells.Initialize(device, uploader, data.cells.data(), cellBytes, readState))
    {
        return fail("Uploading the voxel overlay cubes failed.");
    }

    if(!regions.Initialize(device, uploader, data.regions.data(), regionBytes, readState))
    {
        // Never read by any frame: released at once.
        cells.Shutdown();
        return fail("Uploading the voxel overlay regions failed.");
    }

    RetireCurrent(lastSubmissionFence);
    m_cells = std::move(cells);
    m_regions = std::move(regions);
    m_cellCount = static_cast<std::uint32_t>(data.cells.size());
    m_regionCount = static_cast<std::uint32_t>(data.regions.size());
    m_regionVoxels = data.regionVoxels;
    m_generation = m_nextGeneration++;
    return m_generation;
}

void EditorVoxelPass::Clear(std::uint64_t lastSubmissionFence)
{
    RetireCurrent(lastSubmissionFence);
    m_cellCount = 0;
    m_regionCount = 0;
    m_generation = 0;
}

void EditorVoxelPass::RetireCurrent(std::uint64_t lastSubmissionFence)
{
    if(!m_cells.GetResource() && !m_regions.GetResource())
    {
        return;
    }

    // No recording is open, so every frame that could read these buffers was submitted at or before lastSubmissionFence.
    Retired retired;
    retired.cells = std::move(m_cells);
    retired.regions = std::move(m_regions);
    retired.fence = lastSubmissionFence;
    m_retired.push_back(std::move(retired));
}

void EditorVoxelPass::CollectRetired(std::uint64_t completedFence)
{
    // Fences are queued in submission order, so the front always completes first.
    while(!m_retired.empty() && m_retired.front().fence <= completedFence)
    {
        // Buffer has no destructor: shut down to release the resource and its descriptor.
        m_retired.front().cells.Shutdown();
        m_retired.front().regions.Shutdown();
        m_retired.pop_front();
    }
}

void EditorVoxelPass::Execute(ID3D12GraphicsCommandList* commandList, RenderTarget& viewportColor, DepthBuffer& viewportDepth,
    const Matrix4& viewProjection, const EditorVoxelOverlay::Request& request) const
{
    const bool current = request.generation != 0 && request.generation == m_generation && m_regionCount > 0;
    const bool initialized = m_cellPipeline.IsInitialized() && m_boxPipeline.IsInitialized();
    const bool matchingTargets = viewportColor.GetWidth() == viewportDepth.GetWidth() &&
        viewportColor.GetHeight() == viewportDepth.GetHeight();

    if(!current || !initialized || !matchingTargets)
    {
        return;
    }

    VoxelDrawConstants constants;
    constants.clipFromLattice = viewProjection * request.worldFromLattice;
    constants.regionVoxels = m_regionVoxels;

    if(!IsFinite(constants.clipFromLattice))
    {
        return;
    }

    const D3D12_RESOURCE_STATES previousDepthState = viewportDepth.GetState();
    viewportDepth.Transition(commandList, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const D3D12_CPU_DESCRIPTOR_HANDLE colorTarget = viewportColor.GetRTV();
    const D3D12_CPU_DESCRIPTOR_HANDLE depthTarget = viewportDepth.GetDSV();
    commandList->OMSetRenderTargets(1, &colorTarget, FALSE, &depthTarget);

    const auto bind = [&](const Pipeline& pipeline)
    {
        pipeline.Bind(commandList);
        commandList->SetGraphicsRoot32BitConstants(0, ConstantCount, &constants, 0);
        // Without cubes the cell slot is never read; the region buffer stands in so the root SRV is a valid address.
        commandList->SetGraphicsRootShaderResourceView(1, m_cells.GetResource() ? m_cells.GetGPUAddress() : m_regions.GetGPUAddress());
        commandList->SetGraphicsRootShaderResourceView(2, m_regions.GetGPUAddress());
    };

    if(m_cellCount > 0)
    {
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        bind(m_cellPipeline);
        commandList->DrawInstanced(VerticesPerCell, m_cellCount, 0, 0);
    }

    if(request.drawBoxes)
    {
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        bind(m_boxPipeline);
        commandList->DrawInstanced(VerticesPerBox, m_regionCount, 0, 0);
    }

    // Unbind the depth before restoring the tracked state for later consumers.
    commandList->OMSetRenderTargets(1, &colorTarget, FALSE, nullptr);
    viewportDepth.Transition(commandList, previousDepthState);
}
