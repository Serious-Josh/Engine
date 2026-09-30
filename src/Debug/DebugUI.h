#pragma once

#include <climits>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12CommandQueue;
struct HWND__;

using HWND = HWND__*;

class DescriptorHeap;

class DebugUI
{
    public:
        DebugUI() = default;

        DebugUI(const DebugUI&) = delete;
        DebugUI& operator=(const DebugUI&) = delete;

        bool Initialize(HWND windowsHandle, ID3D12Device* device, ID3D12CommandQueue* commandQueue, DescriptorHeap& descriptorHeap);
        void Shutdown();

        void BeginFrame();
        void EndFrame(ID3D12GraphicsCommandList* commandList);

    private:
        DescriptorHeap* m_descriptorHeap = nullptr;
};