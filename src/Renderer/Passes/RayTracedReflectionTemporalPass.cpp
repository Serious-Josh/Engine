#include "RayTracedReflectionTemporalPass.h"

#include "../DescriptorHeap.h"
#include "../GraphicsDevice.h"
#include "../Shader/ShaderLibrary.h"
#include "../GBuffer.h"
#include "Core/Log.h"

bool RayTracedReflectionTemporalPass::Initialize(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    if (width == 0 || height == 0)
    {
        return false;
    }

    ID3D12Device* device = graphicsDevice.GetDevice();

    ShaderDescription shaderDescription{};
    shaderDescription.path = "RayTracedReflectionTemporal.hlsl";
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

    if (!m_pipeline.Initialize(device, pipelineDescription))
    {
        return false;
    }

    for (unsigned int i = 0; i < 2; ++i)
    {
        if (!m_historyTextures[i].Initialize(device, width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            return false;
        }

        if (!m_historyTextures[i].CreateSRV(device, descriptorHeap))
        {
            return false;
        }

        if (!m_historyTextures[i].CreateUAV(device, descriptorHeap))
        {
            return false;
        }

        if (!m_depthHistoryTextures[i].Initialize(device, width, height, 1, 1, DXGI_FORMAT_R32_FLOAT,
                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            return false;
        }

        if (!m_depthHistoryTextures[i].CreateSRV(device, descriptorHeap))
        {
            return false;
        }

        if (!m_depthHistoryTextures[i].CreateUAV(device, descriptorHeap))
        {
            return false;
        }

        if (!m_historyLengthTextures[i].Initialize(device, width, height, 1, 1, DXGI_FORMAT_R32_FLOAT,
                                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            return false;
        }

        if (!m_historyLengthTextures[i].CreateSRV(device, descriptorHeap))
        {

            return false;
        }

        if (!m_historyLengthTextures[i].CreateUAV(device, descriptorHeap))
        {
            return false;
        }
    }

    m_historyReadIndex = 0;
    m_accumulatedFrameCount = 0;

    return true;
}

bool RayTracedReflectionTemporalPass::Resize(

    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    if (!device || width == 0 || height == 0)
    {
        return false;
    }

    for (Texture& texture : m_historyTextures)
    {
        texture.Shutdown();
    }

    for (Texture& texture : m_depthHistoryTextures)
    {
        texture.Shutdown();
    }

    for (Texture& texture : m_historyLengthTextures)
    {
        texture.Shutdown();
    }

    for (unsigned int i = 0; i < 2; ++i)
    {
        if (!m_historyTextures[i].Initialize(device, width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            return false;
        }

        if (!m_historyTextures[i].CreateSRV(device, descriptorHeap) ||
            !m_historyTextures[i].CreateUAV(device, descriptorHeap))
        {
            return false;
        }

        if (!m_depthHistoryTextures[i].Initialize(device, width, height, 1, 1, DXGI_FORMAT_R32_FLOAT,
                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            return false;
        }

        if (!m_depthHistoryTextures[i].CreateSRV(device, descriptorHeap) ||
            !m_depthHistoryTextures[i].CreateUAV(device, descriptorHeap))
        {
            return false;
        }

        if (!m_historyLengthTextures[i].Initialize(device, width, height, 1, 1, DXGI_FORMAT_R32_FLOAT,
                                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            return false;
        }

        if (!m_historyLengthTextures[i].CreateSRV(device, descriptorHeap) ||
            !m_historyLengthTextures[i].CreateUAV(device, descriptorHeap))
        {

            return false;
        }
    }

    m_historyReadIndex = 0;
    m_accumulatedFrameCount = 0;

    return true;
}

void RayTracedReflectionTemporalPass::Shutdown()
{
    m_pipeline.Shutdown();

    for (Texture& texture : m_historyTextures)
    {
        texture.Shutdown();
    }

    for (Texture& texture : m_depthHistoryTextures)
    {
        texture.Shutdown();
    }

    for (Texture& texture : m_historyLengthTextures)
    {
        texture.Shutdown();
    }

    m_historyReadIndex = 0;
    m_accumulatedFrameCount = 0;
}

void RayTracedReflectionTemporalPass::Execute(

    ID3D12GraphicsCommandList* commandList,
    Texture& rawReflection,
    GBuffer& gBuffer,
    UploadBuffer& uploadBuffer,
    const Matrix4& inverseViewProjection,
    const Matrix4& previousViewProjection,
    const Matrix4& previousInverseViewProjection,
    unsigned int width,
    unsigned int height)
{
    if (!commandList || width == 0 || height == 0)
    {
        return;
    }

    const unsigned int writeIndex = 1u - m_historyReadIndex;

    Texture& historyRead = m_historyTextures[m_historyReadIndex];
    Texture& historyWrite = m_historyTextures[writeIndex];
    Texture& depthHistoryRead = m_depthHistoryTextures[m_historyReadIndex];

    Texture& depthHistoryWrite = m_depthHistoryTextures[writeIndex];
    Texture& historyLengthRead = m_historyLengthTextures[m_historyReadIndex];
    Texture& historyLengthWrite = m_historyLengthTextures[writeIndex];

    rawReflection.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    historyRead.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    historyWrite.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    depthHistoryRead.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    depthHistoryWrite.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    historyLengthRead.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    historyLengthWrite.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ReflectionTemporalConstants constants{};

    constants.inverseViewProjection = inverseViewProjection;
    constants.previousViewProjection = previousViewProjection;
    constants.previousInverseViewProjection = previousInverseViewProjection;
    constants.rawReflectionIndex = rawReflection.GetDescriptorIndex();

    constants.depthIndex = gBuffer.GetDepth().GetSRVIndex();
    constants.surfaceIndex = gBuffer.GetSurface().GetSRVIndex();
    constants.historyIndex = historyRead.GetDescriptorIndex();
    constants.previousDepthIndex = depthHistoryRead.GetDescriptorIndex();

    constants.outputDepthIndex = depthHistoryWrite.GetUAVIndex();
    constants.previousHistoryLengthIndex = historyLengthRead.GetDescriptorIndex();
    constants.outputHistoryLengthIndex = historyLengthWrite.GetUAVIndex();
    constants.outputReflectionIndex = historyWrite.GetUAVIndex();

    constants.width = width;
    constants.height = height;
    constants.accumulatedFrameCount = m_accumulatedFrameCount;

    const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress =
        uploadBuffer.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if (constantsAddress == 0)
    {
        return;
    }

    const unsigned int groupCountX = (width + 7) / 8;

    const unsigned int groupCountY = (height + 7) / 8;

    m_pipeline.Bind(commandList);

    commandList->SetComputeRootConstantBufferView(0, constantsAddress);

    commandList->Dispatch(groupCountX, groupCountY, 1);

    historyWrite.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    m_historyReadIndex = writeIndex;

    ++m_accumulatedFrameCount;
}

Texture& RayTracedReflectionTemporalPass::GetReflectionTexture()
{
    return m_historyTextures[m_historyReadIndex];
}

const Texture& RayTracedReflectionTemporalPass::GetReflectionTexture() const
{
    return m_historyTextures[m_historyReadIndex];
}

unsigned int RayTracedReflectionTemporalPass::GetAccumulatedFrameCount() const
{
    return m_accumulatedFrameCount;
}

void RayTracedReflectionTemporalPass::ResetHistory()
{
    m_accumulatedFrameCount = 0;
}
