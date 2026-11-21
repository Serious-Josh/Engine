#ifndef ENGINE_RTXDI_APPLICATION_BRIDGE
#define ENGINE_RTXDI_APPLICATION_BRIDGE

// RTXPT.3 R3.4: the RTXDI application bridge (RAB_*) for initial ReSTIR DI sampling at the raster-primary
// vertex. Used only by the raster-primary ReSTIR DI variant and its verification.
//
// The includer first includes RtxdiLights.hlsli and then provides, before including this file:
//   GPULight RtxdiBridgeLoadLight(uint lightIndex, bool previousFrame);  (RtxdiInvalidLightRecord() outside that frame's set)
//   PathTraceEnvironment RtxdiBridgeEnvironment();
//   #define RTXDI_BRIDGE_ENVIRONMENT_SAMPLER   (a SamplerState)
//   #define RTXDI_LIGHT_RESERVOIR_BUFFER       (a RWStructuredBuffer<RTXDI_PackedDIReservoir>)
// and, for the Reservoir environment strategy (RtxdiEnvironmentConfig.hlsli):
//   bool RtxdiBridgeEscapes(RAB_Surface surface, float3 direction);  (a ray from the surface reaches the distant HDRI)
//
// Estimator. Every candidate is a point y in RTXDI's uv domain [0, 1]^2 of one light. Its contribution there is
//   F(y) = BSDF * cos * Le / pdf_w(y)   for the sun cone and the HDRI (pdf_w: Engine's solid-angle density of
//                                       the uv -> direction mapping, i.e. the Jacobian dw/duv),
//   F(y) = BSDF * cos * E              for delta lights (point, spot, directional, delta sun; uv unused),
// exactly the value PathTraceShadeLightSample weights before MIS. RTXDI's target is luminance(F), unshadowed,
// which is positive wherever F is. RTXDI returns the selected sample y and the reservoir's unbiased contribution
// weight W; the direct-light estimate is F(y) * V(y) * W.
//
// Candidate strategies. Local and infinite lights are sampled by light (no BSDF strategy can reach a delta light or the
// sun, which no BSDF candidate enters), so their candidates carry MIS weight 1 (RTXPT.3 Gate 1). PT.2B, Reservoir strategy
// only: the HDRI is RTXDI's environment light and has two strategies over the same uv domain. Environment candidates draw
// uv uniformly; Engine's CDF warp makes that luminance importance sampling, so their uv-space density is 1 on every cell
// with mass. BSDF candidates sample the surface's non-delta BSDF (density PathTracePdf) and, when their ray escapes, enter
// the HDRI at the inverse-warped uv (uv-space density PathTracePdf / pdf_w). RTXDI_LightBrdfMisWeight combines the two
// with the balance heuristic over the candidate counts; the result is the solid-angle balance heuristic of the reference
// estimator, and a BSDF candidate whose ray is blocked is a zero-weight candidate (its F V is 0 anyway).

#include "RtxdiEnvironmentConfig.hlsli"

// Presampling (RIS buffers, presampled environment tiles) is not part of this configuration: Engine's CDF warp samples
// the HDRI directly, and local lights are selected uniformly (stratified; PT.2C measured power RIS against it). An includer
// that needs RTXDI types before this file must define it to 0 itself, before its first RTXDI include.
#ifndef RTXDI_ENABLE_PRESAMPLING
#define RTXDI_ENABLE_PRESAMPLING 0
#elif RTXDI_ENABLE_PRESAMPLING != 0
#error "RtxdiApplicationBridge.hlsli requires RTXDI_ENABLE_PRESAMPLING 0."
#endif

#include "Rtxdi/RtxdiParameters.h"
#include "Rtxdi/DI/ReSTIRDIParameters.h"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"

struct RAB_Surface
{
    float3 position;
    PathTraceSurface surface;
    bool valid;
    // Distance along the camera's forward axis (temporal and spatial reuse only).
    float linearDepth;
};

struct RAB_LightInfo
{
    // The light's record in the frame it was loaded for (RtxdiLights.hlsli).
    GPULight light;
};

struct RAB_LightSample
{
    PathTraceLightSample sample;
};

RAB_Surface RAB_EmptySurface()
{
    RAB_Surface surface = (RAB_Surface)0;
    surface.valid = false;
    return surface;
}

