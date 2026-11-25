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
#include <algorithm>
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
class BLASMemoryPool;

struct SceneBLASRetirementBatchIdentity
{
    std::uint64_t sceneLifetime = 0;

    BLASRetirementBatchIdentity blas{};
};

// Displacement's proof that a recorded TLAS references one mesh's current BLAS: the retirement batch it replaced, the
// caller's geometry version and recording generation, and the TLAS recording, BLAS address and upload slot observed.
struct TLASGeometryRecordingEvidence
{
    SceneBLASRetirementBatchIdentity retirement{};

    std::uint64_t geometryVersion = 0;
    std::uint64_t recordingGeneration = 0;
    std::uint64_t tlasRecording = 0;

    D3D12_GPU_VIRTUAL_ADDRESS blasAddress = 0;
    unsigned int frameSlot = UINT_MAX;
};

// W6.2: a copy-on-write table of per-object values. Copying shares the chunks (O(objects / ChunkSize) pointers); Mutable clones
// only a chunk another table still shares. A residency candidate copies the active membership's tables and changes its batch's
// entries, so it costs O(batch), and a published membership's chunks are never written. Owner (main) thread only.
template<typename Value>
class ChunkedTable
{
public:
    static constexpr std::size_t ChunkSize = 256;

    std::size_t size() const noexcept
    {
        return m_size;
    }

    bool empty() const noexcept
    {
        return m_size == 0;
    }

    const Value& operator[](std::size_t index) const
    {
        return (*m_chunks[index / ChunkSize])[index % ChunkSize];
    }

    // An entry this table may change. Clones its chunk first when any other table shares it.
    Value& Mutable(std::size_t index)
    {
        auto& chunk = m_chunks[index / ChunkSize];

        if(chunk.use_count() > 1)
        {
            chunk = std::make_shared<std::vector<Value>>(*chunk);
        }

        return (*chunk)[index % ChunkSize];
    }

    void Assign(const std::vector<Value>& values)
    {
        Resize(values.size());

        for(std::size_t index = 0; index < values.size(); ++index)
        {
            (*m_chunks[index / ChunkSize])[index % ChunkSize] = values[index];
        }
    }

    // Default-valued entries in unshared chunks.
    void Resize(std::size_t count)
    {
        m_chunks.clear();
        m_size = count;

        for(std::size_t first = 0; first < count; first += ChunkSize)
        {
            m_chunks.push_back(std::make_shared<std::vector<Value>>((std::min)(ChunkSize, count - first)));
        }
    }

    void clear() noexcept
    {
        m_chunks.clear();
        m_size = 0;
    }

    void swap(ChunkedTable& other) noexcept
    {
        m_chunks.swap(other.m_chunks);
        std::swap(m_size, other.m_size);
    }

    class ConstIterator
    {
    public:
        ConstIterator(const ChunkedTable* table, std::size_t index) : m_table(table), m_index(index)
        {
        }

        const Value& operator*() const
        {
            return (*m_table)[m_index];
        }

        ConstIterator& operator++()
        {
            ++m_index;
            return *this;
        }

        bool operator!=(const ConstIterator& other) const noexcept
        {
            return m_index != other.m_index;
        }

    private:
        const ChunkedTable* m_table = nullptr;
        std::size_t m_index = 0;
    };

    ConstIterator begin() const
    {
        return {this, 0};
    }

