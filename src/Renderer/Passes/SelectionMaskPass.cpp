#include "SelectionMaskPass.h"

#include "Core/Log.h"
#include <cassert>

#include "Renderer/Shader/ShaderLibrary.h"

bool SelectionMaskPass::Initialize(
    ID3D12Device* device,
    ShaderLibrary& shaderLibrary,
    const VertexLayout& vertexLayout)
{
    if (IsInitialized() || !device)
    {
        return false;
    }

    std::vector<LayoutAttribute> layout;

    for (const auto& attribute : vertexLayout.GetAttributes())
    {
        if (!attribute.semantic)
        {
            return false;
        }

        layout.push_back({attribute.semantic, attribute.semanticIndex, attribute.type, attribute.offset});
    }

    ShaderDescription vertexDescription{};
    vertexDescription.path = "SelectionMask.hlsl";
    vertexDescription.entryPoint = "VSMain";
    vertexDescription.stage = ShaderStage::Vertex;

    Shader* vertexShader = shaderLibrary.Load(vertexDescription);

    if (!vertexShader)
    {
        Log::Error("Failed to load selection mask vertex shader.");
        return false;
    }

    ShaderDescription pixelDescription{};

    pixelDescription.path = "SelectionMask.hlsl";
    pixelDescription.entryPoint = "PSMain";
    pixelDescription.stage = ShaderStage::Pixel;

    Shader* pixelShader = shaderLibrary.Load(pixelDescription);

    if (!pixelShader)
    {
        Log::Error("Failed to load selection mask pixel shader.");
        return false;
    }

    PipelineDescription description{};

    description.vertexShader = vertexShader;
    description.pixelShader = pixelShader;
    description.vertexLayout = &vertexLayout;

    description.renderTargets.count = 1;
    description.renderTargets.formats[0] = DXGI_FORMAT_R8_UNORM;

    description.depthEnabled = true;
    description.mirroredWindingVariant = true;
    description.depthFormat = DXGI_FORMAT_D32_FLOAT;
    description.depthComparison = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    description.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

    RootParameterDescription cameraParameter{};
    cameraParameter.type = RootParameterType::ConstantBuffer;
    cameraParameter.shaderRegister = 1;
    cameraParameter.visibility = D3D12_SHADER_VISIBILITY_VERTEX;
    description.rootParameters.push_back(cameraParameter);

    RootParameterDescription objectParameter{};
    objectParameter.type = RootParameterType::ConstantBuffer;
    objectParameter.shaderRegister = 2;
    objectParameter.visibility = D3D12_SHADER_VISIBILITY_VERTEX;
    description.rootParameters.push_back(objectParameter);

    if (!m_pipeline.Initialize(device, description))
    {
        Log::Error("Failed to initialize selection mask pipeline.");
        return false;
    }

    // The overlay depth prepass variant: nearest surface wins and is written.
    description.depthComparison = D3D12_COMPARISON_FUNC_LESS;
    description.depthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;

    if (!m_depthPrepassPipeline.Initialize(device, description))
    {
        Log::Error("Failed to initialize the Editor overlay depth prepass pipeline.");
        m_pipeline.Shutdown();
        return false;
    }

    m_layout.swap(layout);
    m_stride = vertexLayout.GetStride();
    return true;
}

void SelectionMaskPass::Bind(ID3D12GraphicsCommandList* commandList, bool mirroredWinding) const
{
    m_pipeline.Bind(commandList, mirroredWinding);
}

void SelectionMaskPass::BindDepthPrepass(ID3D12GraphicsCommandList* commandList, bool mirroredWinding) const
{
    m_depthPrepassPipeline.Bind(commandList, mirroredWinding);
}

void SelectionMaskPass::Shutdown()
{
    m_pipeline.Shutdown();
    m_depthPrepassPipeline.Shutdown();
    m_layout.clear();
    m_stride = 0;
}

bool SelectionMaskPass::IsCompatible(const VertexLayout& layout) const noexcept
{
    if (!IsInitialized() || m_stride != layout.GetStride() || m_layout.size() != layout.GetAttributes().size())
    {
        return false;
    }

    for (size_t i = 0; i < m_layout.size(); ++i)
    {
        const auto& expected = m_layout[i];
        const auto& actual = layout.GetAttributes()[i];

        if (!actual.semantic || expected.semantic != actual.semantic || expected.index != actual.semanticIndex ||
            expected.type != actual.type || expected.offset != actual.offset)
        {
            return false;
        }
    }

    return true;
}

void SelectionMaskPass::AdoptPrepared(SelectionMaskPass& candidate) noexcept
{
    assert(!IsInitialized() && candidate.IsInitialized());
    m_pipeline.Swap(candidate.m_pipeline);
    m_depthPrepassPipeline.Swap(candidate.m_depthPrepassPipeline);
    m_layout.swap(candidate.m_layout);
    std::swap(m_stride, candidate.m_stride);
}
