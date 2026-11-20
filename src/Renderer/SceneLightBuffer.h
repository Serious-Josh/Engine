#pragma once

#include "Renderer/Buffer.h"
#include "Renderer/ShaderData.h"

#include <cstdint>
#include <deque>

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
class SceneLightBuffer
{
    public:
    SceneLightBuffer() = default;
    SceneLightBuffer(const SceneLightBuffer&) = delete;
    SceneLightBuffer& operator=(const SceneLightBuffer&) = delete;

    // Grows the buffer if the table needs it, then records the copy of the table and the transition to shader read.
    // lastSubmittedFence: the newest fence a frame that may read the current buffer was submitted with; completedFence:
    // the GPU's completed fence. False (logged) on allocation or upload failure; constants are then not valid.
    bool Record(ID3D12Device* device, DescriptorHeap& descriptorHeap, ID3D12GraphicsCommandList* commandList,
        UploadBuffer& upload, const SceneLightTable& table, std::uint64_t lastSubmittedFence,
        std::uint64_t completedFence, SceneLightConstants& constants);

    // Releases every buffer. Only after the renderer's GPU drain.
    void Shutdown();

    std::uint32_t GetCapacity() const noexcept
    {
        return m_capacity;
    }

    private:
    struct RetiredBuffer
    {
        Buffer buffer;
        std::uint64_t submissionFence = 0;
    };

    bool Grow(ID3D12Device* device, DescriptorHeap& descriptorHeap, std::uint32_t lightCount,
        std::uint64_t lastSubmittedFence);

    Buffer m_buffer;
    std::uint32_t m_capacity = 0;
    std::deque<RetiredBuffer> m_retired;
};
