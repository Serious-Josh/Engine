#include "RayTracingSceneResources.h"
#include "Scene/FileSceneRuntime.h"

#include "Renderer/Mesh.h"
#include "Renderer/Caustics/CausticGeometrySourceManifest.h"
#include "Renderer/RayTracing/BLASGeometryDescription.h"
#include "Renderer/CommandContext.h"
#include "Renderer/RayTracing/RayTracingAccelerationStructure.h"
#include "Renderer/RenderObject.h"
#include "Scene/Scene.h"
#include "Renderer/Material.h"
#include "Renderer/ShaderData.h"

#include <cstring>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <limits>
#include "Core/Log.h"

namespace
{
    std::uint64_t AllocateSceneRetirementLifetime()
    {
        static std::atomic<std::uint64_t> next{1};
        auto value = next.load(std::memory_order_relaxed);
        while (value != UINT64_MAX)
        {
            if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            {
                return value;
            }
        }

        return 0;
    }

    // The build read exactly the generation's resources, with the descriptor the
    // shared helper derives from the generation's own values (single geometry,
    // no override, opaque, no transform). Never derived from current Mesh state.
    bool CaptureMatchesGeneration(const BLASBuildCapture& capture,
        const CausticContracts::Geometry::MeshSourceGeneration& source) noexcept
    {
        if (capture.identity.buildID == 0 || capture.geometryCount != 1 || capture.countOverride ||
            !capture.result || !capture.scratch || capture.vertices.Get() != source.VertexResource() ||
            capture.indices.Get() != source.IndexResource())
        {
            return false;
        }

        BLASTriangleGeometryInputs inputs{};
        inputs.vertexAddress = source.VertexResource()->GetGPUVirtualAddress();
        inputs.vertexStride = source.VertexStride();
        inputs.vertexCount = source.Source().Vertices().size();
        inputs.indexAddress = source.IndexResource()->GetGPUVirtualAddress();
        inputs.indexFormat = source.IndexFormat();
        inputs.indexCount = source.Source().Indices().size();
        inputs.opaque = true;
        D3D12_RAYTRACING_GEOMETRY_DESC expected{};

        if (!BuildBLASTriangleGeometryDescription(inputs, expected))
        {
            return false;
        }

        const auto& actual = capture.geometry.Triangles;
        return capture.geometry.Type == expected.Type && capture.geometry.Flags == expected.Flags &&
               actual.Transform3x4 == 0 &&
               actual.VertexBuffer.StartAddress == expected.Triangles.VertexBuffer.StartAddress &&
               actual.VertexBuffer.StrideInBytes == expected.Triangles.VertexBuffer.StrideInBytes &&
               actual.VertexCount == expected.Triangles.VertexCount &&
               actual.VertexFormat == expected.Triangles.VertexFormat &&
               actual.IndexBuffer == expected.Triangles.IndexBuffer &&
               actual.IndexCount == expected.Triangles.IndexCount &&
               actual.IndexFormat == expected.Triangles.IndexFormat;
    }
} // namespace

bool RayTracingSceneResources::Initialize(
    ID3D12Device5* device,
    ID3D12GraphicsCommandList4* commandList,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const Scene& scene)
{
    m_blasRegistry.retirementLifetime = 0;

    if (!device || !commandList)
    {
        return false;
    }

    const auto lifetime = AllocateSceneRetirementLifetime();

    if (lifetime == 0)
    {
        return false;
    }

    m_recordingOwner = lifetime;
    const std::vector<RenderObject>& renderObjects = scene.GetRenderObjects();

    for (const RenderObject& renderObject : renderObjects)
    {
        if (!renderObject.mesh)
        {
            continue;
        }

        if (m_blasRegistry.meshToBottomLevel.contains(renderObject.mesh))
        {
            continue;
        }

        RayTracingBottomLevelDescription bottomLevelDescription{};

        RayTracingGeometryDescription geometryDescription{};
        geometryDescription.mesh = renderObject.mesh;
        geometryDescription.opaque = true;

        bottomLevelDescription.geometries.push_back(geometryDescription);
        bottomLevelDescription.buildPreference = RayTracingAccelerationStructureBuildPreference::FastTrace;

        auto bottomLevel = std::make_unique<BottomLevelAccelerationStructure>();

        if (!bottomLevel->Initialize(device, commandList, bottomLevelDescription))
        {
            return false;
        }

        AttributeRecordedBuild(*bottomLevel);
        AssociateRecordedBuild(device, *renderObject.mesh, *bottomLevel);
        const std::size_t bottomLevelIndex = m_blasRegistry.bottomLevels.size();

        m_blasRegistry.meshToBottomLevel.emplace(renderObject.mesh, bottomLevelIndex);

        m_blasRegistry.bottomLevels.push_back(std::move(bottomLevel));
    }

    if (m_blasRegistry.bottomLevels.empty())
    {
        return false;
    }

    m_membership->instances.clear();

    m_membership->instances.reserve(renderObjects.size());

    m_membership->renderObjectToInstance.clear();

    m_membership->renderObjectToInstance.resize(renderObjects.size(), InvalidInstanceIndex);

    std::vector<RayTracingInstanceData> instanceData(renderObjects.size());

    if(!InitializeMaterialSlots(device, uploader, descriptorHeap, scene, instanceData))
    {
        return false;
    }

    if (!BuildInstanceData(*m_membership, renderObjects, m_blasRegistry.meshToBottomLevel, instanceData))
    {
        return false;
    }
    m_membership->instanceRecords = instanceData;

    if (!m_membership->instanceDataBuffer.Initialize(device, uploader, instanceData.data(), sizeof(RayTracingInstanceData),
        static_cast<unsigned int>(instanceData.size()), descriptorHeap))
    {
        return false;
    }

    RayTracingTopLevelDescription topLevelDescription{};
    topLevelDescription.instances = m_membership->instances;
    topLevelDescription.buildPreference = RayTracingAccelerationStructureBuildPreference::FastTrace;

    m_blasRegistry.bottomLevelPointers.clear();
    m_blasRegistry.bottomLevelPointers.reserve(m_blasRegistry.bottomLevels.size());

    for (const auto& bottomLevel : m_blasRegistry.bottomLevels)
    {
        m_blasRegistry.bottomLevelPointers.push_back(bottomLevel.get());
    }

    if (!m_membership->topLevel.Initialize(device, commandList, topLevelDescription, m_blasRegistry.bottomLevelPointers))
    {
        return false;
    }

    AttributeRecordedTLAS(m_membership->topLevel);

    m_blasRegistry.retirementLifetime = lifetime;
    return true;
}

bool RayTracingSceneResources::Update(
    ID3D12GraphicsCommandList4* commandList,
    unsigned int frameIndex,
    const Scene& scene)
{
    if (!commandList)
    {
        return false;
    }

    const std::vector<RenderObject>& renderObjects = scene.GetRenderObjects();

    // Merge independent physics dirtiness without changing authored transform history.
    std::vector<RenderObjectID> changedTransforms = scene.GetChangedTransforms();
    const auto& changedPresentation = scene.GetChangedPresentationTransforms();
    changedTransforms.insert(changedTransforms.end(), changedPresentation.begin(), changedPresentation.end());
    std::sort(changedTransforms.begin(), changedTransforms.end());
    changedTransforms.erase(std::unique(changedTransforms.begin(), changedTransforms.end()), changedTransforms.end());

    if (renderObjects.size() != m_membership->renderObjectToInstance.size())
    {
        return false;
    }

    std::vector<std::size_t> changedInstanceIndices;
    changedInstanceIndices.reserve(changedTransforms.size());

    for (RenderObjectID renderObjectID : changedTransforms)
    {
        if (renderObjectID >= renderObjects.size())
        {
            return false;
        }

        const std::size_t instanceIndex = m_membership->renderObjectToInstance[renderObjectID];

        if (instanceIndex == InvalidInstanceIndex)
        {
            continue;
        }

        if (instanceIndex >= m_membership->instances.size())
        {
            return false;
        }

        m_membership->instances[instanceIndex].transform = renderObjects[renderObjectID].GetPresentationTransform().currentModel;

        changedInstanceIndices.push_back(instanceIndex);
    }

    OrderRecordedGeometryReaders(commandList);
    const bool recorded = m_membership->topLevel.Update(commandList, frameIndex, m_membership->instances,
        changedInstanceIndices, m_blasRegistry.bottomLevelPointers);

    if(recorded)
    {
        AttributeRecordedTLAS(m_membership->topLevel);
    }

    return recorded;
}

