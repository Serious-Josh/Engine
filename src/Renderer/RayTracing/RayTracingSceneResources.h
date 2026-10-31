#pragma once

#include "Renderer/RayTracing/BottomLevelAccelerationStructure.h"
#include "Renderer/RayTracing/TopLevelAccelerationStructure.h"
#include "Renderer/Mesh.h"
#include "Renderer/CommandContext.h"
#include "Renderer/StructuredBuffer.h"
#include "Renderer/RenderObject.h"
#include "Renderer/ShaderData.h"
#include "assets/MeshMetadata.h"
#include "Scene/MeshResidency.h"

#include <d3d12.h>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <string>
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

// Opt-in association of one production BLAS build with the current P2.1 source
// generation it was recorded from. Immutable; pins the generation and the exact
// capture resources. Lifecycle evidence stays with the owning BLAS (looked up by
// build identity) and is never copied here, so a record cannot claim completion.
struct BLASAssociationRecord
{
    CausticContracts::Geometry::GeometrySourceHandle source;
    BLASBuildCapture capture;
    BLASRecordingProvenance provenance{};
    std::uint64_t sceneLifetime = 0;
    std::uint64_t blasCharge = 0; // result + scratch allocation-info bytes
};

using BLASAssociationHandle = std::shared_ptr<const BLASAssociationRecord>;

struct BLASAssociationStatistics
{
    std::size_t liveRecords = 0;  // ledger-owned or externally held
    std::size_t ledgerOwned = 0;
    std::size_t pendingReaders = 0;
    std::uint64_t blasBytes = 0;
    // Separate from the BLAS budget: retained P2.1 source payload and its buffers.
    std::uint64_t sourceBytes = 0;
    std::uint64_t sourceResourceBytes = 0;
    std::uint64_t rejectedIneligible = 0;
    std::uint64_t rejectedCapacity = 0;
    std::uint64_t rejectedBudget = 0;
    std::uint64_t rejectedAllocation = 0;
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

// Advisory preparation only: no accepted publication identity, reader authority
// or historical TLAS traversal permission. Descriptor bytes live in tlasCapture.
struct ProspectiveTLASGeometryJoin
{
    struct Row
    {
        std::string objectID;
        std::string meshReference;
        std::size_t denseSlot = 0;
        std::size_t authoredIndex = SIZE_MAX;
        std::size_t registryIndex = SIZE_MAX;
        std::size_t nativeInstance = SIZE_MAX; // Meshless slots have no native descriptor.
        std::uint64_t bindingRevision = 0;
        std::uint64_t anticipatedBindingRevision = 0;
        Matrix4 presentationTransform;
        const Mesh* mesh = nullptr; // Catalog borrow only; never dereference after its owner ends.
        CausticContracts::Geometry::GeometrySourceHandle source;
        BLASAssociationHandle association;
    };

    std::uint64_t recordingOwner = 0;
    std::uint64_t rendererLifetime = 0;
    std::uint64_t runtimeIdentity = 0;
    std::uint64_t structureRevision = 0;
    std::uint64_t hierarchyRevision = 0;
    std::uint64_t residencyRevision = 0;
    std::uint64_t anticipatedResidencyRevision = 0;
    std::uint64_t bindingEpoch = 0;
    std::uint64_t anticipatedBindingEpoch = 0;
    std::string sceneID;
    bool residencyCommitPending = false;
    TLASRecordingHandle tlasCapture;
    TLASSubmissionEvidence submission;
    std::vector<Row> rows; // Dense scene order, including meshless slots.
    std::size_t mappingBytes = 0;
};

using ProspectiveTLASGeometryJoinHandle = std::shared_ptr<const ProspectiveTLASGeometryJoin>;

// Issued only by final CPU/GPU publication. Descriptive evidence, never reader
// authorization; historical handles cannot preserve in-place TLAS contents.
struct AcceptedTLASGeometry
{
    ProspectiveTLASGeometryJoinHandle geometry;
    const FileSceneRuntime* runtime = nullptr; // Identity only; never dereference historical owner pointers.
    std::uint64_t residencyRevision = 0;
    std::uint64_t bindingEpoch = 0;
    // R.9I.3 P5B: nonzero only for a current-frame record (BuildCurrentFrameGeometry): valid for GPU consumers recorded
    // later in exactly this recording generation, never the owner's current accepted geometry, never reused.
    std::uint64_t currentFrameGeneration = 0;
};
using AcceptedTLASGeometryHandle = std::shared_ptr<const AcceptedTLASGeometry>;
enum class ProspectiveJoinStatus
{
    Unavailable, Ready, WrongLifetime, StaleCandidate, IncompleteCapture, UnprovenRecording,
    SourceUnavailable, MissingAssociation, DescriptorMismatch, CapacityExceeded,
    MappingBudgetExceeded, AllocationFailed, ValidationFailed
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
        friend class SceneMembershipPublication;
        friend class RayTracingRecordingDiagnostic;
        std::uint64_t m_rendererLifetime = 0;
        std::shared_ptr<CandidateState> m_state;
    };

