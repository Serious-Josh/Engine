#pragma once

#include "Math/Matrix4.h"
#include "Renderer/Buffer.h"
#include "Renderer/CommandContext.h"
#include "Renderer/ComputePipeline.h"
#include "Renderer/GpuTimestampProfiler.h"
#include "Renderer/Texture.h"

#include <wrl/client.h>

#include <array>
#include <climits>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class DescriptorHeap;
class GraphicsDevice;
class RenderTarget;
class ShaderLibrary;
class UploadBuffer;

// R.9I.3 production integration: the pixel-centred dedicated caustics estimator (R9I3PixelCentredCausticsGPU*.md)
// in Raster Primary + ReSTIR DI. P1 runs the validated B3-T bounded tiled estimator (assets/Shaders/CausticPixel.hlsl)
// at K = 1 into two production-owned lobe textures. Nothing is combined with ordinary lighting or denoised.
//
// Frame contract. RecordCausticSnapshot accepts at most one caustic snapshot per recording, after every same-frame
// material-table, displacement and TLAS write and before the geometry pass. That snapshot (never a retained earlier
// one) is passed to the seam in RecordPathTraceReference: after ExecuteReSTIRDISplit's output barriers, before any
// denoiser reads the extraction. Renderer::VerifyPixelCausticSeam checks it against the live renderer state: this
// recording generation and frame slot, scene-resource lifetime, current registration, geometry reader still
// Recorded into this list, TLAS address and recording identity, material revision and this slot's table, and the
// scene extent. Only a verified seam records the estimator, into the frame's own command list.
//
// Outputs (scene extent; created on first use, released only at drained boundaries):
//   diffuse    RGBA32F  rgb: this frame's receiver diffuse-lobe radiance (material-modulated, K attempts divided by
//                       the full K, null/occluded attempts included); a: PixelCausticState
//   specular   RGBA32F  rgb: receiver specular-lobe radiance (full BSDF minus the diffuse lobe); a: fault bits
//   faultLatch 32 bytes [0] OR of every published fault, [1] faulting receivers, [2] first faulting pixel,
//                       [3] this frame's epoch (cleared by the frame's first tile); [4] reserved, [5] insertion
//                       refusal bits, [6] pixels validated for insertion, [7] pixels inserted (cleared by CSBegin).
// A pixel's two lobes are published together or not at all: a fault writes zero in both with state Fault. A frame
// whose latch word 0 is nonzero is refused as a whole by every consumer (the debug view now; P2 insertion later).
// After the recording both lobes are NON_PIXEL_SHADER_RESOURCE and the latch is copied to a per-recording readback,
// read only once that submission's fence has completed (CollectCompleted): CPU state never claims GPU execution.
//
// P2 insertion (optional, its own default-off gate) is a GPU-only transaction in the same recording; no CPU readback
// or wait gates it (assets/Shaders/CausticPixelInsert.hlsl):
//   1. CSBegin, before the estimator: every pixel's state becomes the unpublished sentinel; latch words 4..7 clear.
//   2. The estimator publishes both lobes per pixel and ORs every fault into latch word 0. UAV barrier.
//   3. CSValidate over the whole image: the estimator latch and its epoch, no unpublished pixel, legal and
//      self-consistent states, and FP32 extraction + caustic finite and within +-65504 for both lobes of every
//      accepted pixel. Any failure ORs a refusal bit into word 5; no image is written. UAV barrier.
//   4. CSInsert: if word 0, word 5 or the epoch disagree, nothing is written anywhere (whole-image refusal);
//      otherwise each accepted nonzero pixel stores extraction + caustic in each lobe that has caustic radiance,
//      keeping the extraction alpha. The raw diffuse hit distance, G-buffer and accumulation are never written.
//   5. UAV barrier on the extraction textures, before the Engine temporal pass or NRD preparation reads them.
//   Validation completes before insertion begins, so no invocation can add a refusal after another has written.
//   Rounding: the half extraction value is widened to FP32, the FP32 caustic added once, and the sum stored to
//   RGBA16F; the stored value is within one half-precision ULP of the exact FP32 sum (D3D permits either rounding
//   of a UAV float-to-half store). The denoisers' own packed range is not validated here (P3).

// The B3-T state codes (CSClassify/CSPublish), unchanged so the independent assessor reads production output.
enum class PixelCausticState : std::uint32_t
{
    Background = 0,   // Depth 1: no receiver surface.
    Uncertified = 1,  // Decoded surface whose centre ray does not certify the registered receiver.
    Unscheduled = 3,  // Certified, deliberately not processed this frame.
    AcceptedZero = 4, // Valid Monte Carlo estimate of zero (not a claim of zero transport).
    Accepted = 5,     // Valid nonzero estimate.
    Fault = 6         // Refused; contributes nothing.
};

