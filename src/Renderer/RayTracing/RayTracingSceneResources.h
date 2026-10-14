#pragma once

#include "Renderer/RayTracing/BottomLevelAccelerationStructure.h"
#include "Renderer/RayTracing/TopLevelAccelerationStructure.h"
#include "Renderer/StructuredBuffer.h"
#include "Renderer/RenderObject.h"
#include "assets/MeshMetadata.h"
#include "Scene/MeshResidency.h"

#include <d3d12.h>
#include <array>
#include <memory>
#include <optional>
#include <vector>
#include <unordered_map>

class Mesh;
class Scene;
class ResourceUploader;
struct RayTracingInstanceData;
class DescriptorHeap;
class CommandContext;
class Renderer;
class FileSceneRuntime;
class FileSceneAssetCatalog;

struct SceneBLASRetirementBatchIdentity
{
    std::uint64_t sceneLifetime = 0;

    BLASRetirementBatchIdentity blas{};
};

struct TLASGeometryRecordingEvidence
{
    SceneBLASRetirementBatchIdentity retirement{};

    std::uint64_t geometryVersion = 0;
    std::uint64_t recordingGeneration = 0;
    std::uint64_t tlasRecording = 0;

    D3D12_GPU_VIRTUAL_ADDRESS blasAddress = 0;
    unsigned int frameSlot = UINT_MAX;
};

class RayTracingSceneResources
{
    struct CandidateState;

    public:
    struct ResidencyProposal
    {
        std::string objectID;
        // Catalog-owned, already uploaded geometry; no import/upload here.
        MeshResidencyCandidate mesh;
    };

    class ReplacementCandidate
    {
    public:
        ReplacementCandidate() = default;
        ReplacementCandidate(const ReplacementCandidate&) = delete;
        ReplacementCandidate& operator=(const ReplacementCandidate&) = delete;
        ReplacementCandidate(ReplacementCandidate&&) noexcept = default;
        ReplacementCandidate& operator=(ReplacementCandidate&&) noexcept = default;

    private:
        friend class RayTracingSceneResources;
        friend class Renderer;
        std::uint64_t m_rendererLifetime = 0;
        std::shared_ptr<CandidateState> m_state;
    };

    enum class CandidateStatus { Pending, Ready, NeedsTransformRefresh, Stale, Failed };
    static constexpr std::size_t MaxCandidateProposals = 4;
    static constexpr std::size_t MaxCandidateObjects = 4096;
    static constexpr std::size_t MaxCandidateMetadataBytes = 16 * 1024 * 1024;
    static constexpr std::size_t MaxCandidateTableBytes = 16 * 1024 * 1024;
    static constexpr std::size_t MaxCandidateASBytes = 256 * 1024 * 1024;

    bool Initialize(
      ID3D12Device5* device,
      ID3D12GraphicsCommandList4* commandList,
      ResourceUploader& uploader,
      DescriptorHeap& descriptorHeap,
      const Scene& scene);
    bool Update(
        ID3D12GraphicsCommandList4* commandList,
        unsigned int frameIndex,
        const Scene& scene);
    // UNUSED. Synchronizes every registered instance from the current scene
    // and refreshes all BLAS addresses with one TLAS Update/upload write.
    // Caller must fence-acquire frameIndex before invocation and keep its
    // instance upload untouched until GPU completion. Queue order alone
    // does not protect CPU writes. Failure after TLAS mutation is fail-closed,
    // like Update(); no rollback or submission is performed here.
    bool UpdateTransformsAndRefreshTLASReferences(
        ID3D12GraphicsCommandList4* commandList,
        unsigned int frameIndex,
        const Scene& scene);
    bool RebuildMeshBLAS(
        ID3D12Device5* device,
        ID3D12GraphicsCommandList4* commandList,
        const Mesh* mesh);
    // Provisional counts are validated by BLAS construction, never published to Mesh.
    bool RebuildMeshBLAS(
        ID3D12Device5* device,
        ID3D12GraphicsCommandList4* commandList,
        const Mesh* mesh,
        const RayTracingGeometryCounts& counts);

    bool RefreshTLASReferences(
        ID3D12GraphicsCommandList4* commandList,
        unsigned int frameIndex);

    void ReleaseRetiredBLASResources();
    // UNUSED selective ownership path. Requires exactly one retired batch
    // across this scene, belonging to the registered expected mesh. Invalid
    // on overlap, release-all, duplicate release, shutdown/reinitialization.
    // Caller separately proves GPU completion and absence of old references.
    SceneBLASRetirementBatchIdentity GetExclusiveRetirementBatch(const Mesh* mesh) const;
    bool ValidateRetirementBatch(
        const Mesh* mesh,
        SceneBLASRetirementBatchIdentity identity) const;
    bool ReleaseRetirementBatch(
        const Mesh* mesh,
        SceneBLASRetirementBatchIdentity identity,
        bool gpuCompletionConfirmed,
        bool referencesSafe);

