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

        bool Begin(unsigned int frameIndex);
        bool End();

        bool Execute(unsigned int frameIndex);
        void WaitForGPU();

        ID3D12GraphicsCommandList* GetCommandList() const;
        ID3D12CommandQueue* GetCommandQueue() const;
        ID3D12GraphicsCommandList4* GetRayTracingCommandList() const;

    private:
        static constexpr unsigned int FrameCount = 2;

        ID3D12CommandQueue* m_commandQueue = nullptr;
        ID3D12CommandAllocator* m_commandAllocators[FrameCount]{};
        ID3D12GraphicsCommandList* m_commandList = nullptr;
        ID3D12GraphicsCommandList4* m_rayTracingCommandList = nullptr;

        ID3D12Fence* m_fence = nullptr;
        unsigned long long m_fenceValue = 0;
        unsigned long long m_frameFenceValues[FrameCount]{};

        void* m_fenceEvent = nullptr;
};