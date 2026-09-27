#include "GraphicsDevice.h"

#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>

bool GraphicsDevice::Initialize()
{
    #ifdef _DEBUG
    ID3D12Debug* debugController = nullptr;

    if(SUCCEEDED(D3D12GetDebugInterface(
        IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
        debugController->Release();
    }
#endif

    // Getting graphics adapter and creating D3D12 device
    HRESULT result = CreateDXGIFactory2(
        0,
        IID_PPV_ARGS(&m_factory)
    );

    if(FAILED(result))
    {
        return false;
    }

    IDXGIAdapter1* adapter = nullptr;

    for(UINT index = 0; m_factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index)
    {
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);

        if(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            adapter->Release();
            adapter = nullptr;
            continue;
        }

        result = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));

        if(SUCCEEDED(result))
        {
            break;
        }

        adapter->Release();
        adapter = nullptr;
    }

    if(adapter)
    {
        adapter->Release();
    }

    if(m_device == nullptr)
    {
        return false;
    }

    return true;
}

void GraphicsDevice::Shutdown()
{
    if(m_device){
        m_device->Release();
        m_device = nullptr;
    }

    if(m_factory){
        m_factory->Release();
        m_factory = nullptr;
    }
}

ID3D12Device* GraphicsDevice::GetDevice() const
{
    return m_device;
}

IDXGIFactory4* GraphicsDevice::GetFactory() const
{
    return m_factory;
}