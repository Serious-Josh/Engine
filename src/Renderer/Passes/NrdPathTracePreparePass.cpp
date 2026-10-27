#include "NrdPathTracePreparePass.h"
#include "Debug/TransitionTiming.h"

#include "Core/Log.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/UploadBuffer.h"

#include "Renderer/Shader/ShaderLibrary.h"

namespace
{
    // signalViewConstants: a second root constant buffer at b1 (CSResolveRelaxSignalView only).
    bool CreatePipeline(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary, const char* entryPoint,
        ComputePipeline& pipeline, bool signalViewConstants = false)
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

        if(signalViewConstants)
        {
            constants.shaderRegister = 1;
            pipelineDescription.rootParameters.push_back(constants);
        }

        return computeShader && pipeline.Initialize(graphicsDevice.GetDevice(), pipelineDescription);
    }
}

bool NrdPathTracePreparePass::Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary, Encoding encoding)
{
    TransitionTiming::Scope er1Scope("NrdPathTracePreparePass::Initialize", 0.5);
    if(m_initialized || m_initializationFailed)
    {
        return m_initialized && m_encoding == encoding;
    }

    m_encoding = encoding;

    // HYBRID.5: the hybrid has its production resolve, but no bypass resolve.
    if(encoding == Encoding::Hybrid)
    {
        m_initialized = CreatePipeline(graphicsDevice, shaderLibrary, "CSPrepareHybrid", m_preparePipeline) &&
            CreatePipeline(graphicsDevice, shaderLibrary, "CSResolveDenoisedHybrid", m_denoisedResolvePipeline);
        m_initializationFailed = !m_initialized;

        if(!m_initialized)
        {
            Log::Error("The path-traced NRD hybrid preparation pass could not be created.");
            m_preparePipeline.Shutdown();
            m_denoisedResolvePipeline.Shutdown();
        }

        return m_initialized;
    }

    const bool relax = encoding == Encoding::Relax;

    // NRD.RELAX.3: RELAX's denoised resolve is the diagnostic CSResolveDenoisedRelax, never a presentation.
    if(!CreatePipeline(graphicsDevice, shaderLibrary, relax ? "CSPrepareRelax" : "CSPrepare", m_preparePipeline) ||
        !CreatePipeline(graphicsDevice, shaderLibrary, relax ? "CSResolveRelax" : "CSResolve", m_resolvePipeline) ||
        !CreatePipeline(graphicsDevice, shaderLibrary, relax ? "CSResolveDenoisedRelax" : "CSResolveDenoised",
            m_denoisedResolvePipeline))
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
    TransitionTiming::Scope er1Scope("NrdPathTracePreparePass::EnsureTextures", 0.5);
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
    m_signalViewPipeline.Shutdown();
    m_initialized = false;
    m_initializationFailed = false;
    m_signalViewReady = false;
    m_signalViewFailed = false;
    m_encoding = Encoding::Reblur;
}

bool NrdPathTracePreparePass::EnsureRelaxSignalView(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary)
{
    // RELAX-encoded diffuse inputs only: the RELAX and hybrid encodings.
    if(!m_initialized || m_encoding == Encoding::Reblur || m_signalViewFailed)
    {
        return m_signalViewReady;
    }

    if(!m_signalViewReady)
    {
        m_signalViewReady = CreatePipeline(graphicsDevice, shaderLibrary, "CSResolveRelaxSignalView",
            m_signalViewPipeline, true);
        m_signalViewFailed = !m_signalViewReady;

        if(m_signalViewFailed)
        {
            Log::Error("The NRD RELAX signal view pipeline could not be created.");
            m_signalViewPipeline.Shutdown();
        }
    }

    return m_signalViewReady;
}

void NrdPathTracePreparePass::ExecuteRelaxSignalView(
    ID3D12GraphicsCommandList* commandList,
    UploadBuffer& uploadBuffer,
    NrdPathTracePrepareConstants constants,
    const NrdRelaxSignalViewConstants& signalView,
    Texture& denoisedDiffuse,
    Texture& presentation,
    unsigned int width,
    unsigned int height)
{
    if(!m_signalViewReady || !commandList || !HasTextures() || width != m_width || height != m_height)
    {
        return;
    }

    constexpr D3D12_RESOURCE_STATES shaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_diffuse.Transition(commandList, shaderRead);
    denoisedDiffuse.Transition(commandList, shaderRead);
    presentation.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    constants.diffuseIndex = m_diffuse.GetDescriptorIndex();
    constants.denoisedDiffuseIndex = denoisedDiffuse.GetDescriptorIndex();
    constants.presentationIndex = presentation.GetUAVIndex();

    const D3D12_GPU_VIRTUAL_ADDRESS signalViewAddress = uploadBuffer.Allocate(&signalView,
        sizeof(NrdRelaxSignalViewConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(signalViewAddress == 0)
    {
        Log::Error("Failed to allocate NRD RELAX signal view constants.");
        return;
    }

    // Dispatch binds b0 only, so this entry point binds both root constant buffers itself.
    constants.resolution[0] = width;
    constants.resolution[1] = height;
    const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress = uploadBuffer.Allocate(&constants,
        sizeof(NrdPathTracePrepareConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(constantsAddress == 0)
    {
        Log::Error("Failed to allocate path-traced NRD preparation constants.");
        return;
    }

    m_signalViewPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, constantsAddress);
    commandList->SetComputeRootConstantBufferView(1, signalViewAddress);
    commandList->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1);
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
