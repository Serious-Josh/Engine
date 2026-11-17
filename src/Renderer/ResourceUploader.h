#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstddef>
#include <vector>

struct TextureSubresourceData
{
    const void* data = nullptr;
    size_t rowPitch = 0;
    size_t slicePitch = 0;
};

// S0.8B: buffer uploads collected instead of submitted. Each holds its filled UPLOAD-heap staging buffer; the Renderer
// records the copies into a frame-queue recording (Renderer::QueueGeometryUploads). The destination is borrowed: its
// owner keeps it alive until the copies are retired (see Renderer.h).
struct DeferredBufferUpload
{
    ID3D12Resource* destination = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> staging;
    size_t size = 0;
    D3D12_RESOURCE_STATES finalState = D3D12_RESOURCE_STATE_COMMON;
};

struct DeferredBufferUploadBatch
{
    std::vector<DeferredBufferUpload> uploads;
    size_t bytes = 0;
};

class ResourceUploader
{
    public:
    ResourceUploader() = default;
    ~ResourceUploader();

    ResourceUploader(const ResourceUploader&) = delete;
    ResourceUploader& operator=(const ResourceUploader&) = delete;

    bool Initialize(ID3D12Device* device);
    void Shutdown();

    // Unconfirmed post-submission completion aborts before staging or destination teardown.
    // Destination must start in COMMON (promoted by the copy) or COPY_DEST.
    // finalState applies within the submission. Buffers decay to COMMON after
    // ExecuteCommandLists completes; success confirms completion synchronously.
    bool UploadBuffer(
        ID3D12Resource* destination,
        const void* data,
        size_t dataSize,
        D3D12_RESOURCE_STATES finalState);
    bool UploadTexture(
        ID3D12Resource* destination,
        const void* data,
        size_t dataSize);
    bool UploadTexture(
        ID3D12Resource* destination,
        const TextureSubresourceData* subresources,
        unsigned int subresourceCount);

    // S0.8B: while a batch is set, UploadBuffer stages its data and appends it to the batch instead of submitting and
    // waiting (texture uploads are unaffected). The destination's tracked final state is the same as after a
    // synchronous upload; nothing reads it before the Renderer has submitted the copies.
    void SetDeferredBufferBatch(DeferredBufferUploadBatch* batch) noexcept { m_deferredBatch = batch; }

    private:
    DeferredBufferUploadBatch* m_deferredBatch = nullptr;
    bool FinishUpload(ID3D12Resource* uploadBuffer);

    ID3D12Device* m_device = nullptr;

    ID3D12CommandQueue* m_commandQueue = nullptr;

    ID3D12CommandAllocator* m_commandAllocator = nullptr;

    ID3D12GraphicsCommandList* m_commandList = nullptr;

    ID3D12Fence* m_fence = nullptr;
    unsigned long long m_fenceValue = 0;
    void* m_fenceEvent = nullptr;
};
