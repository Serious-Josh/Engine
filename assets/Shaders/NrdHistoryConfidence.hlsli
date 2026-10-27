// NRD RELAX R.9B diagnostics: paired-visibility probes and the diffuse history confidence they produce. Not consumed by
// NRD in R.9B; R.9C optionally consumes the same output in regular RELAX. Signal views remain diagnostic-only.
//
// One probe per 2x2 pixel block (probe grid ceil(W/2) x ceil(H/2)). A probe record is one fixed lighting experiment:
// a receiving surface (world position, viewer-facing geometric normal), one RTXDI light and one quantized sample point
// on it, the visibility traced for that pair, and the pair's unshadowed contribution T. The next frame traces the same
// ray again through its own TLAS, so for a valid record
//   dV = |V_now - V_stored|
// is a change of the scene (moving occluder, inserted or removed geometry), never a change of sample. Both visible ->
// occluded and occluded -> visible are recorded; occluded records are kept.
//
// CSProbe (one thread per probe b):
//   1. Evaluates record b, which the previous frame wrote for its block b. Its probe grid is the previous frame's
//      pixel space, the space NRD samples IN_DIFF_CONFIDENCE in (prevUV). Writes the change signal (g = T dV, w = T)
//      and the record's geometry (world position, normal) for the filter. No valid measurement writes w = 0.
//   2. Writes this frame's record b for block b: a deterministic pixel of the block, its G-buffer surface, a light
//      sample chosen by 4-candidate RIS on T (hash of probe and frame), its visibility through this frame's TLAS.
//   Record b is read and then rewritten by thread b alone, and frames execute in order on one queue: one buffer.
// CSFilter (NrdHistoryConfidencePass::FilterPasses passes, strides 1, 2, 3): 5x5 Gaussian over the probe grid with
//   plane-distance and normal weights, numerator and denominator filtered separately (no temporal filtering). The
//   last pass maps
//     r = sum(k T dV) / sum(k T),   c = max(minConfidence, (1 - saturate(sensitivity r))^2)
//   and writes c, with c = 1 wherever no valid measurement contributed.

#include "RtxdiLights.hlsli"

#include "Rtxdi/RtxdiParameters.h"
#include "Rtxdi/DI/ReSTIRDIParameters.h"

struct NrdHistoryConfidenceProbeConstants
{
    row_major float4x4 inverseViewProjection;

    uint2 resolution;
    uint2 probeResolution;

    float3 cameraPosition;
    uint depthIndex;

    uint surfaceIndex;
    uint geometricNormalIndex;
    uint recordBufferIndex;
    uint signalIndex;

    uint geometryIndex;
    uint frameIndex;
    // 1 when record b was written by the immediately preceding submitted frame under the same light identity.
    uint previousValid;
    // R.9B.2 diagnostics: the R8_UINT probe-state UAV (NRD_CONFIDENCE_STATE_*), written beside the signal.
    uint stateIndex;

    float3 sunDirectionToLight;
    float sunCosAngularRadius;

    float3 sunIrradiance;
    uint environmentTextureIndex;

    uint environmentConditionalCDFIndex;
    uint environmentMarginalCDFIndex;
    uint environmentWidth;
    uint environmentHeight;

    float environmentTotalWeight;
    uint3 padding1;
};

ConstantBuffer<NrdHistoryConfidenceProbeConstants> probeConstants : register(b0);

// The path tracer's scene lights (PathTraceReference.hlsl b1), bound from the same frame allocation.
cbuffer SceneLightConstants : register(b1)
{
    uint sceneLightCount;
    uint3 sceneLightPadding;
    GPULight sceneLights[SCENE_LIGHT_CAPACITY];
};

// The leading fields of PathTraceRtxdiConstants (RtxdiShaderData.h, PathTraceReference.hlsl b2), bound from the frame's
// RTXDI constants: the light records RTXDI indices address, two per register.
#define NRD_CONFIDENCE_LIGHT_RECORD_REGISTERS 33

