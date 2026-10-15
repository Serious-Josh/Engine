#pragma once

#include "assets/ProjectMeshCPUCache.h"
#include "assets/ProjectAssetResolver.h"
#include "assets/ModelImporter.h"
#include "assets/DevelopmentCookedMeshProvider.h"

#include <atomic>
#include <memory>
#include <optional>

// Owner-thread API; one worker, CPU inputs only. Completions never authorize GPU
// upload or residency. Consumers must repeat source and scene-binding validation.
class ProjectMeshPreparationService
{
public:
    struct Limits
    {
        size_t requests = 16; // Includes delivered, consumer-held cancellation tokens.
        size_t pending = 4;
        size_t completed = 4;
        size_t sourceBytes = 32 * 1024 * 1024;
        size_t geometryBytes = 128 * 1024 * 1024;
        size_t preparationBytes = 384 * 1024 * 1024; // Source, importer estimates and flattening copies.
        size_t retainedBytes = 1024 * 1024 * 1024;
    };

    enum class Status
    {
        Accepted, Available, Pending, Retry, Invalid, Failed, Stale, Canceled,
        Missing, Unsupported, IncompatibleCook, Corrupt, IOFailure, AdmissionLimit,
        AllocationFailure, IdentityFailure
    };

    enum class PreparationMode { Geometry, MetadataOnly };

    struct CookedRequest
    {
        std::shared_ptr<const DevelopmentCookedMesh::Provider> provider;
        AssetID asset;
        std::uint64_t generation = 0;
        PreparationMode mode = PreparationMode::Geometry;
    };

    struct Identity
    {
        MeshPreparationSourceKind sourceKind = MeshPreparationSourceKind::FBX;
        PreparationMode mode = PreparationMode::Geometry;
        std::shared_ptr<const DevelopmentCookedMesh::AssetEvidence> cooked;

        std::filesystem::path project;
        std::filesystem::path path;
        std::string assetID;
        std::string sourceIdentity; // File ID, size and last-write time captured under a read pin.
        std::string digest; // Filled on the worker, never hashed on the owner thread.
        std::uint32_t importerRevision = ModelImporter::kImportRevision;
        MeshUVOrientation uvPolicy = MeshUVOrientation::FlipV;
        bool operator==(const Identity&) const = default;
        ProjectMeshCPUCache::Key CacheKey() const;
    };

    struct RequestResult
    {
        Status status = Status::Invalid;
        std::uint64_t ticket = 0;
        std::string error;
    };

    struct Completion
    {
        Status status = Status::Pending;
        Identity identity;
        std::uint64_t ticket = 0;
        std::uint64_t generation = 0;
        std::shared_ptr<const ImportedMesh> geometry;
        std::shared_ptr<const CookedMesh::MetadataSnapshot> metadata;
        std::shared_ptr<const DevelopmentCookedMesh::Provider> provider;
        std::shared_ptr<const std::atomic_bool> current;
        std::string error;
        bool cacheHit = false;
    };

    ProjectMeshPreparationService();
    ~ProjectMeshPreparationService();
    ProjectMeshPreparationService(const ProjectMeshPreparationService&) = delete;
    ProjectMeshPreparationService& operator=(const ProjectMeshPreparationService&) = delete;

    bool Start(const Limits& limits, std::string& error);
    void Shutdown(); // Cancels subscribers, then joins any noninterruptible Assimp import.
    std::uint64_t Generation() const;
    void Invalidate();
    void Cancel(std::uint64_t ticket);
    RequestResult Request(const ProjectPaths& paths, const ProjectAssetRegistry& registry,
        const AssetID& asset, std::uint64_t generation);
    // Development/diagnostic selection only. Same worker/queues/cache; no FBX fallback.
    RequestResult RequestCooked(const CookedRequest& request, ProjectMeshCPUCache& cache);
    Completion PollCooked(std::uint64_t ticket);
    Status ValidateCookedCompletion(const Completion& completion, std::string& error) const;
    // Also advances the hash -> owner-thread cache lookup -> import handoff.
    // No hashing, importing or GPU work occurs here. Poll Pending tickets again.
    Completion Poll(std::uint64_t ticket, const ProjectPaths& paths,
        const ProjectAssetRegistry& registry, ProjectMeshCPUCache& cache);
    bool IsCurrent(const Completion& completion) const;
    // Optional owner-thread cache integration, after successful Poll validation.
    // Caller must still perform final source validation before any future GPU use.
    bool IntegrateCache(Completion& completion, ProjectMeshCPUCache& cache, std::string& error);
    size_t RetainedBytes() const;
#ifndef NDEBUG
    // Read-only phase observation for deterministic lifecycle diagnostics.
    enum class DiagnosticStage { Missing, Queued, Running, Completed };
    DiagnosticStage ObserveDiagnosticStage(std::uint64_t ticket) const;
#endif
    static std::string InspectSourceIdentity(const std::filesystem::path& path);

private:
    struct State;
    std::unique_ptr<State> m_state;
};