    enum class CandidateStatus { Pending, Ready, NeedsTransformRefresh, Stale, Failed };

    // P2.4 GPU reader of one current accepted record. The manager, not this
    // handle, owns every dependency until the reading submission is resolved.
    enum class GeometryReaderState : std::uint8_t
    {
        Acquired, Recorded, SubmissionUncertain, NotSubmitted, Submitted, Completed, Discarded
    };

    class GeometryReader
    {
    public:
        GeometryReader() = default;
        GeometryReader(const GeometryReader&) = delete;
        GeometryReader& operator=(const GeometryReader&) = delete;
        GeometryReader(GeometryReader&&) noexcept = default;
        GeometryReader& operator=(GeometryReader&&) noexcept = default;

        explicit operator bool() const noexcept { return static_cast<bool>(m_record); }
        GeometryReaderState GetState() const noexcept;
        const AcceptedTLASGeometryHandle& GetAcceptedGeometry() const noexcept;

    private:
        friend class RayTracingSceneResources;
        struct Record;
        std::shared_ptr<Record> m_record;
    };

    // A frame acquires after retiring every reader whose fence is observed complete (Renderer). Frame
    // pacing leaves at most FrameCount earlier submissions unfinished, so one slot per in-flight frame
    // plus the acquiring frame's is the steady-state bound; fewer refuse periodically whenever the GPU
    // runs behind the CPU.
    static constexpr std::size_t MaxGeometryReaders = CommandContext::FrameCount + 1;
    std::size_t GetLiveGeometryReaderCount() const noexcept;
    // Diagnostics: each slot's state and submission fence; nullopt for a free slot.
    std::array<std::optional<std::pair<GeometryReaderState, std::uint64_t>>, MaxGeometryReaders>
        DescribeGeometryReaders() const noexcept;

    // The last staged preparation may become stale. This is never accepted or
    // current scene evidence; only the private final publication handoff can accept it.
    ProspectiveTLASGeometryJoinHandle GetStagedProspectiveJoin() const noexcept
    {
        return m_stagedProspectiveJoin;
    }

    ProspectiveJoinStatus GetProspectiveJoinStatus() const noexcept
    {
        return m_prospectiveJoinStatus;
    }

    static constexpr std::size_t MaxProspectiveJoins = 3;
    static constexpr std::size_t MaxProspectiveRows = 4096;
    static constexpr std::size_t MaxProspectiveMappingBytes = 1024 * 1024;
    // Bounded batch target, not an architectural constant: every proposal is
    // charged against the metadata/table/AS budgets below, and a candidate
    // retires one membership whatever its size.
    static constexpr std::size_t MaxCandidateProposals = 32;
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

    // Scene-resource lifetime that association records carry; 0 when uninitialized.
    std::uint64_t GetSceneResourceLifetime() const noexcept
    {
        return m_recordingOwner;
    }

