#pragma once

#include "Renderer/Buffer.h"
#include "Renderer/DisplacementGPUIndexLayout.h"

#include <climits>

class DescriptorHeap;
class ResourceUploader;
class UploadBuffer;

class DisplacementGPUPatchBuffer
{
    public:
    bool Initialize(
      ID3D12Device* device,
      ResourceUploader& uploader,
      DescriptorHeap& descriptorHeap,
      const DisplacementGPUIndexLayout& layout);

    bool PreparePersistentStorage(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        size_t patchCapacity,
        bool gpuIdleConfirmed);

    bool HasReadyPersistentCapacity(size_t requiredPatchCount) const;

    bool RecordUpdate(
        ID3D12GraphicsCommandList* commandList,
        UploadBuffer& uploadBuffer,
        const DisplacementGPUIndexLayout& layout);

    D3D12_RESOURCE_STATES GetStateSnapshot() const
    {
        return m_recordedState;
    }

    void RestoreStateAfterAbort(D3D12_RESOURCE_STATES snapshot);

    bool ConfirmUpdateCompletion(bool gpuCompletionConfirmed);

    void Shutdown();

    unsigned int GetDescriptorIndex() const
    {
        return m_buffer.GetDescriptorIndex();
    }

    ID3D12Resource* GetResource() const
    {
        return m_buffer.GetResource();
    }

    private:
    Buffer m_buffer;

    DescriptorHeap* m_persistentHeap = nullptr;

    D3D12_RESOURCE_STATES m_recordedState = D3D12_RESOURCE_STATE_COMMON;

    bool m_updateUnresolved = false;
};