// The B3-T fault bits, unchanged.
enum PixelCausticFault : std::uint32_t
{
    PixelCausticFaultNone = 0u,
    PixelCausticFaultEpoch = 1u << 0,           // Stale epoch, empty proposal, or invalid mixture fraction.
    PixelCausticFaultProposal = 1u << 1,        // Invalid PDF, proposal parameter or sample point.
    PixelCausticFaultDistance = 1u << 2,        // Degenerate light or receiver segment.
    PixelCausticFaultRadiance = 1u << 3,        // Nonfinite or negative contribution.
    PixelCausticFaultReconstruction = 1u << 4,  // The receiver tuple no longer reconstructs.
    PixelCausticFaultOutputRange = 1u << 6,     // A reduced lobe nonfinite, negative or above the fp16 maximum.
    PixelCausticFaultMalformedPrimary = 1u << 9 // Malformed nonbackground primary depth.
};

// One accepted reflector triangle in world space: CausticPixel.hlsl's MeshTriangle (80 bytes).
struct PixelCausticTriangle
{
    float a[3]{};
    float area = 0.0f;
    float b[3]{};
    float areaCumulative = 0.0f;
    float c[3]{};
    std::uint32_t primitive = 0;
    // Exact identity of the triangle's reflector: TLAS InstanceID (dense slot), native instance index
    // and material table index. Stored as uint32 so no identity passes through a float.
    std::uint32_t instanceID = 0;
    std::uint32_t instanceIndex = 0;
    std::uint32_t material = 0;
    std::uint32_t reserved[5]{};
};

static_assert(sizeof(PixelCausticTriangle) == 80);

// Everything one recording of the estimator reads. The Renderer builds it from the verified seam's snapshot and
// this recording's raster-primary camera; every address belongs to resources retained for this recording.
struct PixelCausticFrameInput
{
    D3D12_GPU_VIRTUAL_ADDRESS topLevel = 0;
    D3D12_GPU_VIRTUAL_ADDRESS materialTable = 0;
    unsigned int materialTableDescriptor = UINT_MAX; // Heap SRV of the same table (sized by GetDimensions).
    std::uint32_t reflectorMaterial = 0;
    std::uint32_t receiverMaterial = 0;
    std::uint32_t materialEnergyTable = 0;
    std::uint32_t materialAverageTable = 0;
    std::uint32_t materialCosineMapping = 0;
    std::uint32_t reflectorDense = 0;
    std::uint32_t reflectorNative = 0;
    std::uint32_t receiverDense = 0;
    std::uint32_t receiverNative = 0;
    std::uint32_t epoch = 0; // Nonzero; this recording's identity.
    std::uint32_t seed = 0;
    std::array<float, 16> lightRecord{}; // GPULight.
    Matrix4 inverseCameraRelativeViewProjection{};
    std::array<float, 3> cameraPosition{};
    unsigned int depthIndex = UINT_MAX;
    unsigned int materialIndex = UINT_MAX;
    unsigned int surfaceIndex = UINT_MAX;
    unsigned int geometricNormalIndex = UINT_MAX;
    // P5D.2A: the G-buffer raster identity SRV (the identity audit, and P5D.2B automatic receiver certification).
    unsigned int identityIndex = UINT_MAX;
    // P5D.2B automatic receivers: certification by exact raster/TLAS identity against this per-InstanceID table
    // ({eligible, native instance, material index, 0}); empty and false for the explicit receiver path.
    bool automaticReceivers = false;
    std::vector<std::array<std::uint32_t, 4>> receiverTable;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<PixelCausticTriangle> triangles;
    float totalArea = 0.0f;
};

// The extraction textures one recording's insertion transaction targets (null: no insertion this frame). The raw
// diffuse hit distance is never written; it is named only so verification can show it unchanged.
struct PixelCausticInsertionTarget
{
    Texture* diffuse = nullptr;
    Texture* specular = nullptr;
    Texture* rawDiffuseHitDistance = nullptr;
};

// The seam's verdict for one recording.
struct PixelCausticSeamResult
{
    bool verified = false;
    bool extractionAvailable = false; // P2 insertion also needs this frame's extraction (a PT denoiser selected).
    bool recorded = false;            // The estimator's commands are in this recording.
    bool insertionRecorded = false;   // The insertion transaction's commands are in this recording.
    std::uint64_t snapshotIdentity = 0;
    std::uint64_t recordingGeneration = 0;
    std::string reason;
};