RAB_LightInfo RAB_EmptyLightInfo()
{
    RAB_LightInfo lightInfo;
    lightInfo.light = RtxdiInvalidLightRecord();
    return lightInfo;
}

RAB_LightSample RAB_EmptyLightSample()
{
    RAB_LightSample lightSample = (RAB_LightSample)0;
    lightSample.sample.valid = false;
    return lightSample;
}

// PT.2B: previousFrame loads the previous frame's record at a previous-frame index, as RTXDI asks for it (bias correction
// evaluates a reused sample at the previous surface it was formed on, with the light as it was then). A light that moved
// or changed translates by stable key (RtxdiLightBridge.h), so this is its previous state, never its current one.
RAB_LightInfo RAB_LoadLightInfo(uint index, bool previousFrame)
{
    RAB_LightInfo lightInfo;
    lightInfo.light = RtxdiBridgeLoadLight(index, previousFrame);
    return lightInfo;
}

RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo lightInfo, RAB_Surface surface, float2 uv)
{
    RAB_LightSample lightSample;
    lightSample.sample = RtxdiSampleLight(lightInfo.light, RtxdiBridgeEnvironment(), RTXDI_BRIDGE_ENVIRONMENT_SAMPLER,
        surface.position, uv);
    return lightSample;
}

// F(y) above, unshadowed, and the part of it from the BSDF's diffuse lobe (signal extraction only). Zero for an
// invalid sample, a direction below the surface or an invalid surface.
float3 RtxdiBridgeSampleContributionLobes(RAB_LightSample lightSample, RAB_Surface surface, out float3 diffuseContribution)
{
    diffuseContribution = float3(0.0f, 0.0f, 0.0f);

    if(!surface.valid || !lightSample.sample.valid ||
        !PathTraceDirectionAboveSurface(surface.surface, lightSample.sample.direction))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 diffuseValue;
    float cosine = dot(surface.surface.normal, lightSample.sample.direction);
    float3 value = PathTraceEvaluateBSDFLobes(surface.surface, lightSample.sample.direction, diffuseValue) * cosine *
        lightSample.sample.radiance;
    float3 diffuse = diffuseValue * cosine * lightSample.sample.radiance;

    // Delta lights carry pdf 0 and their irradiance; the sun cone and HDRI divide by the uv Jacobian.
    if(lightSample.sample.pdf > 0.0f)
    {
        value /= lightSample.sample.pdf;
        diffuse /= lightSample.sample.pdf;
    }

    if(!all(isfinite(value)))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    diffuseContribution = all(isfinite(diffuse)) ? max(diffuse, 0.0f) : float3(0.0f, 0.0f, 0.0f);
    return max(value, 0.0f);
}

float3 RtxdiBridgeSampleContribution(RAB_LightSample lightSample, RAB_Surface surface)
{
    float3 diffuseContribution;
    return RtxdiBridgeSampleContributionLobes(lightSample, surface, diffuseContribution);
}

float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample lightSample, RAB_Surface surface)
{
    return PathTraceLuminance(RtxdiBridgeSampleContribution(lightSample, surface));
}

float RAB_LightSampleSolidAnglePdf(RAB_LightSample lightSample)
{
    return lightSample.sample.pdf;
}

bool RAB_IsAnalyticLightSample(RAB_LightSample lightSample)
{
    return !(lightSample.sample.pdf > 0.0f);
}

void RAB_GetLightDirDistance(RAB_Surface surface, RAB_LightSample lightSample, out float3 o_lightDir,
    out float o_lightDistance)
{
    o_lightDir = lightSample.sample.direction;
    o_lightDistance = lightSample.sample.distance;
}

float3 RAB_GetSurfaceWorldPos(RAB_Surface surface)
{
    return surface.position;
}

float RAB_SurfaceEvaluateBrdfPdf(RAB_Surface surface, float3 direction)
{
    return PathTracePdf(surface.surface, direction);
}

float RAB_EvaluateLocalLightSourcePdf(uint lightIndex)
{
    return 0.0f;
}

#if RTXDI_ENVIRONMENT_STRATEGY == RTXDI_ENVIRONMENT_STRATEGY_RESERVOIR
bool RtxdiBridgeEscapes(RAB_Surface surface, float3 direction);