bool RayTracingSceneResources::UpdateTransformsAndRefreshTLASReferences(
    ID3D12GraphicsCommandList4* commandList,
    unsigned int frameIndex,
    const Scene& scene)
{
    if (!commandList || m_membership->instances.empty() || m_blasRegistry.bottomLevelPointers.empty())
    {
        return false;
    }

    const auto& objects = scene.GetRenderObjects();

    if (objects.size() != m_membership->renderObjectToInstance.size())
    {
        return false;
    }

    for (RenderObjectID id : scene.GetChangedTransforms())
    {
        if (id >= objects.size())
        {

            return false;
        }
    }

    // Read all transforms, including instances not in the dirty list. Validate
    // registration before publishing any scene-level instance changes.

    auto instances = m_membership->instances;

    std::vector<std::size_t> allInstanceIndices;
    allInstanceIndices.reserve(instances.size());

    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
    {
        const RenderObject& object = objects[objectIndex];
        const std::size_t instanceIndex = m_membership->renderObjectToInstance[objectIndex];

        if (instanceIndex == InvalidInstanceIndex)
        {
            if (object.mesh)
            {
                return false; // Structural change requires scene reconstruction.
            }

            continue;
        }

        if (!object.mesh || instanceIndex >= instances.size() || instances[instanceIndex].instanceID != objectIndex)
        {
            return false;
        }

        const auto registered = m_blasRegistry.meshToBottomLevel.find(object.mesh);

        const std::size_t bottomLevelIndex = instances[instanceIndex].bottomLevelIndex;

        if (registered == m_blasRegistry.meshToBottomLevel.end() ||
            registered->second != bottomLevelIndex ||
            bottomLevelIndex >= m_blasRegistry.bottomLevelPointers.size() ||
            !m_blasRegistry.bottomLevelPointers[bottomLevelIndex])
        {
            return false;
        }

        if (m_blasRegistry.bottomLevelPointers[bottomLevelIndex]->GetGPUAddress() == 0)
        {
            return false;
        }

        instances[instanceIndex].transform = object.GetPresentationTransform().currentModel;
        allInstanceIndices.push_back(instanceIndex);
    }

    if (allInstanceIndices.size() != instances.size())
    {
        return false;
    }

    // TLAS Update resolves current BLAS addresses for every supplied instance,
    // propagates pending entries to both slots, and maps only the selected slot
    // once before recording its single build and existing UAV barrier.

    m_membership->instances.swap(instances);
    OrderRecordedGeometryReaders(commandList);
    const bool recorded = m_membership->topLevel.Update(commandList, frameIndex, m_membership->instances,
        allInstanceIndices, m_blasRegistry.bottomLevelPointers);

    if(recorded)
    {
        AttributeRecordedTLAS(m_membership->topLevel);
    }

    return recorded;
}

bool RayTracingSceneResources::RebuildMeshBLAS(
    ID3D12Device5* device,
    ID3D12GraphicsCommandList4* commandList,
    const Mesh* mesh)
{
    return RebuildMeshBLASInternal(device, commandList, mesh, std::nullopt);
}

bool RayTracingSceneResources::RebuildMeshBLAS(
    ID3D12Device5* device,
    ID3D12GraphicsCommandList4* commandList,
    const Mesh* mesh,
    const RayTracingGeometryCounts& counts)
{
    return RebuildMeshBLASInternal(device, commandList, mesh, counts);
}

bool RayTracingSceneResources::RebuildMeshBLASInternal(
    ID3D12Device5* device,
    ID3D12GraphicsCommandList4* commandList,
    const Mesh* mesh,
    const std::optional<RayTracingGeometryCounts>& countOverride)
{
    if (!device || !commandList || !mesh)
    {
        return false;
    }

    const auto it = m_blasRegistry.meshToBottomLevel.find(mesh);

    if (it == m_blasRegistry.meshToBottomLevel.end())
    {
        return false;
    }

    const std::size_t bottomLevelIndex = it->second;

    if (bottomLevelIndex >= m_blasRegistry.bottomLevels.size() || !m_blasRegistry.bottomLevels[bottomLevelIndex])
    {
        return false;
    }

    RayTracingBottomLevelDescription description{};

    RayTracingGeometryDescription geometry{};
    geometry.mesh = mesh;
    geometry.opaque = true;
    geometry.countOverride = countOverride;

    description.geometries.push_back(geometry);

    description.buildPreference = RayTracingAccelerationStructureBuildPreference::FastTrace;

    auto& bottomLevel = *m_blasRegistry.bottomLevels[bottomLevelIndex];

    if (!bottomLevel.Rebuild(device, commandList, description))
    {
        return false;
    }

    AttributeRecordedBuild(bottomLevel);
    AssociateRecordedBuild(device, *mesh, bottomLevel); // count overrides are ineligible
    return true;
}

void RayTracingSceneResources::AttributeRecordedBuild(BottomLevelAccelerationStructure& bottomLevel)
{
    const std::uint64_t generation = m_membership ? m_membership->recordingGeneration : 0;

    if (m_recordingOwner == 0 || generation == 0)
    {
        return; // No truthful recording context: leave the build Unattributed.
    }

    bottomLevel.AttributeCurrentBuild(bottomLevel.GetCurrentBuildCapture().identity, {m_recordingOwner, generation});
}

void RayTracingSceneResources::AssociateRecordedBuild(ID3D12Device5* device, const Mesh& mesh,
    const BottomLevelAccelerationStructure& bottomLevel) noexcept
{
    // Opt-in: only a mesh whose current P2.1 generation is still current.
    const auto source = mesh.GetStaticSourceGeneration();

    if (!source || !mesh.IsCurrentStaticSource(source))
    {
        return;
    }

    const BLASBuildCapture& capture = bottomLevel.GetCurrentBuildCapture();
    const BLASBuildRecording& recording = bottomLevel.GetCurrentBuildRecording();
    const bool eligible = device && recording.state == BLASRecordingState::Recorded &&
        recording.provenance.recordingOwner != 0 && CaptureMatchesGeneration(capture, *source);

    if (!eligible)
    {
        ++m_associationRejections.rejectedIneligible;
        return;
    }

    const D3D12_RESOURCE_DESC resultDescription = capture.result->GetDesc();
    const D3D12_RESOURCE_DESC scratchDescription = capture.scratch->GetDesc();
    const auto resultInfo = device->GetResourceAllocationInfo(0, 1, &resultDescription);
    const auto scratchInfo = device->GetResourceAllocationInfo(0, 1, &scratchDescription);

    if (resultInfo.SizeInBytes == UINT64_MAX || scratchInfo.SizeInBytes == UINT64_MAX ||
        resultInfo.SizeInBytes > UINT64_MAX - scratchInfo.SizeInBytes)
    {
        ++m_associationRejections.rejectedBudget;
        return;
    }

    const std::uint64_t charge = resultInfo.SizeInBytes + scratchInfo.SizeInBytes;

    // Live records include externally held historical records.
    AssociationSlot* freeSlot = nullptr;
    std::uint64_t liveBytes = 0;

    for (auto& slot : m_associations)
    {
        const auto live = slot.owned ? slot.owned : slot.tracked.lock();

        if (live)
        {
            liveBytes += live->blasCharge; // each charge <= budget, at most 8 records
        }
        else if (!freeSlot)
        {
            freeSlot = &slot;
        }
    }

    if (!freeSlot)
    {
        ++m_associationRejections.rejectedCapacity;
        return;
    }

    if (charge > m_associationBudget || liveBytes > m_associationBudget - charge)
    {
        ++m_associationRejections.rejectedBudget;
        return;
    }

    std::shared_ptr<BLASAssociationRecord> record;

    try
    {
        record = std::make_shared<BLASAssociationRecord>();
    }
    catch (const std::bad_alloc&)
    {
        ++m_associationRejections.rejectedAllocation;
        return;
    }

    record->source = source;
    record->capture = capture;
    record->provenance = recording.provenance;
    record->sceneLifetime = m_recordingOwner;
    record->blasCharge = charge;

    *freeSlot = {};
    freeSlot->owned = record;
    freeSlot->tracked = record;
}

