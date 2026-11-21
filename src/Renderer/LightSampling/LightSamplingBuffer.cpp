#include "Renderer/LightSampling/LightSamplingBuffer.h"

#include "Debug/MemoryRegistry.h"
#include "Renderer/DescriptorHeap.h"

#include <d3d12.h>

#include <algorithm>
#include <cstring>

namespace
{
    void ReleaseStaging(ID3D12Resource*& staging)
    {
        if(staging)
        {
            staging->Release();
            staging = nullptr;
        }
    }

    // A committed upload-heap buffer holding the header followed by `bytes` of `data`, or null.
    ID3D12Resource* CreateStaging(ID3D12Device* device, const LightSamplingConstants& header, const void* data,
        std::uint64_t bytes)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = LightSamplingHeaderBytes + bytes;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* staging = nullptr;
        const MemoryCategoryScope memoryScope(MemoryCategory::UploadReadback);

        if(FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging))))
        {
            return nullptr;
        }

        void* mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};

        if(FAILED(staging->Map(0, &noRead, &mapped)) || !mapped)
        {
            staging->Release();
            return nullptr;
        }

        auto* destination = static_cast<unsigned char*>(mapped);
        std::memset(destination, 0, LightSamplingHeaderBytes);
        std::memcpy(destination, &header, sizeof(header));
        std::memcpy(destination + LightSamplingHeaderBytes, data, static_cast<std::size_t>(bytes));
        staging->Unmap(0, nullptr);
        staging->SetName(L"Light Sampling Staging");
        return staging;
    }
}

LightSamplingBuffer::~LightSamplingBuffer()
{
    Shutdown();
}

bool LightSamplingBuffer::Grow(ID3D12Device* device, DescriptorHeap& descriptorHeap, std::uint64_t bytes,
    std::uint64_t retireFence, std::string& error)
{
    // 25% headroom, in 64 KiB steps: a structure tens of megabytes large grows to fit without doubling, and a revision
    // that only adds a few lights reuses the buffer.
    constexpr std::uint64_t Granularity = 64 * 1024;
    const std::uint64_t wanted = (std::max)(bytes + bytes / 4, m_capacityBytes);
    const std::uint64_t capacity = (wanted + Granularity - 1) / Granularity * Granularity;

    const MemoryCategoryScope memoryScope(MemoryCategory::CoreRenderer);
    Buffer buffer;

    if(!buffer.InitializeGPUWritable(device, static_cast<size_t>(capacity)) || !buffer.CreateRawSRV(device, descriptorHeap))
    {
        buffer.Shutdown();
        error = "Failed to allocate the light-sampling buffer (" + std::to_string(capacity) + " bytes).";
        return false;
    }

    buffer.GetResource()->SetName(L"Light Sampling");

    // Submitted frames may still read the current buffer through its descriptor.
    if(m_buffer.GetResource())
    {
        m_retired.push_back(Retired{std::move(m_buffer), nullptr, retireFence});
    }

    m_buffer = std::move(buffer);
    m_capacityBytes = capacity;
    m_publishedRevision = 0;
    return true;
}

void LightSamplingBuffer::ReleaseCompleted(std::uint64_t completedFence)
{
    std::erase_if(m_retired,
        [&](Retired& retired)
        {
            if(retired.pending || retired.fence > completedFence)
            {
                return false;
            }

            retired.buffer.Shutdown();
            ReleaseStaging(retired.staging);
            return true;
        });
}

