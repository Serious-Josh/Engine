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
#include "Scene/EnvironmentalHazeSettings.h"
#include "Scene/MistRenderSettings.h"

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

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

// AIR.4E.5c.2: the extent the reduced raymarch target scales from. The composite always covers the output.
enum class AirMistResolutionBasis : std::uint32_t
{
    // The image the mist is composited into (the output extent; before AIR.4E.5c.2, the only basis).
    Output = 0,
    // The frame's scene-render extent (below the output under DLSS or a render scale). Experimental.
    Internal = 1
};

// V1b-1 diagnostics (AirVolumetricV1b1Report.md): how the raymarch reads the froxel sun visibility volume.
enum class AirFroxelFilter : std::uint32_t
{
    // V1a: a raw buffer, trilinear by eight loads in the shader.
    Manual = 0,
    // A 3D R16_UNORM texture with hardware trilinear filtering (the traced volume copied in by CSFroxelResolve).
    Hardware = 1
};

// V1b-1 diagnostics: where each froxel's sample lies inside it when froxelJitter is on (off: always its centre).
enum class AirFroxelJitterSequence : std::uint32_t
{
    // V1a: a per-froxel hash of its index and froxelSeed, the same every frame.
    Static = 0,
    // Halton(5) along depth, centred in x and y; one offset for every froxel, advanced each frame (16-frame cycle).
    Depth = 1,
    // Halton(2, 3, 5) in x, y and depth; the depth component equals Depth's.
    Full = 2
};

// V1b-2 diagnostics: how CSFroxelResolve rectifies the reprojected history before blending.
enum class AirFroxelHistoryClip : std::uint32_t
{
    // V1b-1: none (a plain exponential moving average).
    None = 0,
    // Variance clipping against the traced centre and its six face neighbours (Lerviks 2026).
    Variance = 1,
    // V1b-3: the variance box widened by the froxel's own temporal deviation (froxelTemporalGamma; needs the second-moment channel).
    HistoryAware = 2
};

// Fog step 1: how the render volume's fast path blends an open footprint (EditorMist.hlsl, RenderVolumeSample).
enum class AirMistRenderVolumeFilter : std::uint32_t
{
    // Eight Loads blended in the shader with AirRenderMistAndVelocity's own weights: only fp16 rounding differs.
    Manual = 0,
    // One hardware trilinear fetch per texture (weights quantized by the sampler, typically to 8 bits).
    Hardware = 1,
    // Manual within renderVolumeNearDistance metres of the camera, hardware beyond.
    Hybrid = 2
};

// Fog step 2b: when the lit render volume march takes the hybrid froxel lights.
enum class AirMistFroxelLights : std::uint32_t
{
    // On while at least EditorMistPass::FroxelLightsOnCount table Lights reach the lit fog (counted on the GPU, read back a few frames
    // later), off again once at most FroxelLightsOffCount do; off until the first count arrives.
    Automatic = 0,
    // Every Light per sample (the exact path).
    Off = 1,
    // Always the hybrid lights.
    On = 2
};

