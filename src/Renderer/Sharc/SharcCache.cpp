#include "Renderer/Sharc/SharcCache.h"

#include "Core/Log.h"
#include "Debug/MemoryRegistry.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/UploadBuffer.h"

#include <climits>
#include <cstring>

namespace
{
    constexpr std::uint32_t GroupSize = 256;

    // NVIDIA's headers as the CMake build deploys them (ENGINE_SHARC_ROOT); absent, NVIDIA SHARC is unavailable.
    constexpr const char* RequiredHeaders[] = {"SharcCommon.h", "SharcTypes.h", "HashGridCommon.h", "HashGridTypes.h"};

    bool CreateStructuredBuffer(ID3D12Device* device, DescriptorHeap& descriptorHeap, Buffer& buffer, std::uint64_t bytes,
        std::uint32_t stride, const wchar_t* name)
    {
        if(bytes == 0 || bytes > UINT_MAX || !buffer.InitializeGPUWritable(device, static_cast<size_t>(bytes)) ||
            !buffer.CreateStructuredUAV(device, descriptorHeap, stride))
        {
            return false;
        }

        buffer.GetResource()->SetName(name);
        return true;
    }

    // invalidation: the pass also binds SharcNvInvalidationConstants at b1 (CSInvalidateRegions).
    bool CreatePipeline(ID3D12Device* device, ShaderLibrary& shaderLibrary, const char* entryPoint,
        ComputePipeline& pipeline, bool invalidation = false)
    {
        ShaderDescription shaderDescription{};
        shaderDescription.path = "SharcNv/SharcNvResolve.hlsl";
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

        if(invalidation)
        {
            RootParameterDescription regions = constants;
            regions.shaderRegister = 1;
            pipelineDescription.rootParameters.push_back(regions);
        }

        return computeShader && pipeline.Initialize(device, pipelineDescription);
    }
}

std::string SharcCache::CapabilityFailure(const GraphicsDevice& device, const std::filesystem::path& shaderRoot,
    bool packaged)
{
    if(!device.SupportsRayTracing())
    {
        return "NVIDIA SHARC needs DXR ray tracing.";
    }

    if(static_cast<unsigned int>(device.GetHighestShaderModel()) < static_cast<unsigned int>(D3D_SHADER_MODEL_6_6))
    {
        return "NVIDIA SHARC needs Shader Model 6.6.";
    }

    if(!device.SupportsInt64ShaderOps())
    {
        return "NVIDIA SHARC needs 64-bit integer shader operations (its hash keys).";
    }

    if(!device.SupportsAtomicInt64OnDescriptorHeapResources())
    {
        return "NVIDIA SHARC needs 64-bit atomics on descriptor-heap resources (its hash keys).";
    }

    if(!device.SupportsNative16BitShaderOps())
    {
        return "NVIDIA SHARC needs native 16-bit shader operations (its resolved radiance).";
    }

    // Packaged games hold NVIDIA SHARC as compiled DXIL; a missing cooked library fails its pipeline preparation instead.
    if(packaged)
    {
        return {};
    }

    for(const char* header : RequiredHeaders)
    {
        std::error_code error;

        if(!std::filesystem::is_regular_file(shaderRoot / "Sharc" / header, error))
        {
            return "NVIDIA SHARC's shader headers are not deployed (assets/Shaders/Sharc/" + std::string(header) +
                "); configure ENGINE_SHARC_ROOT with the v1.8.3 checkout.";
        }
    }

    return {};
}

bool SharcCache::IsValidSettings(const Settings& settings)
{
    return settings.capacityLog2 >= MinimumCapacityLog2 && settings.capacityLog2 <= MaximumCapacityLog2 &&
        settings.sceneScale > 0.0f && settings.accumulationFrames >= 1 && settings.accumulationFrames <= 1024 &&
        settings.staleFrames >= 8 && settings.staleFrames <= 1024 && settings.radianceScale > 0.0f;
}

