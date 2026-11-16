#include "Renderer/Passes/EditorMistPass.h"
#include "Debug/MemoryRegistry.h"

#include "Renderer/DepthBuffer.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/UploadBuffer.h"
#include "Core/Log.h"

#include <DirectXTex.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace
{
// Must match the EditorMistConstants cbuffer of EditorMist.hlsl.
struct EditorMistConstants
{
    Matrix4 clipToWorld;
    float cameraPosition[3] = {0.0f, 0.0f, 0.0f};
    float extinction = 0.0f;
    AirFieldBinding field;
    std::uint32_t mist0Index = 0;
    std::uint32_t mist1Index = 0;
    std::uint32_t occupancyIndex = 0;
    std::uint32_t depthIndex = 0;
    std::uint32_t fullSize[2] = {0, 0};
    std::uint32_t reducedSize[2] = {0, 0};
    std::uint32_t resultIndex = 0;
    std::uint32_t distanceIndex = 0;
    std::uint32_t maxSteps = 0;
    float minimumStep = 0.0f;
    // AIR.4F F4.1 detail: AirMistDetail, then the texture and whether detail is on.
    float phaseOffsetA[3] = {0.0f, 0.0f, 0.0f};
    float detailStrength = 0.0f;
    float phaseOffsetB[3] = {0.0f, 0.0f, 0.0f};
    float inverseDetailScale = 0.0f;
    float phaseTimeA = 0.0f;
    float phaseTimeB = 0.0f;
    float phaseWeightA = 1.0f;
    float noiseMeanInverse = 1.0f;
    std::uint32_t noiseIndex = 0;
    std::uint32_t detailEnabled = 0;
    // AIR.4F F4.2.
    std::uint32_t jitterEnabled = 1;
    std::uint32_t wideUpsample = 1;
    // AIR.4F F4.3.
    std::uint32_t lightingEnabled = 0;
    std::uint32_t visibilityIndex = UINT32_MAX;
    float scattering = 0.0f;
    float anisotropy = 0.0f;
    // AIR.4F.2d.3e: the indirect scattering strength (0: none, and no CSIndirectFill) and the gather's reach in metres, in what were
    // the vectors' padding (the layout is unchanged).
    float sunDirection[3] = {0.0f, -1.0f, 0.0f};
    float indirectScattering = 0.0f;
    float sunRadiance[3] = {0.0f, 0.0f, 0.0f};
    float indirectReach = 0.0f;
    float ambient = 0.0f;
    std::uint32_t irradianceIndex = UINT32_MAX;
    std::uint32_t visibilitySamples = 4;
    float lightingPadding2 = 0.0f;
    // AIR.4F F4.4: AirMistCurl, then the texture and whether curl is on.
    float curlDisplacement = 0.0f;
    float curlInverseScale = 0.0f;
    float curlFlow = 0.0f;
    float curlPadding0 = 0.0f;
    std::uint32_t curlIndex = UINT32_MAX;
    std::uint32_t curlEnabled = 0;
    std::uint32_t curlPadding1[2] = {0, 0};
    // AIR.4F F4.4.2: AirMistWisps, then the texture and whether wisps are on.
    float wispStrength = 0.0f;
    float wispInverseScale = 0.0f;
    float wispMeanInverse = 1.0f;
    float wispSquareMeanInverse = 1.0f;
    std::uint32_t wispIndex = UINT32_MAX;
    std::uint32_t wispEnabled = 0;
    // AIR.4F F4.4.2e: the density-dependent detail's strength (0: off).
    float densityDetailStrength = 0.0f;
    float densityPadding = 0.0f;
    // AIR.4F.2d.2: the scattering tint and multiple-scattering strength (neutral: white and 0).
    float scatteringTint[3] = {1.0f, 1.0f, 1.0f};
    float multipleScattering = 0.0f;
    // AIR.4F.2d.3: whether the AIR mist is marched, the haze, the termination transmittance, the near clearance.
    std::uint32_t mistActive = 1;
    std::uint32_t hazeEnabled = 0;
    float hazeExtinction = 0.0f;
    float opaqueTransmittance = 0.01f;
    float hazeAlbedo[3] = {0.0f, 0.0f, 0.0f};
    float hazeNearDistance = 0.0f;
    float hazeNearTransition = 1.0f;
    float hazeAmbient = 0.0f;
    float mistNearDistance = 0.0f;
    float mistNearTransition = 1.0f;
    std::uint32_t heightEnabled = 0;
    float baseHeight = 0.0f;
    float maxHeight = 0.0f;
    float heightTransition = 0.0f;
    // V1a diagnostics: the froxel sun visibility volume (EditorMist.hlsl, FroxelSliceOf).
    std::uint32_t froxelIndex = UINT32_MAX;
    std::uint32_t froxelSize[3] = {0, 0, 0};
    float froxelNear = 0.0f;
    float froxelSlicesPerLog = 0.0f;
    float froxelLogPerSlice = 0.0f;
    std::uint32_t froxelSeed = 0;
    // EditorMist.hlsl's FroxelJitterCentre, FroxelJitterHash or FroxelJitterOffset.
    std::uint32_t froxelJitter = 0;
    // V1b-1 diagnostics (in what was the froxel block's padding): the froxel texture written or read, the history read, and whether it is.
    std::uint32_t froxelTextureIndex = UINT32_MAX;
    std::uint32_t froxelHistoryIndex = UINT32_MAX;
    std::uint32_t froxelHistoryValid = 0;
    // V1b-1: the previous frame's reprojection (CSFroxelResolve), the blend and the frame's sub-froxel offset.
    Matrix4 previousWorldToClip;
    float previousCameraPosition[3] = {0.0f, 0.0f, 0.0f};
    float froxelBlend = 1.0f;
    float froxelOffset[3] = {0.5f, 0.5f, 0.5f};
    float previousFroxelNear = 0.0f;
    std::uint32_t previousFroxelSize[3] = {0, 0, 0};
    float previousFroxelSlicesPerLog = 0.0f;
    // V1b-2: the resolve's history rectification (EditorMist.hlsl FroxelClipNone or FroxelClipVariance) and its box half-width in
    // standard deviations.
    std::uint32_t froxelClip = 0;
    float froxelClipGamma = 1.0f;
    std::uint32_t froxelClipIncludesCurrent = 0;
    std::uint32_t froxelClipPadding = 0;
    // V1b-2: anti-flicker (EditorMist.hlsl): the blend's minimum (at or above froxelBlend: off) and the clip distance of full blend.
    float froxelBlendMinimum = 1.0f;
    float froxelFlickerDistance = 0.1f;
    std::uint32_t froxelFlickerPadding[2] = {0, 0};
    // V1b-3 diagnostics (EditorMist.hlsl): jitter decorrelation (0 none, 1 depth axis, 2 every axis), the second-moment channel, the
    // history-aware box's width in temporal deviations, and the adaptive blend's switch, disagreement range, maximum and gated clipping.
    std::uint32_t froxelDecorrelate = 0;
    std::uint32_t froxelMoments = 0;
    float froxelTemporalGamma = 1.0f;
    std::uint32_t froxelAdaptiveBlend = 0;
    float froxelAdaptiveLow = 1.5f;
    float froxelAdaptiveHigh = 4.0f;
    float froxelAdaptiveBlendMax = 0.5f;
    std::uint32_t froxelClipGated = 0;
    // V1c-2 diagnostics: the froxel indirect fill texture (its UAV for CSFroxelIndirect, its SRV for the *FroxelLit marches).
    std::uint32_t froxelIndirectIndex = UINT32_MAX;
    // V1c-2b diagnostics: CSFroxelIndirect's fill-only reconstruction and empty-brick skipping (EditorMist.hlsl).
    std::uint32_t froxelIndirectFillOnly = 0;
    std::uint32_t froxelIndirectSkipEmpty = 0;
    // V1c-4 diagnostics: the froxel local-light volume (its UAV for CSFroxelLocalLights, its SRV for the *FroxelLitLocal marches).
    std::uint32_t froxelLocalLightIndex = UINT32_MAX;
    // V1d: the integrated renderer's medium, integrated-light and segment volumes, and the Lights' near-source radius (EditorMist.hlsl).
    std::uint32_t froxelMediumIndex = UINT32_MAX;
    std::uint32_t froxelIntegratedIndex = UINT32_MAX;
    std::uint32_t froxelSegmentIndex = UINT32_MAX;
    float froxelNearLightRadius = 0.0f;
    // V1d aliasing investigation: the integration's sub-samples per slice (1 in production use).
    std::uint32_t froxelIntegrateSamples = 1;
    std::uint32_t froxelIntegrateCellVisibility = 0;
    // V1d correction: the cell-visibility volume and the medium's AIR samples per froxel.
    std::uint32_t froxelCellVisibilityIndex = UINT32_MAX;
    std::uint32_t froxelMediumSamples = 1;
    // V1d experimental filter: CSFilterIntegrated's output.
    std::uint32_t filteredResultIndex = UINT32_MAX;
    // V1d filter diagnostic (temporary; EditorMist.hlsl).
    std::uint32_t filterDiagnostic = 0;
    // V1d distant banding: the medium's samples per slice by its depth in cells (EditorMist.hlsl).
    std::uint32_t froxelMediumAdaptive = 0;
    // V1d lateral experiment (diagnostic, temporary): the medium's 4 lateral rays per depth sample.
    std::uint32_t froxelMediumLateral = 0;
    // Raymarch dither experiment (diagnostic, temporary): PixelOffset's pattern (EditorMist.hlsl).
    std::uint32_t ditherMode = 0;
    std::uint32_t padding[3] = {0, 0, 0};
};

static_assert(sizeof(EditorMistConstants) == 768);
static_assert(offsetof(EditorMistConstants, previousWorldToClip) == 512);

// EditorMist.hlsl's froxelJitter values.
constexpr std::uint32_t FroxelJitterCentre = 0;
constexpr std::uint32_t FroxelJitterHash = 1;
constexpr std::uint32_t FroxelJitterOffset = 2;

// V1b-1: the jitter sequence's length (the thesis's Halton(2, 3, 5) of length 16), and the history's invalidation for a sun that turned
// more than about 0.1 degree since the last frame (the cosine of that angle).
constexpr std::uint64_t FroxelSequenceLength = 16;
constexpr double SunChangeCosine = 0.9999985;

// The radical inverse of `index` in `base`: the Halton sequence's coordinate in [0, 1).
double RadicalInverse(std::uint64_t index, std::uint32_t base)
{
    double result = 0.0;
    double scale = 1.0 / static_cast<double>(base);

    while(index > 0)
    {
        result += static_cast<double>(index % base) * scale;
        index /= base;
        scale /= static_cast<double>(base);
    }

    return result;
}

// A phase cycle's texture offset: three values in [0, 1) from an integer hash of the cycle's index, so each restart shows another part
// of the tile rather than pulsing the same pattern.
void CycleOffset(std::int64_t cycle, float (&offset)[3])
{
    std::uint64_t state = static_cast<std::uint64_t>(cycle) * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;

    for(float& component : offset)
    {
        state ^= state >> 29;
        state *= 0xBF58476D1CE4E5B9ull;
        state ^= state >> 32;
        component = static_cast<float>(state >> 40) / 16777216.0f;
    }
}

constexpr std::uint32_t BrickCells = 8;
// The depth buffer's state while the raymarch and the composite read it.
constexpr D3D12_RESOURCE_STATES DepthReadState = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES ShaderReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

void UnorderedAccessBarrier(ID3D12GraphicsCommandList* commandList)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = nullptr;
    commandList->ResourceBarrier(1, &barrier);
}

// What a compute pipeline binds besides its constants (b0).
struct ComputeBindings
{
    // AIR.4F F4.1: the Worley texture's sampler (s0).
    bool noiseSampler = false;
    // AIR.4F F4.3: the scene's Light table (b1) and acceleration structure (t0).
    bool lightTable = false;
    bool topLevel = false;
    // V1b-1: the froxel volumes' sampler (s1).
    bool froxelSampler = false;
};

