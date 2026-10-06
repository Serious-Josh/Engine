#include "GpuTimestampProfiler.h"

bool GpuTimestampProfiler::Initialize(ID3D12Device* device, ID3D12CommandQueue* commandQueue)
{
    if(!device || !commandQueue){
        return false;
    }

    D3D12_QUERY_HEAP_DESC queryHeapDescription{};
    queryHeapDescription.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryHeapDescription.Count = QueryCount;
    queryHeapDescription.NodeMask = 0;

    HRESULT result = device->CreateQueryHeap(&queryHeapDescription, IID_PPV_ARGS(&m_queryHeap));

    if(FAILED(result)){
        Shutdown();
        return false;
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_READBACK;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resourceDescription{};
    resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDescription.Alignment = 0;
    resourceDescription.Width = ReadbackBufferSize;
    resourceDescription.Height = 1;
    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;
    resourceDescription.Format = DXGI_FORMAT_UNKNOWN;
    resourceDescription.SampleDesc.Count = 1;
    resourceDescription.SampleDesc.Quality = 0;
    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDescription.Flags = D3D12_RESOURCE_FLAG_NONE;

    result = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readbackBuffer));

    if(FAILED(result)){
        Shutdown();
        return false;
    }

    result = commandQueue->GetTimestampFrequency(&m_timestampFrequency);

    if(FAILED(result) || m_timestampFrequency == 0){
        Shutdown();
        return false;
    }

    return true;
}

void GpuTimestampProfiler::Shutdown()
{
    for(unsigned int i = 0; i < FrameCount; ++i){
        m_frameSlots[i] = {};
    }

    if(m_readbackBuffer){
        m_readbackBuffer->Release();
        m_readbackBuffer = nullptr;
    }

    if(m_queryHeap){
        m_queryHeap->Release();
        m_queryHeap = nullptr;
    }

    m_results = {};
    m_timestampFrequency = 0;
}

void GpuTimestampProfiler::BeginFrame(ID3D12GraphicsCommandList* commandList, unsigned int frameIndex)
{
    if(!commandList || frameIndex >= FrameCount){
        return;
    }

    m_frameSlots[frameIndex].recordedRegions = 0;

    BeginRegion(commandList, frameIndex, Region::TotalFrame);
}

void GpuTimestampProfiler::BeginRegion(ID3D12GraphicsCommandList* commandList, unsigned int frameIndex, Region region)
{
    if(!commandList || frameIndex >= FrameCount){
        return;
    }

    const unsigned int regionIndex = static_cast<unsigned int>(region);

    if(regionIndex >= RegionCount){
        return;
    }

    commandList->EndQuery(m_queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, GetBeginQueryIndex(frameIndex, region));
}

void GpuTimestampProfiler::EndRegion(ID3D12GraphicsCommandList* commandList, unsigned int frameIndex, Region region)
{
    if(!commandList || frameIndex >= FrameCount){
        return;
    }

    const unsigned int regionIndex = static_cast<unsigned int>(region);

    if(regionIndex >= RegionCount){
        return;
    }

    commandList->EndQuery(m_queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, GetEndQueryIndex(frameIndex, region));

    m_frameSlots[frameIndex].recordedRegions |= (1u << regionIndex);
}

void GpuTimestampProfiler::EndFrameAndResolve(ID3D12GraphicsCommandList* commandList, unsigned int frameIndex)
{
    if(!commandList || frameIndex >= FrameCount){
        return;
    }

    EndRegion(commandList, frameIndex, Region::TotalFrame);

    const uint32_t recordedRegions = m_frameSlots[frameIndex].recordedRegions;

    for(unsigned int regionIndex = 0; regionIndex < RegionCount; ++regionIndex){
        const uint32_t regionBit = 1u << regionIndex;

        if((recordedRegions & regionBit) == 0){
            continue;
        }

        const Region region = static_cast<Region>(regionIndex);
        const unsigned int queryIndex = GetBeginQueryIndex(frameIndex, region);
        const uint64_t destinationOffset = static_cast<uint64_t>(frameIndex) * ReadbackBytesPerFrame + static_cast<uint64_t>(regionIndex) * QueriesPerRegion * sizeof(uint64_t);

        commandList->ResolveQueryData(m_queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, queryIndex, QueriesPerRegion, m_readbackBuffer, destinationOffset);
    }
}

