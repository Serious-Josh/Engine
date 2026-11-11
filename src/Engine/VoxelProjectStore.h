#pragma once

// VX.4B: voxel assets as project assets: Content/Voxels/<uuid>.voxelregion and <uuid>.voxelmanifest, registered in the project
// asset registry. See Documentation/Voxel/VoxelArchitecture.md, section 6.7.
//
// Crash safety, without a multi-file transaction the registry does not have:
// * Stage (any thread) writes the asset to its final file, through a temporary and a rename, but does not register it. An
//   unregistered file is invisible: Read resolves registered assets only.
// * Publish (main thread) registers every staged asset in one registry save. The save is atomic (temporary + MoveFileExW), so the
//   registry either gains all of them or none.
// * The scene's manifest reference is the switch that makes a bake current, and the scene file is written atomically too.
// A crash at any point therefore leaves the scene pointing at a complete previous bake; at worst it leaves unregistered files or
// registered but unreferenced assets, which cleanup removes (VX.6). Assets are immutable: nothing is ever rewritten in place.
//
// Asset references are registry UUIDs, generated per Stage: equal content baked twice gets different references, so compare bakes
// by WorldBakeResult::contentHash, never by manifest bytes.

#include "Voxel/VoxelAssetStore.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>

class ProjectPaths;
class ProjectAssetRegistry;

namespace VoxelScene
{
class ProjectAssetStore final : public Voxel::AssetStore
{
public:
    // Main thread. Captures where every registered voxel asset lives, so Read never touches the live registry from a worker.
    // `currentManifest` is the scene's manifest reference (empty when it has none). Both references must outlive the store.
    ProjectAssetStore(const ProjectPaths& paths, ProjectAssetRegistry& registry, std::string currentManifest);
    // Read-only: Read and GetCurrentManifest only; Stage and Publish fail.
    ProjectAssetStore(const ProjectPaths& paths, const ProjectAssetRegistry& registry, std::string currentManifest);
    ~ProjectAssetStore() override;

    std::optional<std::string> GetCurrentManifest() const override;
    std::expected<std::vector<std::byte>, std::string> Read(std::string_view reference) const override;
    std::expected<std::string, std::string> Stage(std::span<const std::byte> bytes) override;
    // Main thread: mutates the registry.
    std::expected<void, std::string> Publish(std::span<const std::string> staged, std::string_view manifest) override;
    void Discard(std::span<const std::string> staged) noexcept override;

    std::size_t GetStagedCount() const;

private:
    struct StagedAsset
    {
        std::filesystem::path relativePath;
        bool manifest = false;
    };

    const ProjectPaths& m_paths;
    // Null for a read-only store.
    ProjectAssetRegistry* m_registry = nullptr;
    mutable std::mutex m_mutex;
    // Registered voxel assets: reference -> absolute path.
    std::map<std::string, std::filesystem::path, std::less<>> m_published;
    std::map<std::string, StagedAsset, std::less<>> m_staged;
    std::optional<std::string> m_current;
};
}