    // Opt-in BLAS-side evidence only (no TLAS, material or caustic acceptance).
    // Returns the ledger-owned record for the mesh's CURRENT registry build when the
    // lifetime is current, the mesh still owns exactly this P2.1 generation, the
    // build read that generation's resources/descriptor without override, and it is
    // Completed with no submission uncertainty. Otherwise empty. Historical records
    // are never returned. The returned handle counts against ledger capacity.
    BLASAssociationHandle QueryCompletedBLASAssociation(
        const Mesh& mesh,
        const CausticContracts::Geometry::GeometrySourceHandle& source,
        std::uint64_t sceneLifetime) const noexcept;

    private:
    friend class Renderer;
    friend class FileSceneRuntime;
    friend class MeshResidencyPublication;
    friend class SceneMembershipPublication;
#if defined(ENGINE_HAS_VERIFICATION)
    friend class SceneDuplicationVerification;
    friend class PresentationTransformVerification;
    friend class SceneMembershipVerification;
    friend class SceneHierarchyVerification;
    friend class DescriptorRecoveryVerification;
    friend class ResidencyBatchVerification;
    friend class CausticVisibleVerification;
#endif
    friend class SceneMaterialBindingPublication;
    // Ignored caustics diagnostic host: drives the real recording hooks below.
    friend class RayTracingRecordingDiagnostic;
#if defined(ENGINE_HAS_VERIFICATION)
    friend class SceneMaterialBindingVerification;
    friend class MaterialAdmissionVerification;
#endif

    // Renderer-only observations cover every tracked list conservatively,
    // including builds and later rendering uses of an unchanged snapshot.
    bool BeginMembershipRecording(std::uint64_t generation);
    bool AttemptMembershipSubmission(std::uint64_t generation);
    bool ConfirmMembershipSubmission(std::uint64_t generation, const CommandContext& context);
    void MembershipNotSubmitted(std::uint64_t generation);
    void DiscardMembershipRecording(std::uint64_t generation);
    void CollectRetiredMemberships(const CommandContext& context);

    // Why a candidate could not be prepared. Callers must not treat exhaustion as a
    // transient synchronization deferral: retrying it unchanged cannot succeed.
    enum class CandidateFailure
    {
        None,
        DescriptorExhausted,
        Other
    };

    // A candidate owns its triangle-slot, slot-material and instance-data SRVs while the
    // active membership keeps its own until fence retirement.
    static constexpr std::size_t CandidateDescriptorCount = 3;

    std::optional<ReplacementCandidate> PrepareReplacementCandidate(ID3D12Device5* device,
        DescriptorHeap& heap, const FileSceneRuntime& runtime, FileSceneAssetCatalog& catalog,
        const std::vector<ResidencyProposal>& proposals, std::size_t materialCount, std::string& error,
        const FileSceneRuntime* membershipSource = nullptr, CandidateFailure* failure = nullptr);
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
    // One staged scene-side batch entry, matched against the candidate proposal
    // at the same position.
    struct AdoptionEntry
    {
        std::string_view objectID;
        std::size_t objectIndex = 0;
        const MeshResidencyCandidate* mesh = nullptr;
        Material* fallback = nullptr;
        const std::map<std::string, Material*>* slots = nullptr;
    };
    AdoptionReadiness PrepareCandidateAdoption(const ReplacementCandidate& candidate,
        std::span<const AdoptionEntry> entries, std::size_t materialCount, AdoptionEvidence& evidence,
        std::string& error, std::string* failedObjectID = nullptr);

    // Coordinator only, immediately after fresh readiness with a retained source
    // pin and no intervening mutation/recording. All storage is already reserved.
    AdoptionReadiness PrepareRegistryAdoption(const ReplacementCandidate&, AdoptionEvidence&, std::string&);
    void AdoptPreparedCandidate(const ReplacementCandidate& candidate, const AdoptionEvidence& evidence) noexcept;

    bool InitializeMaterialSlots(ID3D12Device5* device, ResourceUploader& uploader,
        DescriptorHeap& heap, const Scene& scene, std::vector<RayTracingInstanceData>& instances);

    bool RebuildMeshBLASInternal(
      ID3D12Device5* device,
      ID3D12GraphicsCommandList4* commandList,
      const Mesh* mesh,
      const std::optional<RayTracingGeometryCounts>& countOverride);

