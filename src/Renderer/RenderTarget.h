#pragma once

#include <d3d12.h>
#include <dxgiformat.h>
#include <climits>

class DescriptorHeap;

class RenderTarget
{
    public:
        RenderTarget() = default;

        RenderTarget(const RenderTarget&) = delete;
        RenderTarget& operator=(const RenderTarget&) = delete;

        RenderTarget(RenderTarget&& other) noexcept;
        RenderTarget& operator=(RenderTarget&& other) noexcept;

        bool Initialize(ID3D12Device* device, unsigned int width, unsigned int height, DXGI_FORMAT format, DescriptorHeap& rtvHeap, DescriptorHeap& resourceHeap);

        void Shutdown();

        void Transition(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES newState);

        ID3D12Resource* GetResource() const;

        D3D12_CPU_DESCRIPTOR_HANDLE GetRTV() const;

        unsigned int GetSRVIndex() const;

        unsigned int GetWidth() const;
        unsigned int GetHeight() const;
        DXGI_FORMAT GetFormat() const;

    private:
        ID3D12Resource* m_resource = nullptr;

        DescriptorHeap* m_rtvHeap = nullptr;
        DescriptorHeap* m_resourceHeap = nullptr;

        unsigned int m_rtvIndex = UINT_MAX;
        unsigned int m_srvIndex = UINT_MAX;

        unsigned int m_width = 0;
        unsigned int m_height = 0;

        DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;

        D3D12_RESOURCE_STATES m_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
};