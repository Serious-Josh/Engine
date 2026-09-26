#pragma once

struct IDXGIFactory4;
struct ID3D12Device;
struct ID3D12CommandQueue;
struct IDXGISwapChain3;
struct ID3D12Resource;
struct ID3D12DescriptorHeap;
struct ID3D12CommandAllocator;
struct ID3D12GraphicsCommandList;

class Renderer
{
    public:
        bool Initialize(void* windowHandle, unsigned int width, unsigned int height);
        void Render();
        void Shutdown();
    private:
        IDXGIFactory4* m_factory = nullptr;
        ID3D12Device* m_device = nullptr;
        ID3D12CommandQueue* m_commandQueue = nullptr;
        IDXGISwapChain3* m_swapChain = nullptr;

        ID3D12Resource* m_backBuffers[2]{};
        ID3D12DescriptorHeap* m_renderTargetHeap = nullptr;

        unsigned int m_rtvDescriptorSize = 0;

        ID3D12CommandAllocator* m_commandAllocator = nullptr;
        ID3D12GraphicsCommandList* m_commandList = nullptr;

        unsigned int m_currentBackBuffer = 0;
};
