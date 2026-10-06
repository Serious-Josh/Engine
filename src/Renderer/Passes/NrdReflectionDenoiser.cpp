#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer/Passes/NrdReflectionDenoiser.h"
#include "Renderer/DescriptorHeap.h"
#include "Math/Matrix4.h"

#include <NRDIntegration.hpp>

#include <cstring>

namespace
{
    void StoreNrdMatrix(float destination[16], const Matrix4& matrix)
    {
        const Matrix4 transposed = matrix.Transpose();
        std::memcpy(destination, &transposed, sizeof(float) * 16);
    }
}

bool NrdReflectionDenoiser::Initialize(ID3D12Device* device, ID3D12CommandQueue* graphicsQueue, DescriptorHeap& descriptorHeap, uint32_t width, uint32_t height, uint32_t queuedFrameCount)
{
    if(!device || !graphicsQueue || width == 0 || height == 0 || queuedFrameCount == 0){
        return false;
    }

    m_device = device;
    m_graphicsQueue = graphicsQueue;
    m_queuedFrameCount = queuedFrameCount;

    if(!Recreate(descriptorHeap, width, height)){
        m_device = nullptr;
        m_graphicsQueue = nullptr;
        m_queuedFrameCount = 0;
        return false;
    }

    return true;
}

bool NrdReflectionDenoiser::Resize(DescriptorHeap& descriptorHeap, uint32_t width, uint32_t height)
{
    return Recreate(descriptorHeap, width, height);
}

void NrdReflectionDenoiser::Shutdown()
{
    m_outputTexture.Shutdown();
    m_Integration.Destroy();

    m_device = nullptr;
    m_graphicsQueue = nullptr;
    m_queuedFrameCount = 0;
    m_width = 0;
    m_height = 0;
}