    ConstIterator end() const
    {
        return {this, m_size};
    }

private:
    std::vector<std::shared_ptr<std::vector<Value>>> m_chunks;
    std::size_t m_size = 0;
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
        std::uint64_t m_rendererLifetime = 0;
        std::shared_ptr<CandidateState> m_state;
    };

    enum class CandidateStatus { Pending, Ready, NeedsTransformRefresh, Stale, Failed };

    // Every scene object takes part in each candidate, so the candidate's dense per-object work is bounded by the largest
    // scene the Engine admits (MaximumSceneObjects, M3.5; asserted equal where both are visible). Content-dependent storage
    // stays bounded by the metadata, table and AS byte budgets below, which are limits, not allocations.
    static constexpr std::size_t MaxCandidateObjects = 65536;
    // Bounded batch target, not an architectural constant: every proposal is
    // charged against the metadata/table/AS budgets below, and a candidate
    // retires one membership whatever its size. W6.4: the Engine sizes batches
    // by a frame budget up to this bound.
    static constexpr std::size_t MaxCandidateProposals = 1024;
    // About 3 KiB of the conservative copy estimate per object with short identifiers and one sphere mesh (measured), more with
    // World identifiers and long names: 8 KiB per object at the object bound keeps headroom for both.
    static constexpr std::size_t MaxCandidateMetadataBytes = 8 * 1024 * MaxCandidateObjects;
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

    // Displacement TLAS-use evidence. Capture only after a checked successful TLAS recording, in the caller's same active
    // generation. Version and generation are caller labels, validated by Renderer, not queue proof.
    std::uint64_t GetTLASRecordingIdentity() const
    {
        return m_membership->topLevel.GetRecordingIdentity();
    }

    // PT.2A: the active TLAS's geometry identity (TopLevelAccelerationStructure::GetGeometryIdentity).
    std::uint64_t GetTLASGeometryIdentity() const
    {
        return m_membership->topLevel.GetGeometryIdentity();
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

    // Every BLAS built from here on places its result in this pool (null: a committed buffer per BLAS). The pool must
    // outlive every BLAS, so its owner sets it once and shuts it down after this object's final Shutdown.
    void SetBLASMemoryPool(BLASMemoryPool* pool)
    {
        m_blasMemoryPool = pool;
    }

    void Shutdown();

    const TopLevelAccelerationStructure& GetTopLevel() const;

    // M5.5 Stage 1 (ReSTIR PT history scene): the active membership's TLAS instances and the BLAS view its builds and updates
    // index (bottomLevelIndex), as Update() passes them.
    const std::vector<RayTracingInstanceDescription>& GetActiveInstances() const
    {
        return m_membership->instances;
    }

    const std::vector<const BottomLevelAccelerationStructure*>& GetBottomLevelPointers() const
    {
        return m_blasRegistry.bottomLevelPointers;
    }

    unsigned int GetInstanceDataDescriptorIndex() const;
#ifndef NDEBUG
    // M0c.4 diagnostics (Debug only, CPU only): the active membership's staged records, the ones uploaded for the GPU, against
    // the material table this recording published. For every TLAS instance: its instance record exists, its vertex and index
    // buffer descriptors are neither 0 nor the UINT_MAX sentinel, its slot buffers are the membership's own, and every slot
    // material resolves to a published record whose texture indices are UINT_MAX (absent) or a real descriptor (never 0,
    // which holds no 2D texture). Appends one line per finding (at most `limit`) and returns the number of findings. Repairs
    // nothing.
    std::size_t ValidateStagedRecords(const std::vector<GPUMaterial>& publishedMaterials, std::size_t limit,
        std::string& report) const;
#endif
    std::size_t GetInstanceCount() const;
    bool ValidateMaterialBindings(const Scene& scene) const;

    private:
    friend class Renderer;
    friend class FileSceneRuntime;
    friend class MeshResidencyPublication;
    friend class SceneMembershipPublication;
#if defined(ENGINE_HAS_VERIFICATION)
    friend class SceneDuplicationVerification;
    friend class PresentationTransformVerification;
    friend class SceneMembershipVerification;
    friend class CompositePlacementStress;
    friend class SceneHierarchyVerification;
    friend class DescriptorRecoveryVerification;
    friend class ResidencyBatchVerification;
#endif
    friend class SceneMaterialBindingPublication;
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

        // W6.2: shared, immutable metadata copies (a candidate copies the map's pointers, never the metadata).
        std::unordered_map<const Mesh*, std::shared_ptr<const MeshMetadata>> slotGeometry;
        ChunkedTable<RenderObject> materialBindings;
        ChunkedTable<std::vector<unsigned int>> slotMaterialIndices;
        // CPU copy of immutable lookup metadata, independent of TLAS transforms.
        std::vector<RayTracingInstanceData> instanceRecords;
        // W6.2: CPU copies of the uploaded triangle-slot table and each multi-slot mesh's offset in it, so a later candidate
        // extends them instead of rebuilding. Empty for memberships that predate W6.2 tables (initial scene builds).
        std::vector<unsigned int> triangleTable;
        std::unordered_map<const Mesh*, unsigned int> triangleOffsets;
    };

    static bool BuildMaterialSlotData(MembershipSnapshot& snapshot, const std::vector<RenderObject>& objects,
        std::vector<RayTracingInstanceData>& instances, std::vector<unsigned int>& triangles,
        std::vector<unsigned int>& materials);
    // The vertex, index and attribute lookup of one resident object; table offsets and descriptors are set by the caller.
    static bool BuildInstanceRecord(const RenderObject& renderObject, RayTracingInstanceData& record);
    static bool BuildInstanceData(MembershipSnapshot& snapshot, const std::vector<RenderObject>& objects,
        const std::unordered_map<const Mesh*, std::size_t>& meshIndices,
        std::vector<RayTracingInstanceData>& instances);
    static void PatchTableDescriptors(const MembershipSnapshot& snapshot, std::vector<RayTracingInstanceData>& instances);
    bool ValidateCandidateBindings(const CandidateState& candidate, const FileSceneRuntime& runtime,
        FileSceneAssetCatalog& catalog, std::string& error) const;
    bool HasMatchingCandidateSubmissionState(const ReplacementCandidate& handle) const noexcept;
    void ShutdownReplacementCandidate();
    // One independently owned candidate; manager pin prevents destruction on
    // handle cancellation while recorded/submitted work still references it.
    std::shared_ptr<CandidateState> m_candidate;

    BLASMemoryPool* m_blasMemoryPool = nullptr;

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
