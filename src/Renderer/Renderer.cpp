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

    return true;
}

void Renderer::Shutdown()
{
    if(m_commandQueue){
        m_commandQueue->Release();
        m_commandQueue = nullptr;
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