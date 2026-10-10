#include "GISignedCorrectionPass.h"

#include "Renderer/GraphicsDevice.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/UploadBuffer.h"
#include "Renderer/GBuffer.h"
#include "Renderer/Shader/ShaderLibrary.h"

bool GISignedCorrectionPass::Initialize(
    GraphicsDevice& device,
    ShaderLibrary& shaders,
    DescriptorHeap& heap,
    unsigned int width,
    unsigned int height)
{
    if(width == 0 || height == 0){
        return false;
    }

    ShaderDescription shaderDescription{};
    shaderDescription.path = "GISignedCorrection.hlsl";
    shaderDescription.entryPoint = "CSMain";
    shaderDescription.stage = ShaderStage::Compute;

    Shader* shader = shaders.Load(shaderDescription);

    if(!shader){
        return false;
    }

    ComputePipelineDescription description{};
    description.computeShader = shader;

    RootParameterDescription constants{};
    constants.type = RootParameterType::ConstantBuffer;
    constants.shaderRegister = 0;
    constants.visibility = D3D12_SHADER_VISIBILITY_ALL;

    description.rootParameters.push_back(constants);

    if(!m_pipeline.Initialize(device.GetDevice(), description))
    {
        Shutdown();
        return false;
    }

    shaderDescription.path = "GISignedReconstruct.hlsl";

    Shader* reconstructShader = shaders.Load(shaderDescription);
    description.computeShader = reconstructShader;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    description.staticSamplers.push_back(sampler);

    if(!reconstructShader || !m_reconstructPipeline.Initialize(device.GetDevice(), description) ||
       !CreateTextures(device, heap, width, height))
    {
        Shutdown();
        return false;
    }

    return true;
}