// What the mist view draws this frame.
struct AirMistRenderRequest
{
    // Multiplies the mist's extinction (0 draws nothing visible).
    float opacity = 1.0f;
    AirMistQuality quality = AirMistQuality::Medium;
    // AIR.4E.5c.2: the reduced target is the preset's scale of this basis. The Renderer fills internalWidth and internalHeight with
    // the frame's render extent; 0 (or an extent larger than the output) falls back to the output.
    AirMistResolutionBasis resolutionBasis = AirMistResolutionBasis::Output;
    std::uint32_t internalWidth = 0;
    std::uint32_t internalHeight = 0;
    // AIR.4F F4.1: flow-advected Worley detail (EditorMist.hlsl). Strength 0 to 1 blends the density from unmodulated to fully
    // modulated; the scale is the noise tile's size in metres. Without the noise texture, detail is skipped.
    bool detail = true;
    float detailStrength = 0.5f;
    float detailScale = 4.0f;
    // AIR.4F F4.2: each ray's stable per-pixel start offset (off: every ray starts at its span's entry, which bands), and the
    // composite's 4 x 4 tent upsample (off: F3's 2 x 2 bilinear). Diagnostics; the Editor always uses both.
    bool jitter = true;
    // Raymarch dither experiment (diagnostic, temporary): 0 the production pattern, 1 Bayer 4 x 4 alone, 2 the R2 dither.
    std::uint32_t ditherMode = 0;
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
    // AIR.4F.2d.2 (lit only): the scattering tint multiplies the albedo (each channel 0 to 1; white: no change) and multipleScattering
    // (0 to 1; 0: single scattering as before) turns the sun's phase isotropic with the optical depth crossed (EditorMist.hlsl).
    std::array<float, 3> scatteringTint{1.0f, 1.0f, 1.0f};
    float multipleScattering = 0.0f;
    // AIR.4F.2d.3e (lit with sun shadows only): indirectScattering (0 to 1; 0, the default: none, and no gather runs) gives the shadowed
    // share of the mist the sunlight that sunlit mist around it scattered first (EditorMist.hlsl, CSIndirectFill). indirectReach,
    // internal (not authored): how far the gather's rays reach, in metres (clamped to 1 to 32 cells).
    float indirectScattering = 0.0f;
    float indirectReach = 8.0f;
    // AIR.4F.2d.3. Whether the simulated mist is drawn (the Editor's Show mist; the pass may still draw haze without it). The scene's
    // environmental haze (EnvironmentalHazeSettings): a uniform medium of extinction 1 / hazeAttenuationDistance over the whole ray, in
    // hazeColor. AIR.4F.2d.3b: two near clearances, each a smoothstep weight on its own medium's extinction from its distance (0: off)
    // over its transition: the haze's (EnvironmentalHazeSettings) and the AIR mist's (MistRenderSettings). And the haze's optional
    // world-space height bounds (full density from hazeBaseHeight to hazeMaxHeight, fading over hazeHeightTransition outside).
    bool mistVisible = true;
    bool haze = false;
    float hazeAttenuationDistance = 100.0f;
    std::array<float, 3> hazeColor{0.9f, 0.9f, 0.9f};
    float hazeNearDistance = 0.0f;
    float hazeNearTransition = 5.0f;
    float mistNearDistance = 0.0f;
    float mistNearTransition = 5.0f;
    bool hazeHeightBounds = false;
    float hazeBaseHeight = -10.0f;
    float hazeMaxHeight = 15.0f;
    float hazeHeightTransition = 3.0f;
    // AIR.4F.2d.3a: the fraction of the environment's mean radiance that lights the haze (lit fog only; 0: the sun and Lights only).
    // Independent of `ambient`, which lights the AIR mist alone.
    float hazeAmbient = 0.0f;
    // AIR.4F.2d.3d, internal (not authored): sun visibility rays per fluid piece (1, 2, 4 or 8): the fraction reaching the sun shades the
    // piece. 1 is the single ray from the piece's anchor used before 2d.3d.
    std::uint32_t sunVisibilitySamples = 4;
    // AIR.4F.2d.3, internal (not authored): the raymarch stops marching mist once less than this transmittance is left.
    float terminationTransmittance = 0.01f;
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
    // V1a diagnostics (the Editor never sets them; AirVolumetricV1aReport.md): with froxelSunVisibility, a lit and shadowed mist takes the
    // sun's visibility from a camera-aligned froxel volume (one ray per froxel) instead of the per-piece cell visibility. The volume is
    // the reduced target's size / froxelDivisor in x and y (rounded up) by froxelSlices geometric depth slices from froxelNear metres to
    // the window's farthest corner; froxelJitter places each froxel's sample by a hash of its index and froxelSeed (off: its centre).
    bool froxelSunVisibility = false;
    std::uint32_t froxelDivisor = 8;
    std::uint32_t froxelSlices = 64;
    float froxelNear = 0.5f;
    bool froxelJitter = true;
    std::uint32_t froxelSeed = 0;
    // V1b-1 diagnostics (the Editor never sets them; AirVolumetricV1b1Report.md). froxelFilter picks the raymarch's lookup;
    // froxelTemporal (which implies the hardware filter) accumulates the volume over frames: each froxel blends this frame's trace into
    // the previous frame's resolved volume, reprojected at its centre, with current-frame weight froxelBlend (0 to 1). The history is
    // reset on the request's froxelHistoryReset, a camera move longer than froxelCutDistance metres in one frame, a sun direction
    // change, a new volume size or mapping, a changed jitter configuration, or a frame without the path. froxelSequence moves the
    // jittered sample each frame (it needs froxelJitter).
    AirFroxelFilter froxelFilter = AirFroxelFilter::Manual;
    bool froxelTemporal = false;
    AirFroxelJitterSequence froxelSequence = AirFroxelJitterSequence::Static;
    float froxelBlend = 0.05f;
    float froxelCutDistance = 4.0f;
    bool froxelHistoryReset = false;
    // V1b-2 diagnostics: the history's rectification, and the clipping box's half-width in standard deviations (gamma).
    AirFroxelHistoryClip froxelHistoryClip = AirFroxelHistoryClip::None;
    float froxelClipGamma = 1.0f;
    // V1b-2: widen the clipping box to contain the froxel's own current value (off: the thesis's box exactly).
    bool froxelClipIncludesCurrent = false;
    // V1b-2: anti-flicker with clipping (Lerviks 2026): the blend scales from froxelBlendMinimum (history just clipped) to froxelBlend
    // over a clip distance of 0 to froxelFlickerDistance. The thesis's values: 0.02, 0.05 and 0.1.
    bool froxelAntiFlicker = false;
    float froxelBlendMinimum = 0.02f;
    float froxelFlickerDistance = 0.1f;
    // V1b-3 diagnostics (AirVolumetricV1b3Report.md). froxelDecorrelateJitter rotates the shared Halton offset by a fixed per-froxel hash
    // (the jittered axes only). The resolved volumes then carry a second moment (twice the texture memory) whenever the history-aware
    // box, the adaptive blend or gated clipping is on: z = |current - history| / (temporal deviation + 0.02); the adaptive blend rises
    // from froxelBlend toward froxelAdaptiveBlendMax as z goes from froxelAdaptiveLow to froxelAdaptiveHigh, and gated clipping clips only
    // where z exceeds froxelAdaptiveLow.
    bool froxelDecorrelateJitter = false;
    float froxelTemporalGamma = 1.0f;
    bool froxelAdaptiveBlend = false;
    float froxelAdaptiveLow = 1.5f;
    float froxelAdaptiveHigh = 4.0f;
    float froxelAdaptiveBlendMax = 0.5f;
    bool froxelClipGated = false;
    // V1c-2 diagnostics (AirVolumetricV1c2Report.md): froxelIndirect (on the texture path) resamples CSIndirectFill's per-cell fill into
    // a froxel texture each frame and draws with both lighting terms from froxels; it always uses the atmosphere march.
    // forceAtmosphereMarch runs every other path through the atmosphere march too, for like-for-like comparisons.
    bool froxelIndirect = false;
    bool forceAtmosphereMarch = false;
    // V1c-2b diagnostics (AirVolumetricV1c2bReport.md): CSFroxelIndirect's fill-only reconstruction (identical output, fewer loads) and
    // its skipping of froxels whose brick holds no mist (exactly 0 there).
    bool froxelIndirectFillOnly = false;
    bool froxelIndirectSkipEmpty = false;
    // V1c-4 diagnostics (AirVolumetricV1c4Report.md): on the lit path (froxelIndirect), the table Lights' in-scattering from a froxel
    // volume (per froxel along its camera ray, every frame) in place of the per-sample loop at the mist and haze samples.
    bool froxelLocalLights = false;
    // V1d (AirVolumetricV1dReport.md): on the lit path (froxelIndirect), the integrated froxel renderer: the medium injected and integrated
    // per froxel column, each reduced pixel's result looked up at its scene distance in place of the march. froxelNearLightRadius (metres;
    // 0: none) hands each point or spot Light's in-scattering within 2 x radius of it to the lookup, integrated along each pixel's ray.
    bool froxelIntegrated = false;
    float froxelNearLightRadius = 1.5f;
    // V1d aliasing investigation (diagnostic only; AirVolumetricV1dAliasingInvestigation.md): sub-samples per slice in the integration
    // (1: each froxel's own visibility, medium and fill; more: trilinear samples at that many depths within each slice).
    std::uint32_t froxelIntegrateSamples = 1;
    // V1d aliasing investigation (diagnostic only): the integration's sun visibility from the cells (as the production march) instead of
    // the froxel visibility volume.
    bool froxelIntegrateCellVisibility = false;
    // V1d correction (AirVolumetricV1dAliasingCorrection.md): the integration's sun visibility from the cells, averaged by the medium
    // pass over froxelMediumSamples AIR samples per froxel (each with its own density, visibility and fill), in place of the froxel
    // visibility volume. froxelCellVisibilityVolume takes precedence over froxelIntegrateCellVisibility.
    bool froxelCellVisibilityVolume = false;
    std::uint32_t froxelMediumSamples = 1;
    // V1d distant banding: froxelMediumSamples is then the minimum, and each slice takes one AIR sample per cell of its depth (at most 16).
    bool froxelMediumAdaptive = false;
    // V1d lateral experiment (diagnostic, temporary): each depth sample on 4 distinct camera rays across the froxel's XY footprint.
    bool froxelMediumLateral = false;
    // Experimental filter (off by default): a depth-aware 3 x 3 Gaussian over the reduced result before the composite (CSFilterIntegrated),
    // for the integrated renderer or the raymarch alike.
    bool froxelIntegratedFilter = false;
    // V1d filter diagnostic (temporary; EditorMist.hlsl's filterDiagnostic bits): 0 is the filter as the Editor runs it.
    std::uint32_t froxelFilterDiagnostic = 0;
    // Fog step 1 (Volumetric_Fog_Optimization_Report.md, item 1): the Original Raymarch reads open footprints from the render volume, a
    // per-frame fp16 copy of the mist, velocity, sun visibility and indirect fill (CSBuildRenderVolume), instead of
    // AirRenderMistAndVelocity's gathers. Footprints near geometry, split cells or the window's edge are unchanged. Not used with the
    // froxel paths. renderVolumeNearDistance (metres) applies to the Hybrid filter.
    bool renderVolume = true;
    AirMistRenderVolumeFilter renderVolumeFilter = AirMistRenderVolumeFilter::Manual;
    float renderVolumeNearDistance = 4.0f;
    // Step 1b: the fast path also takes footprints with blocked voxels but no split cell (EditorMist.hlsl's label words): the corners
    // in the sample's own piece's component, exactly as AirRenderMistAndVelocity's link search keeps them.
    bool renderVolumeExtended = true;
    // Fog step 2 (Volumetric_Fog_Optimization_Report.md, section 3.4): hybrid froxel lighting for the lit render volume march. The table
    // Lights' in-scattering beyond froxelLightNearRadius metres of each Light (fading in from r to 2r) comes from a froxel volume rebuilt
    // every frame (the reduced target / froxelLightDivisor in x and y, froxelLightSlices geometric slices); within it, per sample, for the
    // Lights a per-froxel mask lists. The haze takes the same split. Needs the render volume and lighting; off: every Light per sample.
    // Step 2b: automatic by default (AirMistFroxelLights).
    AirMistFroxelLights froxelLights = AirMistFroxelLights::Automatic;
    float froxelLightNearRadius = 1.5f;
    std::uint32_t froxelLightDivisor = 8;
    std::uint32_t froxelLightSlices = 64;

