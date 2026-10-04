#include "RayTracingTestPass.h"

#include "Core/Log.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/Mesh.h"
#include "Renderer/RayTracing/RayTracingAccelerationStructure.h"
#include "Scene/Scene.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/RayTracing/RayTracingSceneResources.h"

#include <vector>

bool RayTracingTestPass::Initialize(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    if(!graphicsDevice.SupportsRayTracing()){
        return false;
    }

    RayTracingPipelineDescription pipelineDescription{};
    pipelineDescription.shaderPath = "RayTracingTest.hlsl";
    pipelineDescription.rayGenerationShader = L"RayGeneration";
    pipelineDescription.missShaders = {L"Miss"};
    pipelineDescription.hitGroups ={{L"HitGroup", L"ClosestHit", L""}};
    pipelineDescription.maxPayloadSize = sizeof(float) * 3;
    pipelineDescription.maxAttributeSize = sizeof(float) * 2;
    pipelineDescription.maxRecursionDepth = 1;

    if(!m_pipeline.Initialize(graphicsDevice.GetRayTracingDevice(), shaderLibrary, pipelineDescription))
    {
        return false;
    }

    if(!m_output.Initialize(graphicsDevice.GetDevice(), width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        Log::Error("Failed to initialize ray tracing output texture.");
        return false;
    }

    if(!m_output.CreateUAV(graphicsDevice.GetDevice(), descriptorHeap))
    {
        Log::Error("Failed to create ray tracing output UAV.");
        return false;
    }

    return true;
}

void RayTracingTestPass::Execute(
    ID3D12GraphicsCommandList4* commandList,
    DescriptorHeap& descriptorHeap,
    const RayTracingSceneResources& rayTracingScene,
    D3D12_GPU_VIRTUAL_ADDRESS reconstructionConstantsAddress,
    RenderTarget& sceneColor,
    unsigned int width,
    unsigned int height)
{
    if(!commandList){
        return;
    }

    m_output.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->SetPipelineState1(m_pipeline.GetStateObject());
    commandList->SetComputeRootSignature(m_pipeline.GetGlobalRootSignature());
    commandList->SetComputeRootShaderResourceView(0, rayTracingScene.GetTopLevel().GetGPUAddress());

    const D3D12_GPU_DESCRIPTOR_HANDLE outputUAV = descriptorHeap.GetGPUHandle(m_output.GetUAVIndex());

    commandList->SetComputeRootDescriptorTable(1, outputUAV);
    commandList->SetComputeRootConstantBufferView(2, reconstructionConstantsAddress);

    D3D12_DISPATCH_RAYS_DESC dispatchDescription = m_pipeline.CreateDispatchDescription(width, height);

    commandList->DispatchRays(&dispatchDescription);

    m_output.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);

    sceneColor.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);

    commandList->CopyResource(sceneColor.GetResource(), m_output.GetResource());
}

void RayTracingTestPass::Shutdown()
{
    m_pipeline.Shutdown();
    m_output.Shutdown();
}