    // Attributes a just-recorded registry build to the active tracked recording;
    // without one the build stays Unattributed. Candidate builds are not covered.
    void AttributeRecordedBuild(BottomLevelAccelerationStructure& bottomLevel);
    void AttributeRecordedTLAS(TopLevelAccelerationStructure& topLevel);
    void TransitionRecordedBuilds(std::uint64_t generation, BLASRecordingState next);
    void ConfirmRecordedBuilds(std::uint64_t generation, std::uint64_t fence, std::uint64_t lifetime);
    std::uint64_t EnsureRecordingOwner();
    // Candidate-owned new builds; implemented beside CandidateState.
    void ObserveCandidateCompletion(std::uint64_t completed, std::uint64_t lifetime) noexcept;
    TopLevelAccelerationStructure* GetCandidateTopLevel() noexcept;
    const BLASBuildRecording* FindCandidateBuildRecording(BLASBuildIdentity identity) const noexcept;

    // Bounded opt-in association ledger. Exhaustion or ineligibility never fails
    // the ordinary build; it is counted and the build proceeds unassociated.
    static constexpr std::size_t MaxAssociationRecords = 8;
    static constexpr std::uint64_t MaxAssociationBLASBytes = 32ull * 1024 * 1024;
    static constexpr std::size_t MaxAssociationReaders = 2;

    struct AssociationReader
    {
        std::uint64_t fence = 0;
        std::uint64_t lifetime = 0;
    };

    struct AssociationSlot
    {
        BLASAssociationHandle owned; // ledger ownership
        std::weak_ptr<const BLASAssociationRecord> tracked; // live while any holder remains
        std::array<AssociationReader, MaxAssociationReaders> readers{};
    };

    void AssociateRecordedBuild(ID3D12Device5* device, const Mesh& mesh,
        const BottomLevelAccelerationStructure& bottomLevel) noexcept;
    // Current (or retired/candidate) recording for the record's build; null once
    // the owning BLAS no longer holds that build. current = it is the BLAS's current build.
    const BLASBuildRecording* FindAssociatedBuild(const BLASAssociationRecord& record, bool& current) const noexcept;
    // Readers of a Completed record only, with a confirmed fence and its lifetime.
    bool RegisterAssociationReader(const BLASAssociationHandle& record, std::uint64_t fence,
        std::uint64_t lifetime) noexcept;
    void CollectAssociations(std::uint64_t completed, std::uint64_t lifetime) noexcept;
    BLASAssociationStatistics GetAssociationStatistics() const noexcept;

    std::array<AssociationSlot, MaxAssociationRecords> m_associations{};
    std::uint64_t m_associationBudget = MaxAssociationBLASBytes; // lowered only by the diagnostic host
    BLASAssociationStatistics m_associationRejections{};

    // Scene-resource lifetime that disambiguates renderer generations in build
    // provenance. Assigned when Initialize starts recording; cleared by Shutdown.
    std::uint64_t m_recordingOwner = 0;

    static constexpr std::size_t InvalidInstanceIndex = static_cast<std::size_t>(-1);
    // The published membership's native TLAS instance of a render object (its TLAS InstanceID is the render
    // object index), or InvalidInstanceIndex.
    std::size_t GetNativeInstance(std::size_t renderObject) const noexcept
    {
        return m_membership && renderObject < m_membership->renderObjectToInstance.size() ?
            m_membership->renderObjectToInstance[renderObject] : InvalidInstanceIndex;
    }

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
        // CPU copy of immutable lookup metadata, independent of TLAS transforms.
        std::vector<RayTracingInstanceData> instanceRecords;
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
    bool HasMatchingCandidateSubmissionState(const ReplacementCandidate& handle) const noexcept;
    void ShutdownReplacementCandidate();
    // Called only after coordinator final readiness/source validation. Repeats
    // candidate currency checks; all optional work is before ordinary commit.
    ProspectiveJoinStatus PrepareProspectiveJoin(const ReplacementCandidate& candidate,
        const FileSceneRuntime& runtime, FileSceneAssetCatalog& catalog, const CommandContext& context,
        std::uint64_t rendererLifetime) noexcept;
    // Shared exact join validation. A candidate supplies its prospective snapshot;
    // null joins the active membership with the runtime's committed identities.
    // currentFrameGeneration (P5B, BuildCurrentFrameGeometry only): join the active membership's capture recorded in
    // this still-open generation instead of a completed one; every other check is unchanged.
    ProspectiveJoinStatus BuildGeometryJoin(const CandidateState* candidate, const FileSceneRuntime& runtime,
        FileSceneAssetCatalog& catalog, const CommandContext& context, std::uint64_t rendererLifetime,
        ProspectiveTLASGeometryJoinHandle& joined, std::uint64_t currentFrameGeneration = 0) noexcept;
    BLASAssociationHandle FindProspectiveAssociation(const Mesh& mesh,
        const TLASRecordingCapture::BLASReference& reference) const noexcept;
    void InvalidateProspectiveJoin() noexcept;

