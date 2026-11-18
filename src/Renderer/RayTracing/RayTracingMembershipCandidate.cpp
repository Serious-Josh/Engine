#include <cassert>
#include "Debug/MemoryRegistry.h"
#include "RayTracingSceneResources.h"

#include "Renderer/CommandContext.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/Material.h"
#include "Renderer/Mesh.h"
#include "Renderer/ShaderData.h"
#include "Scene/FileSceneRuntime.h"
#include "Scene/FileSceneAssetCatalog.h"

#include <wrl/client.h>
#include <algorithm>
#include <string>
#include <cstring>
#include <set>
#include <type_traits>

using Microsoft::WRL::ComPtr;

namespace
{
    bool Fail(std::string& error, const char* message)
    {
        error = message;
        return false;
    }

    bool Charge(std::size_t bytes, std::size_t limit, std::size_t& total)
    {
        if (bytes > limit || total > limit - bytes)
        {
            return false;
        }

        total += bytes;
        return true;
    }

    bool ChargeText(const std::string& text, std::size_t& total)
    {
        // Multiple copied identities/bindings plus allocator/node overhead.
        return text.size() <= RayTracingSceneResources::MaxCandidateMetadataBytes / 8 &&
            Charge(text.size() * 8 + 128, RayTracingSceneResources::MaxCandidateMetadataBytes, total);
    }

    bool ChargeMetadata(const MeshMetadata& metadata, std::size_t& total)
    {
        for (const auto& slot : metadata.materialSlots)
        {
            if (!ChargeText(slot.id, total) || !ChargeText(slot.name, total))
            {
                return false;
            }
        }

        for (const auto& submesh : metadata.submeshes)
        {
            if (!ChargeText(submesh.name, total))
            {
                return false;
            }
        }

        return true;
    }

    bool ChargeGPUAllocation(std::size_t bytes, std::size_t& total)
    {
        if (!bytes || bytes > RayTracingSceneResources::MaxCandidateASBytes)
        {
            return false;
        }

        constexpr auto alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        const auto aligned = (bytes + alignment - 1) & ~(alignment - 1);
        return Charge(aligned, RayTracingSceneResources::MaxCandidateASBytes, total);
    }

    bool ChargeAS(ID3D12Device5* device, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs,
        std::size_t& total)
    {
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
        const auto scratch = (std::max)(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes);

        return ChargeGPUAllocation(info.ResultDataMaxSizeInBytes, total) && ChargeGPUAllocation(scratch, total);
    }

    // ENGINE_W6_FULL_CANDIDATES=1 builds every residency candidate from the whole scene (the pre-W6.2 path), for comparison.
    bool FullCandidatesForced()
    {
        static const bool forced = std::getenv("ENGINE_W6_FULL_CANDIDATES") != nullptr;
        return forced;
    }

    std::shared_ptr<const std::vector<std::string>> CollectMaterialReferences(const FileSceneRuntime& runtime)
    {
        std::set<std::string_view> unique;

        for (const auto& authored : runtime.GetFileScene().GetDocument().objects)
        {
            unique.insert(authored.materialReference);

            for (const auto& [slot, reference] : authored.materialSlotBindings)
            {
                unique.insert(reference);
            }
        }

        return std::make_shared<const std::vector<std::string>>(unique.begin(), unique.end());
    }

    bool CreateUpload(ID3D12Device* device, const void* data, std::size_t bytes, ComPtr<ID3D12Resource>& upload)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;

        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
        {
            return false;
        }

        MemoryRegistry::TrackResource(upload);

        void* mapped = nullptr;
        const D3D12_RANGE read{0, 0};

        if (FAILED(upload->Map(0, &read, &mapped)))
        {
            return false;
        }

        std::memcpy(mapped, data, bytes);
        upload->Unmap(0, nullptr);
        return true;
    }
}

// These fields record separate facts, not one exclusive phase. Preparation owns
// private resources; recording sets generation, an attempted submission arms
// uncertain, and confirmation records fence/lifetime and clears generation.
// A nonzero fence alone does not prove completion. Failed/uncertain candidates
// retain the manager pin until checked collection or drained teardown.
struct RayTracingSceneResources::CandidateState
{
    struct GeometryDependency
    {
        const Mesh* mesh = nullptr;
        const BottomLevelAccelerationStructure* blas = nullptr;
        ComPtr<ID3D12Resource> vertices;
        ComPtr<ID3D12Resource> indices;
        ComPtr<ID3D12Resource> blasResource;
        D3D12_GPU_VIRTUAL_ADDRESS blasAddress = 0;
        unsigned int vertexDescriptor = UINT_MAX;
        unsigned int indexDescriptor = UINT_MAX;
        std::size_t vertexCount = 0;
        std::size_t indexCount = 0;
    };

    std::unique_ptr<MembershipSnapshot> snapshot = std::make_unique<MembershipSnapshot>();
    std::vector<std::unique_ptr<BottomLevelAccelerationStructure>> newBLAS;
    std::vector<const Mesh*> newMeshes;
    std::vector<const BottomLevelAccelerationStructure*> blasPointers;
    // Sorted by mesh (W6.2): an incremental candidate copies and extends its base's set.
    std::vector<GeometryDependency> dependencies;
    // Complete prospective index registry. Readiness stages it; adoption will
    // swap it, avoiding unordered_map node allocation during atomic acceptance.
    std::unordered_map<const Mesh*, std::size_t> adoptionIndices;
    std::size_t stableBLASCount = 0;
    std::size_t materialCount = 0;
    std::uint64_t adoptionLifetime = 0;
    std::array<ComPtr<ID3D12Resource>, 3> uploads;
    std::array<std::size_t, 3> uploadBytes{};

    // W6.1: what preparation read, revalidated by each poll without per-object metadata copies. The authored stamp proves
    // every persistent ID, reference and override unchanged; the binding revisions and live render-object bindings cover
    // runtime changes. Every object except a proposal equals its snapshot binding, so only proposals keep their
    // pre-residency bindings (sorted by object index).
    FileSceneRuntime::AuthoredStamp authoredStamp;
    std::vector<std::uint64_t> bindingRevisions;
    // Unique authored material references (fallbacks and slot overrides), shared with later candidates of the same stamp.
    std::shared_ptr<const std::vector<std::string>> materialReferences;
    std::vector<std::pair<std::size_t, RenderObject>> proposalOriginals;
    std::vector<ResidencyProposal> proposals;
    const Scene* scene = nullptr;
    const FileSceneAssetCatalog* catalog = nullptr;
    const MembershipSnapshot* originalMembership = nullptr;
    std::uint64_t runtimeIdentity = 0;
    std::uint64_t residencyRevision = 0;
    std::uint64_t structureRevision = 0;