bool SharcCache::EnsureResources(ID3D12Device* device, ShaderLibrary& shaderLibrary, DescriptorHeap& descriptorHeap,
    const Settings& settings, std::uint64_t lastSubmissionFence, bool failForTesting)
{
    // Only the buffer size is a property of the allocation; the other settings are constants. A change of what a stored
    // value means (the radiance scale, the grid) clears the contents.
    if(m_resources && m_settings.capacityLog2 == settings.capacityLog2)
    {
        if(m_settings.sceneScale != settings.sceneScale || m_settings.radianceScale != settings.radianceScale)
        {
            RequestClear();
        }

        m_settings = settings;
        return true;
    }

    Retire(lastSubmissionFence);

    if(!device || !IsValidSettings(settings) || !EnsurePipelines(device, shaderLibrary))
    {
        ++m_diagnostics.allocationFailures;
        return false;
    }

    const std::uint64_t entries = std::uint64_t{1} << settings.capacityLog2;
    Resources resources;
    const bool created = !failForTesting &&
        CreateStructuredBuffer(device, descriptorHeap, resources.hashEntries, entries * KeyStride, KeyStride,
            L"NVIDIA SHARC hash entries") &&
        CreateStructuredBuffer(device, descriptorHeap, resources.accumulation, entries * AccumulationStride,
            AccumulationStride, L"NVIDIA SHARC accumulation") &&
        CreateStructuredBuffer(device, descriptorHeap, resources.resolved, entries * ResolvedStride, ResolvedStride,
            L"NVIDIA SHARC resolved") &&
        resources.statistics.InitializeGPUWritable(device, 4 * StatisticWords) &&
        resources.statistics.CreateRawUAV(device, descriptorHeap);

    if(!created)
    {
        // Nothing was submitted with the partial set, so it is released at once.
        ReleaseResources(resources);
        ++m_diagnostics.allocationFailures;
        Log::Error("The NVIDIA SHARC buffers could not be created; rendering continues without the cache.");
        return false;
    }

    resources.statistics.GetResource()->SetName(L"NVIDIA SHARC statistics");
    m_resources = std::move(resources);
    m_settings = settings;
    m_clearNeeded = true;
    m_clearRecorded = false;
    ++m_diagnostics.allocations;
    return true;
}

void SharcCache::Retire(std::uint64_t lastSubmissionFence)
{
    m_usable = false;

    if(!m_resources)
    {
        return;
    }

    m_retired.push_back(RetiredResources{std::move(*m_resources), lastSubmissionFence});
    m_resources.reset();
    m_readbackRecorded[0] = false;
    m_readbackRecorded[1] = false;
    ++m_diagnostics.retirements;
}

void SharcCache::ReleaseRetired(std::uint64_t completedFence)
{
    for(size_t index = 0; index < m_retired.size();)
    {
        if(m_retired[index].lastUseFence <= completedFence)
        {
            ReleaseResources(m_retired[index].resources);
            m_retired.erase(m_retired.begin() + static_cast<std::ptrdiff_t>(index));
        }
        else
        {
            ++index;
        }
    }
}

void SharcCache::Shutdown()
{
    if(m_resources)
    {
        ReleaseResources(*m_resources);
        m_resources.reset();
    }

    for(RetiredResources& retired : m_retired)
    {
        ReleaseResources(retired.resources);
    }

    m_retired.clear();
    m_clearPipeline.Shutdown();
    m_clearStatisticsPipeline.Shutdown();
    m_accumulationStatisticsPipeline.Shutdown();
    m_resolvePipeline.Shutdown();
    m_resolvedStatisticsPipeline.Shutdown();
    m_invalidatePipeline.Shutdown();
    m_pendingRegions.clear();
    m_pipelinesReady = false;
    m_pipelinesFailed = false;
    m_readback.Reset();
    m_readbackRecorded[0] = false;
    m_readbackRecorded[1] = false;
    m_clearNeeded = true;
    m_clearRecorded = false;
    m_usable = false;
}

void SharcCache::BeginRecording()
{
    // A clear recorded by a recording that was never submitted did not run.
    m_clearNeeded = m_clearNeeded || m_clearRecorded;
    m_clearRecorded = false;
    m_usable = false;
}

void SharcCache::CommitSubmission()
{
    if(m_clearRecorded)
    {
        ++m_diagnostics.clearsCommitted;
    }

    m_clearRecorded = false;
}

void SharcCache::MarkSubmissionUncertain()
{
    m_clearNeeded = true;
    m_clearRecorded = false;
    m_usable = false;
    ++m_diagnostics.uncertainSubmissions;
}

void SharcCache::RequestClear()
{
    m_clearNeeded = true;
    m_pendingRegions.clear();
}

void SharcCache::RequestInvalidation(const std::vector<std::array<float, 4>>& regions)
{
    if(m_clearNeeded || regions.empty())
    {
        return;
    }

    if(m_pendingRegions.size() + regions.size() > MaximumInvalidationRegions)
    {
        RequestClear();
        return;
    }

    m_pendingRegions.insert(m_pendingRegions.end(), regions.begin(), regions.end());
}

