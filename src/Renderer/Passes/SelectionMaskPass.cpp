#include "SelectionMaskPass.h"

#include "Core/Log.h"

#include "Renderer/Shader/ShaderLibrary.h"

bool SelectionMaskPass::Initialize(
    ID3D12Device* device,
    ShaderLibrary& shaderLibrary,
    const VertexLayout& vertexLayout)
{
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

    return true;
}

void SelectionMaskPass::Bind(ID3D12GraphicsCommandList* commandList) const
{
    m_pipeline.Bind(commandList);
}

void SelectionMaskPass::Shutdown()
{
    m_pipeline.Shutdown();
}
