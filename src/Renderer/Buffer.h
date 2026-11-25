#pragma once

#include <climits>
#include <cstdint>
#include <d3d12.h>

#include "ResourceStateTransaction.h"

class DescriptorHeap;
class ResourceUploader;

class Buffer
{
    public:
    Buffer() = default;

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    bool Initialize(
        ID3D12Device* device,
        ResourceUploader& uploader,
        const void* data,
        size_t size,
        D3D12_RESOURCE_STATES finalState);

    bool InitializeGPUWritable(
        ID3D12Device* device,
        size_t capacityBytes);

    // Allocates once in COMMON and synchronously uploads the active prefix.
    // finalState applies within the upload submission; completed buffer state
    // is COMMON after decay, permitting subsequent read-state promotion.
    bool InitializeGPUWritableSeeded(
        ID3D12Device* device,
        ResourceUploader& uploader,
        const void* data,
        size_t size,
        size_t capacityBytes,
        D3D12_RESOURCE_STATES finalState);

    bool ReplaceData(
        ID3D12Device* device,
        ResourceUploader& uploader,
        const void* data,
        size_t size,
        D3D12_RESOURCE_STATES finalState);

    bool PrepareReplacement(
        ID3D12Device* device,
        ResourceUploader& uploader,
        const void* data,
        size_t size,
        D3D12_RESOURCE_STATES finalState,
        Buffer& destination) const;

    bool CommitReplacement(
        ID3D12Device* device,
        Buffer& prepared);

    bool CreateRawSRV(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap);
    // A structured SRV of `stride`-byte elements in place of the raw one (the whole allocation; the width must be a
    // multiple of the stride).
    bool CreateStructuredSRV(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        std::uint32_t stride);
    bool CreateRawUAV(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap);
    // A structured UAV of `stride`-byte elements in place of the raw one (NVIDIA SHARC's RWStructuredBuffer bindings).
    bool CreateStructuredUAV(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        std::uint32_t stride);
    void Shutdown();

    D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const;
    ID3D12Resource* GetResource() const;

    D3D12_RESOURCE_STATES GetResourceState() const;

    // CPU tracking only. Use only after discarding unsubmitted barriers. (R.1: discarded recordings also restore tracked
    // states automatically; this explicit restore is authoritative and ends the buffer's transaction.)
    void RestoreStateAfterAbort(D3D12_RESOURCE_STATES state)
    {
        m_resourceState.Reset(state);
    }

    void Transition(
        ID3D12GraphicsCommandList* commandList,
        D3D12_RESOURCE_STATES newState);

    // The transition to `newState` as a barrier for the caller to record in `commandList` (before any use in the new state),
    // tracked as done; false, with `barrier` untouched, when the buffer is already there.
    bool PrepareTransition(
        ID3D12GraphicsCommandList* commandList,
        D3D12_RESOURCE_STATES newState,
        D3D12_RESOURCE_BARRIER& barrier);

    unsigned int GetDescriptorIndex() const;
    unsigned int GetUAVDescriptorIndex() const;

    private:
    // placementAllowed: the buffer may be placed in Engine-owned memory (TextureMemoryAllocator.h); false keeps it
    // committed (seeded buffers, whose unwritten remainder owners may expect zeroed).
    bool AllocateGPUWritable(
      ID3D12Device* device,
      size_t capacityBytes,
      D3D12_RESOURCE_STATES initialState,
      bool placementAllowed = true);
    ID3D12Resource* m_resource = nullptr;

    DescriptorHeap* m_descriptorHeap = nullptr;
    unsigned int m_descriptorIndex = UINT_MAX;
    unsigned int m_uavDescriptorIndex = UINT_MAX;

    TransactionalResourceState<D3D12_RESOURCE_STATES> m_resourceState{D3D12_RESOURCE_STATE_COMMON};
};
