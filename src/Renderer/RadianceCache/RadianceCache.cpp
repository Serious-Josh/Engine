#include "RadianceCache.h"

#include "Core/Log.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/GraphicsDevice.h"
#include "Renderer/UploadBuffer.h"

#include "Renderer/Shader/ShaderLibrary.h"

#include <algorithm>
#include <bit>
#include <climits>
#include <string>

namespace
{
    constexpr std::uint32_t ResolveGroupSize = 64;
    constexpr std::uint32_t ClearWordsPerGroup = 64 * 4;

    bool CreatePipeline(
        GraphicsDevice& graphicsDevice,
        ShaderLibrary& shaderLibrary,
        const char* entryPoint,
        ComputePipeline& pipeline)
    {
        ShaderDescription shaderDescription{};
        shaderDescription.path = "RadianceCacheResolve.hlsl";
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

    bool CreateRawBuffer(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        Buffer& buffer,
        std::uint64_t bytes,
        bool readView,
        const wchar_t* name)
    {
        if(bytes == 0 || bytes > UINT_MAX)
        {
            return false;
        }

        if(!buffer.InitializeGPUWritable(device, static_cast<size_t>(bytes)) ||
            !buffer.CreateRawUAV(device, descriptorHeap))
        {
            return false;
        }

        if(readView && !buffer.CreateRawSRV(device, descriptorHeap))
        {
            return false;
        }

        buffer.GetResource()->SetName(name);
        return true;
    }
}

bool RadianceCache::Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary)
{
    if(m_initialized)
    {
        return true;
    }

    if(m_initializationFailed)
    {
        return false;
    }

    if(!CreatePipeline(graphicsDevice, shaderLibrary, "CSResolve", m_resolvePipeline) ||
        !CreatePipeline(graphicsDevice, shaderLibrary, "CSClear", m_clearPipeline))
    {
        Log::Error("The radiance cache pipelines could not be created.");
        m_resolvePipeline.Shutdown();
        m_clearPipeline.Shutdown();
        m_initializationFailed = true;
        return false;
    }

    m_initialized = true;
    return true;
}

bool RadianceCache::CreateResources(
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    const RadianceCacheSettings& settings,
    std::uint64_t lastSubmissionFence,
    std::uint64_t completedFence)
{
    const bool validSlotCount = std::has_single_bit(settings.slotCount) && settings.slotCount >= MinimumSlotCount;
    const bool validCellSize = settings.cellSize > 0.0f && settings.cellSize < 1.0e6f;

    if(!device || !validSlotCount || !validCellSize || !(settings.maximumSampleCount >= 1.0f))
    {
        Log::Error("Invalid radiance cache settings.");
        return false;
    }

    ReleaseRetired(completedFence);

    // Submitted work may still use the current buffers through their descriptors, so they are kept until it completes.
    if(HasResources())
    {
        m_retired.push_back(RetiredResources{std::move(m_resources), lastSubmissionFence});
    }

    const std::uint64_t slots = settings.slotCount;
    const std::uint64_t buckets = slots / BucketSlots;
    const bool created =
        CreateRawBuffer(device, descriptorHeap, m_resources.tags, slots * 4, true, L"Radiance Cache Tags") &&
        CreateRawBuffer(device, descriptorHeap, m_resources.keys, slots * 8, true, L"Radiance Cache Keys") &&
        CreateRawBuffer(device, descriptorHeap, m_resources.accumulation, slots * 32, false,
            L"Radiance Cache Accumulation") &&
        CreateRawBuffer(device, descriptorHeap, m_resources.published, slots * 32, true, L"Radiance Cache Published") &&
        CreateRawBuffer(device, descriptorHeap, m_resources.bucketFlags, buckets * 4, false,
            L"Radiance Cache Bucket Flags") &&
        CreateRawBuffer(device, descriptorHeap, m_resources.statistics, StatisticCount * 4, false,
            L"Radiance Cache Statistics");

    if(!created)
    {
        Log::Error("The radiance cache buffers could not be created (" + std::to_string(settings.slotCount) +
            " slots).");
        ReleaseResources(m_resources);
        return false;
    }

    m_settings = settings;
    return true;
}

void RadianceCache::ReleaseRetired(std::uint64_t completedFence)
{
    std::erase_if(m_retired,
        [&](RetiredResources& retired)
        {
            if(retired.submissionFence > completedFence)
            {
                return false;
            }

            ReleaseResources(retired.resources);
            return true;
        });
}

void RadianceCache::ReleaseResources(Resources& resources)
{
    resources.tags.Shutdown();
    resources.keys.Shutdown();
    resources.accumulation.Shutdown();
    resources.published.Shutdown();
    resources.bucketFlags.Shutdown();
    resources.statistics.Shutdown();
}

void RadianceCache::Shutdown()
{
    for(RetiredResources& retired : m_retired)
    {
        ReleaseResources(retired.resources);
    }

    m_retired.clear();
    ReleaseResources(m_resources);
    m_resolvePipeline.Shutdown();
    m_clearPipeline.Shutdown();
    m_initialized = false;
    m_initializationFailed = false;
}

