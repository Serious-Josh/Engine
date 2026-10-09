#include "AOTemporalPass.h"

#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/GBuffer.h"
#include "Renderer/UploadBuffer.h"

#include "Renderer/Shader/ShaderLibrary.h"

bool AOTemporalPass::Initialize(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    ID3D12Device* device = graphicsDevice.GetDevice();

    ShaderDescription shaderDescription{};
    shaderDescription.path = "AOTemporal.hlsl";
    shaderDescription.entryPoint = "CSMain";
    shaderDescription.stage = ShaderStage::Compute;

    Shader* computeShader = shaderLibrary.Load(shaderDescription);

    if(!computeShader){
        return false;
    }

    ComputePipelineDescription pipelineDescription{};
    pipelineDescription.computeShader = computeShader;

    RootParameterDescription constants{};
    constants.type = RootParameterType::ConstantBuffer;
    constants.shaderRegister = 0;
    constants.visibility = D3D12_SHADER_VISIBILITY_ALL;

    pipelineDescription.rootParameters.push_back(constants);

    if(!m_pipeline.Initialize(device, pipelineDescription)){
        return false;
    }

    return Resize(
        graphicsDevice,
        descriptorHeap,
        width,
        height);
}
bool AOTemporalPass::Resize(
    GraphicsDevice& graphicsDevice,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    ID3D12Device* device = graphicsDevice.GetDevice();

    for(unsigned int i = 0; i < 2; ++i)
    {
        m_accumulatedAO[i].Shutdown();
        m_historyDepth[i].Shutdown();
        m_historyLength[i].Shutdown();
    }

    auto createTexture = [&](Texture& texture, DXGI_FORMAT format) -> bool
    {
        if(!texture.Initialize(
            device,
            width,
            height,
            1,
            1,
            format,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            return false;
        }

        if(!texture.CreateSRV(device, descriptorHeap))
            return false;

        if(!texture.CreateUAV(device, descriptorHeap))
            return false;

        return true;
    };

    for(unsigned int i = 0; i < 2; ++i)
    {
        if(!createTexture(m_accumulatedAO[i], DXGI_FORMAT_R16_FLOAT) ||
        !createTexture(m_historyDepth[i], DXGI_FORMAT_R32_FLOAT) ||
        !createTexture(m_historyLength[i], DXGI_FORMAT_R16_FLOAT))
        {
            return false;
        }
    }

    m_historyIndex = 0;
    m_accumulatedFrameCount = 0;

    return true;
}

void AOTemporalPass::Shutdown()
{
    m_pipeline.Shutdown();

    for(unsigned int i = 0; i < 2; ++i)
    {
        m_accumulatedAO[i].Shutdown();
        m_historyDepth[i].Shutdown();
        m_historyLength[i].Shutdown();
    }

    m_historyIndex = 0;
    m_accumulatedFrameCount = 0;
}

void AOTemporalPass::Execute(
    ID3D12GraphicsCommandList* commandList,
    Texture& rawAO,
    GBuffer& gBuffer,
    UploadBuffer& uploadBuffer,
    const Matrix4& inverseViewProjection,
    const Matrix4& previousViewProjection,
    const Matrix4& previousInverseViewProjection,
    unsigned int width,
    unsigned int height)
{
    const unsigned int writeIndex = 1u - m_historyIndex;
    const unsigned int readIndex = m_historyIndex;

    Texture& outputAO = m_accumulatedAO[writeIndex];
    Texture& outputDepth = m_historyDepth[writeIndex];
    Texture& outputHistoryLength = m_historyLength[writeIndex];

    rawAO.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gBuffer.GetDepth().Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gBuffer.GetSurface().Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if(m_accumulatedFrameCount > 0)
    {
        m_accumulatedAO[readIndex].Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_historyDepth[readIndex].Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_historyLength[readIndex].Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    outputAO.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    outputDepth.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    outputHistoryLength.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    AOTemporalConstants constants{};
    constants.inverseViewProjection = inverseViewProjection;
    constants.previousViewProjection = previousViewProjection;
    constants.previousInverseViewProjection = previousInverseViewProjection;

    constants.rawAOIndex = rawAO.GetDescriptorIndex();
    constants.depthIndex = gBuffer.GetDepth().GetSRVIndex();
    constants.surfaceIndex = gBuffer.GetSurface().GetSRVIndex();

    constants.previousAOIndex = m_accumulatedAO[readIndex].GetDescriptorIndex();
    constants.previousDepthIndex = m_historyDepth[readIndex].GetDescriptorIndex();
    constants.previousHistoryLengthIndex = m_historyLength[readIndex].GetDescriptorIndex();

    constants.outputAOIndex = outputAO.GetUAVIndex();
    constants.outputDepthIndex = outputDepth.GetUAVIndex();
    constants.outputHistoryLengthIndex = outputHistoryLength.GetUAVIndex();

    constants.width = width;
    constants.height = height;
    constants.accumulatedFrameCount = m_accumulatedFrameCount;

    const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress = uploadBuffer.Allocate(&constants, sizeof(AOTemporalConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(constantsAddress == 0)
    {
        return;
    }

    m_pipeline.Bind(commandList);

    commandList->SetComputeRootConstantBufferView(0, constantsAddress);

    commandList->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1);

    outputAO.Transition(
        commandList,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    outputDepth.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    outputHistoryLength.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    m_historyIndex = writeIndex;

    if(m_accumulatedFrameCount < 20u)
    {
        ++m_accumulatedFrameCount;
    }
}

Texture& AOTemporalPass::GetAccumulatedAOTexture()
{
    return m_accumulatedAO[m_historyIndex];
}

void AOTemporalPass::ResetHistory()
{
    m_historyIndex = 0;
    m_accumulatedFrameCount = 0;
}