const BLASBuildRecording* RayTracingSceneResources::FindAssociatedBuild(
    const BLASAssociationRecord& record, bool& current) const noexcept
{
    current = false;
    const auto& identity = record.capture.identity;

    for (const auto& bottomLevel : m_blasRegistry.bottomLevels)
    {
        if (!bottomLevel)
        {
            continue;
        }

        const auto& currentIdentity = bottomLevel->GetCurrentBuildCapture().identity;

        if (currentIdentity.ownerLifetime == identity.ownerLifetime && currentIdentity.buildID == identity.buildID)
        {
            current = true;
            return &bottomLevel->GetCurrentBuildRecording();
        }

        for (std::size_t index = 0; const auto* retired = bottomLevel->GetRetiredBuildCapture(index); ++index)
        {
            if (retired->identity.ownerLifetime == identity.ownerLifetime && retired->identity.buildID == identity.buildID)
            {
                return bottomLevel->GetRetiredBuildRecording(index);
            }
        }
    }

    // Not yet adopted candidate builds are never current.
    return FindCandidateBuildRecording(identity);
}

bool RayTracingSceneResources::RegisterAssociationReader(const BLASAssociationHandle& record, std::uint64_t fence,
    std::uint64_t lifetime) noexcept
{
    // A reader without a confirmed fence (including an uncertain submission) is
    // never registered; such a reader keeps the caller's own pins instead.
    if (!record || fence == 0 || fence == UINT64_MAX || lifetime == 0)
    {
        return false;
    }

    for (auto& slot : m_associations)
    {
        if (slot.owned != record)
        {
            continue;
        }

        bool current = false;
        const auto* build = FindAssociatedBuild(*record, current);

        if (!build || build->state != BLASRecordingState::Completed)
        {
            return false;
        }

        for (auto& reader : slot.readers)
        {
            if (reader.fence == 0)
            {
                reader = {fence, lifetime};
                return true;
            }
        }

        return false;
    }

    return false;
}

void RayTracingSceneResources::CollectAssociations(std::uint64_t completed, std::uint64_t lifetime) noexcept
{
    for (auto& slot : m_associations)
    {
        if (!slot.owned)
        {
            continue;
        }

        // Only a valid completed fence in the reader's own lifetime retires it.
        // A wrong-lifetime reader stays until checked teardown.
        bool readersPending = false;

        for (auto& reader : slot.readers)
        {
            if (reader.fence != 0 && reader.lifetime == lifetime && completed >= reader.fence)
            {
                reader = {};
            }

            readersPending = readersPending || reader.fence != 0;
        }

        bool current = false;
        const auto* build = FindAssociatedBuild(*slot.owned, current);

        // A missing build was destroyed by checked collection/teardown. Uncertain,
        // unsubmitted and unproven builds keep ledger ownership.
        const bool resolved = !build || build->state == BLASRecordingState::Completed ||
                              build->state == BLASRecordingState::Discarded;

        // Completion does not supersede a prospective build. The manager must
        // still own this exact build and a candidate handle must remain alive;
        // an external association record alone cannot protect it. Checked
        // candidate collection runs first and removes abandoned builds once
        // recording/submission obligations permit it. Discarded builds need no
        // prospective protection even while a failed candidate handle survives.
        const bool prospective = build && build->state == BLASRecordingState::Completed &&
            m_candidate && m_candidate.use_count() > 1 &&
            FindCandidateBuildRecording(slot.owned->capture.identity) == build;

        if(!readersPending && !current && !prospective && resolved)
        {
            slot.owned.reset(); // tracked keeps counting any external holder
        }
    }
}

BLASAssociationStatistics RayTracingSceneResources::GetAssociationStatistics() const noexcept
{
    BLASAssociationStatistics statistics = m_associationRejections;

    for (const auto& slot : m_associations)
    {
        const auto live = slot.owned ? slot.owned : slot.tracked.lock();

        if (!live)
        {
            continue;
        }

        ++statistics.liveRecords;
        statistics.ledgerOwned += slot.owned ? 1 : 0;
        statistics.blasBytes += live->blasCharge;
        statistics.sourceBytes += live->source->Source().SourceBytes();
        statistics.sourceResourceBytes += live->source->AllocatedResourceBytes();

        for (const auto& reader : slot.readers)
        {
            statistics.pendingReaders += reader.fence != 0 ? 1 : 0;
        }
    }

    return statistics;
}

bool RayTracingSceneResources::MatchesAcceptedGeometry(const AcceptedTLASGeometry& accepted,
    const FileSceneRuntime& runtime, const Scene* rendererScene, std::uint64_t rendererLifetime) const noexcept
{
    const auto& join = accepted.geometry;
    if(!join || accepted.runtime != &runtime || !rendererLifetime || join->rendererLifetime != rendererLifetime ||
        !m_recordingOwner || join->recordingOwner != m_recordingOwner || rendererScene != &runtime.GetScene() ||
        join->runtimeIdentity != runtime.GetRuntimeIdentity() || !m_membership || m_membership->submissionUncertain ||
        m_membership->recordingGeneration != accepted.currentFrameGeneration || accepted.bindingEpoch != m_materialBindingEpoch ||
        accepted.residencyRevision != runtime.GetResidencyRevision() ||
        join->structureRevision != runtime.GetScene().GetStructureRevision() ||
        join->hierarchyRevision != runtime.GetHierarchyRevision() ||
        join->tlasCapture != m_membership->topLevel.GetRecordingCapture())
    {
        return false;
    }
    TLASSubmissionEvidence evidence;
    // P5B: a current-frame record's capture is still Recorded in that generation (no fence yet).
    const auto expectedState = accepted.currentFrameGeneration ? TLASRecordingState::Recorded : TLASRecordingState::Completed;
    if(!m_membership->topLevel.GetSubmissionEvidence(join->tlasCapture, evidence) ||
        evidence.state != expectedState || evidence.provenance != join->submission.provenance ||
        evidence.ownerLifetime != join->submission.ownerLifetime || evidence.recordingIdentity != join->submission.recordingIdentity ||
        evidence.submissionFence != join->submission.submissionFence || evidence.submissionLifetime != join->submission.submissionLifetime ||
        (accepted.currentFrameGeneration && evidence.provenance.generation != accepted.currentFrameGeneration))
    {
        return false;
    }
    const auto& mapping = runtime.GetObjectMapping();
    const auto& document = runtime.GetFileScene().GetDocument();
    const auto& objects = runtime.GetScene().GetRenderObjects();
    const auto count = join->rows.size();
    if(document.id != join->sceneID || document.objects.size() != count || objects.size() != count ||
        mapping.scene != rendererScene || mapping.structureRevision != join->structureRevision ||
        mapping.idToRuntimeIndex.size() != count || mapping.bindingRevisions.size() != count ||
        mapping.committedObjects.size() != count || m_membership->materialBindings.size() != count)
    {
        return false;
    }
    // Iterate the already committed nodes. No hashing or construction of metadata
    // receipts is permitted in the final noexcept publication transaction.
    for(const auto& [objectID, index] : mapping.idToRuntimeIndex)
    {
        if(index >= count || join->rows[index].objectID != objectID)
        {
            return false;
        }
    }
    for(std::size_t index = 0; index < count; ++index)
    {
        const auto& row = join->rows[index];
        if(row.denseSlot != index || row.authoredIndex >= document.objects.size() ||
            mapping.bindingRevisions[index] != row.anticipatedBindingRevision)
        {
            return false;
        }
        const auto& authored = document.objects[row.authoredIndex];
        const auto& object = objects[index];
        const auto& prepared = m_membership->materialBindings[index];
        const auto currentTransform = object.GetPresentationTransform().currentModel;
        if(authored.id != row.objectID || authored.meshReference != row.meshReference || object.mesh != row.mesh ||
            !MatchesCommittedSceneObjectBindings(authored, object, mapping.committedObjects[index]) ||
            object.mesh != prepared.mesh || object.material != prepared.material ||
            object.slotMaterials != prepared.slotMaterials || object.materialsDeferred != prepared.materialsDeferred ||
            std::memcmp(currentTransform.m, row.presentationTransform.m, sizeof(currentTransform.m)) != 0)
        {
            return false;
        }
        if(!row.mesh)
        {
            continue;
        }
        if(!row.source || !row.mesh->IsCurrentStaticSource(row.source) || !row.association ||
            row.registryIndex >= m_blasRegistry.bottomLevels.size() || !m_blasRegistry.bottomLevels[row.registryIndex])
        {
            return false;
        }
        const auto& blas = *m_blasRegistry.bottomLevels[row.registryIndex];
        const auto& capture = blas.GetCurrentBuildCapture();
        const auto& recording = blas.GetCurrentBuildRecording();
        const auto& association = *row.association;
        bool registered = false;
        for(const auto& [mesh, registryIndex] : m_blasRegistry.meshToBottomLevel)
        {
            if(mesh == row.mesh)
            {
                registered = registryIndex == row.registryIndex;
                break;
            }
        }
        bool ledgerOwned = false;
        for(const auto& slot : m_associations)
        {
            ledgerOwned = ledgerOwned || slot.owned == row.association;
        }
        if(!registered || !ledgerOwned || association.source != row.source || association.sceneLifetime != m_recordingOwner ||
            capture.identity.ownerLifetime != association.capture.identity.ownerLifetime ||
            capture.identity.buildID != association.capture.identity.buildID || capture.result != association.capture.result ||
            capture.result.Get() != blas.GetResource() || recording.state != BLASRecordingState::Completed ||
            recording.provenance != association.provenance || !CaptureMatchesGeneration(capture, *row.source))
        {
            return false;
        }
    }
    return true;
}

