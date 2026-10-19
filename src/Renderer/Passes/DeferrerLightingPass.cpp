#include "DeferredLightingPass.h"
#include "ToneMapPass.h"

#include "Core/Log.h"
#include "Renderer/GBuffer.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/StructuredBuffer.h"
#include "Renderer/Texture.h"
#include "Renderer/Shader/ShaderLibrary.h"

bool DeferredLightingPass::Initialize(
    ID3D12Device* device,
    ShaderLibrary& shaderLibrary)
{
    ShaderDescription vertexDescription{};
    vertexDescription.path = "DeferredLighting.hlsl";
    vertexDescription.entryPoint = "VSMain";
    vertexDescription.stage = ShaderStage::Vertex;

    Shader* vertexShader = shaderLibrary.Load(vertexDescription);

    if (!vertexShader)
    {
        Log::Error("Failed to load deferred lighting vertex shader.");
        return false;
    }

    ShaderDescription pixelDescription{};

    pixelDescription.path = "DeferredLighting.hlsl";
    pixelDescription.entryPoint = "PSMain";
    pixelDescription.stage = ShaderStage::Pixel;

    Shader* pixelShader = shaderLibrary.Load(pixelDescription);

    if (!pixelShader)
    {
        Log::Error("Failed to load deferred lighting pixel shader.");
        return false;
    }

    PipelineDescription pipelineDescription{};

    pipelineDescription.vertexShader = vertexShader;
    pipelineDescription.pixelShader = pixelShader;
    pipelineDescription.vertexLayout = nullptr;
    pipelineDescription.renderTargets.count = 1;

    pipelineDescription.renderTargets.formats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;

    pipelineDescription.sampler.filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;

    pipelineDescription.sampler.addressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    pipelineDescription.sampler.addressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    pipelineDescription.sampler.addressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    // Match the convolution's HDRI sampler without altering production sampler0.
    D3D12_STATIC_SAMPLER_DESC referenceSampler{};
    referenceSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    referenceSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    referenceSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    referenceSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    referenceSampler.MaxAnisotropy = 1;
    referenceSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    referenceSampler.MaxLOD = D3D12_FLOAT32_MAX;

    referenceSampler.ShaderRegister = 1;
    referenceSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    pipelineDescription.additionalSamplers.push_back(referenceSampler);

    pipelineDescription.rootParameters.push_back({RootParameterType::Constants, 0, 18, D3D12_SHADER_VISIBILITY_PIXEL});

    pipelineDescription.rootParameters.push_back(
        {RootParameterType::ConstantBuffer, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL});

    pipelineDescription.rootParameters.push_back(
        {RootParameterType::ConstantBuffer, 2, 0, D3D12_SHADER_VISIBILITY_PIXEL});

    // LIGHT.1 unshadowed Point/Spot/additional Directional Light table (SceneLightConstants).
    pipelineDescription.rootParameters.push_back(
        {RootParameterType::ConstantBuffer, 3, 0, D3D12_SHADER_VISIBILITY_PIXEL});

    if (!m_pipeline.Initialize(device, pipelineDescription))
    {
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
    Texture& diffuseIrradiance,
    Texture& brdfIntegrationLUT,
    Texture& ggxAverageAlbedoLUT,
    Texture& ggxMultiscatterIrradiance,
    Texture& shadowVisiblity,
    Texture& reflectionTexture,
    Texture& giIrradiance,
    Texture& aoVisibility,
    bool enableRTGI,
    float rtgiContactAOStrength,
    RenderTarget& sceneColor,
    D3D12_GPU_VIRTUAL_ADDRESS reconstructionConstantsAddress,
    D3D12_GPU_VIRTUAL_ADDRESS lightConstantsAddress,
    D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstantsAddress,
    unsigned int debugView,
    bool enableShadingNormalCorrection)
{
    gBuffer.GetMaterial().Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gBuffer.GetSurface().Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gBuffer.GetGeometricNormal().Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gBuffer.GetMotion().Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gBuffer.GetDepth().Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    reflectionTexture.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    giIrradiance.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    if (!enableRTGI || debugView == 21u || rtgiContactAOStrength > 0.0f)
    {
        aoVisibility.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    sceneColor.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_CPU_DESCRIPTOR_HANDLE sceneColorRTV = sceneColor.GetRTV();

    const float clearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};

    commandList->ClearRenderTargetView(sceneColorRTV, clearColor, 0, nullptr);

    commandList->OMSetRenderTargets(1, &sceneColorRTV, FALSE, nullptr);

    m_pipeline.Bind(commandList);

#ifdef NDEBUG
    enableShadingNormalCorrection = true;

    // This temporary diagnostic is selectable only in development builds.
    if (debugView == static_cast<unsigned int>(ToneMapDebugView::ShadingNormalViewHorizon))
    {
        debugView = 0u;
    }
#endif

    unsigned int resourceIndices[] = {
        gBuffer.GetMaterial().GetSRVIndex(),      gBuffer.GetSurface().GetSRVIndex(),
        gBuffer.GetDepth().GetSRVIndex(),         materialBuffer.GetDescriptorIndex(),
        environmentTexture.GetDescriptorIndex(),  environmentalCubemap.GetDescriptorIndex(),
        diffuseIrradiance.GetDescriptorIndex(),   brdfIntegrationLUT.GetDescriptorIndex(),
        ggxAverageAlbedoLUT.GetDescriptorIndex(), ggxMultiscatterIrradiance.GetDescriptorIndex(),
        shadowVisiblity.GetDescriptorIndex(),     reflectionTexture.GetDescriptorIndex(),
        giIrradiance.GetDescriptorIndex(),        enableRTGI ? 1u : 0u,
        aoVisibility.GetDescriptorIndex(),        debugView};

    commandList->SetGraphicsRoot32BitConstants(0, 16, resourceIndices, 0);

    commandList->SetGraphicsRoot32BitConstants(0, 1, &rtgiContactAOStrength, 16);
    const unsigned int correctionEnabled = enableShadingNormalCorrection ? 1u : 0u;
    commandList->SetGraphicsRoot32BitConstants(0, 1, &correctionEnabled, 17);
    commandList->SetGraphicsRootConstantBufferView(1, reconstructionConstantsAddress);
    commandList->SetGraphicsRootConstantBufferView(2, lightConstantsAddress);
    commandList->SetGraphicsRootConstantBufferView(3, sceneLightConstantsAddress);

    commandList->DrawInstanced(3, 1, 0, 0);
}

void DeferredLightingPass::Shutdown()
{
    m_pipeline.Shutdown();
}
