#include "Mesh.h"

bool Mesh::Initialize(ID3D12Device* device, const void* vertices, size_t vertexCount, const VertexLayout& layout)
{
    m_layout = layout;

    if(!m_vertexBuffer.Initialize(device, vertices, vertexCount * layout.GetStride())){
        return false;
    }

    m_vertexBufferView.BufferLocation = m_vertexBuffer.GetGPUAddress();

    m_vertexBufferView.StrideInBytes = static_cast<UINT>(vertexCount * layout.GetStride());

    m_vertexBufferView.SizeInBytes = static_cast<UINT>(layout.GetStride());

    return true;
}

void Mesh::Shutdown(){
    m_vertexBuffer.Shutdown();
}

void Mesh::Bind(ID3D12GraphicsCommandList* commandList) const
{
    commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
}

const VertexLayout& Mesh::GetLayout() const
{
    return m_layout;
}