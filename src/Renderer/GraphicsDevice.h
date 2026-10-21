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

#ifdef _DEBUG
    static void ReportDeviceRemovedData(
        ID3D12Device* device,
        const char* operationContext);
#endif

    ID3D12Device* GetDevice() const;
    IDXGIFactory4* GetFactory() const;

    bool SupportsRayTracing() const;

    ID3D12Device5* GetRayTracingDevice() const;

    // Reported at initialization; the path tracer's library selection reads them (SER S6B).
    D3D12_RAYTRACING_TIER GetRayTracingTier() const;
    D3D_SHADER_MODEL GetHighestShaderModel() const;
    bool ShaderExecutionReorderingActuallyReorders() const;

    private:
    void QueryShaderCapabilities();

    IDXGIFactory4* m_factory = nullptr;

    ID3D12Device* m_device = nullptr;

    ID3D12Device5* m_rayTracingDevice = nullptr;

    D3D12_RAYTRACING_TIER m_rayTracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;

    // 0 when the query failed.
    D3D_SHADER_MODEL m_highestShaderModel = static_cast<D3D_SHADER_MODEL>(0);
    bool m_serActuallyReorders = false;
};