AcceptedTLASGeometryHandle RayTracingSceneResources::QueryAcceptedGeometry(const FileSceneRuntime& runtime,
    const Scene* rendererScene, std::uint64_t rendererLifetime) const noexcept
{
    if(!m_acceptedGeometry || m_acceptedGeometry->runtime != &runtime ||
        m_acceptedGeometry->geometry->rendererLifetime != rendererLifetime || rendererScene != &runtime.GetScene())
    {
        return {};
    }
    if(!m_acceptedGeometry || !MatchesAcceptedGeometry(*m_acceptedGeometry, runtime, rendererScene, rendererLifetime))
    {
        // An observed mismatch ends owner currency. Restoring matching values
        // cannot resurrect a historical accepted record through a later query.
        m_acceptedGeometry.reset();
        return {};
    }
    return m_acceptedGeometry;
}

struct RayTracingSceneResources::GeometryReader::Record
{
    // Retains the join: exact capture (TLAS/BLAS/instance pins), sources and associations.
    AcceptedTLASGeometryHandle accepted;
    GeometryReaderState state = GeometryReaderState::Acquired;
    std::uint64_t generation = 0;
    std::uint64_t fence = 0;
    std::uint64_t lifetime = 0;
};

RayTracingSceneResources::GeometryReaderState RayTracingSceneResources::GeometryReader::GetState() const noexcept
{
    return m_record ? m_record->state : GeometryReaderState::Discarded;
}

const AcceptedTLASGeometryHandle& RayTracingSceneResources::GeometryReader::GetAcceptedGeometry() const noexcept
{
    static const AcceptedTLASGeometryHandle none;
    return m_record ? m_record->accepted : none;
}

std::size_t RayTracingSceneResources::GetLiveGeometryReaderCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(m_geometryReaders.begin(), m_geometryReaders.end(),
        [](const auto& reader) { return static_cast<bool>(reader); }));
}

RayTracingSceneResources::GeometryReader RayTracingSceneResources::AcquireGeometryReader(
    const AcceptedTLASGeometryHandle& accepted, const FileSceneRuntime& runtime, const Scene* rendererScene,
    std::uint64_t rendererLifetime) noexcept
{
    GeometryReader reader;

    // Only the owner's current record, revalidated now against authoritative state.
    // A historical record or retained handle never grants new traversal permission.
    if(!accepted || QueryAcceptedGeometry(runtime, rendererScene, rendererLifetime) != accepted)
    {
        return reader;
    }

    // An acquired reader whose handle was dropped never reached the GPU.
    for(auto& slot : m_geometryReaders)
    {
        if(slot && slot->state == GeometryReaderState::Acquired && slot.use_count() == 1)
        {
            slot.reset();
        }
    }

    const auto available = std::find_if(m_geometryReaders.begin(), m_geometryReaders.end(),
        [](const auto& slot) { return !slot; });

    if(available == m_geometryReaders.end())
    {
        return reader;
    }

    try
    {
        auto record = std::make_shared<GeometryReader::Record>();
        record->accepted = accepted;
        *available = record;
        reader.m_record = std::move(record);
    }
    catch(...)
    {
        reader = {};
    }

    return reader;
}

bool RayTracingSceneResources::RecordGeometryReader(const GeometryReader& reader, std::uint64_t generation,
    std::uint64_t rendererLifetime, D3D12_GPU_VIRTUAL_ADDRESS& topLevel) noexcept
{
    topLevel = 0;
    const auto& record = reader.m_record;

    if(!record || record->state != GeometryReaderState::Acquired ||
        std::find(m_geometryReaders.begin(), m_geometryReaders.end(), record) == m_geometryReaders.end())
    {
        return false;
    }

    // The active tracked list, before any in-place TLAS write in it: the latest
    // capture must still be the accepted one, and the owner must still hold it.
    const auto& join = *record->accepted->geometry;
    const auto capture = m_membership->topLevel.GetRecordingCapture();

    const auto currentFrame = record->accepted->currentFrameGeneration;
    const bool owned = currentFrame ? currentFrame == generation : m_acceptedGeometry == record->accepted;

    if(!generation || m_membership->recordingGeneration != generation || m_membership->submissionUncertain ||
        !owned || capture != join.tlasCapture || !capture->result ||
        !m_recordingOwner || join.recordingOwner != m_recordingOwner || join.rendererLifetime != rendererLifetime)
    {
        return false;
    }

    for(const auto& row : join.rows)
    {
        if(row.mesh && (!row.source || !row.mesh->IsCurrentStaticSource(row.source)))
        {
            return false; // Revoked after acquisition.
        }
    }

    record->state = GeometryReaderState::Recorded;
    record->generation = generation;
    topLevel = capture->result->GetGPUVirtualAddress();
    return true;
}

AcceptedTLASGeometryHandle RayTracingSceneResources::BuildCurrentFrameGeometry(const FileSceneRuntime& runtime,
    FileSceneAssetCatalog& catalog, const CommandContext& context, const Scene* rendererScene, std::uint64_t rendererLifetime,
    std::uint64_t generation) noexcept
{
    // Only inside the active recording, for the active membership, never while a publication is in flight.
    if(!generation || !m_membership || m_membership->recordingGeneration != generation || m_candidate ||
        !m_publicationHandoff.expired() || rendererScene != &runtime.GetScene())
    {
        return {};
    }

    const auto started = std::chrono::steady_clock::now();
    AcceptedTLASGeometryHandle result;
    ProspectiveTLASGeometryJoinHandle joined;

    if(BuildGeometryJoin(nullptr, runtime, catalog, context, rendererLifetime, joined, generation) == ProspectiveJoinStatus::Ready)
    {
        try
        {
            auto record = std::make_shared<AcceptedTLASGeometry>();
            record->geometry = std::move(joined);
            record->runtime = &runtime;
            record->residencyRevision = record->geometry->residencyRevision;
            record->bindingEpoch = record->geometry->bindingEpoch;
            record->currentFrameGeneration = generation;

            // The same exact validation as accepted geometry; the record is returned, never stored as current.
            if(MatchesAcceptedGeometry(*record, runtime, rendererScene, rendererLifetime))
            {
                result = std::move(record);
            }
        }
        catch(...)
        {
            result.reset();
        }
    }

    ++m_currentFrameJoinCount;
    m_currentFrameJoinNanoseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count());
    return result;
}