// Caustics-only debug views (CausticPixelView.hlsl). None alters a lighting signal or denoiser input.
enum class PixelCausticView : std::uint32_t
{
    None = 0,
    Diffuse = 1,
    Specular = 2,
    Combined = 3,
    Validity = 4
};

class PixelCausticPass
{
public:
    static constexpr DXGI_FORMAT OutputFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
    static constexpr std::uint32_t FaultLatchBytes = 32;
    static constexpr std::uint32_t AttemptsPerReceiver = 1; // K for production frames.
    static constexpr std::uint32_t MaximumAttemptsPerReceiver = 16; // The B3-T validated bound.
    static constexpr std::uint32_t TileCapacity = 4096;     // The B3-T validated tile capacity.
    static constexpr std::uint32_t MaximumTriangles = 128;
    static constexpr std::uint32_t MaximumDimension = 4096;
    static constexpr std::uint64_t MaximumResourceBytes = 512ull << 20;

    bool Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary);
    void Shutdown();

    void BeginFrame()
    {
        m_lastSeam = {};
        m_lastSeam.reason = "not a Raster Primary + ReSTIR DI path-traced frame";
        m_recordedThisFrame = false;
    }

    // While disabled or refused by the gate; never counts as a seam.
    void SetInactiveReason(std::string reason)
    {
        m_lastSeam = {};
        m_lastSeam.reason = std::move(reason);
        m_recordedThisFrame = false;
    }

    void RecordSeam(PixelCausticSeamResult result)
    {
        ++m_seamVisits;
        m_verifiedSeams += result.verified ? 1u : 0u;
        m_lastSeam = std::move(result);
    }

    void SetInsertionRecorded(bool recorded)
    {
        m_lastSeam.insertionRecorded = recorded;
    }

    // Updates the last seam's verdict after the estimator was recorded or refused.
    void SetSeamRecorded(bool recorded, std::string reason)
    {
        m_lastSeam.recorded = recorded;
        m_lastSeam.reason = std::move(reason);
    }

    const PixelCausticSeamResult& GetLastSeam() const noexcept
    {
        return m_lastSeam;
    }

    std::uint64_t GetSeamVisits() const noexcept
    {
        return m_seamVisits;
    }

    std::uint64_t GetVerifiedSeams() const noexcept
    {
        return m_verifiedSeams;
    }

    // Records classification, proposal setup, sampling with FP64 reduction and publication for every image pixel,
    // in tiles of TileCapacity, followed by the latch readback copy. Everything fallible (pipelines, allocation,
    // upload space, measurement capacity) is checked before the first command; a refusal records nothing.
    bool Record(ID3D12GraphicsCommandList* commandList, ID3D12Device* device, DescriptorHeap& heap,
        UploadBuffer& uploadBuffer, const PixelCausticFrameInput& input, const PixelCausticInsertionTarget* insertion,
        const CommandContext& context,
        std::uint64_t generation, GpuTimestampProfiler& profiler, unsigned int frameSlot);

    // Replaces the ordinary output-to-scene-colour copy with a caustics-only view of this recording's outputs.
    bool RecordView(ID3D12GraphicsCommandList* commandList, DescriptorHeap& heap, UploadBuffer& uploadBuffer,
        PixelCausticView view, RenderTarget& sceneColor, GpuTimestampProfiler& profiler, unsigned int frameSlot);

    bool WasRecordedThisFrame() const noexcept
    {
        return m_recordedThisFrame;
    }

    const std::string& GetReason() const noexcept
    {
        return m_reason;
    }

    // Submission ownership, as CausticPass: a recording's latch readback is read only after its fence completes.
    void CollectCompleted(const CommandContext& context);
    void ArmSubmission(std::uint64_t generation);
    void ConfirmSubmission(std::uint64_t generation, std::uint64_t fence, std::uint64_t lifetime);
    void ProvenNotSubmitted(std::uint64_t generation);
    void DiscardRecording(std::uint64_t generation);

    // GPU-confirmed evidence: completed recordings whose latch carried their own epoch.
    std::uint64_t GetCompletedFrames() const noexcept
    {
        return m_completedFrames;
    }

    std::uint64_t GetFaultedFrames() const noexcept
    {
        return m_faultedFrames;
    }

    std::uint32_t GetLastCompletedFaults() const noexcept
    {
        return m_lastCompletedFaults;
    }

    std::uint32_t GetLastCompletedEpoch() const noexcept
    {
        return m_lastCompletedEpoch;
    }

    // Whether this device can read and write RGBA16F through typed UAVs, which insertion requires.
    bool IsInsertionSupported() const noexcept
    {
        return m_insertionSupported;
    }

    // GPU-confirmed insertion evidence of the newest completed recording that carried the transaction.
    struct InsertionResult
    {
        bool recorded = false;
        std::uint32_t epoch = 0;
        std::uint32_t refusal = 0;
        std::uint32_t validated = 0;
        std::uint32_t inserted = 0;
    };

    const InsertionResult& GetLastCompletedInsertion() const noexcept
    {
        return m_lastCompletedInsertion;
    }

    std::uint64_t GetInsertedFrames() const noexcept
    {
        return m_insertedFrames;
    }

    std::uint64_t GetRefusedInsertions() const noexcept
    {
        return m_refusedInsertions;
    }

    // Committed bytes of every resource this pass owns.
    std::uint64_t GetResourceBytes() const noexcept;

    // Only at a drained boundary (resize, shutdown): no recorded frame may still use them.
    void ReleaseResources();

    Texture& GetDiffuseTexture()
    {
        return m_diffuse;
    }

    Texture& GetSpecularTexture()
    {
        return m_specular;
    }

    Buffer& GetFaultLatch()
    {
        return m_faultLatch;
    }

    // The caustics-only view's output, copied to scene colour by RecordView.
    Texture& GetViewTexture()
    {
        return m_present;
    }

