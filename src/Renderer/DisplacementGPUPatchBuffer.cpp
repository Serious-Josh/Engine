#include "DisplacementGPUPatchBuffer.h"

#include "Renderer/DescriptorHeap.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/UploadBuffer.h"
#include "Core/Log.h"

#include <cstdint>
#include <limits>
#include <utility>
#include <cstdlib>

bool DisplacementGPUPatchBuffer::Initialize(
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const DisplacementGPUIndexLayout& layout)
{
    if (m_persistentHeap || !device || layout.patches.empty())
    {
        return false;
    }

    constexpr std::uint64_t patchStride = sizeof(DisplacementGPUPatch);

    constexpr std::uint64_t maxRawBufferBytes = static_cast<std::uint64_t>((std::numeric_limits<UINT>::max)());

    const std::uint64_t patchCount = layout.patches.size();

    if (patchCount > maxRawBufferBytes / patchStride)
    {
        return false;
    }

    const size_t bufferBytes = layout.patches.size() * sizeof(DisplacementGPUPatch);

    if (m_buffer.GetResource())
    {
        return m_buffer.ReplaceData(device, uploader, layout.patches.data(), bufferBytes,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    if (!m_buffer.Initialize(device, uploader, layout.patches.data(), bufferBytes,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
    {
        return false;
    }

    if (!m_buffer.CreateRawSRV(device, descriptorHeap))
    {
        m_buffer.Shutdown();
        return false;
    }

    return true;
}

bool DisplacementGPUPatchBuffer::PreparePersistentStorage(

    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    size_t patchCapacity,
    bool gpuIdleConfirmed)
{
    if (!device || !gpuIdleConfirmed || m_updateUnresolved || patchCapacity == 0)
    {
        return false;
    }

    if (patchCapacity > UINT_MAX / sizeof(DisplacementGPUPatch) ||

        patchCapacity > (std::numeric_limits<size_t>::max)() / sizeof(DisplacementGPUPatch) ||
        (m_buffer.GetResource() && !m_persistentHeap) || (m_persistentHeap && m_persistentHeap != &descriptorHeap))
    {
        return false;
    }

    const size_t capacityBytes = patchCapacity * sizeof(DisplacementGPUPatch);

    if (m_buffer.GetResource() && m_buffer.GetResource()->GetDesc().Width >= capacityBytes)
    {
        m_recordedState = D3D12_RESOURCE_STATE_COMMON;
        return true;
    }

    Buffer prepared;

    if (!prepared.InitializeGPUWritable(device, capacityBytes))
    {
        return false;
    }

    if (m_buffer.GetResource())
    {
        if (!m_buffer.CommitReplacement(device, prepared))
        {
            prepared.Shutdown();
            return false;
        }
    }
    else
    {
        if (!prepared.CreateRawSRV(device, descriptorHeap))
        {

            prepared.Shutdown();

            return false;
        }

        m_buffer = std::move(prepared);
    }
    prepared.Shutdown();
    m_persistentHeap = &descriptorHeap;

    m_recordedState = D3D12_RESOURCE_STATE_COMMON;
    return true;
}

bool DisplacementGPUPatchBuffer::HasReadyPersistentCapacity(size_t requiredPatchCount) const
{
    if (!m_persistentHeap || !m_buffer.GetResource() || m_updateUnresolved ||

        m_recordedState != D3D12_RESOURCE_STATE_COMMON)
    {
        return false;
    }

    if (requiredPatchCount == 0 || requiredPatchCount > UINT_MAX / sizeof(DisplacementGPUPatch) ||

        requiredPatchCount > (std::numeric_limits<size_t>::max)() / sizeof(DisplacementGPUPatch) ||

        m_buffer.GetDescriptorIndex() == UINT_MAX)
    {
        return false;
    }

    if (!m_persistentHeap->GetHeap())
    {
        return false;
    }

    const D3D12_DESCRIPTOR_HEAP_DESC heap = m_persistentHeap->GetHeap()->GetDesc();

    const D3D12_RESOURCE_DESC resource = m_buffer.GetResource()->GetDesc();

    return heap.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
           (heap.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0 &&
           m_buffer.GetDescriptorIndex() < heap.NumDescriptors &&
           resource.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && resource.Width != 0 &&
           resource.Width % sizeof(DisplacementGPUPatch) == 0 && resource.Width <= UINT_MAX &&
           resource.Width >= requiredPatchCount * sizeof(DisplacementGPUPatch);
}

bool DisplacementGPUPatchBuffer::RecordUpdate(
    ID3D12GraphicsCommandList* commandList,
    UploadBuffer& uploadBuffer,
    const DisplacementGPUIndexLayout& layout)
{
    if (!commandList || commandList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !m_persistentHeap ||
        m_updateUnresolved)
    {
        return false;
    }

    if (!m_buffer.GetResource() || m_buffer.GetDescriptorIndex() == UINT_MAX || layout.patches.empty() ||

        m_recordedState != D3D12_RESOURCE_STATE_COMMON)
    {
        return false;
    }

    if (layout.patches.size() > UINT_MAX / sizeof(DisplacementGPUPatch) ||

        layout.patches.size() > (std::numeric_limits<size_t>::max)() / sizeof(DisplacementGPUPatch))
    {
        return false;
    }

    const size_t bytes = layout.patches.size() * sizeof(DisplacementGPUPatch);
    ID3D12Resource* source = uploadBuffer.GetResource();

    if (!source || bytes > m_buffer.GetResource()->GetDesc().Width)
    {
        return false;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS address =

        uploadBuffer.Allocate(layout.patches.data(), bytes, sizeof(std::uint32_t));
    const D3D12_GPU_VIRTUAL_ADDRESS base = source->GetGPUVirtualAddress();

    if (address == 0 || address < base)
    {
        return false;
    }

    const UINT64 offset = address - base;
    const UINT64 sourceBytes = source->GetDesc().Width;

    if (offset % sizeof(std::uint32_t) != 0 || offset > sourceBytes || bytes > sourceBytes - offset)
    {
        return false;
    }

    commandList->CopyBufferRegion(m_buffer.GetResource(), 0, source, offset, bytes);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;

    barrier.Transition.pResource = m_buffer.GetResource();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(1, &barrier);
    m_recordedState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    m_updateUnresolved = true;
    return true;
}

void DisplacementGPUPatchBuffer::RestoreStateAfterAbort(D3D12_RESOURCE_STATES snapshot)
{
    m_recordedState = snapshot;
    m_updateUnresolved = false;
}

bool DisplacementGPUPatchBuffer::ConfirmUpdateCompletion(bool gpuCompletionConfirmed)
{
    if (!gpuCompletionConfirmed || !m_updateUnresolved)
    {
        return false;
    }

    m_recordedState = D3D12_RESOURCE_STATE_COMMON;
    m_updateUnresolved = false;
    return true;
}

void DisplacementGPUPatchBuffer::Shutdown()
{
    if (m_updateUnresolved)
    {
        Log::Error("Patch metadata recording/submission unresolved; aborting without resource teardown.");

        std::abort();
    }

    m_buffer.Shutdown();

    m_persistentHeap = nullptr;
    m_recordedState = D3D12_RESOURCE_STATE_COMMON;
    m_updateUnresolved = false;
}