    // Reconstructed active membership only, at the Engine owner boundary. Never
    // used by candidate publication; it issues the same accepted record.
    enum class ReconstructionAcceptance { Pending, Refused, Accepted };
    ReconstructionAcceptance AcceptReconstructedGeometry(const FileSceneRuntime& runtime,
        FileSceneAssetCatalog& catalog, const CommandContext& context, const Scene* rendererScene,
        std::uint64_t rendererLifetime) noexcept;
    std::uint64_t GetMembershipAdoptionCount() const noexcept { return m_membershipAdoptions; }
    std::uint64_t m_membershipAdoptions = 0; // Any adopted publication supersedes reconstruction evidence.

    // Acquisition outside recording revalidates the record as the owner's current
    // evidence. Recording binds it to the active list only while its exact capture
    // is still the latest TLAS contents.
    GeometryReader AcquireGeometryReader(const AcceptedTLASGeometryHandle& accepted, const FileSceneRuntime& runtime,
        const Scene* rendererScene, std::uint64_t rendererLifetime) noexcept;
    bool RecordGeometryReader(const GeometryReader& reader, std::uint64_t generation, std::uint64_t rendererLifetime,
        D3D12_GPU_VIRTUAL_ADDRESS& topLevel) noexcept;

    // R.9I.3 P5B: transform-only motion. Inside the active recording, after this frame's TLAS write, a record of the
    // capture recorded in this generation, validated exactly as accepted geometry (rows, bindings, residency,
    // completed BLAS builds, byte-exact instance/presentation transforms) except that its proof is "recorded in this
    // list" rather than "completed". It is never m_acceptedGeometry; GPU work recorded after the TLAS write and its
    // UAV barrier in the same list sees exactly that build, and the list executes all-or-nothing.
    AcceptedTLASGeometryHandle BuildCurrentFrameGeometry(const FileSceneRuntime& runtime, FileSceneAssetCatalog& catalog,
        const CommandContext& context, const Scene* rendererScene, std::uint64_t rendererLifetime,
        std::uint64_t generation) noexcept;
    GeometryReader AcquireCurrentFrameGeometryReader(const AcceptedTLASGeometryHandle& record,
        const FileSceneRuntime& runtime, const Scene* rendererScene, std::uint64_t rendererLifetime,
        std::uint64_t generation) noexcept;
    // Current-frame joins never take the persistent/publication join slots.
    std::array<std::weak_ptr<const ProspectiveTLASGeometryJoin>, MaxGeometryReaders + 1> m_currentFrameJoins;
    // Diagnostics: current-frame joins built and their total CPU time.
    std::uint64_t m_currentFrameJoinCount = 0;
    std::uint64_t m_currentFrameJoinNanoseconds = 0;
    // A reader recorded earlier in this list must finish before an in-place TLAS write.
    void OrderRecordedGeometryReaders(ID3D12GraphicsCommandList4* commandList) noexcept;
    void TransitionGeometryReaders(std::uint64_t generation, GeometryReaderState from, GeometryReaderState to) noexcept;
    void CollectGeometryReaders(std::uint64_t completed, std::uint64_t lifetime) noexcept;
    std::array<std::shared_ptr<GeometryReader::Record>, MaxGeometryReaders> m_geometryReaders;

