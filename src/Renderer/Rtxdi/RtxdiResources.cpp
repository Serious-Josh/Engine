#include "Renderer/Rtxdi/RtxdiResources.h"
#include "Debug/MemoryRegistry.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/UploadBuffer.h"
#include "Core/Log.h"

#include "Rtxdi/RtxdiUtils.h"

#include <algorithm>
#include <cstring>
#include <string>

RTXDI_DISpatioTemporalResamplingParameters RtxdiResources::SpatioTemporalParameters()
{
    RTXDI_DISpatioTemporalResamplingParameters parameters = rtxdi::GetDefaultReSTIRDISpatioTemporalResamplingParams();
    parameters.biasCorrectionMode = ReSTIRDI_SpatioTemporalBiasCorrectionMode::Raytraced;
    parameters.discountNaiveSamples = 0;
    // RTXPT D.2C.2R: a reservoir history cap of 4 (SDK default 20). Long-lived high-weight reservoirs near steep
    // target gradients (a spot light's cone edge) persisted and propagated as bright sparkles; cap 4 removes most
    // of them with no measured bias or cost.
    parameters.maxHistoryLength = 4;
    return parameters;
}

bool RtxdiResources::Initialize(
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);

    Shutdown();

    if(!device || width == 0 || height == 0)
    {
        return false;
    }

    rtxdi::ReSTIRDIStaticParameters staticParameters;
    staticParameters.NeighborOffsetCount = NeighborOffsetCount;
    staticParameters.RenderWidth = width;
    staticParameters.RenderHeight = height;
    staticParameters.CheckerboardSamplingMode = rtxdi::CheckerboardMode::Off;
    m_context.emplace(staticParameters);

    // Fused mode: each frame reads the previous output array and writes the next one (UpdateBufferIndices).
    m_context->SetResamplingMode(rtxdi::ReSTIRDI_ResamplingMode::FusedSpatiotemporal);
    m_context->SetSpatioTemporalResamplingParameters(SpatioTemporalParameters());

    // The SDK's own pitch; ReservoirArrayElements must agree, since the shaders address with these values.
    const RTXDI_ReservoirBufferParameters parameters = m_context->GetReservoirBufferParameters();
    const std::uint64_t arrayElements = ReservoirArrayElements(width, height);

    if(parameters.reservoirArrayPitch != arrayElements)
    {
        Log::Error("RTXDI reservoir pitch differs from Engine's sizing; ReSTIR DI resources not created.");
        Shutdown();
        return false;
    }

    const std::uint64_t elements = arrayElements * ReservoirArrayCount;
    m_descriptorHeap = &descriptorHeap;

    if(!m_reservoirs.InitializeGPUWritable(device, static_cast<size_t>(elements * ReservoirStride)) ||
        !CreateReservoirUAV(device, descriptorHeap, elements) || !CreateNeighborOffsets(device, descriptorHeap) ||
        !m_lightData.InitializeGPUWritable(device, RtxdiLightDataLayout::CapacityBytes) ||
        !m_lightData.CreateRawSRV(device, descriptorHeap))
    {
        Log::Error("Failed to create the RTXDI reservoir buffer, neighbor offsets or light data.");
        Shutdown();
        return false;
    }

    m_reservoirs.GetResource()->SetName(L"RTXDI DI Reservoirs");
    m_lightData.GetResource()->SetName(L"RTXDI Light Data");

    m_width = width;
    m_height = height;
    m_initialized = true;
    return true;
}

bool RtxdiResources::Resize(
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    unsigned int width,
    unsigned int height)
{
    if(!m_initialized)
    {
        return true;
    }

    return Initialize(device, descriptorHeap, width, height);
}

void RtxdiResources::Shutdown()
{
    InvalidateHistory();

    if(m_descriptorHeap && m_reservoirUAVIndex != UINT_MAX)
    {
        m_descriptorHeap->Free(m_reservoirUAVIndex);
    }

    if(m_descriptorHeap && m_neighborOffsetsSRVIndex != UINT_MAX)
    {
        m_descriptorHeap->Free(m_neighborOffsetsSRVIndex);
    }

    if(m_neighborOffsets)
    {
        m_neighborOffsets->Release();
        m_neighborOffsets = nullptr;
    }

    m_descriptorHeap = nullptr;
    m_reservoirUAVIndex = UINT_MAX;
    m_neighborOffsetsSRVIndex = UINT_MAX;
    m_reservoirs.Shutdown();
    m_lightData.Shutdown();
    m_context.reset();
    m_frameIndex = 0;
    m_lastFrameUsedHistory = false;

    m_width = 0;
    m_height = 0;
    m_initialized = false;
}

