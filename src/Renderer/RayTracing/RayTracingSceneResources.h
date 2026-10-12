#pragma once

#include "Renderer/RayTracing/BottomLevelAccelerationStructure.h"
#include "Renderer/RayTracing/TopLevelAccelerationStructure.h"
#include "Renderer/StructuredBuffer.h"

#include <d3d12.h>
#include <memory>
#include <optional>
#include <vector>
#include <unordered_map>

class Mesh;
class Scene;
class ResourceUploader;
class DescriptorHeap;

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
    public:
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
        return m_topLevel.GetRecordingIdentity();
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

    private:
    bool RebuildMeshBLASInternal(
      ID3D12Device5* device,
      ID3D12GraphicsCommandList4* commandList,
      const Mesh* mesh,
      const std::optional<RayTracingGeometryCounts>& countOverride);

    static constexpr std::size_t InvalidInstanceIndex = static_cast<std::size_t>(-1);

    std::vector<std::unique_ptr<BottomLevelAccelerationStructure>> m_bottomLevels;

    std::unordered_map<const Mesh*, std::size_t> m_meshToBottomLevel;

    std::vector<RayTracingInstanceDescription> m_instances;

    std::vector<std::size_t> m_renderObjectToInstance;

    std::vector<const BottomLevelAccelerationStructure*> m_bottomLevelPointers;

    TopLevelAccelerationStructure m_topLevel;

    std::uint64_t m_retirementLifetime = 0;

    StructuredBuffer m_instanceDataBuffer;
};
