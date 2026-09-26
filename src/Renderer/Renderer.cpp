#include "Renderer.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <dxgi1_6.h>
#include <d3d12.h>


bool Renderer::Initialize(void* windowHandle, unsigned int width, unsigned int height)
{
    //Getting graphics adapter and creating D3D12 device
    HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory));
    
    if(FAILED(result)){
        return false;
    }

    IDXGIAdapter1* adapter = nullptr;

    for(UINT index = 0; m_factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index){
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);

        if(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE){
            adapter->Release();
            adapter = nullptr;
            continue;
        }

        result = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));

        if(SUCCEEDED(result)){
            break;
        }

        adapter->Release();
        adapter = nullptr;
    }

    if(adapter){
        adapter->Release();
    }

    if(m_device == nullptr){
        return false;
    }

    //Creating the command queue
    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDescription.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDescription.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDescription.NodeMask = 0;

    result = m_device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&m_commandQueue));
    
    if(FAILED(result)){
        return false;
    }


    //Creating the swap chain
    DXGI_SWAP_CHAIN_DESC1 swapChainDescription{};
    swapChainDescription.Width = width;
    swapChainDescription.Height = height;
    swapChainDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDescription.Stereo = FALSE;
    swapChainDescription.SampleDesc.Count = 1;
    swapChainDescription.SampleDesc.Quality = 0;
    swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDescription.BufferCount = 2;
    swapChainDescription.Scaling = DXGI_SCALING_STRETCH;
    swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDescription.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapChainDescription.Flags = 0;

    IDXGISwapChain1* swapChain = nullptr;

    result = m_factory->CreateSwapChainForHwnd(m_commandQueue, static_cast<HWND>(windowHandle), &swapChainDescription, nullptr, nullptr, &swapChain);
    
    if(FAILED(result)){
        return false;
    }

    result = swapChain->QueryInterface(IID_PPV_ARGS(&m_swapChain));

    swapChain->Release();
    
    if(FAILED(result)){
        return false;
    }


    //Creating RTV Heap for Swap Chain Back Buffers
    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.NumDescriptors = 2;
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    heapDescription.NodeMask = 0;

    result = m_device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&m_renderTargetHeap));
    
    if(FAILED(result)){
        return false;
    }

    
    //Retrieving Back Buffers
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_renderTargetHeap->GetCPUDescriptorHandleForHeapStart();

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for(UINT index = 0; index < 2; ++index){
        result = m_swapChain->GetBuffer(index, IID_PPV_ARGS(&m_backBuffers[index]));
        
        if(FAILED(result)){
            return false;
        }

        m_device->CreateRenderTargetView(m_backBuffers[index], nullptr, rtvHandle);

        rtvHandle.ptr += m_rtvDescriptorSize;
    }


    //Creating Command Allocator and Command List
    result = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator));
    
    if(FAILED(result)){
        return false;
    }

    result = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator, nullptr, IID_PPV_ARGS(&m_commandList));
    
    if(FAILED(result)){
        return false;
    }

    result = m_commandList->Close();
    
    if(FAILED(result)){
        return false;
    }


    //Create Allocator Fence
    result = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));

    if(FAILED(result)){
        return false;
    }

    m_fenceValue = 1;

    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if(m_fenceEvent == nullptr){
        return false;
    }

    return true;
}

void Renderer::Render()
{
    HRESULT result = m_commandAllocator->Reset();

    if(FAILED(result)){
        return;
    }

    result = m_commandList->Reset(m_commandAllocator, nullptr);

    if(FAILED(result)){
        return;
    }

    ID3D12Resource* backBuffer = m_backBuffers[m_currentBackBuffer];

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    m_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_renderTargetHeap->GetCPUDescriptorHandleForHeapStart();

    rtvHandle.ptr += static_cast<SIZE_T>(m_currentBackBuffer * m_rtvDescriptorSize);

    const float clearColor[] = {0.1f, 0.2f, 0.4f, 1.0f};

    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

    m_commandList->ResourceBarrier(1, &barrier);
    
    result = m_commandList->Close();

    if(FAILED(result)){
        return;
    }

    ID3D12CommandList* commandLists[] = {m_commandList};
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    
    const unsigned long long currentFenceValue = m_fenceValue;

    result = m_commandQueue->Signal(m_fence, currentFenceValue);

    if(FAILED(result)){
        return;
    }

    m_fenceValue++;

    if(m_fence->GetCompletedValue() < currentFenceValue){
        result = m_fence->SetEventOnCompletion(currentFenceValue, static_cast<HANDLE>(m_fenceEvent));

        if(FAILED(result)){
            return;
        }

        WaitForSingleObject(static_cast<HANDLE>(m_fenceEvent), INFINITE);
    }

    m_swapChain->Present(1, 0);
    m_currentBackBuffer = m_swapChain->GetCurrentBackBufferIndex();
}

void Renderer::Shutdown()
{
    if(m_fence){
        m_fence->Release();
        m_fence = nullptr;
    }

    if(m_fenceEvent){
        CloseHandle(static_cast<HANDLE>(m_fenceEvent));
        m_fenceEvent = nullptr;
    }

    for(ID3D12Resource*& backBuffer : m_backBuffers){
        if(backBuffer){
            backBuffer->Release();
            backBuffer = nullptr;
        }
    }

    if(m_renderTargetHeap){
        m_renderTargetHeap->Release();
        m_renderTargetHeap = nullptr;
    }

    if(m_swapChain){
        m_swapChain->Release();
        m_swapChain = nullptr;
    }

    if(m_commandQueue){
        m_commandQueue->Release();
        m_commandQueue = nullptr;
    }

    if(m_commandList){
        m_commandList->Release();
        m_commandList = nullptr;
    }

    if(m_commandAllocator){
        m_commandAllocator->Release();
        m_commandAllocator = nullptr;
    }

    if(m_device){
        m_device->Release();
        m_device = nullptr;
    }

    if(m_factory){
        m_factory->Release();
        m_factory = nullptr;
    }
}