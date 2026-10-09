#include "RayTracedAOPass.h"

#include "Core/Log.h"

#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"

#include "Renderer/Shader/ShaderLibrary.h"

#include "Renderer/RayTracing/RayTracingPipelineDescription.h"
#include "Renderer/RayTracing/RayTracingSceneResources.h"

bool RayTracedAOPass::Initialize(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    if(!graphicsDevice.SupportsRayTracing() || width == 0 || height == 0){
        return false;
    }

    if(!CreateTextures(graphicsDevice.GetDevice(), descriptorHeap, width, height)){
        Shutdown();
        return false;
    }

    RayTracingPipelineDescription pipelineDescription{};

    pipelineDescription.shaderPath = "RayTracedAO.hlsl";
    pipelineDescription.rayGenerationShader = L"AORayGeneration";
    pipelineDescription.missShaders = {L"AOMiss"};

    pipelineDescription.hitGroups = {
        {L"AOHitGroup", L"AOClosestHit", L""}
    };

    pipelineDescription.rootParameters =
    {
        {RootParameterType::ShaderResource, 0},
        {RootParameterType::DescriptorTable, 0, 0, D3D12_SHADER_VISIBILITY_ALL, DescriptorRangeType::UnorderedAccess, 1},
        {RootParameterType::ConstantBuffer, 0}
    };

    pipelineDescription.maxPayloadSize = 4;
    pipelineDescription.maxAttributeSize = 8;
    pipelineDescription.maxRecursionDepth = 1;

    if(!m_pipeline.Initialize(
        graphicsDevice.GetRayTracingDevice(),
        shaderLibrary,
        pipelineDescription))
    {
        Log::Error("Failed to initialize ray traced AO DXR pipeline.");
        Shutdown();
        return false;
    }

    return true;
}

bool RayTracedAOPass::CreateTextures(
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    if(!device || width == 0 || height == 0){
        return false;
    }

    if(!m_rawAOTexture.Initialize(
        device, width, height, 1, 1,
        DXGI_FORMAT_R16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        Log::Error("Failed to create raw ray traced AO texture.");
        return false;
    }

    if(!m_rawAOTexture.CreateSRV(device, descriptorHeap)){
        Log::Error("Failed to create raw ray traced AO SRV.");
        return false;
    }

    if(!m_rawAOTexture.CreateUAV(device, descriptorHeap)){
        Log::Error("Failed to create raw ray traced AO UAV.");
        return false;
    }

    return true;
}

bool RayTracedAOPass::Resize(
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    if(!device || width == 0 || height == 0){
        return false;
    }

    m_rawAOTexture.Shutdown();

    if(!CreateTextures(device, descriptorHeap, width, height)){
        Shutdown();
        return false;
    }

    return true;
}

void RayTracedAOPass::Execute(
    ID3D12GraphicsCommandList4* commandList,
    DescriptorHeap& descriptorHeap,
    const RayTracingSceneResources& rayTracingScene,
    D3D12_GPU_VIRTUAL_ADDRESS aoConstantsAddress,
    unsigned int width,
    unsigned int height)
{
    if(!commandList || width == 0 || height == 0){
        return;
    }

    m_rawAOTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    m_pipeline.Bind(commandList);

    commandList->SetComputeRootShaderResourceView(0, rayTracingScene.GetTopLevel().GetGPUAddress());
    commandList->SetComputeRootDescriptorTable(1, descriptorHeap.GetGPUHandle(m_rawAOTexture.GetUAVIndex()));
    commandList->SetComputeRootConstantBufferView(2, aoConstantsAddress);

    m_pipeline.Dispatch(commandList, width, height);

    constexpr D3D12_RESOURCE_STATES shaderReadState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    m_rawAOTexture.Transition(commandList, shaderReadState);
}

void RayTracedAOPass::Shutdown()
{
    m_rawAOTexture.Shutdown();
    m_pipeline.Shutdown();
}

Texture& RayTracedAOPass::GetRawAOTexture()
{
    return m_rawAOTexture;
}

const Texture& RayTracedAOPass::GetRawAOTexture() const
{
    return m_rawAOTexture;
}