#pragma once

#include "RayTracingAccelerationStructure.h"

#include <d3d12.h>
#include <climits>
#include <cstddef>

// Pure native BLAS triangle-descriptor construction shared by the production BLAS
// builder and diagnostic builders. Callers keep their own eligibility validation
// (indexed meshes, count overrides, admitted generations); this helper only maps
// already-validated values to D3D12 fields. No allocation, GPU calls or global state.
struct BLASTriangleGeometryInputs
{
    D3D12_GPU_VIRTUAL_ADDRESS vertexAddress = 0;
    UINT64 vertexStride = 0;
    std::size_t vertexCount = 0;
    D3D12_GPU_VIRTUAL_ADDRESS indexAddress = 0;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
    std::size_t indexCount = 0;
    bool opaque = true;
};

// Positions are always FLOAT3 at the start of each vertex; no per-geometry transform.
// Fails, leaving output unchanged, only when a count cannot be represented as UINT.
inline bool BuildBLASTriangleGeometryDescription(
    const BLASTriangleGeometryInputs& inputs,
    D3D12_RAYTRACING_GEOMETRY_DESC& output) noexcept
{
    if(inputs.vertexCount > UINT_MAX || inputs.indexCount > UINT_MAX)
    {
        return false;
    }

    D3D12_RAYTRACING_GEOMETRY_DESC description{};
    description.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    description.Flags = inputs.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    description.Triangles.VertexBuffer.StartAddress = inputs.vertexAddress;
    description.Triangles.VertexBuffer.StrideInBytes = inputs.vertexStride;
    description.Triangles.VertexCount = static_cast<UINT>(inputs.vertexCount);
    description.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    description.Triangles.IndexBuffer = inputs.indexAddress;
    description.Triangles.IndexCount = static_cast<UINT>(inputs.indexCount);
    description.Triangles.IndexFormat = inputs.indexFormat;
    output = description;
    return true;
}

inline D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS BLASBuildFlags(
    RayTracingAccelerationStructureBuildPreference preference) noexcept
{
    return preference == RayTracingAccelerationStructureBuildPreference::FastBuild
               ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD
               : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
}