cbuffer RtxdiConstants : register(b2)
{
    RTXDI_LightBufferParameters rtxdiLightBuffer;
    RTXDI_ReservoirBufferParameters rtxdiReservoirBuffer;
    RTXDI_DIBufferIndices rtxdiBufferIndices;

    uint rtxdiReservoirBufferIndex;
    uint rtxdiLocalLightSamples;
    uint rtxdiInfiniteLightSamples;
    uint rtxdiPadding;

    uint4 rtxdiLightRecords[NRD_CONFIDENCE_LIGHT_RECORD_REGISTERS];

    RTXDI_RuntimeParameters rtxdiRuntime;
    RTXDI_DISpatioTemporalResamplingParameters rtxdiSpatioTemporal;

    uint rtxdiHistoryValid;
    uint rtxdiNeighborOffsetsIndex;
    uint rtxdiMotionIndex;
    uint rtxdiCurrentLightCount;
};

RaytracingAccelerationStructure scene : register(t0);
SamplerState environmentSampler : register(s1);

static const uint NRD_CONFIDENCE_RECORD_BYTES = 32u;
static const uint NRD_CONFIDENCE_VALID = 0x80000000u;
static const uint NRD_CONFIDENCE_VISIBLE = 0x40000000u;
static const uint NRD_CONFIDENCE_LIGHT_MASK = 0xFFFFu;
static const uint NRD_CONFIDENCE_CANDIDATES = 4u;

// R.9B.2 probe state of the evaluated (previous) record, for signal view 6 only. Diagnostic: the records, signal and
// confidence never depend on it.
static const uint NRD_CONFIDENCE_STATE_INVALID = 0u;
static const uint NRD_CONFIDENCE_STATE_TABLE_LIGHT = 1u;    // point, spot or directional table light, unchanged
static const uint NRD_CONFIDENCE_STATE_SUN = 2u;            // unchanged
static const uint NRD_CONFIDENCE_STATE_ENVIRONMENT = 3u;    // unchanged
static const uint NRD_CONFIDENCE_STATE_OCCLUDED = 4u;       // visible -> occluded
static const uint NRD_CONFIDENCE_STATE_REVEALED = 5u;       // occluded -> visible

// --- Shared helpers -----------------------------------------------------------------------------------------------

uint NrdConfidenceHash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7FEB352Du;
    value ^= value >> 15u;
    value *= 0x846CA68Bu;
    value ^= value >> 16u;
    return value;
}

float NrdConfidenceUnorm(inout uint state)
{
    state = NrdConfidenceHash(state + 0x9E3779B9u);
    return float(state >> 8u) * (1.0f / 16777216.0f);
}

float2 NrdConfidenceOctWrap(float2 value)
{
    return (1.0f - abs(value.yx)) * select(value >= 0.0f, 1.0f, -1.0f);
}

uint NrdConfidencePackNormal(float3 normal)
{
    normal /= abs(normal.x) + abs(normal.y) + abs(normal.z);
    float2 encoded = normal.z >= 0.0f ? normal.xy : NrdConfidenceOctWrap(normal.xy);
    uint2 quantized = uint2(round(saturate(encoded * 0.5f + 0.5f) * 65535.0f));
    return quantized.x | (quantized.y << 16u);
}

float3 NrdConfidenceUnpackNormal(uint packed)
{
    float2 encoded = float2(packed & 0xFFFFu, packed >> 16u) / 65535.0f * 2.0f - 1.0f;
    float3 normal = float3(encoded, 1.0f - abs(encoded.x) - abs(encoded.y));
    float t = saturate(-normal.z);
    normal.xy += select(normal.xy >= 0.0f, -t, t);
    return normalize(normal);
}

// The geometry texture's w: a 12 + 12 bit octahedral normal plus 1, an integer below 2^24 and so exact in a float (a raw
// asfloat of the 32-bit code could be a NaN the hardware does not preserve). 0 means "no surface".
float NrdConfidenceGeometryNormal(uint packed)
{
    uint x = (packed & 0xFFFFu) >> 4u;
    uint y = (packed >> 16u) >> 4u;
    return float((x | (y << 12u)) + 1u);
}

float3 NrdConfidenceGeometryUnpackNormal(float code)
{
    uint value = uint(code) - 1u;
    uint x = value & 0xFFFu;
    uint y = value >> 12u;
    return NrdConfidenceUnpackNormal(((x << 4u) | 8u) | (((y << 4u) | 8u) << 16u));
}

// --- CSProbe --------------------------------------------------------------------------------------------------------