bool MakeComputePipeline(ID3D12Device* device, ShaderLibrary& shaderLibrary, const char* entryPoint, ComputePipeline& pipeline,
    ComputeBindings bindings = {})
{
    const Shader* shader = shaderLibrary.Load({"EditorMist.hlsl", entryPoint, ShaderStage::Compute});

    if(!shader)
    {
        return false;
    }

    ComputePipelineDescription description{};
    description.computeShader = shader;
    RootParameterDescription constants{};
    constants.type = RootParameterType::ConstantBuffer;
    constants.shaderRegister = 0;
    constants.visibility = D3D12_SHADER_VISIBILITY_ALL;
    description.rootParameters.push_back(constants);

    if(bindings.lightTable)
    {
        RootParameterDescription table{};
        table.type = RootParameterType::ConstantBuffer;
        table.shaderRegister = 1;
        table.visibility = D3D12_SHADER_VISIBILITY_ALL;
        description.rootParameters.push_back(table);
    }

    if(bindings.topLevel)
    {
        RootParameterDescription topLevel{};
        topLevel.type = RootParameterType::ShaderResource;
        topLevel.shaderRegister = 0;
        topLevel.visibility = D3D12_SHADER_VISIBILITY_ALL;
        description.rootParameters.push_back(topLevel);
    }

    // AIR.4F F4.1: the Worley texture tiles on every axis and is filtered trilinearly.
    if(bindings.noiseSampler)
    {
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        description.staticSamplers.push_back(sampler);
    }

    // V1b-1: trilinear between froxel centres, clamped to the first and last centres on every axis (as V1a's manual lookup clamps).
    if(bindings.froxelSampler)
    {
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 1;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        description.staticSamplers.push_back(sampler);
    }

    return pipeline.Initialize(device, description);
}
}

AirMistRenderRequest MakeAirMistRenderRequest(const MistRenderSettings& settings, AirMistQuality quality) noexcept
{
    AirMistRenderRequest request;
    request.opacity = settings.opacity;
    request.quality = quality;
    request.detail = settings.detail;
    request.detailStrength = settings.detailStrength;
    request.detailScale = settings.detailScale;
    request.curl = settings.curl;
    request.curlStrength = settings.curlStrength;
    request.curlScale = settings.curlScale;
    request.curlFlow = settings.curlFlow;
    request.wisps = settings.wisps;
    request.wispStrength = settings.wispStrength;
    request.wispScale = settings.wispScale;
    request.densityDetail = settings.densityDetail;
    request.densityDetailStrength = settings.densityDetailStrength;
    request.lighting = settings.lighting;
    request.sunShadows = settings.sunShadows;
    request.scattering = settings.scattering;
    request.anisotropy = settings.anisotropy;
    request.ambient = settings.ambient;
    request.scatteringTint = settings.scatteringTint;
    request.multipleScattering = settings.multipleScattering;
    request.indirectScattering = settings.indirectScattering;
    request.mistNearDistance = settings.nearDistance;
    request.mistNearTransition = settings.nearTransition;
    return request;
}

std::optional<AirMistRenderRequest> BuildFogRenderRequest(const FogRenderInputs& inputs)
{
    // AIR.4F.2d.3: the request also carries the haze, so it exists whenever the haze is enabled, with or without AIR.
    if(!inputs.mistVisible && !inputs.haze.enabled)
    {
        return std::nullopt;
    }

    AirMistRenderRequest request = MakeAirMistRenderRequest(inputs.appearance, inputs.quality);
    request.resolutionBasis = inputs.resolutionBasis;
    request.mistVisible = inputs.mistVisible;

    // The chosen look (raymarch noise study and visual review): the pure Bayer 4 x 4 dither (EditorMist.hlsl's ditherMode 1), and the
    // depth-aware 3 x 3 Gaussian over the reduced result with its taps 2 reduced pixels apart (filterDiagnostic's stride), for either
    // renderer.
    request.ditherMode = 1;
    request.froxelIntegratedFilter = true;
    request.froxelFilterDiagnostic = 2;

    const EnvironmentalHazeSettings& haze = inputs.haze;
    request.haze = haze.enabled;
    request.hazeAttenuationDistance = haze.attenuationDistance;
    request.hazeColor = haze.color;
    // AIR.4F.2d.3a: disabled haze lights nothing, so its request keeps the default (0).
    request.hazeAmbient = haze.enabled ? haze.ambient : 0.0f;

    // AIR.4F.2d.3b: the haze's clearance and height bounds (only for enabled haze); the AIR mist's clearance came with its settings.
    if(haze.enabled)
    {
        request.hazeNearDistance = haze.nearDistance;
        request.hazeNearTransition = haze.nearTransition;
        request.hazeHeightBounds = haze.heightBounds;
        request.hazeBaseHeight = haze.baseHeight;
        request.hazeMaxHeight = haze.maxHeight;
        request.hazeHeightTransition = haze.heightTransition;
    }

    return request;
}

// V1d follow-up: the experimental integrated froxel renderer (AirVolumetricV1dReport.md) on the V1b-3 froxel candidate (hardware filter,
// depth jitter decorrelated, widened variance clip) with the froxel indirect fill. V1d correction (AirVolumetricV1dAliasingCorrection.md):
// the sun visibility from the cells, 4 AIR samples per froxel, no froxel history.
void ApplyIntegratedFroxelRenderer(AirMistRenderRequest& request, bool adaptiveSamples, int lateralMode, bool doubleResolution) noexcept
{
    request.froxelSunVisibility = true;
    request.froxelFilter = AirFroxelFilter::Hardware;
    request.froxelTemporal = true;
    request.froxelSequence = AirFroxelJitterSequence::Depth;
    request.froxelHistoryClip = AirFroxelHistoryClip::Variance;
    request.froxelClipIncludesCurrent = true;
    request.froxelDecorrelateJitter = true;
    request.froxelIndirect = true;
    request.froxelIndirectFillOnly = true;
    request.froxelIntegrated = true;
    request.froxelCellVisibilityVolume = true;
    // V1d distant banding (temporary control): the medium's adaptive samples per slice (4 to 16, one per AIR cell of depth).
    request.froxelMediumAdaptive = adaptiveSamples;
    // V1d lateral experiment (temporary controls): 2 x 2 lateral rays (cost parity: 1 depth sample each), and double XY.
    request.froxelMediumLateral = lateralMode != 0;
    request.froxelMediumSamples = lateralMode == 2 ? 1u : 4u;
    request.froxelDivisor = doubleResolution ? 4u : 8u;
}

void RestrictToPackagedVolumetrics(AirMistRenderRequest& request) noexcept
{
    request.forceAtmosphereMarch = false;
    request.froxelSunVisibility = false;
    request.froxelTemporal = false;
    request.froxelIndirect = false;
    request.froxelLocalLights = false;
    request.froxelIntegrated = false;
}

AirMistDetailPhases EditorMistPass::DetailPhasesAt(double mistSeconds) noexcept
{
    AirMistDetailPhases phases;
    const double cycles = (std::max)(0.0, std::isfinite(mistSeconds) ? mistSeconds : 0.0) / DetailPhaseSeconds;
    const double cycleA = std::floor(cycles);
    const double cycleB = std::floor(cycles + 0.5);
    const double phaseA = cycles - cycleA;
    const double phaseB = cycles + 0.5 - cycleB;
    CycleOffset(static_cast<std::int64_t>(cycleA) * 2, phases.offsetA);
    CycleOffset(static_cast<std::int64_t>(cycleB) * 2 + 1, phases.offsetB);
    phases.timeA = static_cast<float>(phaseA * DetailPhaseSeconds);
    phases.timeB = static_cast<float>(phaseB * DetailPhaseSeconds);
    phases.weightA = static_cast<float>(1.0 - std::abs(2.0 * phaseA - 1.0));
    return phases;
}

AirMistQualitySettings AirMistQualityOf(AirMistQuality quality) noexcept
{
    switch(quality)
    {
    case AirMistQuality::Low:
        return {0.5f, 32};
    case AirMistQuality::High:
        return {0.70710678f, 96};
    case AirMistQuality::Medium:
        break;
    }

    return {0.5f, 64};
}

bool EditorMistPass::DecodeNoise(const NoiseSource& source, DirectX::TexMetadata& metadata, DirectX::ScratchImage& image)
{
    if(!source.bytes.empty())
    {
        return SUCCEEDED(DirectX::LoadFromDDSMemory(source.bytes.data(), source.bytes.size(), DirectX::DDS_FLAGS_NONE, &metadata, image));
    }

    return !source.path.empty() && SUCCEEDED(DirectX::LoadFromDDSFile(source.path.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image));
}

bool EditorMistPass::LoadScalarVolume(ResourceUploader& uploader, const NoiseSource& source, const char* what,
    ScalarVolume& volume)
{
    const MemoryCategoryScope memoryScope(MemoryCategory::AirVolumetrics);

    DirectX::TexMetadata metadata{};
    DirectX::ScratchImage image;

    if(!DecodeNoise(source, metadata, image))
    {
        Log::Warning(std::string("Mist ") + what + " texture missing or unreadable (" + source.label + "): mist draws without it.");
        return false;
    }

    const bool cube = metadata.dimension == DirectX::TEX_DIMENSION_TEXTURE3D && metadata.format == DXGI_FORMAT_R8_UNORM &&
        metadata.width == metadata.height && metadata.width == metadata.depth && metadata.width >= 4 && metadata.width <= 256 &&
        (metadata.width & (metadata.width - 1)) == 0;

    if(!cube)
    {
        Log::Warning(std::string("Mist ") + what + " texture is not a power-of-two R8_UNORM 3D cube (" + source.label +
            "): mist draws without it.");
        return false;
    }

    const std::uint32_t size = static_cast<std::uint32_t>(metadata.width);
    const DirectX::Image* top = image.GetImage(0, 0, 0);
    const std::size_t texels = std::size_t{size} * size * size;

    if(top == nullptr || top->rowPitch != size || top->slicePitch != std::size_t{size} * size)
    {
        Log::Warning(std::string("Mist ") + what + " texture has an unexpected layout (" + source.label + "): mist draws without it.");
        return false;
    }

    // 1 / the mean, so noise / mean averages 1 and modulation keeps the mist's average density; likewise for the square (F4.4.2e).
    std::uint64_t sum = 0;
    std::uint64_t squareSum = 0;

    for(std::size_t index = 0; index < texels; ++index)
    {
        const std::uint64_t value = top->pixels[index];
        sum += value;
        squareSum += value * value;
    }

    if(sum == 0)
    {
        Log::Warning(std::string("Mist ") + what + " texture is blank (" + source.label + "): mist draws without it.");
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    description.Width = size;
    description.Height = size;
    description.DepthOrArraySize = static_cast<UINT16>(size);
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R8_UNORM;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    Microsoft::WRL::ComPtr<ID3D12Resource> noise;

    // Created in COMMON: the upload's copy promotes it, and shader reads promote it to a read state afterwards.
    if(FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&noise))) || !uploader.UploadTexture(noise.Get(), top->pixels, texels))
    {
        Log::Warning(std::string("Mist ") + what + " texture could not be uploaded: mist draws without it.");
        return false;
    }

    MemoryRegistry::TrackResource(noise, MemoryCategory::AirVolumetrics);

    const unsigned int descriptor = m_descriptorHeap->Allocate();

    if(descriptor == UINT32_MAX)
    {
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture3D.MipLevels = 1;
    m_device->CreateShaderResourceView(noise.Get(), &view, m_descriptorHeap->GetCPUHandle(descriptor));
    volume.resource = noise;
    volume.descriptor = descriptor;
    volume.size = size;
    volume.meanInverse = static_cast<float>(255.0 * static_cast<double>(texels) / static_cast<double>(sum));
    volume.squareMeanInverse = static_cast<float>(255.0 * 255.0 * static_cast<double>(texels) / static_cast<double>(squareSum));
    return true;
}

bool EditorMistPass::LoadNoise(ResourceUploader& uploader, const NoiseSource& source)
{
    ScalarVolume volume;

    if(!LoadScalarVolume(uploader, source, "detail (Worley)", volume))
    {
        return false;
    }

    m_noise = volume.resource;
    m_noiseDescriptor = volume.descriptor;
    m_noiseSize = volume.size;
    m_noiseMeanInverse = volume.meanInverse;
    return true;
}

int EditorMistPass::LoadPackagedNoise(ResourceUploader& uploader, std::span<const std::byte> worley, std::span<const std::byte> curl,
    std::span<const std::byte> filaments)
{
    // Loading over already loaded volumes would leak their descriptors; a packaged runtime's Initialize loaded none.
    if(m_noise || m_curl || m_filaments.resource)
    {
        Log::Warning("Mist noise is already loaded; the packaged noise volumes were not loaded again.");
        return 0;
    }

    int loaded = 0;
    loaded += LoadNoise(uploader, {{}, worley, "archive MistWorley64.dds"}) ? 1 : 0;
    loaded += LoadCurl(uploader, {{}, curl, "archive MistCurl64.dds"}) ? 1 : 0;
    loaded += LoadFilaments(uploader, {{}, filaments, "archive MistFilaments128.dds"}) ? 1 : 0;
    return loaded;
}

bool EditorMistPass::LoadFilaments(ResourceUploader& uploader, const NoiseSource& source)
{
    return LoadScalarVolume(uploader, source, "wisp (filament)", m_filaments);
}

void EditorMistPass::ReleaseFilaments()
{
    if(m_filaments.descriptor != UINT32_MAX && m_descriptorHeap != nullptr)
    {
        m_descriptorHeap->Free(m_filaments.descriptor);
    }

    m_filaments = ScalarVolume{};
}


