#include "ResourceUploader.h"
#include "Core/Log.h"
#ifdef _DEBUG
#include "GraphicsDevice.h"
#endif

#include <cstring>
#include <cstdlib>
#include <string>
#include <limits>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace
{
    [[noreturn]] void FailUploadSynchronization(ID3D12Device* device, const std::string& message)
    {
        Log::Error(message);
        Log::Error("Upload device removal HRESULT: " + std::to_string(device->GetDeviceRemovedReason()));
#ifdef _DEBUG
        GraphicsDevice::ReportDeviceRemovedData(device, "ResourceUploader::FailUploadSynchronization");
#endif
        Log::Error("Upload work may still be in flight; aborting without staging release or resource teardown.");
        std::abort();
    }
}

bool ResourceUploader::Initialize(ID3D12Device* device)
{
    if (!device)
    {
        return false;
    }

    m_device = device;

    D3D12_COMMAND_QUEUE_DESC queueDescription{};

    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    HRESULT result = m_device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&m_commandQueue));

    if (FAILED(result))
    {
        return false;
    }

    result = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator));

    if (FAILED(result))
    {
        return false;
    }

    result = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator, nullptr,
                                         IID_PPV_ARGS(&m_commandList));

    if (FAILED(result))
    {
        return false;
    }

    result = m_commandList->Close();

    if (FAILED(result))
    {
        Log::Error("Failed to close initial upload command list. HRESULT: " + std::to_string(result));
        return false;
    }

    result = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));

    if (FAILED(result))
    {
        return false;
    }

    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (!m_fenceEvent)
    {
        return false;
    }

    return true;
}

void ResourceUploader::Shutdown()
{
    // Submitted uploads complete synchronously or abort before reaching teardown.

    if (m_fenceEvent)
    {
        CloseHandle(static_cast<HANDLE>(m_fenceEvent));

        m_fenceEvent = nullptr;
    }

    if (m_fence)
    {
        m_fence->Release();
        m_fence = nullptr;
    }

    if (m_commandList)
    {
        m_commandList->Release();
        m_commandList = nullptr;
    }

    if (m_commandAllocator)
    {
        m_commandAllocator->Release();
        m_commandAllocator = nullptr;
    }

    if (m_commandQueue)
    {
        m_commandQueue->Release();
        m_commandQueue = nullptr;
    }

    m_device = nullptr;

    m_fenceValue = 0;
}

ResourceUploader::~ResourceUploader()
{
    Shutdown();
}

bool ResourceUploader::UploadTexture(

    ID3D12Resource* destination,
    const void* data,
    size_t dataSize)
{
    if (!m_device ||
        !m_commandQueue ||
        !m_commandAllocator ||
        !m_commandList)
    {
        return false;
    }

    if (!m_fence ||
        !m_fenceEvent ||
        !destination ||
        !data)
    {
        return false;
    }

    if (dataSize == 0)
    {
        return false;
    }

    const D3D12_RESOURCE_DESC destinationDescription = destination->GetDesc();

    UINT64 uploadSize = 0;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};

    UINT numRows = 0;

    UINT64 rowSize = 0;

    m_device->GetCopyableFootprints(&destinationDescription, 0, 1, 0, &footprint, &numRows, &rowSize, &uploadSize);

    if (uploadSize == 0)
    {
        return false;
    }

    if (dataSize < rowSize * numRows)
    {
        return false;
    }

    ID3D12Resource* uploadBuffer = nullptr;

    D3D12_HEAP_PROPERTIES heapProperties{};

    heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDescription{};

    bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDescription.Width = uploadSize;
    bufferDescription.Height = 1;
    bufferDescription.DepthOrArraySize = 1;

    bufferDescription.MipLevels = 1;
    bufferDescription.SampleDesc.Count = 1;
    bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT result =
        m_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &bufferDescription,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));

    if (FAILED(result))
    {
        return false;
    }

    void* mappedData = nullptr;

    result = uploadBuffer->Map(0, nullptr, &mappedData);

    if (FAILED(result))
    {
        uploadBuffer->Release();
        return false;
    }

    unsigned char* destinationData = static_cast<unsigned char*>(mappedData);

    const unsigned char* sourceData = static_cast<const unsigned char*>(data);

    for (UINT row = 0; row < numRows; ++row)
    {
        std::memcpy(destinationData + footprint.Offset + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                    sourceData + static_cast<size_t>(row) * rowSize, static_cast<size_t>(rowSize));
    }

    uploadBuffer->Unmap(0, nullptr);

    result = m_commandAllocator->Reset();

    if (FAILED(result))
    {
        Log::Error("Failed to reset texture upload allocator. HRESULT: " + std::to_string(result));

        uploadBuffer->Release();
        return false;
    }

    result = m_commandList->Reset(m_commandAllocator, nullptr);

    if (FAILED(result))
    {
        Log::Error("Failed to reset texture upload command list. HRESULT: " + std::to_string(result));
        uploadBuffer->Release();

        return false;
    }

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = uploadBuffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION destinationLocation{};

    destinationLocation.pResource = destination;
    destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destinationLocation.SubresourceIndex = 0;

    m_commandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &source, nullptr);

    return FinishUpload(uploadBuffer);
}

