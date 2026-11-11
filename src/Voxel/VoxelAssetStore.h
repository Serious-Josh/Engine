#pragma once

// VX.4A: where a world bake reads the previous bake and writes the new one. See Documentation/Voxel/VoxelArchitecture.md, section 6.
//
// Publication is a single switch: new assets are staged (written, but invisible), and Publish makes all of them visible and the
// new manifest current in one step, or does nothing. Readers only ever see the current manifest and the published assets it
// references, so no partially completed bake is ever exposed. Stored assets are immutable; removing unreferenced ones is
// cleanup's job (VX.6), never the bake's.
//
// The Engine project asset registry will implement this interface in VX.6. MemoryAssetStore is the reference implementation used by
// the diagnostics. Implementations must be thread-safe: bake workers stage concurrently.

#include <cstddef>
#include <expected>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Voxel
{
class AssetStore
{
public:
    // No voxel asset may be larger: the region format's cap, which is also the manifest's (VoxelRegion.h, VoxelManifest.h). Stage
    // refuses anything larger, so an oversized asset can never enter a publication.
    static constexpr std::size_t MaximumAssetBytes = std::size_t{512} << 20;

    virtual ~AssetStore() = default;

    // The reference of the currently published manifest, if any.
    virtual std::optional<std::string> GetCurrentManifest() const = 0;

    // A published asset's bytes. Staged assets are not readable.
    virtual std::expected<std::vector<std::byte>, std::string> Read(std::string_view reference) const = 0;

    // Writes an asset nothing can see yet and returns its reference. Fails for more than MaximumAssetBytes.
    virtual std::expected<std::string, std::string> Stage(std::span<const std::byte> bytes) = 0;

    // Publishes the staged assets and makes `manifest` (staged or published) current: all or nothing.
    virtual std::expected<void, std::string> Publish(std::span<const std::string> staged, std::string_view manifest) = 0;

    // Deletes staged assets that will never be published. Published assets are never touched.
    virtual void Discard(std::span<const std::string> staged) noexcept = 0;
};

// Content-addressed: an asset's reference is "voxel:<FNV-1a 64 of its bytes, hex>-<size>", so the same bytes always get the same
// reference, whichever thread or bake order produced them. Staging bytes whose reference is already published with different
// bytes (a damaged copy) stages the correct bytes, and publishing them repairs the copy.
class MemoryAssetStore : public AssetStore
{
public:
    std::optional<std::string> GetCurrentManifest() const override;
    std::expected<std::vector<std::byte>, std::string> Read(std::string_view reference) const override;
    std::expected<std::string, std::string> Stage(std::span<const std::byte> bytes) override;
    std::expected<void, std::string> Publish(std::span<const std::string> staged, std::string_view manifest) override;
    void Discard(std::span<const std::string> staged) noexcept override;

    static std::string ReferenceFor(std::span<const std::byte> bytes);

    // Diagnostics: damage or remove a published asset, and count what is stored.
    void ReplacePublished(const std::string& reference, std::vector<std::byte> bytes);
    void RemovePublished(const std::string& reference);
    std::size_t GetPublishedCount() const;
    std::size_t GetStagedCount() const;

private:
    mutable std::mutex m_mutex;
    std::map<std::string, std::vector<std::byte>, std::less<>> m_published;
    std::map<std::string, std::vector<std::byte>, std::less<>> m_staged;
    std::optional<std::string> m_current;
};
}