void EditorMistPass::ReleaseNoise()
{
    if(m_noiseDescriptor != UINT32_MAX && m_descriptorHeap != nullptr)
    {
        m_descriptorHeap->Free(m_noiseDescriptor);
    }

    m_noiseDescriptor = UINT32_MAX;
    m_noise.Reset();
    m_noiseSize = 0;
    m_noiseMeanInverse = 1.0f;
}

bool EditorMistPass::LoadCurl(ResourceUploader& uploader, const NoiseSource& source)
{
    const MemoryCategoryScope memoryScope(MemoryCategory::AirVolumetrics);

    DirectX::TexMetadata metadata{};
    DirectX::ScratchImage image;

    if(!DecodeNoise(source, metadata, image))
    {
        Log::Warning("Mist curl texture missing or unreadable (" + source.label + "): mist detail is never curl-warped.");
        return false;
    }

    const bool cube = metadata.dimension == DirectX::TEX_DIMENSION_TEXTURE3D && metadata.format == DXGI_FORMAT_R16G16B16A16_SNORM &&
        metadata.width == metadata.height && metadata.width == metadata.depth && metadata.width >= 4 && metadata.width <= 256 &&
        (metadata.width & (metadata.width - 1)) == 0;
    const std::uint32_t size = static_cast<std::uint32_t>(metadata.width);
    const DirectX::Image* top = image.GetImage(0, 0, 0);
    const std::size_t texels = std::size_t{size} * size * size;

    if(!cube || top == nullptr || top->rowPitch != std::size_t{size} * 8 || top->slicePitch != std::size_t{size} * size * 8)
    {
        Log::Warning("Mist curl texture is not a power-of-two R16G16B16A16_SNORM 3D cube (" + source.label + "): mist detail is never curl-warped.");
        return false;
    }

    // 1 / the RMS vector length, so the strength control is the RMS displacement in metres. SNORM decodes v / 32767, clamped to -1.
    double squares = 0.0;

    for(std::size_t index = 0; index < texels; ++index)
    {
        for(std::size_t component = 0; component < 3; ++component)
        {
            std::int16_t encoded = 0;
            std::memcpy(&encoded, top->pixels + index * 8 + component * 2, 2);
            const double value = (std::max)(static_cast<double>(encoded) / 32767.0, -1.0);
            squares += value * value;
        }
    }

    if(!(squares > 0.0))
    {
        Log::Warning("Mist curl texture is blank (" + source.label + "): mist detail is never curl-warped.");
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    description.Width = size;
    description.Height = size;
    description.DepthOrArraySize = static_cast<UINT16>(size);
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R16G16B16A16_SNORM;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    Microsoft::WRL::ComPtr<ID3D12Resource> curl;

    // Created in COMMON: the upload's copy promotes it, and shader reads promote it to a read state afterwards.
    if(FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&curl))) || !uploader.UploadTexture(curl.Get(), top->pixels, texels * 8))
    {
        Log::Warning("Mist curl texture could not be uploaded: mist detail is never curl-warped.");
        return false;
    }

    MemoryRegistry::TrackResource(curl, MemoryCategory::AirVolumetrics);

    const unsigned int descriptor = m_descriptorHeap->Allocate();

    if(descriptor == UINT32_MAX)
    {
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R16G16B16A16_SNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture3D.MipLevels = 1;
    m_device->CreateShaderResourceView(curl.Get(), &view, m_descriptorHeap->GetCPUHandle(descriptor));
    m_curl = curl;
    m_curlDescriptor = descriptor;
    m_curlSize = size;
    m_curlRmsInverse = static_cast<float>(1.0 / std::sqrt(squares / static_cast<double>(texels)));
    return true;
}

void EditorMistPass::ReleaseCurl()
{
    if(m_curlDescriptor != UINT32_MAX && m_descriptorHeap != nullptr)
    {
        m_descriptorHeap->Free(m_curlDescriptor);
    }

    m_curlDescriptor = UINT32_MAX;
    m_curl.Reset();
    m_curlSize = 0;
    m_curlRmsInverse = 1.0f;
}

bool EditorMistPass::Initialize(ID3D12Device* device, DescriptorHeap& descriptorHeap, ShaderLibrary& shaderLibrary, ResourceUploader& uploader,
    const std::filesystem::path& noisePath, const std::filesystem::path& curlPath, const std::filesystem::path& filamentPath)
{
    const MemoryCategoryScope memoryScope(MemoryCategory::AirVolumetrics);

    m_device = device;
    m_descriptorHeap = &descriptorHeap;
    m_shaderLibrary = &shaderLibrary;

    // R1.2b: the haze core only (the atmosphere marches here, the composites below). The AIR mist's own stages are created by
    // EnsureMistPipelines: eagerly by the Editor; in a packaged runtime only when its startup scene requests AIR (R1.5b), whose
    // bootstrap then holds them.
    if(!MakeComputePipeline(device, shaderLibrary, "CSRaymarchAtmosphere", m_atmospherePipeline, {true, true, false}) ||
        !MakeComputePipeline(device, shaderLibrary, "CSRaymarchAtmosphereBounded", m_boundedAtmospherePipeline, {true, true, false}))
    {
        return false;
    }

    // A missing texture only disables detail (or, F4.4, its curl warp). R1.2b: an empty path (a packaged runtime) loads nothing;
    // R1.5b: a packaged runtime with AIR loads the archive's volumes through LoadPackagedNoise.
    if(!noisePath.empty())
    {
        LoadNoise(uploader, NoiseSource::FromFile(noisePath));
    }

    if(!curlPath.empty())
    {
        LoadCurl(uploader, NoiseSource::FromFile(curlPath));
    }

    if(!filamentPath.empty())
    {
        LoadFilaments(uploader, NoiseSource::FromFile(filamentPath));
    }

    const Shader* vertexShader = shaderLibrary.Load({"EditorMist.hlsl", "VSComposite", ShaderStage::Vertex});
    const Shader* pixelShader = shaderLibrary.Load({"EditorMist.hlsl", "PSComposite", ShaderStage::Pixel});

    if(!vertexShader || !pixelShader)
    {
        return false;
    }

    // Straight-alpha "over" blending of (colour / alpha, alpha): scene * transmittance + mist colour. No depth test: the composite
    // reads depth itself.
    PipelineDescription description;
    description.vertexShader = vertexShader;
    description.pixelShader = pixelShader;
    description.renderTargets.count = 1;
    description.renderTargets.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.alphaBlend = true;
    description.rootParameters.push_back({RootParameterType::ConstantBuffer, 0, 0, D3D12_SHADER_VISIBILITY_ALL});

    if(!m_compositePipeline.Initialize(device, description))
    {
        return false;
    }

    // AIR.4F F4.3: the same composite into the HDR scene colour, for lit mist (scene radiance, before tone mapping).
    description.renderTargets.formats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    return m_hdrCompositePipeline.Initialize(device, description);
}

void EditorMistPass::Shutdown()
{
    // Buffer has no destructor: each is shut down explicitly to release its resource and descriptors.
    for(RetiredBuffer& retired : m_retired)
    {
        retired.buffer.Shutdown();
    }

    m_retired.clear();

    for(Buffer* buffer : StorageBuffers())
    {
        buffer->Shutdown();
    }

    m_froxels.Shutdown();
    m_froxelCapacity = 0;
    m_filteredResult.Shutdown();
    m_filteredResultBytes = 0;

    for(RetiredTexture& retired : m_retiredTextures)
    {
        ReleaseFroxelTexture(retired.texture);
    }

    m_retiredTextures.clear();

    for(FroxelTexture& texture : m_froxelTextures)
    {
        ReleaseFroxelTexture(texture);
    }

    ReleaseFroxelTexture(m_froxelIndirect);
    ReleaseFroxelTexture(m_froxelLocalLights);
    m_froxelLocalLightBytes = 0;

    for(FroxelTexture& texture : m_froxelIntegratedTextures)
    {
        ReleaseFroxelTexture(texture);
    }

    m_froxelIntegratedBytes = 0;
    m_froxelTextureSize = {0, 0, 0};
    m_froxelTextureBytes = 0;
    m_froxelIndirectBytes = 0;
    m_froxelHistory = FroxelHistory{};

    m_width = 0;
    m_height = 0;
    m_bricks = 0;
    ReleaseNoise();
    ReleaseCurl();
    ReleaseFilaments();
    m_occupancyPipeline.Shutdown();
    m_raymarchPipeline.Shutdown();
    m_atmospherePipeline.Shutdown();
    m_boundedAtmospherePipeline.Shutdown();
    m_compositePipeline.Shutdown();
    m_visibilityPipeline.Shutdown();
    m_indirectPipeline.Shutdown();
    m_hdrCompositePipeline.Shutdown();
    m_froxelPipeline.Shutdown();
    m_raymarchFroxelPipeline.Shutdown();
    m_atmosphereFroxelPipeline.Shutdown();
    m_boundedAtmosphereFroxelPipeline.Shutdown();
    m_froxelResolvePipeline.Shutdown();
    m_froxelIndirectPipeline.Shutdown();
    m_atmosphereLitPipeline.Shutdown();
    m_boundedAtmosphereLitPipeline.Shutdown();
    m_froxelLocalLightPipeline.Shutdown();
    m_atmosphereLitLocalPipeline.Shutdown();
    m_boundedAtmosphereLitLocalPipeline.Shutdown();
    m_froxelMediumPipeline.Shutdown();
    m_froxelIntegratePipeline.Shutdown();
    m_boundedFroxelIntegratePipeline.Shutdown();
    m_integratedPipeline.Shutdown();
    m_boundedIntegratedPipeline.Shutdown();
    m_raymarchFilteredPipeline.Shutdown();
    m_atmosphereFilteredPipeline.Shutdown();
    m_boundedAtmosphereFilteredPipeline.Shutdown();
}

void EditorMistPass::RetireStorage(std::uint64_t fence)
{
    for(Buffer* buffer : StorageBuffers())
    {
        if(buffer->GetResource() != nullptr)
        {
            RetiredBuffer retired;
            retired.buffer = std::move(*buffer);
            retired.fence = fence;
            m_retired.push_back(std::move(retired));
        }
    }

    if(m_froxels.GetResource() != nullptr)
    {
        RetiredBuffer retired;
        retired.buffer = std::move(m_froxels);
        retired.fence = fence;
        m_retired.push_back(std::move(retired));
        m_froxelCapacity = 0;
    }

    if(m_filteredResult.GetResource() != nullptr)
    {
        RetiredBuffer retired;
        retired.buffer = std::move(m_filteredResult);
        retired.fence = fence;
        m_retired.push_back(std::move(retired));
        m_filteredResultBytes = 0;
    }

    RetireFroxelTextures(fence);
    m_width = 0;
    m_height = 0;
    m_bricks = 0;
}

void EditorMistPass::CollectRetired(std::uint64_t completedFence)
{
    while(!m_retired.empty() && m_retired.front().fence <= completedFence)
    {
        m_retired.front().buffer.Shutdown();
        m_retired.pop_front();
    }

    while(!m_retiredTextures.empty() && m_retiredTextures.front().fence <= completedFence)
    {
        ReleaseFroxelTexture(m_retiredTextures.front().texture);
        m_retiredTextures.pop_front();
    }
}

void EditorMistPass::ReleaseFroxelTexture(FroxelTexture& texture)
{
    if(m_descriptorHeap != nullptr)
    {
        if(texture.srv != UINT32_MAX)
        {
            m_descriptorHeap->Free(texture.srv);
        }

        if(texture.uav != UINT32_MAX)
        {
            m_descriptorHeap->Free(texture.uav);
        }
    }

    texture = FroxelTexture{};
}

// Frames still in flight may read the textures, so they (and their descriptors) wait for `fence`. The history goes with them.
void EditorMistPass::RetireFroxelTextures(std::uint64_t fence)
{
    for(FroxelTexture& texture : m_froxelTextures)
    {
        if(texture.resource != nullptr)
        {
            RetiredTexture retired;
            retired.texture = std::move(texture);
            retired.fence = fence;
            m_retiredTextures.push_back(std::move(retired));
        }

        texture = FroxelTexture{};
    }

    // V1c-2: the indirect texture holds only this frame's resample, so it carries no history; it is retired with the pair.
    if(m_froxelIndirect.resource != nullptr)
    {
        RetiredTexture retired;
        retired.texture = std::move(m_froxelIndirect);
        retired.fence = fence;
        m_retiredTextures.push_back(std::move(retired));
    }

    m_froxelIndirect = FroxelTexture{};

    // V1c-4: likewise the local-light volume (this frame's Lights only, no history).
    if(m_froxelLocalLights.resource != nullptr)
    {
        RetiredTexture retired;
        retired.texture = std::move(m_froxelLocalLights);
        retired.fence = fence;
        m_retiredTextures.push_back(std::move(retired));
    }

    m_froxelLocalLights = FroxelTexture{};
    m_froxelLocalLightBytes = 0;

    // V1d: likewise the integrated renderer's volumes (rebuilt every frame, no history).
    for(FroxelTexture& texture : m_froxelIntegratedTextures)
    {
        if(texture.resource != nullptr)
        {
            RetiredTexture retired;
            retired.texture = std::move(texture);
            retired.fence = fence;
            m_retiredTextures.push_back(std::move(retired));
        }

        texture = FroxelTexture{};
    }

    m_froxelIntegratedBytes = 0;
    m_froxelTextureSize = {0, 0, 0};
    m_froxelTextureBytes = 0;
    m_froxelIndirectBytes = 0;
    m_froxelHistory.valid = false;
}

