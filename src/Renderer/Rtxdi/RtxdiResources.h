#pragma once

#include "Renderer/Buffer.h"
#include "Renderer/Rtxdi/RtxdiLightBridge.h"
#include "Renderer/Rtxdi/RtxdiShaderData.h"

#include "Rtxdi/DI/ReSTIRDI.h"

#include <d3d12.h>

#include <climits>
#include <cstdint>
#include <optional>
#include <deque>
#include <string>
#include <vector>

class DescriptorHeap;
class UploadBuffer;

// RTXPT.3 R3.5 / PT.2A: what previous reservoirs were computed against. Reuse always needs the same scene resources,
// the same TLAS (address) referencing the same geometry (TopLevelAccelerationStructure::GetGeometryIdentity: no rebuilt
// BLAS, membership change or re-resolved instance) and the same materials (previous-surface target PDFs).
// topLevelRecording changes with every TLAS recording, including transform-only updates. Before PT.2A it was part of the
// required identity, so any moving object discarded all history; it is now compared only under the conservative policy.
// When it differs, bias-correction visibility of previous surfaces is traced through the moved TLAS (RtxdiReuseBridge.hlsli).
struct RtxdiHistorySignature
{
    std::uint64_t sceneResourceLifetime = 0;
    D3D12_GPU_VIRTUAL_ADDRESS topLevelAddress = 0;
    std::uint64_t topLevelGeometry = 0;
    std::uint64_t topLevelRecording = 0;
    std::uint64_t materialRevision = 0;

    bool operator==(const RtxdiHistorySignature&) const = default;
};

// PT.2A: why the most recent BeginFrame did or did not reuse history (the first failing condition, in this order).
enum class RtxdiHistoryDecision : std::uint8_t
{
    Reused,
    NoCommittedFrame,
    PreviousSurfacesUnavailable,
    SceneResources,
    TopLevel,
    Geometry,
    Material,
    Transforms
};

// Which changes keep history. preserveThroughTransforms (PT.2A): moved instances keep history; otherwise any TLAS recording
// change drops it. preserveThroughLightChanges (PT.2B): a light that moved or changed keeps its samples (translated by stable
// key, evaluated with its previous record by bias correction); otherwise only lights whose whole identity is unchanged
// translate. Both off is the pre-PT.2A conservative behaviour.
struct RtxdiHistoryPolicy
{
    bool preserveThroughTransforms = true;
    bool preserveThroughLightChanges = true;
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

    // PT.2B: the light data RecordLightData copies to the GPU. The previous records are those the previous reservoirs were
    // computed with (empty without history).
    std::vector<RtxdiLightRecord> lightRecords;
    std::vector<RtxdiLightRecord> previousLightRecords;
    // currentToPrevious (one entry per current light) followed by previousToCurrent (one per previous light).
    std::vector<std::int32_t> lightTranslation;
    std::uint32_t previousLightCount = 0;
};

