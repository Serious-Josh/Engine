// M5.1 (ReSTIR PT Enhanced, configuration B): unified direct/indirect initial path sampling.
//
// Compiled only through the ShaderVariantTable row ReSTIRPT/RestirPtInitial.hlsl, which includes PathTraceReference.hlsl
// in compute form (PATH_TRACE_DIRECT_COMPUTE) and then this file. It reuses the path tracer's surfaces, lights, BSDF
// primitives, inline radiance tracing and extraction conventions; no production library changes.
//
// Specification: Documentation/Rendering/ReSTIR_PT_Enhanced_M5_Implementation_Specification.md (sections 3 and 4).
// Per pixel and sample: one path tree from the raster primary x1. Every vertex x_j (j = 1 .. maxPathDepth) does next-event
// estimation with all lobes (RIS over light-tile candidates for finite table lights; the directional table lights, the sun
// and the environment each as their own sample), then samples one lobe and continues. Each NEE endpoint and each
// BSDF-sampled escape is one candidate path; the candidates stream into one reservoir (weighted reservoir sampling).
//
// Domains (the Q-2 normalization, specification section 4.6): a candidate is identified by its length k, its technique
// (NEE or BSDF) and, for NEE, its light class (finite table, one directional table light, sun, environment). Each such
// domain receives exactly one candidate per tree; the MIS weights between NEE and BSDF techniques of the same length are
// inside F. GRIS with one canonical candidate per disjoint domain gives W = sum(w) / p_hat(Y) with no 1/M factor, so
// F(Y) W is an unbiased estimate of sum_i F_i / Q_i (Q_i: the candidate's Russian-roulette survival probability).
// RESTIR_PT_FLAG_NORMALIZATION_ONE_OVER_M divides by the streamed candidate count instead, for the Q-2 gate only.

#include "../LightSampling.hlsli"

cbuffer RestirPtConstants : register(b2)
{
    // Raw UAV of the reservoirs (RESTIR_PT_RESERVOIR_BYTES per pixel), 0xFFFFFFFF when none is stored.
    uint restirPtReservoirIndex;
    // Raw SRV of the light tiles: tileCount * tileSize entries of {light index, realized probability bits}.
    uint restirPtLightTileIndex;
    // The light-sampling buffer published this frame (its directional table lights are enumerated from it).
    uint restirPtLightSamplingIndex;
    uint restirPtFlags;

    uint restirPtTileCount;
    uint restirPtTileSize;
    // RIS candidates at the primary hit; bounce j draws max(1, floor(primary / j^2)) (specification Q-4).
    uint restirPtPrimaryCandidates;
    // Paper vertex index from which Russian roulette follows a continuation (0: never).
    uint restirPtRouletteFromVertex;

    uint restirPtFrameIndex;
    // Raw UAV of RESTIR_PT_COUNTER_* 32-bit counters, 0xFFFFFFFF when none.
    uint restirPtCounterIndex;
    uint restirPtDebugView;
    uint restirPtTableLightCount;

    // M5.1-4: the original reconnection criteria [P 14]: minimum distance (2% of the smallest scene dimension) and
    // alpha_min; the per-instance transforms' count (t1).
    float restirPtReconnectionMinDistance;
    float restirPtReconnectionRoughness;
    uint restirPtTransformCount;
    // M5.5 temporal correctness (Stage 1): the history scene's TLAS descriptor (RESTIR_PT_FLAG_HISTORY_SCENE).
    uint restirPtHistorySceneIndex;

    // M5.2 temporal reuse: the previous frame's reservoirs (raw UAV; 0xFFFFFFFF without history), whether history may be
    // read, the G-buffer motion texture (current UV - previous UV, previous - current view depth), the confidence cap.
    uint restirPtPreviousReservoirIndex;
    uint restirPtHistoryValid;
    uint restirPtMotionIndex;
    uint restirPtTemporalConfidenceCap;

    // The previous frame's G-buffer (valid only with history).
    uint restirPtPreviousDepthIndex;
    uint restirPtPreviousSurfaceIndex;
    uint restirPtPreviousGeometricNormalIndex;
    uint restirPtPreviousMaterialIndex;

    // The previous camera (position, forward) and the current camera's forward: linear depth = dot(forward, p - camera).
    float3 restirPtPreviousCameraPosition;
    // M5.4 C5: the replay list (word 0 the count, then packed pixels) and which temporal-pass dispatch this is (0 all
    // pixels, deferring replays; 1 the listed pixels).
    uint restirPtReplayListIndex;
    float3 restirPtPreviousCameraForward;
    uint restirPtReplayPass;
    float3 restirPtCameraForward;
    // Optimization experiment B1a: the stagnancy maps (this frame's smoothed map, written; last frame's, read).
    uint restirPtStagnancyOutputIndex;

    row_major float4x4 restirPtPreviousInverseCameraRelativeViewProjection;

    // M5.3 spatial reuse: the initial/temporal kernel's reservoirs (both spatial passes read them), the final reservoirs
    // (the resample pass writes them), the pair records, the pairing textures, and this frame's pairing transforms
    // (RestirPtPairingPartner).
    uint restirPtSpatialScratchIndex;
    uint restirPtSpatialOutputIndex;
    uint restirPtPairsIndex;
    uint restirPtPairingIndex;
    uint4 restirPtPairingTransform[3];

    // M5.4 C6 dual motion vectors [P 11; Zeng et al. 2021, Eq. 6]: the current camera-relative view-projection (to project
    // the occluder's moved point), the G-buffer raster identity (InstanceID + 1 per pixel), and ReSTIR PT's own instance
    // history: last frame's (read by the temporal lookup) and this frame's (written by the initial kernel), ping-ponged with
    // the reservoir sets so an abandoned frame never advances it.
    row_major float4x4 restirPtCameraRelativeViewProjection;
    uint restirPtIdentityIndex;
    uint restirPtPreviousInstanceIndex;
    uint restirPtInstanceOutputIndex;
    // M5.5-B experiment (off at 0): final-shading decorrelation probability (RTXDI "Uniform" mode).
    float restirPtDecorrelation;

    // M5.4 C7 duplication map [P 9-10, section 5; spec section 6.6]: this frame's map (written after the final reservoirs)
    // and last frame's (read by temporal reuse), ping-ponged with the reservoir sets.
    uint restirPtDuplicationOutputIndex;
    uint restirPtPreviousDuplicationIndex;
    // Optimization experiment A5: the spatial partner count (1-3; RESTIR_PT_SPATIAL_NEIGHBOURS pairing textures exist).
    uint restirPtSpatialPairCount;
    uint restirPtStagnancyPreviousIndex;
};

// M5.5-B experiment: final-shading decorrelation (NVIDIA RTXDI RTXDI_PTApplyDecorrelation, "Uniform" mode only). True with
// the constant probability restirPtDecorrelation, from a hash stream of its own (pixel, frame), so the decision is
// independent of every reservoir and of both estimates. The caller then shows the initial kernel's estimate (the extraction
// textures, which the last pass reads before writing). Always false at 0: the outputs are unchanged.
bool RestirPtDecorrelate(uint2 pixel, float probability)
{
    if(!(probability > 0.0f))
    {
        return false;
    }

    const uint hash = PathTraceHash(pixel.x ^ PathTraceHash(pixel.y ^ PathTraceHash(restirPtFrameIndex ^ 0x44434F52u)));
    return float(hash >> 8u) * (1.0f / 16777216.0f) < probability;
}

// M5.1-4 (M5.2 prerequisite): per-instance transforms by TLAS InstanceID (ReSTIRPTInstanceTransform): the first three rows
// of the model matrix (world = M p) and of its inverse.
struct RestirPtInstanceTransform
{
    row_major float3x4 objectToWorld;
    row_major float3x4 worldToObject;
    // M5.4 C6: the previous frame's world space to this frame's for points on this instance (exactly the identity for an
    // unmoved instance).
    row_major float3x4 previousToCurrent;
    // M5.5 Stage 1: the previous frame's model matrix and its inverse (the history scene's instance transforms).
    row_major float3x4 previousObjectToWorld;
    row_major float3x4 previousWorldToObject;
};

StructuredBuffer<RestirPtInstanceTransform> restirPtTransforms : register(t1);

// M5.5 temporal correctness, Stage 1: shifts into the temporal domain (toPrevious) are evaluated in the scene that domain's
// samples were drawn from. GRIS needs p_t(T^-1 x) = 0 wherever domain t could not have produced the path; the current
// scene gives it weight for paths a moved occluder blocked last frame, which under-counts newly unoccluded light. With
// RESTIR_PT_FLAG_HISTORY_SCENE, every ray of such a shift is traced against the history TLAS (the committed frame's
// instances at their transforms over the same BLASes) and x_k is rebuilt with the previous transforms.
static bool g_restirPtHistoryScene = false;

#define RESTIR_PT_TRACE_INLINE(query, flags, ray) \
    if(g_restirPtHistoryScene) \
    { \
        RaytracingAccelerationStructure historyScene = ResourceDescriptorHeap[restirPtHistorySceneIndex]; \
        query.TraceRayInline(historyScene, flags, 0xFF, ray); \
    } \
    else \
    { \
        query.TraceRayInline(scene, flags, 0xFF, ray); \
    }

RestirPtInstanceTransform RestirPtLoadInstanceTransform(uint instanceID)
{
    RestirPtInstanceTransform transform = restirPtTransforms[instanceID];

    if(g_restirPtHistoryScene)
    {
        transform.objectToWorld = transform.previousObjectToWorld;
        transform.worldToObject = transform.previousWorldToObject;
    }

    return transform;
}

static const uint RESTIR_PT_FLAG_EXTRACT = 1u << 0;
static const uint RESTIR_PT_FLAG_RAW_DIFFUSE_HIT_DISTANCE = 1u << 1;
// M5.1-2: RIS over primaryCandidates / j^2 light-tile candidates; off: one candidate (plain light-tile sampling).
static const uint RESTIR_PT_FLAG_RIS = 1u << 2;
// M5.1-3: one candidate per tree selected into the reservoir; off: every candidate accumulated (plain path tracing).
static const uint RESTIR_PT_FLAG_RESERVOIR = 1u << 3;
// Q-2 gate only: W = sum(w) / (p_hat(Y) M) with M the streamed candidate count (the original code's finalizeRIS).
static const uint RESTIR_PT_FLAG_NORMALIZATION_ONE_OVER_M = 1u << 4;
// Gate isolation: restrict the image to direct (k <= 2) or indirect (k >= 3) candidates.
static const uint RESTIR_PT_FLAG_DIRECT_ONLY = 1u << 5;
static const uint RESTIR_PT_FLAG_INDIRECT_ONLY = 1u << 6;
// M5.1 T-1.8 test only: NaN injected at RestirPtInjectedPixel, into the x1 table-light NEE candidate or each sample's result.
static const uint RESTIR_PT_FLAG_INJECT_CANDIDATE_NAN = 1u << 7;
static const uint RESTIR_PT_FLAG_INJECT_RESULT_NAN = 1u << 8;
// M5.1-6 F3 reconstruction experiment (not the published estimator's output): the reservoir is selected and stored exactly
// as without it, but the image and the denoisers' signals receive the path tree's full sum (every candidate / survival).
static const uint RESTIR_PT_FLAG_OUTPUT_SUM = 1u << 9;
// M5.1-4 T-2.1 test only: shift each selected sample into its own pixel and count identities.
static const uint RESTIR_PT_FLAG_SELF_SHIFT = 1u << 10;
// T-2.1 diagnostic: shift from the in-register (unquantized) reconnection data instead of the stored reservoir words.
static const uint RESTIR_PT_FLAG_SELF_SHIFT_UNQUANTIZED = 1u << 11;
// T-2.1 round 3 quantization decomposition (stored-word runs): use the in-register rcWi, or the in-register barycentrics,
// in place of the stored field (everything else from the stored words).
static const uint RESTIR_PT_FLAG_SHIFT_EXACT_DIRECTION = 1u << 13;
static const uint RESTIR_PT_FLAG_SHIFT_EXACT_BARYCENTRICS = 1u << 14;
// M5.2 C0 performance baseline only: one reconnection-type visibility ray per sample, from x1 to the selected sample's hit
// reconnection vertex, excluding its own triangle (as shifts trace it) or, with the second flag, as a plain opaque ray.
static const uint RESTIR_PT_FLAG_VISIBILITY_BENCH = 1u << 15;
static const uint RESTIR_PT_FLAG_VISIBILITY_BENCH_PLAIN = 1u << 16;
// M5.2: temporal reuse (sample 0 resamples with the previous frame's reservoir when history is valid).
static const uint RESTIR_PT_FLAG_TEMPORAL = 1u << 17;
// M5.3 spatial reuse: the initial kernel leaves the image to the resample pass (1u << 19), colour-noise reduction in the
// resample pass (1u << 20), and the pre-pass round trip (diagnostics variant only, 1u << 21).
static const uint RESTIR_PT_FLAG_SPATIAL = 1u << 19;
static const uint RESTIR_PT_FLAG_SPATIAL_COLOUR = 1u << 20;
static const uint RESTIR_PT_FLAG_SPATIAL_ROUND_TRIP = 1u << 21;
// M5.4 C1: temporal reuse runs as its own pass (RestirPtTemporalCS) after the initial kernel, which then stores the
// canonical reservoir and leaves the image to the temporal (or spatial resample) pass. Set only at one sample per frame.
static const uint RESTIR_PT_FLAG_TEMPORAL_PASS = 1u << 22;
// M5.4 C4: the published reconnection criteria (Eq. 5 dual ray footprint + single-vertex roughness [P 7-8; S 2-5]) in
// place of the original ones (d_min and the roughness of both vertices [P 6]); and, test only, the A' comparison rule for
// NEE leaving x_k (the inverse footprint with the BSDF density toward the light, a proxy; the default skips it).
static const uint RESTIR_PT_FLAG_FOOTPRINT_CRITERIA = 1u << 23;
static const uint RESTIR_PT_FLAG_NEE_INVERSE_PROXY = 1u << 24;
// M5.4 C5: replay compaction in the temporal pass [P 10-11, section 6.2.2]: pixels whose shifts need a traced replay are
// deferred to a second dispatch over a compacted list.
static const uint RESTIR_PT_FLAG_REPLAY_COMPACTION = 1u << 25;
// M5.4 C6: dual motion vectors in the temporal lookup.
static const uint RESTIR_PT_FLAG_DUAL_MOTION = 1u << 26;
// M5.4 C6 counters (8 words from 14192): dual lookups attempted (the primary failed at stage 2 or 3 with dual motion
// on), with an occluder instance, inside the view, with a previous surface, accepted.
static const uint RESTIR_PT_COUNTER_DUAL = 14192u;
// M5.4 C7: the duplication map is computed (measure) and, separately, applied to temporal reuse's confidence cap
// (biased, [P 10]: it breaks the MIS weights' partition of unity where it acts). Off by default.
static const uint RESTIR_PT_FLAG_DUPLICATION_MAP = 1u << 27;
static const uint RESTIR_PT_FLAG_DUPLICATION_APPLY = 1u << 28;
// M5.5 Stage 1: shifts into the temporal domain use the history scene (restirPtHistorySceneIndex).
static const uint RESTIR_PT_FLAG_HISTORY_SCENE = 1u << 29;
// Optimization experiment B1a: stagnancy-driven decorrelation (NVIDIA RTXDI "Stagnancy" mode) in place of Uniform.
static const uint RESTIR_PT_FLAG_STAGNANCY = 1u << 30;
// Optimization experiment B1b: firefly replacement (NVIDIA RTXDI boiling filter forcing the decorrelation swap).
static const uint RESTIR_PT_FLAG_FIREFLY = 1u << 31;
// [P 9]: c_Cap = lerp(c_Cap_default, c_Cap_min, D^alpha) with c_Cap_min = 1 and alpha = 0.1; D = duplicates / 288 in 17 x 17.
static const float RESTIR_PT_DUPLICATION_MIN_CAP = 1.0f;
static const float RESTIR_PT_DUPLICATION_ALPHA = 0.1f;
// M5.4 C7 counters (8 words from 14200): pixels with a sample, the sum of their duplicate counts, and D in bins
// 0, (0, 0.01], (0.01, 0.05], (0.05, 0.2], (0.2, 0.5], > 0.5.
static const uint RESTIR_PT_COUNTER_DUPLICATION = 14200u;
// Eq. 5's constant c [P 7].
static const float RESTIR_PT_FOOTPRINT_C = 0.02f;
// M5.4 C4 counters (16 words from 14176, counters on): capture's decisions [0 forward evaluated, 1 forward passed,
// 2 inverse evaluated (BSDF continuations and escapes leaving x_k), 3 inverse passed, 4 NEE-at-x_k candidates,
// 5 A' proxy evaluated, 6 A' proxy passed], 7 purely diffuse skips, and the forward test's proximity to the threshold
// |log10(footprint / threshold)| in bins < 0.01, < 0.03, < 0.1, < 0.3, < 1, >= 1 (8-13).
static const uint RESTIR_PT_COUNTER_FOOTPRINT = 14176u;
static const uint RESTIR_PT_SPATIAL_NEIGHBOURS = 3u;
static const uint RESTIR_PT_PAIR_BYTES = 32u;
// M5.3 counters (14160-14175): RestirPtSpatialPairsCS 0-7, RestirPtSpatialResampleCS 8-12.
static const uint RESTIR_PT_COUNTER_SPATIAL = 14160u;
// The temporal code is compiled only into the temporal kernel (ReSTIRPT/RestirPtInitialTemporal.hlsl) and the diagnostics
// variant; production with temporal reuse off carries none of it (M5.2 C4: it slowed initial sampling about 15%).
#if defined(RESTIR_PT_TEMPORAL) || defined(RESTIR_PT_DIAGNOSTICS)
#define RESTIR_PT_TEMPORAL_KERNEL 1
#endif
// M5.2 C4 test only (diagnostics variant): the temporal round-trip check (counters 14144-14159).
static const uint RESTIR_PT_FLAG_TEMPORAL_ROUND_TRIP = 1u << 18;

// Reservoir pathFlags (specification section 7.1).
static const uint RESTIR_PT_PATH_RC_INDEX_SHIFT = 4u;
static const uint RESTIR_PT_PATH_NEE = 1u << 16;
static const uint RESTIR_PT_PATH_PREVIOUS_DELTA = 1u << 17;
static const uint RESTIR_PT_PATH_LIGHT_TYPE_SHIFT = 18u;
static const uint RESTIR_PT_PATH_LOBES_SHIFT = 20u;
static const uint RESTIR_PT_PATH_REPLAY_ONLY = 1u << 24;
// lambda_k, the lobe x_k's continuation sampled (bits 25-26; T-2.1 P1).
static const uint RESTIR_PT_PATH_VERTEX_LOBE_SHIFT = 25u;
// Optimization experiment B1a: the sample's age in frames (bits 27-31, saturating at 31): 0 for a fresh sample, +1 each
// time temporal reuse keeps it, carried when spatial reuse adopts it (RTXDI's reservoir age). Updated only in stagnancy
// mode; no shift, weight or MIS reads it.
static const uint RESTIR_PT_PATH_AGE_SHIFT = 27u;

uint RestirPtPathAge(uint pathFlags)
{
    return pathFlags >> RESTIR_PT_PATH_AGE_SHIFT;
}

uint RestirPtFlagsAged(uint pathFlags)
{
    return (pathFlags & ~(31u << RESTIR_PT_PATH_AGE_SHIFT)) |
        (min(RestirPtPathAge(pathFlags) + 1u, 31u) << RESTIR_PT_PATH_AGE_SHIFT);
}

// The final-shading decorrelation probability (call once per active pixel in the final pass). Uniform: the constant
// restirPtDecorrelation. Stagnancy (RTXDI_PTApplyDecorrelation, RTXDI_PTComputeSmoothedDuplicationMap): the selected
// sample's age / 40, smoothed by an EMA (0.2) against a 5 x 5 (step 2) average of last frame's smoothed map at the
// reprojected pixel; p = saturate(4 * factor * smoothed^0.5) with factor = restirPtDecorrelation (RTXDI defaults: 0.4,
// exponent 0.5, EMA 0.2). The smoothed map is written here. Biased (age correlates with the sample; smoothing reduces it).
float RestirPtDecorrelationProbability(uint2 pixel, uint selectedPathFlags)
{
    if((restirPtFlags & RESTIR_PT_FLAG_STAGNANCY) == 0u || restirPtStagnancyOutputIndex == 0xFFFFFFFFu)
    {
        return restirPtDecorrelation;
    }

    const float current = min(1.0f, float(RestirPtPathAge(selectedPathFlags)) / 40.0f);
    float smoothed = current;

    if(restirPtHistoryValid != 0u && restirPtStagnancyPreviousIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> motionTexture = ResourceDescriptorHeap[restirPtMotionIndex];
        RWByteAddressBuffer previousMap = ResourceDescriptorHeap[restirPtStagnancyPreviousIndex];
        const float2 motion = motionTexture.Load(int3(pixel, 0)).xy;
        // Engine's motion is currentUV - previousUV.
        const int2 previousPixel = int2(float2(pixel) + 0.5f - motion * float2(resolution));
        float sum = 0.0f;
        float taps = 0.0f;

        [unroll]
        for(int dy = -2; dy <= 2; ++dy)
        {
            [unroll]
            for(int dx = -2; dx <= 2; ++dx)
            {
                const int2 tap = previousPixel + int2(dx, dy) * 2;

                if(all(tap >= 0) && all(tap < int2(resolution)))
                {
                    sum += asfloat(previousMap.Load(4u * (uint(tap.y) * resolution.x + uint(tap.x))));
                    taps += 1.0f;
                }
            }
        }

        smoothed = taps > 0.0f ? 0.2f * current + 0.8f * (sum / taps) : current;
    }

    RWByteAddressBuffer outputMap = ResourceDescriptorHeap[restirPtStagnancyOutputIndex];
    outputMap.Store(4u * (pixel.y * resolution.x + pixel.x), asuint(smoothed));
    return saturate(4.0f * restirPtDecorrelation * sqrt(saturate(smoothed)));
}
// lightType (bits 18-19): the end vertex's emitter class.
static const uint RESTIR_PT_LIGHT_ENVIRONMENT = 0u;
static const uint RESTIR_PT_LIGHT_SUN = 1u;
static const uint RESTIR_PT_LIGHT_ANALYTIC = 2u;
// A table light endpoint rebuilt from its index (T-2.1 P3; point, spot and directional table lights, D5): the instance
// word is its light index, rcRadiance holds 1 / p_1 (finite lights) or 1 (directional) with the light's incident radiance
// factored out, and a shift re-evaluates direction, attenuation and visibility at its own y_(k-1).
static const uint RESTIR_PT_LIGHT_TABLE = 3u;

// T-2.1 counters (RESTIR_PT_COUNTER_*): attempted shifts, identities within tolerance, replay or identity mismatches,
// replay-only selections, the largest relative |F' - F| / |F| (float bits), the largest |J - 1| (float bits).
static const uint RESTIR_PT_COUNTER_SHIFT_ATTEMPTED = 10u;
static const uint RESTIR_PT_COUNTER_SHIFT_IDENTITY = 11u;
static const uint RESTIR_PT_COUNTER_SHIFT_MISMATCH = 12u;
static const uint RESTIR_PT_COUNTER_SHIFT_REPLAY_ONLY = 13u;
static const uint RESTIR_PT_COUNTER_SHIFT_MAX_ERROR = 14u;
static const uint RESTIR_PT_COUNTER_SHIFT_MAX_JACOBIAN_ERROR = 15u;
// T-2.1 diagnostic bins (test only): counter 16 + case * 10 + reason; per case, the largest |J - 1| (56 + case) and
// |F'/F - 1| (60 + case), as float bits. Reasons 1 identity, 2 F mismatch only, 3 J mismatch, 4.. the shift's failures.
static const uint RESTIR_PT_COUNTER_SHIFT_BINS = 16u;
static const uint RESTIR_PT_COUNTER_SHIFT_CASE_MAX_JACOBIAN = 56u;
static const uint RESTIR_PT_COUNTER_SHIFT_CASE_MAX_ERROR = 60u;
static const uint RESTIR_PT_SHIFT_CASE_HIT_CONTINUATION = 0u;
static const uint RESTIR_PT_SHIFT_CASE_HIT_NEE = 1u;
static const uint RESTIR_PT_SHIFT_CASE_LIGHT = 2u;
static const uint RESTIR_PT_SHIFT_CASE_ENVIRONMENT = 3u;
static const uint RESTIR_PT_SHIFT_REASON_SKIPPED = 0u;
static const uint RESTIR_PT_SHIFT_REASON_IDENTITY = 1u;
static const uint RESTIR_PT_SHIFT_REASON_F_MISMATCH = 2u;
static const uint RESTIR_PT_SHIFT_REASON_J_MISMATCH = 3u;
static const uint RESTIR_PT_SHIFT_REASON_VISIBILITY = 4u;
static const uint RESTIR_PT_SHIFT_REASON_PREVIOUS_LOBE = 5u;
static const uint RESTIR_PT_SHIFT_REASON_REBUILD = 6u;
static const uint RESTIR_PT_SHIFT_REASON_VERTEX_DENSITY = 7u;
static const uint RESTIR_PT_SHIFT_REASON_REPLAY = 8u;
static const uint RESTIR_PT_SHIFT_REASON_NON_FINITE = 9u;

// T-2.1 round 3 diagnostics (test only, self-shift runs; ReSTIRPTPass::CounterCount words in all).
// Hit-reconnection geometry histograms: [hit case 2][outcome 5][histogram 4][bin 6]. Histograms: d / d_min, |cos theta_k|
// (x_k's geometric normal), cos theta_(k-1) of the sampled direction (x_(k-1)'s shading normal), and the angle between the
// sampled direction omega and the vertex-to-vertex direction omega'.
static const uint RESTIR_PT_COUNTER_GEOMETRY = 64u;
static const uint RESTIR_PT_GEOMETRY_OUTCOMES = 5u;
static const uint RESTIR_PT_GEOMETRY_HISTOGRAMS = 4u;
static const uint RESTIR_PT_HISTOGRAM_BINS = 6u;
static const uint RESTIR_PT_OUTCOME_IDENTITY = 0u;
static const uint RESTIR_PT_OUTCOME_F_MISMATCH = 1u;
static const uint RESTIR_PT_OUTCOME_J_MISMATCH = 2u;
static const uint RESTIR_PT_OUTCOME_VISIBILITY = 3u;
static const uint RESTIR_PT_OUTCOME_OTHER_FAILURE = 4u;
// Hit-reconnection mechanisms: [hit case 2][outcome 5][mechanism 6]. Mechanisms: a re-selection of x_k's lobe from the
// seed (x_k seen along omega') differs from the stored lambda_k; x_k's two-sided normal flips between omega and omega';
// the shift is an identity when x_(k-1)'s
// lobe density and weight use omega (x_k along omega'), when x_k is seen along omega (x_(k-1) at omega'), or both; the
// x_(k-1) density is 0 at omega' but not at omega.
static const uint RESTIR_PT_COUNTER_MECHANISM = 304u;
static const uint RESTIR_PT_MECHANISMS = 6u;
static const uint RESTIR_PT_MECHANISM_LOBE_FLIP = 0u;
static const uint RESTIR_PT_MECHANISM_NORMAL_FLIP = 1u;
static const uint RESTIR_PT_MECHANISM_SAMPLED_PREVIOUS = 2u;
static const uint RESTIR_PT_MECHANISM_SAMPLED_VERTEX = 3u;
static const uint RESTIR_PT_MECHANISM_SAMPLED_BOTH = 4u;
static const uint RESTIR_PT_MECHANISM_PREVIOUS_DENSITY_LOST = 5u;
// Quantization study: [quantization case 5][outcome 2: identity, not][histogram 5][bin 6]. Cases: hit + continuation,
// hit + NEE, table-light endpoint, distant-light endpoint (sun or environment NEE), environment endpoint (BSDF escape).
// Histograms: the stored rcWi's angular error (0 in unquantized runs), the GGX alpha of the vertex that evaluates toward
// rcWi (bin 5: its sampled lobe is the cosine lobe), that vertex's |cos| toward rcWi, |F' - F| and F (luminance).
static const uint RESTIR_PT_COUNTER_QUANTIZATION = 368u;
static const uint RESTIR_PT_QUANTIZATION_HISTOGRAMS = 5u;
static const uint RESTIR_PT_QUANTIZATION_HIT_CONTINUATION = 0u;
static const uint RESTIR_PT_QUANTIZATION_HIT_NEE = 1u;
static const uint RESTIR_PT_QUANTIZATION_TABLE_LIGHT = 2u;
static const uint RESTIR_PT_QUANTIZATION_DISTANT_LIGHT = 3u;
static const uint RESTIR_PT_QUANTIZATION_ESCAPE = 4u;
// Energy (paired, same samples): [quantization case 5][sum 4][64-bit fixed point, 2 words]. Sums: F W over evaluated
// shifts, (F' - F) W where positive, (F - F') W where positive, F W of failed shifts. Luminance, RESTIR_PT_ENERGY_SCALE.
static const uint RESTIR_PT_COUNTER_ENERGY = 668u;
static const uint RESTIR_PT_ENERGY_SUMS = 4u;
static const float RESTIR_PT_ENERGY_SCALE = 1048576.0f;
// Analytic-light experiment: [light type 3: point, spot, directional][result 4: identity, F mismatch, not visible,
// no incident radiance], then the largest direction error against the in-register direction per type (float bits).
static const uint RESTIR_PT_COUNTER_LIGHT_EXPERIMENT = 708u;
static const uint RESTIR_PT_COUNTER_LIGHT_EXPERIMENT_ANGLE = 720u;
// Hit-reconnection visibility failures whose blocker is x_k's own triangle: [hit case 2].
static const uint RESTIR_PT_COUNTER_VISIBILITY_SELF_HIT = 724u;
// Stored-word runs: hit reconnections whose quantized barycentrics change x_k's material: [hit case 2][identity, not].
static const uint RESTIR_PT_COUNTER_MATERIAL_CHANGED = 726u;
// P1: stored lambda_k differs from the in-register lambda_k.
static const uint RESTIR_PT_COUNTER_STORED_LOBE_MISMATCH = 730u;
// P4: [sun, environment] regenerated count (731-732); regenerated density differs from the stored lightPdf (733); the
// largest angle to the in-register direction per light (734-735, float bits).
static const uint RESTIR_PT_COUNTER_DISTANT_REGENERATED = 731u;
static const uint RESTIR_PT_COUNTER_DISTANT_PDF_MISMATCH = 733u;
static const uint RESTIR_PT_COUNTER_DISTANT_ANGLE = 734u;
// T-2.1b (stored-word runs): the decoded rcWi re-encodes to different bits (quantization not idempotent, 736); per
// quantization case, the largest |F' - F| W (800-804, float bits).
static const uint RESTIR_PT_COUNTER_OCT_REENCODE = 736u;
// T-2.1a: visibility failures that are edge ties: the base path's own ray (offset origin along omega, closest hit) reaches
// x_k, while the shift's ray toward the rebuilt x_k (the same line up to float rounding) meets a blocker whose world-space
// distance from its triangle's nearest edge is within the two rays' separation there: [hit case 2].
static const uint RESTIR_PT_COUNTER_EDGE_TIE = 737u;
// Round 5: hit reconnections whose shift omega' differs in any bit from capture's (739); stored rcWi bits that are not a
// fixed point of 8 decode/re-encode cycles (805); M5.2 C0 visibility bench: rays traced and rays visible (806-807).
static const uint RESTIR_PT_COUNTER_VISIBILITY_BENCH = 806u;
// M5.2 C1 temporal lookup probe (counters on, history valid): pixels with history, reprojected inside the view, previous
// surface valid, compatible (normal and depth tests), previous reservoir holds a sample, and that sample reconnects
// (808-813).
static const uint RESTIR_PT_COUNTER_TEMPORAL_LOOKUP = 808u;
// M5.2 temporal resampling (counters on): pixels resampled, temporal sample selected, forward shift valid, reverse shift
// valid, identity candidates (neighbour = this pixel), identities (814-819).
static const uint RESTIR_PT_COUNTER_TEMPORAL = 814u;
static const uint RESTIR_PT_COUNTER_DIRECTION_BITS = 739u;
static const uint RESTIR_PT_COUNTER_OCT_CYCLE = 805u;
static const uint RESTIR_PT_COUNTER_MAX_WEIGHTED_ERROR = 800u;
// P3 validation, a table light evaluated at a different receiver (the previous sample's light endpoint at this path's
// y_(k-1)): [light type 3: point, spot, directional][8: tested, direction error > 1e-6 rad, incident radiance error
// > 1e-5 (relative to the light's cone- and window-free radiance there), visibility agrees with the reference, leak (visible, reference occluded), false occlusion, reference
// ambiguous, the largest relative radiance error (float bits)].
static const uint RESTIR_PT_COUNTER_LIGHT_CROSS = 776u;
// P2 validation, cross pairs: [method 3: P2 (own triangle excluded), offset endpoint, plain 0.999 d rule][reference 4:
// occluded by another object, visible, ambiguous, occluded by x_k's own mesh][verdict 2: occluded, visible]. A leak is
// an occluded reference with verdict visible.
static const uint RESTIR_PT_COUNTER_CROSS_VISIBILITY = 748u;
// Dumps of non-identical shifts: [shift case 4][pool 2: any, severe][RESTIR_PT_DUMP_SLOTS records of RESTIR_PT_DUMP_WORDS
// words]; severe: max(|F'/F - 1|, |J - 1|) > 1 or a visibility failure. The slots used per case and pool at 740.
static const uint RESTIR_PT_COUNTER_DUMP_USED = 740u;
static const uint RESTIR_PT_COUNTER_DUMPS = 832u;
static const uint RESTIR_PT_DUMP_SLOTS = 32u;
static const uint RESTIR_PT_DUMP_WORDS = 52u;
// A shift is an identity when |F'/F - 1| and |J - 1| are both within this (T-2.1; unchanged in round 3).
static const float RESTIR_PT_SHIFT_TOLERANCE = 1.0e-2f;

bool RestirPtInjectedPixel(uint2 pixel)
{
    return pixel.x % 61u == 7u && pixel.y % 53u == 5u;
}

static const uint RESTIR_PT_LOBE_COSINE = 0u;
static const uint RESTIR_PT_LOBE_GGX = 1u;
static const uint RESTIR_PT_LOBE_DELTA = 2u;

// Light class of a candidate's end vertex (reservoir flags bits 18-20).
static const uint RESTIR_PT_END_TABLE = 0u;
static const uint RESTIR_PT_END_DIRECTIONAL = 1u;
static const uint RESTIR_PT_END_SUN = 2u;
static const uint RESTIR_PT_END_ENVIRONMENT = 3u;
static const uint RESTIR_PT_END_ESCAPE = 4u;
static const uint RESTIR_PT_END_CAMERA = 5u;

static const uint RESTIR_PT_RESERVOIR_BYTES = 64u;

// Counters (32-bit, wrap-around, read as deltas over short windows).
static const uint RESTIR_PT_COUNTER_CONTINUATION_RAYS = 0u;
static const uint RESTIR_PT_COUNTER_SHADOW_RAYS = 1u;
static const uint RESTIR_PT_COUNTER_CANDIDATES = 2u;
static const uint RESTIR_PT_COUNTER_SAMPLES = 3u;
// Selected length k = 1 .. 5 at offsets 4 .. 8.
static const uint RESTIR_PT_COUNTER_SELECTED_LENGTH = 4u;
// Gate validity: per-vertex contributions (NEE sum or escape at x_j, j >= 2) whose luminance exceeds the production path
// tracer's PATH_TRACE_INDIRECT_LUMINANCE_CLAMP. Configuration B has no clamp (the paper's estimator is unbiased); a
// comparison against the clamped production reference is valid only where this stays negligible.
static const uint RESTIR_PT_COUNTER_CLAMP_WOULD_ENGAGE = 9u;

