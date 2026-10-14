#pragma once

#include "assets/ProjectMeshCPUCache.h"

#include <cstdint>
#include <utility>

class Mesh;
class FileSceneAssetCatalog;

// Copied evidence only. The catalog, not this receipt, owns the GPU mesh.
struct MeshResidencyCandidate
{
    ProjectMeshCPUCache::Key key;
    Mesh* mesh = nullptr;

    std::uint64_t catalogIdentity = 0;
    std::uint64_t vertexAddress = 0;
    std::uint64_t indexAddress = 0;

    enum class SourceKind { ProjectAsset, Builtin };
    SourceKind sourceKind = SourceKind::ProjectAsset;
    std::string builtinReference; // Empty for external assets; never a CPU cache key.

};

struct MeshResidencyToken
{
    std::uint64_t runtimeIdentity = 0;
    std::uint64_t transaction = 0;
    std::uint64_t bindingRevision = 0;
    bool operator==(const MeshResidencyToken&) const = default;
};

// Pins external source bytes against writes/deletion through acceptance. Built-in
// evidence has no OS handle: catalog/session ownership is rechecked at acceptance.
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
          m_builtinValidated(std::exchange(other.m_builtinValidated, false)) {}
    explicit operator bool() const noexcept { return m_handle != nullptr || m_builtinValidated; }

private:
    friend class FileSceneAssetCatalog;
    explicit MeshResidencySourceLock(void* handle) : m_handle(handle) {}

    void* m_handle = nullptr;
    bool m_builtinValidated = false;
};