    // UNUSED read-only evidence. Capture only after a checked successful
    // TLAS recording, in the caller's same active generation. Version and
    // generation are caller labels, validated by Renderer, not queue proof.
    std::uint64_t GetTLASRecordingIdentity() const
    {
        return m_membership->topLevel.GetRecordingIdentity();
    }

    bool TryGetTLASGeometryEvidence(
        const Mesh* mesh,
        SceneBLASRetirementBatchIdentity batch,
        std::uint64_t geometryVersion,
        std::uint64_t recordingGeneration,
        unsigned int frameSlot,
        TLASGeometryRecordingEvidence& evidence) const;
    // Also usable when a later frame consumes the existing shared TLAS
    // without rebuilding. Any intervening TLAS recording requires recapture.
    bool ValidateTLASGeometryEvidence(
        const Mesh* mesh,
        const TLASGeometryRecordingEvidence& evidence) const;

    void Shutdown();

    const TopLevelAccelerationStructure& GetTopLevel() const;

    unsigned int GetInstanceDataDescriptorIndex() const;
    std::size_t GetInstanceCount() const;
    bool ValidateMaterialBindings(const Scene& scene) const;

    private:
    friend class Renderer;
    friend class MeshResidencyPublication;

    // Renderer-only observations cover every tracked list conservatively,
    // including builds and later rendering uses of an unchanged snapshot.
    bool BeginMembershipRecording(std::uint64_t generation);
    bool AttemptMembershipSubmission(std::uint64_t generation);
    bool ConfirmMembershipSubmission(std::uint64_t generation, const CommandContext& context);
    void MembershipNotSubmitted(std::uint64_t generation);
    void DiscardMembershipRecording(std::uint64_t generation);
    void CollectRetiredMemberships(const CommandContext& context);

    std::optional<ReplacementCandidate> PrepareReplacementCandidate(ID3D12Device5* device,
        DescriptorHeap& heap, const FileSceneRuntime& runtime, FileSceneAssetCatalog& catalog,
        const std::vector<ResidencyProposal>& proposals, std::size_t materialCount, std::string& error);
    bool RecordReplacementCandidate(ID3D12Device5* device, ID3D12GraphicsCommandList4* commands,
        std::uint64_t generation, std::string& error);
    CandidateStatus PollReplacementCandidate(const ReplacementCandidate& candidate, const CommandContext& context,
        const FileSceneRuntime& runtime, FileSceneAssetCatalog& catalog, std::string& error) const;
    bool RefreshReplacementTransforms(const ReplacementCandidate& candidate, ID3D12GraphicsCommandList4* commands,
        unsigned int frameSlot, std::uint64_t generation, const FileSceneRuntime& runtime, std::string& error);
    void CollectReplacementCandidate(const CommandContext& context);
    bool AttemptCandidateSubmission(std::uint64_t generation);
    bool ConfirmCandidateSubmission(std::uint64_t generation, const CommandContext& context);
    void DiscardCandidateRecording(std::uint64_t generation);
    void CandidateNotSubmitted(std::uint64_t generation);

    enum class AdoptionReadiness { Ready, Stale, Unsupported };
    struct AdoptionEvidence
    {
        std::uint64_t fence = 0;
        std::uint64_t fenceLifetime = 0;
        std::size_t stableBLASCount = 0;
        std::size_t newBLASCount = 0;
        std::size_t retirementSlot = 0;
    };
    std::uint64_t ReserveBLASRegistryLifetime() const;
    AdoptionReadiness PrepareCandidateAdoption(const ReplacementCandidate& candidate,
        std::string_view objectID, std::size_t objectIndex, const MeshResidencyCandidate& mesh,
        Material* fallback, const std::map<std::string, Material*>& slots, std::size_t materialCount,
        AdoptionEvidence& evidence, std::string& error);

    // Coordinator only, immediately after fresh readiness with a retained source
    // pin and no intervening mutation/recording. All storage is already reserved.
    void AdoptPreparedCandidate(const ReplacementCandidate& candidate, const AdoptionEvidence& evidence) noexcept;

    bool InitializeMaterialSlots(ID3D12Device5* device, ResourceUploader& uploader,
        DescriptorHeap& heap, const Scene& scene, std::vector<RayTracingInstanceData>& instances);

    bool RebuildMeshBLASInternal(
      ID3D12Device5* device,
      ID3D12GraphicsCommandList4* commandList,
      const Mesh* mesh,
      const std::optional<RayTracingGeometryCounts>& countOverride);