// Random-number dimensions of a vertex (counter-based: any vertex's numbers are addressable for replay).
static const uint RESTIR_PT_DIMENSION_LOBE = 0u;
static const uint RESTIR_PT_DIMENSION_DIRECTION = 1u;
static const uint RESTIR_PT_DIMENSION_ROULETTE = 3u;
static const uint RESTIR_PT_DIMENSION_SUN = 4u;
static const uint RESTIR_PT_DIMENSION_ENVIRONMENT = 6u;
static const uint RESTIR_PT_DIMENSION_RIS = 16u;
static const uint RESTIR_PT_DIMENSION_STREAM = 200u;
// M5.2: the temporal neighbour's reprojection jitter (two dimensions at vertex 0).
static const uint RESTIR_PT_DIMENSION_TEMPORAL = 240u;
// M5.3: the spatial resampling's selection (vertex 0, 248 canonical, 249-251 partners).
static const uint RESTIR_PT_DIMENSION_SPATIAL = 248u;

// -----------------------------------------------------------------------------
// Counter-based random numbers (specification section 4.3)
// -----------------------------------------------------------------------------

uint RestirPtInitialSeed(uint2 pixel, uint sampleIndex)
{
    return PathTraceHash(pixel.x ^ PathTraceHash(pixel.y ^ PathTraceHash(sampleIndex ^ PathTraceHash(randomSeed ^
        0x52505431u))));
}

uint RestirPtRandomUint(uint seed, uint vertex, uint dimension)
{
    return PathTraceHash(seed ^ PathTraceHash((vertex << 12) ^ dimension ^ 0x9E3779B9u));
}

float RestirPtRandom(uint seed, uint vertex, uint dimension)
{
    return float(RestirPtRandomUint(seed, vertex, dimension) >> 8) * (1.0f / 16777216.0f);
}

// -----------------------------------------------------------------------------
// Lobe-indexed BSDF (specification section 4.2). The two sampled lobes partition PathTraceEvaluateBSDFLobes exactly:
// the GGX lobe is the GGX single scatter (VNDF sampling, or the delta mirror on a delta surface); the cosine lobe is the
// multiple scatter plus the diffuse term (cosine sampling). Their selection probabilities are the production ones.
// -----------------------------------------------------------------------------

float RestirPtLobeProbability(PathTraceSurface surface, uint lobe)
{
    return lobe == RESTIR_PT_LOBE_COSINE ? 1.0f - surface.specularProbability : surface.specularProbability;
}

// f_lobe without the cosine; diffuseValue is its EON diffuse part (the extraction's diffuse signal).
float3 RestirPtEvaluateLobe(PathTraceSurface surface, uint lobe, float3 direction, out float3 diffuseValue)
{
    diffuseValue = float3(0.0f, 0.0f, 0.0f);

    if(lobe == RESTIR_PT_LOBE_DELTA || !PathTraceDirectionAboveSurface(surface, direction))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    if(lobe == RESTIR_PT_LOBE_GGX)
    {
        return surface.deltaSpecular ? float3(0.0f, 0.0f, 0.0f) : PathTraceEvaluateGGXSingleScatter(surface, direction);
    }

    const float normalDotLight = dot(surface.normal, direction);
    const float3 multipleScatter = PathTraceEvaluateMultipleScatter(surface, normalDotLight);

    if(surface.material.model == 2u)
    {
        return multipleScatter;
    }

    diffuseValue = PathTraceDiffuseWeight(surface, normalDotLight) * EvaluateEONDiffuse(PathTraceDiffuseAlbedo(surface),
        surface.material.roughness, surface.normal, direction, surface.view);
    return diffuseValue + multipleScatter;
}

// The lobe's own solid-angle density (without its selection probability).
float RestirPtLobePdf(PathTraceSurface surface, uint lobe, float3 direction)
{
    if(lobe == RESTIR_PT_LOBE_COSINE)
    {
        return max(dot(surface.normal, direction), 0.0f) * INV_PI;
    }

    if(lobe == RESTIR_PT_LOBE_GGX && !surface.deltaSpecular)
    {
        return PathTraceVisibleNormalPdf(surface, direction);
    }

    return 0.0f;
}

struct RestirPtLobeSample
{
    bool valid;
    uint lobe;
    float3 direction;
    // f_lobe cos / (P(lobe) p_lobe), or F / P(lobe) for the delta mirror.
    float3 weight;
    // The EON diffuse part of weight.
    float3 diffuseWeight;
    // The all-lobe (mixture) density PathTracePdf, for MIS against light sampling; 0 for the delta mirror.
    float pdfAll;
};

RestirPtLobeSample RestirPtSampleLobe(PathTraceSurface surface, float3 random)
{
    RestirPtLobeSample result;
    result.valid = false;
    result.lobe = RESTIR_PT_LOBE_COSINE;
    result.direction = surface.normal;
    result.weight = float3(0.0f, 0.0f, 0.0f);
    result.diffuseWeight = float3(0.0f, 0.0f, 0.0f);
    result.pdfAll = 0.0f;

    const bool chooseSpecular = random.x < surface.specularProbability;

    if(surface.deltaSpecular && chooseSpecular)
    {
        result.lobe = RESTIR_PT_LOBE_DELTA;
        result.direction = reflect(-surface.view, surface.normal);

        if(dot(surface.geometricNormal, result.direction) <= 0.0f || surface.specularProbability <= 0.0f)
        {
            return result;
        }

        result.weight = surface.deltaFresnel / surface.specularProbability;
        result.valid = all(isfinite(result.weight));
        return result;
    }

    if(chooseSpecular)
    {
        result.lobe = RESTIR_PT_LOBE_GGX;
        result.direction = reflect(-surface.view, PathTraceSampleVisibleNormal(surface, random.yz));
    }
    else
    {
        result.lobe = RESTIR_PT_LOBE_COSINE;
        result.direction = PathTraceSampleCosine(surface, random.yz);
    }

    if(!PathTraceDirectionAboveSurface(surface, result.direction))
    {
        return result;
    }

    const float lobePdf = RestirPtLobeProbability(surface, result.lobe) *
        RestirPtLobePdf(surface, result.lobe, result.direction);

    if(!(lobePdf > 0.0f) || !isfinite(lobePdf))
    {
        return result;
    }

    float3 diffuseValue;
    const float3 value = RestirPtEvaluateLobe(surface, result.lobe, result.direction, diffuseValue);
    const float cosine = dot(surface.normal, result.direction);
    result.weight = value * cosine / lobePdf;
    result.diffuseWeight = diffuseValue * cosine / lobePdf;
    result.pdfAll = PathTracePdf(surface, result.direction);
    result.valid = all(isfinite(result.weight)) && any(result.weight > 0.0f);
    return result;
}

// Balance heuristic of strategy a against b (specification section 4.2, supplemental section 5).
float RestirPtBalanceHeuristic(float pdfA, float pdfB)
{
    return pdfA + pdfB > 0.0f ? pdfA / (pdfA + pdfB) : 0.0f;
}

// -----------------------------------------------------------------------------
// Candidates and the reservoir
// -----------------------------------------------------------------------------

// M5.1-4: a candidate's reconnection data (specification section 4.7, stored as section 7.1).
struct RestirPtReconnection
{
    // Paper vertex k of the reconnection vertex (2 .. length); 0: replay-only.
    uint index;
    // A hit vertex: its TLAS InstanceID, primitive and barycentrics; a table-light endpoint: the light index in instance.
    uint instance;
    uint primitive;
    float2 barycentrics;
    // The direction leaving x_k (toward x_(k+1)); for a light or environment endpoint, the direction to it.
    float3 wi;
    // The radiance arriving back at x_k from its suffix (suffix throughput and emission), with x_k's own BSDF factor and
    // its x_(k-1)-dependent MIS weight divided out; for an endpoint, the emitter term arriving at x_(k-1). For a hit
    // vertex: F / (prefix throughput * x_(k-1)'s lobe weight * x_k's factor), the two factors as a shift evaluates them
    // (along omega' = (x_k - x_(k-1)) / d), so that the shift into its own pixel reproduces F.
    float3 radiance;
    // The base path's terms at the reconnection [S 8]: P(lambda) p_lambda(omega_(k-1)) cos(theta_k) / d^2, times
    // P(lambda_k) p_lambda_k(omega_k) when x_k continues by BSDF sampling; omega_(k-1) is omega' and x_k is seen along
    // it, as a shift evaluates them. Environment endpoint: the direction's P(lambda) p_lambda. Analytic-light endpoint: 1
    // (delta position; the forced rule). 0: x_k's terms could not be evaluated (the sample cannot shift).
    float jacobian;
    // The light-sampling density of an NEE end (sun or environment), for the MIS weight's recomputation; 0 otherwise.
    float lightPdf;
    uint lightType;
    // lambda_k: the lobe x_k's continuation sampled (pathFlags bits 25-26). A shift evaluates x_k's BSDF with this stored
    // lobe (specification section 5, step 3), never a re-selection.
    uint vertexLobe;
    // T-2.1 round 4 diagnostic (in register only, never stored): capture's rcRadiance divisor factors, the luminance of
    // the prefix throughput times x_(k-1)'s weight, and of x_k's factor.
    float captureArrive;
    float captureLocal;
    // Capture's omega' and cos theta_(k-1) = dot(n_s(x_(k-1)), omega') (in register only; T-2.1 round 5 attribution).
    float3 captureDirection;
    float captureCosine;
};

RestirPtReconnection RestirPtNoReconnection()
{
    RestirPtReconnection rc = (RestirPtReconnection)0;
    return rc;
}

float2 RestirPtOctEncode(float3 direction)
{
    direction /= abs(direction.x) + abs(direction.y) + abs(direction.z);
    float2 encoded = direction.z >= 0.0f ? direction.xy :
        (1.0f - abs(direction.yx)) * select(direction.xy >= 0.0f, 1.0f, -1.0f);
    return encoded * 0.5f + 0.5f;
}

float3 RestirPtOctDecode(float2 encoded)
{
    encoded = encoded * 2.0f - 1.0f;
    float3 direction = float3(encoded, 1.0f - abs(encoded.x) - abs(encoded.y));
    const float fold = saturate(-direction.z);
    direction.xy += select(direction.xy >= 0.0f, -fold, fold);
    return normalize(direction);
}

uint RestirPtPackUnorm2x16(float2 value)
{
    const uint2 packed = uint2(round(saturate(value) * 65535.0f));
    return packed.x | (packed.y << 16);
}

float2 RestirPtUnpackUnorm2x16(uint value)
{
    return float2(value & 0xFFFFu, value >> 16) * (1.0f / 65535.0f);
}

// rcWi's stored octahedral bits (T-2.1b): the encoding's fixed point. Quantizing a decoded direction does not always give
// back the same bits, and under reuse a shifted sample's rcWi is stored again from decoded data; storing the fixed point
// q = encode(decode(q)) makes repeated storage exact, so quantization error cannot accumulate across reuse.
uint RestirPtEncodeDirection(float3 direction)
{
    uint bits = RestirPtPackUnorm2x16(RestirPtOctEncode(any(direction != 0.0f) ? direction : float3(0.0f, 0.0f, 1.0f)));

    for(uint iteration = 0u; iteration < 4u; ++iteration)
    {
        const uint again = RestirPtPackUnorm2x16(RestirPtOctEncode(RestirPtOctDecode(RestirPtUnpackUnorm2x16(bits))));

        if(again == bits)
        {
            break;
        }

        bits = again;
    }

    return bits;
}

// A direction as the reservoir stores and a shift decodes it (RestirPtLoadReconnection).
float3 RestirPtStoredDirection(float3 direction)
{
    return RestirPtOctDecode(RestirPtUnpackUnorm2x16(RestirPtEncodeDirection(direction)));
}

struct RestirPtCandidate
{
    // The PSS integrand: MIS-weighted contribution without the roulette compensation.
    float3 contribution;
    // Its EON diffuse part (signal extraction).
    float3 diffuse;
    // Russian-roulette survival probability of the candidate's prefix (its initial PSS density).
    float survival;
    uint length;
    uint endClass;
    uint lightIndex;
    // D1: the candidate's unbiased contribution weight factor beside 1 / survival: W_RIS p_1 for a finite table-light NEE
    // candidate (supplemental section 5), 1 otherwise.
    float sourceWeight;
    // M5.1-4: its reconnection data and the sampled lobes at x1 and x2 (2 bits each).
    RestirPtReconnection rc;
    uint lobes;
};

struct RestirPtStream
{
    float weightSum;
    uint count;
    RestirPtCandidate selected;
    // Plain path tracing (reservoir off): every candidate's contribution / survival.
    float3 sum;
    float3 diffuseSum;
};

RestirPtCandidate RestirPtMakeCandidate(float3 contribution, float3 diffuse, float survival, uint length, uint endClass,
    uint lightIndex)
{
    RestirPtCandidate candidate;
    candidate.contribution = contribution;
    candidate.diffuse = diffuse;
    candidate.survival = survival;
    candidate.length = length;
    candidate.endClass = endClass;
    candidate.lightIndex = lightIndex;
    candidate.sourceWeight = 1.0f;
    candidate.rc = RestirPtNoReconnection();
    candidate.lobes = 0u;
    return candidate;
}

RestirPtCandidate RestirPtWithReconnection(RestirPtCandidate candidate, RestirPtReconnection rc, uint lobes)
{
    candidate.rc = rc;
    candidate.lobes = lobes;
    return candidate;
}

RestirPtStream RestirPtEmptyStream()
{
    RestirPtStream stream;
    stream.weightSum = 0.0f;
    stream.count = 0u;
    stream.selected = RestirPtMakeCandidate(float3(0.0f, 0.0f, 0.0f), float3(0.0f, 0.0f, 0.0f), 1.0f, 0u, 0u, 0u);
    stream.sum = float3(0.0f, 0.0f, 0.0f);
    stream.diffuseSum = float3(0.0f, 0.0f, 0.0f);
    return stream;
}

bool RestirPtCandidateInImage(RestirPtCandidate candidate)
{
    if((restirPtFlags & RESTIR_PT_FLAG_DIRECT_ONLY) != 0u && candidate.length > 2u)
    {
        return false;
    }

    if((restirPtFlags & RESTIR_PT_FLAG_INDIRECT_ONLY) != 0u && candidate.length < 3u)
    {
        return false;
    }

    return true;
}

// Weighted reservoir sampling (GRIS) with w = p_hat W_candidate, p_hat = luminance(contribution) (the PSS integrand) and
// W_candidate = sourceWeight / survival (D1).
void RestirPtStreamCandidate(inout RestirPtStream stream, RestirPtCandidate candidate, float selectionRandom)
{
    if(!RestirPtCandidateInImage(candidate))
    {
        return;
    }

    ++stream.count;
    const float target = PathTraceLuminance(candidate.contribution);

    if(!(target > 0.0f) || !all(isfinite(candidate.contribution)) || !(candidate.survival > 0.0f) ||
        !(candidate.sourceWeight > 0.0f) || !isfinite(candidate.sourceWeight))
    {
        return;
    }

    const float candidateWeight = candidate.sourceWeight / candidate.survival;
    stream.sum += candidate.contribution * candidateWeight;
    stream.diffuseSum += candidate.diffuse * candidateWeight;
    const float weight = target * candidateWeight;
    stream.weightSum += weight;

    if(selectionRandom * stream.weightSum <= weight)
    {
        stream.selected = candidate;
    }
}

// -----------------------------------------------------------------------------
// Next-event estimation at a vertex (all lobes)
// -----------------------------------------------------------------------------

// M5.1-4: an NEE contribution's factors, contribution = bsdfCos * lightTerm * mis (reconnection data).
struct RestirPtNeeParts
{
    float3 direction;
    // f_all cos at the shading vertex.
    float3 bsdfCos;
    // The emitter term: Li V / p_1 (finite table lights, D1), Li / pdf (sun, environment), Li (directional table lights).
    float3 lightTerm;
    float mis;
    // The light sample's solid-angle density (0: delta light, no MIS).
    float lightPdf;
    // T-2.1 P3: the selected table light's incident radiance Li at the shading vertex, 1 / p_1 (lightTerm = Li / p_1; 1 for
    // a directional table light), and whether the light can be rebuilt from its index (point, spot, directional).
    float3 incident;
    float inverseSourcePdf;
    bool tableLight;
};

RestirPtNeeParts RestirPtNoNeeParts()
{
    RestirPtNeeParts parts = (RestirPtNeeParts)0;
    parts.mis = 1.0f;
    return parts;
}

// f_all cos for a direction, and its diffuse part.
float3 RestirPtBsdfCos(PathTraceSurface surface, float3 direction, out float3 diffuse)
{
    diffuse = float3(0.0f, 0.0f, 0.0f);

    if(!PathTraceDirectionAboveSurface(surface, direction))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 diffuseValue;
    const float cosine = dot(surface.normal, direction);
    const float3 value = PathTraceEvaluateBSDFLobes(surface, direction, diffuseValue) * cosine;
    diffuse = diffuseValue * cosine;
    return value;
}

// Unshadowed f_all cos Li for a light sample, and its diffuse part.
float3 RestirPtUnshadowed(PathTraceSurface surface, PathTraceLightSample lightSample, out float3 diffuse)
{
    diffuse = float3(0.0f, 0.0f, 0.0f);

    if(!lightSample.valid || !PathTraceDirectionAboveSurface(surface, lightSample.direction))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 diffuseValue;
    const float cosine = dot(surface.normal, lightSample.direction);
    const float3 value = PathTraceEvaluateBSDFLobes(surface, lightSample.direction, diffuseValue) * cosine *
        lightSample.radiance;
    diffuse = diffuseValue * cosine * lightSample.radiance;
    return value;
}

void RestirPtCount(uint counter, uint value)
{
    if(restirPtCounterIndex == 0xFFFFFFFFu)
    {
        return;
    }

    const uint total = WaveActiveSum(value);

    if(WaveIsFirstLane() && total != 0u)
    {
        RWByteAddressBuffer counters = ResourceDescriptorHeap[restirPtCounterIndex];
        counters.InterlockedAdd(counter * 4u, total);
    }
}

// The light tile of this 8x8 screen tile, sample and bounce (specification section 4.4; per-bounce choice approved).
uint RestirPtTileBase(uint2 pixel, uint sampleIndex, uint vertex)
{
    const uint tile = PathTraceHash((pixel.x >> 3u) ^ PathTraceHash((pixel.y >> 3u) ^ PathTraceHash(vertex ^
        PathTraceHash(sampleIndex ^ PathTraceHash(restirPtFrameIndex ^ 0x544C4954u)))));
    return (tile % max(restirPtTileCount, 1u)) * restirPtTileSize;
}

uint RestirPtCandidatesAtVertex(uint vertex)
{
    if((restirPtFlags & RESTIR_PT_FLAG_RIS) == 0u)
    {
        return 1u;
    }

    return max(restirPtPrimaryCandidates / (vertex * vertex), 1u);
}

// RIS over the light-tile candidates of the finite table lights: one shadow ray for the selected light (lightIndex).
// The published NEE representation (supplemental section 5, decision D1): the RIS process is a black box, and the
// candidate's PSS integrand is the single-sample NEE integrand omega_1 f / p_1, here f_all cos E V / p_1 (delta lights:
// omega_1 = M p_1 / (M p_1 + p_2) = 1, since p_2 = 0), with p_1 the light tile's source probability of the selected light.
// The RIS effect goes into the candidate's unbiased contribution weight instead: sourceWeight = W_RIS p_1, which the
// stream multiplies into the candidate's resampling weight (W_U = W_RIS p_1 / Q). Returns the integrand and its diffuse
// part.
float3 RestirPtTableLightNee(PathTraceSurface surface, float3 position, uint seed, uint vertex, uint tileBase,
    inout uint shadowRays, out float3 diffuse, out uint lightIndex, out RestirPtNeeParts parts, out float sourceWeight)
{
    diffuse = float3(0.0f, 0.0f, 0.0f);
    lightIndex = 0xFFFFFFFFu;
    parts = RestirPtNoNeeParts();
    sourceWeight = 1.0f;

    if(restirPtLightTileIndex == 0xFFFFFFFFu || restirPtTileSize == 0u || sceneLightCount == 0u)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    ByteAddressBuffer tiles = ResourceDescriptorHeap[restirPtLightTileIndex];
    const uint candidates = RestirPtCandidatesAtVertex(vertex);
    float weightSum = 0.0f;
    float selectedTarget = 0.0f;
    float3 selectedValue = float3(0.0f, 0.0f, 0.0f);
    float3 selectedDiffuse = float3(0.0f, 0.0f, 0.0f);
    PathTraceLightSample selectedSample = (PathTraceLightSample)0;
    bool selectedTable = false;
    float selectedSourcePdf = 0.0f;

    for(uint candidate = 0u; candidate < candidates; ++candidate)
    {
        const uint entryIndex = LightSamplingMulHi(RestirPtRandomUint(seed, vertex, RESTIR_PT_DIMENSION_RIS + 2u * candidate),
            restirPtTileSize);
        const uint2 entry = tiles.Load2((tileBase + entryIndex) * 8u);
        const float sourcePdf = asfloat(entry.y);

        if(entry.x >= sceneLightCount || !(sourcePdf > 0.0f))
        {
            continue;
        }

        const GPULight candidateLight = SceneLightLoad(sceneLightBufferIndex, entry.x);
        const PathTraceLightSample lightSample = PathTraceSampleTableLight(candidateLight, position);
        float3 candidateDiffuse;
        const float3 value = RestirPtUnshadowed(surface, lightSample, candidateDiffuse);
        const float target = PathTraceLuminance(value);

        if(!(target > 0.0f) || !all(isfinite(value)))
        {
            continue;
        }

        const float weight = target / (float(candidates) * sourcePdf);
        weightSum += weight;

        if(RestirPtRandom(seed, vertex, RESTIR_PT_DIMENSION_RIS + 2u * candidate + 1u) * weightSum <= weight)
        {
            selectedTarget = target;
            selectedValue = value;
            selectedDiffuse = candidateDiffuse;
            selectedSample = lightSample;
            selectedTable = candidateLight.type == SCENE_LIGHT_POINT || candidateLight.type == SCENE_LIGHT_SPOT;
            selectedSourcePdf = sourcePdf;
            lightIndex = entry.x;
        }
    }

    if(!(selectedTarget > 0.0f) || !(weightSum > 0.0f))
    {
        lightIndex = 0xFFFFFFFFu;
        return float3(0.0f, 0.0f, 0.0f);
    }

    uint rays = 0u;
    const bool visible = PathTraceVisible(position, surface.geometricNormal, selectedSample.direction,
        selectedSample.distance, rays);
    shadowRays += rays;

    if(!visible)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    // W_RIS = (1 / p_hat(y)) (1 / M) sum_c p_hat(c) / p_tile(c) (specification section 4.2).
    const float risWeight = weightSum / selectedTarget;
    const float inverseSourcePdf = 1.0f / selectedSourcePdf;
    sourceWeight = risWeight * selectedSourcePdf;
    diffuse = selectedDiffuse * inverseSourcePdf;
    float3 bsdfDiffuse;
    parts.direction = selectedSample.direction;
    parts.bsdfCos = RestirPtBsdfCos(surface, selectedSample.direction, bsdfDiffuse);
    parts.lightTerm = selectedSample.radiance * inverseSourcePdf;
    parts.incident = selectedSample.radiance;
    parts.inverseSourcePdf = inverseSourcePdf;
    parts.tableLight = selectedTable;
    return selectedValue * inverseSourcePdf;
}

// One delta (table directional) or MIS-weighted (sun, environment) light sample: f_all cos Li V m / pdf.
float3 RestirPtSingleLightNee(PathTraceSurface surface, float3 position, PathTraceLightSample lightSample,
    inout uint shadowRays, out float3 diffuse, out RestirPtNeeParts parts)
{
    float3 unshadowedDiffuse;
    const float3 value = RestirPtUnshadowed(surface, lightSample, unshadowedDiffuse);
    diffuse = float3(0.0f, 0.0f, 0.0f);
    parts = RestirPtNoNeeParts();

    if(!any(value > 0.0f))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float weight = 1.0f;

    if(lightSample.pdf > 0.0f)
    {
        weight = RestirPtBalanceHeuristic(lightSample.pdf, PathTracePdf(surface, lightSample.direction)) / lightSample.pdf;
    }

    uint rays = 0u;
    const bool visible = PathTraceVisible(position, surface.geometricNormal, lightSample.direction, lightSample.distance,
        rays);
    shadowRays += rays;

    if(!visible)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    diffuse = unshadowedDiffuse * weight;
    float3 bsdfDiffuse;
    parts.direction = lightSample.direction;
    parts.bsdfCos = RestirPtBsdfCos(surface, lightSample.direction, bsdfDiffuse);
    parts.lightPdf = lightSample.pdf;
    // weight = m / pdf for MIS-weighted samples, 1 for delta lights: lightTerm = Li / pdf (or Li), mis = m.
    parts.mis = lightSample.pdf > 0.0f ? RestirPtBalanceHeuristic(lightSample.pdf, PathTracePdf(surface,
        lightSample.direction)) : 1.0f;
    parts.lightTerm = lightSample.pdf > 0.0f ? lightSample.radiance / lightSample.pdf : lightSample.radiance;
    return value * weight;
}

// BSDF-sampled escape radiance with balance-heuristic weights against the sun and environment light samples.
float3 RestirPtEscapeRadiance(float3 direction, bool previousDelta, float previousPdfAll)
{
    const PathTraceEnvironment environment = PathTraceFrameEnvironment();
    const PathTraceSun sun = PathTraceFrameSun();
    const float3 environmentRadiance = PathTraceEnvironmentRadiance(environment, environmentSampler, direction);
    const float3 sunRadiance = PathTraceSunRadiance(sun, direction);

    if(previousDelta)
    {
        return environmentRadiance + sunRadiance;
    }

    const float environmentWeight = PathTraceEnvironmentSamplingEnabled(environment) ?
        RestirPtBalanceHeuristic(previousPdfAll, PathTraceEnvironmentPdf(environment, direction)) : 1.0f;
    const float sunWeight = RestirPtBalanceHeuristic(previousPdfAll, PathTraceSunPdf(sun, direction));
    return environmentRadiance * environmentWeight + sunRadiance * sunWeight;
}

// -----------------------------------------------------------------------------
// The path tree
// -----------------------------------------------------------------------------

struct RestirPtTreeResult
{
    RestirPtStream stream;
    uint continuationRays;
    uint shadowRays;
    // x1's continuation, for the denoisers' hit distances (as the production extraction): its length (HALF_MAXIMUM on a
    // miss, negative without one) and whether it took the GGX or delta lobe.
    float primaryHitDistance;
    bool primarySpecular;
    // Gate validity indicator (RESTIR_PT_COUNTER_CLAMP_WOULD_ENGAGE): vertices j >= 2 whose NEE sum, or whose escape, would
    // exceed the production path tracer's absolute indirect clamp (its throughput divided by roulette survival, as there).
    uint wouldClamp;
};

// The diffuse part, per channel, of x1's sampled lobe weight: carried by every deeper candidate (production convention).
float3 RestirPtDiffuseShare(RestirPtLobeSample lobeSample)
{
    return select(lobeSample.weight > 0.0f, saturate(lobeSample.diffuseWeight / max(lobeSample.weight, 1.0e-30f)), 0.0f);
}

// -----------------------------------------------------------------------------
// M5.1-4 reconnection data (specification sections 4.7 and 7.1)
// -----------------------------------------------------------------------------

struct RestirPtHitIdentity
{
    uint instance;
    uint primitive;
    float2 barycentrics;
};

// PathTraceTraceRadianceInline (PathTraceReference.hlsl) with the committed hit's TLAS InstanceID, primitive and
// barycentrics (B's own copy: the production function is unchanged).
void RestirPtTraceInline(RayDesc ray, inout PathTracePayload payload, out RestirPtHitIdentity identity)
{
    identity = (RestirPtHitIdentity)0;
    RayQuery<RAY_FLAG_NONE> query;
    RESTIR_PT_TRACE_INLINE(query, RAY_FLAG_NONE, ray);

    while(query.Proceed())
    {
        if(query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            query.CommitNonOpaqueTriangleHit();
        }
    }

    if(query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        identity.instance = query.CommittedInstanceID();
        identity.primitive = query.CommittedPrimitiveIndex();
        identity.barycentrics = query.CommittedTriangleBarycentrics();
#define PATH_TRACE_HIT_INSTANCE_ID query.CommittedInstanceID()
#define PATH_TRACE_HIT_PRIMITIVE_INDEX query.CommittedPrimitiveIndex()
#define PATH_TRACE_HIT_BARYCENTRICS query.CommittedTriangleBarycentrics()
#define PATH_TRACE_HIT_WORLD_TO_OBJECT query.CommittedWorldToObject3x4()
#define PATH_TRACE_HIT_OBJECT_TO_WORLD query.CommittedObjectToWorld3x4()
#define PATH_TRACE_HIT_WORLD_RAY_DIRECTION query.WorldRayDirection()
#define PATH_TRACE_HIT_T query.CommittedRayT()
#include "../PathTraceClosestHitBody.hlsli"
#undef PATH_TRACE_HIT_INSTANCE_ID
#undef PATH_TRACE_HIT_PRIMITIVE_INDEX
#undef PATH_TRACE_HIT_BARYCENTRICS
#undef PATH_TRACE_HIT_WORLD_TO_OBJECT
#undef PATH_TRACE_HIT_OBJECT_TO_WORLD
#undef PATH_TRACE_HIT_WORLD_RAY_DIRECTION
#undef PATH_TRACE_HIT_T
    }
    else
    {
        payload.hitDistance = -1.0f;
    }
}

// The reconnection's vertex-to-vertex direction omega' and distance d from y_(k-1) to x_k. Capture and every shift call this
// one function, so omega' is the same to the last bit given the same positions (T-2.1 round 5: two algebraically equal
// spellings differed in the last bit for about 40% of reconnections, which grazing evaluations amplified to 1-8%).
float3 RestirPtReconnectionDirection(float3 previousPosition, float3 vertexPosition, out float distance)
{
    const float3 offset = vertexPosition - previousPosition;
    distance = length(offset);
    return distance > 0.0f ? offset / distance : float3(0.0f, 0.0f, 0.0f);
}

// A stored vertex's world position (instance, primitive, barycentrics), from the per-instance transforms.
bool RestirPtVertexPosition(uint instanceID, uint primitive, float2 storedBarycentrics, out float3 position)
{
    position = float3(0.0f, 0.0f, 0.0f);

    if(instanceID >= restirPtTransformCount)
    {
        return false;
    }

    StructuredBuffer<RayTracingInstanceData> rebuildInstances = ResourceDescriptorHeap[instanceDataIndex];
    const RayTracingInstanceData rebuildInstance = rebuildInstances[instanceID];
    ByteAddressBuffer rebuildVertices = ResourceDescriptorHeap[NonUniformResourceIndex(rebuildInstance.vertexBufferIndex)];
    const uint3 rebuildIndices = PathTraceTriangleIndices(rebuildInstance, primitive);
    const float3 weights = float3(1.0f - storedBarycentrics.x - storedBarycentrics.y, storedBarycentrics.x,
        storedBarycentrics.y);
    const float3 objectPosition =
        PathTraceLoadPosition(rebuildVertices, rebuildIndices.x, rebuildInstance.vertexStride) * weights.x +
        PathTraceLoadPosition(rebuildVertices, rebuildIndices.y, rebuildInstance.vertexStride) * weights.y +
        PathTraceLoadPosition(rebuildVertices, rebuildIndices.z, rebuildInstance.vertexStride) * weights.z;
    position = mul(RestirPtLoadInstanceTransform(instanceID).objectToWorld, float4(objectPosition, 1.0f));
    return true;
}

// Rebuilds a stored vertex (instance, primitive, barycentrics) without tracing, from the per-instance transforms: its world
// position and the payload the closest-hit body computes for a ray from origin (M5.2 prerequisite).
bool RestirPtRebuildVertex(uint instanceID, uint primitive, float2 storedBarycentrics, float3 origin, out float3 position,
    out PathTracePayload payload)
{
    payload = (PathTracePayload)0;
    payload.hitDistance = -1.0f;

    if(!RestirPtVertexPosition(instanceID, primitive, storedBarycentrics, position))
    {
        return false;
    }

    const RestirPtInstanceTransform transform = RestirPtLoadInstanceTransform(instanceID);
    const float3 offset = position - origin;
    const float distance = length(offset);

    if(!(distance > 0.0f))
    {
        return false;
    }

    const float3 rebuildDirection = offset / distance;

    {
#define PATH_TRACE_HIT_INSTANCE_ID instanceID
#define PATH_TRACE_HIT_PRIMITIVE_INDEX primitive
#define PATH_TRACE_HIT_BARYCENTRICS storedBarycentrics
#define PATH_TRACE_HIT_WORLD_TO_OBJECT transform.worldToObject
#define PATH_TRACE_HIT_OBJECT_TO_WORLD transform.objectToWorld
#define PATH_TRACE_HIT_WORLD_RAY_DIRECTION rebuildDirection
#define PATH_TRACE_HIT_T distance
#include "../PathTraceClosestHitBody.hlsli"
#undef PATH_TRACE_HIT_INSTANCE_ID
#undef PATH_TRACE_HIT_PRIMITIVE_INDEX
#undef PATH_TRACE_HIT_BARYCENTRICS
#undef PATH_TRACE_HIT_WORLD_TO_OBJECT
#undef PATH_TRACE_HIT_OBJECT_TO_WORLD
#undef PATH_TRACE_HIT_WORLD_RAY_DIRECTION
#undef PATH_TRACE_HIT_T
    }

    return true;
}

// Criterion 1 (and the original's x_(k-1) test) on the sampled lobe: the cosine lobe counts as rough, a delta lobe never,
// the GGX lobe when its alpha (perceptual roughness squared, as the BSDF uses) reaches alpha_min.
bool RestirPtLobeRough(PathTraceSurface surface, uint lobe)
{
    if(lobe == RESTIR_PT_LOBE_DELTA)
    {
        return false;
    }

    return lobe == RESTIR_PT_LOBE_COSINE || surface.material.roughness * surface.material.roughness >=
        restirPtReconnectionRoughness;
}

// The original criteria's x_k test [P 14]: the material's roughness (GGX alpha) reaches alpha_min, never a delta surface.
// (A diffuse base does not make a sharp specular coat rough: x_k's BSDF, evaluated from a changed incoming direction, must
// be well conditioned, which is what the criterion is for. T-2.1 measured the alternative: J errors up to 217x.)
// M5.4 C4: Eq. 5 [P 7; S 2-5].
bool RestirPtFootprintCriteria()
{
    return (restirPtFlags & RESTIR_PT_FLAG_FOOTPRINT_CRITERIA) != 0u;
}

// Eq. 5's right-hand side for a path whose primary hit is `primary`, seen from `camera`: (c / 100) times the primary
// footprint R_pri^2 = |x0 - x1|^2 / (<n_x1, d_x1x0> / 4 pi) [P 7, Eq. 5; S 2, Eq. 6]. Each path uses its own camera and
// primary (a shift evaluates the destination's). No resolution term, as published.
float RestirPtFootprintThreshold(float3 camera, PathTraceRasterFirstVertex primary)
{
    const float3 offset = primary.position - camera;
    const float cosine = abs(dot(primary.surface.geometricNormal, primary.surface.view));
    return (RESTIR_PT_FOOTPRINT_C / 100.0f) * 4.0f * PI * dot(offset, offset) / max(cosine, 1.0e-6f);
}

// A footprint test, in multiplied form: the footprint 1 / (p G) reaches the threshold, p the marginal directional density
// (the MIS one [S 3]) and G = |cos| / d^2 at the receiving vertex. A zero density is an unbounded footprint (passes).
bool RestirPtFootprintPasses(float density, float cosine, float distance, float threshold)
{
    return density * cosine * threshold <= distance * distance;
}

