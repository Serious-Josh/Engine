#include "SwapChain.h"
#include "Core/Log.h"
#ifdef _DEBUG
#include "GraphicsDevice.h"
#endif

#include <string>

#include <dxgi1_6.h>
#include <Windows.h>

bool SwapChain::Initialize(IDXGIFactory4* factory, ID3D12Device* device, ID3D12CommandQueue* commandQueue, void* windowHandle, unsigned int width, unsigned int height)
{
    if(!factory || !device || !commandQueue || !windowHandle){
        return false;
    }

    m_device = device;
    m_width = width;
    m_height = height;

    DXGI_SWAP_CHAIN_DESC1 swapChainDescription{};

    swapChainDescription.Width = m_width;
    swapChainDescription.Height = m_height;
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

    HRESULT result = factory->CreateSwapChainForHwnd(commandQueue, static_cast<HWND>(windowHandle), &swapChainDescription, nullptr, nullptr, &swapChain);

    if(FAILED(result))
    {
        return false;
    }

    result = swapChain->QueryInterface(IID_PPV_ARGS(&m_swapChain));

    swapChain->Release();

    if(FAILED(result))
    {
        return false;
    }


    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};

    heapDescription.NumDescriptors = 2;
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    heapDescription.NodeMask = 0;

    result = m_device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&m_rtvHeap));

    if(FAILED(result))
    {
        return false;
    }

    m_rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    return CreateBackBufferViews();
}

bool SwapChain::Resize(unsigned int width, unsigned int height)
{
    if(width == 0 || height == 0){
        return false;
    }

    // The caller must wait for the GPU before releasing the back buffers.
    for(ID3D12Resource*& backBuffer : m_backBuffers){
        if(backBuffer){
            backBuffer->Release();
            backBuffer = nullptr;
        }
    }

    HRESULT result = m_swapChain->ResizeBuffers(2, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);

    if(FAILED(result)){
        Log::Error("Failed to resize swap-chain buffers. HRESULT: " + std::to_string(result));
#ifdef _DEBUG
        GraphicsDevice::ReportDeviceRemovedData(m_device, "SwapChain::Resize ResizeBuffers");
#endif
        return false;
    }

    m_width = width;
    m_height = height;

    return CreateBackBufferViews();
}

void SwapChain::Shutdown()
{
    for(ID3D12Resource*& backBuffer : m_backBuffers){
        if(backBuffer){
            backBuffer->Release();
            backBuffer = nullptr;
        }
    }

    if(m_rtvHeap){
        m_rtvHeap->Release();
        m_rtvHeap = nullptr;
    }

    if(m_swapChain){
        m_swapChain->Release();
        m_swapChain = nullptr;
    }

    m_device = nullptr;

    m_rtvDescriptorSize = 0;
    m_currentBackBuffer = 0;
    m_width = 0;
    m_height = 0;
}

bool SwapChain::CreateBackBufferViews()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for(UINT index = 0; index < 2; ++index)
    {
        HRESULT result = m_swapChain->GetBuffer(index, IID_PPV_ARGS(&m_backBuffers[index]));

        if(FAILED(result))
        {
            Log::Error("Failed to acquire swap-chain back buffer " + std::to_string(index) + ". HRESULT: " + std::to_string(result));
            return false;
        }

        m_backBuffers[index]->SetName(index == 0 ? L"Back Buffer 0" : L"Back Buffer 1");

        m_device->CreateRenderTargetView(m_backBuffers[index], nullptr, rtvHandle);

        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    m_currentBackBuffer = m_swapChain->GetCurrentBackBufferIndex();

    return true;
}

ID3D12Resource* SwapChain::GetCurrentBackBuffer() const
{
    return m_backBuffers[m_currentBackBuffer];
}

D3D12_CPU_DESCRIPTOR_HANDLE SwapChain::GetCurrentRTV() const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    handle.ptr += static_cast<SIZE_T>(m_currentBackBuffer) * m_rtvDescriptorSize;

    return handle;
}

unsigned int SwapChain::GetCurrentBackBufferIndex() const
{
    return m_currentBackBuffer;
}

void SwapChain::UpdateBackBufferIndex()
{
    m_currentBackBuffer = m_swapChain->GetCurrentBackBufferIndex();
}

bool SwapChain::Present()
{
    HRESULT result = m_swapChain->Present(0, 0);

    if(FAILED(result)){
        Log::Error("Failed to present swap chain. Back-buffer index: " + std::to_string(m_currentBackBuffer) + "; HRESULT: " + std::to_string(result));
#ifdef _DEBUG
        GraphicsDevice::ReportDeviceRemovedData(m_device, "SwapChain::Present");
#endif
        return false;
    }

    UpdateBackBufferIndex();

    return true;
}