    bool operator==(const AirMistRenderRequest&) const = default;
};

// AIR.4F F4.4.1: a scene's mist appearance (MistRenderSettings) and the Editor's quality preset as one frame's request; the
// implementation switches (jitter, upsample) keep their defaults.
AirMistRenderRequest MakeAirMistRenderRequest(const MistRenderSettings& settings, AirMistQuality quality) noexcept;

// R1.2b: what one frame's fog request is built from. The Editor and packaged frames build their requests the same way
// (BuildFogRenderRequest); only where the inputs come from differs.
struct FogRenderInputs
{
    // The scene's mist appearance. The haze's lighting, scattering, tint, phase and multiple scattering come from it too.
    MistRenderSettings appearance;
    // The scene's environmental haze (disabled: none).
    EnvironmentalHazeSettings haze;
    AirMistQuality quality = AirMistQuality::Medium;
    AirMistResolutionBasis resolutionBasis = AirMistResolutionBasis::Internal;
    // Whether the AIR mist is marched (the pass still requires a valid mist field).
    bool mistVisible = false;
};

// R1.2b: the frame's fog request with the production look (the Bayer 4 x 4 dither and the depth-aware Gaussian over the reduced
// result), or nullopt when neither the mist is shown nor the haze is enabled.
std::optional<AirMistRenderRequest> BuildFogRenderRequest(const FogRenderInputs& inputs);

// V1d follow-up (Editor diagnostic, never packaged): turns `request` into the experimental integrated froxel renderer.
void ApplyIntegratedFroxelRenderer(AirMistRenderRequest& request, bool adaptiveSamples, int lateralMode, bool doubleResolution) noexcept;

