#pragma once
#include "assets/CookedMeshProvider.h"
#include "assets/EngineAssetPak.h"

namespace CookedMeshSource
{
    struct ArchivePolicy
    {
        EngineAssetPak::Limits archive;
        CookedMesh::Policy mesh;
    };
    class ArchiveProvider;
    struct ArchiveOpenResult : Outcome { std::shared_ptr<const ArchiveProvider> provider; };

    // Explicit opt-in source. Pins this immutable pair until the last provider,
    // payload or publication acceptance lease releases it. Never opens loose meshes.
    class ArchiveProvider final : public Provider
    {
    public:
        static ArchiveOpenResult Open(const std::filesystem::path& projectRoot,
            const std::filesystem::path& toc, const std::filesystem::path& payload, const ArchivePolicy& policy = {});
        // Packaged source: no project root/registry or development path binding.
        static ArchiveOpenResult OpenArchive(const std::filesystem::path& toc, const std::filesystem::path& payload, const ArchivePolicy& policy = {});
        const EngineAssetPak::Entry* ArchiveEntry(const AssetID& id) const noexcept;
        std::vector<std::byte> ReadEntry(const AssetID& id, EngineAssetPak::Type type, std::uint32_t version, size_t maxBytes) const;
        const MappingEvidence& Mapping() const noexcept override;
        size_t EntryCount() const noexcept override;
        // Metadata-only enumeration; includes non-mesh entries, which Lookup rejects.
        std::span<const EngineAssetPak::Entry> Entries() const noexcept;
        LookupResult Lookup(const AssetID& asset) const override;
        PayloadResult Acquire(const AssetID& asset) const override;
        PayloadResult Acquire(const AssetID& asset, const CookedMesh::Policy& admission) const override;
        MetadataResult ReadMetadata(const AssetID& asset) const override;
        Outcome CheckCurrent() const override;
        AcceptanceResult PinAcceptance(const AssetEvidence& expected) const override;
    private:
        struct State;
        explicit ArchiveProvider(std::shared_ptr<const State> state) : m_state(std::move(state)) {}
        std::shared_ptr<const State> m_state;
    };
}
namespace ArchiveCookedMesh
{
    using Provider = CookedMeshSource::ArchiveProvider;
    using Policy = CookedMeshSource::ArchivePolicy;
    using OpenResult = CookedMeshSource::ArchiveOpenResult;
}
