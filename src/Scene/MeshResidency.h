#pragma once

#include "assets/ProjectMeshCPUCache.h"
#include "assets/CookedMeshProvider.h"

#include <cstdint>
#include <utility>
#include <vector>

class Mesh;
class FileSceneAssetCatalog;

// CPU/source lifetime evidence only. This is not a final publication pin: the
// provider's currentness check cannot exclude a concurrent activation replacement.
struct CookedMeshResidencyEvidence
{
    std::shared_ptr<const CookedMeshSource::Provider> provider;
    std::shared_ptr<const CookedMeshSource::AssetEvidence> asset;
    std::shared_ptr<const CookedMesh::MetadataSnapshot> metadata;
    std::shared_ptr<const std::atomic_bool> current;
};

bool ValidateCookedMeshResidencyEvidence(const CookedMeshResidencyEvidence& evidence,
    std::string_view reference, std::string* error = nullptr);

// Copied evidence only. The catalog, not this receipt, owns the GPU mesh.
struct MeshResidencyCandidate
{
    ProjectMeshCPUCache::Key key;
    Mesh* mesh = nullptr;

    std::uint64_t catalogIdentity = 0;
    std::uint64_t vertexAddress = 0;
    std::uint64_t indexAddress = 0;

    enum class SourceKind { ProjectAsset, Builtin, Cooked };
    SourceKind sourceKind = SourceKind::ProjectAsset;
    std::string builtinReference; // Empty for external assets; never a CPU cache key.
    std::shared_ptr<const CookedMeshResidencyEvidence> cooked;

};

// Identifies one staged batch transaction. Per-object binding revisions are
// held and revalidated by each staged entry, not by the token.
struct MeshResidencyToken
{
    std::uint64_t runtimeIdentity = 0;
    std::uint64_t transaction = 0;
    std::uint64_t entryCount = 0;
    bool operator==(const MeshResidencyToken&) const = default;
};

// Pins FBX bytes against writes/deletion through acceptance. Built-in evidence
// has no OS handle. Cooked acceptance pins the current activation and manifest,
// and retains the exact immutable mapping used to prepare catalog geometry.
// Does not own GPU resources or authorize renderer publication.
class MeshResidencySourceLock
{
public:
    MeshResidencySourceLock() = default;
    ~MeshResidencySourceLock();
    MeshResidencySourceLock(const MeshResidencySourceLock&) = delete;
    MeshResidencySourceLock& operator=(const MeshResidencySourceLock&) = delete;
    MeshResidencySourceLock(MeshResidencySourceLock&& other) noexcept
        : m_handle(std::exchange(other.m_handle, nullptr)),
          m_builtinValidated(std::exchange(other.m_builtinValidated, false)),
          m_cookedPreparation(std::move(other.m_cookedPreparation)),
          m_cookedAcceptance(std::move(other.m_cookedAcceptance)) {}
    explicit operator bool() const noexcept
    {
        return m_handle != nullptr || m_builtinValidated || (m_cookedPreparation && m_cookedAcceptance);
    }

private:
    friend class FileSceneAssetCatalog;
    explicit MeshResidencySourceLock(void* handle) : m_handle(handle) {}

    void* m_handle = nullptr;
    bool m_builtinValidated = false;
    std::shared_ptr<const CookedMeshResidencyEvidence> m_cookedPreparation;
    std::shared_ptr<const void> m_cookedAcceptance;
};

// Final-acceptance pins for a batch: one per unique source mesh, each acquired
// during validation and held by the coordinator until its commit returns.
struct MeshResidencySourcePins
{
    std::vector<MeshResidencySourceLock> locks;

    explicit operator bool() const noexcept
    {
        return !locks.empty();
    }
};