SharcCache::Constants SharcCache::MakeConstants() const
{
    Constants constants{};

    if(!m_resources)
    {
        return constants;
    }

    constants.hashEntriesIndex = m_resources->hashEntries.GetUAVDescriptorIndex();
    constants.accumulationIndex = m_resources->accumulation.GetUAVDescriptorIndex();
    constants.resolvedIndex = m_resources->resolved.GetUAVDescriptorIndex();
    constants.capacity = 1u << m_settings.capacityLog2;
    constants.sceneScale = m_settings.sceneScale;
    constants.radianceScale = m_settings.radianceScale;
    constants.accumulationFrames = m_settings.accumulationFrames;

    constants.staleFrames = m_settings.staleFrames;
    constants.statisticsIndex = m_resources->statistics.GetUAVDescriptorIndex();
    return constants;
}

bool SharcCache::RecordFrameBegin(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer,
    const Constants& constants)
{
    m_usable = false;

    if(!m_resources || !m_pipelinesReady || !commandList)
    {
        return false;
    }

    Resources& resources = *m_resources;

    for(Buffer* buffer : {&resources.hashEntries, &resources.accumulation, &resources.resolved, &resources.statistics})
    {
        buffer->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    if(m_clearNeeded)
    {
        if(!RecordDispatch(commandList, uploadBuffer, constants, m_clearPipeline, constants.capacity))
        {
            return false;
        }

        RecordUAVBarrier(commandList);
        m_clearNeeded = false;
        m_clearRecorded = true;
        m_resolvedFrames = 0;
        ++m_diagnostics.clearsRecorded;
        m_pendingRegions.clear();
    }

    // Lighting discontinuities: before the update, so this frame's training refills the forgotten entries.
    if(!m_pendingRegions.empty())
    {
        InvalidationConstants invalidation{};
        invalidation.count[0] = static_cast<std::uint32_t>(m_pendingRegions.size());

        for(std::size_t region = 0; region < m_pendingRegions.size(); ++region)
        {
            std::memcpy(invalidation.regions[region], m_pendingRegions[region].data(), sizeof(invalidation.regions[region]));
        }

        if(!RecordDispatch(commandList, uploadBuffer, constants, m_invalidatePipeline, constants.capacity, &invalidation))
        {
            return false;
        }

        RecordUAVBarrier(commandList);
        ++m_diagnostics.invalidationsRecorded;
        m_diagnostics.regionsInvalidated += m_pendingRegions.size();
        m_pendingRegions.clear();
    }

    if((constants.flags & FlagStatistics) != 0)
    {
        if(!RecordDispatch(commandList, uploadBuffer, constants, m_clearStatisticsPipeline, StatisticWords))
        {
            return false;
        }

        RecordUAVBarrier(commandList);
    }

    m_usable = true;
    return true;
}

void SharcCache::RecordUAVBarrier(ID3D12GraphicsCommandList* commandList)
{
    if(!m_resources || !commandList)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER barriers[4]{};
    ID3D12Resource* resources[] = {m_resources->hashEntries.GetResource(), m_resources->accumulation.GetResource(),
        m_resources->resolved.GetResource(), m_resources->statistics.GetResource()};

    for(size_t index = 0; index < std::size(barriers); ++index)
    {
        barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[index].UAV.pResource = resources[index];
    }

    commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
}

bool SharcCache::RecordResolve(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer,
    const Constants& constants, std::uint32_t frameSlot)
{
    if(!m_usable || !commandList)
    {
        return false;
    }

    const bool statistics = (constants.flags & FlagStatistics) != 0;
    RecordUAVBarrier(commandList);

    if(statistics)
    {
        if(!RecordDispatch(commandList, uploadBuffer, constants, m_accumulationStatisticsPipeline, constants.capacity))
        {
            return false;
        }

        RecordUAVBarrier(commandList);
    }

    if(!RecordDispatch(commandList, uploadBuffer, constants, m_resolvePipeline, constants.capacity))
    {
        return false;
    }

    RecordUAVBarrier(commandList);
    ++m_resolvedFrames;
    ++m_diagnostics.resolvesRecorded;

    if(statistics)
    {
        if(!RecordDispatch(commandList, uploadBuffer, constants, m_resolvedStatisticsPipeline, constants.capacity))
        {
            return false;
        }

        RecordUAVBarrier(commandList);
    }

    return true;
}

void SharcCache::RecordStatisticsReadback(ID3D12GraphicsCommandList* commandList, const Constants& constants,
    std::uint32_t frameSlot)
{
    if(frameSlot >= ReadbackSlots)
    {
        return;
    }

    m_readbackRecorded[frameSlot] = false;

    if(!m_usable || !commandList || (constants.flags & FlagStatistics) == 0)
    {
        return;
    }

    ID3D12Device* device = nullptr;

    if(FAILED(commandList->GetDevice(IID_PPV_ARGS(&device))))
    {
        return;
    }

    const bool readback = EnsureReadback(device);
    device->Release();

    if(!readback)
    {
        return;
    }

    // The transition orders the render dispatch's counter writes before the copy.
    Buffer& buffer = m_resources->statistics;
    buffer.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyBufferRegion(m_readback.Get(), frameSlot * StatisticWords * 4u, buffer.GetResource(), 0,
        StatisticWords * 4u);
    buffer.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_readbackRecorded[frameSlot] = true;
}

bool SharcCache::ReadStatistics(std::uint32_t frameSlot, std::uint32_t (&words)[StatisticWords]) const
{
    if(!m_readback || frameSlot >= ReadbackSlots || !m_readbackRecorded[frameSlot])
    {
        return false;
    }

    const D3D12_RANGE range{frameSlot * StatisticWords * 4u, (frameSlot + 1) * StatisticWords * 4u};
    void* mapped = nullptr;

    if(FAILED(m_readback->Map(0, &range, &mapped)))
    {
        return false;
    }

    std::memcpy(words, static_cast<const std::uint8_t*>(mapped) + range.Begin, sizeof(words));
    const D3D12_RANGE written{0, 0};
    m_readback->Unmap(0, &written);
    return true;
}

void SharcCache::ReleaseResources(Resources& resources)
{
    resources.hashEntries.Shutdown();
    resources.accumulation.Shutdown();
    resources.resolved.Shutdown();
    resources.statistics.Shutdown();
}

bool SharcCache::EnsurePipelines(ID3D12Device* device, ShaderLibrary& shaderLibrary)
{
    if(m_pipelinesReady)
    {
        return true;
    }

    if(m_pipelinesFailed)
    {
        return false;
    }

    if(!CreatePipeline(device, shaderLibrary, "CSClear", m_clearPipeline) ||
        !CreatePipeline(device, shaderLibrary, "CSClearStatistics", m_clearStatisticsPipeline) ||
        !CreatePipeline(device, shaderLibrary, "CSAccumulationStatistics", m_accumulationStatisticsPipeline) ||
        !CreatePipeline(device, shaderLibrary, "CSResolve", m_resolvePipeline) ||
        !CreatePipeline(device, shaderLibrary, "CSResolvedStatistics", m_resolvedStatisticsPipeline) ||
        !CreatePipeline(device, shaderLibrary, "CSInvalidateRegions", m_invalidatePipeline, true))
    {
        Log::Error("The NVIDIA SHARC compute pipelines could not be created; the cache stays disabled.");
        m_clearPipeline.Shutdown();
        m_clearStatisticsPipeline.Shutdown();
        m_accumulationStatisticsPipeline.Shutdown();
        m_resolvePipeline.Shutdown();
        m_resolvedStatisticsPipeline.Shutdown();
        m_invalidatePipeline.Shutdown();
        m_pipelinesFailed = true;
        return false;
    }

    m_pipelinesReady = true;
    return true;
}

bool SharcCache::RecordDispatch(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer,
    const Constants& constants, const ComputePipeline& pipeline, std::uint32_t threads,
    const InvalidationConstants* invalidation)
{
    const D3D12_GPU_VIRTUAL_ADDRESS address = uploadBuffer.Allocate(&constants, sizeof(Constants),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(address == 0)
    {
        Log::Error("Failed to allocate NVIDIA SHARC constants.");
        return false;
    }

    D3D12_GPU_VIRTUAL_ADDRESS invalidationAddress = 0;

    if(invalidation)
    {
        invalidationAddress = uploadBuffer.Allocate(invalidation, sizeof(InvalidationConstants),
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(invalidationAddress == 0)
        {
            Log::Error("Failed to allocate the NVIDIA SHARC invalidation regions.");
            return false;
        }
    }

    pipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);

    if(invalidation)
    {
        commandList->SetComputeRootConstantBufferView(1, invalidationAddress);
    }

    commandList->Dispatch((threads + GroupSize - 1) / GroupSize, 1, 1);
    return true;
}

bool SharcCache::EnsureReadback(ID3D12Device* device)
{
    if(m_readback)
    {
        return true;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = ReadbackSlots * StatisticWords * 4u;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if(FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&m_readback))))
    {
        return false;
    }

    MemoryRegistry::TrackResource(m_readback);
    m_readback->SetName(L"NVIDIA SHARC statistics readback");
    return true;
}