// The inverse ray footprint at x_k [S 3, Eq. 20], (p_xk(omega_k) G(x_k -> x_(k-1)))^-1, for a BSDF-sampled omega_k: x_k seen
// from x_(k-1) (along omega'), previousCosine = |<n_(x_(k-1)), omega'>|. Footnote 6 [P 8]: skipped for a purely diffuse x_k
// (no specular lobe; the literal reading, M5.4 decision). Emissive x_k and light-sampled omega_k never call it.
bool RestirPtInverseFootprintPasses(PathTraceSurface vertexSurface, float3 outgoing, float previousCosine, float distance,
    float threshold)
{
    if(!vertexSurface.deltaSpecular && !(vertexSurface.specularProbability > 0.0f))
    {
        return true;
    }

    return RestirPtFootprintPasses(PathTracePdf(vertexSurface, outgoing), previousCosine, distance, threshold);
}

bool RestirPtSurfaceRough(PathTraceSurface surface)
{
    return !surface.deltaSpecular && surface.material.roughness * surface.material.roughness >= restirPtReconnectionRoughness;
}

float RestirPtLobeDensity(PathTraceSurface surface, uint lobe, float3 direction)
{
    return RestirPtLobeProbability(surface, lobe) * RestirPtLobePdf(surface, lobe, direction);
}

float3 RestirPtDivide(float3 numerator, float3 denominator)
{
    return select(denominator != 0.0f, numerator / denominator, 0.0f);
}

// The prefix's reconnection state: the first qualifying hit vertex x_k so far, the throughput arriving at it, and (once
// x_k has continued) its continuation's direction, lobe weight and density.
// The cached terms are evaluated as a shift evaluates them (T-2.1 round 3): along the vertex-to-vertex direction
// omega' = (x_k - x_(k-1)) / d, not the base path's sampled direction omega, which leaves the offset ray origin and differs
// from omega' by about (ray offset) / d. Contributions (F) are unchanged; only rcRadiance's divisor and the cached Jacobian
// use omega'.
struct RestirPtPrefixState
{
    RestirPtReconnection rc;
    // The prefix throughput times x_(k-1)'s lobe weight along omega'.
    float3 arrive;
    // x_k's continuation direction, and its replayed lobe's weight and density there, with x_k seen along omega'.
    float3 continuationDirection;
    float3 continuationWeight;
    float continuationDensity;
    uint lobes;
    // omega' and x_k's hit payload, to rebuild x_k as a shift sees it.
    float3 reconnectionDirection;
    PathTracePayload vertexPayload;
    // M5.4 C4: |<n_(x_(k-1)), omega'>| and |x_k - x_(k-1)|, for x_k's inverse footprint, and this path's Eq. 5 threshold.
    float previousCosine;
    float reconnectionDistance;
    float footprintThreshold;
};

// An NEE candidate's reconnection data at shading vertex v (contribution = throughput * bsdfCos * lightTerm * mis).
// reconnectionSurface: x_k seen along omega' (used when x_k is the shading vertex).
RestirPtReconnection RestirPtNeeReconnection(RestirPtPrefixState prefix, uint vertex, RestirPtNeeParts parts,
    float3 contribution, uint lightType, uint lightIndex, PathTraceSurface reconnectionSurface)
{
    RestirPtReconnection rc = prefix.rc;
    rc.lightType = lightType;
    // M5.4 C4: NEE leaving x_k has a light-sampled omega_k, whose density does not depend on x_(k-1) (Eq. 4b is absent from
    // the Jacobian), so the inverse footprint is skipped (Option B, the Engine interpretation of footnote 6). Test only
    // (A'): the inverse footprint with the BSDF density toward the light; failing it postpones the reconnection to the
    // light vertex (the forced rule), exactly as the shift applies it.
    bool vertexQualifies = prefix.rc.index == vertex;

    if(vertexQualifies && RestirPtFootprintCriteria())
    {
        RestirPtCount(RESTIR_PT_COUNTER_FOOTPRINT + 4u, 1u);

        if((restirPtFlags & RESTIR_PT_FLAG_NEE_INVERSE_PROXY) != 0u)
        {
            RestirPtCount(RESTIR_PT_COUNTER_FOOTPRINT + 5u, 1u);
            vertexQualifies = RestirPtInverseFootprintPasses(reconnectionSurface, parts.direction, prefix.previousCosine,
                prefix.reconnectionDistance, prefix.footprintThreshold);
            RestirPtCount(RESTIR_PT_COUNTER_FOOTPRINT + 6u, vertexQualifies ? 1u : 0u);
        }
    }

    if(vertexQualifies)
    {
        // x_k is the shading vertex: the light sample leaves it (light-sampled, so no continuation density). rcRadiance
        // divides out f_all cos and the MIS weight as a shift recomputes them, with x_k seen along omega'. Where that
        // factor is 0 (the light at x_k's horizon seen along omega'; spec section 5 step 5 rejects f = 0), x_k is not a
        // valid reconnection for this candidate: it is stored replay-only (T-2.1 round 4, decision D2).
        float3 diffuse;
        const float mis = parts.lightPdf > 0.0f ? RestirPtBalanceHeuristic(parts.lightPdf, PathTracePdf(reconnectionSurface,
            parts.direction)) : 1.0f;
        const float3 local = RestirPtBsdfCos(reconnectionSurface, parts.direction, diffuse) * mis;
        rc.wi = parts.direction;
        rc.radiance = RestirPtDivide(contribution, prefix.arrive * local);
        rc.lightPdf = parts.lightPdf;
        rc.captureArrive = PathTraceLuminance(prefix.arrive);
        rc.captureLocal = PathTraceLuminance(local);

        if(!any(local > 0.0f))
        {
            rc.index = 0u;
        }

        return rc;
    }

    if(prefix.rc.index != 0u && prefix.rc.index != vertex)
    {
        // x_k earlier on the prefix: the suffix leaves it along its continuation.
        rc.wi = prefix.continuationDirection;
        rc.radiance = RestirPtDivide(contribution, prefix.arrive * prefix.continuationWeight);
        rc.jacobian = prefix.rc.jacobian * prefix.continuationDensity;
        rc.captureArrive = PathTraceLuminance(prefix.arrive);
        rc.captureLocal = PathTraceLuminance(prefix.continuationWeight);
        return rc;
    }

    // Forced NEE reconnection [P 11]: the light vertex (delta position: the forced rule, J = 1).
    rc.index = vertex + 1u;
    rc.instance = lightIndex;
    rc.wi = parts.direction;
    rc.radiance = parts.lightTerm;
    rc.jacobian = 1.0f;
    rc.lightPdf = parts.lightPdf;

    // T-2.1 P3: a table light's incident radiance is factored out: rcRadiance keeps 1 / p_1 (D1; p_1 depends only on the
    // light index; 1 for a directional table light), so a shift re-evaluates direction, attenuation and visibility from the
    // light index at its own y_(k-1): F' = T' f' Li'(y) / p_1.
    if(parts.tableLight)
    {
        rc.lightType = RESTIR_PT_LIGHT_TABLE;
        rc.radiance = parts.inverseSourcePdf.xxx;
    }

    return rc;
}

// A BSDF-sampled escape's reconnection data at vertex v (contribution = throughput * lobe weight * radiance).
RestirPtReconnection RestirPtEscapeReconnection(RestirPtPrefixState prefix, uint vertex, PathTraceSurface surface,
    RestirPtLobeSample lobeSample, float3 radiance, float3 contribution)
{
    RestirPtReconnection rc = prefix.rc;
    rc.lightType = RESTIR_PT_LIGHT_ENVIRONMENT;

    if(prefix.rc.index != 0u)
    {
        // x_k at or before this vertex: the suffix leaves x_k along its continuation (when x_k is this vertex, the
        // continuation is this escape; its weight and density are x_k's as a shift evaluates them).
        rc.wi = prefix.continuationDirection;
        rc.radiance = RestirPtDivide(contribution, prefix.arrive * prefix.continuationWeight);
        rc.jacobian = prefix.rc.jacobian * prefix.continuationDensity;
        rc.captureArrive = PathTraceLuminance(prefix.arrive);
        rc.captureLocal = PathTraceLuminance(prefix.continuationWeight);
        return rc;
    }

    // Environment endpoint [P 8]: the roughness test only; J = the direction's density ratio.
    if(RestirPtLobeRough(surface, lobeSample.lobe))
    {
        rc.index = vertex + 1u;
        rc.wi = lobeSample.direction;
        rc.radiance = radiance;
        rc.jacobian = RestirPtLobeDensity(surface, lobeSample.lobe, lobeSample.direction);
    }

    return rc;
}

RestirPtTreeResult RestirPtTracePathTree(uint2 pixel, uint sampleIndex, PathTraceRasterFirstVertex firstVertex, uint seed)
{
    RestirPtTreeResult result;
    result.stream = RestirPtEmptyStream();
    result.continuationRays = 0u;
    result.shadowRays = 0u;
    result.primaryHitDistance = PATH_TRACE_EXTRACTION_NO_SPECULAR;
    result.primarySpecular = false;
    result.wouldClamp = 0u;

    if(firstVertex.state == 0)
    {
        // A background pixel: the camera sees the sun and environment directly (k = 1).
        const float3 radiance = PathTraceMissRadiance(firstVertex.backgroundDirection, true, 0.0f);
        RestirPtStreamCandidate(result.stream, RestirPtMakeCandidate(radiance, float3(0.0f, 0.0f, 0.0f), 1.0f, 1u,
            RESTIR_PT_END_CAMERA, 0u), RestirPtRandom(seed, 0u, RESTIR_PT_DIMENSION_STREAM));
        return result;
    }

    if(firstVertex.state < 0)
    {
        return result;
    }

    const PathTraceSun sun = PathTraceFrameSun();
    const PathTraceEnvironment environment = PathTraceFrameEnvironment();
    const LightSamplingConstants sampling = LightSamplingLoadConstants(restirPtLightSamplingIndex);
    const bool samplingValid = restirPtLightSamplingIndex != 0xFFFFFFFFu && sampling.valid != 0u &&
        sampling.tableLightCount == sceneLightCount;

    PathTraceSurface surface = firstVertex.surface;
    // M5.4 C4: this path tree's Eq. 5 threshold (its own camera and primary hit).
    const float footprintThreshold = firstVertex.state > 0 ? RestirPtFootprintThreshold(cameraPosition, firstVertex) : 0.0f;
    float3 position = firstVertex.position;
    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    float3 diffuseShare = float3(1.0f, 1.0f, 1.0f);
    float survival = 1.0f;
    // M5.1-4: the prefix's reconnection state.
    RestirPtPrefixState prefix = (RestirPtPrefixState)0;

    [loop]
    for(uint vertex = 1u; vertex <= maxPathDepth; ++vertex)
    {
        const bool hasNonDeltaLobe = !surface.deltaSpecular || surface.specularProbability < 1.0f;
        // A k = 2 candidate's diffuse part is its own lobe split; deeper candidates carry x1's (diffuseShare).
        const bool primary = vertex == 1u;
        // M5.1-4: at the reconnection vertex x_k, x_k as a shift sees it (seen along omega'), for its cached terms. If it
        // cannot be built, x_k does not qualify after all (never observed; decision D2).
        PathTraceSurface reconnectionSurface = surface;

        if(prefix.rc.index == vertex && !PathTraceBuildSurface(prefix.vertexPayload, prefix.reconnectionDirection,
            reconnectionSurface))
        {
            prefix.rc = RestirPtNoReconnection();
        }

        if(hasNonDeltaLobe)
        {
            float3 diffuse;
            uint lightIndex;
            RestirPtNeeParts parts;
            // The vertex's NEE sum (the clamp indicator only).
            float3 vertexNee = float3(0.0f, 0.0f, 0.0f);

            // Finite table lights: RIS over the light tile.
            float sourceWeight;
            float3 contribution = RestirPtTableLightNee(surface, position, seed, vertex,
                RestirPtTileBase(pixel, sampleIndex, vertex), result.shadowRays, diffuse, lightIndex, parts, sourceWeight);

            if(primary && (restirPtFlags & RESTIR_PT_FLAG_INJECT_CANDIDATE_NAN) != 0u && RestirPtInjectedPixel(pixel))
            {
                contribution = asfloat(0x7FC00000u).xxx;
            }

            RestirPtCandidate tableCandidate = RestirPtWithReconnection(RestirPtMakeCandidate(throughput * contribution,
                primary ? diffuse : throughput * contribution * diffuseShare, survival, vertex + 1u, RESTIR_PT_END_TABLE,
                lightIndex), RestirPtNeeReconnection(prefix, vertex, parts, throughput * contribution,
                RESTIR_PT_LIGHT_ANALYTIC, lightIndex, reconnectionSurface), prefix.lobes);
            tableCandidate.sourceWeight = sourceWeight;
            RestirPtStreamCandidate(result.stream, tableCandidate,
                RestirPtRandom(seed, vertex, RESTIR_PT_DIMENSION_STREAM));
            vertexNee += throughput * contribution * sourceWeight;

            // Directional table lights: enumerated, each its own delta sample and domain.
            const uint directionalCount = samplingValid ? min(sampling.directionalCount, 8u) : 0u;

            for(uint directional = 0u; directional < directionalCount; ++directional)
            {
                const uint directionalLight = LightSamplingDirectionalLight(sampling, directional);
                const PathTraceLightSample lightSample = PathTraceSampleTableLight(
                    SceneLightLoad(sceneLightBufferIndex, directionalLight), position);
                contribution = RestirPtSingleLightNee(surface, position, lightSample, result.shadowRays, diffuse, parts);
                // P3: a directional table light is rebuilt from its index too (fixed direction; p_1 = 1).
                parts.incident = lightSample.radiance;
                parts.inverseSourcePdf = 1.0f;
                parts.tableLight = true;
                RestirPtStreamCandidate(result.stream, RestirPtWithReconnection(RestirPtMakeCandidate(
                    throughput * contribution, primary ? diffuse : throughput * contribution * diffuseShare, survival,
                    vertex + 1u, RESTIR_PT_END_DIRECTIONAL, directionalLight), RestirPtNeeReconnection(prefix, vertex, parts,
                    throughput * contribution, RESTIR_PT_LIGHT_ANALYTIC, directionalLight, reconnectionSurface),
                    prefix.lobes), RestirPtRandom(seed, vertex, RESTIR_PT_DIMENSION_STREAM + 8u + directional));
                vertexNee += throughput * contribution;
            }

            // The sun and the environment: one MIS-weighted sample each.
            if(PathTraceSunHasEnergy(sun))
            {
                const float2 sunRandom = float2(RestirPtRandom(seed, vertex, RESTIR_PT_DIMENSION_SUN),
                    RestirPtRandom(seed, vertex, RESTIR_PT_DIMENSION_SUN + 1u));
                contribution = RestirPtSingleLightNee(surface, position, PathTraceSampleSun(sun, sunRandom),
                    result.shadowRays, diffuse, parts);
                RestirPtStreamCandidate(result.stream, RestirPtWithReconnection(RestirPtMakeCandidate(
                    throughput * contribution, primary ? diffuse : throughput * contribution * diffuseShare, survival,
                    vertex + 1u, RESTIR_PT_END_SUN, 0u), RestirPtNeeReconnection(prefix, vertex, parts,
                    throughput * contribution, RESTIR_PT_LIGHT_SUN, 0u, reconnectionSurface), prefix.lobes),
                    RestirPtRandom(seed, vertex, RESTIR_PT_DIMENSION_STREAM + 1u));
                vertexNee += throughput * contribution;
            }

            if(PathTraceEnvironmentSamplingEnabled(environment))
            {
                const float4 environmentRandom = float4(RestirPtRandom(seed, vertex, RESTIR_PT_DIMENSION_ENVIRONMENT),
                    RestirPtRandom(seed, vertex, RESTIR_PT_DIMENSION_ENVIRONMENT + 1u),
                    RestirPtRandom(seed, vertex, RESTIR_PT_DIMENSION_ENVIRONMENT + 2u),
                    RestirPtRandom(seed, vertex, RESTIR_PT_DIMENSION_ENVIRONMENT + 3u));
                contribution = RestirPtSingleLightNee(surface, position, PathTraceSampleEnvironment(environment,
                    environmentSampler, environmentRandom), result.shadowRays, diffuse, parts);
                RestirPtStreamCandidate(result.stream, RestirPtWithReconnection(RestirPtMakeCandidate(
                    throughput * contribution, primary ? diffuse : throughput * contribution * diffuseShare, survival,
                    vertex + 1u, RESTIR_PT_END_ENVIRONMENT, 0u), RestirPtNeeReconnection(prefix, vertex, parts,
                    throughput * contribution, RESTIR_PT_LIGHT_ENVIRONMENT, 0u, reconnectionSurface), prefix.lobes),
                    RestirPtRandom(seed, vertex, RESTIR_PT_DIMENSION_STREAM + 2u));
                vertexNee += throughput * contribution;
            }

            if(vertex >= 2u && PathTraceLuminance(vertexNee / survival) > PATH_TRACE_INDIRECT_LUMINANCE_CLAMP)
            {
                ++result.wouldClamp;
            }
        }

        // One lobe, one continuation.
        const float3 lobeRandom = float3(RestirPtRandom(seed, vertex, RESTIR_PT_DIMENSION_LOBE),
            RestirPtRandom(seed, vertex, RESTIR_PT_DIMENSION_DIRECTION),
            RestirPtRandom(seed, vertex, RESTIR_PT_DIMENSION_DIRECTION + 1u));
        const RestirPtLobeSample lobeSample = RestirPtSampleLobe(surface, lobeRandom);

        if(!lobeSample.valid)
        {
            break;
        }

        if(primary)
        {
            diffuseShare = RestirPtDiffuseShare(lobeSample);
            result.primarySpecular = lobeSample.lobe != RESTIR_PT_LOBE_COSINE;
        }

        // M5.1-4: the sampled lobes at x1 and x2 (pathFlags bits 20-23), and x_k's continuation once x_k is this vertex.
        if(vertex <= 2u)
        {
            prefix.lobes |= (lobeSample.lobe & 3u) << (2u * (vertex - 1u));
        }

        if(prefix.rc.index == vertex)
        {
            // x_k's continuation terms as a shift evaluates them: the sampled lobe lambda_k (stored, P1), with x_k seen
            // along omega', its weight and density toward the base path's continuation direction. Where that lobe has no
            // density or BSDF value there (the direction can fall below the horizon seen along omega'; spec section 5,
            // step 5: reject when f = 0), x_k does not qualify after all: the reconnection is dropped, and a later
            // vertex or the forced NEE rule may qualify (decision D2). Candidates already streamed from x_k's NEE keep
            // their own, valid reconnection.
            const float storedLobeDensity = RestirPtLobeDensity(reconnectionSurface, lobeSample.lobe, lobeSample.direction);
            prefix.rc.vertexLobe = lobeSample.lobe;
            prefix.continuationDirection = lobeSample.direction;
            prefix.continuationWeight = float3(0.0f, 0.0f, 0.0f);
            prefix.continuationDensity = storedLobeDensity;

            if(storedLobeDensity > 0.0f)
            {
                float3 diffuseValue;
                prefix.continuationWeight = RestirPtEvaluateLobe(reconnectionSurface, lobeSample.lobe, lobeSample.direction,
                    diffuseValue) * dot(reconnectionSurface.normal, lobeSample.direction) / storedLobeDensity;
            }

            // M5.4 C4: the inverse ray footprint for the BSDF-sampled omega_k (this continuation, or the escape it becomes),
            // with x_k seen along omega' as a shift sees it. Failing it, x_k does not qualify for this path: reconnection
            // is postponed (a later vertex, the environment-endpoint rule, or replay-only).
            bool inversePasses = true;

            if(RestirPtFootprintCriteria() && any(prefix.continuationWeight > 0.0f))
            {
                const bool diffuseOnly = !reconnectionSurface.deltaSpecular &&
                    !(reconnectionSurface.specularProbability > 0.0f);
                RestirPtCount(RESTIR_PT_COUNTER_FOOTPRINT + (diffuseOnly ? 7u : 2u), 1u);
                inversePasses = RestirPtInverseFootprintPasses(reconnectionSurface, lobeSample.direction,
                    prefix.previousCosine, prefix.reconnectionDistance, prefix.footprintThreshold);
                RestirPtCount(RESTIR_PT_COUNTER_FOOTPRINT + 3u, !diffuseOnly && inversePasses ? 1u : 0u);
            }

            if(!any(prefix.continuationWeight > 0.0f) || !inversePasses)
            {
                const uint lobes = prefix.lobes;
                prefix = (RestirPtPrefixState)0;
                prefix.lobes = lobes;
            }
        }

        const bool continuationRough = RestirPtLobeRough(surface, lobeSample.lobe);
        const float3 nextThroughput = throughput * lobeSample.weight;

        // Russian roulette after the continuation is sampled, initial sampling only (supplemental section 6): the
        // survival probability enters the candidates' PSS density, never their contribution.
        if(restirPtRouletteFromVertex != 0u && vertex >= restirPtRouletteFromVertex)
        {
            const float continueProbability = min(max(nextThroughput.r, max(nextThroughput.g, nextThroughput.b)), 0.95f);

            if(!(continueProbability > 0.0f) || RestirPtRandom(seed, vertex, RESTIR_PT_DIMENSION_ROULETTE) >=
                continueProbability)
            {
                break;
            }

            survival *= continueProbability;
        }

        RayDesc ray;
        ray.Origin = PathTraceOffsetRayOrigin(position, surface.geometricNormal);
        ray.Direction = lobeSample.direction;
        ray.TMin = 0.0f;
        ray.TMax = PATH_TRACE_DISTANT_LIGHT_DISTANCE;

        PathTracePayload payload;
        payload.hitDistance = -1.0f;
        payload.geometricNormal = float3(0.0f, 0.0f, 1.0f);
        payload.shadingNormal = float3(0.0f, 0.0f, 1.0f);
        payload.uv = float2(0.0f, 0.0f);
        payload.materialIndex = 0u;
        RestirPtHitIdentity identity;
        RestirPtTraceInline(ray, payload, identity);
        ++result.continuationRays;

        if(primary)
        {
            result.primaryHitDistance = payload.hitDistance < 0.0f ? PATH_TRACE_HALF_MAXIMUM : payload.hitDistance;
        }

        if(payload.hitDistance < 0.0f)
        {
            // BSDF-sampled escape: k = vertex + 1, technique BSDF.
            // M5.3 close-out (user-approved 2026-11-22): where a shift re-evaluates this escape from the stored rcWi (an
            // environment endpoint left by this vertex, or x_k = this vertex continuing straight to it; see
            // RestirPtEscapeReconnection), capture evaluates it as the shift does: at the decoded oct16 direction, with
            // the mixture density of the vertex as a shift sees it (x_k along omega'). The same-pixel shift then
            // reproduces F. Other escapes (replay-only, or x_k earlier on the path) are never re-evaluated from rcWi.
            const bool shiftEvaluatesEscape = lobeSample.lobe != RESTIR_PT_LOBE_DELTA && (prefix.rc.index == vertex ||
                (prefix.rc.index == 0u && RestirPtLobeRough(surface, lobeSample.lobe)));
            float3 radiance;

            if(shiftEvaluatesEscape)
            {
                const float3 storedDirection = RestirPtStoredDirection(lobeSample.direction);
                const float storedDensity = prefix.rc.index == vertex ? PathTracePdf(reconnectionSurface, storedDirection) :
                    PathTracePdf(surface, storedDirection);
                radiance = RestirPtEscapeRadiance(storedDirection, false, storedDensity);
            }
            else
            {
                radiance = RestirPtEscapeRadiance(lobeSample.direction, lobeSample.lobe == RESTIR_PT_LOBE_DELTA,
                    lobeSample.pdfAll);
            }

            const float3 contribution = nextThroughput * radiance;
            const float3 diffuse = primary ? throughput * lobeSample.diffuseWeight * radiance : contribution * diffuseShare;
            RestirPtStreamCandidate(result.stream, RestirPtWithReconnection(RestirPtMakeCandidate(contribution, diffuse,
                survival, vertex + 1u, RESTIR_PT_END_ESCAPE, 0u), RestirPtEscapeReconnection(prefix, vertex, surface,
                lobeSample, radiance, contribution), prefix.lobes),
                RestirPtRandom(seed, vertex, RESTIR_PT_DIMENSION_STREAM + 3u));

            if(vertex >= 2u && PathTraceLuminance(contribution / survival) > PATH_TRACE_INDIRECT_LUMINANCE_CLAMP)
            {
                ++result.wouldClamp;
            }

            break;
        }

        if(vertex == maxPathDepth)
        {
            break;
        }

        const float3 previousPosition = position;
        position = ray.Origin + ray.Direction * payload.hitDistance;

        // M5.1-4: x_(vertex+1) is the reconnection vertex when it is the first that meets the original criteria [P 14]:
        // the sampled lobe at x_vertex is rough, |x_vertex x_(vertex+1)| >= d_min, and x_(vertex+1) itself is rough.
        // Its cached terms use the geometry between the vertex positions themselves, as a shift evaluates it: the geometry
        // term, and x_vertex's lobe density and weight along omega' (evaluated here, while surface is still x_vertex).
        // x_(vertex+1) is rebuilt from its hit identity exactly as a shift rebuilds it (position and payload from the
        // per-instance transforms; the traced hit differs in the last bits, which matters at grazing angles). A vertex
        // that cannot be rebuilt, or that omega' puts outside x_vertex's lobe or BSDF (f = 0, specification section 5,
        // step 5), does not qualify (a shift could not reach it).
        float3 reconnectionVertex = position;
        PathTracePayload reconnectionPayload = payload;
        bool reconnectionRebuilt = false;

        if(prefix.rc.index == 0u && continuationRough)
        {
            reconnectionRebuilt = RestirPtRebuildVertex(identity.instance, identity.primitive, identity.barycentrics,
                previousPosition, reconnectionVertex, reconnectionPayload);
        }

        float reconnectionDistance;
        const float3 reconnectionDirection = RestirPtReconnectionDirection(previousPosition, reconnectionVertex,
            reconnectionDistance);
        // M5.4 C4: the published criteria drop d_min (and x_k's roughness, below) for Eq. 5's forward ray footprint.
        const bool footprintCriteria = RestirPtFootprintCriteria();
        const bool reconnectionCandidate = reconnectionRebuilt && (footprintCriteria ||
            reconnectionDistance >= restirPtReconnectionMinDistance) && reconnectionDistance > 0.0f;
        float reconnectionDensity = 0.0f;
        float3 reconnectionWeight = float3(0.0f, 0.0f, 0.0f);
        float reconnectionCosine = 0.0f;
        bool forwardPasses = false;
        float previousCosine = 0.0f;

        if(reconnectionCandidate)
        {
            reconnectionDensity = RestirPtLobeDensity(surface, lobeSample.lobe, reconnectionDirection);

            if(reconnectionDensity > 0.0f)
            {
                float3 diffuseValue;
                reconnectionCosine = dot(surface.normal, reconnectionDirection);
                reconnectionWeight = RestirPtEvaluateLobe(surface, lobeSample.lobe, reconnectionDirection, diffuseValue) *
                    reconnectionCosine / reconnectionDensity;
            }

            if(footprintCriteria)
            {
                // Eq. 5 forward term at x_(vertex+1): x_vertex's marginal density along omega' times
                // G(x_vertex -> x_(vertex+1)), as RestirPtPairForward evaluates it in a shift.
                const float marginal = PathTracePdf(surface, reconnectionDirection);
                const float vertexCosine = abs(dot(reconnectionPayload.geometricNormal, reconnectionDirection));
                previousCosine = abs(dot(surface.geometricNormal, reconnectionDirection));
                forwardPasses = RestirPtFootprintPasses(marginal, vertexCosine, reconnectionDistance, footprintThreshold);
                RestirPtCount(RESTIR_PT_COUNTER_FOOTPRINT, 1u);
                RestirPtCount(RESTIR_PT_COUNTER_FOOTPRINT + 1u, forwardPasses ? 1u : 0u);
                const float ratio = marginal * vertexCosine * footprintThreshold > 0.0f ? abs(log10(reconnectionDistance *
                    reconnectionDistance / (marginal * vertexCosine * footprintThreshold))) : 1.0e30f;
                const uint proximityBin = ratio < 0.01f ? 0u : ratio < 0.03f ? 1u : ratio < 0.1f ? 2u : ratio < 0.3f ? 3u :
                    ratio < 1.0f ? 4u : 5u;
                RestirPtCount(RESTIR_PT_COUNTER_FOOTPRINT + 8u + proximityBin, 1u);
            }
        }

        if(!PathTraceBuildSurface(payload, ray.Direction, surface))
        {
            break;
        }

        if(reconnectionCandidate && reconnectionDensity > 0.0f && any(reconnectionWeight > 0.0f) &&
            (footprintCriteria ? forwardPasses : RestirPtSurfaceRough(surface)))
        {
            prefix.rc.index = vertex + 1u;
            prefix.rc.instance = identity.instance;
            prefix.rc.primitive = identity.primitive;
            prefix.rc.barycentrics = identity.barycentrics;
            const float vertexCosine = abs(dot(reconnectionPayload.geometricNormal, reconnectionDirection));
            prefix.rc.jacobian = reconnectionDensity * vertexCosine / (reconnectionDistance * reconnectionDistance);
            prefix.arrive = throughput * reconnectionWeight;
            prefix.reconnectionDirection = reconnectionDirection;
            prefix.vertexPayload = reconnectionPayload;
            prefix.rc.captureDirection = reconnectionDirection;
            prefix.rc.captureCosine = reconnectionCosine;
            prefix.previousCosine = previousCosine;
            prefix.reconnectionDistance = reconnectionDistance;
            prefix.footprintThreshold = footprintThreshold;
        }

        throughput = nextThroughput;
    }

    return result;
}

// -----------------------------------------------------------------------------
// M5.2 temporal neighbour (specification section 6.1)
// -----------------------------------------------------------------------------

// A primary surface from the previous frame's G-buffer, reconstructed as RtxdiBridgeLoadSurface does for ReSTIR DI (the
// previous camera, the previous G-buffer slot): its position, its surface seen from the previous camera, and its linear
// depth along the previous camera's forward axis.
struct RestirPtPrimarySurface
{
    bool valid;
    float3 position;
    PathTraceSurface surface;
    float linearDepth;
};

RestirPtPrimarySurface RestirPtLoadPreviousPrimary(int2 pixel)
{
    RestirPtPrimarySurface primary = (RestirPtPrimarySurface)0;

    if(restirPtHistoryValid == 0u || any(pixel < 0) || any(pixel >= int2(resolution)))
    {
        return primary;
    }

    Texture2D<float> depthTexture = ResourceDescriptorHeap[restirPtPreviousDepthIndex];
    Texture2D<float4> materialTexture = ResourceDescriptorHeap[restirPtPreviousMaterialIndex];
    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[restirPtPreviousSurfaceIndex];
    Texture2D<float4> geometricNormalTexture = ResourceDescriptorHeap[restirPtPreviousGeometricNormalIndex];
    const PathTraceGBufferSample gBuffer = PathTraceLoadGBuffer(uint2(pixel), depthTexture, materialTexture,
        surfaceTexture, geometricNormalTexture);

    // Background pixels have no surface.
    if(gBuffer.depth >= 1.0f)
    {
        return primary;
    }

    PathTracePrimaryCamera camera;
    camera.inverseCameraRelativeViewProjection = restirPtPreviousInverseCameraRelativeViewProjection;
    camera.cameraPosition = restirPtPreviousCameraPosition;
    camera.resolution = resolution;
    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[materialBufferIndex];
    PathTracePrimaryVertex vertex;

    if(!PathTraceReconstructPrimaryVertex(uint2(pixel), gBuffer, camera, materials, materialTableIndex,
        materialAverageTableIndex, materialTableCosineMapping, vertex))
    {
        return primary;
    }

    primary.valid = true;
    primary.position = vertex.position;
    primary.surface = vertex.surface;
    primary.linearDepth = dot(restirPtPreviousCameraForward, vertex.position - restirPtPreviousCameraPosition);
    return primary;
}

// The temporal neighbour of pixel b [O TemporalReuse; spec section 6.1]: the motion-vector reprojection with a random
// sub-pixel jitter, then the original's spatial-reuse compatibility test applied to it (decision Q-10): shading normals
// within 60 degrees (dot >= 0.5) and the previous linear depth within 10% of the expected one (current depth plus the
// G-buffer's view-depth change). stage: how far the lookup got (0 no history, 1 outside the view, 2 no previous surface,
// 3 incompatible, 4 accepted).
struct RestirPtTemporalNeighbor
{
    uint stage;
    int2 pixel;
    RestirPtPrimarySurface previous;
    // M5.4 C6: the accepted neighbour came from the dual motion vector; counter words (RESTIR_PT_COUNTER_DUAL + n).
    bool dual;
    uint dualProgress;
};

// The Q-10 compatibility test of a previous primary surface against this pixel's (as the original lookup applies it).
bool RestirPtTemporalCompatible(PathTraceRasterFirstVertex firstVertex, RestirPtPrimarySurface previous, float motionDepth)
{
    const float linearDepth = dot(restirPtCameraForward, firstVertex.position - cameraPosition);
    const float expectedPreviousDepth = linearDepth + motionDepth;
    return dot(firstVertex.surface.normal, previous.surface.normal) >= 0.5f &&
        abs(previous.linearDepth - expectedPreviousDepth) < 0.1f * abs(expectedPreviousDepth);
}

RestirPtTemporalNeighbor RestirPtFindTemporalNeighbor(uint2 pixel, PathTraceRasterFirstVertex firstVertex, uint seed)
{
    RestirPtTemporalNeighbor neighbor = (RestirPtTemporalNeighbor)0;

    if(restirPtHistoryValid == 0u || firstVertex.state <= 0)
    {
        return neighbor;
    }

    Texture2D<float4> motionTexture = ResourceDescriptorHeap[restirPtMotionIndex];
    const float4 motion = motionTexture.Load(int3(pixel, 0));
    const float2 jitter = float2(RestirPtRandom(seed, 0u, RESTIR_PT_DIMENSION_TEMPORAL),
        RestirPtRandom(seed, 0u, RESTIR_PT_DIMENSION_TEMPORAL + 1u)) - 0.5f;
    const float2 reprojected = float2(pixel) - motion.xy * float2(resolution) + jitter;
    neighbor.pixel = int2(round(reprojected));
    neighbor.stage = 1u;

    if(any(neighbor.pixel < 0) || any(neighbor.pixel >= int2(resolution)))
    {
        return neighbor;
    }

    neighbor.stage = 2u;
    neighbor.previous = RestirPtLoadPreviousPrimary(neighbor.pixel);

    if(!neighbor.previous.valid)
    {
        return neighbor;
    }

    neighbor.stage = 3u;

    if(RestirPtTemporalCompatible(firstVertex, neighbor.previous, motion.z))
    {
        neighbor.stage = 4u;
        return neighbor;
    }

    // M5.4 C6: the reprojection reached the previous surface at y but it is not this one (a disocclusion: y lies on the
    // occluder). Zeng et al. 2021 Eq. 6: z is where that occluder point is now (its instance's previous-to-current
    // transform, projected), and x' = y + (x - z) the previous location with the same position relative to the occluder.
    // Only the neighbour's location changes; the shift and Talbot MIS are those of any neighbour.
    if((restirPtFlags & RESTIR_PT_FLAG_DUAL_MOTION) != 0u && restirPtPreviousInstanceIndex != 0xFFFFFFFFu)
    {
        neighbor.dualProgress = 1u;
        RWByteAddressBuffer instances = ResourceDescriptorHeap[restirPtPreviousInstanceIndex];
        const uint occluder = instances.Load(4u * (uint(neighbor.pixel.y) * resolution.x + uint(neighbor.pixel.x)));

        // 0: background; InvalidIdentity: no exact TLAS mapping; beyond the transforms: not a current instance.
        if(occluder != 0u && occluder != 0xFFFFFFFFu && occluder - 1u < restirPtTransformCount)
        {
            neighbor.dualProgress = 2u;
            const float3 moved = mul(restirPtTransforms[occluder - 1u].previousToCurrent,
                float4(neighbor.previous.position, 1.0f));
            const float4 clip = mul(restirPtCameraRelativeViewProjection, float4(moved - cameraPosition, 1.0f));

            if(clip.w > 0.0f)
            {
                const float2 ndc = clip.xy / clip.w;
                const float2 z = (float2(ndc.x, -ndc.y) * 0.5f + 0.5f) * float2(resolution) - 0.5f;
                const int2 dualPixel = int2(round(reprojected + (float2(pixel) - z)));

                if(all(dualPixel >= 0) && all(dualPixel < int2(resolution)))
                {
                    neighbor.dualProgress = 3u;
                    const RestirPtPrimarySurface dualPrevious = RestirPtLoadPreviousPrimary(dualPixel);

                    if(dualPrevious.valid)
                    {
                        neighbor.dualProgress = 4u;

                        if(RestirPtTemporalCompatible(firstVertex, dualPrevious, motion.z))
                        {
                            neighbor.dualProgress = 5u;
                            neighbor.dual = true;
                            neighbor.pixel = dualPixel;
                            neighbor.previous = dualPrevious;
                            neighbor.stage = 4u;
                        }
                    }
                }
            }
        }
    }

    return neighbor;
}

