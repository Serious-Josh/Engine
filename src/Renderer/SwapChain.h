#pragma once

#include <d3d12.h>

struct IDXGIFactory4;
struct IDXGISwapChain3;
struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12DescriptorHeap;

class SwapChain
{
    public:
    // FG.2: two buffers when the frame queue presents every frame (as before); three for the Frame Generation present path,
    // which presents twice per frame from its own queue.
    static constexpr unsigned int MaximumBufferCount = 3;

    SwapChain() = default;

    SwapChain(const SwapChain&) = delete;
    SwapChain& operator=(const SwapChain&) = delete;

    bool Initialize(
        IDXGIFactory4* factory,
        ID3D12Device* device,
        ID3D12CommandQueue* commandQueue,
        void* windowHandle,
        unsigned int width,
        unsigned int height,
        unsigned int bufferCount = 2);
    bool Resize(
        unsigned int width,
        unsigned int height);

    void Shutdown();

    ID3D12Resource* GetCurrentBackBuffer() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRTV() const;

    unsigned int GetCurrentBackBufferIndex() const;

    void UpdateBackBufferIndex();

    bool Present();

    private:
    bool CreateBackBufferViews();

    ID3D12Device* m_device = nullptr;

    IDXGISwapChain3* m_swapChain = nullptr;

    ID3D12Resource* m_backBuffers[MaximumBufferCount]{};
    unsigned int m_bufferCount = 0;
    ID3D12DescriptorHeap* m_rtvHeap = nullptr;

    unsigned int m_rtvDescriptorSize = 0;
    unsigned int m_currentBackBuffer = 0;

    unsigned int m_width = 0;
    unsigned int m_height = 0;
};
