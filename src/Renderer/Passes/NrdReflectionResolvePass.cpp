#include "NrdReflectionResolvePass.h"

#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/GBuffer.h"
#include "Renderer/UploadBuffer.h"
#include "Core/Log.h"

bool NrdReflectionResolvePass::Initialize(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    ShaderDescription shaderDescription{};
    shaderDescription.path = "NrdReflectionResolve.hlsl";
    shaderDescription.entryPoint = "CSMain";
    shaderDescription.stage = ShaderStage::Compute;

    Shader* computeShader = shaderLibrary.Load(shaderDescription);

    if(!computeShader)
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

    if(!m_pipeline.Initialize(graphicsDevice.GetDevice(), pipelineDescription))
    {
        return false;
    }

    if(!CreateTexture(graphicsDevice, descriptorHeap, width, height))
    {
        m_pipeline.Shutdown();
        return false;
    }

    return true;
}

bool NrdReflectionResolvePass::Resize(
    GraphicsDevice& graphicsDevice,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    m_outputTexture.Shutdown();

    return CreateTexture(graphicsDevice, descriptorHeap, width, height);
}

void NrdReflectionResolvePass::Execute(
    ID3D12GraphicsCommandList* commandList,
    GBuffer& gBuffer,
    Texture& nrdOutput,
    UploadBuffer& uploadBuffer,
    const Matrix4& inverseViewProjection,
    const Matrix4& view,
    unsigned int materialBufferIndex,
    unsigned int width,
    unsigned int height)
{
    if(!commandList || width == 0 || height == 0)
    {
        return;
    }

    gBuffer.GetDepth().Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gBuffer.GetMaterial().Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gBuffer.GetSurface().Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    nrdOutput.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_outputTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    NrdReflectionResolveConstants constants{};

    constants.inverseViewProjection = inverseViewProjection;
    constants.view = view;
    constants.depthIndex = gBuffer.GetDepth().GetSRVIndex();
    constants.materialIndex = gBuffer.GetMaterial().GetSRVIndex();
    constants.surfaceIndex = gBuffer.GetSurface().GetSRVIndex();
    constants.materialBufferIndex = materialBufferIndex;
    constants.nrdOutputIndex = nrdOutput.GetDescriptorIndex();
    constants.outputIndex = m_outputTexture.GetUAVIndex();
    constants.width = width;
    constants.height = height;

    const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress = uploadBuffer.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(constantsAddress == 0)
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

void NrdReflectionResolvePass::Shutdown()
{
    m_outputTexture.Shutdown();
    m_pipeline.Shutdown();
}

bool NrdReflectionResolvePass::CreateTexture(
    GraphicsDevice& graphicsDevice,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    ID3D12Device* device = graphicsDevice.GetDevice();

    if(!m_outputTexture.Initialize(device, width, height, 1, 1,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        Log::Error(
            "Failed to create NRD resolved reflection texture.");

        return false;
    }

    if(!m_outputTexture.CreateSRV(device, descriptorHeap))
    {
        Log::Error("Failed to create NRD resolved reflection SRV.");

        m_outputTexture.Shutdown();
        return false;
    }

    if(!m_outputTexture.CreateUAV(device, descriptorHeap))
    {
        Log::Error("Failed to create NRD resolved reflection UAV.");

        m_outputTexture.Shutdown();
        return false;
    }

    return true;
}

Texture& NrdReflectionResolvePass::GetOutputTexture()
{
    return m_outputTexture;
}

const Texture& NrdReflectionResolvePass::GetOutputTexture() const
{
    return m_outputTexture;
}