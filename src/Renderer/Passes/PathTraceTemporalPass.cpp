#include "PathTraceTemporalPass.h"

#include "Core/Log.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/UploadBuffer.h"

#include "Renderer/Shader/ShaderLibrary.h"

bool PathTraceTemporalPass::Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary)
{
    if(m_initialized)
    {
        return true;
    }

    if(m_initializationFailed)
    {
        return false;
    }

    ShaderDescription shaderDescription{};
    shaderDescription.path = "PathTraceTemporal.hlsl";
    shaderDescription.entryPoint = "CSMain";
    shaderDescription.stage = ShaderStage::Compute;
    Shader* computeShader = shaderLibrary.Load(shaderDescription);

    ComputePipelineDescription pipelineDescription{};
    pipelineDescription.computeShader = computeShader;

    RootParameterDescription constants{};
    constants.type = RootParameterType::ConstantBuffer;
    constants.shaderRegister = 0;
    constants.visibility = D3D12_SHADER_VISIBILITY_ALL;
    pipelineDescription.rootParameters.push_back(constants);

    // SHARC.6A: the frame's SHARC query binding for the history prior (b1).
    RootParameterDescription priorCache{};
    priorCache.type = RootParameterType::ConstantBuffer;
    priorCache.shaderRegister = 1;
    priorCache.visibility = D3D12_SHADER_VISIBILITY_ALL;
    pipelineDescription.rootParameters.push_back(priorCache);

    if(!computeShader || !m_pipeline.Initialize(graphicsDevice.GetDevice(), pipelineDescription))
    {
        Log::Error("The path tracing temporal pass could not be created.");
        m_initializationFailed = true;
        return false;
    }

    m_initialized = true;
    return true;
}

bool PathTraceTemporalPass::EnsureHistory(
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    if(m_diffuse[0].GetResource() && m_width == width && m_height == height)
    {
        return true;
    }

    ReleaseHistory();

    constexpr D3D12_RESOURCE_FLAGS unorderedAccess = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    constexpr D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const wchar_t* names[6] = {L"Path Trace Temporal Diffuse 0", L"Path Trace Temporal Diffuse 1",
        L"Path Trace Temporal Specular 0", L"Path Trace Temporal Specular 1", L"Path Trace Temporal Moments 0",
        L"Path Trace Temporal Moments 1"};
    Texture* textures[6] = {&m_diffuse[0], &m_diffuse[1], &m_specular[0], &m_specular[1], &m_moments[0], &m_moments[1]};

    for(int index = 0; index < 6; ++index)
    {
        Texture& texture = *textures[index];
        // Second moments in fp32: luminance squared spans far more range than fp16 holds (RTXPT D.2C.3).
        const DXGI_FORMAT format = index < 4 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32G32_FLOAT;

        if(!texture.Initialize(device, width, height, 1, 1, format, unorderedAccess, initialState) ||
            !texture.CreateSRV(device, descriptorHeap) || !texture.CreateUAV(device, descriptorHeap))
        {
            Log::Error("The path tracing temporal history could not be created.");
            ReleaseHistory();
            return false;
        }

        texture.GetResource()->SetName(names[index]);
    }

    m_width = width;
    m_height = height;
    return true;
}

