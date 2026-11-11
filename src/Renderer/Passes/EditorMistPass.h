#pragma once

// AIR.4F F3: the Editor's volumetric mist (EditorMist.hlsl): occupancy, a reduced-resolution raymarch through F1's piece-aware mist
// field, and a depth-aware composite into the final scene image. See Documentation/Air/AirArchitecture.md, AIR.4F F3.
//
// Storage (raw buffers, SRV and UAV in the bindless heap), created on the first frame that draws mist and replaced only when the
// target size or the window's brick count changes: the reduced result (float4 per pixel), its scene distances (float per pixel) and
// the occupancy flags (one u32 per 8-cell brick). A frame that draws no mist retires them with the last submission fence, so a hidden
// mist holds nothing; retired buffers are released once the GPU completes that fence (or at Shutdown, after the Renderer's drain).

#include "Math/Matrix4.h"
#include "Math/Vector3.h"
#include "Renderer/AirField.h"
#include "Renderer/Buffer.h"
#include "Renderer/ComputePipeline.h"
#include "Renderer/Pipeline.h"
#include "Scene/MistRenderSettings.h"

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>

#include <d3d12.h>
#include <wrl/client.h>

class DepthBuffer;
class DescriptorHeap;
class RenderTarget;
class ResourceUploader;
class ShaderLibrary;
class UploadBuffer;

enum class AirMistQuality : std::uint32_t
{
    // Half width and height, at most 32 steps.
    Low = 0,
    // Half width and height, at most 64 steps.
    Medium = 1,
    // Half the pixels (1/sqrt(2) per axis), at most 96 steps.
    High = 2
};

// What the mist view draws this frame.
struct AirMistRenderRequest
{
    // Multiplies the mist's extinction (0 draws nothing visible).
    float opacity = 1.0f;
    AirMistQuality quality = AirMistQuality::Medium;
    // AIR.4F F4.1: flow-advected Worley detail (EditorMist.hlsl). Strength 0 to 1 blends the density from unmodulated to fully
    // modulated; the scale is the noise tile's size in metres. Without the noise texture, detail is skipped.
    bool detail = true;
    float detailStrength = 0.5f;
    float detailScale = 4.0f;
    // AIR.4F F4.2: each ray's stable per-pixel start offset (off: every ray starts at its span's entry, which bands), and the
    // composite's 4 x 4 tent upsample (off: F3's 2 x 2 bilinear). Diagnostics; the Editor always uses both.
    bool jitter = true;
    bool wideUpsample = true;
    // AIR.4F F4.4.2b diagnostics (the Editor never sets them): the raymarch step's scale (0.25: four times as many samples), and the
    // reduced target's scale per axis (0: the preset's; 1: full resolution).
    float stepScale = 1.0f;
    float resolutionScale = 0.0f;
    // AIR.4F F4.3 single scattering (EditorMist.hlsl). Off: F4.2's unlit colour, composited after tone mapping. On: the mist scatters
    // the scene's light (AirMistLighting) and is composited into the HDR scene colour before tone mapping. sunShadows shadows the sun
    // through per-piece visibility (off: the sun reaches every sample). scattering multiplies the mist colour as its scattering albedo
    // (0 to 1); anisotropy is the Henyey-Greenstein g (-0.9 to 0.9); ambient (F4.3.1) is the fraction of the environment's mean
    // radiance that reaches the mist, unshadowed (0 to 1; 0: none).
    bool lighting = false;
    bool sunShadows = true;
    float scattering = 1.0f;
    float anisotropy = 0.3f;
    float ambient = 0.0f;
    // AIR.4F F4.4: curl-warped Worley detail (needs the detail and the curl texture). curlStrength is the RMS displacement of the
    // detail's lookup in metres (0: none, exactly the unwarped detail), curlScale the curl tile's size in metres, curlFlow how far the
    // curl pattern follows the local air (0 to 1).
    bool curl = false;
    float curlStrength = 0.4f;
    float curlScale = 6.0f;
    float curlFlow = 1.0f;
    // AIR.4F F4.4.2: wisps (needs the detail and the filament texture): strands of the filament mask multiply the detail factor by
    // lerp(1, mask / mean(mask), wispStrength) (0: none, exactly the detail without them); wispScale is the mask tile's size in metres.
    bool wisps = false;
    float wispStrength = 0.6f;
    float wispScale = 8.0f;
    // AIR.4F F4.4.2e: density-dependent detail (needs the detail). With n = 1 - exp(-density x extinction x 1 m), the opacity of a metre
    // of the simulated mist, and s = densityDetailStrength (0 to 1; 0: exactly the detail without it): the Worley strength becomes
    // min(1, strength x (1 + s n / 2)); the wisp value becomes lerp(mask / mean(mask), mask^2 / mean(mask^2), s (1 - n)) and the wisp
    // strength wispStrength x (1 - s n). Every factor still averages 1, so the mist's mean density is unchanged.
    bool densityDetail = false;
    float densityDetailStrength = 1.0f;