// The surface RTXDI_SampleBrdf samples (its callbacks receive only a position), set by RtxdiBridgeSampleLightsForSurface.
static RAB_Surface g_rtxdiBrdfSurface;

// A BSDF candidate: PathTraceSampleBSDF's non-delta events, whose density is PathTracePdf. A delta event (the mirror
// lobe) or a zero-valued event is no candidate; the delta lobe stays with the path's continuation.
bool RAB_SurfaceImportanceSampleBrdf(RAB_Surface surface, inout RTXDI_RandomSamplerState rng, out float3 direction)
{
    float3 random;
    random.x = RTXDI_GetNextRandom(rng);
    random.y = RTXDI_GetNextRandom(rng);
    random.z = RTXDI_GetNextRandom(rng);
    PathTraceBSDFSample bsdfSample = PathTraceSampleBSDF(surface.surface, random);
    direction = bsdfSample.direction;
    return bsdfSample.valid && !bsdfSample.delta;
}

// Engine's RTXDI lights include no emissive geometry, so a BSDF ray never finds a local light: it either hits something
// (no candidate) or escapes, and RTXDI_SampleBrdf then enters the environment light.
bool RAB_TraceRayForLocalLight(float3 origin, float3 direction, float tMin, float tMax, out uint o_lightIndex,
    out float2 o_randXY)
{
    o_lightIndex = RTXDI_InvalidLightIndex;
    o_randXY = float2(0.0f, 0.0f);
    return !RtxdiBridgeEscapes(g_rtxdiBrdfSurface, direction);
}

float2 RAB_GetEnvironmentMapRandXYFromDir(float3 direction)
{
    float2 uv;
    RtxdiEnvironmentUVFromDirection(RtxdiBridgeEnvironment(), direction, uv);
    return uv;
}

// The environment strategy's uv-space density at the direction's uv: 1 where a cell with mass maps there, else 0.
float RAB_EvaluateEnvironmentMapSamplingPdf(float3 direction)
{
    float2 uv;
    return RtxdiEnvironmentUVFromDirection(RtxdiBridgeEnvironment(), direction, uv) ? 1.0f : 0.0f;
}
#else
// BRDF candidates are not part of this configuration (numBrdfSamples = 0): the HDRI is not RTXDI's environment light, and
// the SDK's BRDF strategy reaches no other light. These callbacks report "no candidate" if ever reached.
bool RAB_SurfaceImportanceSampleBrdf(RAB_Surface surface, inout RTXDI_RandomSamplerState rng, out float3 direction)
{
    direction = surface.surface.normal;
    return false;
}

bool RAB_TraceRayForLocalLight(float3 origin, float3 direction, float tMin, float tMax, out uint o_lightIndex,
    out float2 o_randXY)
{
    o_lightIndex = RTXDI_InvalidLightIndex;
    o_randXY = float2(0.0f, 0.0f);
    return false;
}

float2 RAB_GetEnvironmentMapRandXYFromDir(float3 direction)
{
    return float2(0.0f, 0.0f);
}

float RAB_EvaluateEnvironmentMapSamplingPdf(float3 direction)
{
    return 0.0f;
}
#endif

#if defined(RTXDI_BRIDGE_TRACED_INITIAL_VISIBILITY)
// Initial visibility (enableInitialVisibility = 1): the includer traces the selected initial sample from the surface.
// RTXDI then discards an occluded initial sample (W = 0, M kept) before any reuse. This is RTXDI's only caller of this
// callback in the headers Engine includes (InitialSampling.hlsli).
bool RtxdiBridgeInitialVisible(RAB_Surface surface, RAB_LightSample lightSample);

bool RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample lightSample)
{
    return RtxdiBridgeInitialVisible(surface, lightSample);
}
#else
// No traced initial visibility in this includer: "visible" is the no-information answer and never removes a sample.
bool RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample lightSample)
{
    return true;
}
#endif

#include "Rtxdi/DI/InitialSampling.hlsli"
#include "Rtxdi/DI/ReservoirStorage.hlsli"

