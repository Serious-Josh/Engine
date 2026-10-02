#include "ResourceUploader.h"

#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

bool ResourceUploader::Initialize(ID3D12Device* device)
{
    if(!device){
        return false;
    }

    m_device = device;

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    HRESULT result = m_device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&m_commandQueue));

    if(FAILED(result)){
        return false;
    }

    result = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator));

    if(FAILED(result)){
        return false;
    }

    result = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator, nullptr, IID_PPV_ARGS(&m_commandList));

    if(FAILED(result)){
        return false;
    }

    m_commandList->Close();

    result = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));

    if(FAILED(result)){
        return false;
    }

    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if(!m_fenceEvent){
        return false;
    }

    return true;
}

void ResourceUploader::Shutdown()
{
    if(m_fenceEvent){
        CloseHandle(
            static_cast<HANDLE>(m_fenceEvent)
        );

        m_fenceEvent = nullptr;
    }

    if(m_fence){
        m_fence->Release();
        m_fence = nullptr;
    }

    if(m_commandList){
        m_commandList->Release();
        m_commandList = nullptr;
    }

    if(m_commandAllocator){
        m_commandAllocator->Release();
        m_commandAllocator = nullptr;
    }

    if(m_commandQueue){
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

bool ResourceUploader::UploadTexture(ID3D12Resource* destination, const void* data, size_t dataSize)
{
    if(!m_device || !m_commandQueue || !m_commandAllocator || !m_commandList || !m_fence || !m_fenceEvent || !destination || !data || dataSize == 0){
        return false;
    }

    const D3D12_RESOURCE_DESC destinationDescription = destination->GetDesc();

    UINT64 uploadSize = 0;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSize = 0;

    m_device->GetCopyableFootprints(&destinationDescription, 0, 1, 0, &footprint, &numRows, &rowSize, &uploadSize);

    if(uploadSize == 0){
        return false;
    }

    if(dataSize < rowSize * numRows){
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

    HRESULT result = m_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &bufferDescription, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));

    if(FAILED(result)){
        return false;
    }

    void* mappedData = nullptr;

    result = uploadBuffer->Map(0, nullptr, &mappedData);

    if(FAILED(result)){
        uploadBuffer->Release();
        return false;
    }

    unsigned char* destinationData = static_cast<unsigned char*>(mappedData);

    const unsigned char* sourceData = static_cast<const unsigned char*>(data);

    for(UINT row = 0; row < numRows; ++row){
        std::memcpy(destinationData + footprint.Offset + static_cast<size_t>(row) * footprint.Footprint.RowPitch, sourceData + static_cast<size_t>(row) * rowSize, static_cast<size_t>(rowSize));
    }

    uploadBuffer->Unmap(0, nullptr);

    m_commandAllocator->Reset();

    m_commandList->Reset(m_commandAllocator, nullptr);

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

bool ResourceUploader::FinishUpload(ID3D12Resource* uploadBuffer)
{
    HRESULT result = m_commandList->Close();

    if(FAILED(result)){
        uploadBuffer->Release();
        return false;
    }

    ID3D12CommandList* commandLists[] = {m_commandList};
    
    m_commandQueue->ExecuteCommandLists(1, commandLists);

    ++m_fenceValue;

    result = m_commandQueue->Signal(m_fence, m_fenceValue);

    if(FAILED(result)){
        uploadBuffer->Release();
        return false;
    }

    if(m_fence->GetCompletedValue() < m_fenceValue){
        result = m_fence->SetEventOnCompletion(m_fenceValue, static_cast<HANDLE>(m_fenceEvent));

        if(FAILED(result)){
            uploadBuffer->Release();
            return false;
        }

        WaitForSingleObject(static_cast<HANDLE>(m_fenceEvent), INFINITE);
    }

    uploadBuffer->Release();

    return true;
}

bool ResourceUploader::UploadBuffer(ID3D12Resource* destination, const void* data, size_t dataSize, D3D12_RESOURCE_STATES finalState)
{
    if(!m_device || !m_commandQueue || !m_commandAllocator || !m_commandList || !m_fence || !m_fenceEvent || !destination || !data || dataSize == 0){
        return false;
    }

    const D3D12_RESOURCE_DESC description = destination->GetDesc();
    if(description.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || dataSize > description.Width){
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

    HRESULT result = m_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &bufferDescription, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));

    if(FAILED(result)){
        return false;
    }

    void* mappedData = nullptr;

    D3D12_RANGE readRange{};
    result = uploadBuffer->Map(0, &readRange, &mappedData);

    if(FAILED(result)){
        uploadBuffer->Release();
        return false;
    }

    std::memcpy(mappedData, data, dataSize);
    uploadBuffer->Unmap(0, nullptr);

    result = m_commandAllocator->Reset();
    if(FAILED(result)){
        uploadBuffer->Release();
        return false;
    }

    result = m_commandList->Reset(m_commandAllocator, nullptr);
    if(FAILED(result)){
        uploadBuffer->Release();
        return false;
    }

    m_commandList->CopyBufferRegion(destination, 0, uploadBuffer, 0, dataSize);

    if(finalState != D3D12_RESOURCE_STATE_COPY_DEST){
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