bool EditorMistPass::CreateFroxelTexture(const std::array<std::uint32_t, 3>& size, DXGI_FORMAT format, FroxelTexture& texture)
{
    const MemoryCategoryScope memoryScope(MemoryCategory::AirVolumetrics);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    description.Width = size[0];
    description.Height = size[1];
    description.DepthOrArraySize = static_cast<UINT16>(size[2]);
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // Never read before CSFroxelResolve first writes it: an invalid history is not sampled.
    if(FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&texture.resource))))
    {
        return false;
    }

    MemoryRegistry::TrackResource(texture.resource, MemoryCategory::AirVolumetrics);

    texture.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    texture.srv = m_descriptorHeap->Allocate();
    texture.uav = m_descriptorHeap->Allocate();

    if(texture.srv == UINT32_MAX || texture.uav == UINT32_MAX)
    {
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture3D.MipLevels = 1;
    m_device->CreateShaderResourceView(texture.resource.Get(), &view, m_descriptorHeap->GetCPUHandle(texture.srv));

    D3D12_UNORDERED_ACCESS_VIEW_DESC access{};
    access.Format = format;
    access.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    access.Texture3D.WSize = size[2];
    m_device->CreateUnorderedAccessView(texture.resource.Get(), nullptr, &access, m_descriptorHeap->GetCPUHandle(texture.uav));
    return true;
}

bool EditorMistPass::EnsureFroxelTextures(const std::array<std::uint32_t, 3>& size, DXGI_FORMAT format, std::uint64_t lastSubmissionFence)
{
    if(m_froxelTextures[0].resource != nullptr && size == m_froxelTextureSize && format == m_froxelTextureFormat)
    {
        return true;
    }

    RetireFroxelTextures(lastSubmissionFence);

    for(FroxelTexture& texture : m_froxelTextures)
    {
        if(!CreateFroxelTexture(size, format, texture))
        {
            // Never recorded into: released at once.
            for(FroxelTexture& created : m_froxelTextures)
            {
                ReleaseFroxelTexture(created);
            }

            return false;
        }
    }

    m_froxelTextureSize = size;
    m_froxelTextureFormat = format;
    m_froxelLatest = 0;
    D3D12_RESOURCE_DESC description = m_froxelTextures[0].resource->GetDesc();
    const D3D12_RESOURCE_ALLOCATION_INFO allocation = m_device->GetResourceAllocationInfo(0, 1, &description);
    m_froxelTextureBytes = allocation.SizeInBytes * m_froxelTextures.size();
    return true;
}

void EditorMistPass::TransitionFroxelTexture(ID3D12GraphicsCommandList* commandList, FroxelTexture& texture, D3D12_RESOURCE_STATES state)
{
    if(texture.state == state)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = texture.state;
    barrier.Transition.StateAfter = state;
    commandList->ResourceBarrier(1, &barrier);
    texture.state = state;
}

void EditorMistPass::Idle(std::uint64_t lastSubmissionFence, std::uint64_t completedFence)
{
    CollectRetired(completedFence);

    if(HasStorage())
    {
        RetireStorage(lastSubmissionFence);
    }
}

bool EditorMistPass::EnsureStorage(std::uint32_t width, std::uint32_t height, std::uint32_t bricks, std::uint64_t lastSubmissionFence)
{
    if(HasStorage() && width == m_width && height == m_height && bricks == m_bricks)
    {
        return true;
    }

    const MemoryCategoryScope memoryScope(MemoryCategory::AirVolumetrics);

    // A new size: frames still in flight may read the old buffers, so they wait for the last submission.
    RetireStorage(lastSubmissionFence);
    const auto create = [this](Buffer& buffer, std::size_t bytes)
    {
        return buffer.InitializeGPUWritable(m_device, bytes) && buffer.CreateRawSRV(m_device, *m_descriptorHeap) &&
            buffer.CreateRawUAV(m_device, *m_descriptorHeap);
    };
    const std::size_t pixels = std::size_t{width} * height;

    const std::size_t cells = std::size_t{bricks} * BrickCells * BrickCells * BrickCells;

    if(!create(m_result, pixels * 16) || !create(m_distance, pixels * 4) || !create(m_occupancy, std::size_t{bricks} * 4) ||
        !create(m_visibility, cells * 4))
    {
        // Never recorded into: released at once.
        for(Buffer* buffer : StorageBuffers())
        {
            buffer->Shutdown();
        }

        return false;
    }

    m_width = width;
    m_height = height;
    m_bricks = bricks;
    ++m_allocations;
    return true;
}

bool EditorMistPass::EnsureFilterPipeline()
{
    if(m_integratedFilterPipeline.GetRootSignature() != nullptr)
    {
        return true;
    }

    // As the other diagnostic pipelines: a failure is not retried; the composite then reads the result unfiltered.
    if(m_filterPipelineFailed || m_shaderLibrary == nullptr)
    {
        return false;
    }

    m_filterPipelineFailed = !MakeComputePipeline(m_device, *m_shaderLibrary, "CSFilterIntegrated", m_integratedFilterPipeline);
    return !m_filterPipelineFailed;
}

bool EditorMistPass::EnsureMistPipelines()
{
    if(m_occupancyPipeline.GetRootSignature() != nullptr)
    {
        return true;
    }

    // As the filter: a failure is not retried; the mist is then never marched, and the haze still draws.
    if(m_mistPipelinesFailed || m_device == nullptr || m_shaderLibrary == nullptr)
    {
        return false;
    }

    const bool created = MakeComputePipeline(m_device, *m_shaderLibrary, "CSRaymarch", m_raymarchPipeline, {true, true, false}) &&
        MakeComputePipeline(m_device, *m_shaderLibrary, "CSSunVisibility", m_visibilityPipeline, {false, false, true}) &&
        MakeComputePipeline(m_device, *m_shaderLibrary, "CSIndirectFill", m_indirectPipeline, {false, false, true}) &&
        MakeComputePipeline(m_device, *m_shaderLibrary, "CSOccupancy", m_occupancyPipeline);

    // CSOccupancy is created last, so its root signature means all four exist.
    m_mistPipelinesFailed = !created;
    return created;
}

bool EditorMistPass::EnsureFilteredResult(std::size_t bytes, std::uint64_t lastSubmissionFence)
{
    if(m_filteredResult.GetResource() != nullptr && bytes <= m_filteredResultBytes)
    {
        return true;
    }

    const MemoryCategoryScope memoryScope(MemoryCategory::AirVolumetrics);

    // Frames still in flight may read the old copy, so it waits for the last submission.
    if(m_filteredResult.GetResource() != nullptr)
    {
        RetiredBuffer retired;
        retired.buffer = std::move(m_filteredResult);
        retired.fence = lastSubmissionFence;
        m_retired.push_back(std::move(retired));
    }

    m_filteredResultBytes = 0;

    if(!m_filteredResult.InitializeGPUWritable(m_device, bytes) || !m_filteredResult.CreateRawSRV(m_device, *m_descriptorHeap) ||
        !m_filteredResult.CreateRawUAV(m_device, *m_descriptorHeap))
    {
        // Never recorded into: released at once.
        m_filteredResult.Shutdown();
        return false;
    }

    m_filteredResultBytes = bytes;
    return true;
}

bool EditorMistPass::EnsureFroxelStorage(std::size_t froxels, std::uint64_t lastSubmissionFence)
{
    if(m_froxels.GetResource() != nullptr && froxels <= m_froxelCapacity)
    {
        return true;
    }

    const MemoryCategoryScope memoryScope(MemoryCategory::AirVolumetrics);

    // Frames still in flight may read the old volume, so it waits for the last submission.
    if(m_froxels.GetResource() != nullptr)
    {
        RetiredBuffer retired;
        retired.buffer = std::move(m_froxels);
        retired.fence = lastSubmissionFence;
        m_retired.push_back(std::move(retired));
    }

    m_froxelCapacity = 0;

    if(!m_froxels.InitializeGPUWritable(m_device, froxels * 4) || !m_froxels.CreateRawSRV(m_device, *m_descriptorHeap) ||
        !m_froxels.CreateRawUAV(m_device, *m_descriptorHeap))
    {
        m_froxels.Shutdown();
        return false;
    }

    m_froxelCapacity = froxels;
    return true;
}

bool EditorMistPass::EnsureFroxelPipelines()
{
    if(m_froxelPipeline.GetRootSignature() != nullptr)
    {
        return true;
    }

    // A failure is not retried every frame: the diagnostic path then stays off (the production path draws instead).
    if(m_froxelPipelinesFailed || m_shaderLibrary == nullptr)
    {
        return false;
    }

    m_froxelPipelinesFailed =
        !MakeComputePipeline(m_device, *m_shaderLibrary, "CSRaymarchFroxel", m_raymarchFroxelPipeline, {true, true, false}) ||
        !MakeComputePipeline(m_device, *m_shaderLibrary, "CSRaymarchAtmosphereFroxel", m_atmosphereFroxelPipeline, {true, true, false}) ||
        !MakeComputePipeline(m_device, *m_shaderLibrary, "CSRaymarchAtmosphereBoundedFroxel", m_boundedAtmosphereFroxelPipeline,
            {true, true, false}) ||
        !MakeComputePipeline(m_device, *m_shaderLibrary, "CSFroxelSunVisibility", m_froxelPipeline, {false, false, true});
    return !m_froxelPipelinesFailed;
}

bool EditorMistPass::EnsureFroxelIndirectPipelines()
{
    if(m_froxelIndirectPipeline.GetRootSignature() != nullptr)
    {
        return true;
    }

    // As EnsureFroxelPipelines: a failure is not retried; the lit path stays off and the V1b texture path draws instead.
    if(m_froxelIndirectPipelinesFailed || m_shaderLibrary == nullptr)
    {
        return false;
    }

    const ComputeBindings marchBindings{true, true, false, true};
    m_froxelIndirectPipelinesFailed =
        !MakeComputePipeline(m_device, *m_shaderLibrary, "CSRaymarchAtmosphereFroxelLit", m_atmosphereLitPipeline, marchBindings) ||
        !MakeComputePipeline(m_device, *m_shaderLibrary, "CSRaymarchAtmosphereBoundedFroxelLit", m_boundedAtmosphereLitPipeline, marchBindings) ||
        !MakeComputePipeline(m_device, *m_shaderLibrary, "CSFroxelIndirect", m_froxelIndirectPipeline);
    return !m_froxelIndirectPipelinesFailed;
}

bool EditorMistPass::EnsureFroxelLocalLightPipelines()
{
    if(m_froxelLocalLightPipeline.GetRootSignature() != nullptr)
    {
        return true;
    }

    // As EnsureFroxelPipelines: a failure is not retried; the V1c-2 lit path draws instead.
    if(m_froxelLocalLightPipelinesFailed || m_shaderLibrary == nullptr)
    {
        return false;
    }

    const ComputeBindings marchBindings{true, true, false, true};
    // The local-light pass reads the Light table (b1) and nothing else besides its constants.
    const ComputeBindings lightBindings{false, true, false, false};
    m_froxelLocalLightPipelinesFailed =
        !MakeComputePipeline(m_device, *m_shaderLibrary, "CSRaymarchAtmosphereFroxelLitLocal", m_atmosphereLitLocalPipeline, marchBindings) ||
        !MakeComputePipeline(m_device, *m_shaderLibrary, "CSRaymarchAtmosphereBoundedFroxelLitLocal", m_boundedAtmosphereLitLocalPipeline,
            marchBindings) ||
        !MakeComputePipeline(m_device, *m_shaderLibrary, "CSFroxelLocalLights", m_froxelLocalLightPipeline, lightBindings);
    return !m_froxelLocalLightPipelinesFailed;
}