bool NrdReflectionDenoiser::Recreate(DescriptorHeap& descriptorHeap, uint32_t width, uint32_t height)
{
    if(!m_device || !m_graphicsQueue || width == 0 || height == 0 || m_queuedFrameCount == 0){
        return false;
    }

    m_outputTexture.Shutdown();

    nrd::IntegrationCreationDesc integrationDesc{};

    std::strncpy(integrationDesc.name, "ReflectionREBLUR", sizeof(integrationDesc.name) - 1);

    integrationDesc.residencyPriority = 0.0f;
    integrationDesc.resourceWidth = static_cast<uint16_t>(width);
    integrationDesc.resourceHeight = static_cast<uint16_t>(height);
    integrationDesc.queuedFrameNum = static_cast<uint8_t>(m_queuedFrameCount);

    integrationDesc.enableWholeLifetimeDescriptorCaching = false;
    integrationDesc.autoWaitForIdle = true;
    integrationDesc.demoteFloat32to16 = false;
    integrationDesc.promoteFloat16to32 = false;

    constexpr nrd::Identifier kDenoiserIdentifier = 0;

    nrd::DenoiserDesc denoiserDesc{};
    denoiserDesc.identifier = kDenoiserIdentifier;
    denoiserDesc.denoiser = nrd::Denoiser::REBLUR_SPECULAR;

    nrd::InstanceCreationDesc instanceDesc{};
    instanceDesc.denoisers = &denoiserDesc;
    instanceDesc.denoisersNum = 1;

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

    const nrd::Result result = m_Integration.RecreateD3D12(integrationDesc, instanceDesc, deviceDesc);

    if(result != nrd::Result::SUCCESS){
        return false;
    }

    if(!m_outputTexture.Initialize(m_device, width, height, 1, 1,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        return false;
    }

    if(!m_outputTexture.CreateSRV(m_device, descriptorHeap)){
        return false;
    }

    if(!m_outputTexture.CreateUAV(m_device, descriptorHeap)){
        return false;
    }

    m_width = width;
    m_height = height;

    return true;
}

bool NrdReflectionDenoiser::SetFrameSettings(
    const Matrix4& view,
    const Matrix4& projection,
    const Matrix4& previousView,
    const Matrix4& previousProjection,
    uint32_t frameIndex,
    float denoisingRange,
    bool resetHistory)
{
    if(m_width == 0 || m_height == 0 || denoisingRange <= 0.0f)
    {
        return false;
    }

    nrd::CommonSettings commonSettings{};

    StoreNrdMatrix(commonSettings.worldToViewMatrix, view);
    StoreNrdMatrix(commonSettings.worldToViewMatrixPrev, previousView);
    StoreNrdMatrix(commonSettings.viewToClipMatrix, projection);
    StoreNrdMatrix(commonSettings.viewToClipMatrixPrev, previousProjection);

    commonSettings.resourceSize[0] = static_cast<uint16_t>(m_width);
    commonSettings.resourceSize[1] = static_cast<uint16_t>(m_height);

    commonSettings.resourceSizePrev[0] = static_cast<uint16_t>(m_width);
    commonSettings.resourceSizePrev[1] = static_cast<uint16_t>(m_height);

    commonSettings.rectSize[0] = static_cast<uint16_t>(m_width);
    commonSettings.rectSize[1] = static_cast<uint16_t>(m_height);

    commonSettings.rectSizePrev[0] = static_cast<uint16_t>(m_width);
    commonSettings.rectSizePrev[1] = static_cast<uint16_t>(m_height);

    commonSettings.motionVectorScale[0] = 1.0f;
    commonSettings.motionVectorScale[1] = 1.0f;
    commonSettings.motionVectorScale[2] = 1.0f;

    commonSettings.isMotionVectorInWorldSpace = true;

    commonSettings.cameraJitter[0] = 0.0f;
    commonSettings.cameraJitter[1] = 0.0f;
    commonSettings.cameraJitterPrev[0] = 0.0f;
    commonSettings.cameraJitterPrev[1] = 0.0f;

    commonSettings.viewZScale = 1.0f;
    commonSettings.denoisingRange = denoisingRange;
    commonSettings.frameIndex = frameIndex;

    commonSettings.accumulationMode = resetHistory ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;

    const nrd::Result commonResult = m_Integration.SetCommonSettings(commonSettings);

    if(commonResult != nrd::Result::SUCCESS){
        return false;
    }

    nrd::ReblurSettings reblurSettings{};

    reblurSettings.hitDistanceParameters.A = 3.0f;
    reblurSettings.hitDistanceParameters.B = 0.1f;
    reblurSettings.hitDistanceParameters.C = 20.0f;
    reblurSettings.maxAccumulatedFrameNum = 8;
    reblurSettings.checkerboardMode = nrd::CheckerboardMode::OFF;
    reblurSettings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::OFF;

    constexpr nrd::Identifier kDenoiserIdentifier = 0;

    const nrd::Result reblurResult =m_Integration.SetDenoiserSettings(kDenoiserIdentifier, &reblurSettings);

    return reblurResult == nrd::Result::SUCCESS;
}

void NrdReflectionDenoiser::NewFrame()
{
    m_Integration.NewFrame();
}

void NrdReflectionDenoiser::Denoise(
    ID3D12GraphicsCommandList* commandList,
    Texture& motion,
    Texture& normalRoughness,
    Texture& viewZ,
    Texture& specRadianceHitDist)
{
    if(!commandList){
        return;
    }

    m_outputTexture.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    nrd::ResourceSnapshot snapshot{};
    snapshot.restoreInitialState = true;

    const nri::AccessLayoutStage readState{
        nri::AccessBits::SHADER_RESOURCE,
        nri::Layout::SHADER_RESOURCE,
        nri::StageBits::COMPUTE_SHADER
    };

    const nri::AccessLayoutStage writeState{
        nri::AccessBits::SHADER_RESOURCE_STORAGE,
        nri::Layout::SHADER_RESOURCE_STORAGE,
        nri::StageBits::COMPUTE_SHADER
    };

    auto setResource =
        [&](
            nrd::ResourceType slot,
            Texture& texture,
            DXGI_FORMAT format,
            const nri::AccessLayoutStage& state)
        {
            nrd::Resource resource{};

            resource.d3d12.resource = texture.GetResource();
            resource.d3d12.format = static_cast<DXGIFormat>(format);

            resource.state = state;

            snapshot.SetResource(slot, resource);
        };

    setResource(
        nrd::ResourceType::IN_MV,
        motion,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        readState);

    setResource(
        nrd::ResourceType::IN_NORMAL_ROUGHNESS,
        normalRoughness,
        DXGI_FORMAT_R10G10B10A2_UNORM,
        readState);

    setResource(
        nrd::ResourceType::IN_VIEWZ,
        viewZ,
        DXGI_FORMAT_R32_FLOAT,
        readState);

    setResource(
        nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST,
        specRadianceHitDist,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        readState);

    setResource(
        nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST,
        m_outputTexture,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        writeState);

    nri::CommandBufferD3D12Desc commandBuffer{};
    commandBuffer.d3d12CommandList = commandList;

    constexpr nrd::Identifier kDenoiserIdentifier = 0;

    m_Integration.DenoiseD3D12(&kDenoiserIdentifier, 1, commandBuffer, snapshot);
}

Texture& NrdReflectionDenoiser::GetOutputTexture()
{
    return m_outputTexture;
}

const Texture& NrdReflectionDenoiser::GetOutputTexture() const
{
    return m_outputTexture;
}

double NrdReflectionDenoiser::GetTotalMemoryUsageInMb() const
{
    return m_Integration.GetTotalMemoryUsageInMb();
}

double NrdReflectionDenoiser::GetPersistentMemoryUsageInMb() const
{
    return m_Integration.GetPersistentMemoryUsageInMb();
}

double NrdReflectionDenoiser::GetAliasableMemoryUsageInMb() const
{
    return m_Integration.GetAliasableMemoryUsageInMb();
}