#include "ReflectionContributionLimitPass.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/UploadBuffer.h"

bool ReflectionContributionLimitPass::Initialize(
    GraphicsDevice& device,
    ShaderLibrary& shaders,
    DescriptorHeap& heap,
    unsigned int width,
    unsigned int height)
{
    ShaderDescription shader{};
    shader.path = "ReflectionContributionLimit.hlsl";
    shader.entryPoint = "CSMain";
    shader.stage = ShaderStage::Compute;

    ComputePipelineDescription pipeline{};
    pipeline.computeShader = shaders.Load(shader);

    if (!pipeline.computeShader)
    {
        return false;
    }

    RootParameterDescription constants{};

    constants.type = RootParameterType::ConstantBuffer;
    constants.shaderRegister = 0;
    constants.visibility = D3D12_SHADER_VISIBILITY_ALL;
    pipeline.rootParameters.push_back(constants);

    if (!m_pipeline.Initialize(device.GetDevice(), pipeline))
    {
        return false;
    }

    if (!CreateTexture(device, heap, width, height))
    {
        Shutdown();
        return false;
    }

    return true;
}

bool ReflectionContributionLimitPass::CreateTexture(

    GraphicsDevice& device,
    DescriptorHeap& heap,
    unsigned int width,
    unsigned int height)
{
    if (!m_output.Initialize(device.GetDevice(), width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        return false;
    }

    if (!m_output.CreateSRV(device.GetDevice(), heap) || !m_output.CreateUAV(device.GetDevice(), heap))
    {
        m_output.Shutdown();
        return false;
    }
    return true;
}

bool ReflectionContributionLimitPass::Resize(

    GraphicsDevice& device,
    DescriptorHeap& heap,
    unsigned int width,
    unsigned int height)
{
    m_output.Shutdown();
    return CreateTexture(device, heap, width, height);
}

bool ReflectionContributionLimitPass::Execute(
    ID3D12GraphicsCommandList* commands,
    Texture& raw,
    UploadBuffer& upload,
    unsigned int width,
    unsigned int height)
{
    if (!commands || width == 0 || height == 0)
    {
        return false;
    }

    struct Constants
    {
        unsigned int input, output, width, height;
    };
    static_assert(sizeof(Constants) == 16);
    const Constants constants{raw.GetDescriptorIndex(), m_output.GetUAVIndex(), width, height};
    const auto address = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if (!address)
    {
        return false;
    }

    raw.Transition(commands, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    m_output.Transition(commands, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_pipeline.Bind(commands);
    commands->SetComputeRootConstantBufferView(0, address);
    commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    m_output.Transition(commands, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return true;
}

void ReflectionContributionLimitPass::Shutdown()
{
    m_output.Shutdown();
    m_pipeline.Shutdown();
}
