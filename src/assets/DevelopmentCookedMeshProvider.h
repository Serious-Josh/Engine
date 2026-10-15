#pragma once

#include "assets/AssetID.h"
#include "assets/AssetRecord.h"
#include "assets/CookedMesh.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace DevelopmentCookedMesh
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

    struct Policy
    {
        size_t maxActivationBytes = 4096;
        size_t maxManifestBytes = 16 * 1024 * 1024;
        size_t maxEntries = 4096;
        CookedMesh::Policy mesh;
    };

    struct MappingEvidence
    {
        std::string providerNamespace; // Canonical development Cooked directory.
        std::string publishedGeneration; // Opaque publication identity, not an AssetID.
        std::string manifestSHA256;
        std::uint64_t mappingGeneration = 0; // Process-unique snapshot token, never reused.

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
        bool operator==(const AssetEvidence&) const = default;
    };

    class PayloadLease
    {
    public:
        std::span<const std::byte> Bytes() const noexcept { return m_bytes; }
        const AssetEvidence& Evidence() const noexcept { return *m_evidence; }
        const std::shared_ptr<const CookedMesh::MetadataSnapshot>& Metadata() const noexcept { return m_metadata; }

    private:
        friend class Provider;
        PayloadLease() = default;
        std::vector<std::byte> m_bytes;
        std::shared_ptr<const AssetEvidence> m_evidence;
        std::shared_ptr<const CookedMesh::MetadataSnapshot> m_metadata;
    };

    class Provider;

    struct AcceptanceResult : Outcome
    {
        std::shared_ptr<const void> lease; // Immutable mapping and OS pins.
    };

    struct OpenResult : Outcome
    {
        std::shared_ptr<const Provider> provider;
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

    // Read-only immutable mapping. No importer, registry, cache, scene or GPU work.
    class Provider
    {
    public:
        static OpenResult Open(const std::filesystem::path& projectRoot, const Policy& policy);
        const MappingEvidence& Mapping() const noexcept;
        size_t EntryCount() const noexcept;

        LookupResult Lookup(const AssetID& asset) const;
        PayloadResult Acquire(const AssetID& asset) const;
        // Per-consumer admission may tighten, never widen the snapshot policy.
        PayloadResult Acquire(const AssetID& asset, const CookedMesh::Policy& admission) const;
        MetadataResult ReadMetadata(const AssetID& asset) const;
        // Explicit check only; never changes this mapping or revokes leases.
        Outcome CheckCurrent() const;
        AcceptanceResult PinAcceptance(const AssetEvidence& expected) const;

    private:
        struct State;
        explicit Provider(std::shared_ptr<const State> state) : m_state(std::move(state)) {}
        std::shared_ptr<const State> m_state;
    };
}
