#include "NrdPreparePass.h"

#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/GBuffer.h"
#include "Renderer/UploadBuffer.h"
#include "Core/Log.h"

bool NrdPreparePass::Initialize(
    GraphicsDevice& graphicsDevice,
    ShaderLibrary& shaderLibrary,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height,
    Signal signal)
{
    m_signal = signal;

    ShaderDescription shaderDescription{};
    shaderDescription.path = (m_signal == Signal::Diffuse) ? "NrdGIPrepare.hlsl" : "NrdReflectionPrepare.hlsl";
    shaderDescription.entryPoint = "CSMain";
    shaderDescription.stage = ShaderStage::Compute;

    Shader* computeShader = shaderLibrary.Load(shaderDescription);

    if(!computeShader){
        return false;
    }

    ComputePipelineDescription pipelineDescription{};
    pipelineDescription.computeShader = computeShader;

    RootParameterDescription constants{};
    constants.type = RootParameterType::ConstantBuffer;
    constants.shaderRegister = 0;
    constants.visibility = D3D12_SHADER_VISIBILITY_ALL;

    pipelineDescription.rootParameters.push_back(constants);

    if(!m_pipeline.Initialize(graphicsDevice.GetDevice(), pipelineDescription)){
        return false;
    }

    if(!CreateTextures(graphicsDevice, descriptorHeap, width, height)){
        return false;
    }

    return true;
}

bool NrdPreparePass::Resize(
    GraphicsDevice& graphicsDevice,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    m_viewZTexture.Shutdown();
    m_motionTexture.Shutdown();
    m_normalRoughnessTexture.Shutdown();
    m_specRadianceHitDistTexture.Shutdown();
    m_diffRadianceHitDistTexture.Shutdown();

    return CreateTextures(graphicsDevice, descriptorHeap, width, height);
}

void NrdPreparePass::ExecuteReflections(
    ID3D12GraphicsCommandList* commandList,
    GBuffer& gBuffer,
    Texture& rawReflection,
    Texture& trackingDistance,
    UploadBuffer& uploadBuffer,
    const Matrix4& inverseViewProjection,
    const Matrix4& view,
    unsigned int materialBufferIndex,
    unsigned int width,
    unsigned int height,
    float denoisingRange)
{
    if(m_signal != Signal::Specular || !commandList || width == 0 || height == 0)
    {
        return;
    }

    constexpr D3D12_RESOURCE_STATES gBufferReadState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    gBuffer.GetDepth().Transition(commandList, gBufferReadState);
    gBuffer.GetMaterial().Transition(commandList, gBufferReadState);
    gBuffer.GetSurface().Transition(commandList, gBufferReadState);
    gBuffer.GetMotion().Transition(commandList, gBufferReadState);
    rawReflection.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    trackingDistance.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    m_viewZTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_motionTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_normalRoughnessTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_specRadianceHitDistTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    NrdReflectionPrepareConstants constants{};

    constants.inverseViewProjection = inverseViewProjection;
    constants.view = view;

    constants.depthIndex = gBuffer.GetDepth().GetSRVIndex();
    constants.materialIndex = gBuffer.GetMaterial().GetSRVIndex();
    constants.surfaceIndex = gBuffer.GetSurface().GetSRVIndex();
    constants.motionInputIndex = gBuffer.GetMotion().GetSRVIndex();

    constants.materialBufferIndex = materialBufferIndex;
    constants.rawReflectionIndex = rawReflection.GetDescriptorIndex();
    constants.trackingDistanceIndex = trackingDistance.GetDescriptorIndex();
    constants.width = width;

    constants.height = height;
    constants.hitDistanceA = 3.0f;
    constants.hitDistanceB = 0.1f;
    constants.hitDistanceC = 20.0f;

    constants.denoisingRange = denoisingRange;
    constants.viewZOutputIndex = m_viewZTexture.GetUAVIndex();
    constants.motionOutputIndex = m_motionTexture.GetUAVIndex();
    constants.normalRoughnessOutputIndex = m_normalRoughnessTexture.GetUAVIndex();
    
    constants.specRadianceHitDistOutputIndex = m_specRadianceHitDistTexture.GetUAVIndex();
    constants.rawGIIndex = 0;
    constants.giHitDistanceIndex = 0;
    constants.diffRadianceHitDistOutputIndex = 0;

    const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress = uploadBuffer.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(constantsAddress == 0)
    {
        return;
    }

    m_pipeline.Bind(commandList);

    commandList->SetComputeRootConstantBufferView(0, constantsAddress);

    const unsigned int groupCountX = (width + 7) / 8;
    const unsigned int groupCountY = (height + 7) / 8;

    commandList->Dispatch(groupCountX, groupCountY, 1);

    constexpr D3D12_RESOURCE_STATES nrdInputState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    m_viewZTexture.Transition(commandList, nrdInputState);
    m_motionTexture.Transition(commandList, nrdInputState);
    m_normalRoughnessTexture.Transition(commandList, nrdInputState);

    m_specRadianceHitDistTexture.Transition(commandList, nrdInputState);
}

