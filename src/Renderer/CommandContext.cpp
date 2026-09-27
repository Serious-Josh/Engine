#include "CommandContext.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

bool CommandContext::Initialize(ID3D12Device* device)
{
    if(!device){
        return false;
    }

    // Creating command queue
    D3D12_COMMAND_QUEUE_DESC queueDescription{};

    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDescription.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDescription.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDescription.NodeMask = 0;

    HRESULT result = device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&m_commandQueue));

    if(FAILED(result))
    {
        return false;
    }

    // Creating command allocator
    result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator));

    if(FAILED(result))
    {
        return false;
    }

    // Creating command list
    result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator, nullptr, IID_PPV_ARGS(&m_commandList));

    if(FAILED(result))
    {
        return false;
    }

    result = m_commandList->Close();

    if(FAILED(result))
    {
        return false;
    }

    // Creating fence
    result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));

    if(FAILED(result))
    {
        return false;
    }

    m_fenceValue = 1;

    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if(m_fenceEvent == nullptr)
    {
        return false;
    }

    return true;
}

void CommandContext::Shutdown()
{
    if(m_fence)
    {
        m_fence->Release();
        m_fence = nullptr;
    }

    if(m_fenceEvent)
    {
        CloseHandle(
            static_cast<HANDLE>(m_fenceEvent)
        );

        m_fenceEvent = nullptr;
    }

    if(m_commandList)
    {
        m_commandList->Release();
        m_commandList = nullptr;
    }

    if(m_commandAllocator)
    {
        m_commandAllocator->Release();
        m_commandAllocator = nullptr;
    }

    if(m_commandQueue)
    {
        m_commandQueue->Release();
        m_commandQueue = nullptr;
    }

    m_fenceValue = 0;
}

bool CommandContext::Begin()
{
    HRESULT result = m_commandAllocator->Reset();

    if(FAILED(result)){
        return false;
    }

    result = m_commandList->Reset(m_commandAllocator, nullptr);

    return SUCCEEDED(result);
}

bool CommandContext::End()
{
    return SUCCEEDED(m_commandList->Close());
}

void CommandContext::Execute()
{
    ID3D12CommandList* commandLists[] = {m_commandList};
    m_commandQueue->ExecuteCommandLists(1, commandLists);
}

void CommandContext::WaitForGPU()
{
    const unsigned int long long fenceValue = m_fenceValue;

    HRESULT result = m_commandQueue->Signal(m_fence, fenceValue);

    if(FAILED(result))
    {
        return;
    }

    ++m_fenceValue;

    if(m_fence->GetCompletedValue() < fenceValue)
    {
        result = m_fence->SetEventOnCompletion(fenceValue, static_cast<HANDLE>(m_fenceEvent));

        if(FAILED(result))
        {
            return;
        }

        WaitForSingleObject(static_cast<HANDLE>(m_fenceEvent), INFINITE);
    }
}

ID3D12GraphicsCommandList* CommandContext::GetCommandList() const
{
    return m_commandList;
}

ID3D12CommandQueue* CommandContext::GetCommandQueue() const
{
    return m_commandQueue;
}