    std::uint64_t generation = 0;
    std::uint64_t fence = 0;
    std::uint64_t fenceLifetime = 0;
    bool uncertain = false;
    bool failed = false;
    bool valid = true;

    void Shutdown()
    {
        valid = false;
        if (snapshot)
        {
            snapshot->Shutdown();
        }

        for (auto& blas : newBLAS)
        {
            if (blas)
            {
                blas->Shutdown();
            }
        }

        newBLAS.clear();
        blasPointers.clear();
        dependencies.clear();

        for (auto& upload : uploads)
        {
            upload.Reset();
        }
    }
};

std::optional<RayTracingSceneResources::ReplacementCandidate> RayTracingSceneResources::PrepareReplacementCandidate(
    ID3D12Device5* device, DescriptorHeap& heap, const FileSceneRuntime& runtime, FileSceneAssetCatalog& catalog,
    const std::vector<ResidencyProposal>& proposals, std::size_t materialCount, std::string& error,
    const FileSceneRuntime* membershipSource, CandidateFailure* failure)
{
    const MemoryCategoryScope memoryScope(MemoryCategory::AccelerationStructures);

    error.clear();

    // CPU-only preparation: recording and submission happen after this returns,
    // so the pass's source pins never span GPU work.
    const FileSceneAssetCatalog::ValidationPass validationPass(catalog);
    // S0.3: the per-object lookups below share one whole-scene validation (read-only; see MetadataValidationPass).
    const FileSceneRuntime::MetadataValidationPass metadataPass(runtime);

    // Every early return below is an ordinary failure unless marked as exhaustion.
    if (failure)
    {
        *failure = CandidateFailure::Other;
    }

    const auto& scene = runtime.GetScene();
    const auto& objects = scene.GetRenderObjects();

    if (!device || m_candidate || (!membershipSource && proposals.empty()) ||
        (membershipSource && !proposals.empty()) || proposals.size() > MaxCandidateProposals ||
        (!membershipSource && objects.empty()) || objects.size() > MaxCandidateObjects ||
        m_blasRegistry.bottomLevels.size() > MaxCandidateObjects || !ValidateMaterialBindings(membershipSource ? membershipSource->GetScene() : scene))
    {
        Fail(error, "Candidate admission failed: outstanding candidate, invalid membership or work limit.");
        return std::nullopt;
    }

    // W6.2: a residency batch extends the active membership when it carries complete W6.2 tables and the adopted candidate
    // that built it (its sorted geometry dependencies). Otherwise, and for every membership change, the full build runs.
    const auto* previous = m_membership->dependencyOwner.get();
    const auto& active = *m_membership;
    const bool incremental = !membershipSource && !FullCandidatesForced() && previous &&
        active.materialBindings.size() == objects.size() && active.slotMaterialIndices.size() == objects.size() &&
        active.instanceRecords.size() == objects.size() && active.renderObjectToInstance.size() == objects.size() &&
        !active.triangleTable.empty();

    // The full build copies every object's metadata, so its budget is charged over the scene; an incremental candidate
    // copies only the arriving objects' (charged below with each proposal).
    std::size_t metadataBytes = incremental ? 0 : objects.size() * (sizeof(RenderObject) * 3 + sizeof(SceneObjectMetadata));

    for (const auto& authored : runtime.GetFileScene().GetDocument().objects)
    {
        if (incremental)
        {
            break;
        }

        if (!ChargeText(authored.id, metadataBytes) || !ChargeText(authored.name, metadataBytes) ||
            !ChargeText(authored.meshReference, metadataBytes) || !ChargeText(authored.materialReference, metadataBytes))
        {
            Fail(error, "Candidate authored metadata exceeds its memory budget.");
            return std::nullopt;
        }

        for (const auto& [slot, reference] : authored.materialSlotBindings)
        {
            if (!ChargeText(slot, metadataBytes) || !ChargeText(reference, metadataBytes))
            {
                Fail(error, "Candidate authored overrides exceed their memory budget.");
                return std::nullopt;
            }
        }
    }

    for (const auto& object : objects)
    {
        if (incremental)
        {
            break;
        }

        if (object.mesh && !ChargeMetadata(object.mesh->GetMetadata(), metadataBytes))
        {
            Fail(error, "Candidate resident mesh metadata exceeds its memory budget.");
            return std::nullopt;
        }

        for (const auto& [slot, material] : object.slotMaterials)
        {
            if (!ChargeText(slot, metadataBytes))
            {
                Fail(error, "Candidate runtime bindings exceed their memory budget.");
                return std::nullopt;
            }
        }
    }

    // Check descriptor capacity before pinning or allocating anything, so exhaustion is
    // reported without a partial candidate and without a failed-allocation log per attempt.
    if (!heap.CanAllocate(CandidateDescriptorCount))
    {
        if (failure)
        {
            *failure = CandidateFailure::DescriptorExhausted;
        }

        error = "Descriptor heap exhausted: the membership candidate needs " +
            std::to_string(CandidateDescriptorCount) + " SRVs, " + std::to_string(heap.GetAvailableCount()) +
            " available of " + std::to_string(heap.GetCapacity()) + ".";
        return std::nullopt;
    }

    // Establish the manager pin before fallible allocation. Even a later failure
    // retains every resource until successful command discard or GPU completion.
    m_candidate = std::make_shared<CandidateState>();
    auto& state = *m_candidate;
    state.failed = true;
    state.scene = &scene;
    state.catalog = &catalog;
    state.originalMembership = m_membership.get();

    state.runtimeIdentity = runtime.GetRuntimeIdentity();
    state.residencyRevision = runtime.GetResidencyRevision();
    state.structureRevision = scene.GetStructureRevision();
    state.stableBLASCount = m_blasRegistry.bottomLevels.size();
    state.materialCount = materialCount;

    // W6.1: one whole-scene mapping validation (reused by the runtime while unchanged) instead of a metadata copy per object.
    if (!runtime.ValidateObjectMapping(&error) || runtime.GetObjectMapping().bindingRevisions.size() != objects.size())
    {
        Fail(error, "Candidate object identity mapping is incomplete.");
        return std::nullopt;
    }

    state.authoredStamp = runtime.GetAuthoredStamp();
    state.bindingRevisions = runtime.GetObjectMapping().bindingRevisions;
    // Unique authored references: the adopted previous candidate's set while the authored stamp is unchanged.
    state.materialReferences = previous && previous->materialReferences && previous->authoredStamp == state.authoredStamp ?
        previous->materialReferences : CollectMaterialReferences(runtime);

    // Each proposal with the binding it will have once resident, in object order.
    std::vector<std::pair<std::size_t, RenderObject>> arriving;
    arriving.reserve(proposals.size());
    std::set<std::size_t> proposedIndices;
    std::vector<MeshResidencySourceLock> sourceLocks;

    for (const auto& proposal : proposals)
    {
        const auto metadata = runtime.GetObjectMetadata(proposal.objectID, &error);
        auto source = catalog.ValidateMeshResidencyCandidate(proposal.mesh,
            metadata ? metadata->meshReference : std::string_view{}, &error);

        if (!metadata || !source || metadata->GetMeshResidency(objects[metadata->runtimeIndex]) != SceneMeshResidency::Pending ||
            !proposedIndices.insert(metadata->runtimeIndex).second)
        {
            if (error.empty())
            {
                Fail(error, "Candidate proposal must identify a distinct pending object.");
            }

            return std::nullopt;
        }

        if (!ChargeMetadata(proposal.mesh.mesh->GetMetadata(), metadataBytes))
        {
            Fail(error, "Candidate arriving mesh metadata exceeds its memory budget.");
            return std::nullopt;
        }

        sourceLocks.push_back(std::move(source));
        state.proposalOriginals.emplace_back(metadata->runtimeIndex, objects[metadata->runtimeIndex]);
        arriving.emplace_back(metadata->runtimeIndex, objects[metadata->runtimeIndex]);
        auto& object = arriving.back().second;
        const auto& slots = proposal.mesh.mesh->GetMetadata().materialSlots;

        for (const auto& [slotID, reference] : metadata->materialSlotBindings)
        {
            if (std::none_of(slots.begin(), slots.end(), [&](const auto& slot) { return slot.id == slotID; }))
            {
                Fail(error, "Candidate mesh has an unknown authored material-slot override.");
                return std::nullopt;
            }
        }

        object.mesh = proposal.mesh.mesh;
        // Metadata preparation already resolved these current bindings using
        // the catalog fallback policy. Reuse them without synchronous asset loads.
        if (!catalog.ValidateMetadataMaterialReference(metadata->materialReference, &error) || !object.material)
        {
            return std::nullopt;
        }

        auto retainedSlots = std::move(object.slotMaterials);

        for (const auto& slot : slots)
        {
            Material* material = object.material;
            const auto authored = metadata->materialSlotBindings.find(slot.id);

            if (authored != metadata->materialSlotBindings.end())
            {
                const auto retained = retainedSlots.find(slot.id);
                if (!catalog.ValidateMetadataMaterialReference(authored->second, &error) || retained == retainedSlots.end())
                {
                    Fail(error, "Candidate override has no validated retained material.");
                    return std::nullopt;
                }

                material = retained->second;
            }

            if (!material)
            {
                return std::nullopt;
            }

            object.slotMaterials.emplace(slot.id, material);
        }
    }

    std::sort(arriving.begin(), arriving.end(), [](const auto& left, const auto& right) { return left.first < right.first; });

    // Preflight before constructing tables, AS or staging buffers.
    std::size_t tableBytes = objects.size() * sizeof(RayTracingInstanceData) + sizeof(unsigned int);
    std::size_t asBytes = 0;
    state.blasPointers = m_blasRegistry.bottomLevelPointers;
    // Registry index of each BLAS this candidate builds.
    std::unordered_map<const Mesh*, std::size_t> newMeshIndices;
    const auto ownedMaterials = catalog.GetMaterials();

    // A resident object's geometry, slot metadata and slot materials (the candidate's admission rules).
    const auto validateResident = [&](const RenderObject& object)
    {
        const auto& mesh = *object.mesh;
        const auto& metadata = mesh.GetMetadata();

        if (!mesh.IsIndexed() || !mesh.GetVertexBufferResource() || !mesh.GetIndexBufferResource() ||
            !mesh.GetVertexBufferAddress() || !mesh.GetIndexBufferAddress() || !mesh.GetVertexCount() ||
            mesh.GetVertexCount() > UINT_MAX || mesh.GetIndexCount() > UINT_MAX ||
            mesh.GetVertexStride() < sizeof(float) * 3 ||
            mesh.GetVertexBufferResource()->GetDesc().Width < mesh.GetVertexCount() * mesh.GetVertexStride() ||
            mesh.GetIndexBufferResource()->GetDesc().Width < mesh.GetIndexCount() * sizeof(unsigned int) ||
            mesh.GetIndexFormat() != DXGI_FORMAT_R32_UINT || mesh.GetVertexBufferDescriptorIndex() == UINT_MAX ||
            mesh.GetIndexBufferDescriptorIndex() == UINT_MAX || !mesh.GetBounds().valid ||
            !ValidateMeshMetadata(metadata, mesh.GetIndexCount(), error) ||
            !Charge(metadata.materialSlots.size() * sizeof(unsigned int), MaxCandidateTableBytes, tableBytes))
        {
            return Fail(error, "Candidate geometry is incomplete or exceeds the lookup-table budget.");
        }

        for (const auto& slot : metadata.materialSlots)
        {
            const auto* material = object.GetSlotMaterial(slot.id);

            if (!material || material->gpuIndex >= materialCount ||
                std::find(ownedMaterials.begin(), ownedMaterials.end(), material) == ownedMaterials.end())
            {
                return Fail(error, "Candidate slot material is absent from the retained scene material table.");
            }
        }

        return true;
    };

    // One resident mesh's geometry pins, and a new BLAS when the stable registry has none (only an arriving mesh may lack one).
    const auto addDependency = [&](const Mesh& mesh, bool arrivingMesh)
    {
        const auto& metadata = mesh.GetMetadata();

        if (metadata.materialSlots.size() > 1 &&
            !Charge(mesh.GetIndexCount() / 3 * sizeof(unsigned int), MaxCandidateTableBytes, tableBytes))
        {
            return Fail(error, "Candidate triangle lookup exceeds its budget.");
        }

        CandidateState::GeometryDependency dependency;
        dependency.mesh = &mesh;
        dependency.vertices = mesh.GetVertexBufferResource();
        dependency.indices = mesh.GetIndexBufferResource();
        dependency.vertexDescriptor = mesh.GetVertexBufferDescriptorIndex();
        dependency.indexDescriptor = mesh.GetIndexBufferDescriptorIndex();
        dependency.vertexCount = mesh.GetVertexCount();
        dependency.indexCount = mesh.GetIndexCount();
        const auto registered = m_blasRegistry.meshToBottomLevel.find(&mesh);

        if (registered != m_blasRegistry.meshToBottomLevel.end())
        {
            dependency.blas = state.blasPointers[registered->second];
            dependency.blasResource = dependency.blas->GetResource();
            dependency.blasAddress = dependency.blas->GetGPUAddress();
        }
        else
        {
            if (!arrivingMesh)
            {
                return Fail(error, "Existing resident mesh is missing its stable BLAS registration.");
            }

            D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
            geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            geometry.Triangles.VertexBuffer = {mesh.GetVertexBufferAddress(), mesh.GetVertexStride()};
            geometry.Triangles.VertexCount = static_cast<UINT>(mesh.GetVertexCount());
            geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            geometry.Triangles.IndexBuffer = mesh.GetIndexBufferAddress();
            geometry.Triangles.IndexCount = static_cast<UINT>(mesh.GetIndexCount());
            geometry.Triangles.IndexFormat = mesh.GetIndexFormat();

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            inputs.NumDescs = 1;
            inputs.pGeometryDescs = &geometry;
            inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

            if (!ChargeAS(device, inputs, asBytes) || state.newMeshes.size() >= MaxCandidateProposals)
            {
                return Fail(error, "Candidate new-BLAS build exceeds its work budget.");
            }

            newMeshIndices.emplace(&mesh, state.blasPointers.size());
            state.newMeshes.push_back(&mesh);
            state.newBLAS.push_back(std::make_unique<BottomLevelAccelerationStructure>());
            dependency.blas = state.newBLAS.back().get();
            state.blasPointers.push_back(dependency.blas);
        }

        // Appended here; sorted by mesh once complete (below), so a later incremental candidate searches and extends them.
        state.dependencies.push_back(std::move(dependency));
        return true;
    };
    const auto byMesh = [](const CandidateState::GeometryDependency& left, const CandidateState::GeometryDependency& right)
    {
        return std::less<const Mesh*>{}(left.mesh, right.mesh);
    };

    const auto chargeTopLevel = [&](std::size_t residents)
    {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas{};
        tlas.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tlas.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        tlas.NumDescs = static_cast<UINT>(residents);
        tlas.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;

        if ((!membershipSource && !residents) || (residents && (!ChargeAS(device, tlas, asBytes) ||
            !ChargeGPUAllocation(residents * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), asBytes) ||
            !ChargeGPUAllocation(residents * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), asBytes))))
        {
            return Fail(error, "Candidate TLAS build exceeds its work budget.");
        }

        return true;
    };

    std::vector<RayTracingInstanceData> gpuInstances;
    std::vector<unsigned int> triangles;
    std::vector<unsigned int> materials;
    auto& snapshot = *state.snapshot;
    bool tablesBuilt = false;

    if (!incremental)
    {
        auto effective = objects;

        for (auto& [index, object] : arriving)
        {
            effective[index] = object;
        }

        std::set<const Mesh*> meshes;
        std::size_t residents = 0;

        for (const auto& object : effective)
        {
            if (!object.mesh)
            {
                continue;
            }

            ++residents;

            if (!validateResident(object))
            {
                return std::nullopt;
            }

            if (meshes.insert(object.mesh).second)
            {
                const bool arrivingMesh = std::any_of(proposals.begin(), proposals.end(),
                    [&](const auto& proposal) { return proposal.mesh.mesh == object.mesh; });

                if (!addDependency(*object.mesh, arrivingMesh))
                {
                    return std::nullopt;
                }
            }
        }

        std::sort(state.dependencies.begin(), state.dependencies.end(), byMesh);

        if (!chargeTopLevel(residents))
        {
            return std::nullopt;
        }

        auto meshIndices = m_blasRegistry.meshToBottomLevel;
        meshIndices.insert(newMeshIndices.begin(), newMeshIndices.end());
        gpuInstances.resize(objects.size());
        triangles = {0};
        const bool slotsBuilt = BuildMaterialSlotData(snapshot, effective, gpuInstances, triangles, materials);

        if (membershipSource && !residents)
        {
            materials.push_back(0);
        }

        tablesBuilt = (slotsBuilt || (membershipSource && !residents)) && BuildInstanceData(snapshot, effective, meshIndices, gpuInstances);
    }
    else
    {
        // W6.2: start from the active membership's tables (shared chunks and plain copies) and change only what differs.
        snapshot.materialBindings = active.materialBindings;
        snapshot.slotMaterialIndices = active.slotMaterialIndices;
        snapshot.slotGeometry = active.slotGeometry;
        snapshot.triangleTable = active.triangleTable;
        snapshot.triangleOffsets = active.triangleOffsets;
        gpuInstances = active.instanceRecords;
        state.dependencies = previous->dependencies;

        // Objects not arriving: a resident binding equals the active membership's (admission checked it); a pending one may
        // have been rebound since, and is carried as the full build would copy it.
        auto next = arriving.begin();

        for (std::size_t index = 0; index < objects.size(); ++index)
        {
            if (next != arriving.end() && next->first == index)
            {
                ++next;
                continue;
            }

            const auto& current = objects[index];
            const auto& carried = snapshot.materialBindings[index];

            if (current.mesh != carried.mesh || current.material != carried.material ||
                current.slotMaterials != carried.slotMaterials || current.materialsDeferred != carried.materialsDeferred)
            {
                if (current.mesh)
                {
                    Fail(error, "Candidate base membership differs from a resident binding.");
                    return std::nullopt;
                }

                snapshot.materialBindings.Mutable(index) = current;
            }
        }

        // Arriving objects: their geometry, metadata, slot materials, instance record and triangle lookup. A mesh already
        // resident has its dependency in the base set (sorted); one added by this batch is remembered.
        const std::size_t baseDependencies = state.dependencies.size();
        std::set<const Mesh*> addedMeshes;

        for (auto& [index, object] : arriving)
        {
            const auto& mesh = *object.mesh;
            const auto& metadata = mesh.GetMetadata();
            const auto baseEnd = state.dependencies.begin() + static_cast<std::ptrdiff_t>(baseDependencies);
            const auto found = std::lower_bound(state.dependencies.begin(), baseEnd, &mesh,
                [](const CandidateState::GeometryDependency& existing, const Mesh* key) { return std::less<const Mesh*>{}(existing.mesh, key); });
            const bool known = (found != baseEnd && found->mesh == &mesh) || addedMeshes.contains(&mesh);

            if (!validateResident(object))
            {
                return std::nullopt;
            }

            if (!known)
            {
                if (!addDependency(mesh, true))
                {
                    return std::nullopt;
                }

                addedMeshes.insert(&mesh);
            }

            if (!snapshot.slotGeometry.contains(&mesh))
            {
                snapshot.slotGeometry.emplace(&mesh, std::make_shared<const MeshMetadata>(metadata));
            }

            auto& slotIndices = snapshot.slotMaterialIndices.Mutable(index);
            slotIndices.clear();

            for (const auto& slot : metadata.materialSlots)
            {
                slotIndices.push_back(object.GetSlotMaterial(slot.id)->gpuIndex);
            }

            auto& record = gpuInstances[index];
            record = {};

            if (!BuildInstanceRecord(object, record))
            {
                Fail(error, "Candidate arriving mesh lacks the ray-tracing vertex attributes.");
                return std::nullopt;
            }

            record.triangleSlotBufferIndex = UINT_MAX;

            if (metadata.materialSlots.size() > 1)
            {
                auto offset = snapshot.triangleOffsets.find(&mesh);

                if (offset == snapshot.triangleOffsets.end())
                {
                    const auto first = static_cast<unsigned int>(snapshot.triangleTable.size());
                    snapshot.triangleTable.resize(snapshot.triangleTable.size() + mesh.GetIndexCount() / 3);

                    for (const auto& submesh : metadata.submeshes)
                    {
                        std::fill_n(snapshot.triangleTable.begin() + first + submesh.firstIndex / 3, submesh.indexCount / 3,
                            submesh.materialSlot);
                    }

                    offset = snapshot.triangleOffsets.emplace(&mesh, first).first;
                }

                record.triangleSlotOffset = offset->second;
                record.triangleSlotBufferIndex = 0; // Patched after successful GPU allocation.
            }

            snapshot.materialBindings.Mutable(index) = std::move(object);
        }

        std::sort(state.dependencies.begin() + static_cast<std::ptrdiff_t>(baseDependencies), state.dependencies.end(), byMesh);
        std::inplace_merge(state.dependencies.begin(), state.dependencies.begin() + static_cast<std::ptrdiff_t>(baseDependencies),
            state.dependencies.end(), byMesh);

        // Object-order passes over plain data: each resident object's slot-material offset is the running total (material
        // publication requires that layout), and the TLAS lists resident objects densely in object order with their current
        // presentation transforms.
        snapshot.instances.reserve(active.instances.size() + arriving.size());
        snapshot.renderObjectToInstance.assign(objects.size(), InvalidInstanceIndex);
        bool consistent = true;

        for (std::size_t index = 0; index < objects.size() && consistent; ++index)
        {
            const auto* mesh = snapshot.materialBindings[index].mesh;

            if (!mesh)
            {
                continue;
            }

            const auto& indices = snapshot.slotMaterialIndices[index];
            const auto registered = m_blasRegistry.meshToBottomLevel.find(mesh);
            const auto created = newMeshIndices.find(mesh);
            consistent = !indices.empty() && (registered != m_blasRegistry.meshToBottomLevel.end() || created != newMeshIndices.end()) &&
                std::all_of(indices.begin(), indices.end(), [&](unsigned int material) { return material < materialCount; });

            if (!consistent)
            {
                break;
            }

            auto& record = gpuInstances[index];
            record.slotMaterialOffset = static_cast<unsigned int>(materials.size());
            record.materialIndex = indices.front();
            materials.insert(materials.end(), indices.begin(), indices.end());

            RayTracingInstanceDescription description{};
            description.transform = objects[index].GetPresentationTransform().currentModel;
            description.instanceID = static_cast<unsigned int>(index);
            description.mask = 0xFF;
            description.bottomLevelIndex = registered != m_blasRegistry.meshToBottomLevel.end() ? registered->second : created->second;
            snapshot.renderObjectToInstance[index] = snapshot.instances.size();
            snapshot.instances.push_back(description);
        }

        if (!consistent || materials.empty())
        {
            Fail(error, "Candidate incremental tables are inconsistent with the active membership.");
            return std::nullopt;
        }

        triangles = snapshot.triangleTable;

        if (!chargeTopLevel(snapshot.instances.size()))
        {
            return std::nullopt;
        }

        if (!Charge((triangles.size() + materials.size()) * sizeof(unsigned int), MaxCandidateTableBytes, tableBytes))
        {
            Fail(error, "Candidate lookup tables exceed their budget.");
            return std::nullopt;
        }

        tablesBuilt = true;
    }

    if (!tablesBuilt ||
        !snapshot.triangleSlots.InitializeStorage(device, sizeof(unsigned int), static_cast<UINT>(triangles.size()), heap) ||
        !snapshot.slotMaterials.InitializeStorage(device, sizeof(unsigned int), static_cast<UINT>(materials.size()), heap) ||
        !snapshot.instanceDataBuffer.InitializeStorage(device, sizeof(RayTracingInstanceData), static_cast<UINT>((std::max)(objects.size(),std::size_t{1})), heap))
    {
        // The capacity check above makes this unlikely, but a storage failure with an
        // empty heap is still exhaustion. The pinned candidate releases what it allocated.
        if (failure && heap.GetAvailableCount() == 0)
        {
            *failure = CandidateFailure::DescriptorExhausted;
        }

        if (error.empty() || tablesBuilt)
        {
            Fail(error, "Candidate membership/table allocation failed.");
        }

        return std::nullopt;
    }

    PatchTableDescriptors(snapshot, gpuInstances);
    snapshot.instanceRecords = gpuInstances;
    if(gpuInstances.empty()) gpuInstances.resize(1);
    state.uploadBytes = {gpuInstances.size() * sizeof(RayTracingInstanceData),
        triangles.size() * sizeof(unsigned int), materials.size() * sizeof(unsigned int)};

    if (!CreateUpload(device, gpuInstances.data(), state.uploadBytes[0], state.uploads[0]) ||
        !CreateUpload(device, triangles.data(), state.uploadBytes[1], state.uploads[1]) ||
        !CreateUpload(device, materials.data(), state.uploadBytes[2], state.uploads[2]))
    {
        Fail(error, "Candidate staging allocation failed.");
        return std::nullopt;
    }

    std::sort(state.proposalOriginals.begin(), state.proposalOriginals.end(),
        [](const auto& left, const auto& right) { return left.first < right.first; });
    state.proposals = proposals;
    state.failed = false;

    if (failure)
    {
        *failure = CandidateFailure::None;
    }

    ReplacementCandidate result;
    result.m_state = m_candidate;
    return result;
}

