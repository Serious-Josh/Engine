#pragma once

#include "CausticGeometrySourceManifest.h"
#include "Renderer/CommandContext.h"
#include "Renderer/Mesh.h"
#include "Renderer/RayTracing/BLASGeometryDescription.h"
#include <array>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>

// P2.2a isolated diagnostic BLAS association. Never used by the production BLAS
// registry, scene membership or Renderer. Descriptors are built from the retained
// P2.1 generation only, never from mutable Mesh accessors.
namespace CausticContracts::Geometry
{
    // 1 in-flight + 1 current + 2 retained historical records per builder.
    inline constexpr std::size_t MaximumBuildRecords = 4;
    inline constexpr std::size_t MaximumReadersPerRecord = 2;
    // Result + scratch allocation-info charge per build.
    inline constexpr std::uint64_t MaximumBuildResourceBytes = 4ull * 1024 * 1024;

    enum class BuildState { Empty, Prepared, Recorded, Submitted, Completed, Discarded, Uncertain, Retired };
    enum class BuildStatus
    {
        Ready,
        InvalidInput,
        StaleSource,
        Unsupported,
        AllocationFailed,
        CapacityExceeded,
        IdentityExhausted,
        Quarantined,
        WrongState,
        Pending
    };

    struct RecordingIdentity
    {
        std::uint64_t generation = 0;
        unsigned int frameSlot = UINT_MAX;
    };

    struct PreparedBuild
    {
        std::uint64_t ownerLifetime = 0;
        std::uint64_t buildID = 0;
    };

    // Completion evidence copied from the build's own reserved submission token.
    struct BuildSubmission
    {
        std::uint64_t fenceValue = 0;
        std::uint64_t lifetime = 0;
    };

    class DiagnosticBLASBuilder;

    // Immutable after construction. Owns the source generation, exact input
    // resources and result/scratch, so a historical record stays traversable after
    // Mesh replacement. GPU addresses are observations, never identity.
    class BLASBuildRecord final
    {
    public:
        BLASBuildRecord(const BLASBuildRecord&) = delete;
        BLASBuildRecord& operator=(const BLASBuildRecord&) = delete;

        const GeometrySourceHandle& Source() const noexcept { return m_source; }
        ID3D12Resource* VertexResource() const noexcept { return m_vertices.Get(); }
        ID3D12Resource* IndexResource() const noexcept { return m_indices.Get(); }
        std::uint64_t VertexOffset() const noexcept { return 0; }
        std::uint64_t IndexOffset() const noexcept { return 0; }
        const D3D12_RAYTRACING_GEOMETRY_DESC& Geometry() const noexcept { return m_geometry; }
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS BuildFlags() const noexcept { return m_buildFlags; }
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO& Prebuild() const noexcept { return m_prebuild; }

        // Borrowed for diagnostic reads only; never write.
        ID3D12Resource* Result() const noexcept { return m_result.Get(); }
        ID3D12Resource* Scratch() const noexcept { return m_scratch.Get(); }
        D3D12_GPU_VIRTUAL_ADDRESS ResultAddressObservation() const noexcept { return m_resultAddress; }

        std::uint32_t FirstCertifiedPrimitive() const noexcept { return m_source->Source().Certificate().firstPrimitive; }
        std::uint32_t CertifiedPrimitiveCount() const noexcept { return m_source->Source().Certificate().primitiveCount; }
        std::uint64_t BuildID() const noexcept { return m_buildID; }
        std::uint64_t OwnerLifetime() const noexcept { return m_ownerLifetime; }
        RecordingIdentity Recording() const noexcept { return m_recording; }
        std::uint64_t AllocatedBytes() const noexcept { return m_allocatedBytes; }

    private:
        friend class DiagnosticBLASBuilder;
        BLASBuildRecord() = default;

        GeometrySourceHandle m_source;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_vertices, m_indices, m_result, m_scratch;
        D3D12_RAYTRACING_GEOMETRY_DESC m_geometry{};
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS m_buildFlags{};
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO m_prebuild{};
        D3D12_GPU_VIRTUAL_ADDRESS m_resultAddress = 0;
        std::uint64_t m_buildID = 0;
        std::uint64_t m_ownerLifetime = 0;
        RecordingIdentity m_recording{};
        std::uint64_t m_allocatedBytes = 0;
    };
    using BLASBuildHandle = std::shared_ptr<const BLASBuildRecord>;

