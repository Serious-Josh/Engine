#pragma once

#include "assets/AssetID.h"
#include "assets/AssetRecord.h"
#include "assets/CookedMesh.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace DevelopmentCookedMesh { class Provider; }

namespace CookedMeshSource
{
    enum class Status
    {
        Success, Missing, Stale, Corrupt, Unsupported, IncompatibleCook,
        IOFailure, AdmissionLimit, AllocationFailure, IdentityFailure
    };

    struct Outcome
    {
        Status status = Status::Success;
        std::string_view message; // Static storage, including allocation failures.
    };

    struct MappingEvidence
    {
        std::string providerNamespace; // Canonical source namespace (Cooked directory or archive TOC).
        std::string publishedGeneration; // Opaque publication identity, not an AssetID.
        std::string manifestSHA256; // Manifest or archive TOC identity.
        std::uint64_t mappingGeneration = 0; // Process-unique snapshot token, never reused.

        std::string projectNamespace; // Canonical explicit project binding.

        bool operator==(const MappingEvidence&) const = default;
    };

    struct AssetEvidence
    {
        AssetID assetID;
        AssetType assetType = AssetType::Mesh;
        CookedMesh::Evidence payload;
        MappingEvidence mapping;

        std::string recipeIdentity;
        std::string sourceSHA256; // Provenance only; never payload or runtime identity.
        std::string sourcePath; // Provenance only; never resolved/read by this provider.
        std::uint64_t contentGeneration = 0; // Archive entry generation; zero for development manifests (mapping identity carries their generation).

        bool operator==(const AssetEvidence&) const = default;
    };

    class ArchiveProvider;

    class PayloadLease
    {
    public:
        std::span<const std::byte> Bytes() const noexcept { return m_bytes; }
        const AssetEvidence& Evidence() const noexcept { return *m_evidence; }
        const std::shared_ptr<const CookedMesh::MetadataSnapshot>& Metadata() const noexcept { return m_metadata; }

    private:
        friend class ArchiveProvider;
        friend class DevelopmentCookedMesh::Provider;
        PayloadLease() = default;
        std::shared_ptr<const void> m_source; // Optional immutable source/OS pins.
        std::vector<std::byte> m_bytes;
        std::shared_ptr<const AssetEvidence> m_evidence;
        std::shared_ptr<const CookedMesh::MetadataSnapshot> m_metadata;
    };

    struct AcceptanceResult : Outcome
    {
        std::shared_ptr<const void> lease; // Immutable mapping and OS pins.
    };

    struct LookupResult : Outcome
    {
        std::shared_ptr<const AssetEvidence> evidence;
    };

    struct PayloadResult : Outcome
    {
        std::shared_ptr<const PayloadLease> lease;
    };

    struct MetadataResult : Outcome
    {
        std::shared_ptr<const CookedMesh::MetadataSnapshot> metadata;
        std::shared_ptr<const AssetEvidence> evidence;
    };

    // CPU-only source contract shared by preparation and checked residency.
    class Provider
    {
    public:
        virtual ~Provider() = default;
        virtual const MappingEvidence& Mapping() const noexcept = 0;
        virtual size_t EntryCount() const noexcept = 0;
        virtual LookupResult Lookup(const AssetID&) const = 0;
        virtual PayloadResult Acquire(const AssetID&) const = 0;
        virtual PayloadResult Acquire(const AssetID&, const CookedMesh::Policy&) const = 0;
        virtual MetadataResult ReadMetadata(const AssetID&) const = 0;
        virtual Outcome CheckCurrent() const = 0;
        virtual AcceptanceResult PinAcceptance(const AssetEvidence&) const = 0;
    };
    // Process-unique across both implementations; zero means exhaustion.
    std::uint64_t NextMappingGeneration() noexcept;
}