bool RayTracingSceneResources::RecordReplacementCandidate(ID3D12Device5* device,
    ID3D12GraphicsCommandList4* commands, std::uint64_t generation, std::string& error)
{
    if (!m_candidate || m_candidate->failed || !commands || !generation || m_candidate->generation || m_candidate->fence)
    {
        return Fail(error, "Candidate recording state is invalid.");
    }

    auto& state = *m_candidate;
    state.generation = generation; // Pin before the first build/copy command.
    state.failed = true;

    for (std::size_t index = 0; index < state.newMeshes.size(); ++index)
    {
        RayTracingBottomLevelDescription description;
        description.geometries.push_back({state.newMeshes[index], true});
        description.buildPreference = RayTracingAccelerationStructureBuildPreference::FastTrace;

        auto& bottomLevel = *state.newBLAS[index];

        if (!bottomLevel.Initialize(device, commands, description))
        {
            return Fail(error, "Candidate BLAS recording failed.");
        }
    }

    for (auto& dependency : state.dependencies)
    {
        dependency.blasResource = dependency.blas->GetResource();
        dependency.blasAddress = dependency.blas->GetGPUAddress();
    }

    auto& snapshot = *state.snapshot;
    const std::array<ID3D12Resource*, 3> destinations{snapshot.instanceDataBuffer.GetResource(),
        snapshot.triangleSlots.GetResource(), snapshot.slotMaterials.GetResource()};

    for (std::size_t index = 0; index < destinations.size(); ++index)
    {
        commands->CopyBufferRegion(destinations[index], 0, state.uploads[index].Get(), 0, state.uploadBytes[index]);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destinations[index];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        commands->ResourceBarrier(1, &barrier);
    }

    RayTracingTopLevelDescription description;
    description.instances = snapshot.instances;
    description.buildPreference = RayTracingAccelerationStructureBuildPreference::FastTrace;

    if (!snapshot.instances.empty() && !snapshot.topLevel.Initialize(device, commands, description, state.blasPointers))
    {
        return Fail(error, "Candidate TLAS recording failed.");
    }

    state.failed = false;
    return true;
}

