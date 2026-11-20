#include "Renderer/SceneLightBuffer.h"

#include "Core/Log.h"
#include "Debug/MemoryRegistry.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/UploadBuffer.h"

#include <algorithm>
#include <string>

namespace
{
    constexpr std::uint64_t RecordBytes = sizeof(GPULight);
}

bool SceneLightBuffer::Grow(ID3D12Device* device, DescriptorHeap& descriptorHeap, std::uint32_t lightCount,
    std::uint64_t lastSubmittedFence)
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
    const MemoryCategoryScope memoryScope(MemoryCategory::CoreRenderer);
    Buffer buffer;

    // A structured SRV of GPULight records (SceneLightLoad's StructuredBuffer<GPULight>); the allocation is a whole number
    // of records, so the view covers exactly the capacity.
    if(!buffer.InitializeGPUWritable(device, static_cast<size_t>(bytes)) ||
        !buffer.CreateStructuredSRV(device, descriptorHeap, static_cast<std::uint32_t>(RecordBytes)))
    {
        buffer.Shutdown();
        Log::Error("Failed to allocate the scene light buffer for " + std::to_string(lightCount) + " lights (" +
            std::to_string(bytes) + " bytes).");
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
    return true;
}

bool SceneLightBuffer::Record(ID3D12Device* device, DescriptorHeap& descriptorHeap, ID3D12GraphicsCommandList* commandList,
    UploadBuffer& upload, const SceneLightTable& table, std::uint64_t lastSubmittedFence, std::uint64_t completedFence,
    SceneLightConstants& constants)
{
    if(!device || !commandList || table.lights.size() != table.lightCount)
    {
        return false;
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

    if((!m_buffer.GetResource() || table.lightCount > m_capacity) &&
        !Grow(device, descriptorHeap, table.lightCount, lastSubmittedFence))
    {
        return false;
    }

    if(table.lightCount != 0)
    {
        const std::uint64_t bytes = std::uint64_t{table.lightCount} * RecordBytes;
        UploadLocation source;

        if(upload.Allocate(table.lights.data(), static_cast<size_t>(bytes), 16, source) == 0)
        {
            Log::Error("Failed to allocate the scene light upload (" + std::to_string(bytes) + " bytes).");
            return false;
        }

        m_buffer.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(m_buffer.GetResource(), 0, source.resource, source.offset, bytes);
    }

    // Deferred lighting reads it from a pixel shader; every other consumer from compute or ray tracing.
    m_buffer.Transition(commandList, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    constants = {};
    constants.lightCount = table.lightCount;
    constants.lightBufferIndex = m_buffer.GetDescriptorIndex();
    return true;
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
}
