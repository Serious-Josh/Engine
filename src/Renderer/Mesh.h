#pragma once

#include "Renderer/Buffer.h"
#include "Renderer/VertexLayout.h"

#include "Math/Vector3.h"

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

        bool Initialize(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& descriptorHeap, const void* vertices, size_t vertexCount, const VertexLayout& layout);
        bool Initialize(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& descriptorHeap, const void* vertices, size_t vertexCount, const VertexLayout& layout, const unsigned int* indices, size_t indexCount);
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

        const MeshBounds& GetBounds() const;

    private:
        Buffer m_vertexBuffer;
        Buffer m_indexBuffer;
        VertexLayout m_layout;

        size_t m_vertexCount = 0;
        size_t m_indexCount = 0;

        D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW m_indexBufferView{};

        MeshBounds m_bounds;
};