uint64_t GpuTimestampProfiler::GetTimestampFrequency() const
{
    return m_timestampFrequency;
}

void GpuTimestampProfiler::MarkSubmitted(unsigned int frameIndex, uint64_t submissionFence, uint64_t frameSerial)
{
    if(frameIndex >= FrameCount || submissionFence == 0){
        return;
    }

    FrameSlot& slot = m_frameSlots[frameIndex];

    slot.submissionFence = submissionFence;
    slot.frameSerial = frameSerial;
    slot.pending = true;
}

bool GpuTimestampProfiler::CollectCompleted(uint64_t completedFenceValue)
{
    if(completedFenceValue == UINT64_MAX){
        return false;
    }
    
    FrameSlot* newestCompletedSlot = nullptr;
    unsigned int newestCompletedFrameIndex = 0;

    for(unsigned int frameIndex = 0; frameIndex < FrameCount; ++frameIndex)
    {
        FrameSlot& slot = m_frameSlots[frameIndex];

        if(!slot.pending){
            continue;
        }

        if(slot.submissionFence > completedFenceValue){
            continue;
        }

        if(!newestCompletedSlot || slot.frameSerial > newestCompletedSlot->frameSerial)
        {
            newestCompletedSlot = &slot;
            newestCompletedFrameIndex = frameIndex;
        }
    }

    if(!newestCompletedSlot){
        return false;
    }

    const uint64_t readbackOffset = static_cast<uint64_t>(newestCompletedFrameIndex) * ReadbackBytesPerFrame;

    D3D12_RANGE readRange{};
    readRange.Begin = static_cast<SIZE_T>(readbackOffset);
    readRange.End = static_cast<SIZE_T>(readbackOffset + ReadbackBytesPerFrame);

    void* mappedData = nullptr;

    HRESULT result = m_readbackBuffer->Map(0, &readRange, &mappedData);

    if(FAILED(result) || !mappedData){
        return false;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(mappedData);

    const uint64_t* timestamps = reinterpret_cast<const uint64_t*>(bytes + readbackOffset);

    Results collected{};
    collected.frameSerial = newestCompletedSlot->frameSerial;
    collected.validRegions = newestCompletedSlot->recordedRegions;

    for(unsigned int regionIndex = 0; regionIndex < RegionCount; ++regionIndex)
    {
        const uint32_t regionBit = 1u << regionIndex;

        if((collected.validRegions & regionBit) == 0){
            continue;
        }

        const unsigned int timestampIndex = regionIndex * QueriesPerRegion;
        const uint64_t begin = timestamps[timestampIndex];
        const uint64_t end = timestamps[timestampIndex + 1];

        if(end < begin){
            collected.validRegions &= ~regionBit; continue;
        }

        const uint64_t elapsedTicks = end - begin;

        collected.milliseconds[regionIndex] = static_cast<double>(elapsedTicks) * 1000.0 / static_cast<double>(m_timestampFrequency);
    }

    D3D12_RANGE writtenRange{};
    writtenRange.Begin = 0;
    writtenRange.End = 0;

    m_readbackBuffer->Unmap(0, &writtenRange);

    m_results = collected;

    for(unsigned int frameIndex = 0; frameIndex < FrameCount; ++frameIndex)
    {
        FrameSlot& slot = m_frameSlots[frameIndex];

        if(slot.pending && slot.submissionFence <= completedFenceValue && slot.frameSerial <= collected.frameSerial)
        {
            slot.pending = false;
        }
    }

    return true;
}

const GpuTimestampProfiler::Results& GpuTimestampProfiler::GetResults() const
{
    return m_results;
}

unsigned int GpuTimestampProfiler::GetBeginQueryIndex(unsigned int frameIndex, Region region)
{
    const unsigned int regionIndex = static_cast<unsigned int>(region);

    return frameIndex * QueriesPerFrame + regionIndex * QueriesPerRegion;
}

unsigned int GpuTimestampProfiler::GetEndQueryIndex(unsigned int frameIndex, Region region)
{
    return GetBeginQueryIndex(frameIndex, region) + 1;
}