// RTXPT.3: renderer-owned RTXDI state of the raster-primary ReSTIR DI variant (uniform local-light selection,
// fused spatiotemporal reuse, no RIS or presampling resources). Owns one RWStructuredBuffer of
// RTXDI_PackedDIReservoir holding the SDK's three reservoir arrays in its block-linear layout, the SDK's
// neighbor-offset table (immutable), the light data buffer (PT.2B, RtxdiLightDataLayout, rewritten every frame), the
// SDK's ReSTIRDIContext that rotates the arrays, and the light set and history signature the previous reservoirs refer
// to.
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
    // normalization in the pinned SDK), and (RTXPT D.2C.2R) maxHistoryLength 4. The boiling filter is never applied.
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
    // a committed frame exists, the previous surfaces it was computed on are available (`previousSurfacesAvailable`)
    // and its signature matches `signature`: every field except topLevelRecording, which must also match only without
    // policy.preserveThroughTransforms. Lights translate under policy.preserveThroughLightChanges
    // (BuildRtxdiLightTranslation). Replaces any earlier pending frame. Returns false, leaving `frame` untouched, while
    // unallocated.
    bool BeginFrame(const RtxdiLightSet& lights, const RtxdiHistorySignature& signature,
        bool previousSurfacesAvailable, const RtxdiHistoryPolicy& policy, RtxdiFrame& frame);

    // PT.2B: copies the frame's light data (RtxdiLightDataLayout) from a frame upload allocation into the light data buffer
    // and leaves it readable by non-pixel shaders. The copy runs on the GPU timeline after every earlier reader of the
    // buffer in this queue, so one buffer serves all frames in flight. Many-light M2: when the frame needs more room the
    // buffer is replaced by one twice as large (at least the need), the old one retired with lastSubmittedFence and
    // released once completedFence reaches it. Returns false while unallocated, or when allocation or the upload fails.
    bool RecordLightData(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, UploadBuffer& upload,
        const RtxdiFrame& frame, std::uint64_t lastSubmittedFence, std::uint64_t completedFence);

    // Many-light M3: grows the light data, as RecordLightData would, so it can hold a frame of `currentCount` lights with
    // `previousCount` previous lights. The renderer calls it before any consumer reads the frame's lights, so an allocation
    // failure is found while the frame can still be lit consistently without table lights. False (with `error`) while
    // unallocated, above the 32-bit addressable size, or when the allocation fails; the current buffer is then unchanged.
    bool ReserveLightData(ID3D12Device* device, std::size_t currentCount, std::size_t previousCount,
        std::uint64_t lastSubmittedFence, std::uint64_t completedFence, std::string& error);

    // The light count the next frame's translation would read as previous lights (0 without usable committed history).
    std::size_t GetCommittedLightCount() const noexcept
    {
        return m_historyValid ? m_committedLights.records.size() : 0;
    }

    std::uint64_t GetLightDataCapacityBytes() const noexcept
    {
        return m_lightDataCapacityBytes;
    }

    // Test-only: light data growth beyond this many bytes fails as if the allocation had failed; nullopt removes the limit.
    void SetLightDataCapacityLimitForTesting(std::optional<std::uint64_t> bytes) noexcept
    {
        m_lightDataCapacityLimitForTesting = bytes;
    }

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

    // Evidence only: whether the most recent BeginFrame allowed reuse of history, and why.
    bool LastFrameUsedHistory() const noexcept
    {
        return m_lastFrameUsedHistory;
    }

    RtxdiHistoryDecision LastHistoryDecision() const noexcept
    {
        return m_lastHistoryDecision;
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

    // ByteAddressBuffer SRV of the light data (grown by RecordLightData); valid while allocated. Read it after the frame's
    // RecordLightData, since growth replaces the buffer and its descriptor.
    unsigned int GetLightDataSRVIndex() const noexcept
    {
        return m_lightData.GetDescriptorIndex();
    }

    // Left in NON_PIXEL_SHADER_RESOURCE by RecordLightData.
    Buffer& GetLightDataBuffer() noexcept
    {
        return m_lightData;
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
    void ReleaseCompletedLightData(std::uint64_t completedFence);
    bool GrowLightData(ID3D12Device* device, std::uint64_t neededBytes, std::uint64_t lastSubmittedFence, std::string& error);

    struct PendingFrame
    {
        rtxdi::ReSTIRDIContext context;
        RtxdiLightSet lights;
        RtxdiHistorySignature signature;
    };

    Buffer m_reservoirs;
    Buffer m_lightData;
    std::uint64_t m_lightDataCapacityBytes = 0;
    std::optional<std::uint64_t> m_lightDataCapacityLimitForTesting;

    struct RetiredLightData
    {
        Buffer buffer;
        std::uint64_t submissionFence = 0;
    };

    std::deque<RetiredLightData> m_retiredLightData;
    std::vector<std::uint8_t> m_lightDataStaging;
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
    RtxdiHistoryDecision m_lastHistoryDecision = RtxdiHistoryDecision::NoCommittedFrame;

    unsigned int m_width = 0;
    unsigned int m_height = 0;
    bool m_initialized = false;
};
