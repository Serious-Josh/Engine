#include "Mesh.h"
#include "Renderer/DescriptorHeap.h"

bool Mesh::Initialize(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& descriptorHeap, const void* vertices, size_t vertexCount, const VertexLayout& layout)
{
    m_layout = layout;
    m_vertexCount = vertexCount;

    const D3D12_RESOURCE_STATES vertexState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    if(!m_vertexBuffer.Initialize(device, uploader, vertices, vertexCount * layout.GetStride(), vertexState)){
        return false;
    }

    if(!m_vertexBuffer.CreateRawSRV(device, descriptorHeap)){
        Shutdown();
        return false;
    }

    m_vertexBufferView.BufferLocation = m_vertexBuffer.GetGPUAddress();

    m_vertexBufferView.StrideInBytes = static_cast<UINT>(layout.GetStride());

    m_vertexBufferView.SizeInBytes = static_cast<UINT>(vertexCount * layout.GetStride());

    return true;
}

bool Mesh::Initialize(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& descriptorHeap, const void* vertices, size_t vertexCount, const VertexLayout& layout, const unsigned int* indices, size_t indexCount)
{
    if(!indices || indexCount == 0){
        return false;
    }

    if(!Initialize(device, uploader, descriptorHeap, vertices, vertexCount, layout)){
        return false;
    }

    const D3D12_RESOURCE_STATES indexState = D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    if(!m_indexBuffer.Initialize(device, uploader, indices, indexCount * sizeof(unsigned int), indexState)){
        Shutdown();
        return false;
    }

    if(!m_indexBuffer.CreateRawSRV(device, descriptorHeap)){
        Shutdown();
        return false;
    }

    m_indexCount = indexCount;

    m_indexBufferView.BufferLocation = m_indexBuffer.GetGPUAddress();
    m_indexBufferView.SizeInBytes = static_cast<UINT>(indexCount * sizeof(unsigned int));
    m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;

    return true;
}

void Mesh::Shutdown(){
    m_vertexBuffer.Shutdown();
    m_indexBuffer.Shutdown();
    m_vertexCount = 0;
    m_indexCount = 0;
}

void Mesh::Bind(ID3D12GraphicsCommandList* commandList) const
{
    commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);

    if(IsIndexed()){
        commandList->IASetIndexBuffer(&m_indexBufferView);
    }
}

const VertexLayout& Mesh::GetLayout() const
{
    return m_layout;
}

size_t Mesh::GetVertexCount() const
{
    return m_vertexCount;
}

size_t Mesh::GetIndexCount() const
{
    return m_indexCount;
}

bool Mesh::IsIndexed() const
{
    return m_indexCount > 0;
}

D3D12_GPU_VIRTUAL_ADDRESS Mesh::GetVertexBufferAddress() const
{
    return m_vertexBuffer.GetGPUAddress();
}

D3D12_GPU_VIRTUAL_ADDRESS Mesh::GetIndexBufferAddress() const
{
    return m_indexBuffer.GetGPUAddress();
}

UINT Mesh::GetVertexStride() const
{
    return m_vertexBufferView.StrideInBytes;
}

DXGI_FORMAT Mesh::GetIndexFormat() const
{
    return m_indexBufferView.Format;
}

unsigned int Mesh::GetVertexBufferDescriptorIndex() const
{
    return m_vertexBuffer.GetDescriptorIndex();
}

unsigned int Mesh::GetIndexBufferDescriptorIndex() const
{
    return m_indexBuffer.GetDescriptorIndex();
}

const VertexLayout& Mesh::GetVertexLayout() const
{
    return m_layout;
}