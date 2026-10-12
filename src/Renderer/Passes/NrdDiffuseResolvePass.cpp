#include "NrdDiffuseResolvePass.h"

#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/UploadBuffer.h"
#include "Renderer/GBuffer.h"

#include "Renderer/Shader/ShaderLibrary.h"

#include "Core/Log.h"

bool NrdDiffuseResolvePass::Initialize(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    ShaderDescription shaderDescription{};
    shaderDescription.path = "NrdDiffuseResolve.hlsl";
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

    if (!m_pipeline.Initialize(graphicsDevice.GetDevice(), pipelineDescription))
    {
        return false;
    }

    if (!CreateTexture(graphicsDevice, descriptorHeap, width, height))
    {
        m_pipeline.Shutdown();
        return false;
    }

    return true;
}

bool NrdDiffuseResolvePass::Resize(

    GraphicsDevice& graphicsDevice,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    m_outputTexture.Shutdown();

    return CreateTexture(graphicsDevice, descriptorHeap, width, height);
}

void NrdDiffuseResolvePass::Execute(
    ID3D12GraphicsCommandList* commandList,
    GBuffer& gBuffer,
    Texture& nrdOutput,
    Texture& viewZ,
    UploadBuffer& uploadBuffer,
    unsigned int width,
    unsigned int height,
    float denoisingRange)
{
    if (!commandList || width == 0 || height == 0)
    {
        return;
    }

    gBuffer.GetDepth().Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    gBuffer.GetSurface().Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    nrdOutput.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    viewZ.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    m_outputTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    NrdDiffuseResolveConstants constants{};
    constants.nrdOutputIndex = nrdOutput.GetDescriptorIndex();
    constants.outputIndex = m_outputTexture.GetUAVIndex();
    constants.width = width;

    constants.height = height;
    constants.viewZIndex = viewZ.GetDescriptorIndex();
    constants.denoisingRange = denoisingRange;
    constants.depthIndex = gBuffer.GetDepth().GetSRVIndex();

    constants.normalIndex = gBuffer.GetSurface().GetSRVIndex();

    const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress =
        uploadBuffer.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if (constantsAddress == 0)
    {
        return;
    }

    m_pipeline.Bind(commandList);

    commandList->SetComputeRootConstantBufferView(0, constantsAddress);

    const unsigned int groupCountX = (width + 7) / 8;
    const unsigned int groupCountY = (height + 7) / 8;

    commandList->Dispatch(groupCountX, groupCountY, 1);

    m_outputTexture.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void NrdDiffuseResolvePass::Shutdown()
{
    m_outputTexture.Shutdown();
    m_pipeline.Shutdown();
}

bool NrdDiffuseResolvePass::CreateTexture(
    GraphicsDevice& graphicsDevice,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    ID3D12Device* device = graphicsDevice.GetDevice();

    if (!m_outputTexture.Initialize(device, width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        Log::Error("Failed to create NRD resolved diffuse GI texture.");
        return false;
    }

    if (!m_outputTexture.CreateSRV(device, descriptorHeap))
    {
        Log::Error("Failed to create NRD resolved diffuse GI SRV.");

        m_outputTexture.Shutdown();
        return false;
    }

    if (!m_outputTexture.CreateUAV(device, descriptorHeap))
    {
        Log::Error("Failed to create NRD resolved diffuse GI UAV.");

        m_outputTexture.Shutdown();
        return false;
    }

    return true;
}

Texture& NrdDiffuseResolvePass::GetOutputTexture()
{
    return m_outputTexture;
}

const Texture& NrdDiffuseResolvePass::GetOutputTexture() const
{
    return m_outputTexture;
}
