#include "Mesh.h"

bool Mesh::Initialize(ID3D12Device* device, const void* vertices, size_t vertexCount, size_t vertexStride)
{
    const size_t bufferSize = vertexCount * vertexStride;

    if(!m_vertexBuffer.Initialize(device, vertices, bufferSize)){
        return false;
    }

    m_vertexBufferView.BufferLocation =
        m_vertexBuffer.GetGPUAddress();

    m_vertexBufferView.StrideInBytes =
        static_cast<UINT>(vertexStride);

    m_vertexBufferView.SizeInBytes =
        static_cast<UINT>(bufferSize);

    return true;
}

void Mesh::Shutdown(){
    m_vertexBuffer.Shutdown();
}

void Mesh::Bind(ID3D12GraphicsCommandList* commandList) const
{
    commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
}