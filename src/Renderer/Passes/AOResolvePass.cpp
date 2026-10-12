#include "AOResolvePass.h"

#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/UploadBuffer.h"

#include "Renderer/Shader/ShaderLibrary.h"

#include "Core/Log.h"

bool AOResolvePass::Initialize(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    ShaderDescription shaderDescription{};
    shaderDescription.path = "AOResolve.hlsl";
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

bool AOResolvePass::CreateTexture(

    GraphicsDevice& graphicsDevice,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    ID3D12Device* device = graphicsDevice.GetDevice();

    if (!m_outputTexture.Initialize(device, width, height, 1, 1, DXGI_FORMAT_R16_FLOAT,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        Log::Error("Failed to create AO resolve texture.");
        return false;
    }

    if (!m_outputTexture.CreateSRV(device, descriptorHeap))
    {
        Log::Error("Failed to create AO resolve SRV.");
        m_outputTexture.Shutdown();
        return false;
    }

    if (!m_outputTexture.CreateUAV(device, descriptorHeap))
    {

        Log::Error("Failed to create AO resolve UAV.");
        m_outputTexture.Shutdown();
        return false;
    }

    return true;
}

bool AOResolvePass::Resize(
    GraphicsDevice& graphicsDevice,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    m_outputTexture.Shutdown();

    return CreateTexture(graphicsDevice, descriptorHeap, width, height);
}

void AOResolvePass::Execute(
    ID3D12GraphicsCommandList* commandList,
    Texture& filteredAO,
    DepthBuffer& depthTexture,
    RenderTarget& surfaceTexture,
    UploadBuffer& uploadBuffer,
    unsigned int width,
    unsigned int height)
{
    if (!commandList || width == 0 || height == 0)
    {
        return;
    }

    filteredAO.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    depthTexture.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    surfaceTexture.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_outputTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    AOResolveConstants constants{};
    constants.inputAOIndex = filteredAO.GetDescriptorIndex();
    constants.depthIndex = depthTexture.GetSRVIndex();
    constants.surfaceIndex = surfaceTexture.GetSRVIndex();

    constants.outputAOIndex = m_outputTexture.GetUAVIndex();
    constants.width = width;
    constants.height = height;

    const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress =
        uploadBuffer.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if (constantsAddress == 0)
    {
        return;
    }

    m_pipeline.Bind(commandList);

    commandList->SetComputeRootConstantBufferView(0, constantsAddress);

    commandList->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1u);

    m_outputTexture.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void AOResolvePass::Shutdown()
{
    m_outputTexture.Shutdown();
    m_pipeline.Shutdown();
}

Texture& AOResolvePass::GetOutputTexture()
{
    return m_outputTexture;
}

const Texture& AOResolvePass::GetOutputTexture() const
{
    return m_outputTexture;
}
