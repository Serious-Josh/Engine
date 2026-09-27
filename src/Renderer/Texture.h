#pragma once

#include <cstddef>
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

        bool Initialize(ID3D12Device* device, const void* data, unsigned int width, unsigned int height);
        void Shutdown();

        ID3D12Resource* GetResource() const;
        const void* GetInitialData() const;
        size_t GetInitialDataSize() const;
        unsigned int GetDescriptorIndex() const;

        bool CreateSRV(ID3D12Device* device, DescriptorHeap& descriptorHeap, unsigned int descriptorIndex);
    
    private:
        ID3D12Resource* m_resource = nullptr;

        unsigned int m_width;
        unsigned int m_height;

        unsigned int m_descriptorIndex = 0;

        std::vector<unsigned char> m_initialData;
};