#include "NrdHistoryConfidencePass.h"

#include "Core/Log.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/UploadBuffer.h"

#include "Renderer/Shader/ShaderLibrary.h"

namespace
{
    // NrdHistoryConfidence.hlsli NRD_CONFIDENCE_RECORD_BYTES.
    constexpr std::uint64_t RecordBytes = 32;

    Shader* LoadComputeShader(ShaderLibrary& shaderLibrary, const char* path, const char* entryPoint)
    {
        ShaderDescription shaderDescription{};
        shaderDescription.path = path;
        shaderDescription.entryPoint = entryPoint;
        shaderDescription.stage = ShaderStage::Compute;
        return shaderLibrary.Load(shaderDescription);
    }

    RootParameterDescription RootParameter(RootParameterType type, unsigned int shaderRegister)
    {
        RootParameterDescription parameter{};
        parameter.type = type;
        parameter.shaderRegister = shaderRegister;
        parameter.visibility = D3D12_SHADER_VISIBILITY_ALL;
        return parameter;
    }

    void UavBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = resource;
        commandList->ResourceBarrier(1, &barrier);
    }
}

bool NrdHistoryConfidencePass::Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary)
{
    if(m_initialized || m_initializationFailed)
    {
        return m_initialized;
    }

    // The probe pass: b0 probe constants, b1 scene lights, b2 RTXDI constants, t0 the TLAS, and the path tracer's
    // environment sampler at s1 (RayTracingPipeline.cpp: linear, U wraps, V clamps).
    ComputePipelineDescription probeDescription{};
    probeDescription.computeShader = LoadComputeShader(shaderLibrary, "NrdHistoryConfidenceProbe.hlsl", "CSProbe");
    probeDescription.rootParameters = {RootParameter(RootParameterType::ConstantBuffer, 0),
        RootParameter(RootParameterType::ConstantBuffer, 1), RootParameter(RootParameterType::ConstantBuffer, 2),
        RootParameter(RootParameterType::ShaderResource, 0)};

    D3D12_STATIC_SAMPLER_DESC environmentSampler{};
    environmentSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    environmentSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    environmentSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    environmentSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    environmentSampler.MaxAnisotropy = 1;
    environmentSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    environmentSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    environmentSampler.MaxLOD = D3D12_FLOAT32_MAX;
    environmentSampler.ShaderRegister = 1;
    environmentSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    probeDescription.staticSamplers.push_back(environmentSampler);

    ComputePipelineDescription filterDescription{};
    filterDescription.computeShader = LoadComputeShader(shaderLibrary, "NrdHistoryConfidenceFilter.hlsl", "CSFilter");
    filterDescription.rootParameters = {RootParameter(RootParameterType::ConstantBuffer, 0)};

    ID3D12Device* device = graphicsDevice.GetDevice();
    m_initialized = probeDescription.computeShader && filterDescription.computeShader &&
        m_probePipeline.Initialize(device, probeDescription) && m_filterPipeline.Initialize(device, filterDescription);

    if(!m_initialized)
    {
        Log::Error("The NRD history confidence probe pipelines could not be created.");
        m_probePipeline.Shutdown();
        m_filterPipeline.Shutdown();
        m_initializationFailed = true;
    }

    return m_initialized;
}