bool GISignedCorrectionPass::CreateTextures(
    GraphicsDevice& device,
    DescriptorHeap& heap,
    unsigned int width,
    unsigned int height)
{
    if(width == 0 || height == 0){
        return false;
    }

    const unsigned int giWidth = (width + 1u) / 2u;
    const unsigned int giHeight = (height + 1u) / 2u;

    Texture* textures[] = {&m_correction, &m_history[0], &m_history[1]};

    for(Texture* texture : textures)
    {
        if(!texture->Initialize(device.GetDevice(), giWidth, giHeight, 1, 1,
            DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
            !texture->CreateSRV(device.GetDevice(), heap) || !texture->CreateUAV(device.GetDevice(), heap))
        {
            for(Texture* allocated : textures){
                allocated->Shutdown();
            }

            return false;
        }
    }

    if(!m_previousDepth.Initialize(device.GetDevice(), width, height, 1, 1, DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST) ||
       !m_previousDepth.CreateSRV(device.GetDevice(), heap) ||
       !m_previousNormal.Initialize(device.GetDevice(), width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST) ||
       !m_previousNormal.CreateSRV(device.GetDevice(), heap) ||
       !m_reconstructed.Initialize(device.GetDevice(), giWidth, giHeight, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
       !m_reconstructed.CreateSRV(device.GetDevice(), heap) || !m_reconstructed.CreateUAV(device.GetDevice(), heap))
    {
        for(Texture* texture : textures){
            texture->Shutdown();
        }

        m_previousDepth.Shutdown();
        m_previousNormal.Shutdown();
        m_reconstructed.Shutdown();

        return false;
    }

    m_width = giWidth;
    m_height = giHeight;

    ResetHistory();

    return true;
}

bool GISignedCorrectionPass::Resize(GraphicsDevice& device, DescriptorHeap& heap,
    unsigned int width, unsigned int height)
{
    m_correction.Shutdown();

    for(Texture& history : m_history){
        history.Shutdown();
    }

    m_previousDepth.Shutdown();
    m_previousNormal.Shutdown();
    m_reconstructed.Shutdown();

    m_width = m_height = 0;

    ResetHistory();

    return CreateTextures(device, heap, width, height);
}

bool GISignedCorrectionPass::Execute(
    ID3D12GraphicsCommandList* commandList,
    UploadBuffer& uploadBuffer,
    GBuffer& gBuffer,
    const Matrix4& view,
    const Matrix4& projection)
{
    auto& depth = gBuffer.GetDepth();
    auto& normal = gBuffer.GetSurface();

    const unsigned int fullWidth = depth.GetWidth();
    const unsigned int fullHeight = depth.GetHeight();

    if(!commandList || fullWidth == 0 || fullHeight == 0 ||
       m_width != (fullWidth + 1u) / 2u || m_height != (fullHeight + 1u) / 2u ||
       !m_previousDepth.GetResource() || m_previousDepth.GetResource()->GetDesc().Width != fullWidth ||
       m_previousDepth.GetResource()->GetDesc().Height != fullHeight){
        return false;
    }

    if(m_historyValid)
    {
        const Matrix4 inverseView = view.Inverse();
        const Matrix4 previousInverseView = m_previousView.Inverse();

        float distanceSquared = 0.0f;
        float facingDot = 0.0f;

        bool projectionChanged = false;

        for(unsigned int i = 0; i < 3; ++i)
        {
            const float difference = inverseView.m[i][3] - previousInverseView.m[i][3];

            distanceSquared += difference * difference;
            facingDot += inverseView.m[i][2] * previousInverseView.m[i][2];
        }

        for(unsigned int i = 0; i < 4; ++i){
            for(unsigned int j = 0; j < 4; ++j){
                projectionChanged |= projection.m[i][j] != m_previousProjection.m[i][j];
            }
        }

        if(!std::isfinite(distanceSquared) || !std::isfinite(facingDot) || distanceSquared > 1.0f || facingDot < 0.5f || projectionChanged){
            ResetHistory();
        }
    }

    const Matrix4 viewProjection = projection * view;
    const Matrix4 inverseViewProjection = viewProjection.Inverse();

    const unsigned int writeIndex = 1u - m_readIndex;

    GISignedCorrectionConstants constants{};
    constants.inverseViewProjection = inverseViewProjection;
    constants.previousViewProjection = m_historyValid ? m_previousViewProjection : viewProjection;
    constants.previousInverseViewProjection = m_historyValid ? m_previousInverseViewProjection : inverseViewProjection;
    constants.correctionIndex = m_correction.GetDescriptorIndex();
    constants.previousHistoryIndex = m_history[m_readIndex].GetDescriptorIndex();
    constants.outputHistoryIndex = m_history[writeIndex].GetUAVIndex();
    constants.depthIndex = depth.GetSRVIndex();
    constants.normalIndex = normal.GetSRVIndex();
    constants.previousDepthIndex = m_previousDepth.GetDescriptorIndex();
    constants.previousNormalIndex = m_previousNormal.GetDescriptorIndex();
    constants.historyValid = m_historyValid ? 1u : 0u;
    constants.fullWidth = fullWidth;
    constants.fullHeight = fullHeight;
    constants.giWidth = m_width;
    constants.giHeight = m_height;

    const auto address = uploadBuffer.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(address == 0){
        return false;
    }

    m_correction.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    depth.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    normal.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if(m_historyValid)
    {
        m_history[m_readIndex].Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_previousDepth.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_previousNormal.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    m_history[writeIndex].Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    m_pipeline.Bind(commandList);

    commandList->SetComputeRootConstantBufferView(0, address);
    commandList->Dispatch((m_width + 7u) / 8u, (m_height + 7u) / 8u, 1);

    m_history[writeIndex].Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    depth.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    normal.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_previousDepth.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    m_previousNormal.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);

    commandList->CopyResource(m_previousDepth.GetResource(), depth.GetResource());
    commandList->CopyResource(m_previousNormal.GetResource(), normal.GetResource());

    m_previousDepth.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_previousNormal.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    depth.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    normal.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    m_readIndex = writeIndex;
    m_previousView = view;
    m_previousProjection = projection;
    m_previousViewProjection = viewProjection;
    m_previousInverseViewProjection = inverseViewProjection;
    m_historyValid = true;

    return true;
}

void GISignedCorrectionPass::ResetHistory()
{
    m_readIndex = 0;
    m_historyValid = false;
}

bool GISignedCorrectionPass::Reconstruct(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer,
    GBuffer& gBuffer, Texture& baseline, Texture& productionGI)
{
    if(!commandList || !m_historyValid || m_width == 0 || m_height == 0){
        return false;
    }

    GISignedReconstructConstants constants{};
    constants.historyIndex = m_history[m_readIndex].GetDescriptorIndex();
    constants.depthIndex = gBuffer.GetDepth().GetSRVIndex();
    constants.normalIndex = gBuffer.GetSurface().GetSRVIndex();
    constants.baselineIndex = baseline.GetDescriptorIndex();
    constants.productionGIIndex = productionGI.GetDescriptorIndex();
    constants.outputIndex = m_reconstructed.GetUAVIndex();
    constants.fullWidth = gBuffer.GetDepth().GetWidth();
    constants.fullHeight = gBuffer.GetDepth().GetHeight();
    constants.giWidth = m_width;
    constants.giHeight = m_height;

    const auto address = uploadBuffer.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(address == 0){
        return false;
    }

    m_history[m_readIndex].Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gBuffer.GetDepth().Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gBuffer.GetSurface().Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    constexpr auto readState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    baseline.Transition(commandList, readState);
    productionGI.Transition(commandList, readState);
    m_reconstructed.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    m_reconstructPipeline.Bind(commandList);

    commandList->SetComputeRootConstantBufferView(0, address);
    commandList->Dispatch((m_width + 7u) / 8u, (m_height + 7u) / 8u, 1);

    m_reconstructed.Transition(commandList, readState);

    return true;
}

void GISignedCorrectionPass::Shutdown()
{
    m_correction.Shutdown();

    for(Texture& history : m_history){
        history.Shutdown();
    }

    m_previousDepth.Shutdown();
    m_previousNormal.Shutdown();
    m_reconstructed.Shutdown();
    m_pipeline.Shutdown();
    m_reconstructPipeline.Shutdown();

    m_width = m_height = 0;

    ResetHistory();
}
