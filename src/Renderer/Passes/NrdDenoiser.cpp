#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer/Passes/NrdDenoiser.h"
#include "Debug/TransitionTiming.h"
#include "Renderer/DescriptorHeap.h"
#include "Math/Matrix4.h"
#include "Core/Log.h"

#include <NRDIntegration.hpp>

#include <cstring>
#include <limits>
#include <string>

namespace
{
    void StoreNrdMatrix(
        float destination[16],
        const Matrix4& matrix)
    {
        const Matrix4 transposed = matrix.Transpose();
        std::memcpy(destination, &transposed, sizeof(float) * 16);
    }

    // The radiance denoisers this wrapper supports (no SH, occlusion or SIGMA variants yet).
    bool IsSupportedDenoiser(nrd::Denoiser denoiser)
    {
        switch(denoiser)
        {
        case nrd::Denoiser::REBLUR_DIFFUSE:
        case nrd::Denoiser::REBLUR_SPECULAR:
        case nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR:
        case nrd::Denoiser::RELAX_DIFFUSE:
        case nrd::Denoiser::RELAX_SPECULAR:
        case nrd::Denoiser::RELAX_DIFFUSE_SPECULAR:
            return true;
        default:
            return false;
        }
    }

    bool HasDiffuseSignal(nrd::Denoiser denoiser)
    {
        return denoiser == nrd::Denoiser::REBLUR_DIFFUSE || denoiser == nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR ||
            denoiser == nrd::Denoiser::RELAX_DIFFUSE || denoiser == nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;
    }

    bool HasSpecularSignal(nrd::Denoiser denoiser)
    {
        return denoiser == nrd::Denoiser::REBLUR_SPECULAR || denoiser == nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR ||
            denoiser == nrd::Denoiser::RELAX_SPECULAR || denoiser == nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;
    }