void NrdPreparePass::ExecuteGI(
    ID3D12GraphicsCommandList* commandList,
    GBuffer& gBuffer,
    Texture& rawGI,
    Texture& giHitDistance,
    UploadBuffer& uploadBuffer,
    const Matrix4& inverseViewProjection,
    const Matrix4& view,
    unsigned int materialBufferIndex,
    unsigned int fullWidth,
    unsigned int fullHeight,
    unsigned int giWidth,
    unsigned int giHeight,
    float denoisingRange)
{
    if(m_signal != Signal::Diffuse || !commandList ||
       fullWidth == 0 || fullHeight == 0 || giWidth == 0 || giHeight == 0)
    {
        return;
    }

    constexpr D3D12_RESOURCE_STATES gBufferReadState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    gBuffer.GetDepth().Transition(commandList, gBufferReadState);
    gBuffer.GetMaterial().Transition(commandList, gBufferReadState);
    gBuffer.GetSurface().Transition(commandList, gBufferReadState);
    gBuffer.GetMotion().Transition(commandList, gBufferReadState);

    rawGI.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    giHitDistance.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    m_viewZTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_motionTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_normalRoughnessTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_diffRadianceHitDistTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    NrdGIPrepareConstants constants{};

    constants.inverseViewProjection = inverseViewProjection;
    constants.view = view;

    constants.depthIndex = gBuffer.GetDepth().GetSRVIndex();
    constants.materialIndex = gBuffer.GetMaterial().GetSRVIndex();
    constants.surfaceIndex = gBuffer.GetSurface().GetSRVIndex();
    constants.motionInputIndex = gBuffer.GetMotion().GetSRVIndex();

    constants.materialBufferIndex = materialBufferIndex;
    constants.rawGIIndex = rawGI.GetDescriptorIndex();
    constants.giHitDistanceIndex = giHitDistance.GetDescriptorIndex();
    constants.viewZOutputIndex = m_viewZTexture.GetUAVIndex();

    constants.motionOutputIndex = m_motionTexture.GetUAVIndex();
    constants.normalRoughnessOutputIndex = m_normalRoughnessTexture.GetUAVIndex();
    constants.diffRadianceHitDistOutputIndex = m_diffRadianceHitDistTexture.GetUAVIndex();
    constants.fullWidth = fullWidth;

    constants.fullHeight = fullHeight;
    constants.giWidth = giWidth;
    constants.giHeight = giHeight;
    constants.padding = 0;

    constants.hitDistanceA = 3.0f;
    constants.hitDistanceB = 0.1f;
    constants.hitDistanceC = 20.0f;
    constants.denoisingRange = denoisingRange;

    const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress = uploadBuffer.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(constantsAddress == 0)
    {
        return;
    }

    m_pipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, constantsAddress);

    const unsigned int groupCountX = (giWidth + 7) / 8;
    const unsigned int groupCountY = (giHeight + 7) / 8;
    commandList->Dispatch(groupCountX, groupCountY, 1);

    constexpr D3D12_RESOURCE_STATES nrdInputState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    m_viewZTexture.Transition(commandList, nrdInputState);
    m_motionTexture.Transition(commandList, nrdInputState);
    m_normalRoughnessTexture.Transition(commandList, nrdInputState);
    m_diffRadianceHitDistTexture.Transition(commandList, nrdInputState);
}

void NrdPreparePass::Shutdown()
{
    m_viewZTexture.Shutdown();
    m_motionTexture.Shutdown();
    m_normalRoughnessTexture.Shutdown();
    m_specRadianceHitDistTexture.Shutdown();
    m_diffRadianceHitDistTexture.Shutdown();
    m_pipeline.Shutdown();
}