RayTracingSceneResources::GeometryReader RayTracingSceneResources::AcquireCurrentFrameGeometryReader(
    const AcceptedTLASGeometryHandle& record, const FileSceneRuntime& runtime, const Scene* rendererScene,
    std::uint64_t rendererLifetime, std::uint64_t generation) noexcept
{
    GeometryReader reader;

    // Only a current-frame record of exactly this generation, revalidated now.
    if(!record || !generation || record->currentFrameGeneration != generation ||
        !MatchesAcceptedGeometry(*record, runtime, rendererScene, rendererLifetime))
    {
        return reader;
    }

    // The same bounded slots and lifetime as every reader.
    for(auto& slot : m_geometryReaders)
    {
        if(slot && slot->state == GeometryReaderState::Acquired && slot.use_count() == 1)
        {
            slot.reset();
        }
    }

    const auto available = std::find_if(m_geometryReaders.begin(), m_geometryReaders.end(),
        [](const auto& slot) { return !slot; });

    if(available == m_geometryReaders.end())
    {
        return reader;
    }

    try
    {
        auto entry = std::make_shared<GeometryReader::Record>();
        entry->accepted = record;
        *available = entry;
        reader.m_record = std::move(entry);
    }
    catch(...)
    {
        reader = {};
    }

    return reader;
}

void RayTracingSceneResources::OrderRecordedGeometryReaders(ID3D12GraphicsCommandList4* commandList) noexcept
{
    const auto generation = m_membership->recordingGeneration;
    const bool readerInList = generation && std::any_of(m_geometryReaders.begin(), m_geometryReaders.end(),
        [generation](const auto& reader)
        {
            return reader && reader->state == GeometryReaderState::Recorded && reader->generation == generation;
        });

    // Ordinary rendering records no accepted reader, so it never pays this barrier.
    if(readerInList && commandList && m_membership->topLevel.m_resource)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = m_membership->topLevel.m_resource;
        commandList->ResourceBarrier(1, &barrier);
    }
}

void RayTracingSceneResources::TransitionGeometryReaders(std::uint64_t generation, GeometryReaderState from,
    GeometryReaderState to) noexcept
{
    for(auto& reader : m_geometryReaders)
    {
        if(!reader || !generation || reader->generation != generation || reader->state != from)
        {
            continue;
        }

        reader->state = to;

        // Only a proven abort releases an unsubmitted reader's dependencies here.
        if(to == GeometryReaderState::Discarded)
        {
            reader.reset();
        }
    }
}

std::array<std::optional<std::pair<RayTracingSceneResources::GeometryReaderState, std::uint64_t>>,
    RayTracingSceneResources::MaxGeometryReaders> RayTracingSceneResources::DescribeGeometryReaders() const noexcept
{
    std::array<std::optional<std::pair<GeometryReaderState, std::uint64_t>>, MaxGeometryReaders> slots{};

    for(std::size_t index = 0; index < MaxGeometryReaders; ++index)
    {
        if(m_geometryReaders[index])
        {
            slots[index] = std::make_pair(m_geometryReaders[index]->state, m_geometryReaders[index]->fence);
        }
    }

    return slots;
}

void RayTracingSceneResources::CollectGeometryReaders(std::uint64_t completed, std::uint64_t lifetime) noexcept
{
    for(auto& reader : m_geometryReaders)
    {
        if(!reader)
        {
            continue;
        }

        // Only an observed completed fence in the reader's own context lifetime retires it.
        // Uncertain and unresolved readers stay retained whatever their CPU handle does.
        if(reader->state == GeometryReaderState::Submitted && reader->lifetime == lifetime &&
            completed >= reader->fence)
        {
            reader->state = GeometryReaderState::Completed;
            reader.reset();
        }
        else if(reader->state == GeometryReaderState::Acquired && reader.use_count() == 1)
        {
            reader.reset();
        }
    }
}

BLASAssociationHandle RayTracingSceneResources::FindProspectiveAssociation(const Mesh& mesh,
    const TLASRecordingCapture::BLASReference& reference) const noexcept
{
    const auto source = mesh.GetStaticSourceGeneration();
    if(!source || !mesh.IsCurrentStaticSource(source))
    {
        return {};
    }
    for(const auto& slot : m_associations)
    {
        const auto& record = slot.owned;
        if(!record || record->source != source || record->sceneLifetime != m_recordingOwner ||
            record->capture.identity.ownerLifetime != reference.identity.ownerLifetime ||
            record->capture.identity.buildID != reference.identity.buildID || record->capture.result != reference.result ||
            !CaptureMatchesGeneration(record->capture, *source))
        {
            continue;
        }
        bool current = false;
        const auto* build = FindAssociatedBuild(*record, current);
        if(build && build->state == BLASRecordingState::Completed && build->provenance == record->provenance)
        {
            return record;
        }
    }
    return {};
}

BLASAssociationHandle RayTracingSceneResources::QueryCompletedBLASAssociation(const Mesh& mesh,
    const CausticContracts::Geometry::GeometrySourceHandle& source, std::uint64_t sceneLifetime) const noexcept
{
    // Current scene-resource lifetime and the exact generation the Mesh still owns.
    if (sceneLifetime == 0 || sceneLifetime != m_recordingOwner || !source || !mesh.IsCurrentStaticSource(source))
    {
        return {};
    }

    // No relevant recording or submission uncertainty may remain.
    if (!m_membership || m_membership->submissionUncertain)
    {
        return {};
    }

    // The registry slot for this exact Mesh object.
    const auto registered = m_blasRegistry.meshToBottomLevel.find(&mesh);

    if (registered == m_blasRegistry.meshToBottomLevel.end() || registered->second >= m_blasRegistry.bottomLevels.size() ||
        !m_blasRegistry.bottomLevels[registered->second])
    {
        return {};
    }

    const auto& bottomLevel = *m_blasRegistry.bottomLevels[registered->second];
    const BLASBuildCapture& currentCapture = bottomLevel.GetCurrentBuildCapture();
    const BLASBuildRecording& currentRecording = bottomLevel.GetCurrentBuildRecording();

    // Authoritative completion of the current build, judged on the BLAS's own
    // capture against the generation (not on any record contents).
    if (currentRecording.state != BLASRecordingState::Completed || currentCapture.result.Get() != bottomLevel.GetResource() ||
        !CaptureMatchesGeneration(currentCapture, *source))
    {
        return {};
    }

    for (const auto& slot : m_associations)
    {
        const auto& record = slot.owned;

        // Ledger-owned record of exactly the current build, generation and lifetime.
        if (!record || record->sceneLifetime != sceneLifetime || record->source != source ||
            record->provenance != currentRecording.provenance ||
            record->capture.identity.ownerLifetime != currentCapture.identity.ownerLifetime ||
            record->capture.identity.buildID != currentCapture.identity.buildID ||
            record->capture.result != currentCapture.result)
        {
            continue;
        }

        return record;
    }

    return {};
}

std::uint64_t RayTracingSceneResources::EnsureRecordingOwner()
{
    // Metadata-only scenes never ran Initialize; candidates still need an owner.
    if (m_recordingOwner == 0)
    {
        m_recordingOwner = AllocateSceneRetirementLifetime();
    }

    return m_recordingOwner;
}

void RayTracingSceneResources::AttributeRecordedTLAS(TopLevelAccelerationStructure& topLevel)
{
    m_acceptedGeometry.reset();
    const auto generation = m_membership ? m_membership->recordingGeneration : 0;
    topLevel.AttributeCurrentCapture(topLevel.GetRecordingCapture(), {m_recordingOwner, generation});
}

void RayTracingSceneResources::ConfirmRecordedBuilds(
    std::uint64_t generation,
    std::uint64_t fence,
    std::uint64_t lifetime)
{
    if (m_recordingOwner == 0 || generation == 0)
    {
        return;
    }

    m_membership->topLevel.ConfirmCaptures({m_recordingOwner, generation}, fence, lifetime);

    for (const auto& bottomLevel : m_blasRegistry.bottomLevels)
    {
        if (bottomLevel)
        {
            bottomLevel->ConfirmRecording({m_recordingOwner, generation}, fence, lifetime);
        }
    }
}