    bool operator==(const AirMistRenderRequest&) const = default;
};

// AIR.4F F4.4.1: a scene's mist appearance (MistRenderSettings) and the Editor's quality preset as one frame's request; the
// implementation switches (jitter, upsample) keep their defaults.
AirMistRenderRequest MakeAirMistRenderRequest(const MistRenderSettings& settings, AirMistQuality quality) noexcept;

// AIR.4F F4.3: the frame's scene lighting a lit mist pass reads, as deferred lighting uses it.
struct AirMistLighting
{
    // The direction the sun's light travels, and colour x intensity (render units; zero for a dark sun).
    Vector3 sunDirection{0.0f, -1.0f, 0.0f};
    Vector3 sunRadiance{0.0f, 0.0f, 0.0f};
    // The uploaded SceneLightConstants (the table Lights, unshadowed) and the scene's TLAS (0: none, and the sun is not shadowed).
    D3D12_GPU_VIRTUAL_ADDRESS lightTable = 0;
    D3D12_GPU_VIRTUAL_ADDRESS topLevel = 0;
    // AIR.4F F4.3.1: the environment's diffuse irradiance cube SRV, as deferred lighting reads it (UINT32_MAX: no ambient light).
    std::uint32_t environmentIrradiance = UINT32_MAX;
};

// AIR.4F F4.1: the detail's two flow phases at a transport time, as EditorMist.hlsl reads them (AirMistDetail).
struct AirMistDetailPhases
{
    float offsetA[3] = {0.0f, 0.0f, 0.0f};
    float offsetB[3] = {0.0f, 0.0f, 0.0f};
    // Seconds since each phase restarted, and phase A's weight.
    float timeA = 0.0f;
    float timeB = 0.0f;
    float weightA = 1.0f;
};

// The reduced target's scale per axis and the step limit of a preset.
struct AirMistQualitySettings
{
    float scale = 0.5f;
    std::uint32_t maxSteps = 64;
};

AirMistQualitySettings AirMistQualityOf(AirMistQuality quality) noexcept;

class EditorMistPass
{
public:
    // Extinction per metre per unit of mist density at opacity 1: a density of 1 hides about 63% of the background per metre.
    static constexpr float ExtinctionPerDensity = 1.0f;
    // AIR.4F F4.1: each detail phase lasts this long before it restarts (while its weight is zero).
    static constexpr double DetailPhaseSeconds = 2.0;

    // Phase B runs half a period behind phase A; each restarts (a new cycle offset, its time back to 0) exactly when its weight
    // 1 - |2 * phase - 1| is 0.
    static AirMistDetailPhases DetailPhasesAt(double mistSeconds) noexcept;

    // `noisePath` names the Worley texture (a 3D R8_UNORM DDS of equal power-of-two sides); when it is missing or invalid the pass
    // still initializes and draws without detail. AIR.4F F4.4: `curlPath` names the curl texture (a 3D R16G16B16A16_SNORM DDS of equal
    // power-of-two sides); without it the detail is never warped.
    bool Initialize(ID3D12Device* device, DescriptorHeap& descriptorHeap, ShaderLibrary& shaderLibrary, ResourceUploader& uploader,
        const std::filesystem::path& noisePath, const std::filesystem::path& curlPath = {}, const std::filesystem::path& filamentPath = {});
    // After the Renderer's drain.
    void Shutdown();

    // Draws the mist into `viewportColor` (the final scene image) against `viewportDepth`, both full size. Returns whether anything
    // was recorded. `timestamps`, when given, receives four timestamps from `firstQuery`: start, after occupancy (and, lit and shadowed,
    // the sun's visibility), after the raymarch, after the composite.
    // `mistSeconds` is the mist's transport time (AirField::GetMistSeconds), which drives the detail's phases.
    // AIR.4F F4.3: a lit request needs `lighting` (with a light table) and an R16G16B16A16_FLOAT HDR `viewportColor`; an unlit one draws
    // into the R8G8B8A8_UNORM final image. Either way the other combination draws nothing.
    bool Execute(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, UploadBuffer& upload, RenderTarget& viewportColor,
        DepthBuffer& viewportDepth, const Matrix4& viewProjection, const Vector3& cameraPosition, const AirFieldBinding& field,
        const AirMistBinding& mist, const AirMistRenderRequest& request, double mistSeconds, std::uint64_t lastSubmissionFence,
        std::uint64_t completedFence, ID3D12QueryHeap* timestamps = nullptr, UINT firstQuery = 0,
        const AirMistLighting* lighting = nullptr);

