#include "Mesh.h"
#include "Debug/MemoryRegistry.h"

#include "Renderer/DescriptorHeap.h"
#include "Renderer/Vertex.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <limits>
#include <string_view>
#include <cstdint>
#include <climits>
#include <type_traits>

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

    // Two faces that meet at an edge at more than this angle (30 degrees) form a sharp edge.
    constexpr float SilhouetteSharpEdgeCosine = 0.866f;

    // Per triangle, bits 0, 1 and 2 for its edges (vertex 0 to 1, 1 to 2, 2 to 0) where the surface ends (no other
    // triangle shares the edge) or turns sharply (the one other triangle there meets it at more than 30 degrees), such as
    // the outline of a plane or the corners of a box. Edges are matched by position, so a UV seam or a normal split on a
    // smooth or flat surface is no edge here: a floor tiled several times over one quad, a sphere's seam or the shared
    // diagonal of a quad never clip. An edge three or more triangles share is left unmarked. Empty when no edge qualifies
    // or the data cannot be read.
    std::vector<std::uint32_t> CalculateSilhouetteEdges(const void* vertices, size_t vertexCount,
        const VertexLayout& layout, const unsigned int* indices, size_t indexCount, const MeshBounds& bounds)
    {
        const size_t stride = layout.GetStride();
        const VertexAttribute* positionAttribute = nullptr;

        for (const VertexAttribute& attribute : layout.GetAttributes())
        {
            if (std::string_view(attribute.semantic ? attribute.semantic : "") == "POSITION" &&
                attribute.semanticIndex == 0 && attribute.type == VertexAttributeType::Float3)
            {
                positionAttribute = &attribute;
                break;
            }
        }

        if (!vertices || !indices || indexCount < 3 || indexCount % 3 != 0 || stride == 0 || !positionAttribute ||
            !bounds.valid || positionAttribute->offset > stride || sizeof(float) * 3 > stride - positionAttribute->offset)
        {
            return {};
        }

        const auto* vertexBytes = static_cast<const unsigned char*>(vertices);
        const auto position = [&](unsigned int index)
        {
            float value[3];
            std::memcpy(value, vertexBytes + static_cast<size_t>(index) * stride + positionAttribute->offset,
                sizeof(value));
            return Vector3{value[0], value[1], value[2]};
        };

        // Positions closer than a millionth of the mesh's size are one point (split vertices of a box corner).
        const Vector3 extent = bounds.max - bounds.min;
        const float cell = (std::max)((std::max)(extent.x, extent.y), (std::max)(extent.z, 1.0e-6f)) * 1.0e-6f;
        std::unordered_map<std::uint64_t, std::uint32_t> pointIds;
        std::vector<std::uint32_t> pointOfVertex(vertexCount, UINT32_MAX);

        const auto pointId = [&](unsigned int index) -> std::uint32_t
        {
            if (pointOfVertex[index] != UINT32_MAX)
            {
                return pointOfVertex[index];
            }

            const Vector3 p = position(index);
            const auto quantize = [&](float value)
            {
                return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::llround(value / cell)) & 0x1FFFFF);
            };
            const std::uint64_t key = quantize(p.x) | (quantize(p.y) << 21) | (quantize(p.z) << 42);
            const auto inserted = pointIds.emplace(key, static_cast<std::uint32_t>(pointIds.size()));
            pointOfVertex[index] = inserted.first->second;
            return inserted.first->second;
        };

        const size_t triangleCount = indexCount / 3;
        std::vector<Vector3> faceNormals(triangleCount);
        std::vector<std::array<std::uint32_t, 3>> trianglePoints(triangleCount);

        // Each edge, by its two points: how many triangles use it and the first two of them (triangle * 3 + edge).
        struct EdgeUse
        {
            std::uint32_t count = 0;
            std::uint32_t first = UINT32_MAX;
            std::uint32_t second = UINT32_MAX;
        };

        std::unordered_map<std::uint64_t, EdgeUse> edges;
        edges.reserve(indexCount);

        for (size_t triangle = 0; triangle < triangleCount; ++triangle)
        {
            const unsigned int* corner = indices + triangle * 3;

            if (corner[0] >= vertexCount || corner[1] >= vertexCount || corner[2] >= vertexCount)
            {
                return {};
            }

            const Vector3 a = position(corner[0]);
            const Vector3 b = position(corner[1]);
            const Vector3 c = position(corner[2]);
            faceNormals[triangle] = Vector3::Cross(b - a, c - a).Normalized();

            for (size_t edge = 0; edge < 3; ++edge)
            {
                trianglePoints[triangle][edge] = pointId(corner[edge]);
            }

            for (std::uint32_t edge = 0; edge < 3; ++edge)
            {
                const std::uint32_t from = trianglePoints[triangle][edge];
                const std::uint32_t to = trianglePoints[triangle][(edge + 1) % 3];

                if (from == to)
                {
                    continue;
                }

                const std::uint64_t key = (static_cast<std::uint64_t>((std::min)(from, to)) << 32) | (std::max)(from, to);
                EdgeUse& use = edges[key];
                const std::uint32_t reference = static_cast<std::uint32_t>(triangle * 3 + edge);
                if (use.count == 0)
                {
                    use.first = reference;
                }
                else if (use.count == 1)
                {
                    use.second = reference;
                }

                ++use.count;
            }
        }

        std::vector<std::uint32_t> flags(triangleCount, 0u);
        bool any = false;

        for (const auto& [key, use] : edges)
        {
            bool sharp = use.count == 1;

            if (use.count == 2)
            {
                const Vector3& first = faceNormals[use.first / 3];
                const Vector3& second = faceNormals[use.second / 3];
                sharp = Vector3::Dot(first, second) < SilhouetteSharpEdgeCosine;
            }

            if (!sharp)
            {
                continue;
            }

            flags[use.first / 3] |= 1u << (use.first % 3);

            if (use.count == 2)
            {
                flags[use.second / 3] |= 1u << (use.second % 3);
            }

            any = true;
        }

        return any ? flags : std::vector<std::uint32_t>{};
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
    const MemoryCategoryScope memoryScope(MemoryCategory::MeshesTextures);

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

    const MemoryCategoryScope memoryScope(MemoryCategory::MeshesTextures);

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

    // Without its silhouette edges the mesh still renders; parallax materials then keep its straight outline.
    const std::vector<std::uint32_t> silhouetteEdges = CalculateSilhouetteEdges(vertices, vertexCount, layout, indices,
        indexCount, m_bounds);

    if (!silhouetteEdges.empty() &&
        (!m_silhouetteEdges.Initialize(device, uploader, silhouetteEdges.data(),
            silhouetteEdges.size() * sizeof(std::uint32_t), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) ||
         !m_silhouetteEdges.CreateRawSRV(device, descriptorHeap)))
    {
        m_silhouetteEdges.Shutdown();
    }

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
    const MemoryCategoryScope memoryScope(MemoryCategory::MeshesTextures);

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
    const MemoryCategoryScope memoryScope(MemoryCategory::MeshesTextures);

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

    // The new geometry's silhouette edges in place of the old ones (the caller has drained GPU work, as above). Instance
    // records keep the descriptor they were built with, so an existing table is replaced in place and a new one waits
    // for the next record build. A table that cannot be prepared is cleared, which only drops the clipping.
    std::vector<std::uint32_t> silhouetteEdges = CalculateSilhouetteEdges(vertices, vertexCount, m_layout, indices,
        indexCount, m_bounds);

    if (m_silhouetteEdges.GetDescriptorIndex() != UINT_MAX)
    {
        if (silhouetteEdges.empty())
        {
            silhouetteEdges.assign(indexCount / 3, 0u);
        }

        Buffer preparedEdges;
        const bool edgesReplaced = m_silhouetteEdges.PrepareReplacement(device, uploader, silhouetteEdges.data(),
            silhouetteEdges.size() * sizeof(std::uint32_t), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, preparedEdges) &&
            m_silhouetteEdges.CommitReplacement(device, preparedEdges);
        preparedEdges.Shutdown();

        if (!edgesReplaced)
        {
            m_silhouetteEdges.Shutdown();
        }
    }
    else if (!silhouetteEdges.empty() &&
        (!m_silhouetteEdges.Initialize(device, uploader, silhouetteEdges.data(),
            silhouetteEdges.size() * sizeof(std::uint32_t), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) ||
         !m_silhouetteEdges.CreateRawSRV(device, descriptorHeap)))
    {
        m_silhouetteEdges.Shutdown();
    }

    return true;
}

void Mesh::Shutdown()
{
    m_vertexBuffer.Shutdown();
    m_indexBuffer.Shutdown();
    m_silhouetteEdges.Shutdown();
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

unsigned int Mesh::GetSilhouetteEdgeDescriptorIndex() const
{
    return m_silhouetteEdges.GetDescriptorIndex();
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
