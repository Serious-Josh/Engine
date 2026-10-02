#include "DeferredLightingPass.h"

#include "Core/Log.h"
#include "Renderer/GBuffer.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/StructuredBuffer.h"
#include "Renderer/Texture.h"
#include "Renderer/Shader/ShaderLibrary.h"

bool DeferredLightingPass::Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary)
{
    ShaderDescription vertexDescription{};
    vertexDescription.path = "DeferredLighting.hlsl";
    vertexDescription.entryPoint = "VSMain";
    vertexDescription.stage = ShaderStage::Vertex;

    Shader* vertexShader = shaderLibrary.Load(vertexDescription);

    if(!vertexShader){
        Log::Error("Failed to load deferred lighting vertex shader.");
        return false;
    }

    ShaderDescription pixelDescription{};
    pixelDescription.path = "DeferredLighting.hlsl";
    pixelDescription.entryPoint = "PSMain";
    pixelDescription.stage = ShaderStage::Pixel;

    Shader* pixelShader = shaderLibrary.Load(pixelDescription);

    if(!pixelShader){
        Log::Error("Failed to load deferred lighting pixel shader.");
        return false;
    }

    PipelineDescription pipelineDescription{};
    pipelineDescription.vertexShader = vertexShader;
    pipelineDescription.pixelShader = pixelShader;
    pipelineDescription.vertexLayout = nullptr;
    pipelineDescription.renderTargets.count = 1;
    pipelineDescription.renderTargets.formats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;

    pipelineDescription.sampler.filter =
        D3D12_FILTER_MIN_MAG_MIP_LINEAR;

    pipelineDescription.sampler.addressU =
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    pipelineDescription.sampler.addressV =
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    pipelineDescription.sampler.addressW =
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    pipelineDescription.rootParameters.push_back(
        {RootParameterType::Constants, 0, 7, D3D12_SHADER_VISIBILITY_PIXEL}
    );

    pipelineDescription.rootParameters.push_back(
        {RootParameterType::ConstantBuffer, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL}
    );

    pipelineDescription.rootParameters.push_back(
        {RootParameterType::ConstantBuffer, 2, 0, D3D12_SHADER_VISIBILITY_PIXEL}
    );

    if(!m_pipeline.Initialize(device, pipelineDescription)){
        Log::Error("Failed to initialize deferred lighting pipeline.");
        return false;
    }

    return true;
}

void DeferredLightingPass::Execute(
    ID3D12GraphicsCommandList* commandList,
    GBuffer& gBuffer,
    StructuredBuffer& materialBuffer,
    Texture& environmentTexture,
    Texture& environmentalCubemap,
    Texture& brdfIntegrationLUT,
    RenderTarget& sceneColor,
    D3D12_GPU_VIRTUAL_ADDRESS reconstructionConstantsAddress,
    D3D12_GPU_VIRTUAL_ADDRESS lightConstantsAddress)
{
    sceneColor.Transition(
        commandList,
        D3D12_RESOURCE_STATE_RENDER_TARGET
    );

    D3D12_CPU_DESCRIPTOR_HANDLE sceneColorRTV =
        sceneColor.GetRTV();

    const float clearColor[] =
    {
        0.0f,
        0.0f,
        0.0f,
        1.0f
    };

    commandList->ClearRenderTargetView(
        sceneColorRTV,
        clearColor,
        0,
        nullptr
    );

    commandList->OMSetRenderTargets(
        1,
        &sceneColorRTV,
        FALSE,
        nullptr
    );

    m_pipeline.Bind(commandList);

    unsigned int resourceIndices[] =
    {
        gBuffer.GetMaterial().GetSRVIndex(),
        gBuffer.GetSurface().GetSRVIndex(),
        gBuffer.GetDepth().GetSRVIndex(),
        materialBuffer.GetDescriptorIndex(),
        environmentTexture.GetDescriptorIndex(),
        environmentalCubemap.GetDescriptorIndex(),
        brdfIntegrationLUT.GetDescriptorIndex()
    };

    commandList->SetGraphicsRoot32BitConstants(
        0,
        7,
        resourceIndices,
        0
    );

    commandList->SetGraphicsRootConstantBufferView(
        1,
        reconstructionConstantsAddress
    );

    commandList->SetGraphicsRootConstantBufferView(
        2,
        lightConstantsAddress
    );

    commandList->DrawInstanced(3, 1, 0, 0);
}

void DeferredLightingPass::Shutdown()
{
    m_pipeline.Shutdown();
}