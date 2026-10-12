#include "Renderer/Displacement/DisplacementGeometryBinding.h"

#include "Renderer/Mesh.h"
#include "Renderer/Vertex.h"
#include "Renderer/RayTracing/RayTracingAccelerationStructure.h"

#include <climits>
#include <cstddef>
#include <cstdint>

bool RendererDisplacement::TryBuildGeometryBinding(
    const Mesh& mesh,
    const RayTracingGeometryCounts& targetCounts,
    GeometryBinding& binding)
{
    const size_t vertexCount = targetCounts.vertexCount;
    const size_t indexCount = targetCounts.indexCount;
    const UINT stride = mesh.GetVertexStride();
    constexpr size_t indexStride = sizeof(std::uint32_t);

    if (vertexCount == 0 || indexCount == 0 || indexCount % 3 != 0 || stride != sizeof(Vertex))
    {
        return false;
    }

    if (vertexCount > UINT_MAX / stride || indexCount > UINT_MAX / indexStride ||

        vertexCount > mesh.GetVertexCapacity() || indexCount > mesh.GetIndexCapacity())
    {
        return false;
    }

    if (mesh.GetIndexFormat() != DXGI_FORMAT_R32_UINT)
    {
        return false;
    }

    ID3D12Resource* vertices = mesh.GetVertexBufferResource();

    ID3D12Resource* indices = mesh.GetIndexBufferResource();

    if (!vertices || !indices)
    {
        return false;
    }

    const auto vertexAddress = mesh.GetVertexBufferAddress();

    const auto indexAddress = mesh.GetIndexBufferAddress();
    const size_t vertexBytes = vertexCount * stride;
    const size_t indexBytes = indexCount * indexStride;

    if (vertexAddress == 0 || indexAddress == 0 || vertexAddress % sizeof(float) != 0 ||
        indexAddress % indexStride != 0)
    {
        return false;
    }

    if (vertexBytes > UINT64_MAX - vertexAddress || indexBytes > UINT64_MAX - indexAddress)
    {
        return false;
    }

    const D3D12_RESOURCE_DESC vertexDesc = vertices->GetDesc();

    const D3D12_RESOURCE_DESC indexDesc = indices->GetDesc();

    if (vertexDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        indexDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        (vertexDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0 ||
        (indexDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
    {
        return false;
    }

    if (vertexBytes > vertexDesc.Width || indexBytes > indexDesc.Width)
    {
        return false;
    }

    GeometryBinding prepared{};
    prepared.mesh = &mesh;
    prepared.vertexCount = static_cast<UINT>(vertexCount);

    prepared.indexCount = static_cast<UINT>(indexCount);
    prepared.vertexView = {vertexAddress, static_cast<UINT>(vertexBytes), stride};
    prepared.indexView = {indexAddress, static_cast<UINT>(indexBytes), DXGI_FORMAT_R32_UINT};
    binding = prepared;

    return true;
}
