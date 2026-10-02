#pragma once

#include "Renderer/Buffer.h"
#include "Renderer/VertexLayout.h"

#include <d3d12.h>
#include <cstddef>

class Mesh
{
    public:
        Mesh() = default;

        Mesh(const Mesh&) = delete;
        Mesh& operator=(const Mesh&) = delete;

        bool Initialize(ID3D12Device* device, ResourceUploader& uploader, const void* vertices, size_t vertexCount, const VertexLayout& layout);
        bool Initialize(ID3D12Device* device, ResourceUploader& uploader, const void* vertices, size_t vertexCount, const VertexLayout& layout, const unsigned int* indices, size_t indexCount);
        void Shutdown();
        void Bind(ID3D12GraphicsCommandList* commandList) const;

        const VertexLayout& GetLayout() const;
        size_t GetVertexCount() const;
        size_t GetIndexCount() const;
        bool IsIndexed() const;

    private:
        Buffer m_vertexBuffer;
        Buffer m_indexBuffer;
        VertexLayout m_layout;

        size_t m_vertexCount = 0;
        size_t m_indexCount = 0;

        D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW m_indexBufferView{};
};