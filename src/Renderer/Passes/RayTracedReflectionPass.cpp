#include "RayTracedReflectionPass.h"

#include "Core/Log.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/RayTracing/RayTracingSceneResources.h"
#include "Renderer/Shader/ShaderLibrary.h"

bool RayTracedReflectionPass::Initialize(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    if(!graphicsDevice.SupportsRayTracing() || width == 0 || height == 0)
    {
        return false;
    }

    ID3D12Device* device = graphicsDevice.GetDevice();

    if(!m_rawReflectionTexture.Initialize(device, width, height, 1, 1,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        return false;
    }

    if(!m_rawReflectionTexture.CreateSRV(device, descriptorHeap))
    {
        return false;
    }

    if(!m_rawReflectionTexture.CreateUAV(device, descriptorHeap))
    {
        return false;
    }

    if(!m_hitDistanceTexture.Initialize(device, width, height, 1, 1,
        DXGI_FORMAT_R16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        return false;
    }

    if(!m_hitDistanceTexture.CreateSRV(device, descriptorHeap))
    {
        return false;
    }

    if(!m_hitDistanceTexture.CreateUAV(device, descriptorHeap))
    {
        return false;
    }

    RayTracingPipelineDescription pipelineDescription{};

    pipelineDescription.shaderPath = "RayTracedReflection.hlsl";
    pipelineDescription.rayGenerationShader = L"RayGeneration";
    pipelineDescription.missShaders = {L"Miss"};

    pipelineDescription.hitGroups =
    {
        {
            L"HitGroup",
            L"ClosestHit",
            L""
        }
    };

    pipelineDescription.rootParameters =
    {
        {RootParameterType::ShaderResource, 0},
        {RootParameterType::DescriptorTable, 0, 0, D3D12_SHADER_VISIBILITY_ALL, DescriptorRangeType::UnorderedAccess, 1},
        {RootParameterType::DescriptorTable, 1, 0, D3D12_SHADER_VISIBILITY_ALL, DescriptorRangeType::UnorderedAccess, 1},
        {RootParameterType::ConstantBuffer, 0},
        {RootParameterType::ConstantBuffer, 1}
    };

    pipelineDescription.maxPayloadSize = sizeof(float) * 4;
    pipelineDescription.maxAttributeSize = sizeof(float) * 2;
    pipelineDescription.maxRecursionDepth = 1;

    if(!m_pipeline.Initialize(graphicsDevice.GetRayTracingDevice(), shaderLibrary, pipelineDescription))
    {
        Shutdown();
        return false;
    }

    return true;
}

void RayTracedReflectionPass::Execute(
    ID3D12GraphicsCommandList4* commandList,
    DescriptorHeap& descriptorHeap,
    const RayTracingSceneResources& rayTracingScene,
    D3D12_GPU_VIRTUAL_ADDRESS reflectionConstantsAddress,
    D3D12_GPU_VIRTUAL_ADDRESS lightConstantsAddress,
    unsigned int width,
    unsigned int height)
{
    if(!commandList || reflectionConstantsAddress == 0 || lightConstantsAddress == 0)
    {
        return;
    }

    m_rawReflectionTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    m_hitDistanceTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    m_pipeline.Bind(commandList);

    commandList->SetComputeRootShaderResourceView(0, rayTracingScene.GetTopLevel().GetGPUAddress());

    const D3D12_GPU_DESCRIPTOR_HANDLE outputUAV = descriptorHeap.GetGPUHandle(m_rawReflectionTexture.GetUAVIndex());
    const D3D12_GPU_DESCRIPTOR_HANDLE hitDistanceUAV = descriptorHeap.GetGPUHandle(m_hitDistanceTexture.GetUAVIndex());

    commandList->SetComputeRootDescriptorTable(1, outputUAV);
    commandList->SetComputeRootDescriptorTable(2, hitDistanceUAV);
    commandList->SetComputeRootConstantBufferView(3, reflectionConstantsAddress);
    commandList->SetComputeRootConstantBufferView(4, lightConstantsAddress);

    m_pipeline.Dispatch(commandList, width, height);

    constexpr D3D12_RESOURCE_STATES shaderReadState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    m_rawReflectionTexture.Transition(commandList, shaderReadState);

    m_hitDistanceTexture.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

bool RayTracedReflectionPass::Resize(ID3D12Device* device, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height)
{
    if(!device || width == 0 || height == 0)
    {
        return false;
    }

    m_rawReflectionTexture.Shutdown();
    m_hitDistanceTexture.Shutdown();

    if(!m_rawReflectionTexture.Initialize(device, width, height, 1, 1,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        Log::Error("Failed to resize raw ray traced reflection texture.");

        return false;
    }

    if(!m_rawReflectionTexture.CreateSRV(device, descriptorHeap))
    {
        Log::Error("Failed to recreate raw ray traced reflection SRV.");

        return false;
    }

    if(!m_rawReflectionTexture.CreateUAV(device, descriptorHeap))
    {
        Log::Error("Failed to recreate raw ray traced reflection UAV.");

        return false;
    }

    if(!m_hitDistanceTexture.Initialize(device, width, height, 1, 1,
        DXGI_FORMAT_R16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        Log::Error("Failed to resize ray traced reflection hit distance texture.");

        return false;
    }

    if(!m_hitDistanceTexture.CreateSRV(device, descriptorHeap))
    {
        Log::Error("Failed to recreate ray traced reflection hit distance SRV.");

        return false;
    }

    if(!m_hitDistanceTexture.CreateUAV(device, descriptorHeap))
    {
        Log::Error("Failed to recreate ray traced reflection hit distance UAV.");

        return false;
    }

    return true;
}

void RayTracedReflectionPass::Shutdown()
{
    m_pipeline.Shutdown();

    m_rawReflectionTexture.Shutdown();
    m_hitDistanceTexture.Shutdown();
}

Texture& RayTracedReflectionPass::GetRawReflectionTexture()
{
    return m_rawReflectionTexture;
}

const Texture& RayTracedReflectionPass::GetRawReflectionTexture() const
{
    return m_rawReflectionTexture;
}

Texture& RayTracedReflectionPass::GetHitDistanceTexture()
{
    return m_hitDistanceTexture;
}

const Texture& RayTracedReflectionPass::GetHitDistanceTexture() const
{
    return m_hitDistanceTexture;
}