// -----------------------------------------------------------------------------
// Reservoir storage (specification section 7.1)
// -----------------------------------------------------------------------------

// pathFlags (section 7.1): length k (0-3), rcIndex (4-7), M = 1 (8-15), NEE technique (16), x_(k-1) delta (17; 0, since
// a delta lobe never qualifies), lightType (18-19), lobes at x1 and x2 (20-23), replay-only (24), lambda_k (25-26).
uint RestirPtPathFlags(RestirPtCandidate selected)
{
    const bool nee = selected.endClass <= RESTIR_PT_END_ENVIRONMENT;
    uint lightType = selected.endClass == RESTIR_PT_END_SUN ? RESTIR_PT_LIGHT_SUN :
        selected.endClass <= RESTIR_PT_END_DIRECTIONAL ? RESTIR_PT_LIGHT_ANALYTIC : RESTIR_PT_LIGHT_ENVIRONMENT;

    if(selected.rc.lightType == RESTIR_PT_LIGHT_TABLE)
    {
        lightType = RESTIR_PT_LIGHT_TABLE;
    }

    return min(selected.length, 15u) | (min(selected.rc.index, 15u) << RESTIR_PT_PATH_RC_INDEX_SHIFT) | (1u << 8u) |
        (nee ? RESTIR_PT_PATH_NEE : 0u) | (lightType << RESTIR_PT_PATH_LIGHT_TYPE_SHIFT) |
        ((selected.lobes & 15u) << RESTIR_PT_PATH_LOBES_SHIFT) |
        (selected.rc.index == 0u ? RESTIR_PT_PATH_REPLAY_ONLY : 0u) |
        ((selected.rc.vertexLobe & 3u) << RESTIR_PT_PATH_VERTEX_LOBE_SHIFT);
}

// rcRandomSeed (specification sections 4.3 and 7.1; Algorithm 1's rcVertexRandomSeed): with the counter-based generator it
// is a pure function of initSeed (word 4) and rcIndex (pathFlags bits 4-7), so it is regenerated, never stored (P5).
uint RestirPtReconnectionRandomSeed(uint initSeed, uint rcIndex)
{
    return PathTraceHash(initSeed ^ PathTraceHash(rcIndex ^ 0x52435244u));
}

// The 64-byte reservoir (specification section 7.1, revised by T-2.1 P5), in 32-bit words:
//   0 W (f32); 1-3 F (the PSS integrand, 3 x f32); 4 initSeed; 5 rcVertex barycentric u (f32); 6 pathFlags;
//   7 rcVertexInstanceID (or the light index); 8 rcVertexPrimitiveIndex; 9 rcVertex barycentric v (f32);
//   10 rcWi (octahedral, 2 x unorm16); 11-13 rcRadiance (3 x f32); 14 cached Jacobian (f32); 15 lightPdf (f32).
// Word 5 held rcRandomSeed, which is regenerated (RestirPtReconnectionRandomSeed); the barycentrics are fp32 because 16-bit
// barycentrics move texture lookups on large tiled triangles (T-2.1 round 3).
void RestirPtStoreReservoir(uint2 pixel, uint seed, RestirPtCandidate selected, float unbiasedContributionWeight)
{
    if(restirPtReservoirIndex == 0xFFFFFFFFu)
    {
        return;
    }

    RWByteAddressBuffer reservoirs = ResourceDescriptorHeap[restirPtReservoirIndex];
    const uint address = (pixel.y * resolution.x + pixel.x) * RESTIR_PT_RESERVOIR_BYTES;
    const RestirPtReconnection rc = selected.rc;
    // The instance word is the light index for a light endpoint rebuilt from its index.
    reservoirs.Store4(address, uint4(asuint(unbiasedContributionWeight), asuint(selected.contribution)));
    reservoirs.Store4(address + 16u, uint4(seed, asuint(rc.barycentrics.x), RestirPtPathFlags(selected), rc.instance));
    reservoirs.Store4(address + 32u, uint4(rc.primitive, asuint(rc.barycentrics.y), RestirPtEncodeDirection(rc.wi),
        asuint(rc.radiance.r)));
    reservoirs.Store4(address + 48u, uint4(asuint(rc.radiance.g), asuint(rc.radiance.b), asuint(rc.jacobian),
        asuint(rc.lightPdf)));
}

// The stored reconnection data of a pixel's reservoir (what M5.2 reads).
RestirPtReconnection RestirPtLoadReconnection(uint2 pixel, out uint flags, out float3 contribution, out uint initSeed)
{
    RWByteAddressBuffer reservoirs = ResourceDescriptorHeap[restirPtReservoirIndex];
    const uint address = (pixel.y * resolution.x + pixel.x) * RESTIR_PT_RESERVOIR_BYTES;
    const uint4 word0 = reservoirs.Load4(address);
    const uint4 word1 = reservoirs.Load4(address + 16u);
    const uint4 word2 = reservoirs.Load4(address + 32u);
    const uint4 word3 = reservoirs.Load4(address + 48u);
    contribution = asfloat(word0.yzw);
    initSeed = word1.x;
    flags = word1.z;
    RestirPtReconnection rc = RestirPtNoReconnection();
    rc.index = (flags >> RESTIR_PT_PATH_RC_INDEX_SHIFT) & 15u;
    rc.instance = word1.w;
    rc.primitive = word2.x;
    rc.barycentrics = float2(asfloat(word1.y), asfloat(word2.y));
    rc.wi = RestirPtOctDecode(RestirPtUnpackUnorm2x16(word2.z));
    rc.radiance = float3(asfloat(word2.w), asfloat(word3.x), asfloat(word3.y));
    rc.jacobian = asfloat(word3.z);
    rc.lightPdf = asfloat(word3.w);
    rc.lightType = (flags >> RESTIR_PT_PATH_LIGHT_TYPE_SHIFT) & 3u;
    rc.vertexLobe = (flags >> RESTIR_PT_PATH_VERTEX_LOBE_SHIFT) & 3u;
    return rc;
}

// -----------------------------------------------------------------------------
// T-2.1 self-shift identity (specification section 12, M5.1-4; test only)
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Reconnection visibility (T-2.1 P2)
// -----------------------------------------------------------------------------

// Any-hit occlusion test over [origin, origin + direction * tMax], ignoring one triangle (excludedInstance,
// excludedPrimitive; 0xFFFFFFFF: none); the blocker's identity when occluded. With an exclusion every triangle is
// inspected as a candidate (RAY_FLAG_FORCE_NON_OPAQUE); every other triangle commits exactly as before (non-opaque
// triangles always commit here). blockerEdge (test only): the blocker hit's smallest barycentric coordinate, its distance
// from the blocker triangle's nearest edge in barycentric units.
bool RestirPtSegmentVisibleExcept(float3 origin, float3 direction, float tMax, uint excludedInstance,
    uint excludedPrimitive, out uint blockerInstance, out uint blockerPrimitive, out float blockerDistance,
    out float blockerEdge)
{
    blockerInstance = 0xFFFFFFFFu;
    blockerPrimitive = 0xFFFFFFFFu;
    blockerDistance = 0.0f;
    blockerEdge = 0.0f;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0f;
    ray.TMax = tMax;
    const bool exclude = excludedInstance != 0xFFFFFFFFu;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    RESTIR_PT_TRACE_INLINE(query, exclude ? RAY_FLAG_FORCE_NON_OPAQUE : RAY_FLAG_NONE, ray);

    while(query.Proceed())
    {
        if(query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            const bool excluded = exclude && query.CandidateInstanceID() == excludedInstance &&
                query.CandidatePrimitiveIndex() == excludedPrimitive;

            if(!excluded)
            {
                query.CommitNonOpaqueTriangleHit();
            }
        }
    }

    if(query.CommittedStatus() == COMMITTED_NOTHING)
    {
        return true;
    }

    const float2 barycentrics = query.CommittedTriangleBarycentrics();
    blockerInstance = query.CommittedInstanceID();
    blockerPrimitive = query.CommittedPrimitiveIndex();
    blockerDistance = query.CommittedRayT();
    blockerEdge = min(min(barycentrics.x, barycentrics.y), 1.0f - barycentrics.x - barycentrics.y);
    return false;
}

bool RestirPtSegmentVisible(float3 origin, float3 direction, float tMax, out uint blockerInstance,
    out uint blockerPrimitive)
{
    float blockerDistance;
    float blockerEdge;
    return RestirPtSegmentVisibleExcept(origin, direction, tMax, 0xFFFFFFFFu, 0xFFFFFFFFu, blockerInstance,
        blockerPrimitive, blockerDistance, blockerEdge);
}

// The reconnection visibility of y_(k-1) -> x_k (P2). The ray is [O]'s: from y_(k-1)'s offset origin aimed at x_k itself
// (the line the base path traced), up to 0.999 d. It ignores exactly one triangle, x_k's own: a planar triangle cannot
// occlude its own point (the segment from y_(k-1) to x_k meets that plane only at x_k), so excluding it removes the
// grazing self-intersection (where 0.001 d sin(theta_k) falls below the hit point's float error) without any epsilon,
// while adjacent triangles and every other object are still tested.
// blockerFraction: the blocker's distance as a fraction of |x_k - origin|; blockerEdge as RestirPtSegmentVisibleExcept.
bool RestirPtReconnectionVisible(float3 previousPosition, float3 previousGeometricNormal, float3 vertexPosition,
    uint vertexInstance, uint vertexPrimitive, out uint blockerInstance, out uint blockerPrimitive,
    out float blockerFraction, out float blockerEdge)
{
    blockerFraction = 0.0f;
    blockerEdge = 0.0f;
    const float3 origin = PathTraceOffsetRayOrigin(previousPosition, previousGeometricNormal);
    const float3 toVertex = vertexPosition - origin;
    const float distance = length(toVertex);

    if(!(distance > 0.0f))
    {
        blockerInstance = 0xFFFFFFFFu;
        blockerPrimitive = 0xFFFFFFFFu;
        return false;
    }

    float blockerDistance;
    const bool visible = RestirPtSegmentVisibleExcept(origin, toVertex / distance, distance * 0.999f, vertexInstance,
        vertexPrimitive, blockerInstance, blockerPrimitive, blockerDistance, blockerEdge);
    blockerFraction = blockerDistance / distance;
    return visible;
}

// Test only (P2 validation, the rejected alternative): x_k's end moved off its surface by the self-intersection offset
// (PathTraceOffsetRayOrigin, toward y_(k-1)'s side), the segment tested up to 0.999 of its length. It removes the
// self-intersection too, but its line differs from the base path's by up to the offset, which flips outcomes at
// silhouette edges along the ray (T-2.1 round 4, report B.8).
bool RestirPtOffsetEndpointVisible(float3 previousPosition, float3 previousGeometricNormal, float3 vertexPosition,
    float3 vertexGeometricNormal, out float3 end)
{
    const float3 origin = PathTraceOffsetRayOrigin(previousPosition, previousGeometricNormal);
    const float3 facingNormal = dot(vertexGeometricNormal, origin - vertexPosition) >= 0.0f ? vertexGeometricNormal :
        -vertexGeometricNormal;
    end = PathTraceOffsetRayOrigin(vertexPosition, facingNormal);
    const float3 segment = end - origin;
    const float segmentLength = length(segment);
    uint blockerInstance;
    uint blockerPrimitive;
    return segmentLength > 0.0f && RestirPtSegmentVisible(origin, segment / segmentLength, segmentLength * 0.999f,
        blockerInstance, blockerPrimitive);
}

// Test only (T-2.1 P2 validation): the independent reference for a reconnection's visibility. A closest-hit trace from
// the offset origin toward x_k: visible (1) when the first surface met is x_k's own triangle, or nothing before x_k;
// occluded when another triangle is met before x_k by more than contactBand (0: another object; 3: another triangle of
// x_k's own mesh); ambiguous (2) when another triangle is met within contactBand of x_k (adjacent or touching geometry).
uint RestirPtReferenceVisibility(float3 origin, float3 vertexPosition, uint vertexInstance, uint vertexPrimitive,
    float contactBand)
{
    const float3 toVertex = vertexPosition - origin;
    const float distance = length(toVertex);
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = toVertex / distance;
    ray.TMin = 0.0f;
    ray.TMax = distance + contactBand;
    RayQuery<RAY_FLAG_NONE> query;
    RESTIR_PT_TRACE_INLINE(query, RAY_FLAG_NONE, ray);

    while(query.Proceed())
    {
        if(query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            query.CommitNonOpaqueTriangleHit();
        }
    }

    if(query.CommittedStatus() == COMMITTED_NOTHING)
    {
        return 1u;
    }

    if(query.CommittedInstanceID() == vertexInstance && query.CommittedPrimitiveIndex() == vertexPrimitive)
    {
        return 1u;
    }

    if(!(query.CommittedRayT() < distance - contactBand))
    {
        return 2u;
    }

    return query.CommittedInstanceID() == vertexInstance ? 3u : 0u;
}

// Test only (P3 validation): a table light's direction and incident radiance at a receiver, restated independently of
// EvaluateSceneLightIncidence from the GPULight fields (Engine's light model: a range window (1 - (d / range)^4)^2 when
// range > 0, zero at or beyond |range|, inverse square with the minimum distance, the smoothstep-free spot cone factor
// squared; a directional light: its fixed direction and radiance).
// scale: the incident radiance without the spot cone factor (the light's radiance at this receiver before the cone), the
// scale against which the cross-receiver test measures errors: relative to Li itself, a near-zero cone or range-window
// factor turns 1-ulp differences in the direction into large relative errors.
float3 RestirPtReferenceIncident(GPULight light, float3 receiver, out float3 direction, out float distance,
    out float3 scale)
{
    if(light.type == SCENE_LIGHT_DIRECTIONAL)
    {
        direction = -light.direction;
        distance = PATH_TRACE_DISTANT_LIGHT_DISTANCE;
        scale = light.radiance;
        return light.radiance;
    }

    const float3 toLight = light.position - receiver;
    distance = length(toLight);
    direction = toLight / max(distance, 1.0e-30f);
    const float range = abs(light.range);

    scale = light.radiance / max(distance * distance, SCENE_LIGHT_MINIMUM_DISTANCE * SCENE_LIGHT_MINIMUM_DISTANCE);

    if(!(distance < range))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const float ratio = distance / range;
    const float fade = light.range > 0.0f ? saturate(1.0f - ratio * ratio * ratio * ratio) : 1.0f;
    const float minimumDistance = SCENE_LIGHT_MINIMUM_DISTANCE;
    float attenuation = fade * fade / max(distance * distance, minimumDistance * minimumDistance);

    if(light.type == SCENE_LIGHT_SPOT)
    {
        const float cone = saturate((dot(-direction, light.direction) - light.cosOuterCone) /
            max(light.cosInnerCone - light.cosOuterCone, 1.0e-4f));
        attenuation *= cone * cone;
    }

    return light.radiance * attenuation;
}

// Test only (P3 validation): the reference visibility of a light from a receiver, a closest-hit trace from the offset
// origin: 1 visible (nothing before the light), 0 occluded (a hit before 0.999 of its distance), 2 ambiguous (a hit
// within the last 0.1%).
uint RestirPtReferenceLightVisibility(float3 receiver, float3 geometricNormal, float3 direction, float distance)
{
    RayDesc ray;
    ray.Origin = PathTraceOffsetRayOrigin(receiver, geometricNormal);
    ray.Direction = direction;
    ray.TMin = 0.0f;
    ray.TMax = distance;
    RayQuery<RAY_FLAG_NONE> query;
    RESTIR_PT_TRACE_INLINE(query, RAY_FLAG_NONE, ray);

    while(query.Proceed())
    {
        if(query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            query.CommitNonOpaqueTriangleHit();
        }
    }

    if(query.CommittedStatus() == COMMITTED_NOTHING)
    {
        return 1u;
    }

    return query.CommittedRayT() < distance * 0.999f ? 0u : 2u;
}

// T-2.1 P4: a distant-light NEE sample's direction (sun disk or environment), regenerated from the path's initSeed at the
// NEE vertex exactly as the path tree drew it: the same random dimensions and the frame's sun and environment. Its density
// comes with it (for the MIS recomputation). Neither depends on the receiving position.
bool RestirPtDistantLightDirection(uint lightType, uint initSeed, uint vertex, out float3 direction, out float pdf)
{
    PathTraceLightSample lightSample;

    if(lightType == RESTIR_PT_LIGHT_SUN)
    {
        lightSample = PathTraceSampleSun(PathTraceFrameSun(), float2(RestirPtRandom(initSeed, vertex,
            RESTIR_PT_DIMENSION_SUN), RestirPtRandom(initSeed, vertex, RESTIR_PT_DIMENSION_SUN + 1u)));
    }
    else
    {
        lightSample = PathTraceSampleEnvironment(PathTraceFrameEnvironment(), environmentSampler, float4(
            RestirPtRandom(initSeed, vertex, RESTIR_PT_DIMENSION_ENVIRONMENT),
            RestirPtRandom(initSeed, vertex, RESTIR_PT_DIMENSION_ENVIRONMENT + 1u),
            RestirPtRandom(initSeed, vertex, RESTIR_PT_DIMENSION_ENVIRONMENT + 2u),
            RestirPtRandom(initSeed, vertex, RESTIR_PT_DIMENSION_ENVIRONMENT + 3u)));
    }

    direction = lightSample.direction;
    pdf = lightSample.pdf;
    return lightSample.valid;
}

// Test only (T-2.1a edge ties): the world-space distance from location to the nearest edge of a stored triangle (instance,
// primitive), from the per-instance transforms, and the triangle's unit normal; a negative value when the triangle cannot
// be rebuilt.
float RestirPtTriangleEdgeDistance(uint instanceID, uint primitive, float3 location, out float3 normal)
{
    normal = float3(0.0f, 0.0f, 1.0f);
    float3 corners[3];

    if(!RestirPtVertexPosition(instanceID, primitive, float2(0.0f, 0.0f), corners[0]) ||
        !RestirPtVertexPosition(instanceID, primitive, float2(1.0f, 0.0f), corners[1]) ||
        !RestirPtVertexPosition(instanceID, primitive, float2(0.0f, 1.0f), corners[2]))
    {
        return -1.0f;
    }

    normal = normalize(cross(corners[1] - corners[0], corners[2] - corners[0]));
    float nearest = 1.0e30f;

    for(uint edge = 0u; edge < 3u; ++edge)
    {
        const float3 start = corners[edge];
        const float3 span = corners[(edge + 1u) % 3u] - start;
        const float along = saturate(dot(location - start, span) / max(dot(span, span), 1.0e-30f));
        nearest = min(nearest, length(location - (start + span * along)));
    }

    return nearest;
}

// The angle between two unit directions (accurate near 0, unlike acos).
float RestirPtAngle(float3 a, float3 b)
{
    return atan2(length(cross(a, b)), dot(a, b));
}

float RestirPtRelativeError(float value, float target)
{
    return target > 0.0f ? abs(value - target) / target : 0.0f;
}

// One evaluation of a hit reconnection y_(k-1) -> x_k (specification section 5, step 3): x_(k-1)'s lobe density and
// weight toward previousDirection, x_k's surface as seen along vertexDirection, and x_k's NEE or continuation toward rcWi.
// A shift uses the vertex-to-vertex direction for both; the round-3 counterfactuals put the base path's sampled
// direction in one or both.
struct RestirPtHitEvaluation
{
    // RESTIR_PT_SHIFT_REASON_IDENTITY when evaluated; otherwise PREVIOUS_LOBE, REBUILD or VERTEX_DENSITY.
    uint reason;
    // previousWeight * local * rcRadiance (without the replayed prefix throughput).
    float3 value;
    // P p(previous) G, times x_k's continuation density when x_k continues.
    float jacobian;
    float previousDensity;
    float vertexDensity;
    uint vertexLobe;
    float vertexAlpha;
    float specularProbability;
    float3 vertexNormal;
    // The two factors: x_(k-1)'s lobe weight and x_k's continuation weight (or f_all cos MIS for NEE at x_k).
    float3 previousWeight;
    float3 local;
};

RestirPtHitEvaluation RestirPtEvaluateHit(PathTraceSurface surface, uint previousLobe, float3 previousDirection,
    PathTracePayload rebuilt, float3 vertexDirection, float geometry, RestirPtReconnection rc, uint seed, bool neeAtVertex)
{
    RestirPtHitEvaluation evaluation = (RestirPtHitEvaluation)0;
    evaluation.reason = RESTIR_PT_SHIFT_REASON_PREVIOUS_LOBE;
    evaluation.previousDensity = RestirPtLobeDensity(surface, previousLobe, previousDirection);

    if(!(evaluation.previousDensity > 0.0f))
    {
        return evaluation;
    }

    float3 diffuseValue;
    const float3 previousWeight = RestirPtEvaluateLobe(surface, previousLobe, previousDirection, diffuseValue) *
        dot(surface.normal, previousDirection) / evaluation.previousDensity;
    PathTraceSurface vertexSurface;

    if(!PathTraceBuildSurface(rebuilt, vertexDirection, vertexSurface))
    {
        evaluation.reason = RESTIR_PT_SHIFT_REASON_REBUILD;
        return evaluation;
    }

    evaluation.vertexAlpha = vertexSurface.material.roughness * vertexSurface.material.roughness;
    evaluation.specularProbability = vertexSurface.specularProbability;
    evaluation.vertexNormal = vertexSurface.normal;
    evaluation.jacobian = evaluation.previousDensity * geometry;
    float3 local;

    if(neeAtVertex)
    {
        // NEE leaves x_k: f_all cos toward the light and the recomputed MIS weight.
        float3 diffuse;
        const float mis = rc.lightPdf > 0.0f ? RestirPtBalanceHeuristic(rc.lightPdf, PathTracePdf(vertexSurface, rc.wi)) :
            1.0f;
        local = RestirPtBsdfCos(vertexSurface, rc.wi, diffuse) * mis;
    }
    else
    {
        // x_k continues by its stored lobe lambda_k (P1; specification section 5, step 3) toward rcWi; it fails when
        // that lobe has no density there. Diagnostic only: the lobe a re-selection from the seed would pick. (M5.4: the
        // production kernels already drop it as dead code; gating it behind RESTIR_PT_DIAGNOSTICS changed the pre-pass's
        // codegen and measured 1.0-1.8 ms slower, so it stays.)
        const RestirPtLobeSample replayedLobe = RestirPtSampleLobe(vertexSurface, float3(RestirPtRandom(seed, rc.index,
            RESTIR_PT_DIMENSION_LOBE), RestirPtRandom(seed, rc.index, RESTIR_PT_DIMENSION_DIRECTION),
            RestirPtRandom(seed, rc.index, RESTIR_PT_DIMENSION_DIRECTION + 1u)));
        evaluation.vertexLobe = replayedLobe.lobe;
        evaluation.vertexDensity = RestirPtLobeDensity(vertexSurface, rc.vertexLobe, rc.wi);

        if(!(evaluation.vertexDensity > 0.0f))
        {
            evaluation.reason = RESTIR_PT_SHIFT_REASON_VERTEX_DENSITY;
            return evaluation;
        }

        local = RestirPtEvaluateLobe(vertexSurface, rc.vertexLobe, rc.wi, diffuseValue) *
            dot(vertexSurface.normal, rc.wi) / evaluation.vertexDensity;
        evaluation.jacobian *= evaluation.vertexDensity;
    }

    evaluation.value = previousWeight * local * rc.radiance;
    evaluation.previousWeight = previousWeight;
    evaluation.local = local;
    evaluation.reason = RESTIR_PT_SHIFT_REASON_IDENTITY;
    return evaluation;
}

// |F'/F - 1| and |J - 1| of a counterfactual evaluation (a failed evaluation: both infinite).
float2 RestirPtEvaluationErrors(RestirPtHitEvaluation evaluation, float3 throughput, float target, float cachedJacobian)
{
    if(evaluation.reason != RESTIR_PT_SHIFT_REASON_IDENTITY)
    {
        return float2(1.0e30f, 1.0e30f);
    }

    return float2(RestirPtRelativeError(PathTraceLuminance(throughput * evaluation.value), target),
        abs(evaluation.jacobian / cachedJacobian - 1.0f));
}

bool RestirPtWithinTolerance(float2 errors)
{
    return errors.x <= RESTIR_PT_SHIFT_TOLERANCE && errors.y <= RESTIR_PT_SHIFT_TOLERANCE;
}

// -----------------------------------------------------------------------------
// M5.2 production shift (specification section 5)
// -----------------------------------------------------------------------------

// The result of shifting a stored sample into a destination primary vertex y1.
struct RestirPtShifted
{
    // The destination produced the path: F' is its PSS integrand and jacobian |du_y / du_x| the reconnection's ratio of
    // destination to source terms (1 for random replay). Otherwise F' = 0 and the sample contributes nothing here.
    bool valid;
    float3 contribution;
    float jacobian;
    // The destination path's reconnection data: the source's, with the cached terms replaced by the destination's (a
    // reservoir that adopts the shifted sample stores this).
    RestirPtReconnection rc;
    // Why the shift failed (RESTIR_PT_SHIFT_REASON_*; IDENTITY when valid).
    uint reason;
};

// The destination's own reconnection criteria at the pair (y_(j-1) -> y_j) (specification section 4.7, as capture applies
// them, D2 included): a rough sampled lobe at y_(j-1), |y_j - y_(j-1)| >= d_min, y_j rough, and a positive lobe density and
// weight along the vertex-to-vertex direction.
bool RestirPtPairQualifies(PathTraceSurface previousSurface, uint previousLobe, float3 previousPosition,
    float3 vertexPosition, PathTraceSurface vertexSurface)
{
    if(!RestirPtLobeRough(previousSurface, previousLobe) || !RestirPtSurfaceRough(vertexSurface))
    {
        return false;
    }

    float distance;
    const float3 direction = RestirPtReconnectionDirection(previousPosition, vertexPosition, distance);

    if(!(distance >= restirPtReconnectionMinDistance) || !(distance > 0.0f))
    {
        return false;
    }

    const float density = RestirPtLobeDensity(previousSurface, previousLobe, direction);
    float3 diffuseValue;
    return density > 0.0f && any(RestirPtEvaluateLobe(previousSurface, previousLobe, direction, diffuseValue) > 0.0f);
}

// M5.4 C4: the forward half of the published criteria at the pair (y_(j-1) -> y_j), as capture applies it: the
// single-vertex roughness of y_(j-1)'s lobe, a positive lobe density and weight along the vertex-to-vertex direction,
// and Eq. 5's forward ray footprint with y_(j-1)'s marginal density. Outputs |<n_(y_(j-1)), omega'>| and the distance
// for y_j's inverse footprint, which needs y_j's outgoing direction (evaluated by the caller once it is known).
bool RestirPtPairForward(PathTraceSurface previousSurface, uint previousLobe, float3 previousPosition, float3 vertexPosition,
    PathTraceSurface vertexSurface, float threshold, out float previousCosine, out float distance)
{
    previousCosine = 0.0f;
    distance = 0.0f;

    if(!RestirPtLobeRough(previousSurface, previousLobe))
    {
        return false;
    }

    const float3 direction = RestirPtReconnectionDirection(previousPosition, vertexPosition, distance);

    if(!(distance > 0.0f))
    {
        return false;
    }

    const float density = RestirPtLobeDensity(previousSurface, previousLobe, direction);
    float3 diffuseValue;

    if(!(density > 0.0f) || !any(RestirPtEvaluateLobe(previousSurface, previousLobe, direction, diffuseValue) > 0.0f))
    {
        return false;
    }

    previousCosine = abs(dot(previousSurface.geometricNormal, direction));
    return RestirPtFootprintPasses(PathTracePdf(previousSurface, direction), abs(dot(vertexSurface.geometricNormal,
        direction)), distance, threshold);
}

#if defined(RESTIR_PT_DIAGNOSTICS)
// M5.3 escape-MIS study (diagnostics variant): shifts whose path escapes from y_(k-1) or x_k (14174), and those whose
// escape radiance with the destination's MIS weights differs from the stored source-side value by more than 1e-3 (14175).
void RestirPtCountEscapeMis(float3 stored, float3 recomputed)
{
    RestirPtCount(RESTIR_PT_COUNTER_SPATIAL + 14u, 1u);
    RestirPtCount(RESTIR_PT_COUNTER_SPATIAL + 15u, RestirPtRelativeError(PathTraceLuminance(recomputed),
        PathTraceLuminance(stored)) > 1.0e-3f ? 1u : 0u);
}
#endif