    // A frame without mist: retires the storage (released once `lastSubmissionFence` completes) and collects completed retirements.
    void Idle(std::uint64_t lastSubmissionFence, std::uint64_t completedFence);

    bool HasStorage() const noexcept
    {
        return m_result.GetResource() != nullptr;
    }

    std::uint64_t GetExecutions() const noexcept
    {
        return m_executions;
    }

    bool HasNoise() const noexcept
    {
        return m_noise != nullptr;
    }

    bool HasCurl() const noexcept
    {
        return m_curl != nullptr;
    }

    bool HasFilaments() const noexcept
    {
        return m_filaments.resource != nullptr;
    }

    // AIR.4F F4.3: whether the last lit execution shadowed the sun (it had a TLAS and sunShadows), and its visibility rays (two per
    // brick cell of every brick it traced: an upper bound; pieces without fluid cast none).
    bool WasSunShadowed() const noexcept
    {
        return m_sunShadowed;
    }

    std::uint64_t GetVisibilityRayBound() const noexcept
    {
        return m_visibilityRayBound;
    }

private:
    friend class AirFieldVerification;

    struct RetiredBuffer
    {
        Buffer buffer;
        std::uint64_t fence = 0;
    };

    std::array<Buffer*, 4> StorageBuffers() noexcept
    {
        return {&m_result, &m_distance, &m_occupancy, &m_visibility};
    }

    bool EnsureStorage(std::uint32_t width, std::uint32_t height, std::uint32_t bricks, std::uint64_t lastSubmissionFence);
    // Loads the noise texture and its SRV; false (with a warning, and no texture) when the file is missing or not as expected.
    // A loaded R8_UNORM 3D noise texture: the resource, its SRV, its size, 1 / its mean value and (F4.4.2e) 1 / the mean of its square.
    struct ScalarVolume
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        unsigned int descriptor = UINT32_MAX;
        std::uint32_t size = 0;
        float meanInverse = 1.0f;
        float squareMeanInverse = 1.0f;
    };

    // Loads a power-of-two R8_UNORM 3D cube (`what` names it in warnings); false, with a warning, otherwise.
    bool LoadScalarVolume(ResourceUploader& uploader, const std::filesystem::path& path, const char* what, ScalarVolume& volume);
    bool LoadNoise(ResourceUploader& uploader, const std::filesystem::path& path);
    void ReleaseNoise();
    // AIR.4F F4.4.2: the filament mask (MistFilaments128.dds).
    bool LoadFilaments(ResourceUploader& uploader, const std::filesystem::path& path);
    void ReleaseFilaments();
    // AIR.4F F4.4: loads the curl texture and its SRV, and 1 / its RMS vector length; false (with a warning, and no texture) otherwise.
    bool LoadCurl(ResourceUploader& uploader, const std::filesystem::path& path);
    void ReleaseCurl();
    void RetireStorage(std::uint64_t fence);
    void CollectRetired(std::uint64_t completedFence);

    ID3D12Device* m_device = nullptr;
    DescriptorHeap* m_descriptorHeap = nullptr;
    ComputePipeline m_occupancyPipeline;
    ComputePipeline m_raymarchPipeline;
    Pipeline m_compositePipeline;
    // AIR.4F F4.3: the sun visibility pass, and the composite into the HDR scene colour for lit mist.
    ComputePipeline m_visibilityPipeline;
    Pipeline m_hdrCompositePipeline;
    Buffer m_result;
    Buffer m_distance;
    Buffer m_occupancy;
    // AIR.4F F4.3: one uint per window cell (two bits: each piece's sun visibility).
    Buffer m_visibility;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint32_t m_bricks = 0;
    std::deque<RetiredBuffer> m_retired;
    // AIR.4F F4.1: the Worley texture (immutable after load, so it lives until Shutdown), its SRV and 1 / its mean value.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_noise;
    unsigned int m_noiseDescriptor = UINT32_MAX;
    float m_noiseMeanInverse = 1.0f;
    std::uint32_t m_noiseSize = 0;
    // AIR.4F F4.4: the curl texture (immutable after load), its SRV and 1 / its RMS vector length.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_curl;
    unsigned int m_curlDescriptor = UINT32_MAX;
    float m_curlRmsInverse = 1.0f;
    std::uint32_t m_curlSize = 0;
    // AIR.4F F4.4.2: the filament mask (immutable after load).
    ScalarVolume m_filaments;
    std::uint64_t m_executions = 0;
    std::uint64_t m_allocations = 0;
    bool m_sunShadowed = false;
    std::uint64_t m_visibilityRayBound = 0;
};