#if defined(ENGINE_HAS_VERIFICATION)
    // Verification only: B3-T selected-probe capture and controls, through the CausticPixelProbes variant. Production
    // builds compile none of this; production frames always run CausticPixel.hlsl at K = AttemptsPerReceiver.
    struct Diagnostics
    {
        bool enabled = false;
        std::uint32_t attemptsPerReceiver = AttemptsPerReceiver;
        std::uint32_t injectFlags = 0;
        std::uint32_t scheduleDivisor = 1;
        bool seedOverride = false;
        std::uint32_t seed = 0;
        std::array<std::uint32_t, 8> probePixels{};
        // P2: corrupt one publication (CausticPixelInsertTamper.hlsl mode) before validation, and capture the
        // extraction before and after insertion with the caustic outputs.
        std::uint32_t insertionTamper = 0;
        std::array<std::uint32_t, 2> tamperPixel{};
        bool captureInsertion = false;
        // P5D.2A: compare every pixel's raster identity with the pixel-centre TLAS hit (CausticRasterIdentityAudit).
        bool identityAudit = false;
    };

    // P5D.2A identity audit result words (ReadDiagnosticIdentity): 8 category counts, the record count, words 10-12
    // the whole-image certification audit (certified, certified but disagreeing, certified and agreeing), then
    // IdentityAuditRecords records of 8 words from word 16 (see CausticRasterIdentityAudit.hlsl).
    static constexpr std::uint32_t IdentityAuditRecords = 256;
    static constexpr std::uint64_t IdentityAuditBytes = (16ull + IdentityAuditRecords * 8ull) * 4ull;

    // Row-packed texel bytes of the last captured insertion (valid after its submission has completed).
    struct InsertionCapture
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> diffuseBefore;
        std::vector<std::uint8_t> specularBefore;
        std::vector<std::uint8_t> rawBefore;
        std::vector<std::uint8_t> diffuseAfter;
        std::vector<std::uint8_t> specularAfter;
        std::vector<std::uint8_t> rawAfter;
        std::vector<std::uint8_t> causticDiffuse;
        std::vector<std::uint8_t> causticSpecular;
        std::vector<std::uint8_t> latch;
    };

    bool ReadDiagnosticInsertion(InsertionCapture& capture) const;

    // The last diagnostic recording's inputs and probe readbacks (valid after its submission has completed).
    struct DiagnosticCapture
    {
        std::uint32_t epoch = 0;
        std::uint32_t seed = 0;
        std::uint32_t attempts = 0;
        std::vector<PixelCausticTriangle> triangles;
        float totalArea = 0.0f;
        Matrix4 inverseCameraRelativeViewProjection{};
        std::array<float, 3> cameraPosition{};
    };

    void SetDiagnostics(const Diagnostics& diagnostics)
    {
        m_diagnostics = diagnostics;
    }

    const DiagnosticCapture& GetDiagnosticCapture() const noexcept
    {
        return m_diagnosticCapture;
    }

    // Copies the completed probe readbacks: attempts 8x16x96, tuples 8x192, targets 8xNx32, outputs 8x64, latch 16.
    bool ReadDiagnosticIdentity(std::vector<std::uint32_t>& words) const;
    bool ReadDiagnosticProbes(std::vector<std::uint8_t>& attempts, std::vector<std::uint8_t>& tuples,
        std::vector<std::uint8_t>& targets, std::vector<std::uint8_t>& outputs, std::vector<std::uint8_t>& latch) const;
