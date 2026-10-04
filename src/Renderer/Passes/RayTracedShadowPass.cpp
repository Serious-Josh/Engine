#include "RayTracedShadowPass.h"

#include "Core/Log.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/RayTracing/RayTracingSceneResources.h"
#include "Renderer/Shader/ShaderLibrary.h"

bool RayTracedShadowPass::Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height)
{
    if(!graphicsDevice.SupportsRayTracing() || width == 0 || height == 0){
        return false;
    }

    ID3D12Device* device = graphicsDevice.GetDevice();

    for(unsigned int i = 0; i < 2; ++i){
        if(!m_historyTextures[i].Initialize(device, width, height, 1, 1, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)){
            return false;
        }

        if(!m_historyTextures[i].CreateSRV(device, descriptorHeap)){
            return false;
        }

        if(!m_historyTextures[i].CreateUAV(device, descriptorHeap)){
            return false;
        }
    }

    for(unsigned int i = 0; i < 2; ++i){
        if(!m_depthHistoryTextures[i].Initialize(graphicsDevice.GetDevice(), width, height, 1, 1, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)){
            return false;
        }

        if(!m_depthHistoryTextures[i].CreateSRV(graphicsDevice.GetDevice(), descriptorHeap)){
            return false;
        }

        if(!m_depthHistoryTextures[i].CreateUAV(graphicsDevice.GetDevice(), descriptorHeap)){
            return false;
        }
    }

    for(unsigned int i = 0; i < 2; ++i){
        if(!m_historyLengthTextures[i].Initialize(graphicsDevice.GetDevice(), width, height, 1, 1, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)){
            return false;
        }

        if(!m_historyLengthTextures[i].CreateSRV(graphicsDevice.GetDevice(), descriptorHeap)){
            return false;
        }

        if(!m_historyLengthTextures[i].CreateUAV(graphicsDevice.GetDevice(), descriptorHeap)){
            return false;
        }
    }

    RayTracingPipelineDescription pipelineDescription{};
    pipelineDescription.shaderPath = "RayTracedShadow.hlsl";
    pipelineDescription.rayGenerationShader = L"RayGeneration";
    pipelineDescription.missShaders = {L"Miss"};
    pipelineDescription.hitGroups =
    {
        {L"HitGroup", L"ClosestHit", L""}
    };
    pipelineDescription.maxPayloadSize = sizeof(unsigned int);
    pipelineDescription.maxAttributeSize = sizeof(float) * 2;
    pipelineDescription.maxRecursionDepth = 1;

    if(!m_pipeline.Initialize(graphicsDevice.GetRayTracingDevice(), shaderLibrary, pipelineDescription)){
        Shutdown();
        return false;
    }

    m_historyReadIndex = 0;
    m_accumulatedFrameCount = 0;

    return true;
}

void RayTracedShadowPass::Execute(
            ID3D12GraphicsCommandList4* commandList,
            DescriptorHeap& descriptorHeap,
            const RayTracingSceneResources& rayTracingScene,
            D3D12_GPU_VIRTUAL_ADDRESS shadowConstantsAddress,
            unsigned int width,
            unsigned int height)
{
    if(!commandList || shadowConstantsAddress == 0){
        return;
    }

    const unsigned int writeIndex = 1u - m_historyReadIndex;

    Texture& historyRead = m_historyTextures[m_historyReadIndex];
    Texture& historyWrite = m_historyTextures[writeIndex];
    Texture& depthHistoryRead = m_depthHistoryTextures[m_historyReadIndex];
    Texture& depthHistoryWrite = m_depthHistoryTextures[writeIndex];
    Texture& historyLengthRead = m_historyLengthTextures[m_historyReadIndex];
    Texture& historyLengthWrite = m_historyLengthTextures[writeIndex];

    depthHistoryRead.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    depthHistoryWrite.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    historyRead.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    historyWrite.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    historyLengthRead.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    historyLengthWrite.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->SetPipelineState1(m_pipeline.GetStateObject());
    commandList->SetComputeRootSignature(m_pipeline.GetGlobalRootSignature());
    commandList->SetComputeRootShaderResourceView(0, rayTracingScene.GetTopLevel().GetGPUAddress());

    const D3D12_GPU_DESCRIPTOR_HANDLE outputUAV = descriptorHeap.GetGPUHandle(historyWrite.GetUAVIndex());

    commandList->SetComputeRootDescriptorTable(1, outputUAV);
    commandList->SetComputeRootConstantBufferView(2, shadowConstantsAddress);

    D3D12_DISPATCH_RAYS_DESC dispatchDescription = m_pipeline.CreateDispatchDescription(width, height);

    commandList->DispatchRays(&dispatchDescription);

    historyWrite.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    m_historyReadIndex = writeIndex;

    ++m_accumulatedFrameCount;
}