bool RtxdiResources::BeginFrame(const RtxdiLightSet& lights, const RtxdiHistorySignature& signature,
    bool previousSurfacesAvailable, const RtxdiHistoryPolicy& policy, RtxdiFrame& frame)
{
    m_pending.reset();
    m_lastFrameUsedHistory = false;
    m_lastHistoryDecision = RtxdiHistoryDecision::NoCommittedFrame;

    if(!m_initialized)
    {
        return false;
    }

    // Always derived from the committed context, so an abandoned recording never advances the rotation.
    PendingFrame pending{*m_context, lights, signature};
    pending.context.SetFrameIndex(m_frameIndex + 1);

    // SetFrameIndex refreshes the permutation-sampling random number only for the standalone temporal pass. Fused
    // mode needs it refreshed too: a fixed value pairs every pixel with the same temporal partner each frame, and
    // since the permutation is its own inverse the pair trades one history back and forth (RTXPT.3 diagnostic:
    // per-frame estimates correlated 0.9 two frames apart). Same convention as the temporal pass.
    RTXDI_DISpatioTemporalResamplingParameters spatioTemporalParameters =
        pending.context.GetSpatioTemporalResamplingParameters();
    spatioTemporalParameters.uniformRandomNumber = rtxdi::JenkinsHash(m_frameIndex + 1);
    pending.context.SetSpatioTemporalResamplingParameters(spatioTemporalParameters);

    // Previous reservoirs are reusable only against the same geometry and materials, and only when the previous surfaces
    // they were computed on can be reconstructed. Moved instances keep history unless the conservative policy is chosen.
    const RtxdiHistorySignature& committed = m_committedSignature;
    RtxdiHistoryDecision decision = RtxdiHistoryDecision::Reused;

    if(!m_historyValid)
    {
        decision = RtxdiHistoryDecision::NoCommittedFrame;
    }
    else if(!previousSurfacesAvailable)
    {
        decision = RtxdiHistoryDecision::PreviousSurfacesUnavailable;
    }
    else if(committed.sceneResourceLifetime != signature.sceneResourceLifetime)
    {
        decision = RtxdiHistoryDecision::SceneResources;
    }
    else if(committed.topLevelAddress != signature.topLevelAddress)
    {
        decision = RtxdiHistoryDecision::TopLevel;
    }
    else if(committed.topLevelGeometry != signature.topLevelGeometry || signature.topLevelGeometry == 0)
    {
        decision = RtxdiHistoryDecision::Geometry;
    }
    else if(committed.materialRevision != signature.materialRevision)
    {
        decision = RtxdiHistoryDecision::Material;
    }
    else if(!policy.preserveThroughTransforms && committed.topLevelRecording != signature.topLevelRecording)
    {
        decision = RtxdiHistoryDecision::Transforms;
    }

    const bool historyUsable = decision == RtxdiHistoryDecision::Reused;
    m_lastHistoryDecision = decision;

    RtxdiFrame candidate;
    candidate.reservoirBufferParameters = pending.context.GetReservoirBufferParameters();
    candidate.bufferIndices = pending.context.GetBufferIndices();
    candidate.runtimeParameters = pending.context.GetRuntimeParams();
    candidate.lightBufferParameters = lights.bufferParameters;
    candidate.spatioTemporalParameters = pending.context.GetSpatioTemporalResamplingParameters();
    candidate.historyValid = historyUsable;
    candidate.lightRecords = lights.records;

    std::vector<std::int32_t> currentToPrevious;
    std::vector<std::int32_t> previousToCurrent;

    if(historyUsable)
    {
        BuildRtxdiLightTranslation(m_committedLights, lights, policy.preserveThroughLightChanges, currentToPrevious,
            previousToCurrent);
        candidate.previousLightRecords = m_committedLights.records;
    }
    else
    {
        currentToPrevious.assign(lights.records.size(), -1);
    }

    candidate.previousLightCount = static_cast<std::uint32_t>(previousToCurrent.size());
    candidate.lightTranslation = std::move(currentToPrevious);
    candidate.lightTranslation.insert(candidate.lightTranslation.end(), previousToCurrent.begin(),
        previousToCurrent.end());

    m_pending.emplace(std::move(pending));
    m_lastFrameUsedHistory = historyUsable;
    frame = std::move(candidate);
    return true;
}

