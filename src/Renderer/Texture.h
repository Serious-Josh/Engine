#pragma once

#include <cstddef>
#include <climits>
#include <d3d12.h>
#include <vector>
#include <memory>

class DescriptorHeap;

class Texture
{
    public:
    // Opt-in allocation lease: owns the resource and its existing descriptors together.
    // The descriptor heap must outlive every lease. No resource or descriptor is duplicated.
    class RetainedAllocation
    {
    public:
        RetainedAllocation() = default;
        RetainedAllocation(const RetainedAllocation&) = delete;
        RetainedAllocation& operator=(const RetainedAllocation&) = delete;
        ~RetainedAllocation();
        unsigned int GetDescriptorIndex() const noexcept
        {
            return m_srvIndex;
        }

        bool UsesDescriptorHeap(const DescriptorHeap& heap) const noexcept
        {
            return m_heap == &heap;
        }

    private:
        friend class Texture;
        ID3D12Resource* m_resource = nullptr;

        DescriptorHeap* m_heap = nullptr;
        unsigned int m_srvIndex = UINT_MAX;
        unsigned int m_uavIndex = UINT_MAX;
    };
    using Dependency = std::shared_ptr<const RetainedAllocation>;
    Dependency Retain();
    Texture() = default;

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;

    bool Initialize(
        ID3D12Device* device,
        const void* data,
        unsigned int width,
        unsigned int height,
        DXGI_FORMAT format,
        size_t bytesPerPixel);
    bool Initialize(
        ID3D12Device* device,
        unsigned int width,
        unsigned int height,
        unsigned int arraySize,
        unsigned int mipLevels,
        DXGI_FORMAT format,
        D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES initialState);
    bool InitializeMipmapped(
        ID3D12Device* device,
        unsigned int width,
        unsigned int height,
        unsigned int mipLevels,
        DXGI_FORMAT format);
    void Shutdown();

    void Transition(
        ID3D12GraphicsCommandList* commandList,
        D3D12_RESOURCE_STATES newState);
    void Transition(
        ID3D12GraphicsCommandList* commandList,
        D3D12_RESOURCE_STATES newState,
        unsigned int subresource);
    void SetInitialData(
        const void* data,
        size_t dataSize);
    D3D12_RESOURCE_STATES GetSubresourceState(unsigned int subresource) const;

    std::vector<D3D12_RESOURCE_STATES> GetSubresourceStates() const
    {
        return m_subresourceStates;
    }

    // CPU tracking only; snapshot must belong to this unchanged resource.
    // Use only after discarding unsubmitted barriers.
    void RestoreStatesAfterAbort(const std::vector<D3D12_RESOURCE_STATES>& states)
    {
        m_subresourceStates = states;
    }

    ID3D12Resource* GetResource() const;
    const void* GetInitialData() const;
    size_t GetInitialDataSize() const;
    unsigned int GetDescriptorIndex() const;
    unsigned int GetUAVIndex() const;

    bool CreateSRV(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN);
    bool CreateCubeSRV(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap);
    bool CreateUAV(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap);

    static unsigned int CalculateMipLevels(
        unsigned int width,
        unsigned int height);

    private:
    std::shared_ptr<RetainedAllocation> m_retained;

    ID3D12Resource* m_resource = nullptr;

    unsigned int m_width = 0;
    unsigned int m_height = 0;

    unsigned int m_srvIndex = UINT_MAX;
    unsigned int m_uavIndex = UINT_MAX;
    DescriptorHeap* m_descriptorHeap = nullptr;

    std::vector<unsigned char> m_initialData;

    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;

    std::vector<D3D12_RESOURCE_STATES> m_subresourceStates;
};
