#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

// UPSCALING.3A: what the NVIDIA DLSS Super Resolution backend can do on this machine. Ordered by how far initialization
// got; Available means a feature may be created, not that reconstruction is active.
enum class DlssAvailability
{
    // Built without ENGINE_ENABLE_DLSS.
    NotCompiled,
    // Initialize has not run (or Shutdown ran).
    NotInitialized,
    // nvngx_dlss.dll is not next to the executable, where NGX loads it from.
    RuntimeMissing,
    // NVSDK_NGX_D3D12_Init_with_ProjectID or the capability query failed (see the NGX result codes).
    InitializationFailed,
    // The driver is older than DLSS needs (see the minimum driver version).
    DriverOutdated,
    // NGX reports DLSS Super Resolution unavailable on this GPU/driver, or denied for this application.
    Unsupported,
    // NGX reports DLSS Super Resolution available; feature/evaluation state is tracked separately.
    Available
};

const char* DlssAvailabilityName(DlssAvailability availability) noexcept;

// UPSCALING.3D: the NGX Super Resolution quality modes Engine offers, whose render extents come from the NGX
// optimal-settings query (never from fixed scale factors).
enum class DlssQualityPreset
{
    Quality,
    Balanced,
    Performance
};

const char* DlssQualityPresetName(DlssQualityPreset preset) noexcept;

struct DlssStatus
{
    DlssAvailability availability = DlssAvailability::NotInitialized;
    // Raw NVSDK_NGX_Result values (0: the call was not made).
    std::uint32_t initResult = 0;
    std::uint32_t capabilityResult = 0;
    // SuperSampling.* capability parameters, valid once the capability query succeeded.
    bool superSamplingAvailable = false;
    bool needsUpdatedDriver = false;
    std::uint32_t minDriverVersionMajor = 0;
    std::uint32_t minDriverVersionMinor = 0;
    std::int32_t featureInitResult = 0;
    // Wall time of Initialize (NGX init plus the capability query).
    double initializeMilliseconds = 0.0;
    // One line for logs and the Editor readout.
    std::string detail;
};

// UPSCALING.3C: the DLSS Super Resolution feature of one render/output extent pair.
struct DlssFeatureStatus
{
    bool active = false;
    unsigned int renderWidth = 0;
    unsigned int renderHeight = 0;
    unsigned int outputWidth = 0;
    unsigned int outputHeight = 0;
    // NVSDK_NGX_PerfQuality_Value whose dynamic render range contains the render extent (-1: none).
    int perfQuality = -1;
    // UPSCALING.3D: the preset the feature was requested for (-1: none, the first mode whose range holds the extent).
    int requestedPreset = -1;
    // Raw NVSDK_NGX_Result values of the last creation and evaluation (0: not called).
    std::uint32_t createResult = 0;
    std::uint32_t evaluateResult = 0;
    std::uint64_t evaluations = 0;
    const char* perfQualityName = "";
    // The dynamic render ranges NGX reports per quality mode for this output extent.
    std::string supportedRanges;
    // Why the feature is or is not active.
    std::string detail;
};

// UPSCALING.3C: one evaluation. All resources at the states DLSS requires (Programming Guide 3.4): inputs
// NON_PIXEL_SHADER_RESOURCE, output UNORDERED_ACCESS; DLSS returns them to those states.
struct DlssEvaluation
{
    ID3D12Resource* color = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motionVectors = nullptr;
    ID3D12Resource* exposure = nullptr;
    // NGX InJitterOffsetX/Y (render pixels); the caller converts from Engine's convention.
    float jitterOffsetX = 0.0f;
    float jitterOffsetY = 0.0f;
    unsigned int renderWidth = 0;
    unsigned int renderHeight = 0;
    bool reset = false;
};

// UPSCALING.3A: owns the NGX SDK instance for one D3D12 device: initialization, the DLSS Super Resolution capability
// query and shutdown; UPSCALING.3C: the DLSS Super Resolution feature (creation, evaluation, release). Every NGX call in
// Engine goes through this class. Not thread-safe (NGX is not); the render thread owns it.
class DlssBackend
{
public:
    DlssBackend() = default;
    DlssBackend(const DlssBackend&) = delete;
    DlssBackend& operator=(const DlssBackend&) = delete;
    ~DlssBackend();

    // Once per device, after it is created. Never fails the caller: every outcome is reported through GetStatus. A
    // second call without Shutdown is ignored.
    void Initialize(ID3D12Device* device);

    // Releases the NGX instance only after GPU completion, before releasing the device. Idempotent.
    void Shutdown();

    const DlssStatus& GetStatus() const noexcept { return m_status; }

    // UPSCALING.3D: NGX's optimal render extent for preset at this output extent (Programming Guide 5.2.8). False when
    // DLSS is unavailable or NGX reports no extent for the combination.
    bool QueryOptimalRenderExtent(DlssQualityPreset preset, unsigned int outputWidth, unsigned int outputHeight,
        unsigned int& renderWidth, unsigned int& renderHeight);

    // UPSCALING.3C: creates the feature for this extent pair on commandList when none exists or the extents changed
    // (a changed feature is released first, which requires no DLSS work in flight: callers change extents only after a
    // GPU drain). True when a feature for exactly these extents is active. A failure is recorded in GetFeatureStatus and
    // not retried for the same extents. UPSCALING.3D preset: the feature uses that NGX quality mode, which must hold the
    // render extent; without one, the first mode whose range holds it (the diagnostic 75% mode).
    bool EnsureFeature(ID3D12GraphicsCommandList* commandList, unsigned int renderWidth, unsigned int renderHeight,
        unsigned int outputWidth, unsigned int outputHeight, std::optional<DlssQualityPreset> preset = std::nullopt);

    // Records one DLSS evaluation on commandList. NGX leaves the command list's state undefined (Programming Guide
    // 5.2.5): the caller rebinds descriptor heaps, pipeline, root signature, topology, viewport and targets afterwards.
    bool Evaluate(ID3D12GraphicsCommandList* commandList, const DlssEvaluation& evaluation);

    // Releases the feature. Only when no recorded DLSS work is in flight (after a GPU drain).
    void ReleaseFeature();

    const DlssFeatureStatus& GetFeatureStatus() const noexcept { return m_feature; }

    // Diagnostics only: the project ID in place of ENGINE_DLSS_PROJECT_ID (an invalid one exercises the failure path).
    std::optional<std::string> projectIdForTesting;
    // Diagnostics only: any value but Available makes EnsureFeature report no feature (simulates an unavailable DLSS)
    // while an existing feature stays untouched.
    std::optional<DlssAvailability> availabilityOverrideForTesting;

private:
    // The capability parameter map (allocated once); false with the failure in m_feature.
    bool EnsureParameters();

    ID3D12Device* m_device = nullptr;
    bool m_initialized = false;
    DlssStatus m_status;
    // NVSDK_NGX_Parameter* and NVSDK_NGX_Handle*, opaque here so that the header needs no SDK.
    void* m_parameters = nullptr;
    void* m_featureHandle = nullptr;
    DlssFeatureStatus m_feature;
};