bool RtxdiResources::RecordLightData(ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const RtxdiFrame& frame)
{
    const auto currentCount = static_cast<std::uint32_t>(frame.lightRecords.size());
    const auto previousCount = static_cast<std::uint32_t>(frame.previousLightRecords.size());

    if(!m_initialized || !commandList || currentCount > RtxdiLightSet::Capacity ||
        previousCount > RtxdiLightSet::Capacity || frame.previousLightCount != previousCount ||
        frame.lightTranslation.size() != static_cast<std::size_t>(currentCount) + previousCount)
    {
        return false;
    }

    // An empty frame still writes one word, so every frame records the same copy. The staging bytes keep their capacity.
    std::vector<std::uint8_t>& bytes = m_lightDataStaging;
    bytes.assign((std::max)(RtxdiLightDataLayout::Bytes(currentCount, previousCount), 4u), 0);
    std::uint8_t* destination = bytes.data();

    const auto append = [&](const void* source, std::size_t size)
    {
        if(size != 0)
        {
            std::memcpy(destination, source, size);
            destination += size;
        }
    };

    append(frame.lightRecords.data(), frame.lightRecords.size() * RtxdiLightDataLayout::RecordBytes);
    append(frame.previousLightRecords.data(), frame.previousLightRecords.size() * RtxdiLightDataLayout::RecordBytes);
    append(frame.lightTranslation.data(), frame.lightTranslation.size() * RtxdiLightDataLayout::TranslationBytes);

    UploadLocation source;

    if(upload.Allocate(bytes.data(), bytes.size(), 16, source) == 0)
    {
        return false;
    }

    m_lightData.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyBufferRegion(m_lightData.GetResource(), 0, source.resource, source.offset, bytes.size());
    m_lightData.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return true;
}

void RtxdiResources::CommitFrame()
{
    if(!m_pending)
    {
        return;
    }

    m_context = m_pending->context;
    m_frameIndex = m_context->GetFrameIndex();
    m_committedLights = std::move(m_pending->lights);
    m_committedSignature = m_pending->signature;
    m_historyValid = true;
    m_pending.reset();
}

void RtxdiResources::AbandonFrame()
{
    m_pending.reset();
}

void RtxdiResources::InvalidateHistory()
{
    m_pending.reset();
    m_committedLights = {};
    m_committedSignature = {};
    m_historyValid = false;
}

std::uint64_t RtxdiResources::GetResourceBytes() const noexcept
{
    if(!m_initialized)
    {
        return 0;
    }

    const std::uint64_t reservoirBytes = ReservoirArrayElements(m_width, m_height) * ReservoirArrayCount *
        ReservoirStride;
    return reservoirBytes + NeighborOffsetCount * 2 + RtxdiLightDataLayout::CapacityBytes;
}

std::uint64_t RtxdiResources::ReservoirArrayElements(unsigned int width, unsigned int height)
{
    // RTXDI_RESERVOIR_BLOCK_SIZE square blocks, rows of whole blocks (RtxdiUtils.cpp).
    constexpr std::uint64_t blockSize = RTXDI_RESERVOIR_BLOCK_SIZE;
    const std::uint64_t blocksWide = (width + blockSize - 1) / blockSize;
    const std::uint64_t blocksHigh = (height + blockSize - 1) / blockSize;
    return blocksWide * blocksHigh * blockSize * blockSize;
}

bool RtxdiResources::CreateReservoirUAV(ID3D12Device* device, DescriptorHeap& descriptorHeap,
    std::uint64_t elements)
{
    if(elements > UINT_MAX)
    {
        return false;
    }

    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);

    // Buffer offers raw views only; RTXDI_LIGHT_RESERVOIR_BUFFER is a structured buffer.
    const unsigned int index = descriptorHeap.Allocate();

    if(index == UINT_MAX)
    {
        return false;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC description{};
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    description.Buffer.FirstElement = 0;
    description.Buffer.NumElements = static_cast<UINT>(elements);
    description.Buffer.StructureByteStride = ReservoirStride;
    description.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    device->CreateUnorderedAccessView(m_reservoirs.GetResource(), nullptr, &description,
        descriptorHeap.GetCPUHandle(index));

    m_reservoirUAVIndex = index;
    return true;
}

bool RtxdiResources::CreateNeighborOffsets(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);

    // Immutable lookup table, written once through an upload heap: no commands, no upload submission, no wait.
    std::vector<std::uint8_t> offsets(NeighborOffsetCount * 2);
    rtxdi::FillNeighborOffsetBuffer(offsets.data(), NeighborOffsetCount);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = offsets.size();
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if(FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_neighborOffsets))))
    {
        return false;
    }

    MemoryRegistry::TrackResource(m_neighborOffsets);

    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, 0};

    if(FAILED(m_neighborOffsets->Map(0, &readRange, &mapped)))
    {
        return false;
    }

    std::memcpy(mapped, offsets.data(), offsets.size());
    m_neighborOffsets->Unmap(0, nullptr);
    m_neighborOffsets->SetName(L"RTXDI Neighbor Offsets");

    const unsigned int index = descriptorHeap.Allocate();

    if(index == UINT_MAX)
    {
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8_SNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Buffer.NumElements = NeighborOffsetCount;
    device->CreateShaderResourceView(m_neighborOffsets, &view, descriptorHeap.GetCPUHandle(index));

    m_neighborOffsetsSRVIndex = index;
    return true;
}
