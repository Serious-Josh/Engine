#pragma once

#include "Renderer/Buffer.h"

#include <d3d12.h>

class Mesh
{
    public:
        bool Initialize(ID3D12Device* device, const void* vertices, size_t vertexCount, size_t vertexStride);
        void Shutdown();
        void Bind(ID3D12GraphicsCommandList* commandList) const;

    private:
        Buffer m_vertexBuffer;
        D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView{};
};