// Fixed R3.4 configuration (RTXPT.3 Gate 2): uniform local selection. initialVisibility (WNP.2) shadow-tests the selected
// initial sample and discards it when occluded; it needs RTXDI_BRIDGE_TRACED_INITIAL_VISIBILITY. PT.2B: environment and BSDF
// candidates are read only by the Reservoir environment strategy (0 elsewhere).
RTXDI_DIInitialSamplingParameters RtxdiBridgeInitialSamplingParameters(uint localLightSamples,
    uint infiniteLightSamples, uint initialVisibility = 0u, uint environmentSamples = 0u, uint brdfSamples = 0u)
{
    RTXDI_DIInitialSamplingParameters parameters = (RTXDI_DIInitialSamplingParameters)0;
    parameters.numLocalLightSamples = localLightSamples;
    parameters.numInfiniteLightSamples = infiniteLightSamples;
#if RTXDI_ENVIRONMENT_STRATEGY == RTXDI_ENVIRONMENT_STRATEGY_RESERVOIR
    parameters.numEnvironmentSamples = environmentSamples;
    parameters.numBrdfSamples = brdfSamples;
#else
    parameters.numEnvironmentSamples = 0u;
    parameters.numBrdfSamples = 0u;
#endif
    // No MIS cutoff: a BSDF ray is never shortened (the HDRI is at infinity).
    parameters.brdfCutoff = 0.0f;
    parameters.brdfRayMinT = 0.0f;
    parameters.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode_UNIFORM;
    parameters.enableInitialVisibility = initialVisibility;
    return parameters;
}

#ifdef PATH_TRACE_REGIR
// M4.3 ReGIR (experimental; the M43/DirectLighting variants only): RTXDI_SampleLocalLightsInternal (InitialSampling.hlsli)
// with RTXDI's ReGIR selection context, reading the slots ReGIRPresample.hlsl wrote this frame (a 64-byte header, then
// uint2(RTXDI local index, W) per slot) through their UAV, rtxdiPadding0 (0xFFFFFFFF: none, use the uniform candidates).
// The cell is RTXDI_CalculateReGIRCellIndex's (position jittered by (U - 0.5) samplingJitter cellSize per axis with
// coherentRng, then the Grid lookup against the header's window); outside the window, RTXDI's uniform fallback. Each
// candidate is a stratified slot pick whose W is its inverse source pdf, streamed with the SDK's
// RTXDI_StreamLocalLightAtUVIntoReservoir and finalized as the SDK does (Documentation/Rendering/ReGIR_M4_3_Math.md).
static const uint RTXDI_BRIDGE_REGIR_HEADER_BYTES = 64u;
static const uint RTXDI_BRIDGE_REGIR_MAGIC = 0x52654749u;

