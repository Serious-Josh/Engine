#pragma once

#include "Renderer/Buffer.h"
#include "Renderer/VertexLayout.h"

#include "Math/Vector3.h"
#include "assets/MeshMetadata.h"
#include <optional>

#include <d3d12.h>
#include <cstddef>

class DescriptorHeap;

struct MeshBounds
{
    Vector3 min{};
    Vector3 max{};

    bool valid = false;
};

class Mesh
{
    public:
    Mesh() = default;

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    bool Initialize(
        ID3D12Device* device,
        ResourceUploader& uploader,
        DescriptorHeap& descriptorHeap,
        const void* vertices,
        size_t vertexCount,
        const VertexLayout& layout);
    bool Initialize(
        ID3D12Device* device,
        ResourceUploader& uploader,
        DescriptorHeap& descriptorHeap,
        const void* vertices,
        size_t vertexCount,
        const VertexLayout& layout,
        const unsigned int* indices,
        size_t indexCount);

    bool InitializeGPUWritable(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        const VertexLayout& layout,
        size_t vertexCapacity,
        size_t indexCapacity,
        const MeshBounds& bounds);

    bool SetGPUGeneratedGeometryCounts(
        size_t vertexCount,
        size_t indexCount);

    // Empty mesh only. Bounds must enclose the seed and all future GPU output.
    // Seed vertices must match the displacement Vertex ABI (48-byte stride).
    bool InitializeGPUWritableSeeded(
        ID3D12Device* device,
        ResourceUploader& uploader,
        DescriptorHeap& descriptorHeap,
        const VertexLayout& layout,
        const void* vertices,
        size_t vertexCount,
        const unsigned int* indices,
        size_t indexCount,
        size_t vertexCapacity,
        size_t indexCapacity,
        const MeshBounds& bounds);

    bool ReplaceGeometry(
        ID3D12Device* device,
        ResourceUploader& uploader,
        DescriptorHeap& descriptorHeap,
        const void* vertices,
        size_t vertexCount,
        const unsigned int* indices,
        size_t indexCount);

    void Shutdown();
    void Bind(ID3D12GraphicsCommandList* commandList) const;

    size_t GetVertexCount() const;
    size_t GetIndexCount() const;
    bool IsIndexed() const;

    D3D12_GPU_VIRTUAL_ADDRESS GetVertexBufferAddress() const;
    D3D12_GPU_VIRTUAL_ADDRESS GetIndexBufferAddress() const;

    UINT GetVertexStride() const;
    DXGI_FORMAT GetIndexFormat() const;
    const VertexLayout& GetVertexLayout() const;

    unsigned int GetVertexBufferDescriptorIndex() const;
    unsigned int GetIndexBufferDescriptorIndex() const;

    unsigned int GetVertexBufferUAVDescriptorIndex() const;
    unsigned int GetIndexBufferUAVDescriptorIndex() const;

    ID3D12Resource* GetVertexBufferResource() const;
    ID3D12Resource* GetIndexBufferResource() const;
    D3D12_RESOURCE_STATES GetVertexBufferState() const;
    D3D12_RESOURCE_STATES GetIndexBufferState() const;

    // Only valid when the command list containing the transitions was discarded.
    void RestoreBufferStatesAfterAbort(
        D3D12_RESOURCE_STATES vertexState,
        D3D12_RESOURCE_STATES indexState)
    {
        m_vertexBuffer.RestoreStateAfterAbort(vertexState);
        m_indexBuffer.RestoreStateAfterAbort(indexState);
    }

    void TransitionVertexBuffer(
        ID3D12GraphicsCommandList* commandList,
        D3D12_RESOURCE_STATES newState);
    void TransitionIndexBuffer(
        ID3D12GraphicsCommandList* commandList,
        D3D12_RESOURCE_STATES newState);

    size_t GetVertexCapacity() const;
    size_t GetIndexCapacity() const;

    const MeshBounds& GetBounds() const;

    // Metadata is shared with this Mesh, never owned by individual instances.
    // Geometry changes restore the single full-range procedural default.
    bool SetMetadata(MeshMetadata metadata, std::string& error);
    const MeshMetadata& GetMetadata() const noexcept;

    private:
    friend class MarqueeSelectionVerification;
    void ResetMetadata() noexcept;
    MeshMetadata m_defaultMetadata = DefaultMeshMetadata(0);
    std::optional<MeshMetadata> m_metadata;

    Buffer m_vertexBuffer;
    Buffer m_indexBuffer;

    VertexLayout m_layout;

    size_t m_vertexCount = 0;
    size_t m_indexCount = 0;
    size_t m_vertexCapacity = 0;
    size_t m_indexCapacity = 0;

    bool m_gpuWritable = false;

    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView{};

    D3D12_INDEX_BUFFER_VIEW m_indexBufferView{};

    MeshBounds m_bounds;
};
