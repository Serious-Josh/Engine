#include "NrdPathTracePreparePass.h"

#include "Core/Log.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/UploadBuffer.h"

#include "Renderer/Shader/ShaderLibrary.h"

namespace
{
    bool CreatePipeline(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary, const char* entryPoint,
        ComputePipeline& pipeline)
    {
        ShaderDescription shaderDescription{};
        shaderDescription.path = "NrdPathTracePrepare.hlsl";
        shaderDescription.entryPoint = entryPoint;
        shaderDescription.stage = ShaderStage::Compute;
        Shader* computeShader = shaderLibrary.Load(shaderDescription);

        ComputePipelineDescription pipelineDescription{};
        pipelineDescription.computeShader = computeShader;

        RootParameterDescription constants{};
        constants.type = RootParameterType::ConstantBuffer;
        constants.shaderRegister = 0;
        constants.visibility = D3D12_SHADER_VISIBILITY_ALL;
        pipelineDescription.rootParameters.push_back(constants);

        return computeShader && pipeline.Initialize(graphicsDevice.GetDevice(), pipelineDescription);
    }
}

bool NrdPathTracePreparePass::Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary)
{
    if(m_initialized)
    {
        return true;
    }

    if(m_initializationFailed)
    {
        return false;
    }

    if(!CreatePipeline(graphicsDevice, shaderLibrary, "CSPrepare", m_preparePipeline) ||
        !CreatePipeline(graphicsDevice, shaderLibrary, "CSResolve", m_resolvePipeline) ||
        !CreatePipeline(graphicsDevice, shaderLibrary, "CSResolveDenoised", m_denoisedResolvePipeline))
    {
        Log::Error("The path-traced NRD preparation pass could not be created.");
        m_preparePipeline.Shutdown();
        m_resolvePipeline.Shutdown();
        m_denoisedResolvePipeline.Shutdown();
        m_initializationFailed = true;
        return false;
    }

    m_initialized = true;
    return true;
}

bool NrdPathTracePreparePass::EnsureTextures(
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height,
    bool reconstructions)
{
    const bool hasReconstructions = m_reconstructedDiffuse.GetResource() != nullptr;

    if(HasTextures() && m_width == width && m_height == height && (hasReconstructions || !reconstructions))
    {
        return true;
    }

    ReleaseTextures();

    struct TextureDescription
    {
        Texture* texture;
        DXGI_FORMAT format;
        const wchar_t* name;
    };

    const TextureDescription textures[7] = {
        {&m_normalRoughness, DXGI_FORMAT_R10G10B10A2_UNORM, L"NRD Path Trace Normal Roughness"},
        {&m_viewZ, DXGI_FORMAT_R32_FLOAT, L"NRD Path Trace View Z"},
        {&m_diffuse, DXGI_FORMAT_R16G16B16A16_FLOAT, L"NRD Path Trace Diffuse Radiance Hit Distance"},
        {&m_specular, DXGI_FORMAT_R16G16B16A16_FLOAT, L"NRD Path Trace Specular Radiance Hit Distance"},
        {&m_motion, DXGI_FORMAT_R16G16B16A16_FLOAT, L"NRD Path Trace Motion"},
        {&m_reconstructedDiffuse, DXGI_FORMAT_R32G32B32A32_FLOAT, L"NRD Path Trace Reconstructed Diffuse"},
        {&m_reconstructedSpecular, DXGI_FORMAT_R32G32B32A32_FLOAT, L"NRD Path Trace Reconstructed Specular"}};

    for(const TextureDescription& description : textures)
    {
        Texture& texture = *description.texture;

        if(!reconstructions && (&texture == &m_reconstructedDiffuse || &texture == &m_reconstructedSpecular))
        {
            continue;
        }


        if(!texture.Initialize(device, width, height, 1, 1, description.format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS) || !texture.CreateSRV(device, descriptorHeap) ||
            !texture.CreateUAV(device, descriptorHeap))
        {
            Log::Error("The path-traced NRD preparation textures could not be created.");
            ReleaseTextures();
            return false;
        }

        texture.GetResource()->SetName(description.name);
    }

    m_width = width;
    m_height = height;
    ++m_textureCreations;
    return true;
}

void NrdPathTracePreparePass::ReleaseTextures()
{
    m_normalRoughness.Shutdown();
    m_viewZ.Shutdown();
    m_diffuse.Shutdown();
    m_specular.Shutdown();
    m_motion.Shutdown();
    m_reconstructedDiffuse.Shutdown();
    m_reconstructedSpecular.Shutdown();

    m_width = 0;
    m_height = 0;
}