void RayTracingSceneResources::TransitionRecordedBuilds(std::uint64_t generation, BLASRecordingState next)
{
    if (m_recordingOwner == 0 || generation == 0)
    {
        return;
    }

    m_membership->topLevel.TransitionCaptures({m_recordingOwner, generation}, next);

    for (const auto& bottomLevel : m_blasRegistry.bottomLevels)
    {
        if (bottomLevel)
        {
            bottomLevel->TransitionRecording({m_recordingOwner, generation}, next);
        }
    }
}

bool RayTracingSceneResources::RefreshTLASReferences(
    ID3D12GraphicsCommandList4* commandList,
    unsigned int frameIndex)
{
    if (!commandList || m_membership->instances.empty() || m_blasRegistry.bottomLevelPointers.empty())
    {
        return false;
    }

    OrderRecordedGeometryReaders(commandList);
    const bool recorded = m_membership->topLevel.RefreshBLASReferences(commandList, frameIndex,
        m_membership->instances, m_blasRegistry.bottomLevelPointers);

    if(recorded)
    {
        AttributeRecordedTLAS(m_membership->topLevel);
    }

    return recorded;
}

void RayTracingSceneResources::ReleaseRetiredBLASResources()
{
    for (const auto& bottomLevel : m_blasRegistry.bottomLevels)
    {
        if (bottomLevel)
        {
            bottomLevel->ReleaseRetiredResources();
        }
    }
}

SceneBLASRetirementBatchIdentity RayTracingSceneResources::GetExclusiveRetirementBatch(const Mesh* mesh) const
{
    if (!mesh || m_blasRegistry.retirementLifetime == 0)
    {
        return {};
    }

    const auto registered = m_blasRegistry.meshToBottomLevel.find(mesh);

    if (registered == m_blasRegistry.meshToBottomLevel.end() || registered->second >= m_blasRegistry.bottomLevels.size())
    {
        return {};
    }

    const auto* expected = m_blasRegistry.bottomLevels[registered->second].get();

    if (!expected)
    {
        return {};
    }

    for (const auto& bottomLevel : m_blasRegistry.bottomLevels)
    {
        if (!bottomLevel || (bottomLevel.get() != expected && bottomLevel->GetRetirementBatchCount() != 0))
        {
            return {};
        }
    }

    const auto batch = expected->GetExclusiveRetirementBatch();

    if (!expected->ValidateRetirementBatch(batch))
    {
        return {};
    }

    return {m_blasRegistry.retirementLifetime, batch};
}

bool RayTracingSceneResources::ValidateRetirementBatch(

    const Mesh* mesh,
    SceneBLASRetirementBatchIdentity identity) const
{
    const auto current = GetExclusiveRetirementBatch(mesh);

    return identity.sceneLifetime != 0 && identity.sceneLifetime == current.sceneLifetime &&

           identity.blas.ownerLifetime != 0 && identity.blas.ownerLifetime == current.blas.ownerLifetime &&

           identity.blas.batchID != 0 && identity.blas.batchID == current.blas.batchID;
}

bool RayTracingSceneResources::ReleaseRetirementBatch(
    const Mesh* mesh,
    SceneBLASRetirementBatchIdentity identity,
    bool gpuCompletionConfirmed,
    bool referencesSafe)
{
    if (!gpuCompletionConfirmed || !referencesSafe || !ValidateRetirementBatch(mesh, identity))
    {
        return false;
    }

    const auto registered = m_blasRegistry.meshToBottomLevel.find(mesh);

    return m_blasRegistry.bottomLevels[registered->second]->ReleaseRetirementBatch(identity.blas, gpuCompletionConfirmed,
        referencesSafe);
}

bool RayTracingSceneResources::TryGetTLASGeometryEvidence(
    const Mesh* mesh,
    SceneBLASRetirementBatchIdentity batch,
    std::uint64_t geometryVersion,
    std::uint64_t recordingGeneration,
    unsigned int frameSlot,
    TLASGeometryRecordingEvidence& evidence) const
{
    if (!geometryVersion ||
        !recordingGeneration ||
        frameSlot >= 2 ||
        frameSlot != m_membership->topLevel.GetRecordingSlot())
    {
        return false;
    }

    if (!ValidateRetirementBatch(mesh, batch))
    {
        return false;
    }

    const auto registered = m_blasRegistry.meshToBottomLevel.find(mesh);
    TLASGeometryRecordingEvidence candidate{batch,
                                            geometryVersion,
                                            recordingGeneration,
                                            m_membership->topLevel.GetRecordingIdentity(),
                                            m_blasRegistry.bottomLevels[registered->second]->GetGPUAddress(),
                                            frameSlot};

    if (!ValidateTLASGeometryEvidence(mesh, candidate))
    {
        return false;
    }

    evidence = candidate;
    return true;
}

bool RayTracingSceneResources::ValidateTLASGeometryEvidence(
    const Mesh* mesh,
    const TLASGeometryRecordingEvidence& evidence) const
{
    if (!evidence.geometryVersion ||
        !evidence.recordingGeneration ||
        evidence.frameSlot >= 2 ||
        evidence.frameSlot != m_membership->topLevel.GetRecordingSlot())
    {
        return false;
    }

    if (!ValidateRetirementBatch(mesh, evidence.retirement))
    {
        return false;
    }

    const auto registered = m_blasRegistry.meshToBottomLevel.find(mesh);

    if (evidence.blasAddress == 0 || evidence.blasAddress != m_blasRegistry.bottomLevels[registered->second]->GetGPUAddress())
    {
        return false;
    }

    bool found = false;

    for (size_t instanceIndex = 0; instanceIndex < m_membership->instances.size(); ++instanceIndex)
    {
        if (m_membership->instances[instanceIndex].bottomLevelIndex == registered->second)
        {
            found = true;

            if (!m_membership->topLevel.RecordedInstanceUsesBLAS(evidence.tlasRecording, instanceIndex, evidence.blasAddress))
            {
                return false;
            }
        }
    }

    return found;
}

void RayTracingSceneResources::MembershipSnapshot::Shutdown()
{
    topLevel.Shutdown();

    instanceDataBuffer.Shutdown();
    triangleSlots.Shutdown();
    slotMaterials.Shutdown();

    slotGeometry.clear();
    materialBindings.clear();
    slotMaterialIndices.clear();
    instanceRecords.clear();

    instances.clear();
    renderObjectToInstance.clear();

    recordingGeneration = 0;
    lastUseFence = 0;
    fenceLifetime = 0;
    submissionUncertain = false;

    dependencyOwner.reset();
}

void RayTracingSceneResources::StableBLASRegistry::Shutdown()
{
    retirementLifetime = 0;

    for (auto& bottomLevel : bottomLevels)
    {
        bottomLevel->Shutdown();
    }

    bottomLevelPointers.clear();
    bottomLevels.clear();
    meshToBottomLevel.clear();
}

void RayTracingSceneResources::Shutdown()
{
    m_acceptedGeometry.reset();
    // Caller retains the existing GPU-drain/recording-abandonment obligation.
    // That proven drain also resolves every reader, including uncertain submissions.
    for(auto& reader : m_geometryReaders)
    {
        reader.reset();
    }
    // No TLAS or membership descriptor may outlive its referenced BLAS/meshes.
    m_blasRegistry.retirementLifetime = 0;
    ShutdownReplacementCandidate();
    for(auto& bindings : m_retiredMaterialBindings)
        bindings.reset();
    m_membership->Shutdown();

    for (auto& snapshot : m_retiredMemberships)
    {
        if (snapshot)
        {
            snapshot->Shutdown();
            snapshot.reset();
        }
    }

    m_blasRegistry.Shutdown();
    m_recordingOwner = 0;

    // Checked teardown (caller drained and abandoned recordings) ends ledger
    // ownership and reader tracking. External holders keep their own pins and
    // still count against capacity through tracked.
    for (auto& slot : m_associations)
    {
        slot.owned.reset();
        slot.readers = {};
    }
}

const TopLevelAccelerationStructure& RayTracingSceneResources::GetTopLevel() const
{
    return m_membership->topLevel;
}

unsigned int RayTracingSceneResources::GetInstanceDataDescriptorIndex() const
{
    return m_membership->instanceDataBuffer.GetDescriptorIndex();
}

std::size_t RayTracingSceneResources::GetInstanceCount() const
{

    return m_membership->instances.size();
}

