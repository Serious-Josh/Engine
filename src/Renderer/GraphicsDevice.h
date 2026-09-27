#pragma once

struct IDXGIFactory4;
struct ID3D12Device;

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

    private:
        IDXGIFactory4* m_factory = nullptr;
        ID3D12Device* m_device = nullptr;
};