// Shifts a stored sample (initSeed, pathFlags, rc: RestirPtLoadReconnection's fields) into the destination primary
// vertex [P 3-4; S 2; spec section 5]:
//   1. random replay of y1 .. y_(k-1) with the source's random numbers, the same lobes (the stored lambda_1, lambda_2),
//      no roulette, failing on a miss, a lobe change, or an earlier pair that qualifies under the destination's own
//      criteria (invertibility);
//   2. the reconnection y_(k-1) -> x_k: x_k rebuilt from its identity, the pair must qualify, visibility (P2), x_(k-1)'s
//      lobe and x_k's stored lobe (P1) or NEE factor evaluated along omega', J = destination terms / cached terms;
//   light endpoints: the table light rebuilt from its index (P3) or the distant light's direction regenerated from the
//      seed (P4), with a shadow ray from y_(k-1) (J = 1, the forced rule); environment endpoints: the stored lobe's weight
//      toward rcWi and a miss ray (J = density ratio); replay-only: the whole path replayed to its escape (J = 1).
// The destination scene is the current one (instances at their current transforms) [O; spec section 6.1].
RestirPtShifted RestirPtShiftSample(PathTraceRasterFirstVertex destination, uint initSeed, uint pathFlags,
    RestirPtReconnection rc, float footprintThreshold)
{
    RestirPtShifted shifted = (RestirPtShifted)0;
    shifted.rc = rc;
    shifted.reason = RESTIR_PT_SHIFT_REASON_REPLAY;
    const uint pathLength = pathFlags & 15u;
    const bool nee = (pathFlags & RESTIR_PT_PATH_NEE) != 0u;
    const uint storedLobes = (pathFlags >> RESTIR_PT_PATH_LOBES_SHIFT) & 15u;

    if(pathLength < 2u || destination.state <= 0)
    {
        return shifted;
    }

    // Replay-only paths end in a BSDF escape (NEE-ended paths always reconnect, by the forced rule); a replay-only NEE
    // candidate (D2) would need the light tile's random selection, which depends on the source pixel: not reproducible.
    const bool replayOnly = rc.index == 0u;

    if(replayOnly && nee)
    {
        return shifted;
    }

    // The last vertex the replay reaches: y_(k-1), or for replay-only the vertex the escape leaves.
    const uint replayEnd = replayOnly ? pathLength - 1u : rc.index - 1u;
    PathTraceSurface surface = destination.surface;
    float3 position = destination.position;
    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    // M5.4 C4 (published criteria): invertibility needs the destination's own decision at each replayed pair, and a
    // pair's inverse footprint needs its vertex's outgoing direction, known one step later (the next replayed lobe, or
    // after the loop the direction the reconnection, endpoint or escape leaves y_(k-1) along). A pending pair passed the
    // forward half (RestirPtPairForward).
    bool pendingPair = false;
    float pendingCosine = 0.0f;
    float pendingDistance = 0.0f;

    for(uint vertex = 1u; vertex < replayEnd; ++vertex)
    {
        const RestirPtLobeSample lobeSample = RestirPtSampleLobe(surface, float3(RestirPtRandom(initSeed, vertex,
            RESTIR_PT_DIMENSION_LOBE), RestirPtRandom(initSeed, vertex, RESTIR_PT_DIMENSION_DIRECTION),
            RestirPtRandom(initSeed, vertex, RESTIR_PT_DIMENSION_DIRECTION + 1u)));

        if(!lobeSample.valid || (vertex <= 2u && lobeSample.lobe != ((storedLobes >> (2u * (vertex - 1u))) & 3u)))
        {
            return shifted;
        }

        // The previous pair (y_(vertex-1), y_vertex) with y_vertex's BSDF-sampled outgoing direction: if it qualifies,
        // the destination would reconnect earlier than the source did.
        if(pendingPair && RestirPtInverseFootprintPasses(surface, lobeSample.direction, pendingCosine, pendingDistance,
            footprintThreshold))
        {
            return shifted;
        }

        throughput *= lobeSample.weight;
        RayDesc ray;
        ray.Origin = PathTraceOffsetRayOrigin(position, surface.geometricNormal);
        ray.Direction = lobeSample.direction;
        ray.TMin = 0.0f;
        ray.TMax = PATH_TRACE_DISTANT_LIGHT_DISTANCE;
        PathTracePayload payload = (PathTracePayload)0;
        payload.hitDistance = -1.0f;
        RestirPtHitIdentity identity;
        RestirPtTraceInline(ray, payload, identity);

        if(payload.hitDistance < 0.0f)
        {
            return shifted;
        }

        const float3 previousPosition = position;
        const PathTraceSurface previousSurface = surface;
        position = ray.Origin + ray.Direction * payload.hitDistance;

        if(!PathTraceBuildSurface(payload, ray.Direction, surface))
        {
            return shifted;
        }

        // Invertibility: the destination must not reconnect earlier than the source did.
        if(RestirPtFootprintCriteria())
        {
            pendingPair = RestirPtPairForward(previousSurface, lobeSample.lobe, previousPosition, position, surface,
                footprintThreshold, pendingCosine, pendingDistance);
        }
        else if(RestirPtPairQualifies(previousSurface, lobeSample.lobe, previousPosition, position, surface))
        {
            return shifted;
        }
    }

    // y_(k-1): the lobe it samples (the source's lambda_(k-1)).
    const uint lastVertex = replayEnd;
    // M5.4 C2: the replayed lobe sample is used only by a replay-only escape and by reconnections whose lambda_(k-1) is
    // not stored (lastVertex > 2); every other shift (light endpoints, the common case) discarded it.
    RestirPtLobeSample previousLobe = (RestirPtLobeSample)0;

    if(replayOnly || lastVertex > 2u)
    {
        previousLobe = RestirPtSampleLobe(surface, float3(RestirPtRandom(initSeed, lastVertex, RESTIR_PT_DIMENSION_LOBE),
            RestirPtRandom(initSeed, lastVertex, RESTIR_PT_DIMENSION_DIRECTION),
            RestirPtRandom(initSeed, lastVertex, RESTIR_PT_DIMENSION_DIRECTION + 1u)));
    }

    const bool lobeMatches = lastVertex > 2u || previousLobe.lobe == ((storedLobes >> (2u * (lastVertex - 1u))) & 3u);
    // A reconnection at y_(k-1) keeps the stored lobe lambda_(k-1) as an index ([S 1, Eq. 4]: the joint density
    // p(omega', l_(k-1)) in the Jacobian; [S 3]: the lobe index is preserved). M5.3 fix (user-approved 2026-11-22): M5.2
    // re-selected the lobe from the replayed random number and required a match, an identity mapping inconsistent with
    // that Jacobian (forward validity u < P_dst, reverse u < P_src), invisible for temporal reuse (P_dst ~ P_src) and
    // biased for spatial partners. Lobes beyond x2 are not stored (depth > 3 only): those keep the re-selection.
    const bool lobeIndex = lastVertex <= 2u;
    const uint reconnectLobe = lobeIndex ? ((storedLobes >> (2u * (lastVertex - 1u))) & 3u) : previousLobe.lobe;
    const bool reconnectLobeValid = lobeIndex || (previousLobe.valid && lobeMatches);
    const bool endpoint = rc.index == pathLength;
    const bool neeAtVertex = nee && rc.index == pathLength - 1u;

    // P4 for NEE leaving x_k toward a distant light: the direction regenerated from the seed at x_k (pathLength - 1).
    if(neeAtVertex && (rc.lightType == RESTIR_PT_LIGHT_SUN || rc.lightType == RESTIR_PT_LIGHT_ENVIRONMENT))
    {
        float3 regenerated;
        float regeneratedPdf;

        if(!RestirPtDistantLightDirection(rc.lightType, initSeed, pathLength - 1u, regenerated, regeneratedPdf))
        {
            return shifted;
        }

        rc.wi = regenerated;
    }

    if(replayOnly)
    {
        // The escape at the end of the replay, technique BSDF: the lobe's weight and the escape radiance, and the
        // destination must not have qualified an environment endpoint (the source did not).
        if(!previousLobe.valid || !lobeMatches || RestirPtLobeRough(surface, previousLobe.lobe))
        {
            return shifted;
        }

        // M5.4 C4: the last replayed pair, with y_(k-1)'s BSDF-sampled escape direction.
        if(pendingPair && RestirPtInverseFootprintPasses(surface, previousLobe.direction, pendingCosine, pendingDistance,
            footprintThreshold))
        {
            return shifted;
        }

        RayDesc escapeRay;
        escapeRay.Origin = PathTraceOffsetRayOrigin(position, surface.geometricNormal);
        escapeRay.Direction = previousLobe.direction;
        escapeRay.TMin = 0.0f;
        escapeRay.TMax = PATH_TRACE_DISTANT_LIGHT_DISTANCE;
        PathTracePayload escapePayload = (PathTracePayload)0;
        escapePayload.hitDistance = -1.0f;
        RestirPtHitIdentity escapeIdentity;
        RestirPtTraceInline(escapeRay, escapePayload, escapeIdentity);

        if(escapePayload.hitDistance >= 0.0f)
        {
            shifted.reason = RESTIR_PT_SHIFT_REASON_VISIBILITY;
            return shifted;
        }

        shifted.contribution = throughput * previousLobe.weight * RestirPtEscapeRadiance(previousLobe.direction,
            previousLobe.lobe == RESTIR_PT_LOBE_DELTA, previousLobe.pdfAll);
        shifted.jacobian = 1.0f;
    }
    else if(endpoint && nee)
    {
        // A light endpoint (forced NEE): the destination's own NEE toward the same light. y_(k-1) samples no lobe on this
        // path (capture forms NEE candidates before the vertex's lobe is drawn, so lambda_(k-1) is not stored); the replayed
        // lobes before it were checked above.
        float3 direction = rc.wi;
        float3 incident = float3(1.0f, 1.0f, 1.0f);
        float distance = PATH_TRACE_DISTANT_LIGHT_DISTANCE;
        shifted.reason = RESTIR_PT_SHIFT_REASON_REBUILD;

        if(rc.lightType == RESTIR_PT_LIGHT_TABLE)
        {
            // P3: the light rebuilt from its index at y_(k-1).
            if(rc.instance >= sceneLightCount)
            {
                return shifted;
            }

            const PathTraceLightSample lightSample = PathTraceSampleTableLight(SceneLightLoad(sceneLightBufferIndex,
                rc.instance), position);

            if(!lightSample.valid)
            {
                return shifted;
            }

            direction = lightSample.direction;
            incident = lightSample.radiance;
            distance = lightSample.distance;
        }
        else if(rc.lightType == RESTIR_PT_LIGHT_SUN || rc.lightType == RESTIR_PT_LIGHT_ENVIRONMENT)
        {
            // P4: the distant light's direction regenerated from the seed at the NEE vertex.
            float regeneratedPdf;

            if(!RestirPtDistantLightDirection(rc.lightType, initSeed, pathLength - 1u, direction, regeneratedPdf))
            {
                return shifted;
            }
        }
        else
        {
            // Analytic table lights outside P3 keep their stored direction (none are created since P3 covers every table
            // light type; kept for completeness).
            distance = PATH_TRACE_DISTANT_LIGHT_DISTANCE;
        }

        // M5.4 C4: the last replayed pair leaves y_(k-1) by NEE toward this light: Option B decides on the forward half
        // alone; A' (test only) adds the inverse footprint with the BSDF density toward the light, as capture does.
        if(pendingPair && ((restirPtFlags & RESTIR_PT_FLAG_NEE_INVERSE_PROXY) == 0u ||
            RestirPtInverseFootprintPasses(surface, direction, pendingCosine, pendingDistance, footprintThreshold)))
        {
            shifted.reason = RESTIR_PT_SHIFT_REASON_REPLAY;
            return shifted;
        }

        uint rays = 0u;

        if(!PathTraceVisible(position, surface.geometricNormal, direction, distance, rays))
        {
            shifted.reason = RESTIR_PT_SHIFT_REASON_VISIBILITY;
            return shifted;
        }

        float3 diffuse;
        const float mis = rc.lightPdf > 0.0f ? RestirPtBalanceHeuristic(rc.lightPdf, PathTracePdf(surface, direction)) :
            1.0f;
        shifted.contribution = throughput * RestirPtBsdfCos(surface, direction, diffuse) * incident * rc.radiance * mis;
        shifted.jacobian = 1.0f;
    }
    else if(endpoint)
    {
        // An environment endpoint (BSDF escape): the stored lobe toward rcWi, which must leave the scene.
        shifted.reason = RESTIR_PT_SHIFT_REASON_PREVIOUS_LOBE;

        if(!reconnectLobeValid || !RestirPtLobeRough(surface, reconnectLobe))
        {
            return shifted;
        }

        const float density = RestirPtLobeDensity(surface, reconnectLobe, rc.wi);

        if(!(density > 0.0f))
        {
            return shifted;
        }

        // M5.4 C4: the last replayed pair, with y_(k-1)'s BSDF-sampled direction toward the environment.
        if(pendingPair && RestirPtInverseFootprintPasses(surface, rc.wi, pendingCosine, pendingDistance, footprintThreshold))
        {
            shifted.reason = RESTIR_PT_SHIFT_REASON_REPLAY;
            return shifted;
        }

        RayDesc escapeRay;
        escapeRay.Origin = PathTraceOffsetRayOrigin(position, surface.geometricNormal);
        escapeRay.Direction = rc.wi;
        escapeRay.TMin = 0.0f;
        escapeRay.TMax = PATH_TRACE_DISTANT_LIGHT_DISTANCE;
        PathTracePayload escapePayload = (PathTracePayload)0;
        escapePayload.hitDistance = -1.0f;
        RestirPtHitIdentity escapeIdentity;
        RestirPtTraceInline(escapeRay, escapePayload, escapeIdentity);

        if(escapePayload.hitDistance >= 0.0f)
        {
            shifted.reason = RESTIR_PT_SHIFT_REASON_VISIBILITY;
            return shifted;
        }

        float3 diffuseValue;
        // The escape's MIS weights against sun and environment NEE, recomputed with y_(k-1)'s own mixture density
        // (specification section 5, step 3). M5.3 fix (user-approved 2026-11-22): M5.2 used rcRadiance, into which
        // capture folds the source vertex's weights.
        const float3 escapeRadiance = RestirPtEscapeRadiance(rc.wi, false, PathTracePdf(surface, rc.wi));
#if defined(RESTIR_PT_DIAGNOSTICS)
        RestirPtCountEscapeMis(rc.radiance, RestirPtEscapeRadiance(rc.wi, false, PathTracePdf(surface, rc.wi)));
#endif
        shifted.contribution = throughput * RestirPtEvaluateLobe(surface, reconnectLobe, rc.wi, diffuseValue) *
            dot(surface.normal, rc.wi) / density * escapeRadiance;
        shifted.jacobian = density / rc.jacobian;
        shifted.rc.jacobian = density;
    }
    else
    {
        // A hit reconnection y_(k-1) -> x_k.
        shifted.reason = RESTIR_PT_SHIFT_REASON_PREVIOUS_LOBE;

        if(!reconnectLobeValid || reconnectLobe == RESTIR_PT_LOBE_DELTA || !(rc.jacobian > 0.0f))
        {
            return shifted;
        }

        float3 vertexPosition;
        PathTracePayload rebuilt;
        shifted.reason = RESTIR_PT_SHIFT_REASON_REBUILD;

        if(!RestirPtRebuildVertex(rc.instance, rc.primitive, rc.barycentrics, position, vertexPosition, rebuilt))
        {
            return shifted;
        }

        float distance;
        const float3 direction = RestirPtReconnectionDirection(position, vertexPosition, distance);
        PathTraceSurface vertexSurface;

        // M5.4 C4: the last replayed pair, with y_(k-1)'s direction toward x_k.
        if(pendingPair && RestirPtInverseFootprintPasses(surface, direction, pendingCosine, pendingDistance,
            footprintThreshold))
        {
            shifted.reason = RESTIR_PT_SHIFT_REASON_REPLAY;
            return shifted;
        }

        // The reconnection pair must qualify under the destination's criteria (invertibility).
        bool pairQualifies = PathTraceBuildSurface(rebuilt, direction, vertexSurface);

        if(pairQualifies && RestirPtFootprintCriteria())
        {
            float previousCosine;
            float pairDistance;
            pairQualifies = RestirPtPairForward(surface, reconnectLobe, position, vertexPosition, vertexSurface,
                footprintThreshold, previousCosine, pairDistance);

            // x_k's inverse footprint for its BSDF-sampled omega_k (rcWi); NEE leaving x_k skips it (Option B) or, test
            // only, uses the BSDF density toward the light (A').
            if(pairQualifies && (!neeAtVertex || (restirPtFlags & RESTIR_PT_FLAG_NEE_INVERSE_PROXY) != 0u))
            {
                pairQualifies = RestirPtInverseFootprintPasses(vertexSurface, rc.wi, previousCosine, pairDistance,
                    footprintThreshold);
            }
        }
        else if(pairQualifies)
        {
            pairQualifies = RestirPtPairQualifies(surface, reconnectLobe, position, vertexPosition, vertexSurface);
        }

        if(!pairQualifies)
        {
            shifted.reason = RESTIR_PT_SHIFT_REASON_PREVIOUS_LOBE;
            return shifted;
        }

        uint blockerInstance;
        uint blockerPrimitive;
        float blockerFraction;
        float blockerEdge;

        if(!RestirPtReconnectionVisible(position, surface.geometricNormal, vertexPosition, rc.instance, rc.primitive,
            blockerInstance, blockerPrimitive, blockerFraction, blockerEdge))
        {
            shifted.reason = RESTIR_PT_SHIFT_REASON_VISIBILITY;
            return shifted;
        }

        const float geometry = abs(dot(rebuilt.geometricNormal, direction)) / (distance * distance);
        const RestirPtHitEvaluation evaluation = RestirPtEvaluateHit(surface, reconnectLobe, direction, rebuilt,
            direction, geometry, rc, initSeed, neeAtVertex);

        if(evaluation.reason != RESTIR_PT_SHIFT_REASON_IDENTITY)
        {
            shifted.reason = evaluation.reason;
            return shifted;
        }

        float3 value = evaluation.value;

#if defined(RESTIR_PT_DIAGNOSTICS)
        if(!nee && pathLength == rc.index + 1u)
        {
            RestirPtCountEscapeMis(rc.radiance, RestirPtEscapeRadiance(rc.wi, false, PathTracePdf(vertexSurface, rc.wi)));
        }
#endif

        // x_k continues straight to an escape, whose MIS weights depend on x_k's mixture density toward rcWi as seen
        // from y_(k-1): recomputed (specification section 5, step 3; the M5.3 fix above).
        if(!nee && pathLength == rc.index + 1u)
        {
            value = evaluation.previousWeight * evaluation.local * RestirPtEscapeRadiance(rc.wi, false,
                PathTracePdf(vertexSurface, rc.wi));
        }

        shifted.contribution = throughput * value;
        shifted.jacobian = evaluation.jacobian / rc.jacobian;
        shifted.rc.jacobian = evaluation.jacobian;
    }

    shifted.valid = all(isfinite(shifted.contribution)) && isfinite(shifted.jacobian) && shifted.jacobian > 0.0f;
    shifted.reason = shifted.valid ? RESTIR_PT_SHIFT_REASON_IDENTITY : RESTIR_PT_SHIFT_REASON_NON_FINITE;

    if(!shifted.valid)
    {
        shifted.contribution = float3(0.0f, 0.0f, 0.0f);
    }

    return shifted;
}

// -----------------------------------------------------------------------------
// M5.2 temporal reuse (specification section 6.1)
// -----------------------------------------------------------------------------

// A stored reservoir: W, F (the PSS integrand), initSeed, pathFlags (M in bits 8-15) and the reconnection data.
struct RestirPtStoredReservoir
{
    float unbiasedContributionWeight;
    float3 contribution;
    uint initSeed;
    uint pathFlags;
    RestirPtReconnection rc;
};

RestirPtStoredReservoir RestirPtLoadStoredReservoir(uint bufferIndex, uint2 pixel)
{
    RWByteAddressBuffer reservoirs = ResourceDescriptorHeap[bufferIndex];
    const uint address = (pixel.y * resolution.x + pixel.x) * RESTIR_PT_RESERVOIR_BYTES;
    const uint4 word0 = reservoirs.Load4(address);
    const uint4 word1 = reservoirs.Load4(address + 16u);
    const uint4 word2 = reservoirs.Load4(address + 32u);
    const uint4 word3 = reservoirs.Load4(address + 48u);
    RestirPtStoredReservoir stored;
    stored.unbiasedContributionWeight = asfloat(word0.x);
    stored.contribution = asfloat(word0.yzw);
    stored.initSeed = word1.x;
    stored.pathFlags = word1.z;
    stored.rc = RestirPtNoReconnection();
    stored.rc.index = (word1.z >> RESTIR_PT_PATH_RC_INDEX_SHIFT) & 15u;
    stored.rc.instance = word1.w;
    stored.rc.primitive = word2.x;
    stored.rc.barycentrics = float2(asfloat(word1.y), asfloat(word2.y));
    stored.rc.wi = RestirPtOctDecode(RestirPtUnpackUnorm2x16(word2.z));
    stored.rc.radiance = float3(asfloat(word2.w), asfloat(word3.x), asfloat(word3.y));
    stored.rc.jacobian = asfloat(word3.z);
    stored.rc.lightPdf = asfloat(word3.w);
    stored.rc.lightType = (word1.z >> RESTIR_PT_PATH_LIGHT_TYPE_SHIFT) & 3u;
    stored.rc.vertexLobe = (word1.z >> RESTIR_PT_PATH_VERTEX_LOBE_SHIFT) & 3u;
    return stored;
}

// Writes a reservoir from its fields (the stored layout of RestirPtStoreReservoir, specification section 7.1) into the
// reservoir set at bufferIndex.
void RestirPtStoreReservoirFieldsTo(uint bufferIndex, uint2 pixel, float unbiasedContributionWeight, float3 contribution,
    uint initSeed, uint pathFlags, RestirPtReconnection rc)
{
    RWByteAddressBuffer reservoirs = ResourceDescriptorHeap[bufferIndex];
    const uint address = (pixel.y * resolution.x + pixel.x) * RESTIR_PT_RESERVOIR_BYTES;
    reservoirs.Store4(address, uint4(asuint(unbiasedContributionWeight), asuint(contribution)));
    reservoirs.Store4(address + 16u, uint4(initSeed, asuint(rc.barycentrics.x), pathFlags, rc.instance));
    reservoirs.Store4(address + 32u, uint4(rc.primitive, asuint(rc.barycentrics.y), RestirPtEncodeDirection(rc.wi),
        asuint(rc.radiance.r)));
    reservoirs.Store4(address + 48u, uint4(asuint(rc.radiance.g), asuint(rc.radiance.b), asuint(rc.jacobian),
        asuint(rc.lightPdf)));
}

void RestirPtStoreReservoirFields(uint2 pixel, float unbiasedContributionWeight, float3 contribution, uint initSeed,
    uint pathFlags, RestirPtReconnection rc)
{
    RestirPtStoreReservoirFieldsTo(restirPtReservoirIndex, pixel, unbiasedContributionWeight, contribution, initSeed,
        pathFlags, rc);
}

uint RestirPtFlagsWithConfidence(uint pathFlags, uint confidence)
{
    return (pathFlags & ~(255u << 8u)) | (min(confidence, 255u) << 8u);
}

// The outcome of temporal resampling for one pixel (sample 0): the selected sample's destination F, its W and its stored
// fields, and diagnostics.
struct RestirPtTemporalResult
{
    float3 contribution;
    float unbiasedContributionWeight;
    uint initSeed;
    uint pathFlags;
    RestirPtReconnection rc;
    bool temporalSelected;
    // M5.5 diagnostics (debug view 2): the canonical sample's MIS weight m_c and the temporal confidence M_t'.
    float canonicalMis;
    float temporalConfidence;
    // Diagnostics: the lookup stage (RestirPtTemporalNeighbor), whether the forward and reverse shifts succeeded.
    uint stage;
    bool forwardValid;
    bool reverseValid;
    // Test A (temporal identity): the neighbour is this pixel, so a static scene's forward shift must reproduce the
    // stored F with J = 1 (within the M5.1 tolerance); whether it did.
    bool identityCandidate;
    bool identity;
    // M5.4 C5: the resampling was deferred to the replay dispatch (nothing else in the result is meaningful).
    bool deferred;
    // The forward shift's reason (RESTIR_PT_SHIFT_REASON_*) and the temporal sample's kind (0 replay-only, 1 light
    // endpoint, 2 environment endpoint, 3 hit reconnection).
    uint forwardReason;
    uint temporalKind;
    // M5.2 C4 round trip (diagnostics variant only): the forward-shifted temporal sample shifted back into the temporal
    // domain. 0 not run, 1 back shift invalid, 2 F mismatch, 3 J_forward J_back != 1, 4 consistent.
    uint roundTrip;
};

// Temporal GRIS over {canonical c, temporal t} with Talbot (balance) MIS and the confidence cap [P 3; O; spec section 6.1].
//   Canonical Y_c: the current initial sample (F_c, W_c, M_c = 1, its seed, path flags and reconnection data, as the
//   reservoir stores them).
//   Temporal X_t: the previous reservoir at the reprojected pixel (F_t, W_t, M_t' = min(Cap, M_t)).
//   Shifts: T(X_t) into this pixel (F', J_tc), and Y_c into the previous pixel's primary (F'', J_ct), each with the
//   current scene.
//   m_c = M_c p_c(Y_c) / (M_c p_c(Y_c) + M_t' p_t(T^-1 Y_c) J_ct)
//   m_t = M_t' p_t(X_t) / (M_c p_c(T X_t) J_tc + M_t' p_t(X_t))
//   w_c = m_c p_c(Y_c) W_c,  w_t = m_t p_c(T X_t) W_t J_tc;  Y ~ w;  W = (w_c + w_t) / p_c(Y);  M = M_c + M_t'.
// p = luminance(F), F the PSS integrand of its own domain (D1).
// M5.4 C5: whether a shift of a sample with these stored flags traces a random replay (RestirPtShiftSample's replayEnd > 1:
// a reconnection at k >= 3, or a replay-only path of length >= 3; replay-only NEE fails at once).
bool RestirPtShiftNeedsReplay(uint pathFlags)
{
    const uint pathLength = pathFlags & 15u;
    const uint rcIndex = (pathFlags >> RESTIR_PT_PATH_RC_INDEX_SHIFT) & 15u;
    const bool nee = (pathFlags & RESTIR_PT_PATH_NEE) != 0u;

    if(pathLength < 2u || (rcIndex == 0u && nee))
    {
        return false;
    }

    return (rcIndex == 0u ? pathLength - 1u : rcIndex - 1u) > 1u;
}

RestirPtTemporalResult RestirPtTemporalResample(uint2 pixel, PathTraceRasterFirstVertex firstVertex, uint seed,
    float3 canonicalContribution, float canonicalWeight, uint canonicalFlags, RestirPtReconnection canonicalRc,
    bool deferReplay)
{
    RestirPtTemporalResult result = (RestirPtTemporalResult)0;
    result.contribution = canonicalContribution;
    result.unbiasedContributionWeight = canonicalWeight;
    result.initSeed = seed;
    result.pathFlags = RestirPtFlagsWithConfidence(canonicalFlags, 1u);
    result.rc = canonicalRc;

    const RestirPtTemporalNeighbor neighbor = RestirPtFindTemporalNeighbor(pixel, firstVertex, seed);
    result.stage = neighbor.stage;

    if(neighbor.stage < 4u)
    {
        return result;
    }

    const RestirPtStoredReservoir temporal = RestirPtLoadStoredReservoir(restirPtPreviousReservoirIndex,
        uint2(neighbor.pixel));
    float temporalConfidence = float(min((temporal.pathFlags >> 8u) & 255u, restirPtTemporalConfidenceCap));

    // M5.4 C7 (biased, off by default): the duplication map's reduced cap at the temporal neighbour [P 9].
    if((restirPtFlags & RESTIR_PT_FLAG_DUPLICATION_APPLY) != 0u && restirPtPreviousDuplicationIndex != 0xFFFFFFFFu)
    {
        RWByteAddressBuffer duplication = ResourceDescriptorHeap[restirPtPreviousDuplicationIndex];
        const float duplicated = float(duplication.Load(4u * (uint(neighbor.pixel.y) * resolution.x +
            uint(neighbor.pixel.x)))) / 288.0f;
        const float cap = lerp(float(restirPtTemporalConfidenceCap), RESTIR_PT_DUPLICATION_MIN_CAP,
            pow(saturate(duplicated), RESTIR_PT_DUPLICATION_ALPHA));
        temporalConfidence = min(float((temporal.pathFlags >> 8u) & 255u), cap);
    }

    if(!(temporalConfidence > 0.0f))
    {
        return result;
    }

    const float canonicalTarget = PathTraceLuminance(canonicalContribution);
    const float canonicalConfidence = 1.0f;

    const bool temporalHasSample = (temporal.pathFlags & 15u) != 0u && temporal.unbiasedContributionWeight > 0.0f &&
        PathTraceLuminance(temporal.contribution) > 0.0f;
    PathTraceRasterFirstVertex previousVertex = firstVertex;
    previousVertex.state = 1;
    previousVertex.position = neighbor.previous.position;
    previousVertex.surface = neighbor.previous.surface;

    // M5.4 C5: with replay compaction, a pixel whose forward or reverse shift traces a replay leaves this dispatch; the
    // replay dispatch recomputes it from the same inputs (exact).
    if(deferReplay && ((temporalHasSample && RestirPtShiftNeedsReplay(temporal.pathFlags)) ||
        (canonicalTarget > 0.0f && RestirPtShiftNeedsReplay(canonicalFlags))))
    {
        result.deferred = true;
        return result;
    }

    // The shifts, as one non-unrolled loop so the (large) shift is inlined once (M5.2 C4: two inlined copies grew the
    // kernel from 183 to 484 KB of DXIL and slowed initial sampling with temporal reuse off):
    //   0 forward: the temporal sample into this pixel;
    //   1 reverse: the canonical sample into the previous pixel's primary (the current scene);
    //   2 round trip (diagnostics variant only): the forward result back into the temporal domain, which must reproduce
    //     F_t with J_back = 1 / J_forward (bijectivity of the shift).
    RestirPtShifted forward = (RestirPtShifted)0;
    RestirPtShifted reverse = (RestirPtShifted)0;
#if defined(RESTIR_PT_DIAGNOSTICS)
    const uint shiftCount = (restirPtFlags & RESTIR_PT_FLAG_TEMPORAL_ROUND_TRIP) != 0u ? 3u : 2u;
#else
    const uint shiftCount = 2u;
#endif

    [loop]
    for(uint shiftIndex = 0u; shiftIndex < shiftCount; ++shiftIndex)
    {
        const bool toPrevious = shiftIndex != 0u;
        const bool run = shiftIndex == 0u ? temporalHasSample : shiftIndex == 1u ? canonicalTarget > 0.0f : forward.valid;

        if(!run)
        {
            continue;
        }

        PathTraceRasterFirstVertex destination = firstVertex;
        RestirPtReconnection source = temporal.rc;

        if(toPrevious)
        {
            destination = previousVertex;
        }

        if(shiftIndex == 1u)
        {
            // The canonical sample as the reservoir stores it (M5.3 close-out, with the escape-direction correction): its
            // rcWi decoded from oct16, the only quantized reconnection field, as every other shift reads a sample. The
            // reverse shift and capture then evaluate the same path.
            source = canonicalRc;
            source.wi = RestirPtStoredDirection(canonicalRc.wi);
        }
        else if(shiftIndex == 2u)
        {
            source = forward.rc;
        }

        const float footprintThreshold = toPrevious ? RestirPtFootprintThreshold(restirPtPreviousCameraPosition,
            destination) : RestirPtFootprintThreshold(cameraPosition, destination);
        // M5.5 Stage 1: a shift into the temporal domain is evaluated in that domain's scene.
        g_restirPtHistoryScene = toPrevious && (restirPtFlags & RESTIR_PT_FLAG_HISTORY_SCENE) != 0u;
        const RestirPtShifted shifted = RestirPtShiftSample(destination, shiftIndex == 1u ? seed : temporal.initSeed,
            shiftIndex == 1u ? canonicalFlags : temporal.pathFlags, source, footprintThreshold);
        g_restirPtHistoryScene = false;

        if(shiftIndex == 0u)
        {
            forward = shifted;
        }
        else if(shiftIndex == 1u)
        {
            reverse = shifted;
        }
#if defined(RESTIR_PT_DIAGNOSTICS)
        else
        {
            const float storedTarget = PathTraceLuminance(temporal.contribution);
            result.roundTrip = !shifted.valid ? 1u : RestirPtRelativeError(PathTraceLuminance(shifted.contribution),
                storedTarget) > 1.0e-3f ? 2u : abs(forward.jacobian * shifted.jacobian - 1.0f) > 1.0e-3f ? 3u : 4u;
        }
#endif
    }

    result.forwardValid = forward.valid;
    result.reverseValid = reverse.valid;
    result.identityCandidate = temporalHasSample && all(uint2(neighbor.pixel) == pixel);
    result.forwardReason = forward.reason;
    {
        const uint temporalLength = temporal.pathFlags & 15u;
        result.temporalKind = temporal.rc.index == 0u ? 0u : (temporal.rc.index == temporalLength ?
            ((temporal.pathFlags & RESTIR_PT_PATH_NEE) != 0u ? 1u : 2u) : 3u);
    }

    if(result.identityCandidate && forward.valid)
    {
        const float storedTarget = PathTraceLuminance(temporal.contribution);
        result.identity = RestirPtRelativeError(PathTraceLuminance(forward.contribution), storedTarget) <=
            RESTIR_PT_SHIFT_TOLERANCE && abs(forward.jacobian - 1.0f) <= RESTIR_PT_SHIFT_TOLERANCE;
    }

    // Canonical MIS weight and resampling weight.
    const float reverseTerm = reverse.valid ? temporalConfidence * PathTraceLuminance(reverse.contribution) *
        reverse.jacobian : 0.0f;
    const float canonicalTerm = canonicalConfidence * canonicalTarget;
    const float canonicalMis = canonicalTerm > 0.0f ? canonicalTerm / (canonicalTerm + reverseTerm) : 0.0f;
    const float canonicalResampling = canonicalMis * canonicalTarget * canonicalWeight;
    result.canonicalMis = canonicalMis;
    result.temporalConfidence = temporalConfidence;

    // Temporal MIS weight and resampling weight.
    float temporalResampling = 0.0f;
    const float shiftedTarget = forward.valid ? PathTraceLuminance(forward.contribution) : 0.0f;

    if(forward.valid && shiftedTarget > 0.0f)
    {
        const float temporalTerm = temporalConfidence * PathTraceLuminance(temporal.contribution);
        const float crossTerm = canonicalConfidence * shiftedTarget * forward.jacobian;
        const float temporalMis = temporalTerm / (crossTerm + temporalTerm);
        temporalResampling = temporalMis * shiftedTarget * temporal.unbiasedContributionWeight * forward.jacobian;
    }

    const float weightSum = canonicalResampling + temporalResampling;
    const uint confidence = uint(round(canonicalConfidence + temporalConfidence));

    if(!(weightSum > 0.0f) || !isfinite(weightSum))
    {
        // Neither candidate contributes: an empty reservoir that keeps its confidence.
        result.contribution = float3(0.0f, 0.0f, 0.0f);
        result.unbiasedContributionWeight = 0.0f;
        result.pathFlags = RestirPtFlagsWithConfidence(result.pathFlags, confidence);
        return result;
    }

    const bool selectTemporal = RestirPtRandom(seed, 0u, RESTIR_PT_DIMENSION_TEMPORAL + 2u) * weightSum <
        temporalResampling;

    if(selectTemporal)
    {
        result.temporalSelected = true;
        result.contribution = forward.contribution;
        result.unbiasedContributionWeight = weightSum / shiftedTarget;
        result.initSeed = temporal.initSeed;
        result.pathFlags = RestirPtFlagsWithConfidence(temporal.pathFlags, confidence);
        result.rc = forward.rc;

        // B1a/B1b: the kept sample ages by one frame (stagnancy or firefly mode only; default reservoirs unchanged).
        if((restirPtFlags & (RESTIR_PT_FLAG_STAGNANCY | RESTIR_PT_FLAG_FIREFLY)) != 0u)
        {
            result.pathFlags = RestirPtFlagsAged(result.pathFlags);
        }
    }
    else
    {
        result.unbiasedContributionWeight = weightSum / canonicalTarget;
        result.pathFlags = RestirPtFlagsWithConfidence(result.pathFlags, confidence);
    }

    return result;
}

// What one self-shift found: the T-2.1 outcome and the round-3 diagnostics (test only).
struct RestirPtShiftResult
{
    // 0 not shifted (replay-only or no reconnection), 1 shifted, 2 the replay or the reconnection failed.
    uint status;
    uint shiftCase;
    uint reason;
    // |F'/F - 1| (luminance) and |J - 1|.
    float error;
    float jacobianError;
    float targetLuminance;
    float shiftedLuminance;
    float jacobian;
    float cachedJacobian;

    // Hit reconnections (hitGeometry: x_k was rebuilt): the geometry, the sampled direction omega against the
    // vertex-to-vertex direction omega', and the densities along each.
    bool hitGeometry;
    float distance;
    float vertexCosine;
    float sampledCosine;
    float shiftCosine;
    float directionAngle;
    float offsetLength;
    float sampledDensity;
    float shiftDensity;
    float sampledVertexDensity;
    float vertexDensity;
    float previousAlpha;
    float vertexAlpha;
    uint previousLobe;
    // The stored lambda_k, and the lobe a re-selection from the seed would pick with x_k seen along omega' (diagnostic).
    uint vertexLobe;
    uint sampledVertexLobe;
    float specularProbability;
    float sampledSpecularProbability;
    bool normalFlip;
    bool previousDensityLost;
    // A visibility failure whose blocker is another triangle of x_k's own mesh (x_k's own triangle is excluded, P2), and
    // the blocker's distance as a fraction of the reconnection's; an edge tie (RESTIR_PT_COUNTER_EDGE_TIE).
    bool visibilitySelfHit;
    bool edgeTie;
    float blockerFraction;
    // Dump detail (round 4): capture's two divisor factors (luminance; unquantized runs), F and F' per channel,
    // rcRadiance, and x_(k-1)'s and x_k's factors along omega'.
    float captureArrive;
    float captureLocal;
    float3 storedRgb;
    float3 shiftedRgb;
    float3 radianceRgb;
    float3 previousWeightRgb;
    float3 localRgb;
    // Stored-word runs: the quantized barycentrics change x_k's material (roughness or base colour by more than 1%).
    bool materialChanged;
    // P1: the stored lambda_k differs from the in-register one (must never happen).
    bool storedLobeMismatch;
    // T-2.1b: the stored octahedral rcWi, decoded and re-encoded, differs from the stored bits.
    bool octReencodeMismatch;
    // T-2.1b: the stored rcWi bits are not a fixed point of 8 decode/re-encode cycles.
    bool octCycleUnstable;
    // Round 5 attribution (hit reconnections): capture's and the shift's omega' differ in any bit; the two
    // cos theta_(k-1) values (capture, shift).
    bool directionBitsDiffer;
    float captureCosine;
    float shiftCosineBits;
    // P4: the distant light regenerated (0 none, 1 sun, 2 environment), its angle to the in-register direction, and
    // whether its density differs from the stored lightPdf (or the regeneration failed).
    uint distantLight;
    float distantAngle;
    bool distantPdfMismatch;
    // P3 validation at a different receiver: tested; the light's type; direction and relative radiance errors against the
    // independent restatement; the shift's visibility and the reference (0 occluded, 1 visible, 2 ambiguous).
    bool lightCrossTested;
    uint lightCrossType;
    float lightCrossDirectionError;
    float lightCrossRadianceError;
    bool lightCrossVisible;
    uint lightCrossReference;
    // P2 validation: the cross pair was tested; the verdicts of P2, the offset-endpoint alternative and the plain 0.999 d
    // rule; the reference (0 occluded by another object, 1 visible, 2 ambiguous: another surface within the contact band
    // of x_k, 3 occluded by another triangle of x_k's own mesh).
    bool crossTested;
    bool crossVisible;
    bool crossOffsetEndpointVisible;
    bool crossPreviousRuleVisible;
    uint crossReference;
    // Counterfactual errors (|F'/F - 1|, |J - 1|), evaluated once the reconnection is visible: x_(k-1)'s density and
    // weight along omega; x_k seen along omega; both.
    bool counterfactuals;
    float2 sampledPrevious;
    float2 sampledVertex;
    float2 sampledBoth;

    // The quantization study: the case, the stored rcWi's angular error against the in-register direction, and the vertex
    // that evaluates toward rcWi (its GGX alpha, whether its sampled lobe is the cosine lobe, its |cos| toward rcWi).
    uint quantizationCase;
    float directionError;
    float evaluationAlpha;
    bool evaluationCosineLobe;
    float evaluationCosine;

    // P3, table-light endpoints rebuilt from their index: the light's type (0 point, 1 spot,
    // 2 directional; 3 not run), the result (0 identity, 1 F mismatch, 2 not visible, 3 no incident radiance) and the
    // rebuilt direction's angle to the in-register direction.
    uint lightKind;
    uint lightResult;
    float lightDirectionError;
};

