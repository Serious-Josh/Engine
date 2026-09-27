#pragma once

#include <d3d12.h>

class CommandContext
{
    public:
        CommandContext() = default;

        CommandContext(const CommandContext&) = delete;
        CommandContext& operator=(const CommandContext&) = delete;
        
        bool Initialize(ID3D12Device* device);
        void Shutdown();

        bool Begin();
        bool End();

        void Execute();
        void WaitForGPU();

        ID3D12GraphicsCommandList* GetCommandList() const;
        ID3D12CommandQueue* GetCommandQueue() const;

    private:
        ID3D12CommandQueue* m_commandQueue = nullptr;
        ID3D12CommandAllocator* m_commandAllocator = nullptr;
        ID3D12GraphicsCommandList* m_commandList = nullptr;

        ID3D12Fence* m_fence = nullptr;
        unsigned long long m_fenceValue = 0;

        void* m_fenceEvent = nullptr;
};