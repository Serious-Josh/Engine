#include "RayTracedGIPass.h"

#include "Core/Log.h"

#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"

#include "Renderer/Shader/ShaderLibrary.h"

#include "Renderer/RayTracing/RayTracingPipelineDescription.h"
#include "Renderer/RayTracing/RayTracingSceneResources.h"

bool RayTracedGIPass::Initialize(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    if (!graphicsDevice.SupportsRayTracing() || width == 0 || height == 0)
    {
        return false;
    }

    if (!CreateTextures(graphicsDevice.GetDevice(), descriptorHeap, width, height))
    {
        Shutdown();
        return false;
    }

    RayTracingPipelineDescription pipelineDescription{};

    pipelineDescription.shaderPath = "RayTracedGI.hlsl";
    pipelineDescription.rayGenerationShader = L"GIRayGeneration";
    pipelineDescription.missShaders = {L"GIMiss", L"GIVisibilityMiss"};

    pipelineDescription.hitGroups = {{L"GIHitGroup", L"GIClosestHit", L""}};

    pipelineDescription.rootParameters = {{RootParameterType::ShaderResource, 0},
                                          {RootParameterType::DescriptorTable, 0, 0, D3D12_SHADER_VISIBILITY_ALL,
                                           DescriptorRangeType::UnorderedAccess, 1},
                                          {RootParameterType::DescriptorTable, 1, 0, D3D12_SHADER_VISIBILITY_ALL,
                                           DescriptorRangeType::UnorderedAccess, 1},
                                          {RootParameterType::ConstantBuffer, 0},
                                          {RootParameterType::ConstantBuffer, 1}};

    pipelineDescription.maxPayloadSize = 40; // Hit distance, world normal, UV, material index.
    pipelineDescription.maxAttributeSize = 8;
    pipelineDescription.maxRecursionDepth = 1;

    if (!m_pipeline.Initialize(graphicsDevice.GetRayTracingDevice(), shaderLibrary, pipelineDescription))
    {
        Log::Error("Failed to initialize ray traced GI DXR pipeline.");
        Shutdown();
        return false;
    }

    return true;
}

bool RayTracedGIPass::CreateTextures(

    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    if (!device || width == 0 || height == 0)
    {
        return false;
    }

    if (!m_rawGITexture.Initialize(device, width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        Log::Error("Failed to create raw ray traced GI texture.");
        return false;
    }

    if (!m_rawGITexture.CreateSRV(device, descriptorHeap))
    {

        Log::Error("Failed to create raw ray traced GI SRV.");
        return false;
    }

    if (!m_rawGITexture.CreateUAV(device, descriptorHeap))
    {
        Log::Error("Failed to create raw ray traced GI UAV.");

        return false;
    }

    if (!m_hitDistanceTexture.Initialize(device, width, height, 1, 1, DXGI_FORMAT_R32_FLOAT,
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        Log::Error("Failed to create raw ray traced hit distance texture.");
        return false;
    }

    if (!m_hitDistanceTexture.CreateSRV(device, descriptorHeap))
    {
        Log::Error("Failed to create raw ray traced hit distance SRV.");

        return false;
    }

    if (!m_hitDistanceTexture.CreateUAV(device, descriptorHeap))
    {
        Log::Error("Failed to create raw ray traced hit distance UAV.");

        return false;
    }

    return true;
}

bool RayTracedGIPass::Resize(
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    if (!device || width == 0 || height == 0)
    {
        return false;
    }

    m_rawGITexture.Shutdown();
    m_hitDistanceTexture.Shutdown();

    if (!CreateTextures(device, descriptorHeap, width, height))
    {
        Shutdown();
        return false;
    }

    return true;
}

void RayTracedGIPass::Execute(
    ID3D12GraphicsCommandList4* commandList,
    DescriptorHeap& descriptorHeap,
    const RayTracingSceneResources& rayTracingScene,
    D3D12_GPU_VIRTUAL_ADDRESS giConstantsAddress,
    D3D12_GPU_VIRTUAL_ADDRESS lightConstantsAddress,
    unsigned int width,
    unsigned int height)
{
    if (!commandList || width == 0 || height == 0)
    {
        return;
    }

    m_rawGITexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_hitDistanceTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    m_pipeline.Bind(commandList);

    commandList->SetComputeRootShaderResourceView(0, rayTracingScene.GetTopLevel().GetGPUAddress());
    commandList->SetComputeRootDescriptorTable(1, descriptorHeap.GetGPUHandle(m_rawGITexture.GetUAVIndex()));
    commandList->SetComputeRootDescriptorTable(2, descriptorHeap.GetGPUHandle(m_hitDistanceTexture.GetUAVIndex()));
    commandList->SetComputeRootConstantBufferView(3, giConstantsAddress);
    commandList->SetComputeRootConstantBufferView(4, lightConstantsAddress);

    m_pipeline.Dispatch(commandList, width, height);

    constexpr D3D12_RESOURCE_STATES shaderReadState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    m_rawGITexture.Transition(commandList, shaderReadState);
    m_hitDistanceTexture.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void RayTracedGIPass::Shutdown()
{
    m_rawGITexture.Shutdown();
    m_hitDistanceTexture.Shutdown();
    m_pipeline.Shutdown();
}

Texture& RayTracedGIPass::GetRawGITexture()
{
    return m_rawGITexture;
}

const Texture& RayTracedGIPass::GetRawGITexture() const
{
    return m_rawGITexture;
}

Texture& RayTracedGIPass::GetHitDistanceTexture()
{
    return m_hitDistanceTexture;
}

const Texture& RayTracedGIPass::GetHitDistanceTexture() const
{
    return m_hitDistanceTexture;
}