RTXDI_DIReservoir RtxdiBridgeSampleLocalLightsReGIR(inout RTXDI_RandomSamplerState rng,
    inout RTXDI_RandomSamplerState coherentRng, RAB_Surface surface, RTXDI_DIInitialSamplingParameters parameters,
    RTXDI_InitialSamplingMisData misData, RTXDI_LightBufferRegion localLightBufferRegion, out RAB_LightSample o_selectedSample)
{
    o_selectedSample = RAB_EmptyLightSample();
    int cellIndex = -1;
    uint lightsPerCell = 0u;

    if(rtxdiPadding0 != 0xFFFFFFFFu)
    {
        RWByteAddressBuffer header = ResourceDescriptorHeap[rtxdiPadding0];
        const uint4 row0 = header.Load4(0u);
        const uint4 row1 = header.Load4(16u);
        const uint4 row2 = header.Load4(32u);

        if(row0.x == RTXDI_BRIDGE_REGIR_MAGIC)
        {
            const float3 center = asfloat(row0.yzw);
            const uint3 cells = row1.xyz;
            const float cellSize = asfloat(row2.x);
            const float samplingJitter = asfloat(row2.y);
            lightsPerCell = row1.w;
            float3 jitter = float3(RTXDI_GetNextRandom(coherentRng), RTXDI_GetNextRandom(coherentRng),
                RTXDI_GetNextRandom(coherentRng)) - 0.5f;
            const float3 samplingPosition = RAB_GetSurfaceWorldPos(surface) + jitter * (samplingJitter * cellSize);
            const float3 origin = center - float3(cells) * (cellSize * 0.5f);
            const int3 cell = int3(floor((samplingPosition - origin) / cellSize));

            if(all(cell >= 0) && all(cell < int3(cells)))
            {
                cellIndex = cell.x + (cell.y + cell.z * int(cells.y)) * int(cells.x);
            }
        }
    }

    if(cellIndex < 0 || lightsPerCell == 0u)
    {
        return RTXDI_SampleLocalLights(rng, coherentRng, surface, parameters, misData,
            ReSTIRDI_LocalLightSamplingMode_UNIFORM, localLightBufferRegion, o_selectedSample);
    }

    RWByteAddressBuffer slots = ResourceDescriptorHeap[rtxdiPadding0];
    RTXDI_DIReservoir state = RTXDI_EmptyDIReservoir();

    for(uint candidate = 0u; candidate < parameters.numLocalLightSamples; ++candidate)
    {
        float random = RTXDI_GetNextRandom(rng);
#if RTXDI_STRATIFY_LOCAL_SAMPLING
        random = (random + candidate) / parameters.numLocalLightSamples;
#endif
        const uint slot = min(uint(floor(random * lightsPerCell)), lightsPerCell - 1u);
        const uint2 word = slots.Load2(RTXDI_BRIDGE_REGIR_HEADER_BYTES + (uint(cellIndex) * lightsPerCell + slot) * 8u);
        const uint lightIndex = word.x;
        const float inverseSourcePdf = asfloat(word.y);
        const RAB_LightInfo lightInfo = RAB_LoadLightInfo(lightIndex, false);
        const float2 uv = RTXDI_RandomlySelectLocalLightUV(rng);
        RTXDI_StreamLocalLightAtUVIntoReservoir(rng, misData, surface, parameters.brdfCutoff, misData.localLightMisWeight,
            lightIndex, uv, inverseSourcePdf, lightInfo, state, o_selectedSample);
    }

    RTXDI_FinalizeResampling(state, 1.0f, misData.numMisSamples);
    state.M = 1;
    return state;
}
#endif

#if RTXDI_ENVIRONMENT_STRATEGY == RTXDI_ENVIRONMENT_STRATEGY_RESERVOIR
// PT.2B Reservoir strategy: RTXDI_SampleEnvironmentMap without presampling. Its presampled tile would hold uv with the inverse
// of their density; Engine's CDF warp already importance-samples luminance from uniform uv, so each candidate draws uv
// directly with inverse density 1. Weights, normalization and selection are the SDK's (RTXDI_LightBrdfMisWeight,
// RTXDI_StreamSample, RTXDI_FinalizeResampling over all MIS candidates).
RTXDI_DIReservoir RtxdiBridgeSampleEnvironment(inout RTXDI_RandomSamplerState rng, RAB_Surface surface,
    RTXDI_DIInitialSamplingParameters parameters, RTXDI_InitialSamplingMisData misData,
    RTXDI_EnvironmentLightBufferParameters environmentParameters, out RAB_LightSample o_selectedSample)
{
    RTXDI_DIReservoir state = RTXDI_EmptyDIReservoir();
    o_selectedSample = RAB_EmptyLightSample();

    if(environmentParameters.lightPresent == 0u || parameters.numEnvironmentSamples == 0u)
    {
        return state;
    }

    RAB_LightInfo lightInfo = RAB_LoadLightInfo(environmentParameters.lightIndex, false);

    for(uint candidate = 0u; candidate < parameters.numEnvironmentSamples; ++candidate)
    {
        float2 uv;
        uv.x = RTXDI_GetNextRandom(rng);
        uv.y = RTXDI_GetNextRandom(rng);
        RAB_LightSample candidateSample = RAB_SamplePolymorphicLight(lightInfo, surface, uv);
        float blendedSourcePdf = RTXDI_LightBrdfMisWeight(surface, candidateSample, 1.0f,
            misData.environmentMapMisWeight, true, misData.brdfMisWeight, parameters.brdfCutoff);
        float targetPdf = RAB_GetLightSampleTargetPdfForSurface(candidateSample, surface);
        float risRandom = RTXDI_GetNextRandom(rng);

        if(!(blendedSourcePdf > 0.0f))
        {
            continue;
        }

        if(RTXDI_StreamSample(state, environmentParameters.lightIndex, uv, risRandom, targetPdf, 1.0f / blendedSourcePdf))
        {
            o_selectedSample = candidateSample;
        }
    }

    RTXDI_FinalizeResampling(state, 1.0f, float(misData.numMisSamples));
    state.M = 1;
    return state;
}

