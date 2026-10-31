#pragma once

#include "CausticGeometryCertificate.h"
#include <cstring>
#include <memory>
#include <new>
#include <wrl/client.h>

class Mesh;

namespace CausticContracts::Geometry
{
    inline constexpr std::size_t MaximumSourceBytes = 638976;
    inline constexpr std::uint64_t MaximumGenerationResourceBytes = 2ull * 1024 * 1024;
    enum class AdmissionStatus { Ready, InvalidInput, Unsupported, AllocationFailed, UploadFailed, Stale, CapacityExceeded };
    struct AdmissionResult
    {
        AdmissionStatus status;
        Result geometry{Status::Success, NoPrimitive};
    };
    struct StaticSourceRequest
    {
        std::span<const Vertex> vertices;
        std::span<const std::uint32_t> indices;
        Topology topology = Topology::IndexedUInt32Triangles;
        std::uint32_t firstPrimitive = 0, primitiveCount = 0;
        FixturePolicy policy{};
        std::uint64_t fixtureID = 0;
    };

    class ImmutableStaticSource final
    {
    public:
        ImmutableStaticSource(const ImmutableStaticSource&) = delete;
        ImmutableStaticSource& operator=(const ImmutableStaticSource&) = delete;
        std::span<const Vertex> Vertices() const noexcept { return {m_vertices.get(), m_vertexCount}; }
        std::span<const std::uint32_t> Indices() const noexcept { return {m_indices.get(), m_indexCount}; }
        const FixtureCertificate& Certificate() const noexcept { return m_certificate; }
        std::size_t SourceBytes() const noexcept { return m_vertexCount * sizeof(Vertex) + m_indexCount * sizeof(std::uint32_t); }

        // Output atomic. Caller storage must remain readable/stable during copying.
        // Only the owned copy is validated; no mutable aliases escape.
        static AdmissionResult Prepare(const StaticSourceRequest& request,
            std::shared_ptr<const ImmutableStaticSource>& output) noexcept
        {
            if(request.topology != Topology::IndexedUInt32Triangles)
                return {request.topology == Topology::NonIndexedTriangles ? AdmissionStatus::Unsupported : AdmissionStatus::InvalidInput};
            if(request.vertices.empty() || request.indices.empty() || request.vertices.size() > MaximumElements ||
                request.indices.size() > MaximumElements || request.indices.size() % 3 != 0)
                return {AdmissionStatus::InvalidInput};
            try
            {
                auto source = std::shared_ptr<ImmutableStaticSource>(new ImmutableStaticSource);
                source->m_vertexCount = request.vertices.size();
                source->m_indexCount = request.indices.size();
                source->m_vertices = std::make_unique<Vertex[]>(source->m_vertexCount);
                source->m_indices = std::make_unique<std::uint32_t[]>(source->m_indexCount);
                std::memcpy(source->m_vertices.get(), request.vertices.data(), request.vertices.size_bytes());
                std::memcpy(source->m_indices.get(), request.indices.data(), request.indices.size_bytes());
                // The whole uploaded index array must be safe, even outside the certified domain.
                for(const auto index : source->Indices())
                    if(index >= source->m_vertexCount) return {AdmissionStatus::InvalidInput};
                // Ordinary Mesh bounds inspect all positions. Excluded triangles are not certified.
                for(const auto& vertex : source->Vertices())
                    for(const auto position : vertex.position)
                        if(!std::isfinite(position)) return {AdmissionStatus::InvalidInput};
                FixtureInput fixture{source->Vertices(), source->Indices(), request.topology,
                    request.firstPrimitive, request.primitiveCount, {}, request.policy, request.fixtureID};
                const auto result = BuildFixtureCertificate(fixture, source->m_certificate);
                if(result.status != Status::Success)
                    return {result.status == Status::Unsupported ? AdmissionStatus::Unsupported : AdmissionStatus::InvalidInput, result};
                output = std::move(source);
                return {AdmissionStatus::Ready};
            }
            catch(const std::bad_alloc&) { return {AdmissionStatus::AllocationFailed}; }
        }
    private:
        ImmutableStaticSource() = default;
        std::unique_ptr<Vertex[]> m_vertices;
        std::unique_ptr<std::uint32_t[]> m_indices;
        std::size_t m_vertexCount = 0, m_indexCount = 0;
        FixtureCertificate m_certificate{};
    };

    class MeshSourceGeneration final
    {
    public:
        MeshSourceGeneration(const MeshSourceGeneration&) = delete;
        MeshSourceGeneration& operator=(const MeshSourceGeneration&) = delete;
        const ImmutableStaticSource& Source() const noexcept { return *m_source; }
        // Borrowed resources for explicitly owned diagnostic work only. No descriptor lease.
        // No external writes are permitted; COM retention does not enforce content immutability.
        ID3D12Resource* VertexResource() const noexcept { return m_vertices.Get(); }
        ID3D12Resource* IndexResource() const noexcept { return m_indices.Get(); }
        const D3D12_RESOURCE_DESC& VertexDescription() const noexcept { return m_vertexDescription; }
        const D3D12_RESOURCE_DESC& IndexDescription() const noexcept { return m_indexDescription; }
        std::size_t VertexBytes() const noexcept { return m_source->Vertices().size_bytes(); }
        std::size_t IndexBytes() const noexcept { return m_source->Indices().size_bytes(); }
        UINT VertexStride() const noexcept { return sizeof(Vertex); }
        DXGI_FORMAT IndexFormat() const noexcept { return DXGI_FORMAT_R32_UINT; }
        const std::array<unsigned int, 4>& AttributeOffsets() const noexcept { return m_offsets; }
        std::uint64_t AllocatedResourceBytes() const noexcept { return m_allocatedBytes; }
        std::size_t BookkeepingBytes() const noexcept { return sizeof(*this) + sizeof(ImmutableStaticSource); }
        // Standard shared_ptr control blocks/allocator/driver overhead are not measured.
    private:
        friend class ::Mesh;
        MeshSourceGeneration(std::shared_ptr<const ImmutableStaticSource> source, ID3D12Device* device,
            ID3D12Resource* vertices, ID3D12Resource* indices)
            : m_source(std::move(source)), m_vertices(vertices), m_indices(indices),
              m_vertexDescription(vertices->GetDesc()), m_indexDescription(indices->GetDesc())
        {
            const auto vertexInfo = device->GetResourceAllocationInfo(0, 1, &m_vertexDescription);
            const auto indexInfo = device->GetResourceAllocationInfo(0, 1, &m_indexDescription);
            m_allocatedBytes = vertexInfo.SizeInBytes == UINT64_MAX || indexInfo.SizeInBytes == UINT64_MAX ||
                vertexInfo.SizeInBytes > UINT64_MAX - indexInfo.SizeInBytes ? UINT64_MAX :
                vertexInfo.SizeInBytes + indexInfo.SizeInBytes;
        }
        std::shared_ptr<const ImmutableStaticSource> m_source;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_vertices, m_indices;
        D3D12_RESOURCE_DESC m_vertexDescription{}, m_indexDescription{};
        std::uint64_t m_allocatedBytes = 0;
        const std::array<unsigned int, 4> m_offsets{0, 12, 24, 32}; // exact Vertex ABI
    };
    using GeometrySourceHandle = std::shared_ptr<const MeshSourceGeneration>;
}
