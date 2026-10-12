#include "BottomLevelAccelerationStructure.h"
#include "Core/Log.h"

#include "Renderer/Mesh.h"

#include <string>
#include <vector>
#include <utility>
#include <limits>
#include <string_view>
#include <atomic>

namespace
{
    std::uint64_t AllocateRetirementIdentity()
    {
        static std::atomic<std::uint64_t> next{1};
        auto value = next.load(std::memory_order_relaxed);
        while (value != UINT64_MAX)
        {
            if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            {
                return value;
            }
        }

        return 0; // Never reuse an identity, including across reinitialization.
    }

    bool ValidateCountOverride(
        const Mesh& mesh,
        const RayTracingGeometryCounts& counts)
    {
        const size_t stride = mesh.GetVertexStride();
        const size_t indexSize = sizeof(std::uint32_t);
        const auto maxSize = (std::numeric_limits<size_t>::max)();
        const auto maxCount = (std::numeric_limits<UINT>::max)();

        if (counts.vertexCount == 0 ||
            counts.indexCount == 0 ||
            counts.indexCount % 3 != 0 ||
            counts.vertexCount > maxCount)
        {
            return false;
        }

        if (counts.indexCount > maxCount ||
            stride < sizeof(float) * 3 ||
            stride % sizeof(float) != 0 ||
            counts.vertexCount > maxSize / stride)
        {
            return false;
        }

        if (counts.indexCount > maxSize / indexSize ||
            mesh.GetIndexFormat() != DXGI_FORMAT_R32_UINT)
        {
            return false;
        }

        // Existing BLAS descriptions read FLOAT3 POSITION at byte offset zero.

        bool validPosition = false;

        for (const VertexAttribute& attribute : mesh.GetVertexLayout().GetAttributes())
        {
            if (std::string_view(attribute.semantic ? attribute.semantic : "") == "POSITION" &&
                attribute.semanticIndex == 0 && attribute.type == VertexAttributeType::Float3 && attribute.offset == 0)
            {
                validPosition = true;
                break;
            }
        }

        ID3D12Resource* vertices = mesh.GetVertexBufferResource();

        ID3D12Resource* indices = mesh.GetIndexBufferResource();

        if (!validPosition ||
            !vertices ||
            !indices ||
            mesh.GetVertexBufferAddress() == 0)
        {
            return false;
        }

        if (mesh.GetVertexBufferAddress() % sizeof(float) != 0 ||
            mesh.GetIndexBufferAddress() == 0 ||
            mesh.GetIndexBufferAddress() % indexSize != 0)
        {
            return false;
        }

        const D3D12_RESOURCE_DESC vertexDesc = vertices->GetDesc();

        const D3D12_RESOURCE_DESC indexDesc = indices->GetDesc();
        // CPU meshes do not populate explicit capacity members; resource widths
        // remain the authoritative allocation bounds for those meshes.
        return vertexDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
               indexDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
               counts.vertexCount * stride <= vertexDesc.Width && counts.indexCount * indexSize <= indexDesc.Width &&
               (mesh.GetVertexCapacity() == 0 || counts.vertexCount <= mesh.GetVertexCapacity()) &&
               (mesh.GetIndexCapacity() == 0 || counts.indexCount <= mesh.GetIndexCapacity());
    }
} // namespace