bool RayTracingSceneResources::ValidateCandidateBindings(const CandidateState& state, const FileSceneRuntime& runtime,
    FileSceneAssetCatalog& catalog, std::string& error) const
{
    // Whole-scene revalidation: shared materials and sources are checked once per call, and so is the scene metadata
    // (S0.3: one validation for the per-object lookups below).
    const FileSceneAssetCatalog::ValidationPass validationPass(catalog);
    const FileSceneRuntime::MetadataValidationPass metadataPass(runtime);
    const auto& objects = runtime.GetScene().GetRenderObjects();

    if (!state.valid || state.scene != &runtime.GetScene() || state.catalog != &catalog ||
        state.originalMembership != m_membership.get() || state.runtimeIdentity != runtime.GetRuntimeIdentity() ||
        state.residencyRevision != runtime.GetResidencyRevision() ||
        state.structureRevision != runtime.GetScene().GetStructureRevision() || objects.size() != state.bindingRevisions.size())
    {
        return Fail(error, "Candidate belongs to an obsolete scene session or membership.");
    }

    // W6.1: an unchanged authored stamp means every persistent ID, reference and override is the one preparation read; any
    // document edit makes the candidate stale (it is prepared again). Runtime binding changes advance binding revisions.
    if (runtime.GetAuthoredStamp() != state.authoredStamp)
    {
        return Fail(error, "Candidate scene document changed after preparation.");
    }

    if (runtime.GetObjectMapping().bindingRevisions != state.bindingRevisions || !runtime.ValidateObjectMapping(&error))
    {
        return Fail(error, "Candidate object identity or material binding changed.");
    }

    for (const auto& reference : *state.materialReferences)
    {
        if (!catalog.ValidateMetadataMaterialReference(reference, &error))
        {
            return false;
        }
    }

    // Live bindings: every object still holds what preparation copied (a proposal its pre-residency binding).
    auto proposal = state.proposalOriginals.begin();

    for (std::size_t index = 0; index < objects.size(); ++index)
    {
        const RenderObject* original = &state.snapshot->materialBindings[index];

        if (proposal != state.proposalOriginals.end() && proposal->first == index)
        {
            original = &proposal->second;
            ++proposal;
        }

        if (objects[index].mesh != original->mesh || objects[index].material != original->material ||
            objects[index].slotMaterials != original->slotMaterials)
        {
            return Fail(error, "Candidate object identity or material binding changed.");
        }
    }

    for (const auto& proposal : state.proposals)
    {
        const auto identity = runtime.GetObjectMetadata(proposal.objectID, &error);

        if (!identity || !catalog.ValidateMeshResidencyCandidate(proposal.mesh, identity->meshReference, &error))
        {
            return false;
        }
    }

    for (const auto& dependency : state.dependencies)
    {
        const auto& mesh = *dependency.mesh; // Scene/session checked before any asset borrow.

        if (mesh.GetVertexBufferResource() != dependency.vertices.Get() ||
            mesh.GetIndexBufferResource() != dependency.indices.Get() ||
            mesh.GetVertexBufferDescriptorIndex() != dependency.vertexDescriptor ||
            mesh.GetIndexBufferDescriptorIndex() != dependency.indexDescriptor ||
            mesh.GetVertexCount() != dependency.vertexCount || mesh.GetIndexCount() != dependency.indexCount ||
            mesh.GetMetadata() != *state.snapshot->slotGeometry.at(&mesh) ||
            dependency.blas->GetGPUAddress() != dependency.blasAddress)
        {
            return Fail(error, "Candidate geometry or stable BLAS changed.");
        }
    }

    for (std::size_t index = 0; index < objects.size(); ++index)
    {
        const auto& effective = state.snapshot->materialBindings[index];

        if (!effective.mesh)
        {
            continue;
        }

        const auto& slots = effective.mesh->GetMetadata().materialSlots;

        for (std::size_t slot = 0; slot < slots.size(); ++slot)
        {
            const auto* material = effective.GetSlotMaterial(slots[slot].id);

            if (!material || !material->pipeline || !material->texture ||
                material->gpuIndex != state.snapshot->slotMaterialIndices[index][slot])
            {
                return Fail(error, "Candidate material table indices changed.");
            }
        }
    }

    return true;
}

