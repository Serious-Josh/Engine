#pragma once
#include "assets/CookedMeshProvider.h"

namespace DevelopmentCookedMesh
{
    using CookedMeshSource::Status;
    using CookedMeshSource::Outcome;
    using CookedMeshSource::MappingEvidence;
    using CookedMeshSource::AssetEvidence;
    using CookedMeshSource::PayloadLease;
    using CookedMeshSource::AcceptanceResult;
    using CookedMeshSource::LookupResult;
    using CookedMeshSource::PayloadResult;
    using CookedMeshSource::MetadataResult;

    struct Policy
    {
        size_t maxActivationBytes = 4096;
        size_t maxManifestBytes = 16 * 1024 * 1024;
        size_t maxEntries = 4096;
        CookedMesh::Policy mesh;
    };
    class Provider;
    struct OpenResult : Outcome { std::shared_ptr<const Provider> provider; };
    // Read-only immutable mapping. No importer, registry, cache, scene or GPU work.
    class Provider final : public CookedMeshSource::Provider
    {
    public:
        static OpenResult Open(const std::filesystem::path& projectRoot, const Policy& policy);
        const MappingEvidence& Mapping() const noexcept override;
        size_t EntryCount() const noexcept override;

        LookupResult Lookup(const AssetID& asset) const override;
        PayloadResult Acquire(const AssetID& asset) const override;
        // Per-consumer admission may tighten, never widen the snapshot policy.
        PayloadResult Acquire(const AssetID& asset, const CookedMesh::Policy& admission) const override;
        MetadataResult ReadMetadata(const AssetID& asset) const override;
        // Explicit check only; never changes this mapping or revokes leases.
        Outcome CheckCurrent() const override;
        AcceptanceResult PinAcceptance(const AssetEvidence& expected) const override;

    private:
        struct State;
        explicit Provider(std::shared_ptr<const State> state) : m_state(std::move(state)) {}
        std::shared_ptr<const State> m_state;
    };
}
