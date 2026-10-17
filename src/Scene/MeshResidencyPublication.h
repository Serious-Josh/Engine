#pragma once

#include "Renderer/RayTracing/RayTracingSceneResources.h"
#include "Scene/MeshResidency.h"

#include <cstdint>
#include <string>
#include <optional>
#include <thread>

class Renderer;
class FileSceneRuntime;
class FileSceneAssetCatalog;

// Engine thread only, after editor/asset mutations and before Renderer::Render
// calls BeginTrackedRecording. Acceptance never authorizes later-frame adoption.
class MeshResidencyPublication
{
public:
    enum class Status { Ready, Retry, Stale, Unsupported, Failed };

    struct Readiness
    {
        Status status = Status::Stale;
        std::string message;

        // Observed evidence only. No API accepts this as commit authorization.
        std::uint64_t runtimeIdentity = 0;
        std::uint64_t rendererLifetime = 0;
        std::uint64_t materialRevision = 0;

        std::uint64_t candidateFence = 0;
        std::uint64_t fenceLifetime = 0;
    };

    struct AcceptanceResult
    {
        Status status = Status::Retry;
        std::string message;
        bool published = false;
    };

    MeshResidencyPublication() = default;
    MeshResidencyPublication(const MeshResidencyPublication&) = delete;
    MeshResidencyPublication& operator=(const MeshResidencyPublication&) = delete;

    // Main-thread only. Creates the matching DXR proposal from the validated
    // scene preparation, rather than accepting an unrelated membership handle.
    bool Prepare(Renderer& renderer, const FileSceneRuntime& runtime,
        FileSceneAssetCatalog& catalog, const MeshResidencyToken& sceneToken, std::string& error);
    void Cancel() noexcept;
    bool HasCandidate() const noexcept { return m_candidate.has_value(); }

    // Read-only gate for diagnostic synchronous geometry import/upload, using
    // the same scene/recording safety requirements as pipeline preparation.
    static bool CanPrepareGeometry(const Renderer& renderer, const FileSceneRuntime& runtime) noexcept;

    // Explicit preparation only; validates the arriving mesh under a source pin.
    // Cancellation is safe immediately: candidate pipelines are never recorded.
    static bool PrepareSelectionMask(Renderer& renderer, const FileSceneRuntime& runtime,
        FileSceneAssetCatalog& catalog, const MeshResidencyToken& sceneToken, std::string& error);
    static void CancelSelectionMask(Renderer& renderer) noexcept;

    // Engine-thread observation, not a pure/read-only predicate: it may reserve
    // adoption storage. Ready applies only now; no source lock escapes this call.
    // Transform refresh, when needed, submits candidate-only work and returns
    // Retry. A future commit must repeat all checks after GPU completion.
    static Readiness Check(Renderer& renderer, const FileSceneRuntime& runtime,
        FileSceneAssetCatalog& catalog, const MeshResidencyToken& sceneToken,
        const RayTracingSceneResources::ReplacementCandidate& candidate, unsigned int frameSlot);

private:
    friend class Engine;
    // Engine's immediate pre-Render boundary only. Validates and commits with a
    // local source pin; no retained receipt authorizes later-frame publication.
    AcceptanceResult PublishAtBoundary(Renderer& renderer, FileSceneRuntime& runtime,
        FileSceneAssetCatalog& catalog);

    // Shared sequence, deliberately rerun at final acceptance. Optional outputs
    // retain the source pin and staged evidence only for the immediate commit.
    static Readiness CheckPrepared(Renderer& renderer, const FileSceneRuntime& runtime,
        FileSceneAssetCatalog& catalog, const MeshResidencyToken& sceneToken,
        const RayTracingSceneResources::ReplacementCandidate& candidate, unsigned int frameSlot,
        std::optional<MeshResidencySourceLock>* sourcePin,
        RayTracingSceneResources::AdoptionEvidence* adoptionEvidence);
    void ApplyPreparedPublication(Renderer& renderer, FileSceneRuntime& runtime,
        const RayTracingSceneResources::AdoptionEvidence& evidence) noexcept;

    const std::thread::id m_thread = std::this_thread::get_id();
    std::optional<RayTracingSceneResources::ReplacementCandidate> m_candidate;
    MeshResidencyToken m_sceneToken;
    std::uint64_t m_rendererLifetime = 0;

    // ApplyPreparedPublication is the only commit path. All fallible work is
    // completed by CheckPrepared, whose source pin survives through the swaps.
};
