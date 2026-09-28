#pragma once

#include <d3d12.h>
#include <climits>

class Camera;
class DescriptorHeap;

class DepthBuffer
{
    public:
        DepthBuffer() = default;

        DepthBuffer(const DepthBuffer&) = delete;
        DepthBuffer& operator=(const DepthBuffer&) = delete;

        bool Initialize(ID3D12Device* device, unsigned int width, unsigned int height, DescriptorHeap& dsvHeap, DescriptorHeap& resourceHeap);

        void Shutdown();

        void Transition(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES newState);

        ID3D12Resource* GetResource() const;

        D3D12_CPU_DESCRIPTOR_HANDLE GetDSV() const;

        unsigned int GetSRVIndex() const;

        unsigned int GetWidth() const;
        unsigned int GetHeight() const;

    private:
        ID3D12Resource* m_resource = nullptr;

        DescriptorHeap* m_dsvHeap = nullptr;
        DescriptorHeap* m_resourceHeap = nullptr;

        unsigned int m_dsvIndex = UINT_MAX;
        unsigned int m_srvIndex = UINT_MAX;

        unsigned int m_width = 0;
        unsigned int m_height = 0;

        D3D12_RESOURCE_STATES m_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
};