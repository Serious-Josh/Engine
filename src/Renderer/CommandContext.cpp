#include "CommandContext.h"
#include "Core/Log.h"
#ifdef _DEBUG
#include "GraphicsDevice.h"
#endif

#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

bool CommandContext::Initialize(ID3D12Device* device)
{
    if(!device){
        return false;
    }

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

    for(unsigned int i = 0; i < FrameCount; ++i){
        result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i]));

        if(FAILED(result)){
            return false;
        }
    }

    result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[0], nullptr, IID_PPV_ARGS(&m_commandList));

    if(FAILED(result))
    {
        return false;
    }

    result = m_commandList->QueryInterface(IID_PPV_ARGS(&m_rayTracingCommandList));

    if(FAILED(result)){
        return false;
    }

    result = m_commandList->Close();

    if(FAILED(result))
    {
        return false;
    }

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

    if(m_rayTracingCommandList){
        m_rayTracingCommandList->Release();
        m_rayTracingCommandList = nullptr;
    }

    if(m_commandList)
    {
        m_commandList->Release();
        m_commandList = nullptr;
    }

    for(unsigned int i = 0; i < FrameCount; ++i){
        if(m_commandAllocators[i]){
            m_commandAllocators[i]->Release();
            m_commandAllocators[i] = nullptr;
        }
    }

    if(m_commandQueue)
    {
        m_commandQueue->Release();
        m_commandQueue = nullptr;
    }

    m_fenceValue = 0;

    for(unsigned int i = 0; i < FrameCount; ++i){
        m_frameFenceValues[i] = 0;
    }
}

bool CommandContext::Begin(unsigned int frameIndex)
{
    if(frameIndex >= FrameCount){
        return false;
    }

    const unsigned long long frameFenceValue = m_frameFenceValues[frameIndex];
    const auto deviceRemoved = [&]()
    {
        ID3D12Device* device = nullptr;
        const HRESULT result = m_fence->GetDevice(IID_PPV_ARGS(&device));
        if(FAILED(result)){
            Log::Error("Frame fence reports device removal; GetDevice HRESULT: " + std::to_string(result));
        }else{
            const HRESULT reason = device->GetDeviceRemovedReason();
            Log::Error("Frame fence reports device removal; removal HRESULT: " + std::to_string(reason));
#ifdef _DEBUG
            GraphicsDevice::ReportDeviceRemovedData(device, "CommandContext::Begin frame fence");
#endif
            device->Release();
        }
    };

    unsigned long long completedValue = m_fence->GetCompletedValue();
    if(completedValue == UINT64_MAX){
        deviceRemoved();
        return false;
    }

    if(frameFenceValue != 0 && completedValue < frameFenceValue){
        HRESULT result = m_fence->SetEventOnCompletion(frameFenceValue, static_cast<HANDLE>(m_fenceEvent));

        if(FAILED(result)){
            Log::Error("Failed to register frame fence event. HRESULT: " + std::to_string(result));
            return false;
        }

        const DWORD waitResult = WaitForSingleObject(static_cast<HANDLE>(m_fenceEvent), INFINITE);
        if(waitResult == WAIT_FAILED){
            const DWORD error = GetLastError();
            Log::Error("Frame fence wait failed. Win32 error: " + std::to_string(error));
            return false;
        }
        if(waitResult != WAIT_OBJECT_0){
            Log::Error("Unexpected frame fence wait result: " + std::to_string(waitResult));
            return false;
        }

        completedValue = m_fence->GetCompletedValue();
        if(completedValue == UINT64_MAX){
            deviceRemoved();
            return false;
        }
        if(completedValue < frameFenceValue){
            Log::Error("Frame fence has not completed. Requested: " + std::to_string(frameFenceValue) + "; completed: " + std::to_string(completedValue));
            return false;
        }
    }

    ID3D12CommandAllocator* commandAllocator = m_commandAllocators[frameIndex];

    HRESULT result = commandAllocator->Reset();

    if(FAILED(result)){
        return false;
    }

    result = m_commandList->Reset(commandAllocator, nullptr);

    return SUCCEEDED(result);
}

bool CommandContext::End()
{
    return SUCCEEDED(m_commandList->Close());
}