bool RayTracingSceneResources::InitializeMaterialSlots(ID3D12Device5* device, ResourceUploader& uploader,
    DescriptorHeap& heap, const Scene& scene, std::vector<RayTracingInstanceData>& instances)
{
    std::vector<unsigned int> triangles{0};
    std::vector<unsigned int> materials;

    if (!BuildMaterialSlotData(*m_membership, scene.GetRenderObjects(), instances, triangles, materials))
    {
        return false;
    }

    if(materials.empty() ||
        !m_membership->triangleSlots.Initialize(device, uploader, triangles.data(), sizeof(unsigned int),
            static_cast<unsigned int>(triangles.size()), heap) ||
        !m_membership->slotMaterials.Initialize(device, uploader, materials.data(), sizeof(unsigned int),
            static_cast<unsigned int>(materials.size()), heap))
    {
        Log::Error("Failed to allocate DXR material-slot lookup buffers.");
        return false;
    }

    PatchTableDescriptors(*m_membership, instances);

    return true;
}

bool RayTracingSceneResources::ValidateMaterialBindings(const Scene& scene) const
{
    const auto& objects = scene.GetRenderObjects();

    if(!scene.HasRenderableMeshes() && m_membership->instances.empty())
    {
        return true;
    }

    if(objects.size() != m_membership->materialBindings.size())
    {
        return false;
    }

    for(size_t i = 0; i < objects.size(); ++i)
    {
        const auto& object = objects[i];
        const auto& previous = m_membership->materialBindings[i];

        if (object.mesh != previous.mesh)
        {
            return false;
        }

        // Pending authored bindings have no shader-visible instance or lookup
        // entries. Candidate validation separately checks their current epochs,
        // full binding maps and eventual material indices before publication.
        if (!object.mesh)
        {
            continue;
        }

        if (object.material != previous.material || object.slotMaterials != previous.slotMaterials)
        {
            return false;
        }

        const auto& current = object.mesh->GetMetadata();
        const auto& original = m_membership->slotGeometry.at(object.mesh);

        if(current.materialSlots != original.materialSlots ||
            (current.materialSlots.size() > 1 && current.submeshes != original.submeshes))
        {
            return false;
        }

        if(current.materialSlots.size() == 1)
        {
            const auto count = object.mesh->IsIndexed() ? object.mesh->GetIndexCount() : object.mesh->GetVertexCount();

            if(current.submeshes.size() != 1 || current.submeshes.front().firstIndex != 0 ||
                current.submeshes.front().materialSlot != 0 || current.submeshes.front().indexCount != count ||
                count == 0 || count % 3 != 0)
            {
                return false;
            }
        }

        for(size_t slot = 0; slot < current.materialSlots.size(); ++slot)
        {
            const auto* material = object.GetSlotMaterial(current.materialSlots[slot].id);

            if(!material || material->gpuIndex != m_membership->slotMaterialIndices[i][slot])
            {
                return false;
            }
        }
    }

    return true;
}

bool RayTracingSceneResources::BeginMembershipRecording(std::uint64_t generation)
{
    if (!generation || !m_membership || m_membership->recordingGeneration || m_membership->submissionUncertain)
    {
        return false;
    }

    m_membership->recordingGeneration = generation;
    return true;
}

bool RayTracingSceneResources::AttemptMembershipSubmission(std::uint64_t generation)
{
    if (!m_membership || !generation || m_membership->recordingGeneration != generation ||
        m_membership->submissionUncertain)
    {
        return false;
    }

    // Arm BEFORE Execute: signal failure cannot expose the previous fence as
    // proof that this recording is complete (or that it was never submitted).
    if (!AttemptCandidateSubmission(generation))
    {
        return false;
    }

    m_membership->submissionUncertain = true;
    TransitionRecordedBuilds(generation, BLASRecordingState::SubmissionUncertain);
    TransitionGeometryReaders(generation, GeometryReaderState::Recorded, GeometryReaderState::SubmissionUncertain);
    // Resubmission of a proven-unsubmitted recording re-arms its readers too.
    TransitionGeometryReaders(generation, GeometryReaderState::NotSubmitted, GeometryReaderState::SubmissionUncertain);
    return true;
}

bool RayTracingSceneResources::ConfirmMembershipSubmission(std::uint64_t generation, const CommandContext& context)
{
    const auto fence = context.GetLastSubmissionFenceValue();
    const auto lifetime = context.GetSubmissionLifetime();

    if (!m_membership || !generation || m_membership->recordingGeneration != generation ||
        !m_membership->submissionUncertain || !fence || !lifetime ||
        (m_membership->fenceLifetime && m_membership->fenceLifetime != lifetime) ||
        fence <= m_membership->lastUseFence)
    {
        return false;
    }

    if (!ConfirmCandidateSubmission(generation, context))
    {
        return false;
    }

    m_membership->lastUseFence = fence;
    m_membership->fenceLifetime = lifetime;

    m_membership->recordingGeneration = 0;
    m_membership->submissionUncertain = false;
    // The same fence/lifetime that pins this membership; completion is observed later.
    ConfirmRecordedBuilds(generation, fence, lifetime);

    for(auto& reader : m_geometryReaders)
    {
        if(!reader || reader->generation != generation || reader->state != GeometryReaderState::SubmissionUncertain)
        {
            continue;
        }

        reader->state = GeometryReaderState::Submitted;
        reader->fence = fence;
        reader->lifetime = lifetime;

        // Ledger accounting only: the reader's own join pins already retain every association.
        for(const auto& row : reader->accepted->geometry->rows)
        {
            if(row.association)
            {
                RegisterAssociationReader(row.association, fence, lifetime);
            }
        }
    }
    return true;
}

void RayTracingSceneResources::MembershipNotSubmitted(std::uint64_t generation)
{
    CandidateNotSubmitted(generation);
    if (m_membership && generation && m_membership->recordingGeneration == generation)
    {
        // ExecuteReserved explicitly proved no ExecuteCommandLists call.
        // Keep the recording pinned until successful abort or resubmission.
        m_membership->submissionUncertain = false;
        TransitionRecordedBuilds(generation, BLASRecordingState::NotSubmitted);
        TransitionGeometryReaders(generation, GeometryReaderState::SubmissionUncertain, GeometryReaderState::NotSubmitted);
    }
}

void RayTracingSceneResources::DiscardMembershipRecording(std::uint64_t generation)
{
    DiscardCandidateRecording(generation);
    if (m_membership && generation && m_membership->recordingGeneration == generation &&
        !m_membership->submissionUncertain)
    {
        m_membership->recordingGeneration = 0;
        // Renderer calls this only after a proven abort. Builds recorded under it
        // (including a Rebuild's swapped-in current result) never executed.
        TransitionRecordedBuilds(generation, BLASRecordingState::Discarded);
        TransitionGeometryReaders(generation, GeometryReaderState::Recorded, GeometryReaderState::Discarded);
        TransitionGeometryReaders(generation, GeometryReaderState::NotSubmitted, GeometryReaderState::Discarded);
    }
}

bool RayTracingSceneResources::CanRetireActiveMembership() const
{
    if (!m_membership || m_membership->recordingGeneration || m_membership->submissionUncertain)
    {
        return false;
    }

    return std::any_of(m_retiredMemberships.begin(), m_retiredMemberships.end(),
        [](const auto& snapshot) { return !snapshot; });
}

bool RayTracingSceneResources::RetireMembership(std::unique_ptr<MembershipSnapshot>& snapshot)
{
    if (!snapshot || snapshot.get() == m_membership.get() || snapshot->recordingGeneration ||
        snapshot->submissionUncertain)
    {
        return false;
    }

    for (auto& retired : m_retiredMemberships)
    {
        if (!retired)
        {
            retired = std::move(snapshot);
            return true;
        }
    }

    return false; // Defer publication; the caller still owns the snapshot.
}

