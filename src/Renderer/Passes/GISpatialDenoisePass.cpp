#include "GISpatialDenoisePass.h"

#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/GBuffer.h"
#include "Renderer/UploadBuffer.h"

#include "Renderer/Shader/ShaderLibrary.h"

#include "Core/Log.h"

bool GISpatialDenoisePass::Initialize(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    ShaderDescription shaderDescription{};
    shaderDescription.path = "GISpatialDenoise.hlsl";
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

    if (!CreateTextures(graphicsDevice, descriptorHeap, width, height))
    {
        m_pipeline.Shutdown();
        return false;
    }

    return true;
}

bool GISpatialDenoisePass::CreateTextures(

    GraphicsDevice& graphicsDevice,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    ID3D12Device* device = graphicsDevice.GetDevice();

    if (!m_pingTexture.Initialize(device, width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        Log::Error("Failed to create GI spatial denoise ping texture.");
        return false;
    }

    if (!m_pingTexture.CreateSRV(device, descriptorHeap) || !m_pingTexture.CreateUAV(device, descriptorHeap))
    {
        Log::Error("Failed to create GI spatial denoise ping descriptors.");
        m_pingTexture.Shutdown();
        return false;
    }

    if (!m_pongTexture.Initialize(device, width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        Log::Error("Failed to create GI spatial denoise pong texture.");
        m_pingTexture.Shutdown();
        return false;
    }

    if (!m_pongTexture.CreateSRV(device, descriptorHeap) || !m_pongTexture.CreateUAV(device, descriptorHeap))
    {
        Log::Error("Failed to create GI spatial denoise pong descriptors.");
        m_pongTexture.Shutdown();
        m_pingTexture.Shutdown();
        return false;
    }

    return true;
}

bool GISpatialDenoisePass::Resize(
    GraphicsDevice& graphicsDevice,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    m_pingTexture.Shutdown();
    m_pongTexture.Shutdown();

    return CreateTextures(graphicsDevice, descriptorHeap, width, height);
}

void GISpatialDenoisePass::Execute(
    ID3D12GraphicsCommandList* commandList,
    GBuffer& gBuffer,
    Texture& inputGI,
    UploadBuffer& uploadBuffer,
    unsigned int width,
    unsigned int height)
{
    if (!commandList || width == 0 || height == 0)
    {
        return;
    }

    gBuffer.GetDepth().Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gBuffer.GetSurface().Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    inputGI.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_pingTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    m_pipeline.Bind(commandList);

    const unsigned int groupCountX = (width + 7u) / 8u;
    const unsigned int groupCountY = (height + 7u) / 8u;

    for (unsigned int iteration = 0; iteration < 2; ++iteration)
    {
        Texture& source = iteration == 0 ? inputGI : m_pingTexture;
        Texture& destination = iteration == 0 ? m_pingTexture : m_pongTexture;

        source.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        destination.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        GISpatialDenoiseConstants constants{};
        constants.inputIndex = source.GetDescriptorIndex();
        constants.outputIndex = destination.GetUAVIndex();
        constants.depthIndex = gBuffer.GetDepth().GetSRVIndex();

        constants.normalIndex = gBuffer.GetSurface().GetSRVIndex();
        constants.width = width;
        constants.height = height;
        constants.stepWidth = 1u << iteration;

        constants.depthSigma = 1.0f;
        constants.normalPower = 32.0f;

        const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress =
            uploadBuffer.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if (constantsAddress == 0)
        {
            return;
        }

        commandList->SetComputeRootConstantBufferView(0, constantsAddress);

        commandList->Dispatch(groupCountX, groupCountY, 1);
    }

    m_pongTexture.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void GISpatialDenoisePass::Shutdown()
{
    m_pingTexture.Shutdown();

    m_pongTexture.Shutdown();
    m_pipeline.Shutdown();
}

Texture& GISpatialDenoisePass::GetOutputTexture()
{
    return m_pongTexture;
}

const Texture& GISpatialDenoisePass::GetOutputTexture() const
{
    return m_pongTexture;
}
