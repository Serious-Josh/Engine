#pragma once

#include "Renderer/Buffer.h"
#include "Renderer/Rtxdi/RtxdiLightBridge.h"

#include "Rtxdi/DI/ReSTIRDI.h"

#include <d3d12.h>

#include <climits>
#include <cstdint>
#include <optional>
#include <vector>

class DescriptorHeap;

// RTXPT.3 R3.5: what previous reservoirs were computed against. Reuse needs identical visibility geometry (the
// previous-surface visibility rays of ray-traced bias correction are traced through the current TLAS) and
// identical materials (previous-surface target PDFs). These are the renderer's existing accumulation-reset
// identities: scene-resource lifetime, TLAS address and TLAS recording, material revision.
struct RtxdiHistorySignature
{
    std::uint64_t sceneResourceLifetime = 0;
    D3D12_GPU_VIRTUAL_ADDRESS topLevelAddress = 0;
    std::uint64_t topLevelRecording = 0;
    std::uint64_t materialRevision = 0;

    bool operator==(const RtxdiHistorySignature&) const = default;
};

// What one ReSTIR DI recording may use. Values only; valid until the next BeginFrame, CommitFrame,
// AbandonFrame or InvalidateHistory.
struct RtxdiFrame
{
    RTXDI_ReservoirBufferParameters reservoirBufferParameters{};
    RTXDI_DIBufferIndices bufferIndices{};
    RTXDI_RuntimeParameters runtimeParameters{};
    RTXDI_LightBufferParameters lightBufferParameters{};
    RTXDI_DISpatioTemporalResamplingParameters spatioTemporalParameters{};

    // False: no shader may read temporalResamplingInputBufferIndex this frame, and every translation entry
    // is -1.
    bool historyValid = false;

    // Uploaded per frame through the renderer's frame upload buffer, like the scene light table.
    std::vector<RtxdiLightRecord> lightRecords;
    // currentToPrevious (one entry per current light) followed by previousToCurrent (one per previous light).
    std::vector<std::int32_t> lightTranslation;
    std::uint32_t previousLightCount = 0;
};

// RTXPT.3: renderer-owned RTXDI state of the raster-primary ReSTIR DI variant (uniform local-light selection,
// fused spatiotemporal reuse, no RIS or environment resources). Owns one RWStructuredBuffer of
// RTXDI_PackedDIReservoir holding the SDK's three reservoir arrays in its block-linear layout, the SDK's
// neighbor-offset table (immutable), the SDK's ReSTIRDIContext that rotates the arrays, and the light set and
// history signature the previous reservoirs refer to.
//
// Lifetime follows PreviousGBufferHistory and the WaitForGPU model: Initialize, Resize and Shutdown release
// immediately, so the caller guarantees the queue is idle. History follows R4: BeginFrame prepares a pending
// frame from the committed state; only CommitFrame, called after the command list was submitted, makes it the
// previous frame. A recording that fails or is never submitted is abandoned and changes nothing.
class RtxdiResources
{
    public:
    static constexpr std::uint32_t ReservoirArrayCount = rtxdi::c_NumReSTIRDIReservoirBuffers;
    static constexpr std::uint32_t ReservoirStride = sizeof(RTXDI_PackedDIReservoir);
    static constexpr std::uint32_t NeighborOffsetCount = 8192;

    static_assert(ReservoirStride == 24, "RTXDI_PackedDIReservoir layout changed; check RtxdiResources sizing.");

    // RTXPT.3 R3.5/R3.6 fixed fused spatiotemporal configuration (approved Gates 2 and 4): the SDK defaults with
    // ray-traced bias correction and naive-sample discounting off (it is inconsistent with that correction's
    // normalization in the pinned SDK). The boiling filter is never applied.
    static RTXDI_DISpatioTemporalResamplingParameters SpatioTemporalParameters();

    RtxdiResources() = default;

    RtxdiResources(const RtxdiResources&) = delete;
    RtxdiResources& operator=(const RtxdiResources&) = delete;