    bool CreateOutput(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        Texture& output,
        uint32_t width,
        uint32_t height)
    {
        return output.Initialize(device, width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
            output.CreateSRV(device, descriptorHeap) &&
            output.CreateUAV(device, descriptorHeap);
    }
}

NrdDenoiser::~NrdDenoiser()
{
    Shutdown();
}

bool NrdDenoiser::Initialize(
    ID3D12Device* device,
    ID3D12CommandQueue* graphicsQueue,
    DescriptorHeap& descriptorHeap,
    uint32_t width,
    uint32_t height,
    const Description& description)
{
    if (m_device ||
        !device ||
        !graphicsQueue ||
        width == 0 ||
        height == 0 ||
        width > (std::numeric_limits<uint16_t>::max)() ||
        height > (std::numeric_limits<uint16_t>::max)())
    {
        return false;
    }

    if (description.queuedFrameCount == 0 ||
        description.queuedFrameCount > (std::numeric_limits<uint8_t>::max)() ||
        description.denoisers.empty() ||
        description.denoisers.size() > MaximumDenoisers)
    {
        return false;
    }

    for(size_t index = 0; index < description.denoisers.size(); ++index)
    {
        const DenoiserDescription& denoiser = description.denoisers[index];

        if (!IsSupportedDenoiser(denoiser.denoiser))
        {
            Log::Error("NRD instance '" + description.name + "' requests an unsupported denoiser.");
            return false;
        }

        // Reject before entering SDK creation: the native Integration failure cleanup is not safe for an
        // instance rejected for non-unique identifiers. No resources or existing owner have changed here.
        for(size_t previousIndex = 0; previousIndex < index; ++previousIndex)
        {
            if(description.denoisers[previousIndex].identifier == denoiser.identifier)
            {
                Log::Error("NRD instance '" + description.name + "' requests non-unique denoiser identifiers.");
                return false;
            }
        }
    }

    m_device = device;
    m_graphicsQueue = graphicsQueue;
    m_description = description;

    for (size_t index = 0; index < description.denoisers.size(); ++index)
    {
        m_denoisers[index].description = description.denoisers[index];
    }

    if (!Recreate(descriptorHeap, width, height))
    {
        Shutdown();
        return false;
    }

    return true;
}

bool NrdDenoiser::Resize(
    DescriptorHeap& descriptorHeap,
    uint32_t width,
    uint32_t height)
{
    return Recreate(descriptorHeap, width, height);
}

void NrdDenoiser::Shutdown()
{
    // Destroy waits for the GPU (autoWaitForIdle) before the outputs it may still be writing are released. An externally
    // synchronized instance (ER.1 Phase 3.1) does not wait: its caller proved that no submitted work references it.
    m_integration.Destroy();
    ReleaseOutputs();

    for (Denoiser& denoiser : m_denoisers)
    {
        denoiser.description = {};
    }

    m_description = {};
    m_device = nullptr;
    m_graphicsQueue = nullptr;

    m_width = 0;
    m_height = 0;
}

void NrdDenoiser::ReleaseOutputs()
{
    for (Denoiser& denoiser : m_denoisers)
    {
        denoiser.diffuseOutput.Shutdown();
        denoiser.specularOutput.Shutdown();
    }
}

bool NrdDenoiser::Recreate(
    DescriptorHeap& descriptorHeap,
    uint32_t width,
    uint32_t height)
{
    if (!m_device ||
        !m_graphicsQueue ||
        width == 0 ||
        height == 0 ||
        width > (std::numeric_limits<uint16_t>::max)() ||
        height > (std::numeric_limits<uint16_t>::max)())
    {
        return false;
    }

    // Destroy drains submitted work before its external outputs are released (an externally synchronized instance's
    // caller drained it instead: Renderer resizes only after WaitForGPU). Callers must also discard any
    // unsubmitted recording that referenced the old instance. The following SDK Recreate starts from empty state.
    m_integration.Destroy();
    ReleaseOutputs();
    m_width = 0;
    m_height = 0;

    nrd::IntegrationCreationDesc integrationDesc{};

    std::strncpy(integrationDesc.name, m_description.name.c_str(), sizeof(integrationDesc.name) - 1);

    integrationDesc.residencyPriority = 0.0f;
    integrationDesc.resourceWidth = static_cast<uint16_t>(width);
    integrationDesc.resourceHeight = static_cast<uint16_t>(height);
    integrationDesc.queuedFrameNum = static_cast<uint8_t>(m_description.queuedFrameCount);

    integrationDesc.enableWholeLifetimeDescriptorCaching = false;
    integrationDesc.autoWaitForIdle = !m_description.externallySynchronized;
    integrationDesc.demoteFloat32to16 = false;
    integrationDesc.promoteFloat16to32 = m_description.promoteFloat16to32;

    std::vector<nrd::DenoiserDesc> denoiserDescs;

    for (const DenoiserDescription& denoiser : m_description.denoisers)
    {
        denoiserDescs.push_back({denoiser.identifier, denoiser.denoiser});
    }

    nrd::InstanceCreationDesc instanceDesc{};
    instanceDesc.denoisers = denoiserDescs.data();
    instanceDesc.denoisersNum = static_cast<uint32_t>(denoiserDescs.size());

    nri::QueueFamilyD3D12Desc queueFamily{};
    queueFamily.d3d12Queues = &m_graphicsQueue;
    queueFamily.queueNum = 1;
    queueFamily.queueType = nri::QueueType::GRAPHICS;

    nri::DeviceCreationD3D12Desc deviceDesc{};
    deviceDesc.d3d12Device = m_device;
    deviceDesc.queueFamilies = &queueFamily;
    deviceDesc.queueFamilyNum = 1;

    deviceDesc.disableD3D12EnhancedBarriers = true;
    deviceDesc.disableNVAPIInitialization = true;

    TransitionTiming::Scope er1Scope("NRD RecreateD3D12 " + m_description.name);
    const nrd::Result result = m_integration.RecreateD3D12(integrationDesc, instanceDesc, deviceDesc);

    if (result != nrd::Result::SUCCESS)
    {
        Log::Error("Failed to recreate NRD instance '" + m_description.name + "'. nrd::Result: " +
                   std::to_string(static_cast<uint32_t>(result)));
        Shutdown();
        return false;
    }

    // Specular before diffuse, per denoiser: the descriptor order the reflection and GI instances always had.
    for (size_t index = 0; index < m_description.denoisers.size(); ++index)
    {
        Denoiser& denoiser = m_denoisers[index];

        if (HasSpecularSignal(denoiser.description.denoiser) &&
            !CreateOutput(m_device, descriptorHeap, denoiser.specularOutput, width, height))
        {
            Log::Error("Failed to create the specular output of NRD instance '" + m_description.name + "'.");
            Shutdown();
            return false;
        }

        if (HasDiffuseSignal(denoiser.description.denoiser) &&
            !CreateOutput(m_device, descriptorHeap, denoiser.diffuseOutput, width, height))
        {
            Log::Error("Failed to create the diffuse output of NRD instance '" + m_description.name + "'.");
            Shutdown();
            return false;
        }
    }

    m_width = width;
    m_height = height;

    return true;
}

bool NrdDenoiser::SetCommonSettings(const FrameSettings& settings)
{
    if (m_width == 0 || m_height == 0 || settings.denoisingRange <= 0.0f)
    {
        return false;
    }

    nrd::CommonSettings commonSettings{};

    StoreNrdMatrix(commonSettings.worldToViewMatrix, settings.view);
    StoreNrdMatrix(commonSettings.worldToViewMatrixPrev, settings.previousView);
    StoreNrdMatrix(commonSettings.viewToClipMatrix, settings.projection);
    StoreNrdMatrix(commonSettings.viewToClipMatrixPrev, settings.previousProjection);

    commonSettings.resourceSize[0] = static_cast<uint16_t>(m_width);
    commonSettings.resourceSize[1] = static_cast<uint16_t>(m_height);

    commonSettings.resourceSizePrev[0] = static_cast<uint16_t>(m_width);
    commonSettings.resourceSizePrev[1] = static_cast<uint16_t>(m_height);

    commonSettings.rectSize[0] = static_cast<uint16_t>(m_width);
    commonSettings.rectSize[1] = static_cast<uint16_t>(m_height);

    commonSettings.rectSizePrev[0] = static_cast<uint16_t>(m_width);
    commonSettings.rectSizePrev[1] = static_cast<uint16_t>(m_height);

    commonSettings.motionVectorScale[0] = settings.motionVectorScale[0];
    commonSettings.motionVectorScale[1] = settings.motionVectorScale[1];
    commonSettings.motionVectorScale[2] = settings.motionVectorScale[2];

    commonSettings.isMotionVectorInWorldSpace = false;

    commonSettings.cameraJitter[0] = settings.jitter[0];
    commonSettings.cameraJitter[1] = settings.jitter[1];
    commonSettings.cameraJitterPrev[0] = settings.previousJitter[0];
    commonSettings.cameraJitterPrev[1] = settings.previousJitter[1];

    commonSettings.viewZScale = 1.0f;
    commonSettings.denoisingRange = settings.denoisingRange;
    commonSettings.frameIndex = settings.frameIndex;
    commonSettings.accumulationMode = settings.accumulationMode;
    commonSettings.enableValidation = settings.enableValidation;
    commonSettings.isHistoryConfidenceAvailable = settings.historyConfidence;
    m_historyConfidence = settings.historyConfidence;

    return m_integration.SetCommonSettings(commonSettings) == nrd::Result::SUCCESS;
}

bool NrdDenoiser::SetDenoiserSettings(nrd::Identifier identifier, const nrd::ReblurSettings& settings)
{
    const Denoiser* denoiser = FindDenoiser(identifier);

    if (!denoiser)
    {
        return false;
    }

    const nrd::Denoiser type = denoiser->description.denoiser;
    const bool reblur = type == nrd::Denoiser::REBLUR_DIFFUSE || type == nrd::Denoiser::REBLUR_SPECULAR ||
        type == nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;

    return reblur && m_integration.SetDenoiserSettings(identifier, &settings) == nrd::Result::SUCCESS;
}

bool NrdDenoiser::SetDenoiserSettings(nrd::Identifier identifier, const nrd::RelaxSettings& settings)
{
    const Denoiser* denoiser = FindDenoiser(identifier);

    if (!denoiser)
    {
        return false;
    }

    const nrd::Denoiser type = denoiser->description.denoiser;
    const bool relax = type == nrd::Denoiser::RELAX_DIFFUSE || type == nrd::Denoiser::RELAX_SPECULAR ||
        type == nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;

    return relax && m_integration.SetDenoiserSettings(identifier, &settings) == nrd::Result::SUCCESS;
}

void NrdDenoiser::NewFrame()
{
    m_integration.NewFrame();
}

bool NrdDenoiser::Denoise(
    ID3D12GraphicsCommandList* commandList,
    nrd::Identifier identifier,
    const Inputs& inputs)
{
    Denoiser* denoiser = FindDenoiser(identifier);

    if (!commandList || !denoiser || !inputs.motion || !inputs.normalRoughness || !inputs.viewZ)
    {
        return false;
    }

    const bool diffuse = HasDiffuseSignal(denoiser->description.denoiser);
    const bool specular = HasSpecularSignal(denoiser->description.denoiser);

    if ((diffuse && !inputs.diffuseRadianceHitDistance) || (specular && !inputs.specularRadianceHitDistance))
    {
        return false;
    }

    if (specular)
    {
        denoiser->specularOutput.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    if (diffuse)
    {
        denoiser->diffuseOutput.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    nrd::ResourceSnapshot snapshot{};

    snapshot.restoreInitialState = true;

    const nri::AccessLayoutStage readState{nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE,
                                           nri::StageBits::COMPUTE_SHADER};

    const nri::AccessLayoutStage writeState{nri::AccessBits::SHADER_RESOURCE_STORAGE,
                                            nri::Layout::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER};

    auto setResource =
        [&](nrd::ResourceType slot, Texture& texture, DXGI_FORMAT format, const nri::AccessLayoutStage& state)
    {
        nrd::Resource resource{};

        resource.d3d12.resource = texture.GetResource();
        resource.d3d12.format = static_cast<DXGIFormat>(format);

        resource.state = state;

        snapshot.SetResource(slot, resource);
    };

    nrd::Resource motion{};
    motion.d3d12.resource = inputs.motion;
    motion.d3d12.format = static_cast<DXGIFormat>(DXGI_FORMAT_R16G16B16A16_FLOAT);
    motion.state = readState;
    snapshot.SetResource(nrd::ResourceType::IN_MV, motion);

    setResource(nrd::ResourceType::IN_NORMAL_ROUGHNESS, *inputs.normalRoughness, DXGI_FORMAT_R10G10B10A2_UNORM,
                readState);

    setResource(nrd::ResourceType::IN_VIEWZ, *inputs.viewZ, DXGI_FORMAT_R32_FLOAT, readState);

    if (specular)
    {
        setResource(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, *inputs.specularRadianceHitDistance,
                    DXGI_FORMAT_R16G16B16A16_FLOAT, readState);

        setResource(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, denoiser->specularOutput,
                    DXGI_FORMAT_R16G16B16A16_FLOAT, writeState);
    }

    if (diffuse)
    {
        setResource(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, *inputs.diffuseRadianceHitDistance,
                    DXGI_FORMAT_R16G16B16A16_FLOAT, readState);

        setResource(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, denoiser->diffuseOutput,
                    DXGI_FORMAT_R16G16B16A16_FLOAT, writeState);
    }

    // R.9A: the confidence-enabled frame binds both confidence inputs (RELAX samples IN_SPEC_CONFIDENCE too).
    if (m_historyConfidence)
    {
        if (!inputs.diffuseConfidence || (specular && !inputs.specularConfidence))
        {
            return false;
        }

        nrd::Resource confidence{};
        confidence.d3d12.format = static_cast<DXGIFormat>(DXGI_FORMAT_R16_FLOAT);
        confidence.state = readState;
        confidence.d3d12.resource = inputs.diffuseConfidence;
        snapshot.SetResource(nrd::ResourceType::IN_DIFF_CONFIDENCE, confidence);

        if (specular)
        {
            confidence.d3d12.resource = inputs.specularConfidence;
            snapshot.SetResource(nrd::ResourceType::IN_SPEC_CONFIDENCE, confidence);
        }
    }

    if (inputs.validation)
    {
        inputs.validation->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        setResource(nrd::ResourceType::OUT_VALIDATION, *inputs.validation, DXGI_FORMAT_R16G16B16A16_FLOAT, writeState);
    }

    nri::CommandBufferD3D12Desc commandBuffer{};

    commandBuffer.d3d12CommandList = commandList;

    // NRD binds its own descriptor heap and root signature; the caller restores Engine's heap afterwards.
    m_integration.DenoiseD3D12(&identifier, 1, commandBuffer, snapshot);
    return true;
}

NrdDenoiser::Denoiser* NrdDenoiser::FindDenoiser(nrd::Identifier identifier)
{
    for (size_t index = 0; index < m_description.denoisers.size(); ++index)
    {
        if (m_denoisers[index].description.identifier == identifier)
        {
            return &m_denoisers[index];
        }
    }

    return nullptr;
}

Texture& NrdDenoiser::GetDiffuseOutput(nrd::Identifier identifier)
{
    Denoiser* denoiser = FindDenoiser(identifier);
    return denoiser ? denoiser->diffuseOutput : m_emptyOutput;
}

Texture& NrdDenoiser::GetSpecularOutput(nrd::Identifier identifier)
{
    Denoiser* denoiser = FindDenoiser(identifier);
    return denoiser ? denoiser->specularOutput : m_emptyOutput;
}

double NrdDenoiser::GetTotalMemoryUsageInMb() const
{
    return m_integration.GetTotalMemoryUsageInMb();
}

double NrdDenoiser::GetPersistentMemoryUsageInMb() const
{
    return m_integration.GetPersistentMemoryUsageInMb();
}

double NrdDenoiser::GetAliasableMemoryUsageInMb() const
{
    return m_integration.GetAliasableMemoryUsageInMb();
}