RayTracingSceneResources::CandidateStatus RayTracingSceneResources::PollReplacementCandidate(
    const ReplacementCandidate& handle, const CommandContext& context, const FileSceneRuntime& runtime,
    FileSceneAssetCatalog& catalog, std::string& error) const
{
    error.clear();

    if (!handle.m_state || handle.m_state != m_candidate || !handle.m_state->valid || handle.m_state->failed ||
        handle.m_state->uncertain)
    {
        Fail(error, "Candidate failed, was canceled, or has uncertain GPU submission.");
        return CandidateStatus::Failed;
    }

    const auto& state = *handle.m_state;

    if (state.generation || !state.fence)
    {
        return CandidateStatus::Pending;
    }

    const auto completed = context.GetCompletedFenceValue();

    if (completed == UINT64_MAX || state.fenceLifetime != context.GetSubmissionLifetime())
    {
        Fail(error, "Candidate GPU fence provenance is unavailable.");
        return CandidateStatus::Failed;
    }

    if (completed < state.fence)
    {
        return CandidateStatus::Pending;
    }

    if (!ValidateCandidateBindings(state, runtime, catalog, error))
    {
        return CandidateStatus::Stale;
    }

    for (const auto& instance : state.snapshot->instances)
    {
        const auto current = runtime.GetScene().GetRenderObjects()[instance.instanceID].GetPresentationTransform().currentModel;

        if (std::memcmp(current.m, instance.transform.m, sizeof(current.m)) != 0)
        {
            return CandidateStatus::NeedsTransformRefresh;
        }
    }

    return CandidateStatus::Ready;
}

