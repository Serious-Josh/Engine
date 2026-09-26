#pragma once

struct IDXGIFactory4;
struct ID3D12Device;
struct ID3D12CommandQueue;
struct IDXGISwapChain3;

class Renderer
{
    public:
        bool Initialize(void* windowHandle, unsigned int width, unsigned int height);
        void Shutdown();
    private:
        IDXGIFactory4* m_factory = nullptr;
        ID3D12Device* m_device = nullptr;
        ID3D12CommandQueue* m_commandQueue = nullptr;
        IDXGISwapChain3* m_swapChain = nullptr;
};
