#pragma once

#include "Math/Matrix4.h"
#include "Renderer/Buffer.h"
#include "Renderer/CommandContext.h"
#include "Renderer/GpuTimestampProfiler.h"
#include <wrl/client.h>
#include "Renderer/ComputePipeline.h"
#include "Renderer/Texture.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>

struct Vertex;

class DescriptorHeap;
class GraphicsDevice;
class RenderTarget;
class ShaderLibrary;
class UploadBuffer;

// Everything the dedicated caustic tracer reads, taken from one accepted caustic snapshot and its frame.
// The caller (Renderer) guarantees every address belongs to resources retained for this recording.
struct CausticFrameInput
{
    D3D12_GPU_VIRTUAL_ADDRESS topLevel = 0;
    D3D12_GPU_VIRTUAL_ADDRESS materialTable = 0;
    D3D12_GPU_VIRTUAL_ADDRESS lightTable = 0;
    D3D12_GPU_VIRTUAL_ADDRESS instanceData = 0;
    D3D12_GPU_VIRTUAL_ADDRESS slotMaterials = 0;
    D3D12_GPU_VIRTUAL_ADDRESS vertices = 0;
    D3D12_GPU_VIRTUAL_ADDRESS indices = 0;
    std::uint32_t lightOffset = 0;
    std::uint32_t reflectorDense = 0;
    std::uint32_t reflectorNative = 0;
    std::uint32_t receiverDense = 0;
    std::uint32_t receiverNative = 0;
    std::uint32_t materialEnergyTable = 0;
    std::uint32_t materialAverageTable = 0;
    std::uint32_t materialCosineMapping = 0;
    Matrix4 viewProjection{};
    Matrix4 inverseViewProjection{};
    std::array<float, 3> cameraPosition{};
    std::array<float, 3> cameraForward{};
    float jacobianScale = 0.0f;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t triangleCount = 0; // Certified primitives [0, triangleCount) of the shared source.
    DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN; // The Reference output the present copy mirrors.
    // The complete emission proposal (CausticProposalContract) for the reflector's certified bound.
    std::array<float, 3> capAxis{};
    float cosineHalfAngle = -1.0f;
    float inverseSolidAngle = 0.0f;
    float sphereProbability = 0.0f;
    // Accumulation identity: any change resets the caustic history (never Reference's).
    std::array<std::uint64_t, 6> identity{};
    std::array<float, 16> lightRecord{};
};

// R.9I.3 dedicated reflective caustics, additive development mode. Owns its tracer, adaptive RGB64
// accumulation and history; never writes Reference accumulation or any Reference history. All
// work is recorded into the Renderer's tracked frame list on its single direct queue.
class CausticPass
{
public:
    // Deliberately false until numerical, submission, resource and performance
    // acceptance gates have passed. Diagnostic execution never changes this gate.
    static constexpr bool ProductionContributionValidated = false;
    // Experimental activation only: the restricted-domain execution safety evidence (fail-closed
    // epochs, completed-submission fault readback, bounded resources, drained resize; see
    // R9I3FirstVisibleAdditiveCausticsValidation.md) passed. Not image-quality or performance
    // certification; contribution still requires the explicit, default-OFF experimental toggle.
    static constexpr bool ExperimentalExecutionSafetyValidated = true;
    static constexpr std::uint32_t AttemptsPerFrame = 1u << 16;          // <= 2^20 contract maximum.
    static constexpr std::uint32_t PrecisionBits = 63u - 16u;            // b = 63 - ceil(log2 N).
    static constexpr std::uint32_t MaximumDimension = 4096;
    static constexpr std::uint64_t MaximumResourceBytes = 512ull << 20;  // All caustic-owned buffers.

    enum class Status { Uninitialized, Unavailable, Refused, Recorded, Faulted };

    void SetTimingContext(GpuTimestampProfiler& profiler, unsigned int frameSlot)
    {
        m_profiler = &profiler;
        m_frameSlot = frameSlot;
    }