LightSamplingBuffer::RecordResult LightSamplingBuffer::Record(ID3D12Device* device, DescriptorHeap& descriptorHeap,
    ID3D12GraphicsCommandList* commandList, const LightSamplingData& data, std::uint64_t revision,
    std::uint64_t recordingGeneration, std::uint64_t lastSubmittedFence, std::uint64_t completedFence,
    LightSamplingConstants& constants, std::string* failureReason, const LightSamplingPatch* patch)
{
    constants = {};
    m_recordingPending = false;
    ReleaseCompleted(completedFence);

    const auto fail = [&](std::string reason)
    {
        if(failureReason)
        {
            *failureReason = std::move(reason);
        }

        return RecordResult::Failed;
    };

    if(!device || !commandList || data.words.empty())
    {
        return fail("Invalid light-sampling recording arguments.");
    }

    const std::uint64_t bytes = std::uint64_t{data.words.size()} * sizeof(std::uint32_t);
    const std::uint64_t publishedBytes = LightSamplingHeaderBytes + bytes;
    std::string error;

    if(publishedBytes > m_capacityBytes && !Grow(device, descriptorHeap, publishedBytes, lastSubmittedFence, error))
    {
        return fail(error);
    }

    const bool reuse = revision != 0 && revision == m_publishedRevision;
    m_stateBeforeRecording = m_buffer.GetResourceState();

    // The constants as consumers see them, also stored at the start of the buffer (M4.2A: a shader that holds only the
    // descriptor index reads them there).
    LightSamplingConstants published = data.constants;
    published.bufferIndex = m_buffer.GetDescriptorIndex();
    published.valid = 1;
    published.revisionLow = static_cast<std::uint32_t>(revision);
    published.revisionHigh = static_cast<std::uint32_t>(revision >> 32);

    // M4.1.1: a patch applies only on top of exactly its base revision (a grown buffer holds none) and inside the data.
    bool patchable = !reuse && patch && patch->ranges && patch->baseRevision != 0 &&
        patch->baseRevision == m_publishedRevision;

    if(patchable)
    {
        for(const LightSamplingDirtyRange& range : *patch->ranges)
        {
            patchable = patchable && range.wordCount != 0 && range.firstWord + range.wordCount <= data.words.size();
        }
    }

    if(!reuse && patchable)
    {
        // Ranges closer than this many words copy the words between them too (unchanged, so harmless): scattered list
        // edits become a few copies instead of thousands.
        constexpr std::uint64_t MergeGapWords = 64;
        std::vector<LightSamplingDirtyRange> copies;

        for(const LightSamplingDirtyRange& range : *patch->ranges)
        {
            if(!copies.empty() && range.firstWord <= copies.back().firstWord + copies.back().wordCount + MergeGapWords)
            {
                copies.back().wordCount = (std::max)(copies.back().firstWord + copies.back().wordCount,
                    range.firstWord + range.wordCount) - copies.back().firstWord;
                continue;
            }

            copies.push_back(range);
        }

        std::vector<std::uint32_t> gathered;

        for(const LightSamplingDirtyRange& range : copies)
        {
            gathered.insert(gathered.end(), data.words.begin() + static_cast<std::ptrdiff_t>(range.firstWord),
                data.words.begin() + static_cast<std::ptrdiff_t>(range.firstWord + range.wordCount));
        }

        const std::uint64_t gatheredBytes = std::uint64_t{gathered.size()} * sizeof(std::uint32_t);
        ID3D12Resource* staging = CreateStaging(device, published, gathered.data(), gatheredBytes);

        if(!staging)
        {
            return fail("Failed to allocate the light-sampling patch upload (" + std::to_string(gatheredBytes) + " bytes).");
        }

        m_buffer.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(m_buffer.GetResource(), 0, staging, 0, LightSamplingHeaderBytes);
        std::uint64_t stagingWord = 0;

        for(const LightSamplingDirtyRange& range : copies)
        {
            commandList->CopyBufferRegion(m_buffer.GetResource(), LightSamplingHeaderBytes + range.firstWord * 4, staging,
                LightSamplingHeaderBytes + stagingWord * 4, range.wordCount * 4);
            stagingWord += range.wordCount;
        }

        // Read by this recording, whose fence Submitted reports.
        m_retired.push_back(Retired{Buffer{}, staging, 0, true, recordingGeneration});
        m_publishedRevision = revision;
        m_recordingPending = true;
        ++m_copyCount;
        ++m_patchCount;
        m_lastCopyBytes = LightSamplingHeaderBytes + gatheredBytes;
        m_totalCopyBytes += m_lastCopyBytes;
        m_lastCopyWasPatch = true;
    }
    else if(!reuse)
    {
        ID3D12Resource* staging = CreateStaging(device, published, data.words.data(), bytes);

        if(!staging)
        {
            return fail("Failed to allocate the light-sampling staging upload (" + std::to_string(bytes) + " bytes).");
        }

        m_buffer.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(m_buffer.GetResource(), 0, staging, 0, publishedBytes);
        // The staging copy is read by this recording, whose fence Submitted reports.
        m_retired.push_back(Retired{Buffer{}, staging, 0, true, recordingGeneration});
        m_publishedRevision = revision;
        m_recordingPending = true;
        ++m_copyCount;
        m_lastCopyBytes = publishedBytes;
        m_totalCopyBytes += m_lastCopyBytes;
        m_lastCopyWasPatch = false;
    }

    m_buffer.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    constants = published;
    return reuse ? RecordResult::Reused : RecordResult::Published;
}

void LightSamplingBuffer::Submitted(std::uint64_t recordingGeneration, std::uint64_t fence) noexcept
{
    for(Retired& retired : m_retired)
    {
        if(retired.pending && retired.generation == recordingGeneration)
        {
            retired.pending = false;
            retired.fence = fence;
        }
    }
}

void LightSamplingBuffer::DiscardRecording(std::uint64_t lastSubmittedFence) noexcept
{
    // The discarded recording's staging copies never ran.
    for(Retired& retired : m_retired)
    {
        if(retired.pending)
        {
            retired.pending = false;
            retired.fence = lastSubmittedFence;
        }
    }

    if(!m_recordingPending)
    {
        return;
    }

    // The copy never ran: the buffer's contents are unknown and its tracked state is the pre-recording one.
    m_publishedRevision = 0;
    m_buffer.RestoreStateAfterAbort(m_stateBeforeRecording);
    m_recordingPending = false;
}

void LightSamplingBuffer::Shutdown()
{
    for(Retired& retired : m_retired)
    {
        retired.buffer.Shutdown();
        ReleaseStaging(retired.staging);
    }

    m_retired.clear();
    m_buffer.Shutdown();
    m_capacityBytes = 0;
    m_publishedRevision = 0;
    m_recordingPending = false;
}