bool EditorMistPass::EnsureFroxelIntegratedPipelines()
{
    if(m_froxelMediumPipeline.GetRootSignature() != nullptr)
    {
        return true;
    }

    // As EnsureFroxelPipelines: a failure is not retried; the V1c-2 lit path draws instead.
    if(m_froxelIntegratedPipelinesFailed || m_shaderLibrary == nullptr)
    {
        return false;
    }

    // The medium samples the detail textures (s0); the integration reads the environment (s0), the Light table (b1) and, with sub-samples,
    // the froxel volumes (s1); the lookup the same.
    const ComputeBindings mediumBindings{true, false, false, false};
    const ComputeBindings integrateBindings{true, true, false, true};
    const ComputeBindings lookupBindings{true, true, false, true};
    m_froxelIntegratedPipelinesFailed =
        !MakeComputePipeline(m_device, *m_shaderLibrary, "CSFroxelIntegrate", m_froxelIntegratePipeline, integrateBindings) ||
        !MakeComputePipeline(m_device, *m_shaderLibrary, "CSFroxelIntegrateBounded", m_boundedFroxelIntegratePipeline, integrateBindings) ||
        !MakeComputePipeline(m_device, *m_shaderLibrary, "CSRaymarchIntegrated", m_integratedPipeline, lookupBindings) ||
        !MakeComputePipeline(m_device, *m_shaderLibrary, "CSRaymarchIntegratedBounded", m_boundedIntegratedPipeline, lookupBindings) ||
        !MakeComputePipeline(m_device, *m_shaderLibrary, "CSFroxelMedium", m_froxelMediumPipeline, mediumBindings);
    return !m_froxelIntegratedPipelinesFailed;
}

// m_froxelIntegratedTextures' cell-visibility volume (V1d correction; the others are RGBA16F).
static constexpr std::size_t FroxelCellVisibilityTexture = 3;

bool EditorMistPass::EnsureFroxelIntegratedTextures(const std::array<std::uint32_t, 3>& size)
{
    // The history pair was just ensured at `size`; a new size retired these with it.
    if(m_froxelIntegratedTextures[0].resource != nullptr)
    {
        return true;
    }

    std::uint64_t bytes = 0;

    for(std::size_t index = 0; index < m_froxelIntegratedTextures.size(); ++index)
    {
        FroxelTexture& texture = m_froxelIntegratedTextures[index];
        const DXGI_FORMAT format = index == FroxelCellVisibilityTexture ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;

        if(!CreateFroxelTexture(size, format, texture))
        {
            // Never recorded into: released at once.
            for(FroxelTexture& created : m_froxelIntegratedTextures)
            {
                ReleaseFroxelTexture(created);
            }

            return false;
        }

        D3D12_RESOURCE_DESC description = texture.resource->GetDesc();
        bytes += m_device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
    }

    m_froxelIntegratedBytes = bytes;
    return true;
}

bool EditorMistPass::EnsureFroxelLocalLightTexture(const std::array<std::uint32_t, 3>& size)
{
    // The history pair was just ensured at `size`; a new size retired this texture with it.
    if(m_froxelLocalLights.resource != nullptr)
    {
        return true;
    }

    if(!CreateFroxelTexture(size, DXGI_FORMAT_R16G16B16A16_FLOAT, m_froxelLocalLights))
    {
        // Never recorded into: released at once.
        ReleaseFroxelTexture(m_froxelLocalLights);
        return false;
    }

    D3D12_RESOURCE_DESC description = m_froxelLocalLights.resource->GetDesc();
    m_froxelLocalLightBytes = m_device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
    return true;
}

bool EditorMistPass::EnsureFroxelIndirectTexture(const std::array<std::uint32_t, 3>& size)
{
    // The history pair was just ensured at `size`; a new size retired this texture with it.
    if(m_froxelIndirect.resource != nullptr)
    {
        return true;
    }

    if(!CreateFroxelTexture(size, DXGI_FORMAT_R16_UNORM, m_froxelIndirect))
    {
        // Never recorded into: released at once.
        ReleaseFroxelTexture(m_froxelIndirect);
        return false;
    }

    D3D12_RESOURCE_DESC description = m_froxelIndirect.resource->GetDesc();
    m_froxelIndirectBytes = m_device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
    return true;
}

bool EditorMistPass::EnsureFroxelTexturePipelines()
{
    if(m_froxelResolvePipeline.GetRootSignature() != nullptr)
    {
        return true;
    }

    // As EnsureFroxelPipelines: a failure is not retried; the texture path stays off and V1a's buffer lookup draws instead.
    if(m_froxelTexturePipelinesFailed || m_shaderLibrary == nullptr)
    {
        return false;
    }

    const ComputeBindings marchBindings{true, true, false, true};
    const ComputeBindings resolveBindings{false, false, false, true};
    m_froxelTexturePipelinesFailed =
        !MakeComputePipeline(m_device, *m_shaderLibrary, "CSRaymarchFroxelFiltered", m_raymarchFilteredPipeline, marchBindings) ||
        !MakeComputePipeline(m_device, *m_shaderLibrary, "CSRaymarchAtmosphereFroxelFiltered", m_atmosphereFilteredPipeline, marchBindings) ||
        !MakeComputePipeline(m_device, *m_shaderLibrary, "CSRaymarchAtmosphereBoundedFroxelFiltered", m_boundedAtmosphereFilteredPipeline,
            marchBindings) ||
        !MakeComputePipeline(m_device, *m_shaderLibrary, "CSFroxelResolve", m_froxelResolvePipeline, resolveBindings);
    return !m_froxelTexturePipelinesFailed;
}

