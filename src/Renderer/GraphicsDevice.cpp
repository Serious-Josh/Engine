#include "GraphicsDevice.h"
#include "Core/Log.h"

#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <format>
#include <string>

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

        result = D3D12CreateDevice(
            adapter,
            D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(&m_device)
        );

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

    // Query DirectX Raytracing support
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};

    result = m_device->CheckFeatureSupport(
        D3D12_FEATURE_D3D12_OPTIONS5,
        &options5,
        sizeof(options5)
    );

    if(FAILED(result))
    {
        Log::Error(std::format(
            "D3D12_OPTIONS5 feature query failed. HRESULT: 0x{:x}",
            static_cast<unsigned long>(result)));
    }
    else
    {
        m_rayTracingTier = options5.RaytracingTier;

        if(SupportsRayTracing()){
            result = m_device->QueryInterface(IID_PPV_ARGS(&m_rayTracingDevice));

            if(FAILED(result))
            {
                Log::Error(std::format(
                    "Failed to acquire ID3D12Device5. HRESULT: 0x{:x}",
                    static_cast<unsigned long>(result)));

                return false;
            }
        }
    }

    switch(m_rayTracingTier){
        case D3D12_RAYTRACING_TIER_NOT_SUPPORTED:
            Log::Info("DXR ray tracing tier: Not supported");
            break;

        case D3D12_RAYTRACING_TIER_1_0:
            Log::Info("DXR ray tracing tier: 1.0");
            break;

        case D3D12_RAYTRACING_TIER_1_1:
            Log::Info("DXR ray tracing tier: 1.1");
            break;

        case D3D12_RAYTRACING_TIER_1_2:
            Log::Info("DXR ray tracing tier: 1.2");
            break;

        default:
            Log::Info(
                "DXR ray tracing tier: Unknown (" +
                std::to_string(static_cast<unsigned int>(m_rayTracingTier)) +
                ")");
            break;
    }

    return true;
}

void GraphicsDevice::Shutdown()
{
    if(m_rayTracingDevice){
        m_rayTracingDevice->Release();
        m_rayTracingDevice = nullptr;
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

ID3D12Device* GraphicsDevice::GetDevice() const
{
    return m_device;
}

IDXGIFactory4* GraphicsDevice::GetFactory() const
{
    return m_factory;
}

bool GraphicsDevice::SupportsRayTracing() const
{
    return
        m_rayTracingTier !=
        D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
}

D3D12_RAYTRACING_TIER GraphicsDevice::GetRayTracingTier() const
{
    return m_rayTracingTier;
}

ID3D12Device5* GraphicsDevice::GetRayTracingDevice() const
{
    return m_rayTracingDevice;
}