bool RayTracedShadowPass::Resize(ID3D12Device* device, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height)
{
    for(Texture& texture : m_historyTextures){
        texture.Shutdown();
    }

    for(Texture& texture : m_depthHistoryTextures){
        texture.Shutdown();
    }

    for(Texture& texture : m_historyLengthTextures){
        texture.Shutdown();
    }

    for(unsigned int i = 0; i < 2; ++i){
        if(!m_historyTextures[i].Initialize(
            device,
            width,
            height,
            1,
            1,
            DXGI_FORMAT_R16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            Log::Error("Failed to resize ray traced shadow visibility texture.");
            return false;
        }

        if(!m_historyTextures[i].CreateSRV(device, descriptorHeap))
        {
            Log::Error("Failed to recreate ray traced shadow visibility SRV.");
            return false;
        }

        if(!m_historyTextures[i].CreateUAV(device, descriptorHeap))
        {
            Log::Error("Failed to recreate ray traced shadow visibility UAV.");
            return false;
        }
    }

    for(unsigned int i = 0; i < 2; ++i){
        if(!m_depthHistoryTextures[i].Initialize(device, width, height, 1, 1, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)){
            return false;
        }

        if(!m_depthHistoryTextures[i].CreateSRV(device, descriptorHeap)){
            return false;
        }

        if(!m_depthHistoryTextures[i].CreateUAV(device, descriptorHeap)){
            return false;
        }
    }

    for(unsigned int i = 0; i < 2; ++i)
    {
        if(!m_historyLengthTextures[i].Initialize(device, width, height, 1, 1, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            Log::Error("Failed to resize ray traced shadow history length texture.");
            return false;
        }

        if(!m_historyLengthTextures[i].CreateSRV(device, descriptorHeap))
        {
            Log::Error("Failed to recreate ray traced shadow history length SRV.");
            return false;
        }

        if(!m_historyLengthTextures[i].CreateUAV(device, descriptorHeap))
        {
            Log::Error("Failed to recreate ray traced shadow history length UAV.");
            return false;
        }
    }

    m_historyReadIndex = 0;
    m_accumulatedFrameCount = 0;

    return true;
}

void RayTracedShadowPass::Shutdown()
{
    m_pipeline.Shutdown();
    
    for(Texture& texture : m_historyTextures){
        texture.Shutdown();
    }

    for(Texture& depthHistory : m_depthHistoryTextures){
        depthHistory.Shutdown();
    }

    for(Texture& texture : m_historyLengthTextures){
        texture.Shutdown();
    }

    m_historyReadIndex = 0;
    m_accumulatedFrameCount = 0;
}

Texture& RayTracedShadowPass::GetVisibilityTexture()
{
    return m_historyTextures[m_historyReadIndex];
}

const Texture& RayTracedShadowPass::GetVisibilityTexture() const
{
    return m_historyTextures[m_historyReadIndex];
}

unsigned int RayTracedShadowPass::GetAccumulatedFrameCount() const
{
    return m_accumulatedFrameCount;
}

void RayTracedShadowPass::ResetHistory()
{
    m_accumulatedFrameCount = 0;
}

unsigned int RayTracedShadowPass::GetPreviousDepthIndex() const
{
    return m_depthHistoryTextures[m_historyReadIndex].GetDescriptorIndex();
}

unsigned int RayTracedShadowPass::GetOutputDepthIndex() const
{
    const unsigned int writeIndex = 1u - m_historyReadIndex;

    return m_depthHistoryTextures[writeIndex].GetUAVIndex();
}

unsigned int RayTracedShadowPass::GetPreviousHistoryLengthIndex() const
{
    return m_historyLengthTextures[m_historyReadIndex].GetDescriptorIndex();
}

unsigned int RayTracedShadowPass::GetOutputHistoryLengthIndex() const
{
    const unsigned int writeIndex =
        1u - m_historyReadIndex;

    return m_historyLengthTextures[writeIndex].GetUAVIndex();
}