bool ResourceUploader::UploadTexture(
    ID3D12Resource* destination,
    const TextureSubresourceData* subresources,
    unsigned int subresourceCount)
{
    if (!m_device ||
        !m_commandQueue ||
        !m_commandAllocator ||
        !m_commandList)
    {
        return false;
    }

    if (!m_fence ||
        !m_fenceEvent ||
        !destination ||
        !subresources)
    {
        return false;
    }

    if (subresourceCount == 0)
    {
        return false;
    }

    const D3D12_RESOURCE_DESC description = destination->GetDesc();

    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.SampleDesc.Count != 1)
    {
        return false;
    }

    const unsigned int totalSubresources =

        static_cast<unsigned int>(description.MipLevels) * static_cast<unsigned int>(description.DepthOrArraySize);

    if (subresourceCount > totalSubresources)
    {
        return false;
    }

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresourceCount);

    std::vector<UINT> rowCounts(subresourceCount);

    std::vector<UINT64> rowSizes(subresourceCount);

    UINT64 uploadSize = 0;

    m_device->GetCopyableFootprints(&description, 0, subresourceCount, 0, footprints.data(), rowCounts.data(),
                                    rowSizes.data(), &uploadSize);

    if (uploadSize == 0 || uploadSize > (std::numeric_limits<size_t>::max)())
    {
        return false;
    }

    for (unsigned int index = 0; index < subresourceCount; ++index)
    {
        const TextureSubresourceData& source = subresources[index];

        const UINT64 rowSize = rowSizes[index];
        const UINT rowCount = rowCounts[index];

        if (!source.data || rowSize == 0 || rowCount == 0 || source.rowPitch < rowSize)
        {
            return false;
        }

        const UINT64 requiredSize = static_cast<UINT64>(rowCount - 1) * source.rowPitch + rowSize;

        if (source.slicePitch < requiredSize)
        {
            return false;
        }

        const UINT64 destinationEnd = footprints[index].Offset +
                                      static_cast<UINT64>(rowCount - 1) * footprints[index].Footprint.RowPitch +

                                      rowSize;

        if (destinationEnd > uploadSize)
        {
            return false;
        }
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDescription{};
    bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;

    bufferDescription.Width = uploadSize;
    bufferDescription.Height = 1;

    bufferDescription.DepthOrArraySize = 1;

    bufferDescription.MipLevels = 1;
    bufferDescription.SampleDesc.Count = 1;
    bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* uploadBuffer = nullptr;

    HRESULT result =
        m_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &bufferDescription,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));

    if (FAILED(result))
    {
        return false;
    }

    void* mappedData = nullptr;

    result = uploadBuffer->Map(0, nullptr, &mappedData);

    if (FAILED(result))
    {
        uploadBuffer->Release();
        return false;
    }

    unsigned char* destinationBytes = static_cast<unsigned char*>(mappedData);

    for (unsigned int index = 0; index < subresourceCount; ++index)
    {
        const TextureSubresourceData& source = subresources[index];

        const unsigned char* sourceBytes = static_cast<const unsigned char*>(source.data);

        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = footprints[index];

        for (UINT row = 0; row < rowCounts[index]; ++row)
        {
            const size_t sourceOffset = static_cast<size_t>(row) * source.rowPitch;
            const size_t destinationOffset =
                static_cast<size_t>(footprint.Offset) + static_cast<size_t>(row) * footprint.Footprint.RowPitch;

            std::memcpy(destinationBytes + destinationOffset, sourceBytes + sourceOffset,
                        static_cast<size_t>(rowSizes[index]));
        }
    }

    uploadBuffer->Unmap(0, nullptr);

    result = m_commandAllocator->Reset();

    if (FAILED(result))
    {
        uploadBuffer->Release();
        return false;
    }

    result = m_commandList->Reset(m_commandAllocator, nullptr);

    if (FAILED(result))
    {
        uploadBuffer->Release();
        return false;
    }

    for (unsigned int index = 0; index < subresourceCount; ++index)
    {
        D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
        sourceLocation.pResource = uploadBuffer;
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

        sourceLocation.PlacedFootprint = footprints[index];

        D3D12_TEXTURE_COPY_LOCATION destinationLocation{};

        destinationLocation.pResource = destination;

        destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destinationLocation.SubresourceIndex = index;

        m_commandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
    }

    return FinishUpload(uploadBuffer);
}