bool CommandContext::Execute(unsigned int frameIndex)
{
    if(frameIndex >= FrameCount){
        return false;
    }

    ID3D12CommandList* commandLists[] = {m_commandList};
    m_commandQueue->ExecuteCommandLists(1, commandLists);

    const unsigned long long fenceValue = m_fenceValue;

    HRESULT result = m_commandQueue->Signal(m_fence, fenceValue);

    if(FAILED(result)){
        Log::Error("Failed to signal frame fence after command submission. Frame index: " + std::to_string(frameIndex) + "; fence value: " + std::to_string(fenceValue) + "; HRESULT: " + std::to_string(result));
#ifdef _DEBUG
        ID3D12Device* device = nullptr;
        if(SUCCEEDED(m_commandQueue->GetDevice(IID_PPV_ARGS(&device)))){
            GraphicsDevice::ReportDeviceRemovedData(device, "CommandContext::Execute Signal");
            device->Release();
        }
#endif
        return false;
    }

    m_frameFenceValues[frameIndex] = fenceValue;
    ++m_fenceValue;

    return true;
}

bool CommandContext::WaitForGPU()
{
    if(!m_commandQueue || !m_fence || !m_fenceEvent){
        Log::Error("Cannot wait for GPU: command context is not initialized.");
        return false;
    }

    const auto deviceRemoved = [&]()
    {
        ID3D12Device* device = nullptr;
        const HRESULT result = m_fence->GetDevice(IID_PPV_ARGS(&device));
        if(FAILED(result)){
            Log::Error("GPU fence reports device removal; GetDevice HRESULT: " + std::to_string(result));
        }else{
            const HRESULT reason = device->GetDeviceRemovedReason();
            Log::Error("GPU fence reports device removal; removal HRESULT: " + std::to_string(reason));
#ifdef _DEBUG
            GraphicsDevice::ReportDeviceRemovedData(device, "CommandContext::WaitForGPU idle fence");
#endif
            device->Release();
        }
    };

    if(m_fence->GetCompletedValue() == UINT64_MAX){
        deviceRemoved();
        return false;
    }

    const unsigned long long fenceValue = m_fenceValue;
    HRESULT result = m_commandQueue->Signal(m_fence, fenceValue);
    if(FAILED(result)){
        Log::Error("Failed to signal GPU-idle fence; submitted work may still be in flight. HRESULT: " + std::to_string(result));
        return false;
    }

    ++m_fenceValue;

    unsigned long long completedValue = m_fence->GetCompletedValue();
    if(completedValue == UINT64_MAX){
        deviceRemoved();
        return false;
    }

    while(completedValue < fenceValue){
        result = m_fence->SetEventOnCompletion(fenceValue, static_cast<HANDLE>(m_fenceEvent));
        if(FAILED(result)){
            Log::Error("Failed to register GPU-idle fence event. HRESULT: " + std::to_string(result));
            return false;
        }

        const DWORD waitResult = WaitForSingleObject(static_cast<HANDLE>(m_fenceEvent), INFINITE);
        if(waitResult != WAIT_OBJECT_0){
            const DWORD error = waitResult == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
            Log::Error("GPU-idle wait failed. Wait result: " + std::to_string(waitResult) + "; Win32 error: " + std::to_string(error));
            return false;
        }

        completedValue = m_fence->GetCompletedValue();
        if(completedValue == UINT64_MAX){
            deviceRemoved();
            return false;
        }
    }

    return true;
}

ID3D12GraphicsCommandList* CommandContext::GetCommandList() const
{
    return m_commandList;
}

ID3D12CommandQueue* CommandContext::GetCommandQueue() const
{
    return m_commandQueue;
}

ID3D12GraphicsCommandList4* CommandContext::GetRayTracingCommandList() const
{
    return m_rayTracingCommandList;
}

uint64_t CommandContext::GetCompletedFenceValue() const
{
    if(!m_fence){
        return 0;
    }

    return m_fence->GetCompletedValue();
}

uint64_t CommandContext::GetFrameFenceValue(unsigned int frameIndex) const
{
    if(frameIndex >= FrameCount){
        return 0;
    }

    return m_frameFenceValues[frameIndex];
}