bool NrdHistoryConfidencePass::EnsureResources(
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height,
    bool& recreated)
{
    recreated = false;

    if(HasResources() && m_width == width && m_height == height)
    {
        return true;
    }

    ReleaseResources();

    if(!device || width == 0 || height == 0)
    {
        return false;
    }

    const unsigned int probeWidth = (width + 1) / 2;
    const unsigned int probeHeight = (height + 1) / 2;
    const std::uint64_t recordBytes = static_cast<std::uint64_t>(probeWidth) * probeHeight * RecordBytes;

    struct TextureDescription
    {
        Texture* texture;
        DXGI_FORMAT format;
        const wchar_t* name;
    };

    const TextureDescription textures[6] = {
        {&m_signal, DXGI_FORMAT_R32G32_FLOAT, L"NRD History Confidence Signal (diagnostic)"},
        {&m_geometry, DXGI_FORMAT_R32G32B32A32_FLOAT, L"NRD History Confidence Geometry (diagnostic)"},
        {&m_filter[0], DXGI_FORMAT_R32G32_FLOAT, L"NRD History Confidence Filter A (diagnostic)"},
        {&m_filter[1], DXGI_FORMAT_R32G32_FLOAT, L"NRD History Confidence Filter B (diagnostic)"},
        {&m_confidence, DXGI_FORMAT_R16_FLOAT, L"NRD History Confidence (diagnostic)"},
        {&m_state, DXGI_FORMAT_R8_UINT, L"NRD History Confidence Probe State (diagnostic)"}};

    bool created = m_records.InitializeGPUWritable(device, static_cast<size_t>(recordBytes)) &&
        m_records.CreateRawUAV(device, descriptorHeap);

    for(const TextureDescription& description : textures)
    {
        Texture& texture = *description.texture;
        created = created && texture.Initialize(device, probeWidth, probeHeight, 1, 1, description.format,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
            texture.CreateSRV(device, descriptorHeap) && texture.CreateUAV(device, descriptorHeap);

        if(created)
        {
            texture.GetResource()->SetName(description.name);
        }
    }

    if(!created)
    {
        Log::Error("The NRD history confidence probe resources could not be created.");
        ReleaseResources();
        return false;
    }

    m_records.GetResource()->SetName(L"NRD History Confidence Probe Records (diagnostic)");
    m_width = width;
    m_height = height;
    m_probeWidth = probeWidth;
    m_probeHeight = probeHeight;
    recreated = true;
    return true;
}

void NrdHistoryConfidencePass::ReleaseResources()
{
    m_records.Shutdown();
    m_signal.Shutdown();
    m_geometry.Shutdown();
    m_filter[0].Shutdown();
    m_filter[1].Shutdown();
    m_confidence.Shutdown();
    m_state.Shutdown();
    m_width = 0;
    m_height = 0;
    m_probeWidth = 0;
    m_probeHeight = 0;
}

void NrdHistoryConfidencePass::Shutdown()
{
    ReleaseResources();
    m_probePipeline.Shutdown();
    m_filterPipeline.Shutdown();
    m_initialized = false;
    m_initializationFailed = false;
}

bool NrdHistoryConfidencePass::Execute(
    ID3D12GraphicsCommandList* commandList,
    UploadBuffer& uploadBuffer,
    NrdHistoryConfidenceProbeConstants constants,
    D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstants,
    D3D12_GPU_VIRTUAL_ADDRESS rtxdiConstants,
    D3D12_GPU_VIRTUAL_ADDRESS topLevelAccelerationStructure,
    const float recordCameraPosition[3],
    std::uint32_t confidenceStrength)
{
    if(!m_initialized || !commandList || !HasResources() || sceneLightConstants == 0 || rtxdiConstants == 0 ||
        topLevelAccelerationStructure == 0)
    {
        return false;
    }

    constexpr D3D12_RESOURCE_STATES shaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const UINT groupsX = (m_probeWidth + 7u) / 8u;
    const UINT groupsY = (m_probeHeight + 7u) / 8u;

    // Probe pass. The records stay in unordered-access state across frames; the barrier orders the previous frame's
    // writes before this frame's reads (same queue, so the frames themselves are already ordered).
    m_records.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    UavBarrier(commandList, m_records.GetResource());
    m_signal.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_geometry.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_state.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    constants.resolution[0] = m_width;
    constants.resolution[1] = m_height;
    constants.probeResolution[0] = m_probeWidth;
    constants.probeResolution[1] = m_probeHeight;
    constants.recordBufferIndex = m_records.GetUAVDescriptorIndex();
    constants.signalIndex = m_signal.GetUAVIndex();
    constants.geometryIndex = m_geometry.GetUAVIndex();
    constants.stateIndex = m_state.GetUAVIndex();

    const D3D12_GPU_VIRTUAL_ADDRESS probeAddress = uploadBuffer.Allocate(&constants,
        sizeof(NrdHistoryConfidenceProbeConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(probeAddress == 0)
    {
        Log::Error("Failed to allocate NRD history confidence probe constants.");
        return false;
    }

    m_probePipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, probeAddress);
    commandList->SetComputeRootConstantBufferView(1, sceneLightConstants);
    commandList->SetComputeRootConstantBufferView(2, rtxdiConstants);
    commandList->SetComputeRootShaderResourceView(3, topLevelAccelerationStructure);
    commandList->Dispatch(groupsX, groupsY, 1);

    UavBarrier(commandList, m_records.GetResource());
    m_signal.Transition(commandList, shaderRead);
    m_geometry.Transition(commandList, shaderRead);
    m_state.Transition(commandList, shaderRead);

    // Filter passes: signal -> A -> B -> A ..., strides 1, 2, 3; the last also writes the confidence.
    m_confidence.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Texture* input = &m_signal;

    for(std::uint32_t pass = 0; pass < FilterPasses; ++pass)
    {
        Texture& output = m_filter[pass & 1u];
        output.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        NrdHistoryConfidenceFilterConstants filter;
        filter.probeResolution[0] = m_probeWidth;
        filter.probeResolution[1] = m_probeHeight;
        filter.inputIndex = input->GetDescriptorIndex();
        filter.outputIndex = output.GetUAVIndex();
        filter.geometryIndex = m_geometry.GetDescriptorIndex();
        filter.confidenceIndex = m_confidence.GetUAVIndex();
        filter.step = pass + 1;
        filter.lastPass = pass + 1 == FilterPasses ? 1u : 0u;
        filter.recordCameraPosition[0] = recordCameraPosition[0];
        filter.recordCameraPosition[1] = recordCameraPosition[1];
        filter.recordCameraPosition[2] = recordCameraPosition[2];
        filter.sensitivity = Sensitivity;
        filter.minConfidence = MinConfidence;
        filter.planeSensitivity = PlaneSensitivity;
        filter.confidenceStrength = confidenceStrength;

        const D3D12_GPU_VIRTUAL_ADDRESS filterAddress = uploadBuffer.Allocate(&filter,
            sizeof(NrdHistoryConfidenceFilterConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(filterAddress == 0)
        {
            Log::Error("Failed to allocate NRD history confidence filter constants.");
            return false;
        }

        m_filterPipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, filterAddress);
        commandList->Dispatch(groupsX, groupsY, 1);

        output.Transition(commandList, shaderRead);
        input = &output;
    }

    m_confidence.Transition(commandList, shaderRead);
    return true;
}

std::uint64_t NrdHistoryConfidencePass::GetResourceBytes() const
{
    if(!HasResources())
    {
        return 0;
    }

    // Records 32, signal 8, geometry 16, two filter targets 8 each, confidence 2, state 1.
    const std::uint64_t probes = static_cast<std::uint64_t>(m_probeWidth) * m_probeHeight;
    return probes * (RecordBytes + 8 + 16 + 2 * 8 + 2 + 1);
}
