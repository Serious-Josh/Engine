#include "Mesh.h"

#include "Renderer/DescriptorHeap.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <cstdint>
#include <climits>

namespace{
    MeshBounds CalculateBounds(const void* vertices, size_t vertexCount, const VertexLayout& layout)
    {
        MeshBounds bounds{};

        if(!vertices || vertexCount == 0){
            return bounds;
        }

        const size_t stride = layout.GetStride();

        if(stride == 0){
            return bounds;
        }

        const VertexAttribute* positionAttritube = nullptr;

        for(const VertexAttribute& attribute : layout.GetAttributes()){
            if(std::string_view(attribute.semantic ? attribute.semantic : "") == "POSITION" && attribute.semanticIndex == 0 && attribute.type == VertexAttributeType::Float3){
                positionAttritube = &attribute;
                break;
            }
        }

        if(!positionAttritube){
            return bounds;
        }

        const size_t offset = positionAttritube->offset;

        if(offset > stride || sizeof(float) * 3 > stride - offset){
            return bounds;
        }

        const auto* vertexBytes = static_cast<const unsigned char*>(vertices);

        const float largest = (std::numeric_limits<float>::max)();

        bounds.min = Vector3{largest, largest, largest};
        bounds.max = Vector3{-largest, -largest, -largest};

        for(size_t i = 0; i < vertexCount; ++i){
            float position[3];

            std::memcpy(position, vertexBytes + i * stride + offset, sizeof(position));

            if(!std::isfinite(position[0]) ||
               !std::isfinite(position[1]) ||
               !std::isfinite(position[2]))
            {
                return MeshBounds{};
            }

            bounds.min.x = (std::min)(bounds.min.x, position[0]);
            bounds.min.y = (std::min)(bounds.min.y, position[1]);
            bounds.min.z = (std::min)(bounds.min.z, position[2]);

            bounds.max.x = (std::max)(bounds.max.x, position[0]);
            bounds.max.y = (std::max)(bounds.max.y, position[1]);
            bounds.max.z = (std::max)(bounds.max.z, position[2]);
        }

        bounds.valid = true;
        return bounds;
    }
}

bool Mesh::Initialize(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& descriptorHeap, const void* vertices, size_t vertexCount, const VertexLayout& layout)
{
    m_layout = layout;
    m_vertexCount = vertexCount;

    m_bounds = CalculateBounds(vertices, vertexCount, layout);

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

bool Mesh::ReplaceGeometry(
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const void* vertices,
    size_t vertexCount,
    const unsigned int* indices,
    size_t indexCount)
{
    (void)descriptorHeap;

    const size_t stride = m_layout.GetStride();

    if(!device || !vertices || !indices ||
       vertexCount == 0 || indexCount == 0 ||
       !IsIndexed() ||
       stride == 0 ||
       stride > UINT_MAX ||
       vertexCount > UINT_MAX / stride ||
       indexCount > UINT_MAX / sizeof(unsigned int) ||
       m_vertexBuffer.GetDescriptorIndex() == UINT_MAX ||
       m_indexBuffer.GetDescriptorIndex() == UINT_MAX)
    {
        return false;
    }

    const MeshBounds newBounds = CalculateBounds(vertices, vertexCount, m_layout);

    if(!newBounds.valid)
    {
        return false;
    }

    const size_t vertexBytes = vertexCount * stride;
    const size_t indexBytes = indexCount * sizeof(unsigned int);

    const D3D12_RESOURCE_STATES vertexState =
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    const D3D12_RESOURCE_STATES indexState =
        D3D12_RESOURCE_STATE_INDEX_BUFFER |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    Buffer preparedVertices;
    Buffer preparedIndices;

    if(!m_vertexBuffer.PrepareReplacement(
        device, uploader, vertices, vertexBytes,
        vertexState, preparedVertices))
    {
        return false;
    }

    if(!m_indexBuffer.PrepareReplacement(
        device, uploader, indices, indexBytes,
        indexState, preparedIndices))
    {
        preparedVertices.Shutdown();
        return false;
    }

    // Both allocations and uploads have succeeded.
    // The caller must have drained rendering GPU work
    // before this method is invoked.

    const bool vertexCommitted =
        m_vertexBuffer.CommitReplacement(
            device, preparedVertices);

    const bool indexCommitted =
        vertexCommitted &&
        m_indexBuffer.CommitReplacement(
            device, preparedIndices);

    preparedVertices.Shutdown();
    preparedIndices.Shutdown();

    if(!vertexCommitted || !indexCommitted)
    {
        return false;
    }

    m_vertexCount = vertexCount;
    m_indexCount = indexCount;
    m_bounds = newBounds;

    m_vertexBufferView.BufferLocation = m_vertexBuffer.GetGPUAddress();
    m_vertexBufferView.StrideInBytes = static_cast<UINT>(stride);
    m_vertexBufferView.SizeInBytes = static_cast<UINT>(vertexBytes);
    
    m_indexBufferView.BufferLocation = m_indexBuffer.GetGPUAddress();
    m_indexBufferView.SizeInBytes = static_cast<UINT>(indexBytes);
    m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;

    return true;
}

void Mesh::Shutdown(){
    m_vertexBuffer.Shutdown();
    m_indexBuffer.Shutdown();
    m_vertexCount = 0;
    m_indexCount = 0;
    m_bounds = {};
}

void Mesh::Bind(ID3D12GraphicsCommandList* commandList) const
{
    commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);

    if(IsIndexed()){
        commandList->IASetIndexBuffer(&m_indexBufferView);
    }
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

const MeshBounds& Mesh::GetBounds() const
{
    return m_bounds;
}