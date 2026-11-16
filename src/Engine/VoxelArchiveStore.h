#pragma once

// R1.4a: a scene's voxel bake read from a game archive. See Documentation/Runtime/RuntimeVX5ResidencyR1_4a.md.
//
// Build Game cooks every voxel asset of a bake (the .voxelmanifest and each populated .voxelregion) as an EngineAssetPak Resource
// entry, unchanged, under the AssetID of its project asset. A reference "asset:<uuid>" therefore names the same bytes in the project
// and in the archive, and nothing in the bake is rewritten. The archive adds a SHA-256 per entry, checked on every read; the voxel
// formats' own checks run in whoever decodes the bytes, as they do for project assets.
//
// Read-only and immutable: Stage and Publish fail, Discard does nothing. Thread-safe: the archive reader serializes its reads.

#include "Voxel/VoxelAssetStore.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace CookedMeshSource
{
class ArchiveProvider;
}

namespace VoxelScene
{
// The archive entry payload version of every voxel asset: the asset's own bytes, which carry their own format version.
inline constexpr std::uint32_t ArchivedAssetPayloadVersion = 1;

class ArchiveAssetStore final : public Voxel::AssetStore
{
public:
    // `archive` stays pinned for the store's lifetime. `currentManifest` is the cooked scene's manifest reference (empty when it has
    // none).
    ArchiveAssetStore(std::shared_ptr<const CookedMeshSource::ArchiveProvider> archive, std::string currentManifest);

    std::optional<std::string> GetCurrentManifest() const override;
    std::expected<std::vector<std::byte>, std::string> Read(std::string_view reference) const override;
    std::expected<std::string, std::string> Stage(std::span<const std::byte> bytes) override;
    std::expected<void, std::string> Publish(std::span<const std::string> staged, std::string_view manifest) override;
    void Discard(std::span<const std::string> staged) noexcept override;

private:
    std::shared_ptr<const CookedMeshSource::ArchiveProvider> m_archive;
    std::optional<std::string> m_current;
};
}