    bool Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary);
    void Shutdown();
    // Only at a drained boundary (resize, scene release, shutdown): no recorded frame may still use them.
    void ReleaseResources();
    void ResetHistory();
    // Only explicit acknowledgement after all tracked users complete may clear a
    // numerical fault. Ordinary resets/toggles and identity changes cannot recover it.
    bool AcknowledgeFaultAndReset(const CommandContext& context);
    void CollectCompleted(const CommandContext& context);
    void ArmSubmission(std::uint64_t generation);
    void ConfirmSubmission(std::uint64_t generation, std::uint64_t fence, std::uint64_t lifetime);
    void ProvenNotSubmitted(std::uint64_t generation);
    void DiscardRecording(std::uint64_t generation);
    std::uint32_t GetLastFault() const noexcept
    {
        return m_lastFault;
    }

    std::uint64_t GetFaultObservations() const noexcept
    {
        return m_faultObservations;
    }

    std::uint64_t GetCompletedMeasurements() const noexcept
    {
        return m_completedMeasurements;
    }

    bool IsFaulted() const noexcept
    {
        return m_faulted;
    }


    // Records tracing, accumulation and the history fold for this frame. Refusal records nothing.
    bool Record(ID3D12GraphicsCommandList* commandList, ID3D12Device* device, ResourceUploader& uploader,
        DescriptorHeap& heap, UploadBuffer& uploadBuffer, const CausticFrameInput& input, std::uint32_t seed,
        const CommandContext& context, std::uint64_t generation);

    // Replaces the ordinary output-to-scene-colour copy: copy, add the caustic mean (or show it alone),
    // copy to scene colour. The Reference output itself is never written.
    bool Present(ID3D12GraphicsCommandList* commandList, DescriptorHeap& heap, UploadBuffer& uploadBuffer,
        Texture& referenceOutput, RenderTarget& sceneColor, bool causticOnly);

    // A binary64 world-space sphere enclosing the certified primitives [0, primitiveCount) under the
    // reflector's instance transform, and the CausticProposalContract emission proposal toward it
    // (epsilon = 1/4). The defensive uniform-sphere term keeps the estimator unbiased for any cap.
    static bool BuildEmissionProposal(std::span<const Vertex> vertices, std::span<const std::uint32_t> indices,
        std::uint32_t primitiveCount, const Matrix4& model, const std::array<float, 3>& light, CausticFrameInput& input);

    Status GetStatus() const noexcept { return m_status; }
    const std::string& GetReason() const noexcept { return m_reason; }
    std::uint64_t GetResourceBytes() const noexcept { return m_resourceBytes; }
    std::uint64_t GetRecordedFrames() const noexcept { return m_recordedFrames; }
    // Frames recorded into the current history since its last reset (CPU recording count; a GPU
    // fault still invalidates the whole history, reported through IsFaulted).
    std::uint64_t GetHistoryFrames() const noexcept { return m_historyFrames; }

    // Diagnostics: the caustic-owned buffers (raw, UAV state between dispatches).
    Buffer& GetRecords() { return m_records; }
    Buffer& GetSums() { return m_sums; }
    Buffer& GetScales() { return m_scales; }
    Buffer& GetHistory() { return m_history; }
    Buffer& GetControl() { return m_control; }

private:
    friend class CausticNumericalVerification; // Local ignored numerical/lifetime diagnostic.
    bool EnsureResources(ID3D12Device* device, ResourceUploader& uploader, DescriptorHeap& heap,
        std::uint32_t width, std::uint32_t height, DXGI_FORMAT outputFormat);
    bool Refuse(std::string reason);
    bool PrepareMeasurement(const CommandContext& context, std::uint64_t generation, bool reset);
    void CaptureResult(ID3D12GraphicsCommandList* commandList);

    enum class MeasurementState { Empty, Recorded, Uncertain, Submitted };
    struct Measurement
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        MeasurementState state = MeasurementState::Empty;
        std::uint64_t generation = 0;
        std::uint64_t lifetime = 0;
        std::uint64_t fence = 0;
        std::uint64_t epoch = 0;
    };
    std::array<Measurement, CommandContext::FrameCount> m_measurements;
    unsigned int m_activeMeasurement = UINT_MAX;
    std::uint64_t m_epoch = 0;
    std::uint32_t m_lastFault = 0;
    std::uint64_t m_faultObservations = 0;
    std::uint64_t m_completedMeasurements = 0;
    bool m_faulted = false;
    D3D12_GPU_VIRTUAL_ADDRESS m_accumulateAddress = 0;

    GpuTimestampProfiler* m_profiler = nullptr;
    unsigned int m_frameSlot = 0;

    ComputePipeline m_trace;
    ComputePipeline m_clear;
    ComputePipeline m_maximum;
    ComputePipeline m_initialize;
    ComputePipeline m_replay;
    ComputePipeline m_decode;
    ComputePipeline m_finish;
    ComputePipeline m_invalidate;
    ComputePipeline m_seal;
    ComputePipeline m_validateComposite;
    ComputePipeline m_composite;
    bool m_initialized = false;
    bool m_initializationFailed = false;
    bool m_doublePrecision = false;

    Buffer m_records;
    Buffer m_sums;
    Buffer m_scales;
    Buffer m_history;
    Buffer m_control;
    Texture m_present;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    DXGI_FORMAT m_presentFormat = DXGI_FORMAT_UNKNOWN;
    std::uint64_t m_resourceBytes = 0;

    bool m_hasIdentity = false;
    std::array<std::uint64_t, 6> m_identity{};
    std::array<float, 16> m_lightRecord{};
    Matrix4 m_viewProjection{};
    bool m_resetPending = true;
    std::uint64_t m_recordedFrames = 0;
    std::uint64_t m_historyFrames = 0;
    Status m_status = Status::Uninitialized;
    std::string m_reason;
};