bool RayTracingSceneResources::RefreshReplacementTransforms(const ReplacementCandidate& handle,
    ID3D12GraphicsCommandList4* commands, unsigned int frameSlot, std::uint64_t generation,
    const FileSceneRuntime& runtime, std::string& error)
{
    if (!handle.m_state || handle.m_state != m_candidate || !commands || !generation ||
        m_candidate->generation || m_candidate->uncertain || m_candidate->failed)
    {
        return Fail(error, "Candidate transform refresh state is invalid.");
    }

    auto& state = *m_candidate;
    auto& snapshot = *state.snapshot;
    std::vector<std::size_t> allIndices;
    allIndices.reserve(snapshot.instances.size());

    for (std::size_t index = 0; index < snapshot.instances.size(); ++index)
    {
        auto& instance = snapshot.instances[index];
        instance.transform = runtime.GetScene().GetRenderObjects()[instance.instanceID].GetPresentationTransform().currentModel;
        allIndices.push_back(index);
    }

    state.generation = generation;
    state.failed = true;

    if (!snapshot.topLevel.Update(commands, frameSlot, snapshot.instances, allIndices, state.blasPointers))
    {
        return Fail(error, "Candidate full-transform TLAS refresh failed.");
    }

    state.failed = false;
    return true;
}