#ifdef NRD_CONFIDENCE_PROBE

// Must match PathTraceReference.hlsl PathTraceOffsetRayOrigin (Wachter and Binder, Ray Tracing Gems ch. 6).
float3 NrdConfidenceOffsetRayOrigin(float3 position, float3 normal)
{
    const float originThreshold = 1.0f / 32.0f;
    const float floatScale = 1.0f / 65536.0f;
    const float integerScale = 256.0f;

    int3 integerOffset = int3(integerScale * normal);
    float3 integerOffsetPosition = asfloat(asint(position) + select(position < 0.0f, -integerOffset, integerOffset));
    return select(abs(position) < originThreshold, position + floatScale * normal, integerOffsetPosition);
}

// Must match PathTraceReference.hlsl PathTraceVisible on its RayQuery path: every candidate hit occludes.
bool NrdConfidenceVisible(float3 position, float3 geometricNormal, float3 direction, float distance)
{
    RayDesc ray;
    ray.Origin = NrdConfidenceOffsetRayOrigin(position, geometricNormal);
    ray.Direction = direction;
    ray.TMin = 0.0f;
    ray.TMax = distance >= PATH_TRACE_DISTANT_LIGHT_DISTANCE ? PATH_TRACE_DISTANT_LIGHT_DISTANCE :
        distance * (1.0f - 1.0e-4f);

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);

    while(query.Proceed())
    {
        if(query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            query.CommitNonOpaqueTriangleHit();
        }
    }

    return query.CommittedStatus() == COMMITTED_NOTHING;
}

PathTraceSun NrdConfidenceSun()
{
    PathTraceSun sun;
    sun.directionToSun = probeConstants.sunDirectionToLight;
    sun.cosAngularRadius = probeConstants.sunCosAngularRadius;
    sun.irradiance = probeConstants.sunIrradiance;
    return sun;
}

PathTraceEnvironment NrdConfidenceEnvironment()
{
    PathTraceEnvironment environment;
    environment.textureIndex = probeConstants.environmentTextureIndex;
    environment.conditionalCDFIndex = probeConstants.environmentConditionalCDFIndex;
    environment.marginalCDFIndex = probeConstants.environmentMarginalCDFIndex;
    environment.width = probeConstants.environmentWidth;
    environment.height = probeConstants.environmentHeight;
    environment.totalWeight = probeConstants.environmentTotalWeight;
    return environment;
}

// The RTXDI light lightIndex sampled at uv, as RtxdiApplicationBridge.hlsli RAB_LoadLightInfo and
// RAB_SamplePolymorphicLight do. Invalid for an index outside this frame's light set.
PathTraceLightSample NrdConfidenceSampleLight(uint lightIndex, float3 position, float2 uv)
{
    PathTraceLightSample invalid = (PathTraceLightSample)0;

    if(lightIndex >= rtxdiCurrentLightCount)
    {
        return invalid;
    }

    uint4 pair = rtxdiLightRecords[lightIndex >> 1u];
    bool second = (lightIndex & 1u) != 0u;

    RtxdiLightRecord record;
    record.kind = second ? pair.z : pair.x;
    record.sourceIndex = second ? pair.w : pair.y;

    GPULight tableLight = (GPULight)0;

    if(record.kind <= RTXDI_LIGHT_KIND_DIRECTIONAL)
    {
        if(record.sourceIndex >= min(sceneLightCount, SCENE_LIGHT_CAPACITY))
        {
            return invalid;
        }

        tableLight = sceneLights[record.sourceIndex];
    }

    return RtxdiSampleLight(record, tableLight, NrdConfidenceSun(), NrdConfidenceEnvironment(), environmentSampler,
        position, uv);
}

// T: the unshadowed irradiance-weighted contribution, luminance(Le) cos / pdf (cos E for delta lights). The diffuse
// BSDF's albedo / pi is a per-surface constant and is left out; zero below either normal or for a non-finite sample.
float NrdConfidenceContribution(PathTraceLightSample lightSample, float3 shadingNormal, float3 geometricNormal)
{
    if(!lightSample.valid || !all(isfinite(lightSample.direction)) || !(lightSample.distance > 0.0f))
    {
        return 0.0f;
    }

    float cosine = dot(shadingNormal, lightSample.direction);

    if(!(cosine > 0.0f) || !(dot(geometricNormal, lightSample.direction) > 0.0f))
    {
        return 0.0f;
    }

    float contribution = dot(max(lightSample.radiance, 0.0f), float3(0.2126f, 0.7152f, 0.0722f)) * cosine;

    if(lightSample.pdf > 0.0f)
    {
        contribution /= lightSample.pdf;
    }

    return isfinite(contribution) ? max(contribution, 0.0f) : 0.0f;
}