// RTXDI_SampleLightsForSurface (InitialSampling.hlsli) with the environment reservoir above in place of the presampled one;
// every other step, and the order the reservoirs combine in, is the SDK's.
RTXDI_DIReservoir RtxdiBridgeSampleLightsForSurface(inout RTXDI_RandomSamplerState rng,
    inout RTXDI_RandomSamplerState coherentRng, RAB_Surface surface, RTXDI_DIInitialSamplingParameters parameters,
    RTXDI_LightBufferParameters lightBufferParameters, out RAB_LightSample o_lightSample)
{
    o_lightSample = RAB_EmptyLightSample();
    RTXDI_InitialSamplingMisData misData = RTXDI_ComputeInitialSamplingMisData(parameters);

    RAB_LightSample localSample = RAB_EmptyLightSample();
#ifdef PATH_TRACE_REGIR
    RTXDI_DIReservoir localReservoir = RtxdiBridgeSampleLocalLightsReGIR(rng, coherentRng, surface, parameters, misData,
        lightBufferParameters.localLightBufferRegion, localSample);
#else
    RTXDI_DIReservoir localReservoir = RTXDI_SampleLocalLights(rng, coherentRng, surface, parameters, misData,
        parameters.localLightSamplingMode, lightBufferParameters.localLightBufferRegion, localSample);
#endif

    RAB_LightSample infiniteSample = RAB_EmptyLightSample();
    RTXDI_DIReservoir infiniteReservoir = RTXDI_SampleInfiniteLights(rng, surface, parameters.numInfiniteLightSamples,
        lightBufferParameters.infiniteLightBufferRegion, infiniteSample);

    RAB_LightSample environmentSample;
    RTXDI_DIReservoir environmentReservoir = RtxdiBridgeSampleEnvironment(rng, surface, parameters, misData,
        lightBufferParameters.environmentLightParams, environmentSample);

    g_rtxdiBrdfSurface = surface;
    RAB_LightSample brdfSample = RAB_EmptyLightSample();
    RTXDI_DIReservoir brdfReservoir = RTXDI_SampleBrdf(rng, surface, parameters.numBrdfSamples, parameters.brdfCutoff,
        parameters.brdfRayMinT, misData, coherentRng, lightBufferParameters, brdfSample);

    RTXDI_DIReservoir state = RTXDI_EmptyDIReservoir();
    RTXDI_CombineDIReservoirs(state, localReservoir, 0.5f, localReservoir.targetPdf);
    bool selectInfinite = RTXDI_CombineDIReservoirs(state, infiniteReservoir, RTXDI_GetNextRandom(rng),
        infiniteReservoir.targetPdf);
    bool selectEnvironment = RTXDI_CombineDIReservoirs(state, environmentReservoir, RTXDI_GetNextRandom(rng),
        environmentReservoir.targetPdf);
    bool selectBrdf = RTXDI_CombineDIReservoirs(state, brdfReservoir, RTXDI_GetNextRandom(rng), brdfReservoir.targetPdf);

    RTXDI_FinalizeResampling(state, 1.0f, 1.0f);
    state.M = 1;

    if(selectBrdf)
    {
        o_lightSample = brdfSample;
    }
    else if(selectEnvironment)
    {
        o_lightSample = environmentSample;
    }
    else if(selectInfinite)
    {
        o_lightSample = infiniteSample;
    }
    else
    {
        o_lightSample = localSample;
    }

    if(parameters.enableInitialVisibility && RTXDI_IsValidDIReservoir(state))
    {
        if(!RAB_GetConservativeVisibility(surface, o_lightSample))
        {
            RTXDI_StoreVisibilityInDIReservoir(state, 0, true);
        }
    }

    return state;
}
#endif