bool BottomLevelAccelerationStructure::Initialize(
    ID3D12Device5* device,
    ID3D12GraphicsCommandList4* commandList,
    const RayTracingBottomLevelDescription& description)
{
    if (!device || !commandList || description.geometries.empty())
    {
        return false;
    }

    const auto lifetime = AllocateRetirementIdentity();

    if (lifetime == 0)
    {

        return false;
    }

    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescriptions;
    geometryDescriptions.reserve(description.geometries.size());

    for (const RayTracingGeometryDescription& geometry : description.geometries)
    {
        if (!geometry.mesh)
        {
            return false;
        }

        const Mesh& mesh = *geometry.mesh;

        if (geometry.countOverride ? !ValidateCountOverride(mesh, *geometry.countOverride) : !mesh.IsIndexed())
        {
            return false;
        }

        const size_t vertexCount = geometry.countOverride ? geometry.countOverride->vertexCount : mesh.GetVertexCount();

        const size_t indexCount = geometry.countOverride ? geometry.countOverride->indexCount : mesh.GetIndexCount();

        D3D12_RAYTRACING_GEOMETRY_DESC geometryDescription{};
        geometryDescription.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometryDescription.Flags =
            geometry.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        geometryDescription.Triangles.VertexBuffer.StartAddress = mesh.GetVertexBufferAddress();
        geometryDescription.Triangles.VertexBuffer.StrideInBytes = mesh.GetVertexStride();
        geometryDescription.Triangles.VertexCount = static_cast<UINT>(vertexCount);
        geometryDescription.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;

        geometryDescription.Triangles.IndexBuffer = mesh.GetIndexBufferAddress();
        geometryDescription.Triangles.IndexCount = static_cast<UINT>(indexCount);
        geometryDescription.Triangles.IndexFormat = mesh.GetIndexFormat();
        geometryDescriptions.push_back(geometryDescription);
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS buildInputs;
    buildInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;

    buildInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    buildInputs.NumDescs = static_cast<UINT>(geometryDescriptions.size());
    buildInputs.pGeometryDescs = geometryDescriptions.data();
    buildInputs.Flags = description.buildPreference == RayTracingAccelerationStructureBuildPreference::FastBuild
                            ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD
                            : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo{};

    device->GetRaytracingAccelerationStructurePrebuildInfo(&buildInputs, &prebuildInfo);

    if (prebuildInfo.ResultDataMaxSizeInBytes == 0 || prebuildInfo.ScratchDataSizeInBytes == 0)
    {
        return false;
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resourceDescription{};

    resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDescription.Height = 1;
    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;

    resourceDescription.Format = DXGI_FORMAT_UNKNOWN;
    resourceDescription.SampleDesc.Count = 1;
    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    resourceDescription.Width = prebuildInfo.ScratchDataSizeInBytes;

    HRESULT result =
        device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_scratchResource));

    if (FAILED(result))
    {
        Log::Error("Failed to allocate BLAS scratch resource. HRESULT: " + std::to_string(result));
        Shutdown();

        return false;
    }

    resourceDescription.Width = prebuildInfo.ResultDataMaxSizeInBytes;

    result = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription,
                                             D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
                                             IID_PPV_ARGS(&m_resource));

    if (FAILED(result))
    {
        Log::Error("Failed to allocate BLAS result resource. HRESULT: " + std::to_string(result));
        Shutdown();
        return false;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDescription{};
    buildDescription.Inputs = buildInputs;
    buildDescription.ScratchAccelerationStructureData = m_scratchResource->GetGPUVirtualAddress();

    buildDescription.DestAccelerationStructureData = m_resource->GetGPUVirtualAddress();

    commandList->BuildRaytracingAccelerationStructure(&buildDescription, 0, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = m_resource;

    commandList->ResourceBarrier(1, &barrier);

    m_retirementLifetime = lifetime;
    return true;
}

bool BottomLevelAccelerationStructure::Rebuild(
    ID3D12Device5* device,
    ID3D12GraphicsCommandList4* commandList,
    const RayTracingBottomLevelDescription& description)
{
    if (!device || !commandList || !m_resource || m_retirementLifetime == 0)
    {
        return false;
    }

    BottomLevelAccelerationStructure replacement;

    const auto batchID = AllocateRetirementIdentity();

    if (batchID == 0)
    {
        return false;
    }

    // Reserve CPU storage before recording replacement commands/ownership swap.

    m_retiredBatches.reserve(m_retiredBatches.size() + 1);

    if (!replacement.Initialize(device, commandList, description))
    {

        return false;
    }

    std::swap(m_resource, replacement.m_resource);
    std::swap(m_scratchResource, replacement.m_scratchResource);

    // Retain the previous BLAS resources until the renderer
    // establishes that the GPU has finished using them.

    m_retiredBatches.push_back(
        {{m_retirementLifetime, batchID}, replacement.m_resource, replacement.m_scratchResource});
    replacement.m_resource = nullptr;
    replacement.m_scratchResource = nullptr;

    replacement.Shutdown();

    return true;
}

void BottomLevelAccelerationStructure::ReleaseRetiredResources()
{
    for (const RetiredBatch& batch : m_retiredBatches)
    {
        if (batch.result)
        {
            batch.result->Release();
        }

        if (batch.scratch)
        {
            batch.scratch->Release();
        }
    }

    m_retiredBatches.clear();
}

BLASRetirementBatchIdentity BottomLevelAccelerationStructure::GetExclusiveRetirementBatch() const
{
    return m_retiredBatches.size() == 1 ? m_retiredBatches.front().identity : BLASRetirementBatchIdentity{};
}

bool BottomLevelAccelerationStructure::ValidateRetirementBatch(BLASRetirementBatchIdentity identity) const
{
    const auto current = GetExclusiveRetirementBatch();

    return identity.ownerLifetime != 0 && identity.ownerLifetime == m_retirementLifetime &&
           identity.ownerLifetime == current.ownerLifetime && identity.batchID != 0 &&
           identity.batchID == current.batchID;
}

bool BottomLevelAccelerationStructure::ReleaseRetirementBatch(
    BLASRetirementBatchIdentity identity,
    bool gpuCompletionConfirmed,
    bool referencesSafe)
{
    if (!gpuCompletionConfirmed || !referencesSafe || !ValidateRetirementBatch(identity))
    {
        return false;
    }

    // Validation proves exactly one batch; never select an arbitrary batch.
    const RetiredBatch& batch = m_retiredBatches.front();

    if (batch.result)
    {
        batch.result->Release();
    }

    if (batch.scratch)
    {
        batch.scratch->Release();
    }

    m_retiredBatches.clear();
    return true;
}

void BottomLevelAccelerationStructure::Shutdown()
{
    ReleaseRetiredResources();

    m_retirementLifetime = 0;

    if (m_scratchResource)
    {
        m_scratchResource->Release();

        m_scratchResource = nullptr;
    }

    if (m_resource)
    {
        m_resource->Release();
        m_resource = nullptr;
    }
}

D3D12_GPU_VIRTUAL_ADDRESS BottomLevelAccelerationStructure::GetGPUAddress() const
{
    if (!m_resource)
    {
        return 0;
    }

    return m_resource->GetGPUVirtualAddress();
}