    // The reservoir buffer is created in COMMON; consumers transition it with GetReservoirBuffer().Transition.
    // A failure leaves the resources unallocated.
    bool Initialize(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        unsigned int width,
        unsigned int height);

    // Recreates everything at the new extent and drops history; does nothing while unallocated. Caller
    // guarantees the queue is idle. A failure leaves the resources unallocated.
    bool Resize(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        unsigned int width,
        unsigned int height);

    void Shutdown();

    // Prepares this recording from the committed state and the frame's light set. History is usable only if
    // a committed frame exists, its signature equals `signature`, and the previous surfaces it was computed on
    // are available (`previousSurfacesAvailable`). Replaces any earlier pending frame. Returns false, leaving
    // `frame` untouched, while unallocated.
    bool BeginFrame(const RtxdiLightSet& lights, const RtxdiHistorySignature& signature,
        bool previousSurfacesAvailable, RtxdiFrame& frame);

    // After the recording's command list was submitted: its reservoirs and lights become the previous frame.
    // Does nothing without a pending frame.
    void CommitFrame();

    // The recording was not submitted; the committed state is unchanged.
    void AbandonFrame();

    // Drops the pending frame and makes the committed reservoirs unreadable until a frame is committed.
    void InvalidateHistory();

    bool IsInitialized() const noexcept
    {
        return m_initialized;
    }

    bool HasHistory() const noexcept
    {
        return m_historyValid;
    }

    // Evidence only: whether the most recent BeginFrame allowed reuse of history.
    bool LastFrameUsedHistory() const noexcept
    {
        return m_lastFrameUsedHistory;
    }

    Buffer& GetReservoirBuffer() noexcept
    {
        return m_reservoirs;
    }

    // RWStructuredBuffer<RTXDI_PackedDIReservoir> UAV, ReservoirArrayCount * arrayPitch elements.
    unsigned int GetReservoirUAVIndex() const noexcept
    {
        return m_reservoirUAVIndex;
    }

    // Buffer<float2> SRV (R8G8_SNORM) of the SDK's neighbor offsets; upload heap, GENERIC_READ for its lifetime.
    unsigned int GetNeighborOffsetsSRVIndex() const noexcept
    {
        return m_neighborOffsetsSRVIndex;
    }

    // Bytes of GPU memory owned; 0 while unallocated.
    std::uint64_t GetResourceBytes() const noexcept;

    // Reservoirs in one array at the given extent, including the block-linear padding.
    static std::uint64_t ReservoirArrayElements(unsigned int width, unsigned int height);

    private:
    // Local diagnostics only (RTXPT.3 direct-light investigation): reads and overrides m_context's reuse parameters.
    friend class SceneHierarchyVerification;

    bool CreateReservoirUAV(ID3D12Device* device, DescriptorHeap& descriptorHeap, std::uint64_t elements);
    bool CreateNeighborOffsets(ID3D12Device* device, DescriptorHeap& descriptorHeap);

    struct PendingFrame
    {
        rtxdi::ReSTIRDIContext context;
        RtxdiLightSet lights;
        RtxdiHistorySignature signature;
    };

    Buffer m_reservoirs;
    DescriptorHeap* m_descriptorHeap = nullptr;
    unsigned int m_reservoirUAVIndex = UINT_MAX;
    ID3D12Resource* m_neighborOffsets = nullptr;
    unsigned int m_neighborOffsetsSRVIndex = UINT_MAX;

    std::optional<rtxdi::ReSTIRDIContext> m_context;
    // The lights and signature m_context's last output reservoirs refer to; meaningful only while m_historyValid.
    RtxdiLightSet m_committedLights;
    RtxdiHistorySignature m_committedSignature;
    std::optional<PendingFrame> m_pending;
    std::uint32_t m_frameIndex = 0;
    bool m_historyValid = false;
    bool m_lastFrameUsedHistory = false;

    unsigned int m_width = 0;
    unsigned int m_height = 0;
    bool m_initialized = false;
};