void NrdConfidenceWriteInvalid(RWByteAddressBuffer records, uint address)
{
    records.Store4(address, uint4(0u, 0u, 0u, 0u));
    records.Store4(address + 16u, uint4(0u, 0u, 0u, 0u));
}

[numthreads(8, 8, 1)]
void CSProbe(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 probe = dispatchThreadID.xy;

    if(any(probe >= probeConstants.probeResolution))
    {
        return;
    }

    RWByteAddressBuffer records = ResourceDescriptorHeap[probeConstants.recordBufferIndex];
    RWTexture2D<float2> signal = ResourceDescriptorHeap[probeConstants.signalIndex];
    RWTexture2D<float4> geometry = ResourceDescriptorHeap[probeConstants.geometryIndex];
    uint address = (probe.y * probeConstants.probeResolution.x + probe.x) * NRD_CONFIDENCE_RECORD_BYTES;

    // 1. The previous frame's experiment, traced again through this frame's TLAS.
    float2 change = float2(0.0f, 0.0f);
    float4 recordGeometry = float4(0.0f, 0.0f, 0.0f, 0.0f);
    uint probeState = NRD_CONFIDENCE_STATE_INVALID;

    if(probeConstants.previousValid != 0u)
    {
        uint4 record0 = records.Load4(address);
        uint4 record1 = records.Load4(address + 16u);

        if((record1.x & NRD_CONFIDENCE_VALID) != 0u)
        {
            float3 position = asfloat(record0.xyz);
            float3 geometricNormal = NrdConfidenceUnpackNormal(record0.w);
            // A record without a surface stays (0, 0, 0, 0): w = 0 (no geometry for the filter either).
            recordGeometry = float4(position, NrdConfidenceGeometryNormal(record0.w));

            uint lightIndex = record1.x & NRD_CONFIDENCE_LIGHT_MASK;
            float2 uv = float2(record1.y & 0xFFFFu, record1.y >> 16u) / 65535.0f;
            float contribution = asfloat(record1.z);
            float previousVisibility = (record1.x & NRD_CONFIDENCE_VISIBLE) != 0u ? 1.0f : 0.0f;
            PathTraceLightSample lightSample = NrdConfidenceSampleLight(lightIndex, position, uv);

            if(contribution > 0.0f && isfinite(contribution) && all(isfinite(position)) && lightSample.valid &&
                all(isfinite(lightSample.direction)) && lightSample.distance > 0.0f)
            {
                float visibility = NrdConfidenceVisible(position, geometricNormal, lightSample.direction,
                    lightSample.distance) ? 1.0f : 0.0f;
                change = float2(contribution * abs(visibility - previousVisibility), contribution);

                // The stored record's light kind (the same record lookup NrdConfidenceSampleLight made).
                uint4 pair = rtxdiLightRecords[lightIndex >> 1u];
                uint kind = (lightIndex & 1u) != 0u ? pair.z : pair.x;

                if(visibility < previousVisibility)
                {
                    probeState = NRD_CONFIDENCE_STATE_OCCLUDED;
                }
                else if(visibility > previousVisibility)
                {
                    probeState = NRD_CONFIDENCE_STATE_REVEALED;
                }
                else
                {
                    probeState = kind == RTXDI_LIGHT_KIND_SUN ? NRD_CONFIDENCE_STATE_SUN :
                        kind == RTXDI_LIGHT_KIND_ENVIRONMENT ? NRD_CONFIDENCE_STATE_ENVIRONMENT :
                        NRD_CONFIDENCE_STATE_TABLE_LIGHT;
                }
            }
        }
    }

    signal[probe] = change;
    geometry[probe] = recordGeometry;
    RWTexture2D<uint> stateOutput = ResourceDescriptorHeap[probeConstants.stateIndex];
    stateOutput[probe] = probeState;

    // 2. This frame's experiment for block b, read by the next frame.
    uint2 offset = uint2(probeConstants.frameIndex & 1u, (probeConstants.frameIndex >> 1u) & 1u);
    uint2 pixel = min(probe * 2u + offset, probeConstants.resolution - 1u);

    Texture2D<float> depthTexture = ResourceDescriptorHeap[probeConstants.depthIndex];
    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[probeConstants.surfaceIndex];
    Texture2D<float4> geometricNormalTexture = ResourceDescriptorHeap[probeConstants.geometricNormalIndex];
    float depth = depthTexture.Load(int3(pixel, 0));

    if(!isfinite(depth) || depth <= 0.0f || depth >= 1.0f || rtxdiCurrentLightCount == 0u)
    {
        NrdConfidenceWriteInvalid(records, address);
        return;
    }

    // As NrdPathTracePrepare.hlsl: the jittered render inverse puts the pixel centre on the rasterized surface.
    float2 pixelUv = (float2(pixel) + 0.5f) / float2(probeConstants.resolution);
    float4 world = mul(probeConstants.inverseViewProjection, float4(pixelUv.x * 2.0f - 1.0f,
        1.0f - pixelUv.y * 2.0f, depth, 1.0f));
    float3 shadingData = surfaceTexture.Load(int3(pixel, 0)).xyz;
    float3 geometricData = geometricNormalTexture.Load(int3(pixel, 0)).xyz;

    if(!all(isfinite(world)) || abs(world.w) < 1.0e-8f || !all(isfinite(shadingData)) || !all(isfinite(geometricData)) ||
        dot(shadingData, shadingData) < 1.0e-8f || dot(geometricData, geometricData) < 1.0e-8f)
    {
        NrdConfidenceWriteInvalid(records, address);
        return;
    }

    float3 position = world.xyz / world.w;
    float3 toCamera = probeConstants.cameraPosition - position;
    float3 shadingNormal = normalize(shadingData);
    float3 geometricNormal = normalize(geometricData);

    // Both normals face the viewer, as PathTraceFinalizeSurface makes them.
    if(dot(geometricNormal, toCamera) < 0.0f)
    {
        geometricNormal = -geometricNormal;
        shadingNormal = -shadingNormal;
    }

    // 4-candidate RIS on T: probes favour lights that matter here. The chosen uv is quantized before it is sampled, so
    // the stored record reproduces the traced sample exactly.
    uint state = NrdConfidenceHash(probe.x + NrdConfidenceHash(probe.y + NrdConfidenceHash(probeConstants.frameIndex)));
    uint chosenLight = 0u;
    uint chosenUv = 0u;
    float chosenContribution = 0.0f;
    PathTraceLightSample chosenSample = (PathTraceLightSample)0;
    float weightSum = 0.0f;

    for(uint candidate = 0u; candidate < NRD_CONFIDENCE_CANDIDATES; ++candidate)
    {
        uint lightIndex = min(uint(NrdConfidenceUnorm(state) * float(rtxdiCurrentLightCount)),
            rtxdiCurrentLightCount - 1u);
        uint2 quantizedUv = uint2(NrdConfidenceUnorm(state) * 65535.0f, NrdConfidenceUnorm(state) * 65535.0f);
        PathTraceLightSample lightSample = NrdConfidenceSampleLight(lightIndex, position, float2(quantizedUv) / 65535.0f);
        float contribution = NrdConfidenceContribution(lightSample, shadingNormal, geometricNormal);
        weightSum += contribution;

        if(contribution > 0.0f && NrdConfidenceUnorm(state) * weightSum < contribution)
        {
            chosenLight = lightIndex;
            chosenUv = quantizedUv.x | (quantizedUv.y << 16u);
            chosenContribution = contribution;
            chosenSample = lightSample;
        }
    }

    if(!(chosenContribution > 0.0f) || !isfinite(weightSum))
    {
        NrdConfidenceWriteInvalid(records, address);
        return;
    }

    bool visible = NrdConfidenceVisible(position, geometricNormal, chosenSample.direction, chosenSample.distance);
    uint flags = NRD_CONFIDENCE_VALID | (visible ? NRD_CONFIDENCE_VISIBLE : 0u) | chosenLight;
    records.Store4(address, uint4(asuint(position), NrdConfidencePackNormal(geometricNormal)));
    records.Store4(address + 16u, uint4(flags, chosenUv, asuint(chosenContribution), 0u));
}