    enum class PublicationKind { Residency, Membership };
    class PublicationHandoff
    {
    public:
        PublicationHandoff() = default;
        PublicationHandoff(PublicationHandoff&&) noexcept = default;
        PublicationHandoff& operator=(PublicationHandoff&&) noexcept = default;
        PublicationHandoff(const PublicationHandoff&) = delete;
        PublicationHandoff& operator=(const PublicationHandoff&) = delete;
    private:
        friend class RayTracingSceneResources;
        std::shared_ptr<AcceptedTLASGeometry> record;
        std::shared_ptr<CandidateState> candidate;
        const MembershipSnapshot* snapshot = nullptr;
        const FileSceneRuntime* runtime = nullptr;
        const void* transaction = nullptr;
    };
    PublicationHandoff PreparePublicationHandoff(const ReplacementCandidate&, const FileSceneRuntime&,
        const void* transaction, PublicationKind) noexcept;
    void CommitPublicationAcceptance(PublicationHandoff&&, const FileSceneRuntime&,
        const Scene* rendererScene, std::uint64_t rendererLifetime, const void* transaction) noexcept;
    bool MatchesAcceptedGeometry(const AcceptedTLASGeometry&, const FileSceneRuntime&,
        const Scene* rendererScene, std::uint64_t rendererLifetime) const noexcept;
    AcceptedTLASGeometryHandle QueryAcceptedGeometry(const FileSceneRuntime&,
        const Scene* rendererScene, std::uint64_t rendererLifetime) const noexcept;
    mutable AcceptedTLASGeometryHandle m_acceptedGeometry;
    std::weak_ptr<const AcceptedTLASGeometry> m_publicationHandoff;

    std::array<std::weak_ptr<const ProspectiveTLASGeometryJoin>, MaxProspectiveJoins> m_prospectiveJoins;
    ProspectiveTLASGeometryJoinHandle m_stagedProspectiveJoin;
    ProspectiveJoinStatus m_prospectiveJoinStatus = ProspectiveJoinStatus::Unavailable;
    std::size_t m_prospectiveMappingBudget = MaxProspectiveMappingBytes; // Diagnostic may only tighten.

    // One independently owned candidate; manager pin prevents destruction on
    // handle cancellation while recorded/submitted work still references it.
    std::shared_ptr<CandidateState> m_candidate;

    // Explicit Shutdown releases membership before the registry. Keep the same
    // reverse declaration order for any future owning destructors as well.
    StableBLASRegistry m_blasRegistry;
    std::unique_ptr<MembershipSnapshot> m_membership = std::make_unique<MembershipSnapshot>();

    // Adoption moves the OLD snapshot here with its own last-use fence intact.
    // Checked collection releases its TLAS/tables/descriptors; the stable BLAS
    // registry remains owned by this scene resource set. Never evict to make room.
    static constexpr std::size_t MaxRetiredMemberships = 4;
    // Publication checks this at its exclusive pre-recording boundary, after
    // resolving the previous list. Single-threaded: no recording/submission
    // may intervene between this check and the ownership transfer.
    bool CanRetireActiveMembership() const;
    bool RetireMembership(std::unique_ptr<MembershipSnapshot>& snapshot);
    std::array<std::unique_ptr<MembershipSnapshot>, MaxRetiredMemberships> m_retiredMemberships;

    struct RetiredMaterialBindings
    {
        ~RetiredMaterialBindings() { instances.Shutdown(); slots.Shutdown(); }
        StructuredBuffer instances;
        StructuredBuffer slots;
        std::uint64_t fence = 0;
        std::uint64_t lifetime = 0;
    };
    // Separate from membership retirement: no TLAS, BLAS or geometry ownership changes.
    std::array<std::unique_ptr<RetiredMaterialBindings>, 4> m_retiredMaterialBindings;
    std::uint64_t m_materialBindingEpoch = 1;

};