#endif

private:
    enum class MeasurementState { Empty, Recorded, Uncertain, Submitted };

    struct Measurement
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        MeasurementState state = MeasurementState::Empty;
        std::uint64_t generation = 0;
        std::uint64_t lifetime = 0;
        std::uint64_t fence = 0;
        std::uint32_t epoch = 0;
        bool insertion = false;
    };

    bool Refuse(std::string reason);
    bool EnsureResources(ID3D12Device* device, DescriptorHeap& heap, std::uint32_t width, std::uint32_t height,
        std::uint32_t triangleCount);
    // Returns states to the stable boundary of a completed recording.
    void RestoreStableStates();

    bool m_initialized = false;
    bool m_initializationFailed = false;
    ComputePipeline m_classify;
    ComputePipeline m_setup;
    ComputePipeline m_sampleReduce;
    ComputePipeline m_publish;
    ComputePipeline m_view;
    ComputePipeline m_insertBegin;
    ComputePipeline m_insertValidate;
    ComputePipeline m_insertApply;
    bool m_insertionSupported = false;

    Texture m_diffuse;
    Texture m_specular;
    Texture m_present;
    Buffer m_targets;
    Buffer m_scratch;
    Buffer m_faultLatch;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint32_t m_targetTriangles = 0;
    std::uint64_t m_resourceBytes = 0;
    // CPU-tracked states when the current recording began: diffuse, specular, targets, scratch, latch.
    std::array<D3D12_RESOURCE_STATES, 5> m_discardStates{};

    std::array<Measurement, CommandContext::FrameCount + 1> m_measurements;
    std::uint64_t m_completedFrames = 0;
    std::uint64_t m_faultedFrames = 0;
    std::uint32_t m_lastCompletedFaults = 0;
    std::uint32_t m_lastCompletedEpoch = 0;
    InsertionResult m_lastCompletedInsertion;
    std::uint64_t m_insertedFrames = 0;
    std::uint64_t m_refusedInsertions = 0;
    bool m_recordedThisFrame = false;
    std::string m_reason = "not recorded";

    PixelCausticSeamResult m_lastSeam;
    std::uint64_t m_seamVisits = 0;
    std::uint64_t m_verifiedSeams = 0;

#if defined(ENGINE_HAS_VERIFICATION)
    bool EnsureDiagnosticResources(ID3D12Device* device, DescriptorHeap& heap, std::uint32_t triangleCount);

    Diagnostics m_diagnostics;
    DiagnosticCapture m_diagnosticCapture;
    bool m_diagnosticPipelinesReady = false;
    ComputePipeline m_probeClassify;
    ComputePipeline m_probeSetup;
    ComputePipeline m_probeSampleReduce;
    ComputePipeline m_probePublish;
    Buffer m_probeAttempts;
    Buffer m_probeTuples;
    Buffer m_probeOutputs;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_probeAttemptsReadback;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_probeTuplesReadback;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_probeTargetsReadback;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_probeOutputsReadback;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_probeLatchReadback;

    // P2 verification: the tamper kernel and the insertion capture readbacks (texture footprints).
    struct TextureReadback
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0;
        UINT64 rowBytes = 0;
    };

    bool RecordTextureReadback(ID3D12GraphicsCommandList* commandList, ID3D12Device* device, Texture& texture,
        TextureReadback& readback);
    ComputePipeline m_insertTamper;
    bool m_insertTamperReady = false;
    ComputePipeline m_identityClear;
    ComputePipeline m_identityAudit;
    ComputePipeline m_identityCertification;
    bool m_identityAuditReady = false;
    Buffer m_identityAuditBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_identityAuditReadback;
    std::array<TextureReadback, 8> m_insertionReadbacks;
    bool m_insertionCaptured = false;
    std::uint32_t m_insertionCaptureWidth = 0;
    std::uint32_t m_insertionCaptureHeight = 0;
    std::uint32_t m_probeTriangles = 0;
#endif
};
