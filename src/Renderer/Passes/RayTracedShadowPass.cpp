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

    if(!m_visibilityTexture.Initialize(device, width, height, 1, 1, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)){
        return false;
    }

    if(!m_visibilityTexture.CreateSRV(device, descriptorHeap)){
        m_visibilityTexture.Shutdown();
        return false;
    }

    if(!m_visibilityTexture.CreateUAV(device, descriptorHeap)){
        m_visibilityTexture.Shutdown();
        return false;
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
        m_visibilityTexture.Shutdown();
        return false;
    }

    return true;
}

void RayTracedShadowPass::Execute(ID3D12GraphicsCommandList4* commandList,
            DescriptorHeap& descriptorHeap,
            const RayTracingSceneResources& rayTracingScene,
            D3D12_GPU_VIRTUAL_ADDRESS shadowConstantsAddress,
            unsigned int width,
            unsigned int height)
{
    if(!commandList || shadowConstantsAddress == 0){
        return;
    }

    m_visibilityTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->SetPipelineState1(m_pipeline.GetStateObject());
    commandList->SetComputeRootSignature(m_pipeline.GetGlobalRootSignature());
    commandList->SetComputeRootShaderResourceView(0, rayTracingScene.GetTopLevel().GetGPUAddress());

    const D3D12_GPU_DESCRIPTOR_HANDLE outputUAV = descriptorHeap.GetGPUHandle(m_visibilityTexture.GetUAVIndex());

    commandList->SetComputeRootDescriptorTable(1, outputUAV);
    commandList->SetComputeRootConstantBufferView(2, shadowConstantsAddress);

    D3D12_DISPATCH_RAYS_DESC dispatchDescription = m_pipeline.CreateDispatchDescription(width, height);

    commandList->DispatchRays(&dispatchDescription);

    m_visibilityTexture.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

bool RayTracedShadowPass::Resize(ID3D12Device* device, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height)
{
    m_visibilityTexture.Shutdown();

    if(!m_visibilityTexture.Initialize(
        device,
        width,
        height,
        1,
        1,
        DXGI_FORMAT_R8_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
    {
        Log::Error("Failed to resize ray traced shadow visibility texture.");
        return false;
    }

    if(!m_visibilityTexture.CreateSRV(device, descriptorHeap))
    {
        Log::Error("Failed to recreate ray traced shadow visibility SRV.");
        return false;
    }

    if(!m_visibilityTexture.CreateUAV(device, descriptorHeap))
    {
        Log::Error("Failed to recreate ray traced shadow visibility UAV.");
        return false;
    }

    return true;
}

void RayTracedShadowPass::Shutdown()
{
    m_pipeline.Shutdown();
    m_visibilityTexture.Shutdown();
}

Texture& RayTracedShadowPass::GetVisibilityTexture()
{
    return m_visibilityTexture;
}

const Texture& RayTracedShadowPass::GetVisibilityTexture() const
{
    return m_visibilityTexture;
}