void NrdPathTracePreparePass::Shutdown()
{
    ReleaseTextures();
    m_preparePipeline.Shutdown();
    m_resolvePipeline.Shutdown();
    m_denoisedResolvePipeline.Shutdown();
    m_initialized = false;
    m_initializationFailed = false;
}

void NrdPathTracePreparePass::ExecutePrepare(
    ID3D12GraphicsCommandList* commandList,
    UploadBuffer& uploadBuffer,
    NrdPathTracePrepareConstants constants,
    unsigned int width,
    unsigned int height)
{
    if(!m_initialized || !commandList || !HasTextures() || width != m_width || height != m_height)
    {
        return;
    }

    constexpr D3D12_RESOURCE_STATES shaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Texture* outputs[5] = {&m_normalRoughness, &m_viewZ, &m_diffuse, &m_specular, &m_motion};

    for(Texture* output : outputs)
    {
        output->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    constants.diffuseIndex = m_diffuse.GetUAVIndex();
    constants.specularIndex = m_specular.GetUAVIndex();
    Dispatch(commandList, uploadBuffer, m_preparePipeline, constants, width, height);

    for(Texture* output : outputs)
    {
        output->Transition(commandList, shaderRead);
    }
}

void NrdPathTracePreparePass::ExecuteResolve(
    ID3D12GraphicsCommandList* commandList,
    UploadBuffer& uploadBuffer,
    NrdPathTracePrepareConstants constants,
    unsigned int width,
    unsigned int height)
{
    if(!m_initialized || !commandList || !HasTextures() || !m_reconstructedDiffuse.GetResource() || width != m_width ||
        height != m_height)
    {
        return;
    }

    constexpr D3D12_RESOURCE_STATES shaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_diffuse.Transition(commandList, shaderRead);
    m_specular.Transition(commandList, shaderRead);
    m_reconstructedDiffuse.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_reconstructedSpecular.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // The bypass reads the packed inputs where NRD's outputs would be.
    constants.diffuseIndex = m_diffuse.GetDescriptorIndex();
    constants.specularIndex = m_specular.GetDescriptorIndex();
    Dispatch(commandList, uploadBuffer, m_resolvePipeline, constants, width, height);

    m_reconstructedDiffuse.Transition(commandList, shaderRead);
    m_reconstructedSpecular.Transition(commandList, shaderRead);
}

void NrdPathTracePreparePass::ExecuteDenoisedResolve(
    ID3D12GraphicsCommandList* commandList,
    UploadBuffer& uploadBuffer,
    NrdPathTracePrepareConstants constants,
    Texture& denoisedDiffuse,
    Texture& denoisedSpecular,
    Texture& presentation,
    unsigned int width,
    unsigned int height)
{
    if(!m_initialized || !commandList || !HasTextures() || width != m_width || height != m_height)
    {
        return;
    }

    constexpr D3D12_RESOURCE_STATES shaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    denoisedDiffuse.Transition(commandList, shaderRead);
    denoisedSpecular.Transition(commandList, shaderRead);
    presentation.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    constants.denoisedDiffuseIndex = denoisedDiffuse.GetDescriptorIndex();
    constants.denoisedSpecularIndex = denoisedSpecular.GetDescriptorIndex();
    constants.presentationIndex = presentation.GetUAVIndex();
    Dispatch(commandList, uploadBuffer, m_denoisedResolvePipeline, constants, width, height);
}

void NrdPathTracePreparePass::Dispatch(
    ID3D12GraphicsCommandList* commandList,
    UploadBuffer& uploadBuffer,
    ComputePipeline& pipeline,
    NrdPathTracePrepareConstants& constants,
    unsigned int width,
    unsigned int height)
{
    constants.resolution[0] = width;
    constants.resolution[1] = height;
    constants.normalRoughnessIndex = m_normalRoughness.GetUAVIndex();
    constants.viewZIndex = m_viewZ.GetUAVIndex();
    constants.motionOutputIndex = m_motion.GetUAVIndex();
    constants.reconstructedDiffuseIndex = m_reconstructedDiffuse.GetUAVIndex();
    constants.reconstructedSpecularIndex = m_reconstructedSpecular.GetUAVIndex();

    const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress = uploadBuffer.Allocate(&constants,
        sizeof(NrdPathTracePrepareConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(constantsAddress == 0)
    {
        Log::Error("Failed to allocate path-traced NRD preparation constants.");
        return;
    }

    pipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, constantsAddress);
    commandList->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1);
}

std::uint64_t NrdPathTracePreparePass::GetResourceBytes() const
{
    const std::uint64_t pixels = static_cast<std::uint64_t>(m_width) * m_height;
    const std::uint64_t perPixel = InputBytesPerPixel +
        (m_reconstructedDiffuse.GetResource() ? ReconstructionBytesPerPixel : 0);
    return HasTextures() ? pixels * perPixel : 0;
}