bool ResourceUploader::FinishUpload(ID3D12Resource* uploadBuffer)
{
    HRESULT result = m_commandList->Close();

    if (FAILED(result))
    {
        Log::Error("Failed to close upload command list before submission. HRESULT: " + std::to_string(result));
        uploadBuffer->Release();
        return false;
    }

    ID3D12CommandList* commandLists[] = {m_commandList};

    m_commandQueue->ExecuteCommandLists(1, commandLists);

    // From here, failure must not return to callers that may release the destination.
    ++m_fenceValue;

    result = m_commandQueue->Signal(m_fence, m_fenceValue);

    if (FAILED(result))
    {
        FailUploadSynchronization(m_device,
                                  "Failed to signal upload fence after submission. HRESULT: " + std::to_string(result));
    }

    unsigned long long completedValue = m_fence->GetCompletedValue();

    if (completedValue == UINT64_MAX)
    {
        FailUploadSynchronization(m_device, "Upload fence reports device removal after submission.");
    }

    if (completedValue < m_fenceValue)
    {
        result = m_fence->SetEventOnCompletion(m_fenceValue, static_cast<HANDLE>(m_fenceEvent));

        if (FAILED(result))
        {
            FailUploadSynchronization(m_device,
                                      "Failed to register upload fence event. HRESULT: " + std::to_string(result));
        }

        const DWORD waitResult = WaitForSingleObject(static_cast<HANDLE>(m_fenceEvent), INFINITE);

        if (waitResult == WAIT_FAILED)
        {
            const DWORD error = GetLastError();

            FailUploadSynchronization(m_device, "Upload fence wait failed. Win32 error: " + std::to_string(error));
        }

        if (waitResult != WAIT_OBJECT_0)
        {
            FailUploadSynchronization(m_device, "Unexpected upload fence wait result: " + std::to_string(waitResult));
        }
    }

    completedValue = m_fence->GetCompletedValue();

    if (completedValue == UINT64_MAX)
    {
        FailUploadSynchronization(m_device, "Upload fence reports device removal while verifying completion.");
    }

    if (completedValue < m_fenceValue)
    {
        FailUploadSynchronization(m_device,
                                  "Upload fence has not completed. Requested: " + std::to_string(m_fenceValue) +
                                      "; completed: " + std::to_string(completedValue));
    }

    uploadBuffer->Release();

    return true;
}

bool ResourceUploader::UploadBuffer(
    ID3D12Resource* destination,
    const void* data,
    size_t dataSize,
    D3D12_RESOURCE_STATES finalState)
{
    if (!m_device ||
        !m_commandQueue ||
        !m_commandAllocator ||
        !m_commandList)
    {
        return false;
    }

    if (!m_fence ||
        !m_fenceEvent ||
        !destination ||
        !data)
    {
        return false;
    }

    if (dataSize == 0)
    {
        return false;
    }

    const D3D12_RESOURCE_DESC description = destination->GetDesc();

    if (description.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || dataSize > description.Width)
    {
        return false;
    }

    ID3D12Resource* uploadBuffer = nullptr;

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDescription{};
    bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDescription.Width = dataSize;

    bufferDescription.Height = 1;

    bufferDescription.DepthOrArraySize = 1;
    bufferDescription.MipLevels = 1;
    bufferDescription.SampleDesc.Count = 1;

    bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT result =
        m_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &bufferDescription,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));

    if (FAILED(result))
    {
        return false;
    }

    void* mappedData = nullptr;

    D3D12_RANGE readRange{};
    result = uploadBuffer->Map(0, &readRange, &mappedData);

    if (FAILED(result))
    {
        uploadBuffer->Release();
        return false;
    }

    std::memcpy(mappedData, data, dataSize);
    uploadBuffer->Unmap(0, nullptr);

    result = m_commandAllocator->Reset();

    if (FAILED(result))
    {
        uploadBuffer->Release();
        return false;
    }

    result = m_commandList->Reset(m_commandAllocator, nullptr);

    if (FAILED(result))
    {
        uploadBuffer->Release();
        return false;
    }

    // A COMMON buffer implicitly promotes to COPY_DEST for this first access.
    // The following barrier is in the same ExecuteCommandLists submission,
    // before buffer decay, so COPY_DEST is the correct StateBefore.

    m_commandList->CopyBufferRegion(destination, 0, uploadBuffer, 0, dataSize);

    if (finalState != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        D3D12_RESOURCE_BARRIER barrier{};

        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destination;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = finalState;
        m_commandList->ResourceBarrier(1, &barrier);
    }

    return FinishUpload(uploadBuffer);
}
