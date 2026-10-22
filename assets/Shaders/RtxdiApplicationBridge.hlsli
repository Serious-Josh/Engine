#ifndef ENGINE_RTXDI_APPLICATION_BRIDGE
#define ENGINE_RTXDI_APPLICATION_BRIDGE

// RTXPT.3 R3.4: the RTXDI application bridge (RAB_*) for initial ReSTIR DI sampling at the raster-primary
// vertex. Used only by the raster-primary ReSTIR DI variant and its verification.
//
// The includer first includes RtxdiLights.hlsli and then provides, before including this file:
//   RtxdiLightRecord RtxdiBridgeLoadLightRecord(uint lightIndex);
//   GPULight RtxdiBridgeLoadTableLight(uint sourceIndex);
//   PathTraceSun RtxdiBridgeSun();
//   PathTraceEnvironment RtxdiBridgeEnvironment();
//   #define RTXDI_BRIDGE_ENVIRONMENT_SAMPLER   (a SamplerState)
//   #define RTXDI_LIGHT_RESERVOIR_BUFFER       (a RWStructuredBuffer<RTXDI_PackedDIReservoir>)
//
// Estimator. Every candidate is a point y in RTXDI's uv domain [0, 1]^2 of one light. Its contribution there is
//   F(y) = BSDF * cos * Le / pdf_w(y)   for the sun cone and the HDRI (pdf_w: Engine's solid-angle density of
//                                       the uv -> direction mapping, i.e. the Jacobian dw/duv),
//   F(y) = BSDF * cos * E              for delta lights (point, spot, directional, delta sun; uv unused),
// exactly the value PathTraceShadeLightSample weights before MIS. RTXDI's target is luminance(F), unshadowed,
// which is positive wherever F is. RTXDI returns the selected sample y and the reservoir's unbiased contribution
// weight W; the direct-light estimate is F(y) * V(y) * W. Only light sampling is used (no BRDF candidates and no
// initial visibility), so this estimate carries MIS weight 1 (RTXPT.3 Gate 1).

// Presampling (RIS buffers, environment region) is not part of this configuration. An includer that needs RTXDI
// types before this file must define it to 0 itself, before its first RTXDI include.
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
    RtxdiLightRecord record;
    // Read only for table kinds.
    GPULight tableLight;
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
    return (RAB_LightInfo)0;
}

RAB_LightSample RAB_EmptyLightSample()
{
    RAB_LightSample lightSample = (RAB_LightSample)0;
    lightSample.sample.valid = false;
    return lightSample;
}

RAB_LightInfo RAB_LoadLightInfo(uint index, bool previousFrame)
{
    RAB_LightInfo lightInfo = RAB_EmptyLightInfo();

    // Reuse (RtxdiReuseBridge.hlsli) asks for previous-frame lights. Only lights whose whole identity is unchanged
    // translate (R3.2), so a previous light is its current counterpart's record; one without a counterpart is
    // an empty light with no contribution.
#ifdef RTXDI_BRIDGE_PREVIOUS_TO_CURRENT_LIGHT
    if(previousFrame)
    {
        int currentIndex = RTXDI_BRIDGE_PREVIOUS_TO_CURRENT_LIGHT(index);

        if(currentIndex < 0)
        {
            return lightInfo;
        }

        index = uint(currentIndex);
    }
#endif

    lightInfo.record = RtxdiBridgeLoadLightRecord(index);

    if(lightInfo.record.kind <= RTXDI_LIGHT_KIND_DIRECTIONAL)
    {
        lightInfo.tableLight = RtxdiBridgeLoadTableLight(lightInfo.record.sourceIndex);
    }

    return lightInfo;
}

RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo lightInfo, RAB_Surface surface, float2 uv)
{
    RAB_LightSample lightSample;
    lightSample.sample = RtxdiSampleLight(lightInfo.record, lightInfo.tableLight, RtxdiBridgeSun(),
        RtxdiBridgeEnvironment(), RTXDI_BRIDGE_ENVIRONMENT_SAMPLER, surface.position, uv);
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

// BRDF candidates are not part of this configuration (numBrdfSamples = 0): the SDK's BRDF strategy cannot reach
// the sun or the HDRI, which are infinite lights here. These callbacks report "no candidate" if ever reached.
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

float RAB_EvaluateLocalLightSourcePdf(uint lightIndex)
{
    return 0.0f;
}

float2 RAB_GetEnvironmentMapRandXYFromDir(float3 direction)
{
    return float2(0.0f, 0.0f);
}

float RAB_EvaluateEnvironmentMapSamplingPdf(float3 direction)
{
    return 0.0f;
}

// Initial visibility is disabled (enableInitialVisibility = 0); shading traces the selected sample's
// visibility itself. "Visible" is the no-information answer and never removes a sample.
bool RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample lightSample)
{
    return true;
}

#include "Rtxdi/DI/InitialSampling.hlsli"
#include "Rtxdi/DI/ReservoirStorage.hlsli"

// Fixed R3.4 configuration (RTXPT.3 Gate 2): uniform local selection, light candidates only.
RTXDI_DIInitialSamplingParameters RtxdiBridgeInitialSamplingParameters(uint localLightSamples,
    uint infiniteLightSamples)
{
    RTXDI_DIInitialSamplingParameters parameters = (RTXDI_DIInitialSamplingParameters)0;
    parameters.numLocalLightSamples = localLightSamples;
    parameters.numInfiniteLightSamples = infiniteLightSamples;
    parameters.numEnvironmentSamples = 0u;
    parameters.numBrdfSamples = 0u;
    parameters.brdfCutoff = 0.0f;
    parameters.brdfRayMinT = 0.0f;
    parameters.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode_UNIFORM;
    parameters.enableInitialVisibility = 0u;
    return parameters;
}

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

    return RTXDI_SampleLightsForSurface(rng, coherentRng, surface, parameters, lightBufferParameters,
        selectedSample);
}

#endif