// Shifts the pixel's stored sample into its own pixel through its STORED reconnection data (as M5.2 will read it): replays
// the prefix x1 .. x_(k-1) from the base path's seed (same lobes, no roulette), rebuilds x_k from its instance, primitive
// and barycentrics (per-instance transforms, no trace), traces the reconnection's visibility, evaluates the BSDFs at
// y_(k-1) and x_k toward the stored directions, recomputes the NEE MIS weight from lightPdf, and the Jacobian against the
// cached terms. Identity: F' = F and J = 1 (within quantization: 16-bit barycentrics and octahedral rcWi).
// crossVertex (test only, P2 validation): the previous sample's hit reconnection vertex in this pixel (index 0: none);
// crossLightIndex (test only, P3 validation): the previous sample's table-light endpoint (0xFFFFFFFF: none).
RestirPtShiftResult RestirPtSelfShift(uint2 pixel, PathTraceRasterFirstVertex firstVertex, uint seed,
    RestirPtCandidate selected, RestirPtReconnection crossVertex, uint crossLightIndex)
{
    RestirPtShiftResult result = (RestirPtShiftResult)0;
    result.reason = RESTIR_PT_SHIFT_REASON_SKIPPED;
    result.lightKind = 3u;
    uint flags;
    float3 storedContribution;
    uint storedSeed;
    RestirPtReconnection rc = RestirPtLoadReconnection(pixel, flags, storedContribution, storedSeed);

    if((restirPtFlags & RESTIR_PT_FLAG_SELF_SHIFT_UNQUANTIZED) != 0u)
    {
        rc = selected.rc;
        flags = RestirPtPathFlags(selected);
        storedContribution = selected.contribution;
        storedSeed = seed;
    }

    // T-2.1b: under reuse a reconnection shift copies x_k's stored data, so re-storing a shifted sample quantizes decoded
    // values again; errors stay bounded when quantization is idempotent (fp32 fields trivially; checked here for rcWi).
    if((restirPtFlags & RESTIR_PT_FLAG_SELF_SHIFT_UNQUANTIZED) == 0u)
    {
        // The stored bits are RestirPtEncodeDirection(selected.rc.wi); a plain re-encode of the decoded value must give
        // them back.
        result.octReencodeMismatch = RestirPtPackUnorm2x16(RestirPtOctEncode(rc.wi)) !=
            RestirPtEncodeDirection(selected.rc.wi);
        const uint storedBits = RestirPtEncodeDirection(selected.rc.wi);
        uint cycled = storedBits;

        for(uint cycle = 0u; cycle < 8u; ++cycle)
        {
            cycled = RestirPtPackUnorm2x16(RestirPtOctEncode(RestirPtOctDecode(RestirPtUnpackUnorm2x16(cycled))));
        }

        result.octCycleUnstable = cycled != storedBits;
    }

    if((restirPtFlags & RESTIR_PT_FLAG_SHIFT_EXACT_DIRECTION) != 0u)
    {
        rc.wi = selected.rc.wi;
    }

    if((restirPtFlags & RESTIR_PT_FLAG_SHIFT_EXACT_BARYCENTRICS) != 0u)
    {
        rc.barycentrics = selected.rc.barycentrics;
    }

    const uint pathLength = flags & 15u;
    const bool nee = (flags & RESTIR_PT_PATH_NEE) != 0u;

    if(pathLength == 0u || rc.index == 0u || firstVertex.state <= 0)
    {
        return result;
    }

    const bool endpoint = rc.index == pathLength;
    const bool neeAtVertex = nee && rc.index == pathLength - 1u;
    result.status = 2u;
    result.cachedJacobian = rc.jacobian;
    result.targetLuminance = PathTraceLuminance(storedContribution);
    result.directionError = RestirPtAngle(rc.wi, selected.rc.wi);
    result.storedLobeMismatch = !endpoint && !neeAtVertex && rc.vertexLobe != selected.rc.vertexLobe;
    result.captureArrive = selected.rc.captureArrive;
    result.captureLocal = selected.rc.captureLocal;

    if(endpoint)
    {
        result.shiftCase = nee ? RESTIR_PT_SHIFT_CASE_LIGHT : RESTIR_PT_SHIFT_CASE_ENVIRONMENT;
        result.quantizationCase = !nee ? RESTIR_PT_QUANTIZATION_ESCAPE : rc.lightType == RESTIR_PT_LIGHT_ANALYTIC ?
            RESTIR_PT_QUANTIZATION_TABLE_LIGHT : RESTIR_PT_QUANTIZATION_DISTANT_LIGHT;
    }
    else
    {
        result.shiftCase = neeAtVertex ? RESTIR_PT_SHIFT_CASE_HIT_NEE : RESTIR_PT_SHIFT_CASE_HIT_CONTINUATION;
        result.quantizationCase = neeAtVertex ? RESTIR_PT_QUANTIZATION_HIT_NEE : RESTIR_PT_QUANTIZATION_HIT_CONTINUATION;
    }

    // P4: a distant-light NEE end (sun, or environment NEE) regenerates its direction from the stored initSeed at the NEE
    // vertex (pathLength - 1), in place of the octahedral rcWi, where rcWi is that light direction: a light endpoint, or
    // NEE leaving x_k. (When x_k continues, rcWi is x_k's continuation direction and the light lies beyond the suffix.)
    const bool lightDirectionStored = endpoint || neeAtVertex;

    if(nee && lightDirectionStored &&
        (rc.lightType == RESTIR_PT_LIGHT_SUN || rc.lightType == RESTIR_PT_LIGHT_ENVIRONMENT))
    {
        float3 regenerated;
        float regeneratedPdf;
        result.distantLight = rc.lightType == RESTIR_PT_LIGHT_SUN ? 1u : 2u;

        if(RestirPtDistantLightDirection(rc.lightType, storedSeed, pathLength - 1u, regenerated, regeneratedPdf))
        {
            result.distantAngle = RestirPtAngle(regenerated, selected.rc.wi);
            result.distantPdfMismatch = asuint(regeneratedPdf) != asuint(rc.lightPdf);
            rc.wi = regenerated;
        }
        else
        {
            result.distantPdfMismatch = true;
        }
    }

    if(!(rc.jacobian > 0.0f))
    {
        // Capture found x_k's terms not reconstructible (x_k's replayed lobe has no density or BSDF value toward rcWi, its
        // NEE factor is 0, or x_k could not be seen along omega'): the sample cannot shift.
        result.reason = RESTIR_PT_SHIFT_REASON_VERTEX_DENSITY;
        return result;
    }

    // Replay x1 .. x_(k-1).
    const uint previousVertex = rc.index - 1u;
    PathTraceSurface surface = firstVertex.surface;
    float3 position = firstVertex.position;
    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    result.reason = RESTIR_PT_SHIFT_REASON_REPLAY;

    for(uint vertex = 1u; vertex < previousVertex; ++vertex)
    {
        const RestirPtLobeSample lobeSample = RestirPtSampleLobe(surface, float3(RestirPtRandom(seed, vertex,
            RESTIR_PT_DIMENSION_LOBE), RestirPtRandom(seed, vertex, RESTIR_PT_DIMENSION_DIRECTION),
            RestirPtRandom(seed, vertex, RESTIR_PT_DIMENSION_DIRECTION + 1u)));

        if(!lobeSample.valid)
        {
            return result;
        }

        throughput *= lobeSample.weight;
        RayDesc ray;
        ray.Origin = PathTraceOffsetRayOrigin(position, surface.geometricNormal);
        ray.Direction = lobeSample.direction;
        ray.TMin = 0.0f;
        ray.TMax = PATH_TRACE_DISTANT_LIGHT_DISTANCE;
        PathTracePayload payload = (PathTracePayload)0;
        payload.hitDistance = -1.0f;
        RestirPtHitIdentity identity;
        RestirPtTraceInline(ray, payload, identity);

        if(payload.hitDistance < 0.0f)
        {
            return result;
        }

        position = ray.Origin + ray.Direction * payload.hitDistance;

        if(!PathTraceBuildSurface(payload, ray.Direction, surface))
        {
            return result;
        }
    }

    // P2 validation (test only): visibility from this path's y_(k-1) to the previous sample's x_k, a different vertex pair
    // with real occluders, by the P2 test, by the earlier 0.999 d rule and by the closest-hit reference.
    if(crossVertex.index != 0u)
    {
        float3 crossPosition;
        PathTracePayload crossPayload;

        if(RestirPtRebuildVertex(crossVertex.instance, crossVertex.primitive, crossVertex.barycentrics, position,
            crossPosition, crossPayload) && length(crossPosition - position) > 0.0f)
        {
            float3 end;
            uint blockerInstance;
            uint blockerPrimitive;
            const float3 origin = PathTraceOffsetRayOrigin(position, surface.geometricNormal);
            const float3 toVertex = crossPosition - origin;
            const float vertexDistance = length(toVertex);
            result.crossTested = true;
            float blockerFraction;
            float blockerEdge;
            result.crossVisible = RestirPtReconnectionVisible(position, surface.geometricNormal, crossPosition,
                crossVertex.instance, crossVertex.primitive, blockerInstance, blockerPrimitive, blockerFraction,
                blockerEdge);
            result.crossOffsetEndpointVisible = RestirPtOffsetEndpointVisible(position, surface.geometricNormal,
                crossPosition, crossPayload.geometricNormal, end);
            result.crossPreviousRuleVisible = RestirPtSegmentVisible(origin, toVertex / vertexDistance,
                vertexDistance * 0.999f, blockerInstance, blockerPrimitive);
            // The reference's contact band: the last 0.1% of the distance (the margin [O] leaves untested), or twice the
            // endpoint offset, whichever is larger.
            result.crossReference = RestirPtReferenceVisibility(origin, crossPosition, crossVertex.instance,
                crossVertex.primitive, max(0.001f * vertexDistance, 2.0f * length(end - crossPosition)));
        }
    }

    // P3 validation (test only): the previous sample's table-light endpoint evaluated at this path's y_(k-1), a different
    // receiver: the shift's rebuild from the light index against the independent restatement and the reference visibility.
    if(crossLightIndex < sceneLightCount)
    {
        const GPULight light = SceneLightLoad(sceneLightBufferIndex, crossLightIndex);
        const PathTraceLightSample lightSample = PathTraceSampleTableLight(light, position);
        float3 referenceDirection;
        float referenceDistance;
        float3 referenceScale;
        const float3 referenceIncident = RestirPtReferenceIncident(light, position, referenceDirection, referenceDistance,
            referenceScale);
        const float referenceLuminance = PathTraceLuminance(referenceIncident);
        const float scaleLuminance = PathTraceLuminance(referenceScale);
        uint rays = 0u;
        result.lightCrossTested = true;
        result.lightCrossType = min(light.type, 2u);
        result.lightCrossDirectionError = RestirPtAngle(lightSample.direction, referenceDirection);
        // The radiance error relative to the light's cone- and window-free radiance at this receiver (see
        // RestirPtReferenceIncident's scale).
        result.lightCrossRadianceError = scaleLuminance > 0.0f ?
            abs(PathTraceLuminance(lightSample.radiance) - referenceLuminance) / scaleLuminance : 0.0f;
        result.lightCrossVisible = lightSample.valid && PathTraceVisible(position, surface.geometricNormal,
            lightSample.direction, lightSample.distance, rays);
        result.lightCrossReference = referenceLuminance > 0.0f ? RestirPtReferenceLightVisibility(position,
            surface.geometricNormal, referenceDirection, referenceDistance) : 0u;
    }

    // y_(k-1) = x_(k-1): the lobe it sampled (the base path's lambda_(k-1)) and that lobe's direction omega.
    const RestirPtLobeSample previousLobe = RestirPtSampleLobe(surface, float3(RestirPtRandom(seed, previousVertex,
        RESTIR_PT_DIMENSION_LOBE), RestirPtRandom(seed, previousVertex, RESTIR_PT_DIMENSION_DIRECTION),
        RestirPtRandom(seed, previousVertex, RESTIR_PT_DIMENSION_DIRECTION + 1u)));
    result.previousLobe = previousLobe.lobe;
    result.previousAlpha = surface.material.roughness * surface.material.roughness;
    float3 shifted = float3(0.0f, 0.0f, 0.0f);
    float jacobian = 0.0f;

    if(endpoint && nee)
    {
        // A light endpoint (forced NEE): NEE from y_(k-1) toward the light. Table lights (P3) are rebuilt from their index
        // at y_(k-1); the sun and environment (P4) use the regenerated direction (rc.wi above).
        float3 direction = rc.wi;
        float3 incident = float3(1.0f, 1.0f, 1.0f);

        if(rc.lightType == RESTIR_PT_LIGHT_TABLE)
        {
            // P3: direction, attenuation (distance, range window, spot cone) and the shadow ray from the light itself at
            // y_(k-1) (a directional light: its fixed direction); rcRadiance holds the light-independent factor. At a
            // self-shift y_(k-1) = x_(k-1); the cross-receiver test checks a different receiver.
            result.reason = RESTIR_PT_SHIFT_REASON_REBUILD;

            if(rc.instance >= sceneLightCount)
            {
                return result;
            }

            const GPULight light = SceneLightLoad(sceneLightBufferIndex, rc.instance);
            const PathTraceLightSample lightSample = PathTraceSampleTableLight(light, position);
            uint rays = 0u;
            result.lightKind = min(light.type, 2u);
            result.lightDirectionError = RestirPtAngle(lightSample.direction, selected.rc.wi);

            if(!lightSample.valid)
            {
                result.lightResult = 3u;
                return result;
            }

            if(!PathTraceVisible(position, surface.geometricNormal, lightSample.direction, lightSample.distance, rays))
            {
                result.lightResult = 2u;
                result.reason = RESTIR_PT_SHIFT_REASON_VISIBILITY;
                return result;
            }

            direction = lightSample.direction;
            incident = lightSample.radiance;
        }

        float3 diffuse;
        const float mis = rc.lightPdf > 0.0f ? RestirPtBalanceHeuristic(rc.lightPdf, PathTracePdf(surface, direction)) :
            1.0f;
        shifted = throughput * RestirPtBsdfCos(surface, direction, diffuse) * incident * rc.radiance * mis;
        jacobian = 1.0f;
        result.evaluationAlpha = result.previousAlpha;
        result.evaluationCosine = abs(dot(surface.normal, direction));
    }
    else if(endpoint)
    {
        // Environment endpoint (BSDF escape): the stored lobe's weight toward the stored direction.
        result.evaluationAlpha = result.previousAlpha;
        result.evaluationCosineLobe = previousLobe.lobe == RESTIR_PT_LOBE_COSINE;
        result.evaluationCosine = abs(dot(surface.normal, rc.wi));
        result.reason = RESTIR_PT_SHIFT_REASON_PREVIOUS_LOBE;

        if(!previousLobe.valid)
        {
            return result;
        }

        float3 diffuseValue;
        const float density = RestirPtLobeDensity(surface, previousLobe.lobe, rc.wi);

        if(!(density > 0.0f))
        {
            return result;
        }

        shifted = throughput * RestirPtEvaluateLobe(surface, previousLobe.lobe, rc.wi, diffuseValue) *
            dot(surface.normal, rc.wi) / density * rc.radiance;
        jacobian = density;
    }
    else
    {
        // A hit vertex: reconnect y_(k-1) -> x_k.
        result.reason = RESTIR_PT_SHIFT_REASON_PREVIOUS_LOBE;

        if(!previousLobe.valid || previousLobe.lobe == RESTIR_PT_LOBE_DELTA)
        {
            return result;
        }

        float3 rebuiltPosition;
        PathTracePayload rebuilt;
        result.reason = RESTIR_PT_SHIFT_REASON_REBUILD;

        if(!RestirPtRebuildVertex(rc.instance, rc.primitive, rc.barycentrics, position, rebuiltPosition, rebuilt))
        {
            return result;
        }

        float distance;
        const float3 direction = RestirPtReconnectionDirection(position, rebuiltPosition, distance);
        const float3 sampledDirection = previousLobe.direction;
        const float geometry = abs(dot(rebuilt.geometricNormal, direction)) / (distance * distance);
        const float3 visibilityOrigin = PathTraceOffsetRayOrigin(position, surface.geometricNormal);
        result.hitGeometry = true;
        result.directionBitsDiffer = any(asuint(direction) != asuint(selected.rc.captureDirection));
        result.captureCosine = selected.rc.captureCosine;
        result.shiftCosineBits = dot(surface.normal, direction);
        result.distance = distance;
        result.vertexCosine = abs(dot(rebuilt.geometricNormal, direction));
        result.sampledCosine = dot(surface.normal, sampledDirection);
        result.shiftCosine = dot(surface.normal, direction);
        result.directionAngle = RestirPtAngle(sampledDirection, direction);
        result.offsetLength = length(visibilityOrigin - position);
        uint blockerInstance;
        uint blockerPrimitive;

        float blockerFraction;
        float blockerEdge;

        if(!RestirPtReconnectionVisible(position, surface.geometricNormal, rebuiltPosition, rc.instance, rc.primitive,
            blockerInstance, blockerPrimitive, blockerFraction, blockerEdge))
        {
            // x_k's own triangle is never a blocker now (P2). Record whether the blocker is x_k's own mesh and where,
            // its distance from its nearest edge, and whether the base path's own ray (offset origin along omega,
            // closest hit) reaches x_k's triangle: the dump's "local" holds (base ray reaches x_k, blocker edge, 0).
            RayDesc baseRay;
            baseRay.Origin = visibilityOrigin;
            baseRay.Direction = sampledDirection;
            baseRay.TMin = 0.0f;
            baseRay.TMax = PATH_TRACE_DISTANT_LIGHT_DISTANCE;
            PathTracePayload basePayload = (PathTracePayload)0;
            basePayload.hitDistance = -1.0f;
            RestirPtHitIdentity baseIdentity;
            RestirPtTraceInline(baseRay, basePayload, baseIdentity);
            const bool baseReachesVertex = basePayload.hitDistance >= 0.0f && baseIdentity.instance == rc.instance &&
                baseIdentity.primitive == rc.primitive;
            result.reason = RESTIR_PT_SHIFT_REASON_VISIBILITY;
            result.visibilitySelfHit = blockerInstance == rc.instance;
            result.blockerFraction = blockerFraction;
            // An edge tie: the base path's own ray reaches x_k, and the blocker hit lies within the two rays' separation of
            // its triangle's nearest edge, measured in the blocker's plane: rays a distance s apart (sin(angle) t, plus 8
            // ULPs of the coordinates' magnitude for the hit point's own rounding) meet that plane up to s / |cos alpha|
            // apart, alpha the angle between the ray and the blocker's normal. The dump's "local" holds (base ray
            // reaches x_k, edge distance in m, the in-plane separation in m).
            const float3 shiftDirection = normalize(rebuiltPosition - visibilityOrigin);
            const float blockerT = blockerFraction * length(rebuiltPosition - visibilityOrigin);
            const float3 blockerPoint = visibilityOrigin + shiftDirection * blockerT;
            float3 blockerNormal;
            const float edgeDistance = RestirPtTriangleEdgeDistance(blockerInstance, blockerPrimitive, blockerPoint,
                blockerNormal);
            const float3 magnitude = abs(blockerPoint);
            const float rounding = 8.0f * 1.1920929e-7f * max(max(magnitude.x, magnitude.y), max(magnitude.z, 1.0f));
            const float separation = (length(cross(sampledDirection, shiftDirection)) * blockerT + rounding) /
                max(abs(dot(shiftDirection, blockerNormal)), 1.0e-3f);
            result.localRgb = float3(baseReachesVertex ? 1.0f : 0.0f, edgeDistance, separation);
            result.edgeTie = baseReachesVertex && edgeDistance >= 0.0f && edgeDistance <= separation;
            return result;
        }

        // Quantization study: whether the stored (16-bit) barycentrics change x_k's material, through texture lookups at
        // the shifted UV (roughness, or base colour, by more than 1%).
        if(any(rc.barycentrics != selected.rc.barycentrics))
        {
            float3 exactPosition;
            PathTracePayload exactPayload;
            PathTraceSurface exactSurface;
            PathTraceSurface storedSurface;

            if(RestirPtRebuildVertex(rc.instance, rc.primitive, selected.rc.barycentrics, position, exactPosition,
                exactPayload) && PathTraceBuildSurface(exactPayload, direction, exactSurface) &&
                PathTraceBuildSurface(rebuilt, direction, storedSurface))
            {
                const float roughnessChange = abs(storedSurface.material.roughness - exactSurface.material.roughness);
                const float3 colorChange = abs(storedSurface.baseColor - exactSurface.baseColor);
                result.materialChanged = roughnessChange > 0.01f * max(exactSurface.material.roughness, 0.01f) ||
                    any(colorChange > 0.01f * max(exactSurface.baseColor, 0.01f));
            }
        }

        // The shift (omega' at both vertices) and the round-3 counterfactuals (omega at x_(k-1), at x_k, at both).
        const RestirPtHitEvaluation evaluation = RestirPtEvaluateHit(surface, previousLobe.lobe, direction, rebuilt,
            direction, geometry, rc, seed, neeAtVertex);
        const RestirPtHitEvaluation sampledPrevious = RestirPtEvaluateHit(surface, previousLobe.lobe, sampledDirection,
            rebuilt, direction, geometry, rc, seed, neeAtVertex);
        const RestirPtHitEvaluation sampledVertex = RestirPtEvaluateHit(surface, previousLobe.lobe, direction, rebuilt,
            sampledDirection, geometry, rc, seed, neeAtVertex);
        const RestirPtHitEvaluation sampledBoth = RestirPtEvaluateHit(surface, previousLobe.lobe, sampledDirection,
            rebuilt, sampledDirection, geometry, rc, seed, neeAtVertex);
        result.counterfactuals = true;
        result.sampledPrevious = RestirPtEvaluationErrors(sampledPrevious, throughput, result.targetLuminance, rc.jacobian);
        result.sampledVertex = RestirPtEvaluationErrors(sampledVertex, throughput, result.targetLuminance, rc.jacobian);
        result.sampledBoth = RestirPtEvaluationErrors(sampledBoth, throughput, result.targetLuminance, rc.jacobian);
        result.sampledDensity = sampledBoth.previousDensity;
        result.shiftDensity = evaluation.previousDensity;
        result.sampledVertexDensity = sampledBoth.vertexDensity;
        result.vertexDensity = evaluation.vertexDensity;
        result.vertexAlpha = evaluation.vertexAlpha;
        result.vertexLobe = neeAtVertex ? 0u : rc.vertexLobe;
        result.sampledVertexLobe = evaluation.vertexLobe;
        result.specularProbability = evaluation.specularProbability;
        result.sampledSpecularProbability = sampledBoth.specularProbability;
        result.normalFlip = dot(evaluation.vertexNormal, sampledBoth.vertexNormal) < 0.0f;
        result.previousDensityLost = !(evaluation.previousDensity > 0.0f) && sampledBoth.previousDensity > 0.0f;
        result.evaluationAlpha = evaluation.vertexAlpha;
        result.evaluationCosineLobe = !neeAtVertex && evaluation.vertexLobe == RESTIR_PT_LOBE_COSINE;
        result.evaluationCosine = abs(dot(evaluation.vertexNormal, rc.wi));

        if(evaluation.reason != RESTIR_PT_SHIFT_REASON_IDENTITY)
        {
            result.reason = evaluation.reason;
            return result;
        }

        shifted = throughput * evaluation.value;
        jacobian = evaluation.jacobian;
        result.previousWeightRgb = throughput * evaluation.previousWeight;
        result.localRgb = evaluation.local;
    }

    result.shiftedLuminance = PathTraceLuminance(shifted);
    result.jacobian = jacobian;
    result.storedRgb = storedContribution;
    result.shiftedRgb = shifted;
    result.radianceRgb = rc.radiance;
    result.error = RestirPtRelativeError(result.shiftedLuminance, result.targetLuminance);
    result.jacobianError = abs(jacobian / rc.jacobian - 1.0f);

    if(!all(isfinite(shifted)) || !isfinite(jacobian))
    {
        result.reason = RESTIR_PT_SHIFT_REASON_NON_FINITE;
        return result;
    }

    result.status = 1u;

    if(result.error <= RESTIR_PT_SHIFT_TOLERANCE && result.jacobianError <= RESTIR_PT_SHIFT_TOLERANCE)
    {
        result.reason = RESTIR_PT_SHIFT_REASON_IDENTITY;
    }
    else
    {
        result.reason = result.jacobianError <= RESTIR_PT_SHIFT_TOLERANCE ? RESTIR_PT_SHIFT_REASON_F_MISMATCH :
            RESTIR_PT_SHIFT_REASON_J_MISMATCH;
    }

    if(result.lightKind < 3u && result.lightResult == 0u)
    {
        result.lightResult = result.reason == RESTIR_PT_SHIFT_REASON_IDENTITY ? 0u : 1u;
    }

    return result;
}

// -----------------------------------------------------------------------------
// T-2.1 diagnostics recording (test only; self-shift runs)
// -----------------------------------------------------------------------------

// The bin of value among five ascending edges (six bins).
uint RestirPtBin(float value, float edge0, float edge1, float edge2, float edge3, float edge4)
{
    return value < edge0 ? 0u : value < edge1 ? 1u : value < edge2 ? 2u : value < edge3 ? 3u : value < edge4 ? 4u : 5u;
}

uint RestirPtCosineBin(float cosine)
{
    return RestirPtBin(abs(cosine), 0.01f, 0.03f, 0.1f, 0.3f, 0.6f);
}

uint RestirPtMagnitudeBin(float value)
{
    return RestirPtBin(value, 1.0e-6f, 1.0e-4f, 1.0e-2f, 1.0f, 100.0f);
}

uint RestirPtGeometryOutcome(uint reason)
{
    if(reason == RESTIR_PT_SHIFT_REASON_IDENTITY)
    {
        return RESTIR_PT_OUTCOME_IDENTITY;
    }

    if(reason == RESTIR_PT_SHIFT_REASON_F_MISMATCH)
    {
        return RESTIR_PT_OUTCOME_F_MISMATCH;
    }

    if(reason == RESTIR_PT_SHIFT_REASON_J_MISMATCH)
    {
        return RESTIR_PT_OUTCOME_J_MISMATCH;
    }

    return reason == RESTIR_PT_SHIFT_REASON_VISIBILITY ? RESTIR_PT_OUTCOME_VISIBILITY : RESTIR_PT_OUTCOME_OTHER_FAILURE;
}

void RestirPtCountWord(RWByteAddressBuffer counters, uint word)
{
    counters.InterlockedAdd(word * 4u, 1u);
}

// Writes one dump record (RESTIR_PT_DUMP_WORDS words) if the case and pool have a free slot.
void RestirPtDumpShift(RWByteAddressBuffer counters, uint pool, uint2 pixel, uint seed, RestirPtShiftResult shift,
    uint pathFlags, float weight)
{
    const uint usedWord = RESTIR_PT_COUNTER_DUMP_USED + shift.shiftCase * 2u + pool;
    uint slot;
    counters.InterlockedAdd(usedWord * 4u, 1u, slot);

    if(slot >= RESTIR_PT_DUMP_SLOTS)
    {
        return;
    }

    const uint record = RESTIR_PT_COUNTER_DUMPS + ((shift.shiftCase * 2u + pool) * RESTIR_PT_DUMP_SLOTS + slot) *
        RESTIR_PT_DUMP_WORDS;
    const uint packed = shift.shiftCase | (shift.reason << 4u) | (shift.quantizationCase << 8u) |
        (shift.previousLobe << 12u) | (shift.vertexLobe << 14u) | (shift.sampledVertexLobe << 16u) |
        (shift.normalFlip ? 1u << 18u : 0u) | (shift.previousDensityLost ? 1u << 19u : 0u) |
        (shift.counterfactuals ? 1u << 20u : 0u) | (shift.hitGeometry ? 1u << 21u : 0u) |
        (shift.visibilitySelfHit ? 1u << 22u : 0u) | (shift.materialChanged ? 1u << 23u : 0u) |
        (((pathFlags >> RESTIR_PT_PATH_RC_INDEX_SHIFT) & 15u) << 24u) | ((pathFlags & 15u) << 28u);
    counters.Store4(record * 4u, uint4(packed, pixel.x | (pixel.y << 16u), seed, asuint(shift.error)));
    counters.Store4((record + 4u) * 4u, asuint(float4(shift.jacobianError, shift.targetLuminance, shift.shiftedLuminance,
        shift.jacobian)));
    counters.Store4((record + 8u) * 4u, asuint(float4(shift.cachedJacobian, shift.distance, shift.vertexCosine,
        shift.sampledCosine)));
    counters.Store4((record + 12u) * 4u, asuint(float4(shift.shiftCosine, shift.directionAngle, shift.offsetLength,
        shift.sampledDensity)));
    counters.Store4((record + 16u) * 4u, asuint(float4(shift.shiftDensity, shift.sampledVertexDensity,
        shift.vertexDensity, shift.previousAlpha)));
    counters.Store4((record + 20u) * 4u, asuint(float4(shift.vertexAlpha, shift.specularProbability,
        shift.sampledSpecularProbability, shift.sampledPrevious.x)));
    counters.Store4((record + 24u) * 4u, asuint(float4(shift.sampledPrevious.y, shift.sampledVertex.x,
        shift.sampledVertex.y, shift.sampledBoth.x)));
    counters.Store4((record + 28u) * 4u, asuint(float4(shift.sampledBoth.y, shift.directionError,
        shift.evaluationCosine, weight)));
    counters.Store4((record + 32u) * 4u, asuint(float4(shift.storedRgb, shift.blockerFraction)));
    counters.Store4((record + 36u) * 4u, asuint(float4(shift.shiftedRgb, shift.localRgb.g)));
    counters.Store4((record + 40u) * 4u, asuint(float4(shift.radianceRgb, shift.localRgb.b)));
    counters.Store4((record + 44u) * 4u, asuint(float4(shift.previousWeightRgb, shift.localRgb.r)));
    counters.Store4((record + 48u) * 4u, asuint(float4(shift.captureArrive, shift.captureLocal, shift.captureCosine,
        shift.shiftCosineBits)));
}

// The round-3 counters of one attempted shift: hit geometry histograms and mechanisms, the quantization histograms, the
// analytic-light experiment and the dumps.
void RestirPtRecordShiftDiagnostics(uint2 pixel, uint seed, RestirPtShiftResult shift, uint pathFlags, float weight)
{
    if(restirPtCounterIndex == 0xFFFFFFFFu || shift.status == 0u)
    {
        return;
    }

    RWByteAddressBuffer counters = ResourceDescriptorHeap[restirPtCounterIndex];
    const bool identity = shift.reason == RESTIR_PT_SHIFT_REASON_IDENTITY;
    const bool hitCase = shift.shiftCase <= RESTIR_PT_SHIFT_CASE_HIT_NEE;

    if(hitCase && shift.hitGeometry)
    {
        const uint outcome = RestirPtGeometryOutcome(shift.reason);
        const uint histogramBase = RESTIR_PT_COUNTER_GEOMETRY + (shift.shiftCase * RESTIR_PT_GEOMETRY_OUTCOMES + outcome) *
            RESTIR_PT_GEOMETRY_HISTOGRAMS * RESTIR_PT_HISTOGRAM_BINS;
        const float distanceRatio = shift.distance / max(restirPtReconnectionMinDistance, 1.0e-30f);
        RestirPtCountWord(counters, histogramBase + RestirPtBin(distanceRatio, 1.0f, 1.25f, 2.0f, 4.0f, 16.0f));
        RestirPtCountWord(counters, histogramBase + RESTIR_PT_HISTOGRAM_BINS + RestirPtCosineBin(shift.vertexCosine));
        RestirPtCountWord(counters, histogramBase + 2u * RESTIR_PT_HISTOGRAM_BINS + RestirPtCosineBin(shift.sampledCosine));
        RestirPtCountWord(counters, histogramBase + 3u * RESTIR_PT_HISTOGRAM_BINS + RestirPtBin(shift.directionAngle,
            1.0e-6f, 1.0e-5f, 1.0e-4f, 1.0e-3f, 1.0e-2f));

        const uint mechanismBase = RESTIR_PT_COUNTER_MECHANISM + (shift.shiftCase * RESTIR_PT_GEOMETRY_OUTCOMES + outcome) *
            RESTIR_PT_MECHANISMS;

        if(shift.counterfactuals && shift.vertexLobe != shift.sampledVertexLobe)
        {
            RestirPtCountWord(counters, mechanismBase + RESTIR_PT_MECHANISM_LOBE_FLIP);
        }

        if(shift.normalFlip)
        {
            RestirPtCountWord(counters, mechanismBase + RESTIR_PT_MECHANISM_NORMAL_FLIP);
        }

        if(shift.counterfactuals && RestirPtWithinTolerance(shift.sampledPrevious))
        {
            RestirPtCountWord(counters, mechanismBase + RESTIR_PT_MECHANISM_SAMPLED_PREVIOUS);
        }

        if(shift.counterfactuals && RestirPtWithinTolerance(shift.sampledVertex))
        {
            RestirPtCountWord(counters, mechanismBase + RESTIR_PT_MECHANISM_SAMPLED_VERTEX);
        }

        if(shift.counterfactuals && RestirPtWithinTolerance(shift.sampledBoth))
        {
            RestirPtCountWord(counters, mechanismBase + RESTIR_PT_MECHANISM_SAMPLED_BOTH);
        }

        if(shift.previousDensityLost)
        {
            RestirPtCountWord(counters, mechanismBase + RESTIR_PT_MECHANISM_PREVIOUS_DENSITY_LOST);
        }

        if(shift.visibilitySelfHit)
        {
            RestirPtCountWord(counters, RESTIR_PT_COUNTER_VISIBILITY_SELF_HIT + shift.shiftCase);
        }

        if(shift.edgeTie)
        {
            RestirPtCountWord(counters, RESTIR_PT_COUNTER_EDGE_TIE + shift.shiftCase);
        }

        if(shift.materialChanged)
        {
            RestirPtCountWord(counters, RESTIR_PT_COUNTER_MATERIAL_CHANGED + shift.shiftCase * 2u + (identity ? 0u : 1u));
        }
    }

    if(shift.storedLobeMismatch)
    {
        RestirPtCountWord(counters, RESTIR_PT_COUNTER_STORED_LOBE_MISMATCH);
    }

    if(shift.octReencodeMismatch)
    {
        RestirPtCountWord(counters, RESTIR_PT_COUNTER_OCT_REENCODE);
    }

    if(shift.octCycleUnstable)
    {
        RestirPtCountWord(counters, RESTIR_PT_COUNTER_OCT_CYCLE);
    }

    if(shift.directionBitsDiffer)
    {
        RestirPtCountWord(counters, RESTIR_PT_COUNTER_DIRECTION_BITS);
    }

    counters.InterlockedMax((RESTIR_PT_COUNTER_MAX_WEIGHTED_ERROR + shift.quantizationCase) * 4u,
        asuint(abs(shift.shiftedLuminance - shift.targetLuminance) * max(weight, 0.0f)));

    if(shift.lightCrossTested)
    {
        const uint lightBase = RESTIR_PT_COUNTER_LIGHT_CROSS + shift.lightCrossType * 8u;
        RestirPtCountWord(counters, lightBase);

        if(shift.lightCrossDirectionError > 1.0e-6f)
        {
            RestirPtCountWord(counters, lightBase + 1u);
        }

        if(shift.lightCrossRadianceError > 1.0e-5f)
        {
            RestirPtCountWord(counters, lightBase + 2u);
        }

        if(shift.lightCrossReference == 2u)
        {
            RestirPtCountWord(counters, lightBase + 6u);
        }
        else if(shift.lightCrossVisible == (shift.lightCrossReference == 1u))
        {
            RestirPtCountWord(counters, lightBase + 3u);
        }
        else
        {
            RestirPtCountWord(counters, lightBase + (shift.lightCrossVisible ? 4u : 5u));
        }

        counters.InterlockedMax((lightBase + 7u) * 4u, asuint(max(shift.lightCrossRadianceError, 0.0f)));
    }

    if(shift.distantLight != 0u)
    {
        RestirPtCountWord(counters, RESTIR_PT_COUNTER_DISTANT_REGENERATED + shift.distantLight - 1u);
        counters.InterlockedMax((RESTIR_PT_COUNTER_DISTANT_ANGLE + shift.distantLight - 1u) * 4u,
            asuint(max(shift.distantAngle, 0.0f)));

        if(shift.distantPdfMismatch)
        {
            RestirPtCountWord(counters, RESTIR_PT_COUNTER_DISTANT_PDF_MISMATCH);
        }
    }

    if(shift.crossTested)
    {
        const uint crossBase = RESTIR_PT_COUNTER_CROSS_VISIBILITY + shift.crossReference * 2u;
        RestirPtCountWord(counters, crossBase + (shift.crossVisible ? 1u : 0u));
        RestirPtCountWord(counters, crossBase + 8u + (shift.crossOffsetEndpointVisible ? 1u : 0u));
        RestirPtCountWord(counters, crossBase + 16u + (shift.crossPreviousRuleVisible ? 1u : 0u));
    }

    // The quantization study (every case).
    {
        const uint quantizationBase = RESTIR_PT_COUNTER_QUANTIZATION + (shift.quantizationCase * 2u + (identity ? 0u : 1u)) *
            RESTIR_PT_QUANTIZATION_HISTOGRAMS * RESTIR_PT_HISTOGRAM_BINS;
        const uint alphaBin = shift.evaluationCosineLobe ? 5u : RestirPtBin(shift.evaluationAlpha, 0.01f, 0.05f, 0.2f, 0.5f,
            1.0e30f);
        RestirPtCountWord(counters, quantizationBase + RestirPtBin(shift.directionError, 1.0e-6f, 1.0e-5f, 3.0e-5f, 1.0e-4f,
            1.0e-3f));
        RestirPtCountWord(counters, quantizationBase + RESTIR_PT_HISTOGRAM_BINS + alphaBin);
        RestirPtCountWord(counters, quantizationBase + 2u * RESTIR_PT_HISTOGRAM_BINS +
            RestirPtCosineBin(shift.evaluationCosine));
        RestirPtCountWord(counters, quantizationBase + 3u * RESTIR_PT_HISTOGRAM_BINS +
            RestirPtMagnitudeBin(abs(shift.shiftedLuminance - shift.targetLuminance)));
        RestirPtCountWord(counters, quantizationBase + 4u * RESTIR_PT_HISTOGRAM_BINS +
            RestirPtMagnitudeBin(shift.targetLuminance));
    }

    if(shift.lightKind < 3u)
    {
        RestirPtCountWord(counters, RESTIR_PT_COUNTER_LIGHT_EXPERIMENT + shift.lightKind * 4u + shift.lightResult);
        counters.InterlockedMax((RESTIR_PT_COUNTER_LIGHT_EXPERIMENT_ANGLE + shift.lightKind) * 4u,
            asuint(max(shift.lightDirectionError, 0.0f)));
    }

    if(!identity)
    {
        RestirPtDumpShift(counters, 0u, pixel, seed, shift, pathFlags, weight);
        const bool severe = shift.reason == RESTIR_PT_SHIFT_REASON_VISIBILITY ||
            max(shift.error, shift.jacobianError) > 1.0f;

        if(severe)
        {
            RestirPtDumpShift(counters, 1u, pixel, seed, shift, pathFlags, weight);
        }
    }
}

