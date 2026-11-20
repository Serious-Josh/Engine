#pragma once

#include "Renderer/Buffer.h"
#include "Renderer/ShaderData.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

class DescriptorHeap;
class UploadBuffer;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;

// Many-light M2: the GPU copy of the frame's scene light table (SceneLightTable), read by every scene-light consumer
// through SceneLightConstants.lightBufferIndex (a structured SRV of GPULight records, stride 64, read as
// StructuredBuffer<GPULight> by SceneLightLoad in SceneLights.hlsli; the two must change together).
// - Capacity (allocated records) is separate from the light count. It grows geometrically (doubling, at least the count)
//   and never shrinks while the renderer lives; there is no light-count limit beyond memory and 32-bit indexing.
// - Every frame's table is copied from the frame upload ring in the frame command list, before any consumer reads it.
// - A replaced buffer is retired with the last submitted fence and released once that fence completes; its descriptor
//   stays valid for frames already submitted.
// - A zero-light scene keeps a one-record allocation, so the SRV is always valid; shaders read nothing at count 0.
// - Many-light M3: when the table cannot be published (growth or its upload allocation fails), the frame publishes an empty
//   table through the current, still valid buffer instead, and the next frame retries. The buffer's old contents are then
//   never read (count 0), so no frame mixes an earlier table with the current scene.
class SceneLightBuffer
{
    public:
    enum class RecordResult
    {
        // The frame's table was copied and published.
        Published,
        // The table could not be published, so an empty table was published through the current buffer (constants valid,
        // count 0). The frame must render without table lights.
        PublishedEmpty,
        // Nothing can be bound: there is no buffer and none could be created. Constants are not valid.
        Failed
    };

    SceneLightBuffer() = default;
    SceneLightBuffer(const SceneLightBuffer&) = delete;
    SceneLightBuffer& operator=(const SceneLightBuffer&) = delete;

    // Allocates the one-record buffer ahead of the first frame, so an empty binding exists even if a later growth fails.
    // Optional (Record allocates on first use); false when the allocation fails.
    bool Initialize(ID3D12Device* device, DescriptorHeap& descriptorHeap);

    // Grows the buffer if the table needs it, then records the copy of the table and the transition to shader read.
    // lastSubmittedFence: the newest fence a frame that may read the current buffer was submitted with; completedFence:
    // the GPU's completed fence. On PublishedEmpty and Failed, `failureReason` says why; nothing is logged here (the caller
    // logs once per failure episode).
    // M3.5: `contentRevision` identifies the table's content (0 = unknown). When it equals the revision the buffer last
    // published and the buffer was not replaced since, the buffer already holds exactly this table, so no copy is recorded.
    RecordResult Record(ID3D12Device* device, DescriptorHeap& descriptorHeap, ID3D12GraphicsCommandList* commandList,
        UploadBuffer& upload, const SceneLightTable& table, std::uint64_t lastSubmittedFence,
        std::uint64_t completedFence, SceneLightConstants& constants, std::string* failureReason = nullptr,
        std::uint64_t contentRevision = 0);

    // Releases every buffer. Only after the renderer's GPU drain.
    void Shutdown();

    std::uint32_t GetCapacity() const noexcept
    {
        return m_capacity;
    }

    // Test-only: growth beyond this many records fails as if the allocation had failed; nullopt removes the limit.
    void SetCapacityLimitForTesting(std::optional<std::uint32_t> records) noexcept
    {
        m_capacityLimitForTesting = records;
    }

    private:
    struct RetiredBuffer
    {
        Buffer buffer;
        std::uint64_t submissionFence = 0;
    };

    bool Grow(ID3D12Device* device, DescriptorHeap& descriptorHeap, std::uint32_t lightCount,
        std::uint64_t lastSubmittedFence, std::string& error);

    Buffer m_buffer;
    std::uint32_t m_capacity = 0;
    std::deque<RetiredBuffer> m_retired;
    std::optional<std::uint32_t> m_capacityLimitForTesting;
    // The contentRevision of the table m_buffer holds (0 = none known): cleared by growth, an empty publication and Shutdown.
    std::uint64_t m_publishedRevision = 0;
};