bool RayTracingSceneResources::AttemptCandidateSubmission(std::uint64_t generation)
{
    if (!m_candidate || m_candidate->generation != generation)
    {
        return true;
    }

    if (m_candidate->uncertain)
    {
        return false;
    }

    m_candidate->uncertain = true;
    return true;
}

bool RayTracingSceneResources::ConfirmCandidateSubmission(std::uint64_t generation, const CommandContext& context)
{
    if (!m_candidate || m_candidate->generation != generation)
    {
        return true;
    }

    if (!m_candidate->uncertain || !context.GetLastSubmissionFenceValue() ||
        !context.GetSubmissionLifetime())
    {
        return false;
    }

    m_candidate->fence = context.GetLastSubmissionFenceValue();
    m_candidate->fenceLifetime = context.GetSubmissionLifetime();
    m_candidate->generation = 0;
    m_candidate->uncertain = false;
    return true;
}

void RayTracingSceneResources::CandidateNotSubmitted(std::uint64_t generation)
{
    if (m_candidate && m_candidate->generation == generation)
    {
        m_candidate->uncertain = false;
    }
}

void RayTracingSceneResources::DiscardCandidateRecording(std::uint64_t generation)
{
    if (m_candidate && m_candidate->generation == generation && !m_candidate->uncertain)
    {
        m_candidate->generation = 0;
        m_candidate->failed = true;
    }
}

void RayTracingSceneResources::CollectReplacementCandidate(const CommandContext& context)
{
    if (!m_candidate || m_candidate.use_count() != 1 || m_candidate->generation || m_candidate->uncertain)
    {
        return;
    }

    if (m_candidate->fence)
    {
        const auto completed = context.GetCompletedFenceValue();

        if (!completed || completed == UINT64_MAX || m_candidate->fenceLifetime != context.GetSubmissionLifetime() ||
            completed < m_candidate->fence)
        {
            return;
        }
    }

    m_candidate->Shutdown();
    m_candidate.reset();
}

void RayTracingSceneResources::ShutdownReplacementCandidate()
{
    if (m_candidate)
    {
        // Existing checked scene teardown has drained GPU work and discarded
        // old recordings. Handles become invalid without touching scene geometry.
        m_candidate->Shutdown();
        m_candidate.reset();
    }
}

bool RayTracingSceneResources::HasMatchingCandidateSubmissionState(const ReplacementCandidate& handle) const noexcept
{
    // Submission bookkeeping only: callers must separately prove fence completion,
    // current bindings/source ownership and storage prerequisites before adoption.
    return handle.m_state && handle.m_state == m_candidate && m_candidate->valid && !m_candidate->failed &&
        !m_candidate->generation && !m_candidate->uncertain && m_candidate->fence;
}

RayTracingSceneResources::AdoptionReadiness RayTracingSceneResources::PrepareCandidateAdoption(
    const ReplacementCandidate& handle, std::span<const AdoptionEntry> entries, std::size_t materialCount,
    AdoptionEvidence& evidence, std::string& error, std::string* failedObjectID)
{
    if (!HasMatchingCandidateSubmissionState(handle))
    {
        Fail(error, "Candidate GPU preparation is not ready for adoption validation.");
        return AdoptionReadiness::Stale;
    }

    auto& state = *m_candidate;

    if (m_materialBindingEpoch == UINT64_MAX)
    {
        Fail(error, "Renderer binding identities are exhausted.");
        return AdoptionReadiness::Unsupported;
    }

    if (entries.empty() || state.proposals.size() != entries.size())
    {
        Fail(error, "Scene transaction and DXR candidate propose different object/mesh identities.");
        return AdoptionReadiness::Stale;
    }

    if (materialCount != state.materialCount)
    {
        Fail(error, "Scene staged bindings or material-table index layout differs from the DXR candidate.");
        return AdoptionReadiness::Stale;
    }

    // Every entry must match the proposal recorded at the same position; one
    // mismatch rejects the whole batch before any registry staging.
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        const auto& proposal = state.proposals[index];
        const auto& entry = entries[index];
        const auto& mesh = *entry.mesh;
        const auto reject = [&](const char* message)
        {
            Fail(error, message);

            if (failedObjectID)
            {
                *failedObjectID = std::string(entry.objectID);
            }

            return AdoptionReadiness::Stale;
        };

        if (proposal.objectID != entry.objectID || proposal.mesh.key != mesh.key || proposal.mesh.mesh != mesh.mesh ||
            proposal.mesh.sourceKind != mesh.sourceKind || proposal.mesh.builtinReference != mesh.builtinReference ||
            proposal.mesh.cooked != mesh.cooked ||
            proposal.mesh.catalogIdentity != mesh.catalogIdentity || proposal.mesh.vertexAddress != mesh.vertexAddress ||
            proposal.mesh.indexAddress != mesh.indexAddress || entry.objectIndex >= state.snapshot->materialBindings.size())
        {
            return reject("Scene transaction and DXR candidate propose different object/mesh identities.");
        }

        const auto& binding = state.snapshot->materialBindings[entry.objectIndex];

        if (binding.mesh != mesh.mesh || binding.material != entry.fallback || binding.slotMaterials != *entry.slots)
        {
            return reject("Scene staged bindings or material-table index layout differs from the DXR candidate.");
        }
    }

    return PrepareRegistryAdoption(handle, evidence, error);
}