    static constexpr std::size_t InvalidInstanceIndex = static_cast<std::size_t>(-1);

    // Scene-lifetime ownership, keyed by the existing Mesh identity. Mesh and
    // geometry ownership remain with the scene: checked renderer teardown must
    // complete before scene meshes or their descriptors are destroyed. Existing
    // geometry replacement keeps its separate GPU retirement protocol.
    struct StableBLASRegistry
    {
        void Shutdown();

        std::vector<std::unique_ptr<BottomLevelAccelerationStructure>> bottomLevels;
        std::unordered_map<const Mesh*, std::size_t> meshToBottomLevel;

        // Indexed view for TLAS builds/updates; pointees survive vector growth.
        std::vector<const BottomLevelAccelerationStructure*> bottomLevelPointers;
        std::uint64_t retirementLifetime = 0;
    };

    // All state tied to one resident-instance membership. InstanceID remains
    // the runtime object index, including holes for nonresident objects.
    // Never shallow-copy or move these raw GPU/descriptor owners. Future
    // candidates must allocate their own resources and borrow the registry
    // without moving resources out of this unit.
    struct MembershipSnapshot
    {
        MembershipSnapshot() = default;
        MembershipSnapshot(const MembershipSnapshot&) = delete;
        MembershipSnapshot& operator=(const MembershipSnapshot&) = delete;
        MembershipSnapshot(MembershipSnapshot&&) = delete;
        MembershipSnapshot& operator=(MembershipSnapshot&&) = delete;

        // Requires completion of all referencing work and abandonment of old
        // recordings. StructuredBuffer::Shutdown frees descriptors immediately;
        // this is not an independent in-flight snapshot retirement operation.
        void Shutdown();

        // Pending recordings prevent retirement even before submission.
        // Uncertainty is sticky until confirmation or checked drained teardown.
        std::uint64_t recordingGeneration = 0;
        std::uint64_t lastUseFence = 0;
        std::uint64_t fenceLifetime = 0;
        bool submissionUncertain = false;

        TopLevelAccelerationStructure topLevel;
        StructuredBuffer instanceDataBuffer;
        StructuredBuffer triangleSlots;
        StructuredBuffer slotMaterials;

        // Future adoption can retain the already allocated candidate control
        // block here after moving its snapshot/new BLAS out. This preserves
        // geometry/BLAS pins without allocating at the acceptance point.
        std::shared_ptr<const CandidateState> dependencyOwner;

        std::vector<RayTracingInstanceDescription> instances;
        std::vector<std::size_t> renderObjectToInstance;

        std::unordered_map<const Mesh*, MeshMetadata> slotGeometry;
        std::vector<RenderObject> materialBindings;
        std::vector<std::vector<unsigned int>> slotMaterialIndices;
    };

    static bool BuildMaterialSlotData(MembershipSnapshot& snapshot, const std::vector<RenderObject>& objects,
        std::vector<RayTracingInstanceData>& instances, std::vector<unsigned int>& triangles,
        std::vector<unsigned int>& materials);
    static bool BuildInstanceData(MembershipSnapshot& snapshot, const std::vector<RenderObject>& objects,
        const std::unordered_map<const Mesh*, std::size_t>& meshIndices,
        std::vector<RayTracingInstanceData>& instances);
    static void PatchTableDescriptors(const MembershipSnapshot& snapshot, std::vector<RayTracingInstanceData>& instances);
    bool ValidateCandidateBindings(const CandidateState& candidate, const FileSceneRuntime& runtime,
        FileSceneAssetCatalog& catalog, std::string& error) const;
    void ShutdownReplacementCandidate();

    // One independently owned candidate; manager pin prevents destruction on
    // handle cancellation while recorded/submitted work still references it.
    std::shared_ptr<CandidateState> m_candidate;

    // Explicit Shutdown releases membership before the registry. Keep the same
    // reverse declaration order for any future owning destructors as well.
    StableBLASRegistry m_blasRegistry;
    std::unique_ptr<MembershipSnapshot> m_membership = std::make_unique<MembershipSnapshot>();

    // Future coordinator transfers an OLD snapshot here without shutting it
    // down. No active detach/publication or replacement builder exists yet.
    // Failure leaves the caller's ownership intact. Never evict to make room.
    static constexpr std::size_t MaxRetiredMemberships = 4;
    // Future publication must check this at its pre-recording boundary, after
    // resolving the previous list. Single-threaded: no recording/submission
    // may intervene between this check and the ownership transfer.
    bool CanRetireActiveMembership() const;
    bool RetireMembership(std::unique_ptr<MembershipSnapshot>& snapshot);
    std::array<std::unique_ptr<MembershipSnapshot>, MaxRetiredMemberships> m_retiredMemberships;

};
