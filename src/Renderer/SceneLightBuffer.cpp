#include "Renderer/SceneLightBuffer.h"

#include "Debug/MemoryRegistry.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/UploadBuffer.h"

#include <algorithm>

namespace
{
    constexpr std::uint64_t RecordBytes = sizeof(GPULight);
}

bool SceneLightBuffer::Grow(ID3D12Device* device, DescriptorHeap& descriptorHeap, std::uint32_t lightCount,
    std::uint64_t lastSubmittedFence, std::string& error)
{
    // Doubling from the current capacity, at least the light count and at least one record (a zero-light table still
    // binds a valid SRV).
    std::uint64_t capacity = (std::max)(std::uint64_t{1}, std::uint64_t{m_capacity});

    while(capacity < lightCount)
    {
        capacity *= 2;
    }

    capacity = (std::min)(capacity, std::uint64_t{UINT32_MAX});
    const std::uint64_t bytes = capacity * RecordBytes;
    const bool refusedForTesting = m_capacityLimitForTesting && capacity > *m_capacityLimitForTesting;
    const MemoryCategoryScope memoryScope(MemoryCategory::CoreRenderer);
    Buffer buffer;

    // A structured SRV of GPULight records (SceneLightLoad's StructuredBuffer<GPULight>); the allocation is a whole number
    // of records, so the view covers exactly the capacity. On failure the current buffer and its descriptor are untouched.
    if(refusedForTesting || !buffer.InitializeGPUWritable(device, static_cast<size_t>(bytes)) ||
        !buffer.CreateStructuredSRV(device, descriptorHeap, static_cast<std::uint32_t>(RecordBytes)))
    {
        buffer.Shutdown();
        error = "Failed to allocate the scene light buffer for " + std::to_string(lightCount) + " lights (" +
            std::to_string(bytes) + " bytes" + (refusedForTesting ? ", refused by the test capacity limit)." : ").");
        return false;
    }

    buffer.GetResource()->SetName(L"Scene Lights");

    // Submitted frames may still read the current buffer through its descriptor, so neither is released yet.
    if(m_buffer.GetResource())
    {
        m_retired.push_back(RetiredBuffer{std::move(m_buffer), lastSubmittedFence});
    }

    m_buffer = std::move(buffer);
    m_capacity = static_cast<std::uint32_t>(capacity);
    // A new buffer holds no table yet.
    m_publishedRevision = 0;
    return true;
}

bool SceneLightBuffer::Initialize(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    if(m_buffer.GetResource())
    {
        return true;
    }

    std::string error;
    return device && Grow(device, descriptorHeap, 0, 0, error);
}

SceneLightBuffer::RecordResult SceneLightBuffer::Record(ID3D12Device* device, DescriptorHeap& descriptorHeap,
    ID3D12GraphicsCommandList* commandList, UploadBuffer& upload, const SceneLightTable& table,
    std::uint64_t lastSubmittedFence, std::uint64_t completedFence, SceneLightConstants& constants,
    std::string* failureReason, std::uint64_t contentRevision)
{
    const auto fail = [&](RecordResult result, std::string reason)
    {
        if(failureReason)
        {
            *failureReason = std::move(reason);
        }

        return result;
    };

    if(!device || !commandList || table.lights.size() != table.lightCount)
    {
        return fail(RecordResult::Failed, "Invalid scene light buffer recording arguments.");
    }

    // Retired buffers whose last possible reader has completed.
    std::erase_if(m_retired,
        [&](RetiredBuffer& retired)
        {
            if(retired.submissionFence > completedFence)
            {
                return false;
            }

            retired.buffer.Shutdown();
            return true;
        });

    std::string growthError;

    // Without any buffer there is no valid descriptor to fall back to, so a one-record buffer is tried before giving up.
    if(!m_buffer.GetResource() && !Grow(device, descriptorHeap, table.lightCount, lastSubmittedFence, growthError) &&
        !Grow(device, descriptorHeap, 0, lastSubmittedFence, growthError))
    {
        return fail(RecordResult::Failed, growthError);
    }

    std::string publicationError;
    bool tableFits = table.lightCount <= m_capacity ||
        Grow(device, descriptorHeap, table.lightCount, lastSubmittedFence, publicationError);
    UploadLocation source;
    const std::uint64_t bytes = std::uint64_t{table.lightCount} * RecordBytes;

    // M3.5: the buffer already holds this exact table (same content revision, no growth since), so nothing is copied.
    const bool alreadyPublished = tableFits && contentRevision != 0 && contentRevision == m_publishedRevision;

    if(tableFits && !alreadyPublished && table.lightCount != 0 &&
        upload.Allocate(table.lights.data(), static_cast<size_t>(bytes), 16, source) == 0)
    {
        publicationError = "Failed to allocate the scene light upload (" + std::to_string(bytes) + " bytes).";
        tableFits = false;
    }

    // Many-light M3: a table that cannot be published becomes an empty one, bound through the current buffer: its
    // descriptor is valid for this frame and later ones, and count 0 means its earlier contents are never read.
    const std::uint32_t publishedCount = tableFits ? table.lightCount : 0;

    if(publishedCount != 0 && !alreadyPublished)
    {
        m_buffer.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(m_buffer.GetResource(), 0, source.resource, source.offset, bytes);
    }

    // Deferred lighting reads it from a pixel shader; every other consumer from compute or ray tracing.
    m_buffer.Transition(commandList, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    constants = {};
    constants.lightCount = publishedCount;
    constants.lightBufferIndex = m_buffer.GetDescriptorIndex();

    if(!tableFits)
    {
        m_publishedRevision = 0;
        return fail(RecordResult::PublishedEmpty, publicationError);
    }

    m_publishedRevision = contentRevision;
    return RecordResult::Published;
}

void SceneLightBuffer::Shutdown()
{
    for(RetiredBuffer& retired : m_retired)
    {
        retired.buffer.Shutdown();
    }

    m_retired.clear();
    m_buffer.Shutdown();
    m_capacity = 0;
    m_publishedRevision = 0;
}