RadianceCacheBinding RadianceCache::GetBinding(std::uint32_t frameIndex) const
{
    RadianceCacheBinding binding{};
    binding.tagsIndex = m_resources.tags.GetUAVDescriptorIndex();
    binding.keysIndex = m_resources.keys.GetUAVDescriptorIndex();
    binding.accumulationIndex = m_resources.accumulation.GetUAVDescriptorIndex();
    binding.publishedIndex = m_resources.published.GetUAVDescriptorIndex();
    binding.bucketFlagsIndex = m_resources.bucketFlags.GetUAVDescriptorIndex();
    binding.statisticsIndex = m_resources.statistics.GetUAVDescriptorIndex();
    binding.bucketCount = m_settings.slotCount / BucketSlots;
    binding.frameIndex = frameIndex;
    binding.tagsReadIndex = m_resources.tags.GetDescriptorIndex();
    binding.keysReadIndex = m_resources.keys.GetDescriptorIndex();
    binding.publishedReadIndex = m_resources.published.GetDescriptorIndex();
    binding.staleFrames = m_settings.staleFrames;
    binding.cellSize = m_settings.cellSize;
    binding.inverseCellSize = 1.0f / m_settings.cellSize;
    binding.maximumSampleCount = m_settings.maximumSampleCount;
    binding.varianceBlend = m_settings.varianceBlend;
    return binding;
}

bool RadianceCache::RecordClearBuffer(
    ID3D12GraphicsCommandList* commandList,
    UploadBuffer& uploadBuffer,
    const Buffer& buffer,
    std::uint64_t bytes)
{
    RadianceCacheConstants constants{};
    constants.cache = GetBinding(0);
    constants.clearIndex = buffer.GetUAVDescriptorIndex();
    constants.clearWordCount = static_cast<std::uint32_t>(bytes / 4);

    const D3D12_GPU_VIRTUAL_ADDRESS address = uploadBuffer.Allocate(&constants, sizeof(RadianceCacheConstants),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(address == 0)
    {
        Log::Error("Failed to allocate radiance cache clear constants.");
        return false;
    }

    commandList->SetComputeRootConstantBufferView(0, address);
    commandList->Dispatch((constants.clearWordCount + ClearWordsPerGroup - 1) / ClearWordsPerGroup, 1, 1);
    return true;
}

bool RadianceCache::RecordClear(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer,
    bool includeStatistics)
{
    if(!m_initialized || !commandList || !HasResources())
    {
        return false;
    }

    BeginUpdate(commandList);
    m_clearPipeline.Bind(commandList);

    const std::uint64_t slots = m_settings.slotCount;
    bool recorded = RecordClearBuffer(commandList, uploadBuffer, m_resources.tags, slots * 4) &&
        RecordClearBuffer(commandList, uploadBuffer, m_resources.keys, slots * 8) &&
        RecordClearBuffer(commandList, uploadBuffer, m_resources.accumulation, slots * 32) &&
        RecordClearBuffer(commandList, uploadBuffer, m_resources.published, slots * 32) &&
        RecordClearBuffer(commandList, uploadBuffer, m_resources.bucketFlags, slots / BucketSlots * 4);

    if(recorded && includeStatistics)
    {
        recorded = RecordClearBuffer(commandList, uploadBuffer, m_resources.statistics, StatisticCount * 4);
    }

    // The clear completes before any later update, resolve or query touches the buffers.
    RecordUAVBarriers(commandList);
    return recorded;
}

void RadianceCache::BeginUpdate(ID3D12GraphicsCommandList* commandList)
{
    m_resources.tags.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_resources.keys.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_resources.accumulation.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_resources.published.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_resources.bucketFlags.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_resources.statistics.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void RadianceCache::RecordUAVBarriers(ID3D12GraphicsCommandList* commandList)
{
    const Buffer* buffers[] = {&m_resources.tags, &m_resources.keys, &m_resources.accumulation,
        &m_resources.published, &m_resources.bucketFlags, &m_resources.statistics};
    D3D12_RESOURCE_BARRIER barriers[std::size(buffers)]{};

    for(std::size_t index = 0; index < std::size(buffers); ++index)
    {
        barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[index].UAV.pResource = buffers[index]->GetResource();
    }

    commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
}

bool RadianceCache::RecordResolve(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer,
    std::uint32_t frameIndex)
{
    if(!m_initialized || !commandList || !HasResources())
    {
        return false;
    }

    RadianceCacheConstants constants{};
    constants.cache = GetBinding(frameIndex);

    const D3D12_GPU_VIRTUAL_ADDRESS address = uploadBuffer.Allocate(&constants, sizeof(RadianceCacheConstants),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(address == 0)
    {
        Log::Error("Failed to allocate radiance cache resolve constants.");
        return false;
    }

    // Every update write (claims, keys, atomics, overflow flags) completes before resolve reads them.
    BeginUpdate(commandList);
    RecordUAVBarriers(commandList);

    m_resolvePipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    commandList->Dispatch((constants.cache.bucketCount + ResolveGroupSize - 1) / ResolveGroupSize, 1, 1);

    // Publication completes before anything reads or writes the cache again.
    RecordUAVBarriers(commandList);
    return true;
}

void RadianceCache::BeginQuery(ID3D12GraphicsCommandList* commandList)
{
    constexpr D3D12_RESOURCE_STATES readState =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_resources.tags.Transition(commandList, readState);
    m_resources.keys.Transition(commandList, readState);
    m_resources.published.Transition(commandList, readState);
}

std::uint64_t RadianceCache::GetResourceBytes() const
{
    if(!HasResources())
    {
        return 0;
    }

    const std::uint64_t slots = m_settings.slotCount;
    return slots * BytesPerSlot + slots / BucketSlots * 4 + StatisticCount * 4;
}
