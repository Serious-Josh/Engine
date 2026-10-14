#include "Mesh.h"

#include "Renderer/DescriptorHeap.h"
#include "Renderer/Vertex.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <cstdint>
#include <climits>

namespace
{
    MeshBounds CalculateBounds(
        const void* vertices,
        size_t vertexCount,
        const VertexLayout& layout)
    {
        MeshBounds bounds{};

        if (!vertices || vertexCount == 0)
        {
            return bounds;
        }

        const size_t stride = layout.GetStride();

        if (stride == 0)
        {
            return bounds;
        }

        const VertexAttribute* positionAttritube = nullptr;

        for (const VertexAttribute& attribute : layout.GetAttributes())
        {

            if (std::string_view(attribute.semantic ? attribute.semantic : "") == "POSITION" &&
                attribute.semanticIndex == 0 && attribute.type == VertexAttributeType::Float3)
            {
                positionAttritube = &attribute;
                break;
            }
        }

        if (!positionAttritube)
        {

            return bounds;
        }

        const size_t offset = positionAttritube->offset;

        if (offset > stride || sizeof(float) * 3 > stride - offset)
        {
            return bounds;
        }

        const auto* vertexBytes = static_cast<const unsigned char*>(vertices);

        const float largest = (std::numeric_limits<float>::max)();

        bounds.min = Vector3{largest, largest, largest};
        bounds.max = Vector3{-largest, -largest, -largest};

        for (size_t i = 0; i < vertexCount; ++i)
        {
            float position[3];

            std::memcpy(position, vertexBytes + i * stride + offset, sizeof(position));

            if (!std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2]))
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

bool Mesh::Initialize(

    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const void* vertices,
    size_t vertexCount,
    const VertexLayout& layout)
{
    m_layout = layout;
    m_vertexCount = vertexCount;
    ResetMetadata();

    m_bounds = CalculateBounds(vertices, vertexCount, layout);

    const D3D12_RESOURCE_STATES vertexState =
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    if (!m_vertexBuffer.Initialize(device, uploader, vertices, vertexCount * layout.GetStride(), vertexState))
    {
        return false;
    }

    if (!m_vertexBuffer.CreateRawSRV(device, descriptorHeap))
    {
        Shutdown();

        return false;
    }

    m_vertexBufferView.BufferLocation = m_vertexBuffer.GetGPUAddress();

    m_vertexBufferView.StrideInBytes = static_cast<UINT>(layout.GetStride());

    m_vertexBufferView.SizeInBytes = static_cast<UINT>(vertexCount * layout.GetStride());

    return true;
}

bool Mesh::Initialize(
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const void* vertices,
    size_t vertexCount,
    const VertexLayout& layout,
    const unsigned int* indices,
    size_t indexCount)
{
    if (!indices || indexCount == 0)
    {
        return false;
    }

    if (!Initialize(device, uploader, descriptorHeap, vertices, vertexCount, layout))
    {
        return false;
    }

    const D3D12_RESOURCE_STATES indexState =
        D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    if (!m_indexBuffer.Initialize(device, uploader, indices, indexCount * sizeof(unsigned int), indexState))
    {
        Shutdown();

        return false;
    }

    if (!m_indexBuffer.CreateRawSRV(device, descriptorHeap))
    {
        Shutdown();
        return false;
    }

    m_indexCount = indexCount;
    ResetMetadata();

    m_indexBufferView.BufferLocation = m_indexBuffer.GetGPUAddress();
    m_indexBufferView.SizeInBytes = static_cast<UINT>(indexCount * sizeof(unsigned int));

    m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;

    return true;
}

bool Mesh::InitializeGPUWritable(
    ID3D12Device* device,
    DescriptorHeap& descriptorHeap,
    const VertexLayout& layout,
    size_t vertexCapacity,
    size_t indexCapacity,
    const MeshBounds& bounds)
{
    const size_t stride = layout.GetStride();

    if (!device ||
        stride == 0 ||
        stride > UINT_MAX ||
        vertexCapacity == 0)
    {
        return false;
    }

    if (indexCapacity == 0 ||
        vertexCapacity > UINT_MAX / stride ||
        indexCapacity > UINT_MAX / sizeof(unsigned int) ||
        (vertexCapacity * stride) % sizeof(std::uint32_t) != 0)
    {
        return false;
    }

    if (!bounds.valid ||
        !std::isfinite(bounds.min.x) ||
        !std::isfinite(bounds.min.y) ||
        !std::isfinite(bounds.min.z))
    {
        return false;
    }

    if (!std::isfinite(bounds.max.x) ||
        !std::isfinite(bounds.max.y) ||
        !std::isfinite(bounds.max.z) ||
        bounds.min.x > bounds.max.x)
    {
        return false;
    }

    if (bounds.min.y > bounds.max.y ||
        bounds.min.z > bounds.max.z ||
        m_vertexBuffer.GetResource() ||
        m_indexBuffer.GetResource())
    {
        return false;
    }

    const size_t vertexBytes = vertexCapacity * stride;
    const size_t indexBytes = indexCapacity * sizeof(unsigned int);

    Buffer vertices;
    Buffer indices;

    if (!vertices.InitializeGPUWritable(device, vertexBytes) || !vertices.CreateRawSRV(device, descriptorHeap) ||

        !vertices.CreateRawUAV(device, descriptorHeap) || !indices.InitializeGPUWritable(device, indexBytes) ||
        !indices.CreateRawSRV(device, descriptorHeap) || !indices.CreateRawUAV(device, descriptorHeap))
    {
        vertices.Shutdown();
        indices.Shutdown();
        return false;
    }

    m_vertexBuffer = std::move(vertices);
    m_indexBuffer = std::move(indices);

    m_layout = layout;
    m_bounds = bounds;

    m_vertexCapacity = vertexCapacity;
    m_indexCapacity = indexCapacity;

    m_gpuWritable = true;

    // No generated geometry exists yet.
    m_vertexCount = 0;
    m_indexCount = 0;
    ResetMetadata();

    m_vertexBufferView = {};
    m_vertexBufferView.BufferLocation = m_vertexBuffer.GetGPUAddress();
    m_vertexBufferView.StrideInBytes = static_cast<UINT>(stride);
    m_vertexBufferView.SizeInBytes = static_cast<UINT>(vertexBytes);

    m_indexBufferView = {};
    m_indexBufferView.BufferLocation = m_indexBuffer.GetGPUAddress();
    m_indexBufferView.SizeInBytes = static_cast<UINT>(indexBytes);
    m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;

    return true;
}

bool Mesh::InitializeGPUWritableSeeded(
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
    const MeshBounds& bounds)
{
    static_assert(sizeof(unsigned int) == sizeof(std::uint32_t));

    if (!device ||
        !vertices ||
        !indices ||
        vertexCount == 0)
    {
        return false;
    }

    if (indexCount == 0 ||
        indexCount % 3 != 0 ||
        vertexCount > vertexCapacity ||
        indexCount > indexCapacity)
    {
        return false;
    }

    if (vertexCapacity > UINT_MAX / sizeof(Vertex) ||
        indexCapacity > UINT_MAX / sizeof(unsigned int) ||
        vertexCapacity > (std::numeric_limits<size_t>::max)() / sizeof(Vertex) ||
        indexCapacity > (std::numeric_limits<size_t>::max)() / sizeof(unsigned int))
    {
        return false;
    }

    if (m_vertexBuffer.GetResource() ||
        m_indexBuffer.GetResource() ||
        !bounds.valid ||
        !std::isfinite(bounds.min.x))
    {
        return false;
    }

    if (!std::isfinite(bounds.min.y) ||
        !std::isfinite(bounds.min.z) ||
        !std::isfinite(bounds.max.x) ||
        !std::isfinite(bounds.max.y))
    {
        return false;
    }

    if (!std::isfinite(bounds.max.z) ||
        bounds.min.x > bounds.max.x ||
        bounds.min.y > bounds.max.y ||
        bounds.min.z > bounds.max.z)
    {
        return false;
    }

    const VertexLayout expectedLayout = CreateVertexLayout();
    const auto& attributes = layout.GetAttributes();
    const auto& expectedAttributes = expectedLayout.GetAttributes();

    if (layout.GetStride() != sizeof(Vertex) || attributes.size() != expectedAttributes.size())
    {
        return false;
    }

    for (size_t i = 0; i < attributes.size(); ++i)
    {
        const auto& attribute = attributes[i];

        const auto& expected = expectedAttributes[i];

        if (!attribute.semantic ||
            std::strcmp(attribute.semantic, expected.semantic) != 0 ||
            attribute.semanticIndex != expected.semanticIndex ||
            attribute.type != expected.type)
        {
            return false;
        }

        if (attribute.offset != expected.offset)
        {
            return false;
        }
    }

    const MeshBounds seedBounds = CalculateBounds(vertices, vertexCount, layout);

    if (!seedBounds.valid ||
        seedBounds.min.x < bounds.min.x ||
        seedBounds.min.y < bounds.min.y ||
        seedBounds.min.z < bounds.min.z)
    {
        return false;
    }

    if (seedBounds.max.x > bounds.max.x ||
        seedBounds.max.y > bounds.max.y ||
        seedBounds.max.z > bounds.max.z)
    {
        return false;
    }

    for (size_t i = 0; i < indexCount; ++i)
    {
        if (indices[i] >= vertexCount)
        {
            return false;
        }
    }

    constexpr D3D12_RESOURCE_STATES vertexState =

        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    constexpr D3D12_RESOURCE_STATES indexState =
        D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    Buffer seededVertices;
    Buffer seededIndices;

    if (!seededVertices.InitializeGPUWritableSeeded(device, uploader, vertices, vertexCount * sizeof(Vertex),
                                                    vertexCapacity * sizeof(Vertex), vertexState) ||

        !seededIndices.InitializeGPUWritableSeeded(device, uploader, indices, indexCount * sizeof(unsigned int),
                                                   indexCapacity * sizeof(unsigned int), indexState) ||
        !seededVertices.CreateRawSRV(device, descriptorHeap) || !seededVertices.CreateRawUAV(device, descriptorHeap) ||
        !seededIndices.CreateRawSRV(device, descriptorHeap) || !seededIndices.CreateRawUAV(device, descriptorHeap))
    {
        seededVertices.Shutdown();
        seededIndices.Shutdown();
        return false;
    }

    m_vertexBuffer = std::move(seededVertices);
    m_indexBuffer = std::move(seededIndices);
    m_layout = layout;
    m_bounds = bounds;

    m_vertexCapacity = vertexCapacity;
    m_indexCapacity = indexCapacity;
    m_gpuWritable = true;
    // Both synchronous uploads and all descriptor allocations have succeeded.

    m_vertexCount = vertexCount;
    m_indexCount = indexCount;
    ResetMetadata();
    m_vertexBufferView = {};
    m_vertexBufferView.BufferLocation = m_vertexBuffer.GetGPUAddress();

    m_vertexBufferView.StrideInBytes = static_cast<UINT>(sizeof(Vertex));
    m_vertexBufferView.SizeInBytes = static_cast<UINT>(vertexCount * sizeof(Vertex));
    m_indexBufferView = {};
    m_indexBufferView.BufferLocation = m_indexBuffer.GetGPUAddress();

    m_indexBufferView.SizeInBytes = static_cast<UINT>(indexCount * sizeof(unsigned int));
    m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;
    return true;
}

bool Mesh::SetGPUGeneratedGeometryCounts(
    size_t vertexCount,
    size_t indexCount)
{
    if (!m_gpuWritable ||
        !m_vertexBuffer.GetResource() ||
        !m_indexBuffer.GetResource() ||
        vertexCount == 0)
    {
        return false;
    }

    if (indexCount == 0 ||
        vertexCount > m_vertexCapacity ||
        indexCount > m_indexCapacity ||
        indexCount % 3 != 0)
    {
        return false;
    }

    const size_t vertexBytes = vertexCount * m_layout.GetStride();
    const size_t indexBytes = indexCount * sizeof(unsigned int);

    m_vertexCount = vertexCount;
    m_indexCount = indexCount;
    ResetMetadata();

    m_vertexBufferView.SizeInBytes = static_cast<UINT>(vertexBytes);
    m_indexBufferView.SizeInBytes = static_cast<UINT>(indexBytes);

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

    if (m_gpuWritable ||
        !device ||
        !vertices ||
        !indices)
    {
        return false;
    }

    if (vertexCount == 0 ||
        indexCount == 0 ||
        !IsIndexed() ||
        stride == 0)
    {
        return false;
    }

    if (stride > UINT_MAX ||
        vertexCount > UINT_MAX / stride ||
        indexCount > UINT_MAX / sizeof(unsigned int) ||
        m_vertexBuffer.GetDescriptorIndex() == UINT_MAX)
    {
        return false;
    }

    if (m_indexBuffer.GetDescriptorIndex() == UINT_MAX)
    {
        return false;
    }

    const MeshBounds newBounds = CalculateBounds(vertices, vertexCount, m_layout);

    if (!newBounds.valid)
    {
        return false;
    }

    const size_t vertexBytes = vertexCount * stride;
    const size_t indexBytes = indexCount * sizeof(unsigned int);

    const D3D12_RESOURCE_STATES vertexState =
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    const D3D12_RESOURCE_STATES indexState =
        D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    Buffer preparedVertices;
    Buffer preparedIndices;

    if (!m_vertexBuffer.PrepareReplacement(device, uploader, vertices, vertexBytes, vertexState, preparedVertices))
    {
        return false;
    }

    if (!m_indexBuffer.PrepareReplacement(device, uploader, indices, indexBytes, indexState, preparedIndices))
    {
        preparedVertices.Shutdown();
        return false;
    }

    // Both allocations and uploads have succeeded.
    // The caller must have drained rendering GPU work
    // before this method is invoked.

    const bool vertexCommitted = m_vertexBuffer.CommitReplacement(device, preparedVertices);

    const bool indexCommitted = vertexCommitted && m_indexBuffer.CommitReplacement(device, preparedIndices);

    preparedVertices.Shutdown();

    preparedIndices.Shutdown();

    if (!vertexCommitted || !indexCommitted)
    {
        return false;
    }

    m_vertexCount = vertexCount;
    m_indexCount = indexCount;
    ResetMetadata();
    m_bounds = newBounds;

    m_vertexBufferView.BufferLocation = m_vertexBuffer.GetGPUAddress();
    m_vertexBufferView.StrideInBytes = static_cast<UINT>(stride);
    m_vertexBufferView.SizeInBytes = static_cast<UINT>(vertexBytes);

    m_indexBufferView.BufferLocation = m_indexBuffer.GetGPUAddress();
    m_indexBufferView.SizeInBytes = static_cast<UINT>(indexBytes);

    m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;

    return true;
}

void Mesh::Shutdown()
{
    m_vertexBuffer.Shutdown();
    m_indexBuffer.Shutdown();
    m_vertexCount = 0;
    m_indexCount = 0;
    ResetMetadata();
    m_bounds = {};

    m_vertexCapacity = 0;
    m_indexCapacity = 0;
    m_vertexBufferView = {};
    m_indexBufferView = {};

    m_gpuWritable = false;
}

void Mesh::Bind(ID3D12GraphicsCommandList* commandList) const
{
    commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);