// Adds a non-negative value to a 64-bit fixed-point counter (RESTIR_PT_ENERGY_SCALE; low and high words with carry).
void RestirPtAddEnergy(RWByteAddressBuffer counters, uint word, float value)
{
    const float scaled = round(max(value, 0.0f) * RESTIR_PT_ENERGY_SCALE);

    if(!(scaled >= 1.0f) || !(scaled < 1.0e18f))
    {
        return;
    }

    const uint high = uint(scaled * (1.0f / 4294967296.0f));
    const uint low = uint(max(scaled - float(high) * 4294967296.0f, 0.0f));
    uint previous;
    counters.InterlockedAdd(word * 4u, low, previous);
    const uint carry = previous + low < previous ? 1u : 0u;

    if(high + carry != 0u)
    {
        counters.InterlockedAdd((word + 1u) * 4u, high + carry);
    }
}

// The paired energy sums of this sample's shift (wave-reduced per quantization case): F W over evaluated shifts, the
// positive and negative parts of (F' - F) W, and F W of failed shifts. Called by every active lane with the same
// sample index; lanes without a shift pass status 0.
void RestirPtAccumulateShiftEnergy(RestirPtShiftResult shift, float weight)
{
    if(restirPtCounterIndex == 0xFFFFFFFFu)
    {
        return;
    }

    const float stored = shift.targetLuminance * weight;
    const float difference = (shift.shiftedLuminance - shift.targetLuminance) * weight;
    RWByteAddressBuffer counters = ResourceDescriptorHeap[restirPtCounterIndex];

    for(uint quantizationCase = 0u; quantizationCase < 5u; ++quantizationCase)
    {
        const bool evaluated = shift.status == 1u && shift.quantizationCase == quantizationCase;
        const bool failed = shift.status == 2u && shift.quantizationCase == quantizationCase;
        const float4 sums = WaveActiveSum(float4(evaluated ? stored : 0.0f, evaluated ? max(difference, 0.0f) : 0.0f,
            evaluated ? max(-difference, 0.0f) : 0.0f, failed ? stored : 0.0f));

        if(WaveIsFirstLane() && any(sums > 0.0f))
        {
            const uint base = RESTIR_PT_COUNTER_ENERGY + quantizationCase * RESTIR_PT_ENERGY_SUMS * 2u;
            RestirPtAddEnergy(counters, base, sums.x);
            RestirPtAddEnergy(counters, base + 2u, sums.y);
            RestirPtAddEnergy(counters, base + 4u, sums.z);
            RestirPtAddEnergy(counters, base + 6u, sums.w);
        }
    }
}

// -----------------------------------------------------------------------------
// Entry: one thread per pixel
// -----------------------------------------------------------------------------

static const uint RESTIR_PT_GROUP_SIZE = 8u;

float3 RestirPtDebugColor(uint length)
{
    const float3 colors[6] = {float3(0.0f, 0.0f, 0.0f), float3(1.0f, 1.0f, 1.0f), float3(1.0f, 0.2f, 0.1f),
        float3(0.1f, 0.9f, 0.2f), float3(0.2f, 0.4f, 1.0f), float3(1.0f, 0.9f, 0.1f)};
    return colors[min(length, 5u)];
}

[numthreads(RESTIR_PT_GROUP_SIZE, RESTIR_PT_GROUP_SIZE, 1)]
void RestirPtInitialCS(uint3 thread : SV_DispatchThreadID)
{
    const uint2 pixel = thread.xy;

    // M5.4 C6: this frame's instance history (for the next frame's dual motion vectors).
    if((restirPtFlags & RESTIR_PT_FLAG_DUAL_MOTION) != 0u && restirPtInstanceOutputIndex != 0xFFFFFFFFu &&
        restirPtIdentityIndex != 0xFFFFFFFFu && all(pixel < resolution))
    {
        Texture2D<uint2> identity = ResourceDescriptorHeap[restirPtIdentityIndex];
        RWByteAddressBuffer instanceOutput = ResourceDescriptorHeap[restirPtInstanceOutputIndex];
        // M5.5 guide-only PSR: the geometry pass may set the PSR eligibility flag (bit 31) in identity.x; the instance
        // history keeps the identity without it (unmapped all-ones unchanged), as B renders without PSR.
        const uint identityX = identity.Load(int3(pixel, 0)).x;
        instanceOutput.Store(4u * (pixel.y * resolution.x + pixel.x), identityX == 0xFFFFFFFFu ? identityX :
            identityX & 0x7FFFFFFFu);
    }

    // M5.4 C5: the temporal pass's replay list starts empty (a UAV barrier separates this from the temporal pass).
    if((restirPtFlags & RESTIR_PT_FLAG_REPLAY_COMPACTION) != 0u && restirPtReplayListIndex != 0xFFFFFFFFu &&
        all(thread.xy == 0u))
    {
        RWByteAddressBuffer replayList = ResourceDescriptorHeap[restirPtReplayListIndex];
        replayList.Store(0u, 0u);
    }

    const bool active = all(pixel < resolution);
    const bool extract = (restirPtFlags & RESTIR_PT_FLAG_EXTRACT) != 0u;
    const bool reservoirMode = (restirPtFlags & RESTIR_PT_FLAG_RESERVOIR) != 0u;
    uint continuationRays = 0u;
    uint shadowRays = 0u;
    uint candidates = 0u;
    uint wouldClamp = 0u;
    uint rejectedSamples = 0u;
    // The last sample's selected length (debug view) and, per sample, how many selected each length k = 1 .. 5.
    uint selectedLength = 0u;
    uint selectedLengthCounts[5] = {0u, 0u, 0u, 0u, 0u};
    // M5.1-4 T-2.1 (RESTIR_PT_FLAG_SELF_SHIFT): attempted, identical, failed, replay-only; the largest errors.
    uint shiftAttempted = 0u;
    uint shiftIdentity = 0u;
    uint shiftMismatch = 0u;
    uint shiftReplayOnly = 0u;
    float shiftMaxError = 0.0f;
    float shiftMaxJacobianError = 0.0f;
    // M5.2 C0 visibility bench: rays traced and rays visible.
    uint benchRays = 0u;
    uint benchVisible = 0u;
    // M5.2 C1 temporal lookup probe (RESTIR_PT_COUNTER_TEMPORAL_LOOKUP).
    uint temporalProbe[6] = {0u, 0u, 0u, 0u, 0u, 0u};
    // M5.2 temporal resampling (RESTIR_PT_COUNTER_TEMPORAL).
    uint temporalCounts[6] = {0u, 0u, 0u, 0u, 0u, 0u};
#if defined(RESTIR_PT_DIAGNOSTICS)
    // Test A failure breakdown (820-831): forward-shift reasons 0-9, then hit-reconnection and replay-only kinds.
    uint temporalFailures[12] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    // Round trip (14144 + 4 kind + outcome - 1): kinds 0 replay-only, 1 light endpoint, 2 environment, 3 hit.
    uint roundTrips[16] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
#endif

    if(active)
    {
        const PathTraceRasterFirstVertex firstVertex = PathTraceDecodeRasterFirstVertex(pixel);
        float3 sampleSum = float3(0.0f, 0.0f, 0.0f);
        float3 diffuseSum = float3(0.0f, 0.0f, 0.0f);
        float hitDistanceMinimum = PATH_TRACE_HALF_MAXIMUM;
        uint hitSamples = 0u;
        uint missSamples = 0u;
        float normalizedDiffuseHitDistanceSum = 0.0f;
        float rawDiffuseHitDistanceSum = 0.0f;
        uint diffuseHitDistanceSamples = 0u;
        const float diffuseNormalization = extract && firstVertex.state > 0 ?
            PathTraceExtractionDiffuseNormalization(firstVertex.position) : 1.0f;
#if defined(RESTIR_PT_DIAGNOSTICS)
        // T-2.1 P2 validation (self-shift runs only): the previous sample's hit reconnection vertex in this pixel.
        RestirPtReconnection crossVertex = RestirPtNoReconnection();
        // T-2.1 P3 validation (self-shift runs only): the previous sample's table-light endpoint in this pixel.
        uint crossLightIndex = 0xFFFFFFFFu;
#endif

#if defined(RESTIR_PT_TEMPORAL_KERNEL)
        // M5.2 C1 probe: the temporal lookup's stages and what the previous reservoir holds (counters on, history valid).
        if(restirPtCounterIndex != 0xFFFFFFFFu && restirPtHistoryValid != 0u && firstVertex.state > 0 &&
            (restirPtFlags & RESTIR_PT_FLAG_TEMPORAL_PASS) == 0u)
        {
            const RestirPtTemporalNeighbor neighbor = RestirPtFindTemporalNeighbor(pixel, firstVertex,
                RestirPtInitialSeed(pixel, accumulatedSamples));
            temporalProbe[0] = 1u;
            temporalProbe[1] = neighbor.stage >= 2u ? 1u : 0u;
            temporalProbe[2] = neighbor.stage >= 3u ? 1u : 0u;
            temporalProbe[3] = neighbor.stage >= 4u ? 1u : 0u;

            if(neighbor.stage >= 4u)
            {
                RWByteAddressBuffer previousReservoirs = ResourceDescriptorHeap[restirPtPreviousReservoirIndex];
                const uint previousFlags = previousReservoirs.Load((uint(neighbor.pixel.y) * resolution.x +
                    uint(neighbor.pixel.x)) * RESTIR_PT_RESERVOIR_BYTES + 24u);
                temporalProbe[4] = (previousFlags & 15u) != 0u ? 1u : 0u;
                temporalProbe[5] = ((previousFlags >> RESTIR_PT_PATH_RC_INDEX_SHIFT) & 15u) != 0u ? 1u : 0u;
            }
        }
#endif

        for(uint sampleOffset = 0u; sampleOffset < samplesPerFrame; ++sampleOffset)
        {
            const uint sampleIndex = accumulatedSamples + sampleOffset;
            const uint seed = RestirPtInitialSeed(pixel, sampleIndex);
            const RestirPtTreeResult tree = RestirPtTracePathTree(pixel, sampleIndex, firstVertex, seed);
            continuationRays += tree.continuationRays;
            shadowRays += tree.shadowRays;
            candidates += tree.stream.count;
            wouldClamp += tree.wouldClamp;

            float3 radiance;
            float3 diffuse;
#if defined(RESTIR_PT_DIAGNOSTICS)
            // T-2.1 (test only): this sample's self-shift and its unbiased contribution weight (status 0: none).
            RestirPtShiftResult shift = (RestirPtShiftResult)0;
            float shiftWeight = 0.0f;
#endif

            if(reservoirMode)
            {
                const float target = PathTraceLuminance(tree.stream.selected.contribution);
                float unbiasedContributionWeight = target > 0.0f ? tree.stream.weightSum / target : 0.0f;

                if((restirPtFlags & RESTIR_PT_FLAG_NORMALIZATION_ONE_OVER_M) != 0u)
                {
                    unbiasedContributionWeight /= float(max(tree.stream.count, 1u));
                }

                radiance = tree.stream.selected.contribution * unbiasedContributionWeight;
                diffuse = tree.stream.selected.diffuse * unbiasedContributionWeight;
                // M5.2: sample 0 publishes the pixel's reservoir; with temporal reuse it first resamples with history.
                // M5.4 C1: with RESTIR_PT_FLAG_TEMPORAL_PASS the temporal pass resamples instead (this kernel stores the
                // canonical).
                const bool temporalSample = (restirPtFlags & RESTIR_PT_FLAG_TEMPORAL) != 0u && sampleOffset == 0u &&
                    (restirPtFlags & RESTIR_PT_FLAG_TEMPORAL_PASS) == 0u;

#if defined(RESTIR_PT_TEMPORAL_KERNEL)
                if(temporalSample && restirPtHistoryValid != 0u && firstVertex.state > 0)
                {
                    const RestirPtTemporalResult temporalResult = RestirPtTemporalResample(pixel, firstVertex, seed,
                        tree.stream.selected.contribution, unbiasedContributionWeight,
                        RestirPtPathFlags(tree.stream.selected), tree.stream.selected.rc, false);
                    radiance = temporalResult.contribution * temporalResult.unbiasedContributionWeight;

                    if(temporalResult.temporalSelected)
                    {
                        // Denoiser signals only (the image is exact): the selected path's diffuse share follows its stored
                        // x1 lobe (lambda_1).
                        const bool diffuseLobe = ((temporalResult.pathFlags >> RESTIR_PT_PATH_LOBES_SHIFT) & 3u) ==
                            RESTIR_PT_LOBE_COSINE;
                        diffuse = diffuseLobe ? radiance : float3(0.0f, 0.0f, 0.0f);
                    }
                    else
                    {
                        diffuse = tree.stream.selected.diffuse * temporalResult.unbiasedContributionWeight;
                    }

                    RestirPtStoreReservoirFields(pixel, temporalResult.unbiasedContributionWeight,
                        temporalResult.contribution, temporalResult.initSeed, temporalResult.pathFlags, temporalResult.rc);
                    temporalCounts[0] += 1u;
                    temporalCounts[1] += temporalResult.temporalSelected ? 1u : 0u;
                    temporalCounts[2] += temporalResult.forwardValid ? 1u : 0u;
                    temporalCounts[3] += temporalResult.reverseValid ? 1u : 0u;
                    temporalCounts[4] += temporalResult.identityCandidate ? 1u : 0u;
                    temporalCounts[5] += temporalResult.identity ? 1u : 0u;

#if defined(RESTIR_PT_DIAGNOSTICS)
                    // Test A failures: reasons (820-829) and kinds (830 + kind, packed as reason words 830/831 below).
                    for(uint trip = 0u; trip < 16u; ++trip)
                    {
                        roundTrips[trip] += temporalResult.roundTrip != 0u &&
                            trip == 4u * temporalResult.temporalKind + temporalResult.roundTrip - 1u ? 1u : 0u;
                    }

                    if(temporalResult.identityCandidate && !temporalResult.identity)
                    {
                        for(uint reason = 0u; reason < 10u; ++reason)
                        {
                            temporalFailures[reason] += temporalResult.forwardReason == reason ? 1u : 0u;
                        }

                        temporalFailures[10] += temporalResult.temporalKind == 3u ? 1u : 0u;
                        temporalFailures[11] += temporalResult.temporalKind == 0u ? 1u : 0u;
                    }
#endif
                }
#endif

                // F3: the stored reservoir is unchanged (above and below); only the output takes the tree's sum.
                if((restirPtFlags & RESTIR_PT_FLAG_OUTPUT_SUM) != 0u)
                {
                    radiance = tree.stream.sum;
                    diffuse = tree.stream.diffuseSum;
                }
                selectedLength = target > 0.0f ? tree.stream.selected.length : 0u;

                if(selectedLength >= 1u && selectedLength <= 5u)
                {
                    ++selectedLengthCounts[selectedLength - 1u];
                }
                // Without temporal reuse every sample stores (the last one remains); with it, only sample 0 does (above, or
                // here without history).
#if defined(RESTIR_PT_TEMPORAL_KERNEL)
                const bool storeThisSample = (restirPtFlags & RESTIR_PT_FLAG_TEMPORAL) == 0u ||
                    (restirPtFlags & RESTIR_PT_FLAG_TEMPORAL_PASS) != 0u ||
                    (sampleOffset == 0u && (restirPtHistoryValid == 0u || firstVertex.state <= 0));
#else
                // The non-temporal kernel (ReSTIRPTPass selects the temporal one whenever temporal reuse is on).
                const bool storeThisSample = true;
#endif

                if(storeThisSample)
                {
                    RestirPtStoreReservoir(pixel, seed, tree.stream.selected, unbiasedContributionWeight);
                }

#if defined(RESTIR_PT_DIAGNOSTICS)
                if((restirPtFlags & RESTIR_PT_FLAG_SELF_SHIFT) != 0u && target > 0.0f &&
                    restirPtReservoirIndex != 0xFFFFFFFFu)
                {
                    shift = RestirPtSelfShift(pixel, firstVertex, seed, tree.stream.selected, crossVertex,
                        crossLightIndex);
                    shiftWeight = unbiasedContributionWeight;

                    // The next sample's P2 cross pair (this sample's hit reconnection vertex, if any) and P3 cross light
                    // (this sample's table-light endpoint, if any).
                    if(tree.stream.selected.rc.index != 0u &&
                        tree.stream.selected.rc.index < tree.stream.selected.length)
                    {
                        crossVertex = tree.stream.selected.rc;
                    }

                    if(tree.stream.selected.rc.lightType == RESTIR_PT_LIGHT_TABLE &&
                        tree.stream.selected.rc.index == tree.stream.selected.length)
                    {
                        crossLightIndex = tree.stream.selected.rc.instance;
                    }

                    shiftReplayOnly += tree.stream.selected.rc.index == 0u ? 1u : 0u;
                    shiftAttempted += shift.status != 0u ? 1u : 0u;
                    shiftMismatch += shift.status == 2u ? 1u : 0u;

                    if(shift.status == 1u)
                    {
                        // Tolerance: 16-bit barycentrics and octahedral rcWi quantize the stored vertex and direction.
                        shiftIdentity += shift.reason == RESTIR_PT_SHIFT_REASON_IDENTITY ? 1u : 0u;
                        shiftMaxError = max(shiftMaxError, shift.error);
                        shiftMaxJacobianError = max(shiftMaxJacobianError, shift.jacobianError);
                    }

                    // Diagnostic bins (test only; per-sample atomics, self-shift runs only).
                    if(shift.status != 0u && restirPtCounterIndex != 0xFFFFFFFFu)
                    {
                        RWByteAddressBuffer shiftCounters = ResourceDescriptorHeap[restirPtCounterIndex];
                        shiftCounters.InterlockedAdd((RESTIR_PT_COUNTER_SHIFT_BINS + shift.shiftCase * 10u + shift.reason) *
                            4u, 1u);

                        if(shift.status == 1u)
                        {
                            shiftCounters.InterlockedMax((RESTIR_PT_COUNTER_SHIFT_CASE_MAX_JACOBIAN + shift.shiftCase) * 4u,
                                asuint(shift.jacobianError));
                            shiftCounters.InterlockedMax((RESTIR_PT_COUNTER_SHIFT_CASE_MAX_ERROR + shift.shiftCase) * 4u,
                                asuint(shift.error));
                        }

                        RestirPtRecordShiftDiagnostics(pixel, seed, shift, RestirPtPathFlags(tree.stream.selected),
                            unbiasedContributionWeight);
                    }
                }
#endif
            }
            else
            {
                radiance = tree.stream.sum;
                diffuse = tree.stream.diffuseSum;
            }

#if defined(RESTIR_PT_VISIBILITY_BENCH)
            // M5.2 C0 performance baseline: one reconnection-type visibility ray (the result only feeds a counter).
            if((restirPtFlags & RESTIR_PT_FLAG_VISIBILITY_BENCH) != 0u && firstVertex.state > 0 &&
                tree.stream.selected.rc.index != 0u && tree.stream.selected.rc.index < tree.stream.selected.length)
            {
                float3 benchVertex;

                if(RestirPtVertexPosition(tree.stream.selected.rc.instance, tree.stream.selected.rc.primitive,
                    tree.stream.selected.rc.barycentrics, benchVertex))
                {
                    const float3 benchOrigin = PathTraceOffsetRayOrigin(firstVertex.position,
                        firstVertex.surface.geometricNormal);
                    const float3 benchOffset = benchVertex - benchOrigin;
                    const float benchDistance = length(benchOffset);
                    uint blockerInstance;
                    uint blockerPrimitive;
                    float blockerDistance;
                    float blockerEdge;
                    const bool plain = (restirPtFlags & RESTIR_PT_FLAG_VISIBILITY_BENCH_PLAIN) != 0u;
                    const bool visible = benchDistance > 0.0f && RestirPtSegmentVisibleExcept(benchOrigin,
                        benchOffset / benchDistance, benchDistance * 0.999f,
                        plain ? 0xFFFFFFFFu : tree.stream.selected.rc.instance,
                        plain ? 0xFFFFFFFFu : tree.stream.selected.rc.primitive, blockerInstance, blockerPrimitive,
                        blockerDistance, blockerEdge);
                    ++benchRays;
                    benchVisible += visible ? 1u : 0u;
                }
            }
#endif

#if defined(RESTIR_PT_DIAGNOSTICS)
            // The paired energy sums, outside the per-pixel branches (wave operations; every active lane, same sample).
            if((restirPtFlags & RESTIR_PT_FLAG_SELF_SHIFT) != 0u)
            {
                RestirPtAccumulateShiftEnergy(shift, shiftWeight);
            }
#endif

            if((restirPtFlags & RESTIR_PT_FLAG_INJECT_RESULT_NAN) != 0u && RestirPtInjectedPixel(pixel))
            {
                radiance = asfloat(0x7FC00000u).xxx;
            }

            if(!all(isfinite(radiance)) || any(radiance < 0.0f))
            {
                radiance = float3(0.0f, 0.0f, 0.0f);
                diffuse = float3(0.0f, 0.0f, 0.0f);
                ++rejectedSamples;
            }

            sampleSum += radiance;
            diffuseSum += all(isfinite(diffuse)) ? min(max(diffuse, 0.0f), radiance) : float3(0.0f, 0.0f, 0.0f);

            if(firstVertex.state > 0 && tree.primaryHitDistance >= 0.0f)
            {
                if(tree.primarySpecular)
                {
                    if(tree.primaryHitDistance >= PATH_TRACE_HALF_MAXIMUM)
                    {
                        ++missSamples;
                    }
                    else
                    {
                        hitDistanceMinimum = min(hitDistanceMinimum, tree.primaryHitDistance);
                        ++hitSamples;
                    }
                }
                else
                {
                    normalizedDiffuseHitDistanceSum += PathTraceExtractionNormalizedHitDistance(tree.primaryHitDistance,
                        diffuseNormalization);
                    rawDiffuseHitDistanceSum += tree.primaryHitDistance;
                    ++diffuseHitDistanceSamples;
                }
            }
        }

        const float previousCount = float(accumulatedSamples);
        const float totalCount = float(accumulatedSamples + samplesPerFrame);
        float3 mean = sampleSum / totalCount;

        if(accumulatedSamples > 0u)
        {
            mean += accumulationTexture[pixel].rgb * (previousCount / totalCount);
        }

        // M5.3: with spatial reuse the resample pass forms the image from the final reservoirs; M5.4 C1: with the temporal
        // pass, that pass does (or, with spatial reuse too, the resample pass).
        if((restirPtFlags & (RESTIR_PT_FLAG_SPATIAL | RESTIR_PT_FLAG_TEMPORAL_PASS)) == 0u)
        {
            accumulationTexture[pixel] = float4(mean, 1.0f);
            outputTexture[pixel] = float4(min(mean, PATH_TRACE_HALF_MAXIMUM), 1.0f);
        }

        if(restirPtDebugView == 1u)
        {
            outputTexture[pixel] = float4(RestirPtDebugColor(selectedLength), 1.0f);
        }

        if(extract)
        {
            float3 diffuseMean = float3(0.0f, 0.0f, 0.0f);
            float3 specularMean = float3(0.0f, 0.0f, 0.0f);
            float hitDistance = PATH_TRACE_EXTRACTION_NO_SPECULAR;
            float diffuseHitDistance = PATH_TRACE_EXTRACTION_NO_DIFFUSE;
            float rawDiffuseHitDistance = PATH_TRACE_EXTRACTION_NO_DIFFUSE;

            if(firstVertex.state > 0)
            {
                const float frameSamples = float(samplesPerFrame);
                diffuseMean = diffuseSum / frameSamples;
                specularMean = (sampleSum - diffuseSum) / frameSamples;

                if(hitSamples > 0u)
                {
                    hitDistance = hitDistanceMinimum;
                }
                else if(missSamples > 0u)
                {
                    hitDistance = PATH_TRACE_HALF_MAXIMUM;
                }

                if(diffuseHitDistanceSamples > 0u)
                {
                    diffuseHitDistance = normalizedDiffuseHitDistanceSum / float(diffuseHitDistanceSamples);
                    rawDiffuseHitDistance = rawDiffuseHitDistanceSum / float(diffuseHitDistanceSamples);
                }
            }

            if((restirPtFlags & RESTIR_PT_FLAG_RAW_DIFFUSE_HIT_DISTANCE) != 0u &&
                rawDiffuseHitDistanceIndex != PATH_TRACE_NO_RAW_DIFFUSE_HIT_DISTANCE)
            {
                RWTexture2D<float> rawDiffuseHitDistanceOutput = ResourceDescriptorHeap[rawDiffuseHitDistanceIndex];
                rawDiffuseHitDistanceOutput[pixel] = min(rawDiffuseHitDistance, PATH_TRACE_HALF_MAXIMUM);
            }

            RWTexture2D<float4> diffuseOutput = ResourceDescriptorHeap[diffuseOutputIndex];
            RWTexture2D<float4> specularOutput = ResourceDescriptorHeap[specularOutputIndex];
            diffuseOutput[pixel] = float4(clamp(diffuseMean, -PATH_TRACE_HALF_MAXIMUM, PATH_TRACE_HALF_MAXIMUM),
                min(diffuseHitDistance, PATH_TRACE_HALF_MAXIMUM));
            specularOutput[pixel] = float4(clamp(specularMean, -PATH_TRACE_HALF_MAXIMUM, PATH_TRACE_HALF_MAXIMUM),
                min(hitDistance, PATH_TRACE_HALF_MAXIMUM));
        }
    }

    // Statistics as the path tracer keeps them: rays (texels 0-1), completed samples (2-3), rejected samples (4).
    const uint waveRays = WaveActiveSum(continuationRays + shadowRays);
    const uint waveSamples = WaveActiveSum(active ? samplesPerFrame : 0u);
    const uint waveRejected = WaveActiveSum(rejectedSamples);

    if(WaveIsFirstLane())
    {
        if(waveRays > 0u)
        {
            PathTraceAddStatistic64(0u, waveRays);
        }

        if(waveSamples > 0u)
        {
            PathTraceAddStatistic64(2u, waveSamples);
        }

        if(waveRejected > 0u)
        {
            InterlockedAdd(statisticsTexture[uint2(4u, 0u)], waveRejected);
        }
    }

    RestirPtCount(RESTIR_PT_COUNTER_CONTINUATION_RAYS, continuationRays);
    RestirPtCount(RESTIR_PT_COUNTER_SHADOW_RAYS, shadowRays);
    RestirPtCount(RESTIR_PT_COUNTER_CANDIDATES, candidates);
    RestirPtCount(RESTIR_PT_COUNTER_SAMPLES, active ? samplesPerFrame : 0u);
    RestirPtCount(RESTIR_PT_COUNTER_CLAMP_WOULD_ENGAGE, wouldClamp);

    for(uint length = 1u; length <= 5u; ++length)
    {
        RestirPtCount(RESTIR_PT_COUNTER_SELECTED_LENGTH + length - 1u, selectedLengthCounts[length - 1u]);
    }

    if(restirPtHistoryValid != 0u)
    {
        for(uint stage = 0u; stage < 6u; ++stage)
        {
            RestirPtCount(RESTIR_PT_COUNTER_TEMPORAL_LOOKUP + stage, temporalProbe[stage]);
        }

        for(uint outcome = 0u; outcome < 6u; ++outcome)
        {
            RestirPtCount(RESTIR_PT_COUNTER_TEMPORAL + outcome, temporalCounts[outcome]);
        }

#if defined(RESTIR_PT_DIAGNOSTICS)
        for(uint failure = 0u; failure < 12u; ++failure)
        {
            RestirPtCount(820u + failure, temporalFailures[failure]);
        }

        if((restirPtFlags & RESTIR_PT_FLAG_TEMPORAL_ROUND_TRIP) != 0u)
        {
            for(uint trip = 0u; trip < 16u; ++trip)
            {
                RestirPtCount(14144u + trip, roundTrips[trip]);
            }
        }
#endif
    }

#if defined(RESTIR_PT_VISIBILITY_BENCH)
    if((restirPtFlags & RESTIR_PT_FLAG_VISIBILITY_BENCH) != 0u)
    {
        RestirPtCount(RESTIR_PT_COUNTER_VISIBILITY_BENCH, benchRays);
        RestirPtCount(RESTIR_PT_COUNTER_VISIBILITY_BENCH + 1u, benchVisible);
    }
#endif

#if defined(RESTIR_PT_DIAGNOSTICS)
    if((restirPtFlags & RESTIR_PT_FLAG_SELF_SHIFT) != 0u)
    {
        RestirPtCount(RESTIR_PT_COUNTER_SHIFT_ATTEMPTED, shiftAttempted);
        RestirPtCount(RESTIR_PT_COUNTER_SHIFT_IDENTITY, shiftIdentity);
        RestirPtCount(RESTIR_PT_COUNTER_SHIFT_MISMATCH, shiftMismatch);
        RestirPtCount(RESTIR_PT_COUNTER_SHIFT_REPLAY_ONLY, shiftReplayOnly);
        // Non-negative floats order as their bit patterns.
        const float waveMaxError = WaveActiveMax(shiftMaxError);
        const float waveMaxJacobianError = WaveActiveMax(shiftMaxJacobianError);

        if(WaveIsFirstLane() && restirPtCounterIndex != 0xFFFFFFFFu)
        {
            RWByteAddressBuffer counters = ResourceDescriptorHeap[restirPtCounterIndex];
            counters.InterlockedMax(RESTIR_PT_COUNTER_SHIFT_MAX_ERROR * 4u, asuint(waveMaxError));
            counters.InterlockedMax(RESTIR_PT_COUNTER_SHIFT_MAX_JACOBIAN_ERROR * 4u, asuint(waveMaxJacobianError));
        }
    }
#endif
}

// -----------------------------------------------------------------------------
// M5.3 paired spatial reuse (specification section 6.2; [P 3, 6.3, 7])
// -----------------------------------------------------------------------------

// Pixel p's partner in pairing texture t this frame: q = p + M^-1 delta(M p + o), where M is the frame's signed
// permutation (an optional transpose, then flips) and o its offset. Conjugating the texture's involution by the same map
// keeps pairing an involution in screen space [P 3.2].
// restirPtPairingTransform[t]: x = size | flags << 16 (1 transpose, 2 flip x, 4 flip y), y/z = offset, w = first texel.
int2 RestirPtPairingPartner(int2 p, uint t)
{
    const uint4 transform = restirPtPairingTransform[t];
    const int size = int(transform.x & 0xFFFFu);
    const uint flags = transform.x >> 16u;
    int2 mapped = (flags & 1u) != 0u ? p.yx : p;
    mapped.x = (flags & 2u) != 0u ? -mapped.x : mapped.x;
    mapped.y = (flags & 4u) != 0u ? -mapped.y : mapped.y;
    int2 texel = mapped + int2(transform.yz);
    texel = ((texel % size) + size) % size;
    ByteAddressBuffer pairing = ResourceDescriptorHeap[restirPtPairingIndex];
    const uint packed = pairing.Load((transform.w + uint(texel.y * size + texel.x)) * 4u);
    int2 delta = int2(int(packed << 24u) >> 24, int((packed >> 8u) << 24u) >> 24);
    delta.x = (flags & 2u) != 0u ? -delta.x : delta.x;
    delta.y = (flags & 4u) != 0u ? -delta.y : delta.y;
    return p + ((flags & 1u) != 0u ? delta.yx : delta);
}

// [O]'s spatial rejection test, applied from both pixels (M5.3 decision 1) so that both members of a pair agree: shading
// normals within 60 degrees and each linear depth within 10% of the other's.
bool RestirPtSpatialCompatible(PathTraceRasterFirstVertex a, PathTraceRasterFirstVertex b)
{
    if(a.state <= 0 || b.state <= 0)
    {
        return false;
    }

    const float depthA = dot(restirPtCameraForward, a.position - cameraPosition);
    const float depthB = dot(restirPtCameraForward, b.position - cameraPosition);
    const float difference = abs(depthA - depthB);
    return dot(a.surface.normal, b.surface.normal) >= 0.5f && difference < 0.1f * abs(depthA) &&
        difference < 0.1f * abs(depthB);
}

// Whether partner q of pixel p in texture t is a valid pair (on screen, both surfaces, compatible).
bool RestirPtSpatialPairValid(uint2 pixel, PathTraceRasterFirstVertex self, uint t, out int2 partner,
    out PathTraceRasterFirstVertex partnerVertex)
{
    partner = RestirPtPairingPartner(int2(pixel), t);
    partnerVertex = (PathTraceRasterFirstVertex)0;

    if(any(partner < 0) || any(partner >= int2(resolution)) || all(uint2(partner) == pixel))
    {
        return false;
    }

    partnerVertex = PathTraceDecodeRasterFirstVertex(uint2(partner));
    return RestirPtSpatialCompatible(self, partnerVertex);
}

bool RestirPtStoredHasSample(RestirPtStoredReservoir stored)
{
    return (stored.pathFlags & 15u) != 0u && stored.unbiasedContributionWeight > 0.0f &&
        PathTraceLuminance(stored.contribution) > 0.0f;
}

// A pair record (RESTIR_PT_PAIR_BYTES): pixel p's own sample shifted into its partner in texture t: F', the Jacobian,
// the destination's cached Jacobian term (rcJacobian of the shifted path, so a reservoir that adopts it stores it
// exactly) and validity.
struct RestirPtPairRecord
{
    bool valid;
    // M5.4 C2: whether the pair itself is valid (on screen, both surfaces, compatible; RestirPtSpatialPairValid), so the
    // resample pass need not decode the partners' surfaces again.
    bool pairValid;
    float3 contribution;
    float jacobian;
    float destinationJacobian;
};

uint RestirPtPairAddress(uint t, uint2 pixel)
{
    return ((t * resolution.y + pixel.y) * resolution.x + pixel.x) * RESTIR_PT_PAIR_BYTES;
}

RestirPtPairRecord RestirPtLoadPair(uint t, uint2 pixel)
{
    RWByteAddressBuffer pairs = ResourceDescriptorHeap[restirPtPairsIndex];
    const uint address = RestirPtPairAddress(t, pixel);
    const uint4 first = pairs.Load4(address);
    const uint4 second = pairs.Load4(address + 16u);
    RestirPtPairRecord record;
    record.contribution = asfloat(first.xyz);
    record.jacobian = asfloat(first.w);
    record.destinationJacobian = asfloat(second.x);
    record.valid = second.y != 0u;
    record.pairValid = second.z != 0u;
    return record;
}

