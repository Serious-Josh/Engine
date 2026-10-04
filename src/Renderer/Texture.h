#pragma once

#include <cstddef>
#include <climits>
#include <d3d12.h>
#include <vector>

class DescriptorHeap;

class Texture
{
    public:
        Texture() = default;

        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;

        Texture(Texture&& other) noexcept;
        Texture& operator=(Texture&& other) noexcept;

        bool Initialize(ID3D12Device* device, const void* data, unsigned int width, unsigned int height, DXGI_FORMAT format, size_t bytesPerPixel);
        bool Initialize(ID3D12Device* device, unsigned int width, unsigned int height, unsigned int arraySize, unsigned int mipLevels, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initialState);
        void Shutdown();

        void Transition(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES newState);
        void Transition(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES newState, unsigned int subresource);
        void SetInitialData(const void* data, size_t dataSize);

        ID3D12Resource* GetResource() const;
        const void* GetInitialData() const;
        size_t GetInitialDataSize() const;
        unsigned int GetDescriptorIndex() const;
        unsigned int GetUAVIndex() const;

        bool CreateSRV(ID3D12Device* device, DescriptorHeap& descriptorHeap, DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN);
        bool CreateCubeSRV(ID3D12Device* device, DescriptorHeap& descriptorHeap);
        bool CreateUAV(ID3D12Device* device, DescriptorHeap& descriptorHeap);
    
    private:
        ID3D12Resource* m_resource = nullptr;

        unsigned int m_width = 0;
        unsigned int m_height = 0;

        unsigned int m_srvIndex = UINT_MAX;
        unsigned int m_uavIndex = UINT_MAX;
        DescriptorHeap* m_descriptorHeap = nullptr;

        std::vector<unsigned char> m_initialData;

        DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
        size_t m_bytesPerPixel = 0;

        std::vector<D3D12_RESOURCE_STATES> m_subresourceStates;
};