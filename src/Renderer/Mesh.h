#pragma once

#include "Renderer/Buffer.h"
#include "Renderer/VertexLayout.h"

#include <d3d12.h>

class Mesh
{
    public:
        Mesh() = default;

        Mesh(const Mesh&) = delete;
        Mesh& operator=(const Mesh&) = delete;

        bool Initialize(ID3D12Device* device, const void* vertices, size_t vertexCount, const VertexLayout& layout);
        void Shutdown();
        void Bind(ID3D12GraphicsCommandList* commandList) const;

        const VertexLayout& GetLayout() const;

    private:
        Buffer m_vertexBuffer;
        VertexLayout m_layout;

        D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView{};
};