    // Single-threaded, owning-thread contract. Lifecycle state lives here, not in
    // the record. No waits: completion is observed with nonblocking token queries.
    class DiagnosticBLASBuilder final
    {
    public:
        DiagnosticBLASBuilder() = default;
        DiagnosticBLASBuilder(const DiagnosticBLASBuilder&) = delete;
        DiagnosticBLASBuilder& operator=(const DiagnosticBLASBuilder&) = delete;

        // A builder that cannot prove its resources are unused (unresolved recording,
        // pending submission or uncertainty) deliberately leaks its pins rather than
        // releasing resources that may still be queued.
        ~DiagnosticBLASBuilder()
        {
            if(!m_device || CanReleaseAll())
            {
                return;
            }

            auto* quarantine = new(std::nothrow) std::array<Slot, MaximumBuildRecords>(std::move(m_slots));

            if(!quarantine)
            {
                std::abort();
            }
        }

        bool Initialize(ID3D12Device5* device) noexcept
        {
            if(!device || m_device)
            {
                return false;
            }

            const auto lifetime = AllocateOwnerLifetime();

            if(lifetime == 0)
            {
                return false;
            }

            m_device = device;
            m_ownerLifetime = lifetime;
            return true;
        }

        // Validates the admitted generation, allocates result/scratch and reserves a
        // slot. Nothing is recorded and no record is published.
        BuildStatus Prepare(const Mesh& mesh, const GeometrySourceHandle& source, PreparedBuild& output) noexcept
        {
            if(!m_device)
            {
                return BuildStatus::WrongState;
            }

            if(IsQuarantined())
            {
                return BuildStatus::Quarantined;
            }

            if(!source)
            {
                return BuildStatus::InvalidInput;
            }

            if(!mesh.IsCurrentStaticSource(source))
            {
                return BuildStatus::StaleSource;
            }

            D3D12_RAYTRACING_GEOMETRY_DESC geometry{};

            if(!DescribeGeometry(*source, geometry))
            {
                return BuildStatus::Unsupported;
            }

            Slot* slot = FindFreeSlot();

            if(!slot || HasInFlightBuild())
            {
                return BuildStatus::CapacityExceeded;
            }

            if(m_nextBuildID == UINT64_MAX)
            {
                return BuildStatus::IdentityExhausted;
            }

            // Exactly the ordinary production BLAS inputs: one opaque indexed
            // triangle geometry, FLOAT3 positions at offset 0, prefer fast trace.
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            inputs.NumDescs = 1;
            inputs.pGeometryDescs = &geometry;
            inputs.Flags = BLASBuildFlags(RayTracingAccelerationStructureBuildPreference::FastTrace);

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
            m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

            if(prebuild.ResultDataMaxSizeInBytes == 0 || prebuild.ScratchDataSizeInBytes == 0)
            {
                return BuildStatus::Unsupported;
            }

            auto resultDescription = BufferDescription(prebuild.ResultDataMaxSizeInBytes);
            auto scratchDescription = BufferDescription(prebuild.ScratchDataSizeInBytes);
            const auto resultInfo = m_device->GetResourceAllocationInfo(0, 1, &resultDescription);
            const auto scratchInfo = m_device->GetResourceAllocationInfo(0, 1, &scratchDescription);
            const bool chargeValid = resultInfo.SizeInBytes != UINT64_MAX && scratchInfo.SizeInBytes != UINT64_MAX &&
                resultInfo.SizeInBytes <= MaximumBuildResourceBytes &&
                scratchInfo.SizeInBytes <= MaximumBuildResourceBytes - resultInfo.SizeInBytes;

            if(!chargeValid)
            {
                return BuildStatus::AllocationFailed;
            }

            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            Microsoft::WRL::ComPtr<ID3D12Resource> result, scratch;

            if(FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &scratchDescription,
                   D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&scratch))) ||
                FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resultDescription,
                    D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&result))))
            {
                return BuildStatus::AllocationFailed;
            }

            *slot = {};
            slot->state = BuildState::Prepared;
            slot->buildID = m_nextBuildID++;
            slot->source = source;
            slot->geometry = geometry;
            slot->buildFlags = inputs.Flags;
            slot->prebuild = prebuild;
            slot->result = std::move(result);
            slot->scratch = std::move(scratch);
            slot->allocatedBytes = resultInfo.SizeInBytes + scratchInfo.SizeInBytes;
            output = {m_ownerLifetime, slot->buildID};
            return BuildStatus::Ready;
        }

        // Never recorded, so resources were never referenced by a command list.
        BuildStatus CancelPrepared(PreparedBuild prepared) noexcept
        {
            Slot* slot = FindPrepared(prepared);

            if(!slot)
            {
                return BuildStatus::WrongState;
            }

            *slot = {};
            return BuildStatus::Ready;
        }

        // Records exactly one build plus UAV barrier. Recorded is not completion.
        // Strong output guarantee: the record is allocated before any command.
        BuildStatus Record(PreparedBuild prepared, const Mesh& mesh, ID3D12GraphicsCommandList4* commands,
            RecordingIdentity recording, BLASBuildHandle& output) noexcept
        {
            Slot* slot = FindPrepared(prepared);

            if(!slot)
            {
                return BuildStatus::WrongState;
            }

            const bool recordingValid = commands && recording.generation != 0 &&
                recording.generation > m_lastRecordingGeneration && recording.frameSlot < CommandContext::FrameCount;

            if(!recordingValid)
            {
                return BuildStatus::InvalidInput;
            }

            // Mesh mutation between preparation and recording must not slip through.
            if(!mesh.IsCurrentStaticSource(slot->source))
            {
                return BuildStatus::StaleSource;
            }

            std::shared_ptr<BLASBuildRecord> record;

            try
            {
                record = std::shared_ptr<BLASBuildRecord>(new BLASBuildRecord);
            }
            catch(const std::bad_alloc&)
            {
                return BuildStatus::AllocationFailed;
            }

            record->m_source = slot->source;
            record->m_vertices = slot->source->VertexResource();
            record->m_indices = slot->source->IndexResource();
            record->m_geometry = slot->geometry;
            record->m_buildFlags = slot->buildFlags;
            record->m_prebuild = slot->prebuild;
            record->m_result = slot->result;
            record->m_scratch = slot->scratch;
            record->m_resultAddress = slot->result->GetGPUVirtualAddress();
            record->m_buildID = slot->buildID;
            record->m_ownerLifetime = m_ownerLifetime;
            record->m_recording = recording;
            record->m_allocatedBytes = slot->allocatedBytes;

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
            build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            build.Inputs.NumDescs = 1;
            build.Inputs.pGeometryDescs = &record->m_geometry;
            build.Inputs.Flags = record->m_buildFlags;
            build.DestAccelerationStructureData = record->m_resultAddress;
            build.ScratchAccelerationStructureData = slot->scratch->GetGPUVirtualAddress();
            commands->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = slot->result.Get();
            commands->ResourceBarrier(1, &barrier);

            m_lastRecordingGeneration = recording.generation;
            slot->state = BuildState::Recorded;
            slot->recording = recording;
            slot->record = record;
            output = std::move(record);
            return BuildStatus::Ready;
        }

        // Caller proved the list was aborted or never executed. Not allowed while a
        // submission attempt is armed or after any submission.
        BuildStatus Discard(RecordingIdentity recording) noexcept
        {
            Slot* slot = FindRecorded(recording);

            if(!slot || slot->submissionArmed)
            {
                return BuildStatus::WrongState;
            }

            ReleaseToHistory(*slot, BuildState::Discarded);
            return BuildStatus::Ready;
        }

        // Arm before Execute, matching AttemptMembershipSubmission: if the caller
        // never resolves the attempt, the record cannot be treated as unsubmitted.
        BuildStatus BeginSubmission(RecordingIdentity recording) noexcept
        {
            Slot* slot = FindRecorded(recording);

            if(!slot || slot->submissionArmed)
            {
                return BuildStatus::WrongState;
            }

            slot->submissionArmed = true;
            return BuildStatus::Ready;
        }

        // ExecuteReserved returned NotSubmitted: no ExecuteCommandLists occurred.
        // The record stays Recorded until resubmitted or discarded.
        BuildStatus NotSubmitted(RecordingIdentity recording) noexcept
        {
            Slot* slot = FindRecorded(recording);

            if(!slot || !slot->submissionArmed)
            {
                return BuildStatus::WrongState;
            }

            slot->submissionArmed = false;
            return BuildStatus::Ready;
        }

        // Takes ownership of the reserved token; released after proven completion.
        BuildStatus ConfirmSubmission(RecordingIdentity recording, SubmissionFenceToken token,
            const CommandContext& context) noexcept
        {
            Slot* slot = FindRecorded(recording);
            const bool tokenValid = token.fenceValue != 0 && token.fenceValue != UINT64_MAX && token.retentionID != 0 &&
                token.lifetime != 0 && token.lifetime == context.GetSubmissionLifetime();

            if(!slot || !slot->submissionArmed || !tokenValid)
            {
                return BuildStatus::WrongState;
            }

            slot->submissionArmed = false;
            slot->state = BuildState::Submitted;
            slot->token = token;
            slot->submission = {token.fenceValue, token.lifetime};
            return BuildStatus::Ready;
        }

        // ExecuteReserved returned SubmissionUncertain, or association failed.
        // Sticky: pins are kept and the builder accepts no further builds.
        BuildStatus MarkUncertain(RecordingIdentity recording) noexcept
        {
            Slot* slot = FindRecorded(recording);

            if(!slot || !slot->submissionArmed)
            {
                return BuildStatus::WrongState;
            }

            slot->submissionArmed = false;
            slot->state = BuildState::Uncertain;
            return BuildStatus::Ready;
        }

        // Nonblocking. Only a Complete token in the submitting context's lifetime
        // completes a build; Error (device loss, other lifetime) is never completion.
        BuildStatus Poll(CommandContext& context) noexcept
        {
            bool pending = false;

            for(auto& slot : m_slots)
            {
                if(slot.state != BuildState::Submitted)
                {
                    continue;
                }

                if(context.QuerySubmissionFenceStatus(slot.token) != SubmissionFenceStatus::Complete)
                {
                    pending = true;
                    continue;
                }

                // Completion is proven; the evidence is kept in slot.submission.
                context.ReleaseSubmissionFence(slot.token);
                slot.token = {};
                slot.state = BuildState::Completed;
            }

            return pending ? BuildStatus::Pending : BuildStatus::Ready;
        }

        // Later diagnostic readers of a Completed record. The token's ownership
        // transfers here and is released only once the reader is proven complete.
        BuildStatus RegisterReader(const BLASBuildHandle& handle, SubmissionFenceToken token,
            const CommandContext& context) noexcept
        {
            Slot* slot = FindLive(handle);
            const bool tokenValid = token.fenceValue != 0 && token.fenceValue != UINT64_MAX && token.retentionID != 0 &&
                token.lifetime != 0 && token.lifetime == context.GetSubmissionLifetime();

            if(!slot || slot->state != BuildState::Completed || !tokenValid)
            {
                return BuildStatus::WrongState;
            }

            for(auto& reader : slot->readers)
            {
                if(reader.retentionID == 0)
                {
                    reader = token;
                    return BuildStatus::Ready;
                }
            }

            return BuildStatus::CapacityExceeded;
        }

        // Moves a Completed record to history once every reader is proven complete.
        // The builder keeps only a weak reference; external handles stay valid and
        // continue to occupy the slot until released.
        BuildStatus Retire(const BLASBuildHandle& handle, CommandContext& context) noexcept
        {
            Slot* slot = FindLive(handle);

            if(!slot || slot->state != BuildState::Completed)
            {
                return BuildStatus::WrongState;
            }

            for(const auto& reader : slot->readers)
            {
                if(reader.retentionID != 0 && context.QuerySubmissionFenceStatus(reader) != SubmissionFenceStatus::Complete)
                {
                    return BuildStatus::Pending;
                }
            }

            for(auto& reader : slot->readers)
            {
                if(reader.retentionID != 0)
                {
                    context.ReleaseSubmissionFence(reader);
                    reader = {};
                }
            }

            ReleaseToHistory(*slot, BuildState::Retired);
            return BuildStatus::Ready;
        }

        BuildState StateOf(const BLASBuildHandle& handle) const noexcept
        {
            if(!handle || handle->m_ownerLifetime != m_ownerLifetime)
            {
                return BuildState::Empty;
            }

            for(const auto& slot : m_slots)
            {
                if(slot.state != BuildState::Empty && slot.buildID == handle->m_buildID &&
                    (slot.record == handle || slot.history.lock() == handle))
                {
                    return slot.state;
                }
            }

            return BuildState::Empty;
        }

        bool TryGetSubmission(const BLASBuildHandle& handle, BuildSubmission& output) const noexcept
        {
            const auto state = StateOf(handle);

            if(state != BuildState::Submitted && state != BuildState::Completed && state != BuildState::Retired)
            {
                return false;
            }

            for(const auto& slot : m_slots)
            {
                if(slot.buildID == handle->m_buildID)
                {
                    output = slot.submission;
                    return true;
                }
            }

            return false;
        }

        // Latest Completed (not retired) record, or empty.
        BLASBuildHandle Current() const noexcept
        {
            const Slot* latest = nullptr;

            for(const auto& slot : m_slots)
            {
                if(slot.state == BuildState::Completed && (!latest || slot.buildID > latest->buildID))
                {
                    latest = &slot;
                }
            }

            return latest ? latest->record : BLASBuildHandle{};
        }

        bool IsCurrentFor(const Mesh& mesh, const BLASBuildHandle& handle) const noexcept
        {
            return handle && handle == Current() && mesh.IsCurrentStaticSource(handle->m_source);
        }

        bool IsQuarantined() const noexcept
        {
            for(const auto& slot : m_slots)
            {
                if(slot.state == BuildState::Uncertain)
                {
                    return true;
                }
            }

            return false;
        }

        // Releases builder-held pins. Refuses while any record is Recorded, Submitted,
        // Uncertain or has unresolved readers; prepared builds are cancelled.
        bool Shutdown(CommandContext& context) noexcept
        {
            if(!CanReleaseAll())
            {
                return false;
            }

            for(auto& slot : m_slots)
            {
                for(auto& reader : slot.readers)
                {
                    if(reader.retentionID != 0)
                    {
                        context.ReleaseSubmissionFence(reader);
                    }
                }

                slot = {};
            }

            m_device = nullptr;
            return true;
        }

    private:
        struct Slot
        {
            BuildState state = BuildState::Empty;
            std::uint64_t buildID = 0;
            GeometrySourceHandle source;
            D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS buildFlags{};
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
            Microsoft::WRL::ComPtr<ID3D12Resource> result, scratch;
            std::uint64_t allocatedBytes = 0;

            RecordingIdentity recording{};
            bool submissionArmed = false;
            SubmissionFenceToken token{};
            BuildSubmission submission{};
            std::array<SubmissionFenceToken, MaximumReadersPerRecord> readers{};

            // Strong while live; weak once Discarded/Retired.
            BLASBuildHandle record;
            std::weak_ptr<const BLASBuildRecord> history;
        };

        static std::uint64_t AllocateOwnerLifetime() noexcept
        {
            static std::uint64_t next = 1;
            return next == UINT64_MAX ? 0 : next++;
        }

        static D3D12_RESOURCE_DESC BufferDescription(std::uint64_t bytes) noexcept
        {
            D3D12_RESOURCE_DESC description{};
            description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            description.Width = bytes;
            description.Height = 1;
            description.DepthOrArraySize = 1;
            description.MipLevels = 1;
            description.SampleDesc.Count = 1;
            description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return description;
        }

        // Addresses come from the generation's pinned resources and counts from its
        // retained source spans; layout is rechecked against the exact Vertex ABI.
        static bool DescribeGeometry(const MeshSourceGeneration& source, D3D12_RAYTRACING_GEOMETRY_DESC& geometry) noexcept
        {
            ID3D12Resource* vertices = source.VertexResource();
            ID3D12Resource* indices = source.IndexResource();
            const auto vertexCount = source.Source().Vertices().size();
            const auto indexCount = source.Source().Indices().size();
            const bool layoutValid = vertices && indices && source.VertexStride() == sizeof(Vertex) &&
                source.AttributeOffsets()[0] == 0 && source.IndexFormat() == DXGI_FORMAT_R32_UINT &&
                source.VertexDescription().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
                source.IndexDescription().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
                source.VertexDescription().Width >= source.VertexBytes() &&
                source.IndexDescription().Width >= source.IndexBytes();

            if(!layoutValid || vertexCount == 0 || indexCount == 0 || indexCount % 3 != 0)
            {
                return false;
            }

            // Stricter admission above; native field mapping is the production helper.
            BLASTriangleGeometryInputs inputs{};
            inputs.vertexAddress = vertices->GetGPUVirtualAddress();
            inputs.vertexStride = sizeof(Vertex);
            inputs.vertexCount = vertexCount;
            inputs.indexAddress = indices->GetGPUVirtualAddress();
            inputs.indexFormat = DXGI_FORMAT_R32_UINT;
            inputs.indexCount = indexCount;
            inputs.opaque = true;
            return BuildBLASTriangleGeometryDescription(inputs, geometry);
        }

        void ReleaseToHistory(Slot& slot, BuildState state) noexcept
        {
            slot.history = slot.record;
            slot.record.reset();
            slot.source.reset();
            slot.result.Reset();
            slot.scratch.Reset();
            slot.state = state;
        }

        bool CanReleaseAll() const noexcept
        {
            for(const auto& slot : m_slots)
            {
                const bool unresolved = slot.state == BuildState::Recorded || slot.state == BuildState::Submitted ||
                    slot.state == BuildState::Uncertain;

                if(unresolved)
                {
                    return false;
                }

                // Readers must be resolved through Retire, which proves completion.
                for(const auto& reader : slot.readers)
                {
                    if(reader.retentionID != 0)
                    {
                        return false;
                    }
                }
            }

            return true;
        }

        bool HasInFlightBuild() const noexcept
        {
            for(const auto& slot : m_slots)
            {
                const bool inFlight = slot.state == BuildState::Prepared || slot.state == BuildState::Recorded ||
                    slot.state == BuildState::Submitted || slot.state == BuildState::Uncertain;

                if(inFlight)
                {
                    return true;
                }
            }

            return false;
        }

        // History slots are reusable only once no external handle remains.
        Slot* FindFreeSlot() noexcept
        {
            for(auto& slot : m_slots)
            {
                const bool historical = slot.state == BuildState::Discarded || slot.state == BuildState::Retired;

                if(slot.state == BuildState::Empty || (historical && slot.history.expired()))
                {
                    return &slot;
                }
            }

            return nullptr;
        }

        Slot* FindPrepared(PreparedBuild prepared) noexcept
        {
            if(prepared.ownerLifetime == 0 || prepared.ownerLifetime != m_ownerLifetime)
            {
                return nullptr;
            }

            for(auto& slot : m_slots)
            {
                if(slot.state == BuildState::Prepared && slot.buildID == prepared.buildID)
                {
                    return &slot;
                }
            }

            return nullptr;
        }

        Slot* FindRecorded(RecordingIdentity recording) noexcept
        {
            if(recording.generation == 0)
            {
                return nullptr;
            }

            for(auto& slot : m_slots)
            {
                if(slot.state == BuildState::Recorded && slot.recording.generation == recording.generation &&
                    slot.recording.frameSlot == recording.frameSlot)
                {
                    return &slot;
                }
            }

            return nullptr;
        }

        Slot* FindLive(const BLASBuildHandle& handle) noexcept
        {
            if(!handle || handle->m_ownerLifetime != m_ownerLifetime)
            {
                return nullptr;
            }

            for(auto& slot : m_slots)
            {
                if(slot.record == handle)
                {
                    return &slot;
                }
            }

            return nullptr;
        }

        ID3D12Device5* m_device = nullptr;
        std::uint64_t m_ownerLifetime = 0;
        std::uint64_t m_nextBuildID = 1;
        std::uint64_t m_lastRecordingGeneration = 0;
        std::array<Slot, MaximumBuildRecords> m_slots{};
    };
}