// R1.2b/R1.5b: what a packaged runtime may draw: the haze and (R1.5b) the AIR mist, both through the Original Raymarch. Every froxel
// and integrated path is switched off, so none of their (unpackaged, experimental) pipelines is ever needed. The mist itself is left
// as requested; the Renderer draws it only when packaged AIR was initialized.
void RestrictToPackagedVolumetrics(AirMistRenderRequest& request) noexcept;

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

namespace DirectX
{
struct TexMetadata;
class ScratchImage;
}

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
    // R1.2b: Initialize creates the haze core only. An empty path loads nothing (a packaged runtime passes none).
    bool Initialize(ID3D12Device* device, DescriptorHeap& descriptorHeap, ShaderLibrary& shaderLibrary, ResourceUploader& uploader,
        const std::filesystem::path& noisePath, const std::filesystem::path& curlPath = {}, const std::filesystem::path& filamentPath = {});
    // R1.2b: the AIR mist's own stages (occupancy, the mist march, sun visibility, indirect fill). The Editor creates them at
    // initialization; otherwise the first frame with marchable mist does. A failure is not retried: the mist is then never marched.
    bool EnsureMistPipelines();
    // R1.5b: a packaged game's three noise volumes from their archive bytes (PackagedBootstrap::MistNoise), after Initialize and while
    // no noise is loaded. Each is validated exactly as the Editor's loose file is; one that fails is skipped with a warning (its detail
    // is then off). Returns how many loaded (3 when all did).
    int LoadPackagedNoise(ResourceUploader& uploader, std::span<const std::byte> worley, std::span<const std::byte> curl,
        std::span<const std::byte> filaments);
    bool HasMistPipelines() const noexcept
    {
        return m_occupancyPipeline.GetRootSignature() != nullptr;
    }
    // After the Renderer's drain.
    void Shutdown();

    // Draws the mist into `viewportColor` (the final scene image) against `viewportDepth`, both full size. Returns whether anything
    // was recorded. `timestamps`, when given, receives five timestamps from `firstQuery`: 0 start, 4 after occupancy (and, lit and
    // shadowed, the sun's visibility), 1 after the indirect fill (AIR.4F.2d.3e), 2 after the raymarch, 3 after the composite.
    // `mistSeconds` is the mist's transport time (AirField::GetMistSeconds), which drives the detail's phases.
    // AIR.4F F4.3: a lit request needs `lighting` (with a light table) and an R16G16B16A16_FLOAT HDR `viewportColor`; an unlit one draws
    // into the R8G8B8A8_UNORM final image. Either way the other combination draws nothing.
    bool Execute(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, UploadBuffer& upload, RenderTarget& viewportColor,
        DepthBuffer& viewportDepth, const Matrix4& viewProjection, const Vector3& cameraPosition, const AirFieldBinding& field,
        const AirMistBinding& mist, const AirMistRenderRequest& request, double mistSeconds, std::uint64_t lastSubmissionFence,
        std::uint64_t completedFence, ID3D12QueryHeap* timestamps = nullptr, UINT firstQuery = 0,
        const AirMistLighting* lighting = nullptr);

    // AIR.4E.5a diagnostics: a timestamp heap that Execute stamps (5 queries from `firstQuery`, as its own parameters) when its caller
    // passes none, so a profiler can time the pass inside real frames; null (no timestamps) in production.
    void SetDiagnosticTimestamps(ID3D12QueryHeap* timestamps, UINT firstQuery) noexcept
    {
        m_diagnosticTimestamps = timestamps;
        m_diagnosticFirstQuery = firstQuery;
    }

    // V1a diagnostics: turns the request's froxelSunVisibility on for every execution (for profilers driving real frames, whose requests
    // the Editor builds); off in production. With the froxel volume, Execute stamps a sixth query (index 5, after the froxel pass).
    void SetDiagnosticFroxelSunVisibility(bool enabled) noexcept
    {
        m_diagnosticFroxels = enabled;
    }

    // V1b-1 diagnostics: with the override above on, also replaces the request's froxelFilter, froxelTemporal and froxelSequence (the
    // Editor's requests never set them). With the froxel texture, Execute stamps a seventh query (index 6, between the trace and
    // CSFroxelResolve), so the order is 0, 4, 6, 5, 1, 2, 3.
    void SetDiagnosticFroxelMode(AirFroxelFilter filter, bool temporal, AirFroxelJitterSequence sequence,
        AirFroxelHistoryClip clip = AirFroxelHistoryClip::None, bool decorrelate = false, bool indirect = false, bool localLights = false,
        bool integrated = false, float nearLightRadius = 1.5f) noexcept
    {
        m_diagnosticFroxelIntegrated = integrated;
        m_diagnosticNearLightRadius = nearLightRadius;
        m_diagnosticFroxelLocalLights = localLights;
        m_diagnosticFroxelIndirect = indirect;
        m_diagnosticFroxelFilter = filter;
        m_diagnosticFroxelTemporal = temporal;
        m_diagnosticFroxelSequence = sequence;
        m_diagnosticFroxelClip = clip;
        m_diagnosticFroxelDecorrelate = decorrelate;
    }

    // V1c-2b diagnostics: with the override above on, the froxel indirect resample's options; and, for every execution (froxels or not),
    // the atmosphere march in place of the plain one, so a profiler compares lighting sources on the same kernels.
    void SetDiagnosticFroxelIndirectOptions(bool fillOnly, bool skipEmpty) noexcept
    {
        m_diagnosticFroxelIndirectFillOnly = fillOnly;
        m_diagnosticFroxelIndirectSkipEmpty = skipEmpty;
    }

    void SetDiagnosticForceAtmosphereMarch(bool enabled) noexcept
    {
        m_diagnosticForceAtmosphere = enabled;
    }

    // V1c-3 diagnostics (AirVolumetricV1c3Report.md): the cell sun visibility's rays per piece for every execution (1, 2, 4 or 8; 0: the
    // request's, 4 in production), so a profiler can time CSSunVisibility's ray count inside real frames.
    void SetDiagnosticSunVisibilitySamples(std::uint32_t samples) noexcept
    {
        m_diagnosticSunSamples = samples;
    }

    // Fog step 1 diagnostics: override the request's render volume for every execution (enabled -1: the request's; 0 off; 1 on), its
    // filter (-1: the request's) and its near distance (negative: the request's). ENGINE_MIST_RENDER_VOLUME, _FILTER and _NEAR set the
    // same overrides at startup; these setters take precedence.
    void SetDiagnosticRenderVolume(int enabled, int filter = -1, float nearDistance = -1.0f) noexcept
    {
        m_diagnosticRenderVolume = enabled;
        m_diagnosticRenderVolumeFilter = filter;
        m_diagnosticRenderVolumeNear = nearDistance;
    }

    // Fog step 2 diagnostics: override the request's froxelLights for every execution (-1: the request's; 0 off; 1 on; 2 automatic) and
    // its near radius (negative: the request's). ENGINE_MIST_FROXEL_LIGHTS (0 off, 1 on; unset: the request's) and
    // ENGINE_MIST_FROXEL_LIGHTS_NEAR set the same at startup.
    void SetDiagnosticFroxelLights(int enabled, float nearRadius = -1.0f) noexcept
    {
        m_diagnosticFroxelLights = enabled;
        m_diagnosticFroxelLightNear = nearRadius;
    }

    // Fog step 2: whether the last execution lit through the hybrid froxel lights, and their two volumes' bytes (0 when none exist).
    bool WasFroxelLightsUsed() const noexcept
    {
        return m_froxelLightsUsed;
    }

    std::uint64_t GetFroxelLightBytes() const noexcept
    {
        return m_hybridLightBytes;
    }

    // Fog step 2b: the automatic choice's thresholds (table Lights reaching the lit fog), and the latest count read back (-1: none yet).
    static constexpr std::int64_t FroxelLightsOnCount = 8;
    static constexpr std::int64_t FroxelLightsOffCount = 6;

    std::int64_t GetFogLightCount() const noexcept
    {
        return m_fogLightCount;
    }

    // Step 1b diagnostics: override the request's renderVolumeExtended for every execution (-1: the request's; 0 off; 1 on).
    void SetDiagnosticRenderVolumeExtended(int extended) noexcept
    {
        m_diagnosticRenderVolumeExtended = extended;
    }

    // Fog step 1 diagnostics: the counting marches (eight counters, EditorMist.hlsl's RenderVolumeCounters), and a
    // timestamp before the build (the build ends at stamp 1), so a profiler can time it apart from the indirect fill.
    void SetDiagnosticRenderVolumeCount(bool enabled) noexcept
    {
        m_diagnosticRenderVolumeCount = enabled;
    }

    void SetDiagnosticRenderVolumeTimestamp(ID3D12QueryHeap* timestamps, UINT query) noexcept
    {
        m_renderVolumeTimestamps = timestamps;
        m_renderVolumeQuery = query;
    }

    // Fog step 1: whether the last execution marched through the render volume, and its three textures' bytes (0 when none exist).
    bool WasRenderVolumeUsed() const noexcept
    {
        return m_renderVolumeUsed;
    }

    std::uint64_t GetRenderVolumeBytes() const noexcept
    {
        return m_renderVolumeBytes;
    }

    // Fog step 1 diagnostics: the counting marches' counters (empty until they first run), for a profiler to read back.
    Buffer& GetRenderVolumeCounters() noexcept
    {
        return m_renderVolumeCounters;
    }

    // V1a: the last execution's froxel volume (0 x 0 x 0 when it did not use one).
    std::array<std::uint32_t, 3> GetFroxelSize() const noexcept
    {
        return m_froxelSize;
    }

    // V1b-1: the timestamps the last execution ended (5 without froxels, 6 with V1a's buffer lookup, 7 with the froxel texture, V1c-2: 8
    // with the froxel indirect fill, stamp 7 between the cell fill and the resample, V1c-4: 9 with the froxel local lights, stamp 8 between
    // the resample and the Lights). A caller resolves exactly these: resolving a query never ended is a debug-layer error.
    UINT GetLastStampCount() const noexcept
    {
        return m_lastStampCount;
    }

    // V1b-1: whether the last execution blended history into the froxel volume, and how many consecutive executions have accumulated
    // since the last reset (0: the last execution reset or did not accumulate).
    bool WasFroxelHistoryUsed() const noexcept
    {
        return m_froxelHistoryUsed;
    }

    std::uint64_t GetFroxelHistoryLength() const noexcept
    {
        return m_froxelHistoryLength;
    }

    // V1b-1: the reason the last execution did not use history ("" when it did, or it did not run temporally).
    const char* GetFroxelHistoryResetReason() const noexcept
    {
        return m_froxelResetReason;
    }

    // V1b-1: the froxel textures' bytes (both history volumes; 0 before the path first runs).
    std::uint64_t GetFroxelTextureBytes() const noexcept
    {
        return m_froxelTextureBytes;
    }

    // V1d: the integrated renderer's three volumes' bytes (0 unless that path ran).
    std::uint64_t GetFroxelIntegratedBytes() const noexcept
    {
        return m_froxelIntegratedBytes;
    }

    // V1c-4: the froxel local-light volume's bytes (0 unless the lit-local path ran).
    std::uint64_t GetFroxelLocalLightBytes() const noexcept
    {
        return m_froxelLocalLightBytes;
    }

    // V1c-2: the froxel indirect texture's bytes (0 unless the lit path ran).
    std::uint64_t GetFroxelIndirectBytes() const noexcept
    {
        return m_froxelIndirectBytes;
    }

    // A frame without mist: retires the storage (released once `lastSubmissionFence` completes) and collects completed retirements.
    void Idle(std::uint64_t lastSubmissionFence, std::uint64_t completedFence);

    bool HasStorage() const noexcept
    {
        return m_result.GetResource() != nullptr;
    }

    // AIR.4E.5c.2: the reduced target's current extent (0 without storage).
    std::uint32_t GetReducedWidth() const noexcept
    {
        return m_width;
    }

    std::uint32_t GetReducedHeight() const noexcept
    {
        return m_height;
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

    std::uint64_t GetIndirectRayBound() const noexcept
    {
        return m_indirectRayBound;
    }

private:
    friend class AirFieldVerification;

    struct RetiredBuffer
    {
        Buffer buffer;
        std::uint64_t fence = 0;
    };

    // V1b-1: a froxel visibility volume (3D R16_UNORM, SRV and UAV in the bindless heap) and the state the pass last left it in.
    struct FroxelTexture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        unsigned int srv = UINT32_MAX;
        unsigned int uav = UINT32_MAX;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    };

    struct RetiredTexture
    {
        FroxelTexture texture;
        std::uint64_t fence = 0;
    };

    // V1b-1: what the previous froxel execution resolved, as the next one needs it to reproject (and to decide whether it may).
    struct FroxelHistory
    {
        bool valid = false;
        std::uint64_t execution = 0;
        Matrix4 worldToClip;
        Vector3 cameraPosition;
        Vector3 sunDirection;
        std::array<std::uint32_t, 3> size{0, 0, 0};
        float nearDistance = 0.0f;
        float slicesPerLog = 0.0f;
        bool jitter = false;
        AirFroxelJitterSequence sequence = AirFroxelJitterSequence::Static;
        std::uint32_t seed = 0;
        bool decorrelate = false;
    };

    std::array<Buffer*, 4> StorageBuffers() noexcept
    {
        return {&m_result, &m_distance, &m_occupancy, &m_visibility};
    }

    bool EnsureStorage(std::uint32_t width, std::uint32_t height, std::uint32_t bricks, std::uint64_t lastSubmissionFence);
    // V1d experimental filter: m_filteredResult for `bytes`; a larger size retires the old buffer after the last submission.
    bool EnsureFilteredResult(std::size_t bytes, std::uint64_t lastSubmissionFence);
    // The experimental filter's pipeline, created on first use by either path.
    bool EnsureFilterPipeline();
    // V1a: the froxel volume for `froxels` froxels, allocated only when the diagnostic path runs (retired with the other storage).
    bool EnsureFroxelStorage(std::size_t froxels, std::uint64_t lastSubmissionFence);
    // V1b-1: the two froxel textures at exactly `size`, allocated only when the texture path runs. A new size retires the old pair
    // (and so the history); false, with nothing allocated, on failure.
    bool EnsureFroxelTextures(const std::array<std::uint32_t, 3>& size, DXGI_FORMAT format, std::uint64_t lastSubmissionFence);
    bool CreateFroxelTexture(const std::array<std::uint32_t, 3>& size, DXGI_FORMAT format, FroxelTexture& texture);
    void ReleaseFroxelTexture(FroxelTexture& texture);
    void RetireFroxelTextures(std::uint64_t fence);
    static void TransitionFroxelTexture(ID3D12GraphicsCommandList* commandList, FroxelTexture& texture, D3D12_RESOURCE_STATES state);
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

    // R1.5b: where a noise volume's DDS comes from: a loose file (the Editor) or archive bytes (a packaged game). `label` names it in
    // warnings. Decoding is the only step that differs; validation and upload are shared.
    struct NoiseSource
    {
        std::filesystem::path path;
        std::span<const std::byte> bytes;
        std::string label;

        static NoiseSource FromFile(const std::filesystem::path& file)
        {
            return {file, {}, file.string()};
        }
    };

    static bool DecodeNoise(const NoiseSource& source, DirectX::TexMetadata& metadata, DirectX::ScratchImage& image);
    // Loads a power-of-two R8_UNORM 3D cube (`what` names it in warnings); false, with a warning, otherwise.
    bool LoadScalarVolume(ResourceUploader& uploader, const NoiseSource& source, const char* what, ScalarVolume& volume);
    bool LoadNoise(ResourceUploader& uploader, const NoiseSource& source);
    void ReleaseNoise();
    // AIR.4F F4.4.2: the filament mask (MistFilaments128.dds).
    bool LoadFilaments(ResourceUploader& uploader, const NoiseSource& source);
    void ReleaseFilaments();
    // AIR.4F F4.4: loads the curl texture and its SRV, and 1 / its RMS vector length; false (with a warning, and no texture) otherwise.
    bool LoadCurl(ResourceUploader& uploader, const NoiseSource& source);
    void ReleaseCurl();
    void RetireStorage(std::uint64_t fence);
    void CollectRetired(std::uint64_t completedFence);

    ID3D12Device* m_device = nullptr;
    DescriptorHeap* m_descriptorHeap = nullptr;
    ComputePipeline m_occupancyPipeline;
    ComputePipeline m_raymarchPipeline;
    // AIR.4F.2d.3: CSRaymarchAtmosphere, the march with haze and the near clearance.
    ComputePipeline m_atmospherePipeline;
    // AIR.4F.2d.3b: CSRaymarchAtmosphereBounded, for haze with height bounds.
    ComputePipeline m_boundedAtmospherePipeline;
    Pipeline m_compositePipeline;
    // AIR.4F F4.3: the sun visibility pass, and the composite into the HDR scene colour for lit mist.
    ComputePipeline m_visibilityPipeline;
    // AIR.4F.2d.3e: the indirect fill gather (CSIndirectFill), between the sun visibility and the raymarch.
    ComputePipeline m_indirectPipeline;
    Pipeline m_hdrCompositePipeline;
    // V1a diagnostics: the froxel sun visibility pass and the three marches that read it (EditorMist.hlsl, *Froxel).
    ComputePipeline m_froxelPipeline;
    ComputePipeline m_raymarchFroxelPipeline;
    ComputePipeline m_atmosphereFroxelPipeline;
    ComputePipeline m_boundedAtmosphereFroxelPipeline;
    // Built on the diagnostic path's first use, so production frames never compile them.
    ShaderLibrary* m_shaderLibrary = nullptr;
    bool m_froxelPipelinesFailed = false;
    bool EnsureFroxelPipelines();
    // V1b-1 diagnostics: CSFroxelResolve and the three marches reading the froxel texture (EditorMist.hlsl, *FroxelFiltered), built on
    // the texture path's first use; a failure leaves V1a's path working.
    ComputePipeline m_froxelResolvePipeline;
    ComputePipeline m_raymarchFilteredPipeline;
    ComputePipeline m_atmosphereFilteredPipeline;
    ComputePipeline m_boundedAtmosphereFilteredPipeline;
    bool m_froxelTexturePipelinesFailed = false;
    bool EnsureFroxelTexturePipelines();
    // V1c-2 diagnostics: CSFroxelIndirect and the two lit atmosphere marches, built on the lit path's first use.
    ComputePipeline m_froxelIndirectPipeline;
    ComputePipeline m_atmosphereLitPipeline;
    ComputePipeline m_boundedAtmosphereLitPipeline;
    bool m_froxelIndirectPipelinesFailed = false;
    bool EnsureFroxelIndirectPipelines();
    // V1c-2: the froxel indirect fill, an R16_UNORM 3D texture at the froxel size, rewritten every frame (no history); allocated on the
    // lit path's first use and retired with the history pair.
    bool EnsureFroxelIndirectTexture(const std::array<std::uint32_t, 3>& size);
    FroxelTexture m_froxelIndirect;
    std::uint64_t m_froxelIndirectBytes = 0;
    bool m_diagnosticFroxelIndirect = false;
    // V1c-4 diagnostics: CSFroxelLocalLights and the two lit-local marches, built on that path's first use, and its RGBA16F volume (rebuilt
    // every frame, no history; retired with the history pair).
    ComputePipeline m_froxelLocalLightPipeline;
    ComputePipeline m_atmosphereLitLocalPipeline;
    ComputePipeline m_boundedAtmosphereLitLocalPipeline;
    bool m_froxelLocalLightPipelinesFailed = false;
    bool EnsureFroxelLocalLightPipelines();
    bool EnsureFroxelLocalLightTexture(const std::array<std::uint32_t, 3>& size);
    FroxelTexture m_froxelLocalLights;
    std::uint64_t m_froxelLocalLightBytes = 0;
    bool m_diagnosticFroxelLocalLights = false;
    // V1d: the integrated renderer's passes (built on its first use) and volumes (medium, integrated light, segments; RGBA16F at the
    // froxel size, rebuilt every frame, retired with the history pair).
    ComputePipeline m_froxelMediumPipeline;
    ComputePipeline m_froxelIntegratePipeline;
    ComputePipeline m_boundedFroxelIntegratePipeline;
    ComputePipeline m_integratedPipeline;
    ComputePipeline m_boundedIntegratedPipeline;
    // V1d experimental filter.
    ComputePipeline m_integratedFilterPipeline;
    bool m_filterPipelineFailed = false;
    bool m_mistPipelinesFailed = false;
    bool m_froxelIntegratedPipelinesFailed = false;
    bool EnsureFroxelIntegratedPipelines();
    bool EnsureFroxelIntegratedTextures(const std::array<std::uint32_t, 3>& size);
    // Medium, integrated light, segments (RGBA16F) and, for the corrected visibility, the cell visibility (R16_UNORM).
    std::array<FroxelTexture, 4> m_froxelIntegratedTextures;
    std::uint64_t m_froxelIntegratedBytes = 0;
    bool m_diagnosticFroxelIntegrated = false;
    float m_diagnosticNearLightRadius = 1.5f;
    bool m_diagnosticFroxelIndirectFillOnly = false;
    bool m_diagnosticFroxelIndirectSkipEmpty = false;
    bool m_diagnosticForceAtmosphere = false;
    std::uint32_t m_diagnosticSunSamples = 0;
    // Fog step 1: the render volume (EditorMist.hlsl): CSBuildRenderVolume and the three *Volume marches, built on first use (a failure
    // is not retried; the march then reads AirRenderMistAndVelocity as before), the counting marches (diagnostic, likewise), and the
    // mist (RGBA16F), velocity and sun visibility (RGBA16F) and flag and indirect fill (R16G16F) textures at the window's size. The
    // textures are retired with the other storage, or when a frame does not use them.
    bool EnsureRenderVolumePipelines(bool counting);
    bool EnsureRenderVolumeTextures(const std::array<std::uint32_t, 3>& size, std::uint64_t lastSubmissionFence);
    void RetireRenderVolume(std::uint64_t fence);
    ComputePipeline m_renderVolumeBuildPipeline;
    ComputePipeline m_raymarchVolumePipeline;
    ComputePipeline m_atmosphereVolumePipeline;
    ComputePipeline m_boundedAtmosphereVolumePipeline;
    ComputePipeline m_raymarchVolumeCountPipeline;
    ComputePipeline m_atmosphereVolumeCountPipeline;
    ComputePipeline m_boundedAtmosphereVolumeCountPipeline;
    bool m_renderVolumePipelinesFailed = false;
    bool m_renderVolumeCountPipelinesFailed = false;
    std::array<FroxelTexture, 3> m_renderVolume;
    std::array<std::uint32_t, 3> m_renderVolumeSize{0, 0, 0};
    std::uint64_t m_renderVolumeBytes = 0;
    bool m_renderVolumeUsed = false;
    // Eight uints (EditorMist.hlsl's RenderVolumeCounters), created on the counting marches' first use.
    Buffer m_renderVolumeCounters;
    int m_diagnosticRenderVolume = -1;
    int m_diagnosticRenderVolumeFilter = -1;
    float m_diagnosticRenderVolumeNear = -1.0f;
    bool m_diagnosticRenderVolumeCount = false;
    int m_diagnosticRenderVolumeExtended = -1;
    // Fog step 2: CSFroxelHybridLights and the three *VolumeLights marches (built on first use; a failure is not retried and the march
    // lights every sample as before), and the far-light (RGBA16F) and near-mask (R32G32_UINT) volumes at the froxel size, retired with the
    // other storage or when a frame does not use them.
    bool EnsureHybridLightPipelines();
    bool EnsureHybridLightTextures(const std::array<std::uint32_t, 3>& size);
    void RetireHybridLights(std::uint64_t fence);
    ComputePipeline m_froxelHybridLightsPipeline;
    ComputePipeline m_raymarchVolumeLightsPipeline;
    ComputePipeline m_atmosphereVolumeLightsPipeline;
    ComputePipeline m_boundedAtmosphereVolumeLightsPipeline;
    bool m_hybridLightPipelinesFailed = false;
    std::array<FroxelTexture, 2> m_hybridLights;
    std::array<std::uint32_t, 3> m_hybridLightSize{0, 0, 0};
    std::uint64_t m_hybridLightBytes = 0;
    bool m_froxelLightsUsed = false;
    // Step 2b: the automatic choice. CSCountFogLights writes m_fogLightCountBuffer; each counting frame copies it into one of
    // FogLightCountSlots readback slots, read once the GPU passes the fence after the frame's last submission.
    static constexpr std::size_t FogLightCountSlots = 4;
    struct FogLightCountSlot
    {
        std::uint64_t fence = 0;
        bool pending = false;
    };
    bool EnsureFogLightCount();
    void CollectFogLightCounts(std::uint64_t completedFence);
    ComputePipeline m_fogLightCountPipeline;
    bool m_fogLightCountFailed = false;
    Buffer m_fogLightCountBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_fogLightCountReadback;
    std::array<FogLightCountSlot, FogLightCountSlots> m_fogLightCountSlots;
    std::size_t m_fogLightCountNext = 0;
    std::size_t m_fogLightCountRead = 0;
    std::int64_t m_fogLightCount = -1;
    bool m_automaticFroxelLights = false;
    // The frame's last submission fence, for EnsureHybridLightTextures' retirement of a resized pair.
    std::uint64_t m_lastSubmissionFenceForHybrid = 0;
    int m_diagnosticFroxelLights = -1;
    float m_diagnosticFroxelLightNear = -1.0f;
    ID3D12QueryHeap* m_renderVolumeTimestamps = nullptr;
    UINT m_renderVolumeQuery = 0;
    // One float per froxel (x fastest, then y, then the depth slice); empty until the diagnostic path first runs.
    Buffer m_froxels;
    std::size_t m_froxelCapacity = 0;
    std::array<std::uint32_t, 3> m_froxelSize{0, 0, 0};
    bool m_diagnosticFroxels = false;
    AirFroxelFilter m_diagnosticFroxelFilter = AirFroxelFilter::Manual;
    bool m_diagnosticFroxelTemporal = false;
    AirFroxelJitterSequence m_diagnosticFroxelSequence = AirFroxelJitterSequence::Static;
    AirFroxelHistoryClip m_diagnosticFroxelClip = AirFroxelHistoryClip::None;
    bool m_diagnosticFroxelDecorrelate = false;
    // V1b-1: the resolved volumes, ping-ponged: each execution reads m_froxelTextures[m_froxelLatest] as history and writes the other,
    // which becomes the latest. Empty until the texture path first runs; retired with the other storage.
    std::array<FroxelTexture, 2> m_froxelTextures;
    std::array<std::uint32_t, 3> m_froxelTextureSize{0, 0, 0};
    // V1b-3: R16_UNORM, or R16G16_UNORM with the second-moment channel.
    DXGI_FORMAT m_froxelTextureFormat = DXGI_FORMAT_R16_UNORM;
    std::size_t m_froxelLatest = 0;
    std::uint64_t m_froxelTextureBytes = 0;
    std::deque<RetiredTexture> m_retiredTextures;
    FroxelHistory m_froxelHistory;
    // Frames since the last history reset: the jitter sequence's index and the accumulated length.
    std::uint64_t m_froxelSequenceIndex = 0;
    std::uint64_t m_froxelHistoryLength = 0;
    bool m_froxelHistoryUsed = false;
    const char* m_froxelResetReason = "";
    UINT m_lastStampCount = 0;
    Buffer m_result;
    // V1d experimental filter: the reduced result's filtered copy, allocated only while the filter runs (retired with the storage).
    Buffer m_filteredResult;
    std::size_t m_filteredResultBytes = 0;
    Buffer m_distance;
    Buffer m_occupancy;
    // One uint per window cell slot: AIR.4F.2d.3d's sun coverage, 8 bits per piece (bits 0 to 7 piece 0, 8 to 15 piece 1), and
    // AIR.4F.2d.3e's indirect fill, 8 bits per piece (bits 16 to 23 piece 0, 24 to 31 piece 1; 0 without the gather).
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
    ID3D12QueryHeap* m_diagnosticTimestamps = nullptr;
    UINT m_diagnosticFirstQuery = 0;
    std::uint64_t m_allocations = 0;
    bool m_sunShadowed = false;
    std::uint64_t m_visibilityRayBound = 0;
    // AIR.4F.2d.3e: at most this many gather rays in the last execution (0: the gather did not run).
    std::uint64_t m_indirectRayBound = 0;
};