#if defined(RESTIR_PT_TEMPORAL_KERNEL)
// M5.4 C1: temporal reuse as its own pass [S 9 Table 1; spec section 9.4 step 4], after the initial kernel (which, with
// RESTIR_PT_FLAG_TEMPORAL_PASS, stored the canonical reservoir and left the image). Per pixel: the canonical as stored
// (the only quantized field, rcWi, is what the fused kernel's reverse shift already read decoded), the same
// RestirPtTemporalResample, the result stored in place. Without spatial reuse it then forms the image exactly as the
// fused kernel did at one sample per frame, and the denoiser colour split (hit distances stay the initial kernel's).
[numthreads(RESTIR_PT_GROUP_SIZE, RESTIR_PT_GROUP_SIZE, 1)]
void RestirPtTemporalCS(uint3 thread : SV_DispatchThreadID)
{
    // M5.4 C5: the replay dispatch (restirPtReplayPass 1) takes its pixels from the compacted list, in order.
    const bool compaction = (restirPtFlags & RESTIR_PT_FLAG_REPLAY_COMPACTION) != 0u &&
        restirPtReplayListIndex != 0xFFFFFFFFu;
    const bool replayDispatch = compaction && restirPtReplayPass == 1u;
    uint2 pixel = thread.xy;
    bool active = all(pixel < resolution);

    if(replayDispatch)
    {
        RWByteAddressBuffer replayList = ResourceDescriptorHeap[restirPtReplayListIndex];
        const uint entry = thread.y * resolution.x + thread.x;
        active = active && entry < replayList.Load(0u);
        const uint packed = active ? replayList.Load(4u * (entry + 1u)) : 0u;
        pixel = uint2(packed & 0xFFFFu, packed >> 16u);
    }

    uint temporalProbe[6] = {0u, 0u, 0u, 0u, 0u, 0u};
    uint temporalCounts[6] = {0u, 0u, 0u, 0u, 0u, 0u};
#if defined(RESTIR_PT_DIAGNOSTICS)
    uint temporalFailures[12] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    uint roundTrips[16] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
#endif

    if(active)
    {
        const bool extract = (restirPtFlags & RESTIR_PT_FLAG_EXTRACT) != 0u;
        const PathTraceRasterFirstVertex firstVertex = PathTraceDecodeRasterFirstVertex(pixel);
        const RestirPtStoredReservoir canonical = RestirPtLoadStoredReservoir(restirPtReservoirIndex, pixel);
        const uint seed = RestirPtInitialSeed(pixel, accumulatedSamples);
        float3 radiance = canonical.contribution * canonical.unbiasedContributionWeight;
        bool resampled = false;
        bool temporalSelected = false;
        bool selectedDiffuseLobe = false;
        float unbiasedContributionWeight = canonical.unbiasedContributionWeight;

        // M5.2 C1 probe: the temporal lookup's stages and what the previous reservoir holds (counters on, history valid;
        // a deferred pixel's lane leaves before its counts are added, and the replay dispatch probes it again: once each).
        if(restirPtCounterIndex != 0xFFFFFFFFu && restirPtHistoryValid != 0u && firstVertex.state > 0)
        {
            const RestirPtTemporalNeighbor neighbor = RestirPtFindTemporalNeighbor(pixel, firstVertex, seed);
            temporalProbe[0] = 1u;
            temporalProbe[1] = neighbor.stage >= 2u ? 1u : 0u;
            temporalProbe[2] = neighbor.stage >= 3u ? 1u : 0u;
            temporalProbe[3] = neighbor.stage >= 4u ? 1u : 0u;

            // M5.4 C6: how far the dual lookup got (1 attempted .. 5 accepted).
            for(uint progress = 1u; progress <= 5u; ++progress)
            {
                RestirPtCount(RESTIR_PT_COUNTER_DUAL + progress - 1u, neighbor.dualProgress >= progress ? 1u : 0u);
            }

            if(neighbor.stage >= 4u)
            {
                RWByteAddressBuffer previousReservoirs = ResourceDescriptorHeap[restirPtPreviousReservoirIndex];
                const uint previousFlags = previousReservoirs.Load((uint(neighbor.pixel.y) * resolution.x +
                    uint(neighbor.pixel.x)) * RESTIR_PT_RESERVOIR_BYTES + 24u);
                temporalProbe[4] = (previousFlags & 15u) != 0u ? 1u : 0u;
                temporalProbe[5] = ((previousFlags >> RESTIR_PT_PATH_RC_INDEX_SHIFT) & 15u) != 0u ? 1u : 0u;
            }
        }

        if((restirPtFlags & RESTIR_PT_FLAG_TEMPORAL) != 0u && restirPtHistoryValid != 0u && firstVertex.state > 0)
        {
            const RestirPtTemporalResult temporalResult = RestirPtTemporalResample(pixel, firstVertex, seed,
                canonical.contribution, canonical.unbiasedContributionWeight, canonical.pathFlags, canonical.rc,
                compaction && !replayDispatch);

            // M5.4 C5: deferred to the replay dispatch, which does everything below for this pixel.
            if(temporalResult.deferred)
            {
                RWByteAddressBuffer replayList = ResourceDescriptorHeap[restirPtReplayListIndex];
                uint slot;
                replayList.InterlockedAdd(0u, 1u, slot);
                replayList.Store(4u * (slot + 1u), pixel.x | (pixel.y << 16u));
                return;
            }

            radiance = temporalResult.contribution * temporalResult.unbiasedContributionWeight;
            resampled = true;
            temporalSelected = temporalResult.temporalSelected;

            // M5.5 diagnostics (debug view 2; harness only): per pixel, the selected sample's source (1 canonical, 2
            // temporal), its reconnection kind (0 none/replay-only, 1 light endpoint, 2 environment endpoint, 3 hit + NEE
            // edge: cached light visibility, 4 hit + continuation), m_c and M_t'. The image and accumulation are unchanged.
            if(restirPtDebugView == 2u)
            {
                const uint selectedLength = temporalResult.pathFlags & 15u;
                const uint selectedIndex = temporalResult.rc.index;
                const bool selectedNee = (temporalResult.pathFlags & RESTIR_PT_PATH_NEE) != 0u;
                const float kind = selectedIndex == 0u ? 0.0f : selectedIndex == selectedLength ? (selectedNee ? 1.0f : 2.0f) :
                    (selectedNee && selectedIndex + 1u == selectedLength ? 3.0f : 4.0f);
                outputTexture[pixel] = float4(temporalResult.temporalSelected ? 2.0f : 1.0f, kind,
                    temporalResult.canonicalMis, temporalResult.temporalConfidence);
            }
            selectedDiffuseLobe = ((temporalResult.pathFlags >> RESTIR_PT_PATH_LOBES_SHIFT) & 3u) == RESTIR_PT_LOBE_COSINE;
            unbiasedContributionWeight = temporalResult.unbiasedContributionWeight;
            RestirPtStoreReservoirFields(pixel, temporalResult.unbiasedContributionWeight, temporalResult.contribution,
                temporalResult.initSeed, temporalResult.pathFlags, temporalResult.rc);
            temporalCounts[0] += 1u;
            temporalCounts[1] += temporalResult.temporalSelected ? 1u : 0u;
            temporalCounts[2] += temporalResult.forwardValid ? 1u : 0u;
            temporalCounts[3] += temporalResult.reverseValid ? 1u : 0u;
            temporalCounts[4] += temporalResult.identityCandidate ? 1u : 0u;
            temporalCounts[5] += temporalResult.identity ? 1u : 0u;

#if defined(RESTIR_PT_DIAGNOSTICS)
            // Test A failures: reasons (820-829) and kinds (830 + kind, packed as reason words 830/831 below).
            for(uint trip = 0u; trip < 16u; ++trip)
            {
                roundTrips[trip] += temporalResult.roundTrip != 0u &&
                    trip == 4u * temporalResult.temporalKind + temporalResult.roundTrip - 1u ? 1u : 0u;
            }

            if(temporalResult.identityCandidate && !temporalResult.identity)
            {
                for(uint reason = 0u; reason < 10u; ++reason)
                {
                    temporalFailures[reason] += temporalResult.forwardReason == reason ? 1u : 0u;
                }

                temporalFailures[10] += temporalResult.temporalKind == 3u ? 1u : 0u;
                temporalFailures[11] += temporalResult.temporalKind == 0u ? 1u : 0u;
            }
#endif
        }

        // The image and the denoiser split, without spatial reuse (with it, the resample pass forms both).
        if((restirPtFlags & RESTIR_PT_FLAG_SPATIAL) == 0u)
        {
            if(!all(isfinite(radiance)) || any(radiance < 0.0f))
            {
                radiance = float3(0.0f, 0.0f, 0.0f);
            }

            // M5.5-B decorrelation: show the initial kernel's estimate (the extraction textures, left as they are); the
            // stored reservoir is unchanged.
            // B1a stagnancy runs in the resample pass (spatial reuse); temporal-only keeps the Uniform probability.
            const bool decorrelated = extract && firstVertex.state > 0 &&
                RestirPtDecorrelate(pixel, restirPtDecorrelation);

            if(decorrelated)
            {
                RWTexture2D<float4> initialDiffuse = ResourceDescriptorHeap[diffuseOutputIndex];
                RWTexture2D<float4> initialSpecular = ResourceDescriptorHeap[specularOutputIndex];
                radiance = initialDiffuse[pixel].rgb + initialSpecular[pixel].rgb;
            }

            const float previousCount = float(accumulatedSamples);
            const float totalCount = float(accumulatedSamples + samplesPerFrame);
            float3 mean = radiance / totalCount;

            if(accumulatedSamples > 0u)
            {
                mean += accumulationTexture[pixel].rgb * (previousCount / totalCount);
            }

            accumulationTexture[pixel] = float4(mean, 1.0f);

            // Debug views 1 (selected length) and 2 (M5.5 temporal diagnostics) keep their output.
            if(restirPtDebugView == 0u)
            {
                outputTexture[pixel] = float4(min(mean, PATH_TRACE_HALF_MAXIMUM), 1.0f);
            }

            // Denoiser signals only, where resampling ran (elsewhere the initial kernel's split is this pixel's): the
            // selected path's x1 lobe (lambda_1) when the temporal sample won, else the canonical's diffuse share, which
            // the initial kernel wrote at its own W (rescaled to the resampled W).
            if(extract && resampled && !decorrelated)
            {
                RWTexture2D<float4> diffuseOutput = ResourceDescriptorHeap[diffuseOutputIndex];
                RWTexture2D<float4> specularOutput = ResourceDescriptorHeap[specularOutputIndex];
                const float4 canonicalDiffuse = diffuseOutput[pixel];
                float3 diffuse = temporalSelected ? (selectedDiffuseLobe ? radiance : float3(0.0f, 0.0f, 0.0f)) :
                    canonical.unbiasedContributionWeight > 0.0f ? canonicalDiffuse.rgb * (unbiasedContributionWeight /
                    canonical.unbiasedContributionWeight) : float3(0.0f, 0.0f, 0.0f);
                diffuse = all(isfinite(diffuse)) ? min(max(diffuse, 0.0f), radiance) : float3(0.0f, 0.0f, 0.0f);
                diffuseOutput[pixel] = float4(clamp(diffuse, -PATH_TRACE_HALF_MAXIMUM, PATH_TRACE_HALF_MAXIMUM),
                    canonicalDiffuse.w);
                specularOutput[pixel] = float4(clamp(radiance - diffuse, -PATH_TRACE_HALF_MAXIMUM, PATH_TRACE_HALF_MAXIMUM),
                    specularOutput[pixel].w);
            }
        }
    }

    if(restirPtHistoryValid != 0u)
    {
        for(uint stage = 0u; stage < 6u; ++stage)
        {
            RestirPtCount(RESTIR_PT_COUNTER_TEMPORAL_LOOKUP + stage, temporalProbe[stage]);
        }

        for(uint outcome = 0u; outcome < 6u; ++outcome)
        {
            RestirPtCount(RESTIR_PT_COUNTER_TEMPORAL + outcome, temporalCounts[outcome]);
        }

#if defined(RESTIR_PT_DIAGNOSTICS)
        for(uint failure = 0u; failure < 12u; ++failure)
        {
            RestirPtCount(820u + failure, temporalFailures[failure]);
        }

        if((restirPtFlags & RESTIR_PT_FLAG_TEMPORAL_ROUND_TRIP) != 0u)
        {
            for(uint trip = 0u; trip < 16u; ++trip)
            {
                RestirPtCount(14144u + trip, roundTrips[trip]);
            }
        }
#endif
    }
}
#endif

// M5.4 C7: the duplication map [P 9, step 1]: per pixel, the number of pixels in its 17 x 17 window whose final
// reservoir holds a shifted copy of the same initial candidate (the same initSeed), out of 288. An 8 x 8 group loads its
// 24 x 24 neighbourhood of seeds into groupshared memory once.
static const uint RESTIR_PT_DUPLICATION_TILE = 24u;
static const uint RESTIR_PT_NO_SEED = 0xFFFFFFFFu;
groupshared uint restirPtDuplicationSeeds[RESTIR_PT_DUPLICATION_TILE * RESTIR_PT_DUPLICATION_TILE];

[numthreads(RESTIR_PT_GROUP_SIZE, RESTIR_PT_GROUP_SIZE, 1)]
void RestirPtDuplicationCS(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID, uint3 thread : SV_DispatchThreadID)
{
    // The final reservoirs: the resample pass's output with spatial reuse, else the initial/temporal set.
    const uint finalIndex = (restirPtFlags & RESTIR_PT_FLAG_SPATIAL) != 0u ? restirPtSpatialOutputIndex :
        restirPtReservoirIndex;
    RWByteAddressBuffer reservoirs = ResourceDescriptorHeap[finalIndex];
    const int2 origin = int2(group.xy * RESTIR_PT_GROUP_SIZE) - 8;

    for(uint entry = local.y * RESTIR_PT_GROUP_SIZE + local.x; entry < RESTIR_PT_DUPLICATION_TILE * RESTIR_PT_DUPLICATION_TILE;
        entry += RESTIR_PT_GROUP_SIZE * RESTIR_PT_GROUP_SIZE)
    {
        const int2 position = origin + int2(entry % RESTIR_PT_DUPLICATION_TILE, entry / RESTIR_PT_DUPLICATION_TILE);
        uint seed = RESTIR_PT_NO_SEED;

        if(all(position >= 0) && all(position < int2(resolution)))
        {
            const uint address = (uint(position.y) * resolution.x + uint(position.x)) * RESTIR_PT_RESERVOIR_BYTES;
            const float weight = asfloat(reservoirs.Load(address));
            const uint pathFlags = reservoirs.Load(address + 24u);
            seed = weight > 0.0f && (pathFlags & 15u) != 0u ? reservoirs.Load(address + 16u) : RESTIR_PT_NO_SEED;
        }

        restirPtDuplicationSeeds[entry] = seed;
    }

    GroupMemoryBarrierWithGroupSync();
    const bool active = all(thread.xy < resolution);
    uint count = 0u;
    const uint own = restirPtDuplicationSeeds[(local.y + 8u) * RESTIR_PT_DUPLICATION_TILE + local.x + 8u];

    if(active && own != RESTIR_PT_NO_SEED)
    {
        for(uint y = 0u; y < 17u; ++y)
        {
            for(uint x = 0u; x < 17u; ++x)
            {
                count += (x != 8u || y != 8u) && restirPtDuplicationSeeds[(local.y + y) * RESTIR_PT_DUPLICATION_TILE +
                    local.x + x] == own ? 1u : 0u;
            }
        }
    }

    if(active)
    {
        RWByteAddressBuffer duplication = ResourceDescriptorHeap[restirPtDuplicationOutputIndex];
        duplication.Store(4u * (thread.y * resolution.x + thread.x), count);
    }

    const float duplicated = float(count) / 288.0f;
    const bool sampled = active && own != RESTIR_PT_NO_SEED;
    RestirPtCount(RESTIR_PT_COUNTER_DUPLICATION, sampled ? 1u : 0u);
    RestirPtCount(RESTIR_PT_COUNTER_DUPLICATION + 1u, sampled ? count : 0u);
    const uint bin = count == 0u ? 0u : duplicated <= 0.01f ? 1u : duplicated <= 0.05f ? 2u : duplicated <= 0.2f ? 3u :
        duplicated <= 0.5f ? 4u : 5u;

    for(uint b = 0u; b < 6u; ++b)
    {
        RestirPtCount(RESTIR_PT_COUNTER_DUPLICATION + 2u + b, sampled && bin == b ? 1u : 0u);
    }
}

// Pair pre-pass [P 3, 7; spec section 6.2 step 1]: each pixel shifts its own (post-temporal) sample into each valid
// partner, with the M5.2 shift (replay, reconnection, light and environment endpoints, visibility). Each pair's two
// directions are produced by its two pixels, so every shift is computed once.
[numthreads(RESTIR_PT_GROUP_SIZE, RESTIR_PT_GROUP_SIZE, 1)]
void RestirPtSpatialPairsCS(uint3 thread : SV_DispatchThreadID)
{
    const uint2 pixel = thread.xy;
    const bool active = all(pixel < resolution);
    // Counters (RESTIR_PT_COUNTER_SPATIAL + n): 0 pairs from surfaces, 1 partner on screen and compatible, 2 with a
    // sample to shift, 3 shift valid; round trip (diagnostics): 4 back invalid, 5 F mismatch, 6 J product != 1,
    // 7 consistent.
    uint counts[8] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};

    if(active)
    {
        const PathTraceRasterFirstVertex self = PathTraceDecodeRasterFirstVertex(pixel);
        const RestirPtStoredReservoir own = RestirPtLoadStoredReservoir(restirPtSpatialScratchIndex, pixel);
        const bool hasSample = self.state > 0 && RestirPtStoredHasSample(own);
        RWByteAddressBuffer pairs = ResourceDescriptorHeap[restirPtPairsIndex];

        [loop]
        for(uint t = 0u; t < min(restirPtSpatialPairCount, RESTIR_PT_SPATIAL_NEIGHBOURS); ++t)
        {
            RestirPtShifted shifted = (RestirPtShifted)0;
            int2 partner;
            PathTraceRasterFirstVertex partnerVertex;
            const bool pairValid = self.state > 0 && RestirPtSpatialPairValid(pixel, self, t, partner, partnerVertex);
            counts[0] += self.state > 0 ? 1u : 0u;
            counts[1] += pairValid ? 1u : 0u;
#if defined(RESTIR_PT_DIAGNOSTICS)
            // S0 on the GPU: this frame's transformed pairing must be an involution (partner's partner = p; counter 13).
            if((restirPtFlags & RESTIR_PT_FLAG_SPATIAL_ROUND_TRIP) != 0u && self.state > 0 &&
                any(RestirPtPairingPartner(partner, t) != int2(pixel)))
            {
                RestirPtCount(RESTIR_PT_COUNTER_SPATIAL + 13u, 1u);
            }
#endif

            if(pairValid && hasSample)
            {
                counts[2] += 1u;
                shifted = RestirPtShiftSample(partnerVertex, own.initSeed, own.pathFlags, own.rc,
                    RestirPtFootprintThreshold(cameraPosition, partnerVertex));
                counts[3] += shifted.valid ? 1u : 0u;
#if defined(RESTIR_PT_DIAGNOSTICS)
                // Round trip: the shifted path back into p must reproduce F_p, with J_back = 1 / J_forward.
                if(shifted.valid && (restirPtFlags & RESTIR_PT_FLAG_SPATIAL_ROUND_TRIP) != 0u)
                {
                    const RestirPtShifted back = RestirPtShiftSample(self, own.initSeed, own.pathFlags, shifted.rc,
                        RestirPtFootprintThreshold(cameraPosition, self));
                    const uint outcome = !back.valid ? 0u : RestirPtRelativeError(PathTraceLuminance(back.contribution),
                        PathTraceLuminance(own.contribution)) > 1.0e-3f ? 1u :
                        abs(shifted.jacobian * back.jacobian - 1.0f) > 1.0e-3f ? 2u : 3u;

                    for(uint slot = 0u; slot < 4u; ++slot)
                    {
                        counts[4u + slot] += outcome == slot ? 1u : 0u;
                    }
                }
#endif
            }

            const uint address = RestirPtPairAddress(t, pixel);
            // M5.4 C3 (rejected): 20 B records (packed or as two arrays) saved 75 MB at 1080p but slowed this pass by
            // 1-2.6 ms, so the 32 B record stays (performance over memory).
            pairs.Store4(address, uint4(asuint(shifted.contribution), asuint(shifted.jacobian)));
            pairs.Store4(address + 16u, uint4(asuint(shifted.rc.jacobian), shifted.valid ? 1u : 0u, pairValid ? 1u : 0u,
                0u));
        }
    }

    for(uint counter = 0u; counter < 8u; ++counter)
    {
        RestirPtCount(RESTIR_PT_COUNTER_SPATIAL + counter, counts[counter]);
    }
}

// Spatial GRIS [P 2.2, 3; spec section 6.2 step 2]: pairwise MIS [O] over the canonical sample c and the k
// geometrically valid partners q_i (k independent of whether a partner holds a sample):
//   b(y) = (M_c / k) p_c(y);  canonical: m_c = (1 + sum_i b / (b + M_qi p_qi(T_(c->qi) Y_c) J_(c->qi))) / (k + 1);
//   partner: m_i = [M_qi p_qi(X_qi) / (M_qi p_qi(X_qi) + b(T X_qi) J_(qi->c))] / (k + 1);
//   w_c = m_c p_c(Y_c) W_c,  w_i = m_i p_c(T X_qi) W_qi J_(qi->c);  Y ~ w;  W = sum w / p_c(Y);  M = M_c + sum M_qi.
// The weights sum to 1 over the k + 1 techniques for every path. Shading: F(Y) W, or with colour-noise reduction
// [P 6.3] the vector sum of m_i F_i W_i J_i. No shift or visibility ray here: the pre-pass traced them.
// B1b: the group's shaded weights for the firefly test (RTXDI_BoilingFilterInternal over this 8 x 8 group).
groupshared float s_restirPtFireflyWeights[RESTIR_PT_GROUP_SIZE * RESTIR_PT_GROUP_SIZE];
groupshared float s_restirPtFireflyAverage;

[numthreads(RESTIR_PT_GROUP_SIZE, RESTIR_PT_GROUP_SIZE, 1)]
void RestirPtSpatialResampleCS(uint3 thread : SV_DispatchThreadID, uint3 groupThread : SV_GroupThreadID)
{
    const uint2 pixel = thread.xy;
    const bool active = all(pixel < resolution);
    // The shading below runs after the (group-uniform) firefly test, so its inputs live at function scope.
    bool finalExtract = false;
    int finalState = 0;
    uint finalPathFlags = 0u;
    float3 finalRadiance = float3(0.0f, 0.0f, 0.0f);
    float3 finalDiffuse = float3(0.0f, 0.0f, 0.0f);
    // Counters (RESTIR_PT_COUNTER_SPATIAL + 8 + n): 0 surface pixels resampled, 1 valid partners, 2 partner candidates
    // with weight, 3 partner selected, 4 non-finite results (emptied).
    uint counts[5] = {0u, 0u, 0u, 0u, 0u};

    if(active)
    {
        const bool extract = (restirPtFlags & RESTIR_PT_FLAG_EXTRACT) != 0u;
        const bool colourNoiseReduction = (restirPtFlags & RESTIR_PT_FLAG_SPATIAL_COLOUR) != 0u;
        const PathTraceRasterFirstVertex self = PathTraceDecodeRasterFirstVertex(pixel);
        const RestirPtStoredReservoir canonical = RestirPtLoadStoredReservoir(restirPtSpatialScratchIndex, pixel);
        float unbiasedContributionWeight = canonical.unbiasedContributionWeight;
        float3 contribution = canonical.contribution;
        uint initSeed = canonical.initSeed;
        uint pathFlags = canonical.pathFlags;
        RestirPtReconnection rc = canonical.rc;
        float3 radiance = self.state < 0 ? float3(0.0f, 0.0f, 0.0f) : canonical.contribution *
            canonical.unbiasedContributionWeight;
        float3 diffuse = float3(0.0f, 0.0f, 0.0f);

        if(self.state > 0)
        {
            counts[0] += 1u;
            uint validMask = 0u;

            // The pre-pass recorded each pair's validity (RestirPtSpatialPairValid, both pixels agree by symmetry).
            // Optimization experiment A5: only the first restirPtSpatialPairCount pairs exist this frame; paired MIS
            // normalizes over the valid ones (k + 1), so any count stays consistent.
            for(uint t = 0u; t < min(restirPtSpatialPairCount, RESTIR_PT_SPATIAL_NEIGHBOURS); ++t)
            {
                validMask |= RestirPtLoadPair(t, pixel).pairValid ? 1u << t : 0u;
            }

            const uint k = countbits(validMask);
            counts[1] += k;
            const float canonicalTarget = PathTraceLuminance(canonical.contribution);
            const float canonicalConfidence = float((canonical.pathFlags >> 8u) & 255u);
            const float normalization = 1.0f / float(k + 1u);
            const uint seed = RestirPtInitialSeed(pixel, accumulatedSamples);
            float canonicalMisSum = 1.0f;
            float confidence = canonicalConfidence;
            float weightSum = 0.0f;
            float selectedTarget = canonicalTarget;
            float3 colour = float3(0.0f, 0.0f, 0.0f);
            float3 colourDiffuse = float3(0.0f, 0.0f, 0.0f);
            bool partnerSelected = false;

            [loop]
            for(uint t = 0u; t < RESTIR_PT_SPATIAL_NEIGHBOURS; ++t)
            {
                if((validMask & (1u << t)) == 0u)
                {
                    continue;
                }

                const int2 partner = RestirPtPairingPartner(int2(pixel), t);
                const RestirPtStoredReservoir neighbour = RestirPtLoadStoredReservoir(restirPtSpatialScratchIndex,
                    uint2(partner));
                const float neighbourConfidence = float((neighbour.pathFlags >> 8u) & 255u);
                confidence += neighbourConfidence;
                // p's sample in q (p's record) and q's sample in p (q's record: q's partner in texture t is p).
                const RestirPtPairRecord toPartner = RestirPtLoadPair(t, pixel);
                const RestirPtPairRecord fromPartner = RestirPtLoadPair(t, uint2(partner));
                const float canonicalTerm = canonicalConfidence / float(k) * canonicalTarget;
                const float canonicalInPartner = toPartner.valid ? neighbourConfidence *
                    PathTraceLuminance(toPartner.contribution) * toPartner.jacobian : 0.0f;
                canonicalMisSum += canonicalTerm > 0.0f ? canonicalTerm / (canonicalTerm + canonicalInPartner) : 0.0f;
                const float shiftedTarget = fromPartner.valid ? PathTraceLuminance(fromPartner.contribution) : 0.0f;

                if(!(shiftedTarget > 0.0f) || !RestirPtStoredHasSample(neighbour))
                {
                    continue;
                }

                const float neighbourTerm = neighbourConfidence * PathTraceLuminance(neighbour.contribution);
                const float crossTerm = canonicalConfidence / float(k) * shiftedTarget * fromPartner.jacobian;
                const float mis = neighbourTerm / (neighbourTerm + crossTerm) * normalization;
                const float scale = mis * neighbour.unbiasedContributionWeight * fromPartner.jacobian;
                const float weight = scale * shiftedTarget;

                if(!(weight > 0.0f) || !isfinite(weight))
                {
                    continue;
                }

                counts[2] += 1u;
                weightSum += weight;
                colour += scale * fromPartner.contribution;
                const bool partnerDiffuse = ((neighbour.pathFlags >> RESTIR_PT_PATH_LOBES_SHIFT) & 3u) ==
                    RESTIR_PT_LOBE_COSINE;
                colourDiffuse += partnerDiffuse ? scale * fromPartner.contribution : float3(0.0f, 0.0f, 0.0f);

                if(RestirPtRandom(seed, 0u, RESTIR_PT_DIMENSION_SPATIAL + 1u + t) * weightSum < weight)
                {
                    partnerSelected = true;
                    selectedTarget = shiftedTarget;
                    contribution = fromPartner.contribution;
                    initSeed = neighbour.initSeed;
                    pathFlags = neighbour.pathFlags;
                    rc = neighbour.rc;
                    rc.jacobian = fromPartner.destinationJacobian;
                }
            }

            // The canonical candidate.
            const float canonicalScale = canonicalMisSum * normalization * canonical.unbiasedContributionWeight;
            const float canonicalWeight = canonicalScale * canonicalTarget;

            if(canonicalWeight > 0.0f && isfinite(canonicalWeight))
            {
                weightSum += canonicalWeight;
                colour += canonicalScale * canonical.contribution;
                const bool canonicalDiffuse = ((canonical.pathFlags >> RESTIR_PT_PATH_LOBES_SHIFT) & 3u) ==
                    RESTIR_PT_LOBE_COSINE;
                colourDiffuse += canonicalDiffuse ? canonicalScale * canonical.contribution : float3(0.0f, 0.0f, 0.0f);

                if(RestirPtRandom(seed, 0u, RESTIR_PT_DIMENSION_SPATIAL) * weightSum < canonicalWeight)
                {
                    partnerSelected = false;
                    selectedTarget = canonicalTarget;
                    contribution = canonical.contribution;
                    initSeed = canonical.initSeed;
                    pathFlags = canonical.pathFlags;
                    rc = canonical.rc;
                }
            }

            counts[3] += partnerSelected ? 1u : 0u;
            unbiasedContributionWeight = weightSum > 0.0f && selectedTarget > 0.0f ? weightSum / selectedTarget : 0.0f;
            const bool selectedDiffuse = ((pathFlags >> RESTIR_PT_PATH_LOBES_SHIFT) & 3u) == RESTIR_PT_LOBE_COSINE;
            radiance = colourNoiseReduction ? colour : contribution * unbiasedContributionWeight;
            diffuse = colourNoiseReduction ? colourDiffuse : (selectedDiffuse ? radiance : float3(0.0f, 0.0f, 0.0f));

            if(!isfinite(unbiasedContributionWeight) || !all(isfinite(radiance)) || !all(isfinite(diffuse)))
            {
                counts[4] += 1u;
                unbiasedContributionWeight = 0.0f;
                contribution = float3(0.0f, 0.0f, 0.0f);
                radiance = float3(0.0f, 0.0f, 0.0f);
                diffuse = float3(0.0f, 0.0f, 0.0f);
            }

            if(!(unbiasedContributionWeight > 0.0f))
            {
                contribution = float3(0.0f, 0.0f, 0.0f);
            }

            pathFlags = RestirPtFlagsWithConfidence(pathFlags, uint(round(confidence)));
        }

        RestirPtStoreReservoirFieldsTo(restirPtSpatialOutputIndex, pixel, unbiasedContributionWeight, contribution,
            initSeed, pathFlags, rc);
        finalExtract = extract;
        finalState = self.state;
        finalPathFlags = pathFlags;
        finalRadiance = radiance;
        finalDiffuse = diffuse;
    }

    // B1b firefly replacement (RTXDI_PTDetectDecorrelationFireflies with RTXDI_BoilingFilterInternal): a pixel whose shaded
    // reservoir luminance exceeds (10 / strength - 9) x the group's average nonzero luminance (strength 0.7: 5.29x) is
    // decorrelated with probability 1. Only with temporal history and extraction (RR or NRD frames). Every thread of the
    // group takes part (barriers).
    bool firefly = false;

    if((restirPtFlags & RESTIR_PT_FLAG_FIREFLY) != 0u)
    {
        const uint lane = groupThread.y * RESTIR_PT_GROUP_SIZE + groupThread.x;
        const float weight = finalState > 0 && all(isfinite(finalRadiance)) ? PathTraceLuminance(finalRadiance) : 0.0f;
        s_restirPtFireflyWeights[lane] = weight;
        GroupMemoryBarrierWithGroupSync();

        if(lane == 0u)
        {
            float sum = 0.0f;
            float count = 0.0f;

            for(uint index = 0u; index < RESTIR_PT_GROUP_SIZE * RESTIR_PT_GROUP_SIZE; ++index)
            {
                sum += s_restirPtFireflyWeights[index];
                count += s_restirPtFireflyWeights[index] > 0.0f ? 1.0f : 0.0f;
            }

            s_restirPtFireflyAverage = count > 0.0f ? sum / count : 0.0f;
        }

        GroupMemoryBarrierWithGroupSync();
        firefly = restirPtHistoryValid != 0u && weight > s_restirPtFireflyAverage * (10.0f / 0.7f - 9.0f);
    }

    if(active)
    {
        const bool extract = finalExtract;
        float3 radiance = finalRadiance;
        float3 diffuse = finalDiffuse;

        // M5.5-B decorrelation: show the initial kernel's estimate (still in the extraction textures) instead of the reused
        // one; the stored final reservoir above is unchanged.
        // B1a: the probability is evaluated for every active pixel (stagnancy mode writes its smoothed map here).
        // B1b: a firefly is decorrelated with probability 1.
        const float decorrelationProbability = RestirPtDecorrelationProbability(pixel, finalState > 0 ? finalPathFlags : 0u);

        if(extract && finalState > 0 && (firefly || RestirPtDecorrelate(pixel, decorrelationProbability)))
        {
            RWTexture2D<float4> initialDiffuse = ResourceDescriptorHeap[diffuseOutputIndex];
            RWTexture2D<float4> initialSpecular = ResourceDescriptorHeap[specularOutputIndex];
            float3 initialDiffuseValue = initialDiffuse[pixel].rgb;
            float3 initialRadiance = initialDiffuseValue + initialSpecular[pixel].rgb;

            // RTXDI's bias reduction (fireflyReplacementBiasReduction, bound 15): for a new firefly (age 0) the replacement
            // may not exceed 15 x the replaced value, so one firefly is not traded for a brighter one.
            if(firefly && RestirPtPathAge(finalPathFlags) == 0u)
            {
                const float replacedLuminance = PathTraceLuminance(radiance);
                const float initialLuminance = PathTraceLuminance(initialRadiance);
                const float scale = initialLuminance > 15.0f * replacedLuminance && initialLuminance > 0.0f ?
                    15.0f * replacedLuminance / initialLuminance : 1.0f;
                initialRadiance *= scale;
                initialDiffuseValue *= scale;
            }

            diffuse = initialDiffuseValue;
            radiance = initialRadiance;
        }

        // The image (spatial reuse requires one sample per frame; the initial kernel skipped these writes).
        const float previousCount = float(accumulatedSamples);
        const float totalCount = float(accumulatedSamples + samplesPerFrame);
        float3 mean = radiance / totalCount;

        if(accumulatedSamples > 0u)
        {
            mean += accumulationTexture[pixel].rgb * (previousCount / totalCount);
        }

        accumulationTexture[pixel] = float4(mean, 1.0f);

        // M5.5 debug view 2 keeps the temporal pass's diagnostics in the output.
        if(restirPtDebugView != 2u)
        {
            outputTexture[pixel] = float4(min(mean, PATH_TRACE_HALF_MAXIMUM), 1.0f);
        }

        // Denoiser signals only: the colour split by each candidate's x1 lobe (lambda_1); hit distances stay the initial
        // kernel's.
        if(extract && finalState > 0)
        {
            RWTexture2D<float4> diffuseOutput = ResourceDescriptorHeap[diffuseOutputIndex];
            RWTexture2D<float4> specularOutput = ResourceDescriptorHeap[specularOutputIndex];
            diffuseOutput[pixel] = float4(clamp(diffuse, -PATH_TRACE_HALF_MAXIMUM, PATH_TRACE_HALF_MAXIMUM),
                diffuseOutput[pixel].w);
            specularOutput[pixel] = float4(clamp(radiance - diffuse, -PATH_TRACE_HALF_MAXIMUM, PATH_TRACE_HALF_MAXIMUM),
                specularOutput[pixel].w);
        }
    }

    for(uint counter = 0u; counter < 5u; ++counter)
    {
        RestirPtCount(RESTIR_PT_COUNTER_SPATIAL + 8u + counter, counts[counter]);
    }
}