#endif

// --- CSFilter -------------------------------------------------------------------------------------------------------

#ifdef NRD_CONFIDENCE_FILTER

struct NrdHistoryConfidenceFilterConstants
{
    uint2 probeResolution;
    uint inputIndex;
    uint outputIndex;

    uint geometryIndex;
    uint confidenceIndex;
    uint step;
    uint lastPass;

    // The camera the records were made with (the previous frame's): plane distances scale with distance from it.
    float3 recordCameraPosition;
    float sensitivity;

    float minConfidence;
    float planeSensitivity;
    uint confidenceStrength;
    uint padding;
};

ConstantBuffer<NrdHistoryConfidenceFilterConstants> filterConstants : register(b0);

[numthreads(8, 8, 1)]
void CSFilter(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 probe = dispatchThreadID.xy;

    if(any(probe >= filterConstants.probeResolution))
    {
        return;
    }

    Texture2D<float2> input = ResourceDescriptorHeap[filterConstants.inputIndex];
    Texture2D<float4> geometry = ResourceDescriptorHeap[filterConstants.geometryIndex];
    RWTexture2D<float2> output = ResourceDescriptorHeap[filterConstants.outputIndex];
    float4 centerGeometry = geometry.Load(int3(probe, 0));
    float2 center = input.Load(int3(probe, 0));
    float2 sum = center;
    bool hasSurface = centerGeometry.w != 0.0f;

    if(hasSurface)
    {
        float3 centerPosition = centerGeometry.xyz;
        float3 centerNormal = NrdConfidenceGeometryUnpackNormal(centerGeometry.w);
        float planeScale = 1.0f / max(filterConstants.planeSensitivity *
            length(centerPosition - filterConstants.recordCameraPosition), 1.0e-6f);
        float weightSum = 1.0f;

        [unroll]
        for(int y = -2; y <= 2; ++y)
        {
            [unroll]
            for(int x = -2; x <= 2; ++x)
            {
                if(x == 0 && y == 0)
                {
                    continue;
                }

                int2 position = int2(probe) + int2(x, y) * int(filterConstants.step);

                if(any(position < 0) || any(position >= int2(filterConstants.probeResolution)))
                {
                    continue;
                }

                float4 sampleGeometry = geometry.Load(int3(position, 0));

                if(sampleGeometry.w == 0.0f)
                {
                    continue;
                }

                float distance = length(float2(x, y)) * 0.5f;
                float weight = exp(-2.0f * distance * distance);
                weight *= saturate(1.0f - abs(dot(centerNormal, sampleGeometry.xyz - centerPosition)) * planeScale);
                float normalWeight = saturate(dot(centerNormal, NrdConfidenceGeometryUnpackNormal(sampleGeometry.w)));
                weight *= normalWeight * normalWeight;

                sum += input.Load(int3(position, 0)) * weight;
                weightSum += weight;
            }
        }

        sum /= weightSum;
    }

    output[probe] = sum;

    if(filterConstants.lastPass != 0u)
    {
        RWTexture2D<float> confidenceOutput = ResourceDescriptorHeap[filterConstants.confidenceIndex];
        float confidence = 1.0f;

        // No valid measurement (sum.y == 0) is no evidence of change.
        if(hasSurface && sum.y > 0.0f && all(isfinite(sum)))
        {
            float change = saturate(sum.x / sum.y);
            float keep = 1.0f - saturate(filterConstants.sensitivity * change);
            confidence = max(filterConstants.minConfidence, keep * keep);
        }

        // R.9F: after the original filtered mapping, in the same previous-frame space. Strength 1 preserves the
        // original arithmetic and skips pow. This texture also supplies View 5 when the diagnostic is active.
        [branch]
        if(filterConstants.confidenceStrength > 1u)
        {
            confidence = max(filterConstants.minConfidence, pow(saturate(confidence),
                float(filterConstants.confidenceStrength)));
        }

        confidenceOutput[probe] = saturate(confidence);
    }
}

#endif