void RayTracingSceneResources::CollectRetiredMemberships(const CommandContext& context)
{
    CollectReplacementCandidate(context);
    const auto completed = context.GetCompletedFenceValue();
    const auto lifetime = context.GetSubmissionLifetime();

    if (!lifetime || !completed || completed == UINT64_MAX)
    {
        return; // Device loss and unavailable fences are never completion proof.
    }

    // Registry builds (including adopted candidate builds) and in-flight candidate builds.
    for (const auto& bottomLevel : m_blasRegistry.bottomLevels)
    {
        if (bottomLevel)
        {
            bottomLevel->ObserveCompletion(completed, lifetime);
        }
    }

    m_membership->topLevel.ObserveCaptureCompletion(completed, lifetime, m_recordingOwner);
    ObserveCandidateCompletion(completed, lifetime);
    CollectGeometryReaders(completed, lifetime);
    CollectAssociations(completed, lifetime);

    for(auto& bindings : m_retiredMaterialBindings)
    {
        if(bindings && bindings->lifetime == lifetime && completed >= bindings->fence)
            bindings.reset();
    }

    for (auto& snapshot : m_retiredMemberships)
    {
        if(snapshot)
        {
            snapshot->topLevel.ObserveCaptureCompletion(completed, lifetime, m_recordingOwner);
        }

        if (!snapshot || snapshot->recordingGeneration || snapshot->submissionUncertain ||
            !snapshot->lastUseFence || snapshot->fenceLifetime != lifetime || completed < snapshot->lastUseFence)
        {
            continue;
        }

        snapshot->Shutdown();
        snapshot.reset();
    }
}

bool RayTracingSceneResources::BuildMaterialSlotData(MembershipSnapshot& snapshot,
    const std::vector<RenderObject>& objects, std::vector<RayTracingInstanceData>& instances,
    std::vector<unsigned int>& triangles, std::vector<unsigned int>& materials)
{
    std::unordered_map<const Mesh*, unsigned int> triangleOffsets;
    snapshot.materialBindings = objects;
    snapshot.slotMaterialIndices.resize(objects.size());

    for(size_t i = 0; i < objects.size(); ++i)
    {
        const auto& object = objects[i];

        if(!object.mesh)
        {
            continue;
        }

        const auto& metadata = object.mesh->GetMetadata();
        const size_t indexCount = object.mesh->IsIndexed() ? object.mesh->GetIndexCount() : object.mesh->GetVertexCount();
        std::string error;

        if(!ValidateMeshMetadata(metadata, indexCount, error))
        {
            Log::Error("Invalid DXR mesh slot metadata: " + error);
            return false;
        }

        if(metadata.materialSlots.size() > UINT_MAX - materials.size())
        {
            Log::Error("DXR material-slot table exceeds 32-bit addressing.");
            return false;
        }

        auto& instance = instances[i];
        instance.slotMaterialOffset = static_cast<unsigned int>(materials.size());
        instance.triangleSlotBufferIndex = UINT_MAX;
        snapshot.slotGeometry.emplace(object.mesh, metadata);

        for(const auto& slot : metadata.materialSlots)
        {
            const auto* material = object.GetSlotMaterial(slot.id);

            if(!material || !material->pipeline || !material->texture)
            {
                Log::Error("DXR object has an unresolved material slot: " + slot.id);
                return false;
            }

            materials.push_back(material->gpuIndex);
            snapshot.slotMaterialIndices[i].push_back(material->gpuIndex);
        }

        instance.materialIndex = materials[instance.slotMaterialOffset];

        if(metadata.materialSlots.size() == 1)
        {
            continue; // Dynamic single-slot geometry needs no primitive lookup.
        }

        auto offset = triangleOffsets.find(object.mesh);

        if(offset == triangleOffsets.end())
        {
            const size_t triangleCount = indexCount / 3;

            if(triangleCount > UINT_MAX - triangles.size())
            {
                Log::Error("DXR triangle-slot table exceeds 32-bit addressing.");
                return false;
            }

            const auto firstTriangle = static_cast<unsigned int>(triangles.size());
            triangles.resize(triangles.size() + triangleCount);

            for(const auto& submesh : metadata.submeshes)
            {
                const auto begin = triangles.begin() + firstTriangle + submesh.firstIndex / 3;
                std::fill_n(begin, submesh.indexCount / 3, submesh.materialSlot);
            }

            offset = triangleOffsets.emplace(object.mesh, firstTriangle).first;
        }

        instance.triangleSlotOffset = offset->second;
        instance.triangleSlotBufferIndex = 0; // Patched after successful GPU allocation.
    }

    return !materials.empty();
}

void RayTracingSceneResources::PatchTableDescriptors(const MembershipSnapshot& snapshot,
    std::vector<RayTracingInstanceData>& instances)
{
    for(auto& instance : instances)
    {
        instance.slotMaterialBufferIndex = snapshot.slotMaterials.GetDescriptorIndex();

        if(instance.triangleSlotBufferIndex != UINT_MAX)
        {
            instance.triangleSlotBufferIndex = snapshot.triangleSlots.GetDescriptorIndex();
        }
    }

}

bool RayTracingSceneResources::BuildInstanceData(MembershipSnapshot& snapshot,
    const std::vector<RenderObject>& objects, const std::unordered_map<const Mesh*, std::size_t>& meshIndices,
    std::vector<RayTracingInstanceData>& instances)
{
    snapshot.instances.reserve(objects.size());
    snapshot.renderObjectToInstance.assign(objects.size(), InvalidInstanceIndex);

    for (unsigned int i = 0; i < objects.size(); ++i)
    {
        const RenderObject& renderObject = objects[i];

        if (!renderObject.mesh)
        {
            continue;
        }

        const auto bottomLevelIt = meshIndices.find(renderObject.mesh);

        if (bottomLevelIt == meshIndices.end())
        {
            return false;
        }

        if (!renderObject.material)
        {
            return false;
        }

        unsigned int normalOffset = UINT_MAX;

        unsigned int uvOffset = UINT_MAX;
        unsigned int tangentOffset = UINT_MAX;

        for (const VertexAttribute& attribute : renderObject.mesh->GetVertexLayout().GetAttributes())
        {
            if (std::strcmp(attribute.semantic, "NORMAL") == 0 && attribute.semanticIndex == 0 &&

                attribute.type == VertexAttributeType::Float3)
            {
                normalOffset = attribute.offset;
            }

            if (std::strcmp(attribute.semantic, "TEXCOORD") == 0 && attribute.semanticIndex == 0 &&

                attribute.type == VertexAttributeType::Float2)
            {
                uvOffset = attribute.offset;
            }

            if (std::strcmp(attribute.semantic, "TANGENT") == 0 && attribute.semanticIndex == 0 &&

                attribute.type == VertexAttributeType::Float4)
            {
                tangentOffset = attribute.offset;
            }
        }

        if (normalOffset == UINT_MAX || uvOffset == UINT_MAX)
        {
            return false;
        }

        RayTracingInstanceData gpuInstance = instances[i];

        gpuInstance.vertexBufferIndex = renderObject.mesh->GetVertexBufferDescriptorIndex();
        gpuInstance.indexBufferIndex =

            renderObject.mesh->IsIndexed() ? renderObject.mesh->GetIndexBufferDescriptorIndex() : UINT_MAX;
        gpuInstance.vertexStride = renderObject.mesh->GetVertexStride();
        gpuInstance.normalOffset = normalOffset;
        gpuInstance.uvOffset = uvOffset;

        gpuInstance.tangentOffset = tangentOffset;
        gpuInstance.indexed = renderObject.mesh->IsIndexed() ? 1u : 0u;

        instances[i] = gpuInstance;

        RayTracingInstanceDescription instanceDescription{};
        instanceDescription.transform = renderObject.GetPresentationTransform().currentModel;
        instanceDescription.instanceID = i;
        instanceDescription.mask = 0xFF;

        instanceDescription.bottomLevelIndex = bottomLevelIt->second;

        const std::size_t instanceIndex = snapshot.instances.size();

        snapshot.renderObjectToInstance[i] = instanceIndex;

        snapshot.instances.push_back(instanceDescription);
    }

    return true;
}

std::uint64_t RayTracingSceneResources::ReserveBLASRegistryLifetime() const
{
    // Metadata-only scenes have no BLAS registry lifetime yet. Reserve identity
    // before future acceptance; do not publish it into the active registry here.
    if (m_blasRegistry.retirementLifetime)
    {
        return m_blasRegistry.retirementLifetime;
    }

    return AllocateSceneRetirementLifetime();
}