bool EditorMistPass::Execute(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, UploadBuffer& upload,
    RenderTarget& viewportColor, DepthBuffer& viewportDepth, const Matrix4& viewProjection, const Vector3& cameraPosition,
    const AirFieldBinding& field, const AirMistBinding& mist, const AirMistRenderRequest& request, double mistSeconds,
    std::uint64_t lastSubmissionFence, std::uint64_t completedFence, ID3D12QueryHeap* timestamps, UINT firstQuery,
    const AirMistLighting* lighting)
{
    CollectRetired(completedFence);

    // AIR.4E.5a: a profiler's timestamps, when the caller brings none.
    if(timestamps == nullptr && m_diagnosticTimestamps != nullptr)
    {
        timestamps = m_diagnosticTimestamps;
        firstQuery = m_diagnosticFirstQuery;
    }
    const bool matchingTargets = viewportColor.GetWidth() == viewportDepth.GetWidth() && viewportColor.GetHeight() == viewportDepth.GetHeight();
    // AIR.4F F4.3: lit mist is scene radiance for the HDR scene colour; unlit mist is display colour for the final image.
    const bool lit = request.lighting;
    const DXGI_FORMAT targetFormat = lit ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    const bool lightingReady = !lit || (lighting != nullptr && lighting->lightTable != 0);
    // AIR.4F.2d.3: the AIR mist is marched only when shown and present; the haze is drawn whenever enabled, with or without AIR.
    // R1.2b: and only with its own pipelines (EnsureMistPipelines, created here on first use when the Editor has not already).
    const bool mistActive = request.mistVisible && field.valid != 0 && mist.valid != 0 && std::isfinite(request.opacity) && request.opacity > 0.0f &&
        EnsureMistPipelines();
    const bool hazeActive = request.haze && std::isfinite(request.hazeAttenuationDistance) && request.hazeAttenuationDistance > 0.0f;
    const bool drawable = m_compositePipeline.IsInitialized() && (mistActive || hazeActive) && matchingTargets &&
        viewportColor.GetWidth() > 0 && viewportColor.GetHeight() > 0 && viewportColor.GetFormat() == targetFormat && lightingReady;

    if(!drawable)
    {
        Idle(lastSubmissionFence, completedFence);
        return false;
    }

    // The reduced target and the window's bricks (the window is a whole number of 8-cell bricks on each axis).
    const AirMistQualitySettings quality = AirMistQualityOf(request.quality);
    const std::uint32_t fullWidth = viewportColor.GetWidth();
    const std::uint32_t fullHeight = viewportColor.GetHeight();
    const float resolutionScale = request.resolutionScale > 0.0f && request.resolutionScale <= 1.0f ? request.resolutionScale : quality.scale;
    // AIR.4E.5c.2: the basis the preset scales. The raymarch maps reduced pixels to the output through uv, so any reduced size covers
    // the same view.
    const bool internalBasis = request.resolutionBasis == AirMistResolutionBasis::Internal && request.internalWidth > 0 &&
        request.internalHeight > 0 && request.internalWidth <= fullWidth && request.internalHeight <= fullHeight;
    const std::uint32_t basisWidth = internalBasis ? request.internalWidth : fullWidth;
    const std::uint32_t basisHeight = internalBasis ? request.internalHeight : fullHeight;
    const std::uint32_t width = (std::max)(1u, static_cast<std::uint32_t>(std::ceil(static_cast<float>(basisWidth) * resolutionScale)));
    const std::uint32_t height = (std::max)(1u, static_cast<std::uint32_t>(std::ceil(static_cast<float>(basisHeight) * resolutionScale)));
    // Without the mist marched, one brick of storage stands in (never read).
    const std::uint32_t brickCounts[3] = {mistActive ? field.dimensions[0] / BrickCells : 1u, mistActive ? field.dimensions[1] / BrickCells : 1u,
        mistActive ? field.dimensions[2] / BrickCells : 1u};
    const std::uint32_t bricks = brickCounts[0] * brickCounts[1] * brickCounts[2];

    if(bricks == 0 || !EnsureStorage(width, height, bricks, lastSubmissionFence))
    {
        return false;
    }

    EditorMistConstants constants;
    constants.clipToWorld = viewProjection.Inverse();
    constants.cameraPosition[0] = cameraPosition.x;
    constants.cameraPosition[1] = cameraPosition.y;
    constants.cameraPosition[2] = cameraPosition.z;
    constants.extinction = request.opacity * ExtinctionPerDensity;
    constants.field = field;
    constants.mist0Index = mist.piece0Index;
    constants.mist1Index = mist.piece1Index;
    constants.occupancyIndex = m_occupancy.GetUAVDescriptorIndex();
    constants.depthIndex = viewportDepth.GetSRVIndex();
    constants.fullSize[0] = fullWidth;
    constants.fullSize[1] = fullHeight;
    constants.reducedSize[0] = width;
    constants.reducedSize[1] = height;
    constants.resultIndex = m_result.GetUAVDescriptorIndex();
    constants.distanceIndex = m_distance.GetUAVDescriptorIndex();
    // F4.4.2b diagnostics: finer steps scale both limits.
    const float stepScale = std::isfinite(request.stepScale) ? std::clamp(request.stepScale, 1.0f / 64.0f, 1.0f) : 1.0f;
    constants.maxSteps = static_cast<std::uint32_t>(std::ceil(static_cast<float>(quality.maxSteps) / stepScale));
    // About half a cell between samples.
    constants.minimumStep = 0.5f * field.cellSize * stepScale;
    constants.jitterEnabled = request.jitter ? 1u : 0u;
    constants.ditherMode = (std::min)(request.ditherMode, 2u);
    constants.wideUpsample = request.wideUpsample ? 1u : 0u;

    // AIR.4F.2d.3: the haze, the near clearance and the termination. Off, the march is the earlier one bit for bit.
    constants.mistActive = mistActive ? 1u : 0u;
    constants.opaqueTransmittance = std::isfinite(request.terminationTransmittance) ?
        std::clamp(request.terminationTransmittance, 0.0f, 0.5f) : 0.01f;

    if(hazeActive)
    {
        constants.hazeEnabled = 1;
        constants.hazeExtinction = 1.0f / request.hazeAttenuationDistance;

        for(std::size_t channel = 0; channel < 3; ++channel)
        {
            const float color = request.hazeColor[channel];
            constants.hazeAlbedo[channel] = std::isfinite(color) ? std::clamp(color, 0.0f, 1.0f) : 0.0f;
        }

        constants.hazeAmbient = std::isfinite(request.hazeAmbient) ? (std::max)(request.hazeAmbient, 0.0f) : 0.0f;

        // AIR.4F.2d.3b: the haze's own clearance and its height bounds (a layer that is not finite and ordered is drawn unbounded).
        if(std::isfinite(request.hazeNearDistance) && request.hazeNearDistance > 0.0f)
        {
            constants.hazeNearDistance = request.hazeNearDistance;
            constants.hazeNearTransition = std::isfinite(request.hazeNearTransition) ? (std::max)(request.hazeNearTransition, 0.01f) : 1.0f;
        }

        const bool validLayer = std::isfinite(request.hazeBaseHeight) && std::isfinite(request.hazeMaxHeight) &&
            request.hazeMaxHeight > request.hazeBaseHeight && std::isfinite(request.hazeHeightTransition) && request.hazeHeightTransition >= 0.0f;

        // The heights are stored relative to the camera, subtracted in double: the shader then works with d x direction.y alone, so a
        // layer kilometres up loses no precision to cancelling large coordinates.
        if(request.hazeHeightBounds && validLayer)
        {
            constants.heightEnabled = 1;
            constants.baseHeight = static_cast<float>(static_cast<double>(request.hazeBaseHeight) - cameraPosition.y);
            constants.maxHeight = static_cast<float>(static_cast<double>(request.hazeMaxHeight) - cameraPosition.y);
            constants.heightTransition = request.hazeHeightTransition;
        }
    }

    // AIR.4F.2d.3b: the AIR mist's own clearance (only where mist is marched).
    if(mistActive && std::isfinite(request.mistNearDistance) && request.mistNearDistance > 0.0f)
    {
        constants.mistNearDistance = request.mistNearDistance;
        constants.mistNearTransition = std::isfinite(request.mistNearTransition) ? (std::max)(request.mistNearTransition, 0.01f) : 1.0f;
    }

    // AIR.4F F4.1: the two detail phases at this transport time. Phase B runs half a period behind phase A; each restarts (a new cycle
    // offset, tau back to 0) exactly when its weight 1 - |2 * phase - 1| is 0.
    const bool detail = request.detail && m_noise != nullptr && std::isfinite(request.detailStrength) && request.detailStrength > 0.0f &&
        std::isfinite(request.detailScale) && request.detailScale > 0.0f && std::isfinite(mistSeconds);

    if(detail)
    {
        const AirMistDetailPhases phases = DetailPhasesAt(mistSeconds);

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            constants.phaseOffsetA[axis] = phases.offsetA[axis];
            constants.phaseOffsetB[axis] = phases.offsetB[axis];
        }

        constants.phaseTimeA = phases.timeA;
        constants.phaseTimeB = phases.timeB;
        constants.phaseWeightA = phases.weightA;
        constants.detailStrength = (std::min)(request.detailStrength, 1.0f);
        constants.inverseDetailScale = 1.0f / request.detailScale;
        constants.noiseIndex = m_noiseDescriptor;
        constants.noiseMeanInverse = m_noiseMeanInverse;
        constants.detailEnabled = 1;

        // AIR.4F F4.4: the curl warp of the detail's lookup; a strength of 0 leaves the F4.1 detail exactly.
        const bool curl = request.curl && m_curl != nullptr && std::isfinite(request.curlStrength) && request.curlStrength > 0.0f &&
            std::isfinite(request.curlScale) && request.curlScale > 0.0f && std::isfinite(request.curlFlow);

        if(curl)
        {
            constants.curlDisplacement = (std::min)(request.curlStrength, 4.0f) * m_curlRmsInverse;
            constants.curlInverseScale = 1.0f / request.curlScale;
            constants.curlFlow = std::clamp(request.curlFlow, 0.0f, 1.0f);
            constants.curlIndex = m_curlDescriptor;
            constants.curlEnabled = 1;
        }

        // AIR.4F F4.4.2: wisps, a second factor from the filament mask; a strength of 0 leaves the detail exactly as without them.
        const bool wisps = request.wisps && m_filaments.resource != nullptr && std::isfinite(request.wispStrength) && request.wispStrength > 0.0f &&
            std::isfinite(request.wispScale) && request.wispScale > 0.0f;

        if(wisps)
        {
            constants.wispStrength = (std::min)(request.wispStrength, 1.0f);
            constants.wispInverseScale = 1.0f / request.wispScale;
            constants.wispMeanInverse = m_filaments.meanInverse;
            constants.wispSquareMeanInverse = m_filaments.squareMeanInverse;
            constants.wispIndex = m_filaments.descriptor;
            constants.wispEnabled = 1;
        }

        // AIR.4F F4.4.2e: density-dependent detail; a strength of 0 leaves the detail exactly as without it (the shader skips it).
        if(request.densityDetail && std::isfinite(request.densityDetailStrength) && request.densityDetailStrength > 0.0f)
        {
            constants.densityDetailStrength = (std::min)(request.densityDetailStrength, 1.0f);
        }
    }

    // AIR.4F F4.3: single scattering; the sun is shadowed only with a TLAS to trace.
    const bool shadowed = lit && request.sunShadows && lighting->topLevel != 0 && mistActive;
    m_sunShadowed = shadowed;
    m_visibilityRayBound = 0;
    m_indirectRayBound = 0;

    if(lit)
    {
        const auto finiteOr = [](float value, float fallback)
        {
            return std::isfinite(value) ? value : fallback;
        };
        constants.lightingEnabled = 1;
        constants.visibilityIndex = shadowed ? m_visibility.GetUAVDescriptorIndex() : UINT32_MAX;
        constants.scattering = std::clamp(finiteOr(request.scattering, 0.0f), 0.0f, 1.0f);
        constants.anisotropy = std::clamp(finiteOr(request.anisotropy, 0.0f), -0.9f, 0.9f);
        constants.ambient = std::clamp(finiteOr(request.ambient, 0.0f), 0.0f, 1.0f);
        // AIR.4F.2d.2: each tint channel within [0, 1] (a non-finite one is white), the strength within [0, 1].
        constants.scatteringTint[0] = std::clamp(finiteOr(request.scatteringTint[0], 1.0f), 0.0f, 1.0f);
        constants.scatteringTint[1] = std::clamp(finiteOr(request.scatteringTint[1], 1.0f), 0.0f, 1.0f);
        constants.scatteringTint[2] = std::clamp(finiteOr(request.scatteringTint[2], 1.0f), 0.0f, 1.0f);
        constants.multipleScattering = std::clamp(finiteOr(request.multipleScattering, 0.0f), 0.0f, 1.0f);
        // AIR.4F.2d.3e: the gather needs the shadowed visibility (without it every sample is sunlit, and 1 - V = 0). Its reach is 1 to
        // 32 cells (EditorMist.hlsl, IndirectMaximumSteps); a non-finite reach takes the default.
        constants.indirectScattering = shadowed ? std::clamp(finiteOr(request.indirectScattering, 0.0f), 0.0f, 1.0f) : 0.0f;
        constants.indirectReach = std::clamp(finiteOr(request.indirectReach, 8.0f), field.cellSize, 32.0f * field.cellSize);
        constants.irradianceIndex = lighting->environmentIrradiance;
        // AIR.4F.2d.3d: 1, 2, 4 or 8 samples per piece (anything else falls back to 4).
        // V1c-3: a profiler's override of the rays per piece (0: the request's).
        const std::uint32_t samples = m_diagnosticSunSamples != 0 ? m_diagnosticSunSamples : request.sunVisibilitySamples;
        constants.visibilitySamples = samples == 1 || samples == 2 || samples == 4 || samples == 8 ? samples : 4;
        const Vector3 direction = lighting->sunDirection.Normalized();
        const float sun[3] = {lighting->sunRadiance.x, lighting->sunRadiance.y, lighting->sunRadiance.z};
        const float directions[3] = {direction.x, direction.y, direction.z};

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            constants.sunDirection[axis] = std::isfinite(directions[axis]) ? directions[axis] : 0.0f;
            constants.sunRadiance[axis] = (std::max)(finiteOr(sun[axis], 0.0f), 0.0f);
        }
    }

    // V1a diagnostics: the froxel sun visibility volume, for lit and shadowed mist only (otherwise nothing reads a sun visibility).
    bool froxels = shadowed && (request.froxelSunVisibility || m_diagnosticFroxels) && EnsureFroxelPipelines();
    m_froxelSize = {0, 0, 0};

    if(froxels)
    {
        const std::uint32_t divisor = std::clamp(request.froxelDivisor, 1u, 64u);
        const std::uint32_t slices = std::clamp(request.froxelSlices, 1u, 256u);
        const std::array<std::uint32_t, 3> size{(width + divisor - 1) / divisor, (height + divisor - 1) / divisor, slices};

        // Slices run geometrically from the near distance to the window's farthest corner, the farthest any mist sample can be.
        const double camera[3] = {cameraPosition.x, cameraPosition.y, cameraPosition.z};
        double farthest = 0.0;

        for(std::uint32_t corner = 0; corner < 8; ++corner)
        {
            double squared = 0.0;

            for(std::size_t axis = 0; axis < 3; ++axis)
            {
                const std::int64_t cell = static_cast<std::int64_t>(field.minimum[axis]) + ((corner >> axis) & 1u ? field.dimensions[axis] : 0);
                const double offset = static_cast<double>(cell) * field.cellSize - camera[axis];
                squared += offset * offset;
            }

            farthest = (std::max)(farthest, std::sqrt(squared));
        }

        const double nearDistance = std::isfinite(request.froxelNear) && request.froxelNear > 0.0f ? request.froxelNear : 0.5;
        const double logRange = std::log((std::max)(farthest, nearDistance * 1.01) / nearDistance);
        froxels = EnsureFroxelStorage(std::size_t{size[0]} * size[1] * size[2], lastSubmissionFence);

        if(froxels)
        {
            m_froxelSize = size;
            constants.froxelIndex = m_froxels.GetUAVDescriptorIndex();
            constants.froxelSize[0] = size[0];
            constants.froxelSize[1] = size[1];
            constants.froxelSize[2] = size[2];
            constants.froxelNear = static_cast<float>(nearDistance);
            constants.froxelSlicesPerLog = static_cast<float>(static_cast<double>(slices) / logRange);
            constants.froxelLogPerSlice = static_cast<float>(logRange / static_cast<double>(slices));
            constants.froxelSeed = request.froxelSeed;
            constants.froxelJitter = request.froxelJitter ? FroxelJitterHash : FroxelJitterCentre;
        }
    }

    // V1b-1 diagnostics: the hardware-filtered texture, and with it optionally the temporal history. The profiler's override replaces
    // the request's settings when the request itself did not ask for froxels. Without the texture (a pipeline or allocation failure),
    // V1a's buffer lookup draws instead.
    const bool requestedFroxels = request.froxelSunVisibility;
    const AirFroxelFilter froxelFilter = requestedFroxels ? request.froxelFilter : m_diagnosticFroxelFilter;
    const bool froxelTemporal = requestedFroxels ? request.froxelTemporal : m_diagnosticFroxelTemporal;
    const AirFroxelJitterSequence froxelSequence = requestedFroxels ? request.froxelSequence : m_diagnosticFroxelSequence;
    // V1b-2/V1b-3: the history's rectification. The history-aware box, the adaptive blend and gated clipping need each froxel's temporal
    // deviation, so the volumes then carry a second channel (R16G16_UNORM); a change of format recreates them (and resets the history).
    const AirFroxelHistoryClip froxelClip = requestedFroxels ? request.froxelHistoryClip : m_diagnosticFroxelClip;
    const bool froxelDecorrelate = requestedFroxels ? request.froxelDecorrelateJitter : m_diagnosticFroxelDecorrelate;
    const bool froxelMoments = froxelTemporal && (froxelClip == AirFroxelHistoryClip::HistoryAware || request.froxelAdaptiveBlend ||
        request.froxelClipGated);
    const DXGI_FORMAT froxelFormat = froxelMoments ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM;
    const bool froxelTexture = froxels && (froxelTemporal || froxelFilter == AirFroxelFilter::Hardware) && EnsureFroxelTexturePipelines() &&
        EnsureFroxelTextures(m_froxelSize, froxelFormat, lastSubmissionFence);
    // V1c-2 diagnostics: the froxel indirect fill (the profiler's override as for the other settings), on the texture path only, and only
    // with an indirect fill to resample. Without it (a pipeline or allocation failure) the V1b texture path draws instead.
    const bool requestedIndirect = requestedFroxels ? request.froxelIndirect : m_diagnosticFroxelIndirect;
    const bool froxelIndirect = froxelTexture && requestedIndirect && EnsureFroxelIndirectPipelines() &&
        EnsureFroxelIndirectTexture(m_froxelSize);
    // V1c-4 diagnostics: the table Lights from a froxel volume, on the lit path only (it builds on V1c-2's kernels).
    const bool requestedLocalLights = requestedFroxels ? request.froxelLocalLights : m_diagnosticFroxelLocalLights;
    // V1d: the integrated renderer, on the lit path; it replaces the V1c-4 Lights volume (its integration evaluates the Lights) and V1c-2's
    // resample (its medium pass writes the fill).
    const bool requestedIntegrated = requestedFroxels ? request.froxelIntegrated : m_diagnosticFroxelIntegrated;
    const bool froxelIntegrated = froxelIndirect && requestedIntegrated && EnsureFroxelIntegratedPipelines() &&
        EnsureFroxelIntegratedTextures(m_froxelSize);
    // V1d correction: with the medium pass's cell visibility, nothing reads the froxel visibility, so its trace and resolve are skipped
    // (their stamps stay) and its history lapses.
    const bool froxelTraceSkipped = froxelIntegrated && requestedFroxels && request.froxelCellVisibilityVolume;
    const bool froxelLocalLights = froxelIndirect && !froxelIntegrated && requestedLocalLights && EnsureFroxelLocalLightPipelines() &&
        EnsureFroxelLocalLightTexture(m_froxelSize);
    FroxelTexture& froxelMedium = m_froxelIntegratedTextures[0];
    FroxelTexture& froxelIntegratedLight = m_froxelIntegratedTextures[1];
    FroxelTexture& froxelSegments = m_froxelIntegratedTextures[2];
    FroxelTexture& froxelCellVisibility = m_froxelIntegratedTextures[FroxelCellVisibilityTexture];
    bool useFroxelHistory = false;
    const char* froxelResetReason = "";
    FroxelTexture* froxelOutput = nullptr;
    FroxelTexture* froxelPrevious = nullptr;

    if(froxels)
    {
        // Whether the previous execution's volume may be reprojected into this one. Reprojection rejects points outside the previous
        // volume locally; these are the changes it cannot see. Each also restarts the jitter sequence, so a run is deterministic.
        const FroxelHistory& history = m_froxelHistory;
        const Vector3 sunDirection{constants.sunDirection[0], constants.sunDirection[1], constants.sunDirection[2]};
        const double cutDistance = std::isfinite(request.froxelCutDistance) && request.froxelCutDistance > 0.0f ? request.froxelCutDistance : 4.0;
        const double cameraMove = (cameraPosition - history.cameraPosition).Length();
        const double sunCosine = static_cast<double>(Vector3::Dot(sunDirection, history.sunDirection));
        const bool sameMapping = m_froxelSize == history.size && constants.froxelNear == history.nearDistance;
        const bool sameJitter = request.froxelJitter == history.jitter && froxelSequence == history.sequence &&
            request.froxelSeed == history.seed && froxelDecorrelate == history.decorrelate;

        if(!history.valid)
        {
            froxelResetReason = "no history";
        }
        else if(history.execution + 1 != m_executions)
        {
            froxelResetReason = "path inactive in the previous execution";
        }
        else if(request.froxelHistoryReset)
        {
            froxelResetReason = "reset requested";
        }
        else if(!sameMapping)
        {
            froxelResetReason = "volume size or near distance changed";
        }
        else if(!sameJitter)
        {
            froxelResetReason = "jitter configuration changed";
        }
        else if(!(cameraMove <= cutDistance))
        {
            froxelResetReason = "camera cut";
        }
        else if(!(sunCosine >= SunChangeCosine))
        {
            froxelResetReason = "sun direction changed";
        }

        if(*froxelResetReason != '\0')
        {
            m_froxelSequenceIndex = 0;
        }

        useFroxelHistory = froxelTexture && froxelTemporal && *froxelResetReason == '\0';

        // The frame's sub-froxel offset: Halton(2, 3, 5) from index 1, the depth component shared by both sequences.
        if(request.froxelJitter && froxelSequence != AirFroxelJitterSequence::Static)
        {
            const std::uint64_t haltonIndex = m_froxelSequenceIndex % FroxelSequenceLength + 1;
            const bool full = froxelSequence == AirFroxelJitterSequence::Full;
            constants.froxelJitter = FroxelJitterOffset;
            constants.froxelOffset[0] = full ? static_cast<float>(RadicalInverse(haltonIndex, 2)) : 0.5f;
            constants.froxelOffset[1] = full ? static_cast<float>(RadicalInverse(haltonIndex, 3)) : 0.5f;
            constants.froxelOffset[2] = static_cast<float>(RadicalInverse(haltonIndex, 5));

            // V1b-3: the per-froxel rotation of the jittered axes (EditorMist.hlsl FroxelDecorrelateDepth or FroxelDecorrelateAll).
            if(froxelDecorrelate)
            {
                constants.froxelDecorrelate = full ? 2u : 1u;
            }
        }
    }

    if(froxelTexture)
    {
        froxelPrevious = &m_froxelTextures[m_froxelLatest];
        froxelOutput = &m_froxelTextures[1 - m_froxelLatest];
        constants.froxelTextureIndex = froxelOutput->uav;
        constants.froxelHistoryIndex = froxelPrevious->srv;
        constants.froxelHistoryValid = useFroxelHistory ? 1u : 0u;
        constants.froxelBlend = 1.0f;

        // V1b-2: clipping of the reprojected history (the profiler's override as for the other settings). V1b-3: the moments' uses.
        const bool validGamma = std::isfinite(request.froxelClipGamma) && request.froxelClipGamma >= 0.0f;
        const auto finiteOr = [](float value, float fallback)
        {
            return std::isfinite(value) ? value : fallback;
        };
        constants.froxelClip = static_cast<std::uint32_t>(froxelClip);
        constants.froxelMoments = froxelMoments ? 1u : 0u;
        constants.froxelTemporalGamma = (std::max)(finiteOr(request.froxelTemporalGamma, 1.0f), 0.0f);
        constants.froxelAdaptiveBlend = froxelMoments && request.froxelAdaptiveBlend ? 1u : 0u;
        constants.froxelAdaptiveLow = (std::max)(finiteOr(request.froxelAdaptiveLow, 1.5f), 0.0f);
        constants.froxelAdaptiveHigh = (std::max)(finiteOr(request.froxelAdaptiveHigh, 4.0f), constants.froxelAdaptiveLow);
        constants.froxelAdaptiveBlendMax = std::clamp(finiteOr(request.froxelAdaptiveBlendMax, 0.5f), 0.0f, 1.0f);
        constants.froxelClipGated = froxelMoments && request.froxelClipGated ? 1u : 0u;
        constants.froxelClipGamma = validGamma ? request.froxelClipGamma : 1.0f;
        constants.froxelClipIncludesCurrent = request.froxelClipIncludesCurrent ? 1u : 0u;

        if(useFroxelHistory)
        {
            const float blend = std::isfinite(request.froxelBlend) ? std::clamp(request.froxelBlend, 0.0f, 1.0f) : 0.05f;
            const FroxelHistory& history = m_froxelHistory;
            constants.froxelBlend = blend;
            const bool antiFlicker = request.froxelAntiFlicker && std::isfinite(request.froxelBlendMinimum) &&
                std::isfinite(request.froxelFlickerDistance) && request.froxelFlickerDistance > 0.0f;
            constants.froxelBlendMinimum = antiFlicker ? std::clamp(request.froxelBlendMinimum, 0.0f, blend) : blend;
            constants.froxelFlickerDistance = antiFlicker ? request.froxelFlickerDistance : 0.1f;
            constants.previousWorldToClip = history.worldToClip;
            constants.previousCameraPosition[0] = history.cameraPosition.x;
            constants.previousCameraPosition[1] = history.cameraPosition.y;
            constants.previousCameraPosition[2] = history.cameraPosition.z;
            constants.previousFroxelNear = history.nearDistance;
            constants.previousFroxelSlicesPerLog = history.slicesPerLog;
            constants.previousFroxelSize[0] = history.size[0];
            constants.previousFroxelSize[1] = history.size[1];
            constants.previousFroxelSize[2] = history.size[2];
        }
    }

    const D3D12_GPU_VIRTUAL_ADDRESS writeAddress = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    // AIR.4F F4.3: the raymarch writes its results through their UAVs and reads the visibility (V1a: and the froxels) through SRVs.
    constants.visibilityIndex = shadowed ? m_visibility.GetDescriptorIndex() : UINT32_MAX;
    constants.froxelIndex = froxels ? m_froxels.GetDescriptorIndex() : UINT32_MAX;
    // V1b-1: CSFroxelResolve reads the traced buffer (SRV) and writes the output texture (UAV); the march then reads that texture (SRV).
    D3D12_GPU_VIRTUAL_ADDRESS resolveAddress = 0;

    if(froxelTexture)
    {
        resolveAddress = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        constants.froxelTextureIndex = froxelOutput->srv;

        if(resolveAddress == 0)
        {
            return false;
        }
    }

    // V1c-2: CSFroxelIndirect reads the cell fill through the visibility SRV (set above) and writes the indirect texture's UAV; the march
    // then reads that texture's SRV.
    D3D12_GPU_VIRTUAL_ADDRESS indirectAddress = 0;

    if(froxelIntegrated)
    {
        const float radius = requestedFroxels ? request.froxelNearLightRadius : m_diagnosticNearLightRadius;
        constants.froxelNearLightRadius = std::isfinite(radius) ? (std::max)(radius, 0.0f) : 0.0f;
        constants.froxelMediumIndex = froxelMedium.uav;
        constants.froxelIntegrateSamples = std::clamp(requestedFroxels ? request.froxelIntegrateSamples : 1u, 1u, 16u);
        // 0: the froxel visibility volume; 1: cell lookups in the integration (diagnostic); 2: the medium pass's cell-visibility volume.
        const bool cellVolume = requestedFroxels && request.froxelCellVisibilityVolume;
        constants.froxelIntegrateCellVisibility = cellVolume ? 2u : (requestedFroxels && request.froxelIntegrateCellVisibility ? 1u : 0u);
        constants.froxelCellVisibilityIndex = froxelCellVisibility.uav;
        constants.froxelMediumSamples = std::clamp(requestedFroxels ? request.froxelMediumSamples : 1u, 1u, 8u);
        constants.froxelMediumAdaptive = requestedFroxels && request.froxelMediumAdaptive ? 1u : 0u;
        constants.froxelMediumLateral = requestedFroxels && request.froxelMediumLateral ? 1u : 0u;
    }

    if(froxelIndirect)
    {
        constants.froxelIndirectFillOnly = (requestedFroxels ? request.froxelIndirectFillOnly : m_diagnosticFroxelIndirectFillOnly) ? 1u : 0u;
        constants.froxelIndirectSkipEmpty = (requestedFroxels ? request.froxelIndirectSkipEmpty : m_diagnosticFroxelIndirectSkipEmpty) ? 1u : 0u;
        constants.froxelIndirectIndex = m_froxelIndirect.uav;
        indirectAddress = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        constants.froxelIndirectIndex = m_froxelIndirect.srv;

        if(indirectAddress == 0)
        {
            return false;
        }
    }

    // V1d: the integration reads the medium, the fill and the resolved visibility (SRVs) and writes the integrated-light and segment
    // volumes (UAVs); the lookup reads every volume through its SRV.
    D3D12_GPU_VIRTUAL_ADDRESS integrateAddress = 0;

    if(froxelIntegrated)
    {
        constants.froxelMediumIndex = froxelMedium.srv;
        constants.froxelIntegratedIndex = froxelIntegratedLight.uav;
        constants.froxelSegmentIndex = froxelSegments.uav;
        constants.froxelTextureIndex = froxelOutput->srv;
        constants.froxelCellVisibilityIndex = froxelCellVisibility.srv;
        integrateAddress = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        constants.froxelIntegratedIndex = froxelIntegratedLight.srv;
        constants.froxelSegmentIndex = froxelSegments.srv;

        if(integrateAddress == 0)
        {
            return false;
        }
    }

    // V1c-4: CSFroxelLocalLights writes the local-light volume's UAV; the march reads its SRV.
    D3D12_GPU_VIRTUAL_ADDRESS localLightAddress = 0;

    if(froxelLocalLights)
    {
        constants.froxelLocalLightIndex = m_froxelLocalLights.uav;
        localLightAddress = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        constants.froxelLocalLightIndex = m_froxelLocalLights.srv;

        if(localLightAddress == 0)
        {
            return false;
        }
    }

    const D3D12_GPU_VIRTUAL_ADDRESS marchAddress = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    // The composite reads the reduced buffers through their read-only views.
    constants.resultIndex = m_result.GetDescriptorIndex();
    constants.distanceIndex = m_distance.GetDescriptorIndex();
    // Experimental filter: CSFilterIntegrated reads the result and the distances and writes the filtered copy, which the composite then
    // reads in place of the result. The raymarch and the integrated lookup write the same result and distances, so either path can
    // use it; without its pipeline or buffer the composite reads the result unfiltered.
    const bool integratedFilter = request.froxelIntegratedFilter && EnsureFilterPipeline() &&
        EnsureFilteredResult(std::size_t{width} * height * 16, lastSubmissionFence);
    D3D12_GPU_VIRTUAL_ADDRESS filterAddress = 0;

    if(integratedFilter)
    {
        constants.filteredResultIndex = m_filteredResult.GetUAVDescriptorIndex();
        constants.filterDiagnostic = request.froxelFilterDiagnostic;
        filterAddress = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        constants.resultIndex = m_filteredResult.GetDescriptorIndex();
    }

    const D3D12_GPU_VIRTUAL_ADDRESS readAddress = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(writeAddress == 0 || marchAddress == 0 || readAddress == 0 || (integratedFilter && filterAddress == 0))
    {
        return false;
    }

    const auto stamp = [&](UINT index)
    {
        if(timestamps != nullptr)
        {
            commandList->EndQuery(timestamps, D3D12_QUERY_TYPE_TIMESTAMP, firstQuery + index);
        }
    };

    ID3D12DescriptorHeap* heaps[]{descriptorHeap.GetHeap()};
    commandList->SetDescriptorHeaps(1, heaps);
    const D3D12_RESOURCE_STATES previousDepthState = viewportDepth.GetState();
    viewportDepth.Transition(commandList, DepthReadState);

    for(Buffer* buffer : StorageBuffers())
    {
        buffer->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    if(froxels)
    {
        m_froxels.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // V1b-1: this frame writes one volume while reading the other as history, so no texture is both read and written by the resolve.
    if(froxelTexture)
    {
        TransitionFroxelTexture(commandList, *froxelOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TransitionFroxelTexture(commandList, *froxelPrevious, ShaderReadState);
    }

    stamp(0);

    // 1. Occupancy, one group per brick. AIR.4F.2d.3: only when the mist is marched.
    if(mistActive)
    {
        m_occupancyPipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, writeAddress);
        commandList->Dispatch(brickCounts[0], brickCounts[1], brickCounts[2]);
        UnorderedAccessBarrier(commandList);
    }

    // AIR.4F F4.3: 1b. The sun's visibility, one group per brick (the bricks near mist trace), read by the raymarch.
    if(shadowed && mistActive)
    {
        m_visibilityPipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, writeAddress);
        commandList->SetComputeRootShaderResourceView(1, lighting->topLevel);
        commandList->Dispatch(brickCounts[0], brickCounts[1], brickCounts[2]);
        m_visibilityRayBound = std::uint64_t{bricks} * BrickCells * BrickCells * BrickCells * 2 * constants.visibilitySamples;
    }

    // V1b preparation: on the froxel path only, a barrier closes the cell visibility before stamp 4, so the froxel pass cannot overlap it
    // and the interval 4 to 5 holds the froxel pass alone (production frames keep their stamps and barriers exactly).
    if(froxels)
    {
        UnorderedAccessBarrier(commandList);
    }

    // AIR.4F.2d.3e: written on every execution, so a profiler always finds stamps 0 to 4.
    stamp(4);

    // V1a diagnostics: 1b'. The froxel sun visibility, one thread per froxel, reading the occupancy (written before the barrier above).
    // Its own stamp (5) follows its barrier, so the interval 4 to 5 is the froxel pass and 5 to 1 the indirect fill. V1b-1: with the
    // texture, stamp 6 separates the trace (4 to 6) from CSFroxelResolve (6 to 5), and 4 to 5 remains the whole froxel stage.
    if(froxels && !froxelTraceSkipped)
    {
        m_froxelPipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, writeAddress);
        commandList->SetComputeRootShaderResourceView(1, lighting->topLevel);
        commandList->Dispatch((m_froxelSize[0] + 3) / 4, (m_froxelSize[1] + 3) / 4, (m_froxelSize[2] + 3) / 4);
        UnorderedAccessBarrier(commandList);
        m_froxels.Transition(commandList, ShaderReadState);

        // V1b-1: 1b''. The resolve, one thread per froxel: the trace blended with the reprojected history (or copied, without it) into
        // the output texture, which the transition then publishes to the raymarch.
        if(froxelTexture)
        {
            stamp(6);
            m_froxelResolvePipeline.Bind(commandList);
            commandList->SetComputeRootConstantBufferView(0, resolveAddress);
            commandList->Dispatch((m_froxelSize[0] + 3) / 4, (m_froxelSize[1] + 3) / 4, (m_froxelSize[2] + 3) / 4);
            TransitionFroxelTexture(commandList, *froxelOutput, ShaderReadState);
        }

        stamp(5);
    }
    else if(froxels)
    {
        stamp(6);
        stamp(5);
    }

    // AIR.4F.2d.3e: 1c. The indirect fill, one group per brick, reading every cell's finished coverage (hence the barrier) and ORing
    // each cell's fill into its own word's upper half. Only while the strength is above 0 (it is 0 unless shadowed).
    if(constants.indirectScattering > 0.0f)
    {
        UnorderedAccessBarrier(commandList);
        m_indirectPipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, writeAddress);
        commandList->SetComputeRootShaderResourceView(1, lighting->topLevel);
        commandList->Dispatch(brickCounts[0], brickCounts[1], brickCounts[2]);
        m_indirectRayBound = std::uint64_t{bricks} * BrickCells * BrickCells * BrickCells * 2 * 6;
    }

    if(shadowed && mistActive)
    {
        m_visibility.Transition(commandList, ShaderReadState);
    }

    // V1c-2 diagnostics: 1d. The froxel indirect fill, one thread per froxel, resampling the cell fill after CSIndirectFill finished (the
    // transition above orders it) into its texture, which the transition after it publishes to the march. Stamp 7 separates it, so the
    // interval 5 to 7 is the cell fill and 7 to 1 the resample.
    if(froxelIndirect && !froxelIntegrated)
    {
        stamp(7);
        TransitionFroxelTexture(commandList, m_froxelIndirect, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_froxelIndirectPipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, indirectAddress);
        commandList->Dispatch((m_froxelSize[0] + 3) / 4, (m_froxelSize[1] + 3) / 4, (m_froxelSize[2] + 3) / 4);
        TransitionFroxelTexture(commandList, m_froxelIndirect, ShaderReadState);
    }

    // V1d: 1d'. The medium (and the fill) per froxel, then the integration per froxel column. Stamp 7 before the medium, 8 before the
    // integration: 7 to 8 is the medium, 8 to 1 the integration.
    if(froxelIntegrated)
    {
        stamp(7);
        TransitionFroxelTexture(commandList, m_froxelIndirect, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TransitionFroxelTexture(commandList, froxelMedium, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TransitionFroxelTexture(commandList, froxelCellVisibility, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_froxelMediumPipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, indirectAddress);
        commandList->Dispatch((m_froxelSize[0] + 3) / 4, (m_froxelSize[1] + 3) / 4, (m_froxelSize[2] + 3) / 4);
        TransitionFroxelTexture(commandList, m_froxelIndirect, ShaderReadState);
        TransitionFroxelTexture(commandList, froxelMedium, ShaderReadState);
        TransitionFroxelTexture(commandList, froxelCellVisibility, ShaderReadState);
        stamp(8);
        TransitionFroxelTexture(commandList, froxelIntegratedLight, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TransitionFroxelTexture(commandList, froxelSegments, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        (constants.heightEnabled != 0 ? m_boundedFroxelIntegratePipeline : m_froxelIntegratePipeline).Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, integrateAddress);
        commandList->SetComputeRootConstantBufferView(1, lighting->lightTable);
        commandList->Dispatch((m_froxelSize[0] + 7) / 8, (m_froxelSize[1] + 7) / 8, 1);
        TransitionFroxelTexture(commandList, froxelIntegratedLight, ShaderReadState);
        TransitionFroxelTexture(commandList, froxelSegments, ShaderReadState);
    }

    // V1c-4 diagnostics: 1e. The table Lights per froxel along its camera ray into the local-light volume (no other input but the Light
    // table), published to the march by its transition. Stamp 8 separates it: 7 to 8 is the resample, 8 to 1 the Lights.
    if(froxelLocalLights)
    {
        stamp(8);
        TransitionFroxelTexture(commandList, m_froxelLocalLights, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_froxelLocalLightPipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, localLightAddress);
        commandList->SetComputeRootConstantBufferView(1, lighting->lightTable);
        commandList->Dispatch((m_froxelSize[0] + 3) / 4, (m_froxelSize[1] + 3) / 4, (m_froxelSize[2] + 3) / 4);
        TransitionFroxelTexture(commandList, m_froxelLocalLights, ShaderReadState);
    }

    stamp(1);

    // 2. The raymarch, one thread per reduced pixel.
    // AIR.4F.2d.3: the combined march whenever haze or the near clearance is on or the mist is not marched; otherwise the original.
    // V1c-2: the lit froxel path has only atmosphere kernels (EditorMist.hlsl), and forceAtmosphereMarch lets a diagnostic run the other
    // paths through the same kernels for a like-for-like comparison.
    const bool atmosphere = hazeActive || constants.mistNearDistance > 0.0f || !mistActive || froxelIndirect || request.forceAtmosphereMarch ||
        m_diagnosticForceAtmosphere;
    // AIR.4F.2d.3b: height-bounded haze has its own variant.
    ComputePipeline* raymarch = &m_raymarchPipeline;

    if(atmosphere)
    {
        raymarch = constants.heightEnabled != 0 ? &m_boundedAtmospherePipeline : &m_atmospherePipeline;
    }

    // V1a diagnostics: the same march reading the froxel volume's sun visibility.
    if(froxels)
    {
        raymarch = &m_raymarchFroxelPipeline;

        if(atmosphere)
        {
            raymarch = constants.heightEnabled != 0 ? &m_boundedAtmosphereFroxelPipeline : &m_atmosphereFroxelPipeline;
        }
    }

    // V1b-1 diagnostics: the same march reading the froxel texture with hardware filtering.
    if(froxelTexture)
    {
        raymarch = &m_raymarchFilteredPipeline;

        if(atmosphere)
        {
            raymarch = constants.heightEnabled != 0 ? &m_boundedAtmosphereFilteredPipeline : &m_atmosphereFilteredPipeline;
        }
    }

    // V1c-2 diagnostics: both lighting terms from froxels.
    if(froxelIndirect)
    {
        raymarch = constants.heightEnabled != 0 ? &m_boundedAtmosphereLitPipeline : &m_atmosphereLitPipeline;
    }

    // V1c-4 diagnostics: and the table Lights from froxels.
    if(froxelLocalLights)
    {
        raymarch = constants.heightEnabled != 0 ? &m_boundedAtmosphereLitLocalPipeline : &m_atmosphereLitLocalPipeline;
    }

    // V1d: the integrated lookup in place of the march.
    if(froxelIntegrated)
    {
        raymarch = constants.heightEnabled != 0 ? &m_boundedIntegratedPipeline : &m_integratedPipeline;
    }

    raymarch->Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, marchAddress);
    // Unlit, the table is never read; the constants stand in for it so every root parameter is set.
    commandList->SetComputeRootConstantBufferView(1, lit ? lighting->lightTable : writeAddress);
    commandList->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    stamp(2);

    // 3. The composite, reading the reduced results.
    m_result.Transition(commandList, ShaderReadState);
    m_distance.Transition(commandList, ShaderReadState);

    // V1d experimental filter: 2b. The filtered copy, one thread per reduced pixel (inside the 2-to-3 interval).
    if(integratedFilter)
    {
        m_filteredResult.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_integratedFilterPipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, filterAddress);
        commandList->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        m_filteredResult.Transition(commandList, ShaderReadState);
    }

    viewportColor.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE colorTarget = viewportColor.GetRTV();
    commandList->OMSetRenderTargets(1, &colorTarget, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(fullWidth), static_cast<float>(fullHeight), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(fullWidth), static_cast<LONG>(fullHeight)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    (lit ? m_hdrCompositePipeline : m_compositePipeline).Bind(commandList);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->SetGraphicsRootConstantBufferView(0, readAddress);
    commandList->DrawInstanced(3, 1, 0, 0);
    stamp(3);

    viewportDepth.Transition(commandList, previousDepthState);

    // V1b-1: what this execution resolved becomes the next one's history (only the texture path writes one).
    if(froxels)
    {
        ++m_froxelSequenceIndex;
    }

    m_froxelHistoryUsed = useFroxelHistory;
    m_froxelHistoryLength = useFroxelHistory ? m_froxelHistoryLength + 1 : 0;
    m_froxelResetReason = froxelTemporal && froxels ? froxelResetReason : "";
    // A frame without froxels keeps the record: its gap in executions invalidates it next time ("path inactive"), and its textures
    // stay untouched. A froxel frame without the texture (V1a's lookup) wrote no volume, so it leaves no history.
    if(froxels)
    {
        m_froxelHistory.valid = froxelTexture && !froxelTraceSkipped;
    }

    if(froxelTexture && !froxelTraceSkipped)
    {
        m_froxelLatest = 1 - m_froxelLatest;
        m_froxelHistory.execution = m_executions;
        m_froxelHistory.worldToClip = viewProjection;
        m_froxelHistory.cameraPosition = cameraPosition;
        m_froxelHistory.sunDirection = Vector3{constants.sunDirection[0], constants.sunDirection[1], constants.sunDirection[2]};
        m_froxelHistory.size = m_froxelSize;
        m_froxelHistory.nearDistance = constants.froxelNear;
        m_froxelHistory.slicesPerLog = constants.froxelSlicesPerLog;
        m_froxelHistory.jitter = request.froxelJitter;
        m_froxelHistory.sequence = froxelSequence;
        m_froxelHistory.seed = request.froxelSeed;
        m_froxelHistory.decorrelate = froxelDecorrelate;
    }

    m_lastStampCount = froxels ? (froxelLocalLights || froxelIntegrated ? 9u : (froxelIndirect ? 8u : (froxelTexture ? 7u : 6u))) : 5u;
    ++m_executions;
    return true;
}
