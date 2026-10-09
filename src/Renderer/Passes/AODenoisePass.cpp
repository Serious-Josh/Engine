#include "AODenoisePass.h"

#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/GBuffer.h"
#include "Renderer/UploadBuffer.h"

#include "Renderer/Shader/ShaderLibrary.h"

bool AODenoisePass::Initialize(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    ID3D12Device* device = graphicsDevice.GetDevice();

    ShaderDescription shaderDescription{};
    shaderDescription.path = "AODenoise.hlsl";
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

    if(!m_pipeline.Initialize(device, pipelineDescription))
    {
        return false;
    }

    return Resize(
        graphicsDevice,
        descriptorHeap,
        width,
        height);
}

bool AODenoisePass::Resize(
    GraphicsDevice& graphicsDevice,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    ID3D12Device* device = graphicsDevice.GetDevice();

    for(unsigned int i = 0; i < 2; ++i)
    {
        m_filteredAO[i].Shutdown();
    }

    for(unsigned int i = 0; i < 2; ++i)
    {
        Texture& texture = m_filteredAO[i];

        if(!texture.Initialize(device, width, height, 1, 1,
            DXGI_FORMAT_R16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            return false;
        }

        if(!texture.CreateSRV(device, descriptorHeap))
        {
            return false;
        }

        if(!texture.CreateUAV(device, descriptorHeap))
        {
            return false;
        }
    }

    m_outputIndex = 0;

    return true;
}

void AODenoisePass::Execute(
    ID3D12GraphicsCommandList* commandList,
    Texture& accumulatedAO,
    GBuffer& gBuffer,
    UploadBuffer& uploadBuffer,
    const Matrix4& inverseViewProjection,
    unsigned int width,
    unsigned int height)
{
    constexpr D3D12_RESOURCE_STATES shaderReadState =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    accumulatedAO.Transition(commandList, shaderReadState);
    gBuffer.GetDepth().Transition(commandList, shaderReadState);
    gBuffer.GetSurface().Transition(commandList, shaderReadState);

    m_pipeline.Bind(commandList);

    Texture* inputTexture = &accumulatedAO;

    const unsigned int stepWidths[3] = {1u, 2u, 4u};

    for(unsigned int iteration = 0; iteration < 3; ++iteration)
    {
        const unsigned int outputIndex = iteration % 2u;
        Texture& outputTexture = m_filteredAO[outputIndex];

        inputTexture->Transition(commandList, shaderReadState);
        outputTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        AODenoiseConstants constants{};
        constants.inverseViewProjection = inverseViewProjection;
        constants.inputAOIndex = inputTexture->GetDescriptorIndex();
        constants.depthIndex = gBuffer.GetDepth().GetSRVIndex();
        constants.surfaceIndex = gBuffer.GetSurface().GetSRVIndex();
        constants.outputAOIndex = outputTexture.GetUAVIndex();
        constants.width = width;
        constants.height = height;
        constants.stepWidth = stepWidths[iteration];

        const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress = uploadBuffer.Allocate(&constants, sizeof(AODenoiseConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(constantsAddress == 0)
        {
            return;
        }

        commandList->SetComputeRootConstantBufferView(0, constantsAddress);
        commandList->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1u);

        outputTexture.Transition(commandList, shaderReadState);

        inputTexture = &outputTexture;
    }

    m_outputIndex = 0u;
}

void AODenoisePass::Shutdown()
{
    m_pipeline.Shutdown();

    for(unsigned int i = 0; i < 2; ++i)
    {
        m_filteredAO[i].Shutdown();
    }

    m_outputIndex = 0;
}

Texture& AODenoisePass::GetFilteredAOTexture()
{
    return m_filteredAO[m_outputIndex];
}