RayTracingSceneResources::AdoptionReadiness RayTracingSceneResources::PrepareRegistryAdoption(
    const ReplacementCandidate& handle, AdoptionEvidence& evidence, std::string& error)
{
    // Keep epoch exhaustion and retirement availability local: this gate reports
    // Stale, whereas the scene-side adoption gate reports epoch exhaustion as Unsupported.
    if (!HasMatchingCandidateSubmissionState(handle) ||
        m_materialBindingEpoch == UINT64_MAX || !CanRetireActiveMembership())
    {
        Fail(error, "Candidate registry adoption is not ready.");
        return AdoptionReadiness::Stale;
    }

    auto& state = *m_candidate;
    const auto base = state.stableBLASCount;
    const auto added = state.newBLAS.size();
    const auto needed = base + added;

    if (needed > MaxCandidateObjects)
    {
        Fail(error, "Stable BLAS adoption exceeds the scene registry work bound.");
        return AdoptionReadiness::Unsupported;
    }

    if (m_blasRegistry.bottomLevels.size() != base || m_blasRegistry.meshToBottomLevel.size() != base ||
        m_blasRegistry.bottomLevelPointers.size() != base || state.blasPointers.size() != needed ||
        state.newMeshes.size() != added)
    {
        Fail(error, "Stable BLAS registry membership changed after candidate preparation.");
        return AdoptionReadiness::Stale;
    }

    for (std::size_t index = 0; index < base; ++index)
    {
        if (!m_blasRegistry.bottomLevels[index] ||
            m_blasRegistry.bottomLevels[index].get() != state.blasPointers[index] ||
            m_blasRegistry.bottomLevelPointers[index] != state.blasPointers[index])
        {
            Fail(error, "Stable BLAS ownership or registry indexing changed.");
            return AdoptionReadiness::Stale;
        }
    }

    // Stage the prospective registry without changing existing BLAS ownership.
    auto indices = m_blasRegistry.meshToBottomLevel;
    indices.reserve(needed);

    for (std::size_t index = 0; index < added; ++index)
    {
        const auto* blas = state.newBLAS[index].get();

        if (!blas || !blas->GetResource() || !blas->GetGPUAddress() ||
            state.blasPointers[base + index] != blas || !indices.emplace(state.newMeshes[index], base + index).second)
        {
            Fail(error, "Candidate new BLAS is incomplete, already adopted or inconsistently indexed.");
            return AdoptionReadiness::Stale;
        }
    }

    for (const auto& instance : state.snapshot->instances)
    {
        const auto* residentMesh = state.snapshot->materialBindings[instance.instanceID].mesh;
        const auto registered = indices.find(residentMesh);

        if (registered == indices.end() || registered->second != instance.bottomLevelIndex)
        {
            Fail(error, "Candidate TLAS indices do not match the prospective BLAS registry.");
            return AdoptionReadiness::Stale;
        }
    }

    if (!state.adoptionLifetime)
    {
        state.adoptionLifetime = ReserveBLASRegistryLifetime();
    }

    if (!state.adoptionLifetime)
    {
        Fail(error, "Stable BLAS registry lifetime identities are exhausted.");
        return AdoptionReadiness::Unsupported;
    }

    if (m_blasRegistry.retirementLifetime && state.adoptionLifetime != m_blasRegistry.retirementLifetime)
    {
        Fail(error, "Stable BLAS registry lifetime changed.");
        return AdoptionReadiness::Stale;
    }

    // Capacity-only changes preserve registry elements, indices and pointees.
    // Every fallible allocation belongs to readiness, before final acceptance.
    m_blasRegistry.bottomLevels.reserve(needed);
    m_blasRegistry.bottomLevelPointers.reserve(needed);
    static_assert(std::is_nothrow_swappable_v<decltype(state.adoptionIndices)>);
    static_assert(std::is_nothrow_move_constructible_v<std::unique_ptr<BottomLevelAccelerationStructure>>);
    static_assert(std::is_nothrow_copy_assignable_v<std::shared_ptr<const CandidateState>>);
    state.adoptionIndices.swap(indices);

    std::size_t retirementSlot = 0;

    while (retirementSlot < m_retiredMemberships.size() && m_retiredMemberships[retirementSlot])
    {
        ++retirementSlot;
    }

    // Check() established capacity on this same exclusive main-thread boundary.
    assert(retirementSlot < m_retiredMemberships.size());
    evidence = {state.fence, state.fenceLifetime, base, added, retirementSlot};
    return AdoptionReadiness::Ready;
}

void RayTracingSceneResources::AdoptPreparedCandidate(
    const ReplacementCandidate& candidate, const AdoptionEvidence& evidence) noexcept
{
    assert(candidate.m_state && candidate.m_state == m_candidate);
    auto& state = *m_candidate;
    const auto needed = evidence.stableBLASCount + evidence.newBLASCount;

    assert(state.valid && !state.failed && !state.uncertain && !state.generation);
    assert(state.fence == evidence.fence && state.fenceLifetime == evidence.fenceLifetime);
    assert(state.snapshot && state.originalMembership == m_membership.get());
    assert(CanRetireActiveMembership() && evidence.retirementSlot < m_retiredMemberships.size());
    assert(!m_retiredMemberships[evidence.retirementSlot]);

    assert(m_blasRegistry.bottomLevels.size() == evidence.stableBLASCount);
    assert(m_blasRegistry.bottomLevels.capacity() >= needed);
    assert(m_blasRegistry.bottomLevelPointers.capacity() >= needed);
    assert(state.newBLAS.size() == evidence.newBLASCount);

    // No node insertion or vector growth: pointees and existing BLAS indices
    // remain stable. The full prospective registry map was built by readiness.
    m_blasRegistry.meshToBottomLevel.swap(state.adoptionIndices);

    for (auto& blas : state.newBLAS)
    {
        m_blasRegistry.bottomLevelPointers.push_back(blas.get());
        m_blasRegistry.bottomLevels.push_back(std::move(blas));
    }

    m_blasRegistry.retirementLifetime = state.adoptionLifetime;

    // Moving the snapshot out breaks the candidate -> snapshot -> candidate
    // cycle before it can escape. Pins remain owned by the active snapshot.
    state.snapshot->dependencyOwner = m_candidate;
    state.snapshot->lastUseFence = evidence.fence;
    state.snapshot->fenceLifetime = evidence.fenceLifetime;

    // Preserve the OLD snapshot's recording/fence provenance verbatim. No
    // descriptor destruction occurs here; normal checked collection retires it.
    m_retiredMemberships[evidence.retirementSlot] = std::move(m_membership);
    m_membership = std::move(state.snapshot);
    ++m_materialBindingEpoch; // Invalidate prepared binding tables across membership replacement (including ABA).

    // Candidate copy/build completion was proved before commit. Upload buffers
    // are no longer GPU dependencies; geometry and BLAS pins remain retained.
    for (auto& upload : state.uploads)
    {
        upload.Reset();
    }

    state.valid = false;
    m_candidate.reset(); // Active snapshot now retains the candidate dependency owner.
}