bool PathTraceTemporalPass::EnsureExtended(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    if(m_extended.GetResource())
    {
        return true;
    }

    if(!m_diffuse[0].GetResource() || !m_extended.Initialize(device, m_width, m_height, 1, 1,
        DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
        !m_extended.CreateSRV(device, descriptorHeap) || !m_extended.CreateUAV(device, descriptorHeap))
    {
        Log::Error("The path tracing extended diffuse history could not be created.");
        m_extended.Shutdown();
        return false;
    }

    m_extended.GetResource()->SetName(L"Path Trace Extended Diffuse History");
    return true;
}

void PathTraceTemporalPass::ReleaseHistory()
{
    for(int slot = 0; slot < 2; ++slot)
    {
        m_diffuse[slot].Shutdown();
        m_specular[slot].Shutdown();
        m_moments[slot].Shutdown();
    }

    m_extended.Shutdown();

    m_width = 0;
    m_height = 0;
    InvalidateHistory();
}

void PathTraceTemporalPass::Shutdown()
{
    ReleaseHistory();
    m_pipeline.Shutdown();
    m_initialized = false;
    m_initializationFailed = false;
}

void PathTraceTemporalPass::InvalidateHistory()
{
    m_historyValid = false;
    m_pendingSlot.reset();
}

void PathTraceTemporalPass::Execute(
    ID3D12GraphicsCommandList* commandList,
    UploadBuffer& uploadBuffer,
    PathTraceTemporalConstants constants,
    unsigned int width,
    unsigned int height,
    D3D12_GPU_VIRTUAL_ADDRESS priorCacheBinding,
    bool extended)
{
    if(!m_initialized || !commandList || !m_diffuse[0].GetResource() || width != m_width || height != m_height)
    {
        return;
    }

    // Read the committed slot, write the other: the committed history survives until this frame is submitted.
    const unsigned int readSlot = m_committedSlot;
    const unsigned int writeSlot = 1u - m_committedSlot;
    constexpr D3D12_RESOURCE_STATES shaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    m_diffuse[readSlot].Transition(commandList, shaderRead);
    m_specular[readSlot].Transition(commandList, shaderRead);
    m_moments[readSlot].Transition(commandList, shaderRead);
    m_diffuse[writeSlot].Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_specular[writeSlot].Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_moments[writeSlot].Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    constants.historyValid = m_historyValid ? 1u : 0u;
    constants.historyDiffuseIndex = m_diffuse[readSlot].GetDescriptorIndex();
    constants.historySpecularIndex = m_specular[readSlot].GetDescriptorIndex();
    constants.outputDiffuseIndex = m_diffuse[writeSlot].GetUAVIndex();
    constants.outputSpecularIndex = m_specular[writeSlot].GetUAVIndex();
    constants.historyMomentsIndex = m_moments[readSlot].GetDescriptorIndex();
    constants.outputMomentsIndex = m_moments[writeSlot].GetUAVIndex();
    constants.resolution[0] = width;
    constants.resolution[1] = height;
    extended = extended && m_extended.GetResource();
    constants.extendedIndex = extended ? m_extended.GetUAVIndex() : 0u;

    if(extended)
    {
        m_extended.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress = uploadBuffer.Allocate(&constants,
        sizeof(PathTraceTemporalConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(constantsAddress == 0)
    {
        Log::Error("Failed to allocate path tracing temporal constants.");
        return;
    }

    m_pipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, constantsAddress);
    // Without a prior the shader never reads b1; the constants stand in for it.
    commandList->SetComputeRootConstantBufferView(1,
        constants.priorIndex != 0 && priorCacheBinding != 0 ? priorCacheBinding : constantsAddress);
    commandList->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1);

    m_diffuse[writeSlot].Transition(commandList, shaderRead);
    m_specular[writeSlot].Transition(commandList, shaderRead);
    m_moments[writeSlot].Transition(commandList, shaderRead);

    if(extended)
    {
        m_extended.Transition(commandList, shaderRead);
    }

    m_pendingSlot = writeSlot;
    m_lastWrittenSlot = writeSlot;
}

void PathTraceTemporalPass::CommitHistory()
{
    if(!m_pendingSlot)
    {
        return;
    }

    m_committedSlot = *m_pendingSlot;
    m_historyValid = true;
    m_pendingSlot.reset();
}

void PathTraceTemporalPass::AbandonPendingHistory()
{
    m_pendingSlot.reset();
}

std::uint64_t PathTraceTemporalPass::GetResourceBytes() const
{
    const std::uint64_t pixels = static_cast<std::uint64_t>(m_width) * m_height;
    return m_diffuse[0].GetResource() ? pixels * BytesPerPixel + (m_extended.GetResource() ? pixels * 8u : 0u) : 0;
}