    if (IsIndexed())
    {
        commandList->IASetIndexBuffer(&m_indexBufferView);
    }
}

void Mesh::TransitionVertexBuffer(
    ID3D12GraphicsCommandList* commandList,
    D3D12_RESOURCE_STATES newState)
{
    m_vertexBuffer.Transition(commandList, newState);
}

ID3D12Resource* Mesh::GetIndexBufferResource() const
{
    return m_indexBuffer.GetResource();
}

void Mesh::TransitionIndexBuffer(

    ID3D12GraphicsCommandList* commandList,
    D3D12_RESOURCE_STATES newState)
{
    m_indexBuffer.Transition(commandList, newState);
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

unsigned int Mesh::GetVertexBufferUAVDescriptorIndex() const
{
    return m_vertexBuffer.GetUAVDescriptorIndex();
}

unsigned int Mesh::GetIndexBufferUAVDescriptorIndex() const
{
    return m_indexBuffer.GetUAVDescriptorIndex();
}

size_t Mesh::GetVertexCapacity() const
{
    return m_vertexCapacity;
}

size_t Mesh::GetIndexCapacity() const
{
    return m_indexCapacity;
}

ID3D12Resource* Mesh::GetVertexBufferResource() const
{
    return m_vertexBuffer.GetResource();
}

D3D12_RESOURCE_STATES Mesh::GetVertexBufferState() const
{
    return m_vertexBuffer.GetResourceState();
}

D3D12_RESOURCE_STATES Mesh::GetIndexBufferState() const
{
    return m_indexBuffer.GetResourceState();
}

void Mesh::ResetMetadata() noexcept
{
    m_metadata.reset();
    m_defaultMetadata.submeshes.front().indexCount = static_cast<uint32_t>(m_indexCount ? m_indexCount : m_vertexCount);
}

bool Mesh::SetMetadata(MeshMetadata metadata, std::string& error)
{
    if(!ValidateMeshMetadata(metadata, m_indexCount ? m_indexCount : m_vertexCount, error))
    {
        return false;
    }

    m_metadata = std::move(metadata);
    return true;
}

const MeshMetadata& Mesh::GetMetadata() const noexcept
{
    return m_metadata ? *m_metadata : m_defaultMetadata;
}