#if defined(PATH_TRACE_REGIR) && RTXDI_ENVIRONMENT_STRATEGY != RTXDI_ENVIRONMENT_STRATEGY_RESERVOIR
// M4.3 ReGIR, separate-MIS strategy: RTXDI_SampleLightsForSurface (InitialSampling.hlsli, presampling disabled) with
// RtxdiBridgeSampleLocalLightsReGIR in place of RTXDI_SampleLocalLights; every other step and the order the reservoirs
// combine in are the SDK's.
RTXDI_DIReservoir RtxdiBridgeSampleLightsForSurfaceReGIR(inout RTXDI_RandomSamplerState rng,
    inout RTXDI_RandomSamplerState coherentRng, RAB_Surface surface, RTXDI_DIInitialSamplingParameters sampleParams,
    RTXDI_LightBufferParameters lightBufferParams, out RAB_LightSample o_lightSample)
{
    o_lightSample = RAB_EmptyLightSample();
    RTXDI_InitialSamplingMisData misData = RTXDI_ComputeInitialSamplingMisData(sampleParams);

    RAB_LightSample localSample = RAB_EmptyLightSample();
    RTXDI_DIReservoir localReservoir = RtxdiBridgeSampleLocalLightsReGIR(rng, coherentRng, surface, sampleParams,
        misData, lightBufferParams.localLightBufferRegion, localSample);

    RAB_LightSample infiniteSample = RAB_EmptyLightSample();
    RTXDI_DIReservoir infiniteReservoir = RTXDI_SampleInfiniteLights(rng, surface, sampleParams.numInfiniteLightSamples,
        lightBufferParams.infiniteLightBufferRegion, infiniteSample);

    RAB_LightSample brdfSample = RAB_EmptyLightSample();
    RTXDI_DIReservoir brdfReservoir = RTXDI_SampleBrdf(rng, surface, sampleParams.numBrdfSamples,
        sampleParams.brdfCutoff, sampleParams.brdfRayMinT, misData, coherentRng, lightBufferParams, brdfSample);

    RTXDI_DIReservoir state = RTXDI_EmptyDIReservoir();
    RTXDI_CombineDIReservoirs(state, localReservoir, 0.5, localReservoir.targetPdf);
    bool selectInfinite = RTXDI_CombineDIReservoirs(state, infiniteReservoir, RTXDI_GetNextRandom(rng),
        infiniteReservoir.targetPdf);
    bool selectBrdf = RTXDI_CombineDIReservoirs(state, brdfReservoir, RTXDI_GetNextRandom(rng), brdfReservoir.targetPdf);

    RTXDI_FinalizeResampling(state, 1.0, 1.0);
    state.M = 1;

    if(selectBrdf)
        o_lightSample = brdfSample;
    else if(selectInfinite)
        o_lightSample = infiniteSample;
    else
        o_lightSample = localSample;

    if(sampleParams.enableInitialVisibility && RTXDI_IsValidDIReservoir(state))
    {
        if(!RAB_GetConservativeVisibility(surface, o_lightSample))
        {
            RTXDI_StoreVisibilityInDIReservoir(state, 0, true);
        }
    }

    return state;
}
#endif

// One independent initial reservoir for a surface. sampleSeed must differ for every path sample of a pixel
// (the progressive mean relies on independent estimates). selectedSample is the exact candidate RTXDI chose.
RTXDI_DIReservoir RtxdiBridgeSampleInitialReservoir(RAB_Surface surface, uint2 pixel, uint sampleSeed,
    RTXDI_DIInitialSamplingParameters parameters, RTXDI_LightBufferParameters lightBufferParameters,
    out RAB_LightSample selectedSample)
{
    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(pixel, sampleSeed,
        RTXDI_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);
    RTXDI_RandomSamplerState coherentRng = RTXDI_InitRandomSampler(pixel / RTXDI_TILE_SIZE_IN_PIXELS, sampleSeed,
        RTXDI_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);

#if RTXDI_ENVIRONMENT_STRATEGY == RTXDI_ENVIRONMENT_STRATEGY_RESERVOIR
    return RtxdiBridgeSampleLightsForSurface(rng, coherentRng, surface, parameters, lightBufferParameters,
        selectedSample);
#elif defined(PATH_TRACE_REGIR)
    return RtxdiBridgeSampleLightsForSurfaceReGIR(rng, coherentRng, surface, parameters, lightBufferParameters,
        selectedSample);
#else
    return RTXDI_SampleLightsForSurface(rng, coherentRng, surface, parameters, lightBufferParameters,
        selectedSample);
#endif
}

#endif