bool NrdPreparePass::CreateTextures(
    GraphicsDevice& graphicsDevice,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    ID3D12Device* device = graphicsDevice.GetDevice();

    if(!m_viewZTexture.Initialize(device, width, height, 1, 1,
        DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        Log::Error("Failed to create NRD view-Z texture.");
        Shutdown();
        return false;
    }

    if(!m_viewZTexture.CreateSRV(device, descriptorHeap))
    {
        Log::Error("Failed to create NRD view-Z SRV.");
        Shutdown();
        return false;
    }

    if(!m_viewZTexture.CreateUAV(device, descriptorHeap))
    {
        Log::Error("Failed to create NRD view-Z UAV.");
        Shutdown();
        return false;
    }

    if(!m_motionTexture.Initialize(device, width, height, 1, 1,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        Log::Error("Failed to create NRD motion texture.");
        Shutdown();
        return false;
    }

    if(!m_motionTexture.CreateSRV(device, descriptorHeap))
    {
        Log::Error("Failed to create NRD motion SRV.");
        Shutdown();
        return false;
    }

    if(!m_motionTexture.CreateUAV(device, descriptorHeap))
    {
        Log::Error("Failed to create NRD motion UAV.");
        Shutdown();
        return false;
    }

    if(!m_normalRoughnessTexture.Initialize(device, width, height, 1, 1,
        DXGI_FORMAT_R10G10B10A2_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        Log::Error("Failed to create NRD normal/roughness texture.");
        Shutdown();
        return false;
    }

    if(!m_normalRoughnessTexture.CreateSRV(device, descriptorHeap))
    {
        Log::Error("Failed to create NRD normal/roughness SRV.");
        Shutdown();
        return false;
    }

    if(!m_normalRoughnessTexture.CreateUAV(device, descriptorHeap))
    {
        Log::Error("Failed to create NRD normal/roughness UAV.");
        Shutdown();
        return false;
    }

    if(m_signal == Signal::Specular){
        if(!m_specRadianceHitDistTexture.Initialize(device, width, height, 1, 1,
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            Log::Error("Failed to create NRD specular radiance/hit-distance texture.");
            Shutdown();
            return false;
        }

        if(!m_specRadianceHitDistTexture.CreateSRV(device, descriptorHeap))
        {
            Log::Error("Failed to create NRD specular radiance/hit-distance SRV.");
            Shutdown();
            return false;
        }

        if(!m_specRadianceHitDistTexture.CreateUAV(device, descriptorHeap))
        {
            Log::Error("Failed to create NRD specular radiance/hit-distance UAV.");
            Shutdown();
            return false;
        }
    }

    if(m_signal == Signal::Diffuse){
        if(!m_diffRadianceHitDistTexture.Initialize(device, width, height, 1, 1,
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            Log::Error("Failed to create NRD diffuse radiance/hit-distance texture.");
            Shutdown();
            return false;
        }

        if(!m_diffRadianceHitDistTexture.CreateSRV(device, descriptorHeap))
        {
            Log::Error("Failed to create NRD diffuse radiance/hit-distance SRV.");
            Shutdown();
            return false;
        }

        if(!m_diffRadianceHitDistTexture.CreateUAV(device, descriptorHeap))
        {
            Log::Error("Failed to create NRD diffuse radiance/hit-distance UAV.");
            Shutdown();
            return false;
        }
    }

    m_viewZTexture.GetResource()->SetName(
        L"NRD Prepare ViewZ");

    m_motionTexture.GetResource()->SetName(
        L"NRD Prepare Motion");

    m_normalRoughnessTexture.GetResource()->SetName(
        L"NRD Prepare NormalRoughness");

    if(m_signal == Signal::Specular)
    {
        m_specRadianceHitDistTexture.GetResource()->SetName(
            L"NRD Prepare SpecRadianceHitDist");
    }
    else
    {
        m_diffRadianceHitDistTexture.GetResource()->SetName(
            L"NRD Prepare DiffRadianceHitDist");
    }

    return true;
}

Texture& NrdPreparePass::GetViewZTexture()
{
    return m_viewZTexture;
}

const Texture& NrdPreparePass::GetViewZTexture() const
{
    return m_viewZTexture;
}

Texture& NrdPreparePass::GetMotionTexture()
{
    return m_motionTexture;
}

const Texture& NrdPreparePass::GetMotionTexture() const
{
    return m_motionTexture;
}

Texture& NrdPreparePass::GetNormalRoughnessTexture()
{
    return m_normalRoughnessTexture;
}

const Texture& NrdPreparePass::GetNormalRoughnessTexture() const
{
    return m_normalRoughnessTexture;
}

Texture& NrdPreparePass::GetSpecRadianceHitDistTexture()
{
    return m_specRadianceHitDistTexture;
}

const Texture& NrdPreparePass::GetSpecRadianceHitDistTexture() const
{
    return m_specRadianceHitDistTexture;
}

Texture& NrdPreparePass::GetDiffRadianceHitDistTexture()
{
    return m_diffRadianceHitDistTexture;
}

const Texture& NrdPreparePass::GetDiffRadianceHitDistTexture() const
{
    return m_diffRadianceHitDistTexture;
}
