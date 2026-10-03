#pragma once

#include <d3d12.h>

struct IDXGIFactory4;

class GraphicsDevice
{
    public:
        GraphicsDevice() = default;

        GraphicsDevice(const GraphicsDevice&) = delete;
        GraphicsDevice& operator=(const GraphicsDevice&) = delete;

        bool Initialize();
        void Shutdown();

        ID3D12Device* GetDevice() const;
        IDXGIFactory4* GetFactory() const;

        bool SupportsRayTracing() const;
        D3D12_RAYTRACING_TIER GetRayTracingTier() const;

        ID3D12Device5* GetRayTracingDevice() const;

    private:
        IDXGIFactory4* m_factory = nullptr;
        ID3D12Device* m_device = nullptr;
        ID3D12Device5* m_rayTracingDevice = nullptr;

        D3D12_RAYTRACING_TIER m_rayTracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
};