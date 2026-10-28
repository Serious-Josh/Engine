// SHARC.4D-R shader-floor experiment (diagnostics): the D6 cache's query-only hooks, in place of the learning hooks of
// PathTraceRadianceCacheHarvest.hlsli in the query library (PATH_TRACE_RADIANCE_CACHE_QUERY,
// PathTraceReferenceRadianceCacheQuery*.hlsl). There is no harvest state, record, insertion or training statistic: at
// path vertex 1 of a raster-primary path, an eligible surface (the learning rule) is keyed and, by the binding's
// queryMode, looked up in the published entries. Nothing a query reads reaches the image or the path: a lookup's
// result only feeds a never-taken store or the coverage counters.

#include "RadianceCacheD6.hlsli"

cbuffer RadianceCacheD6QueryConstants : register(b3)
{
    RadianceCacheD6Binding radianceCache;
};

// Temporary spatial/coverage investigation. Rebuild/redeploy C++ and shaders together after changing the selector.
// 0: production; 1: green = successful substitution, red = x1 reached but continued, blue = no x1 surface;
// 2: successful substitution's full binding-key identity; 3: its cached hit-only radiance (after the existing clamp,
// before throughput or distant-miss addition); 4: spatial-cell identity only (ignores normal bin and quarter slice).
// Renderer preserves this frame's display output before denoiser resolves and bypasses DLSS/filtering/AgX.
// Colors average over this frame's samples only; accumulation,
// extraction, history-prior output and the actual path estimator remain untouched. Black in 2-4 = no substitution.
#include "PathTraceSharcDiagnosticConfig.hlsli"

#if defined(PATH_TRACE_RADIANCE_CACHE_RENDER) && PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
static float3 g_pathTraceSharcDiagnosticSample = float3(0.0f, 0.0f, 0.0f);
static float3 g_pathTraceSharcDiagnosticSum = float3(0.0f, 0.0f, 0.0f);

void PathTraceSharcDiagnosticBeginSample()
{
    g_pathTraceSharcDiagnosticSample = PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC == 1 ?
        float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 0.0f, 0.0f);
}

float3 PathTraceSharcDiagnosticKeyColor(uint2 key)
{
    // Mix a copy of the exact lookup key; never change the key or use the hash-table slot as spatial identity.
    uint hash = RadianceCacheD6Mix(key.x ^ RadianceCacheD6Mix(key.y));
    return 0.15f + 0.85f * float3(hash & 255u, (hash >> 8u) & 255u, (hash >> 16u) & 255u) / 255.0f;
}
#endif

// SHARC.4D-S termination probe (PATH_TRACE_RADIANCE_CACHE_TERMINATION, the QueryTermination libraries): queryMode 4
// or 5 decides at vertex 1 whether the path ends after x1's BSDF sample. The path's radiance then
// lacks everything after x1's next-event estimation; no cached value replaces it. A timing probe only: its image is
// wrong by construction and is never presented.
// SHARC Experiments B and C (render library only; 0 disables and compiles out all of their code). B: x1 substitutes only
// when the x0-to-x1 segment is at least the lookup cell's edge. C: after x0's specular strategy, x1 substitutes only
// when x0's GGX cone spread over that segment reaches the cell's edge. Defined before the harvest, which carries C's
// x0 state.
#ifndef PATH_TRACE_SHARC_EXPERIMENT_B_SEGMENT_LENGTH
#define PATH_TRACE_SHARC_EXPERIMENT_B_SEGMENT_LENGTH 0
#endif

#ifndef PATH_TRACE_SHARC_EXPERIMENT_C_CONE_SPREAD
#define PATH_TRACE_SHARC_EXPERIMENT_C_CONE_SPREAD 0
#endif

// SHARC Experiment D (render library only; 0 compiles it out): a path's x1 lookup runs only when x0's finalized
// perceptual roughness (PathTraceFinalizeSurface) is at least the threshold, the same decision for every sample of the
// pixel. Independent of x0's sampled strategy. A NaN roughness fails the comparison.
#ifndef PATH_TRACE_SHARC_EXPERIMENT_D_X0_ROUGHNESS
#define PATH_TRACE_SHARC_EXPERIMENT_D_X0_ROUGHNESS 1
#endif

#ifndef PATH_TRACE_SHARC_EXPERIMENT_D_ROUGHNESS_THRESHOLD
#define PATH_TRACE_SHARC_EXPERIMENT_D_ROUGHNESS_THRESHOLD 0.30f
#endif

#if defined(PATH_TRACE_RADIANCE_CACHE_RENDER) && \
    (PATH_TRACE_SHARC_EXPERIMENT_B_SEGMENT_LENGTH != 0 || PATH_TRACE_SHARC_EXPERIMENT_C_CONE_SPREAD != 0)
#define PATH_TRACE_SHARC_EXPERIMENT_BC 1
#else
#define PATH_TRACE_SHARC_EXPERIMENT_BC 0
#endif

#if defined(PATH_TRACE_RADIANCE_CACHE_RENDER) && PATH_TRACE_SHARC_EXPERIMENT_D_X0_ROUGHNESS != 0
#define PATH_TRACE_SHARC_EXPERIMENT_D 1
#else
#define PATH_TRACE_SHARC_EXPERIMENT_D 0
#endif

// Any experiment gate: B, C and D are independent conditions, all of which must admit the lookup.
#define PATH_TRACE_SHARC_EXPERIMENT_GATE (PATH_TRACE_SHARC_EXPERIMENT_BC || PATH_TRACE_SHARC_EXPERIMENT_D)

struct PathTraceRadianceCacheHarvest
{
#ifdef PATH_TRACE_RADIANCE_CACHE_TERMINATION
    bool terminate;
#elif defined(PATH_TRACE_RADIANCE_CACHE_RENDER) && PATH_TRACE_SHARC_EXPERIMENT_C_CONE_SPREAD != 0
    // Experiment C: the GGX alpha of x0's specular strategy (0 for its delta mirror), or negative after its diffuse
    // strategy (no cone restriction). Recorded by the Continue hook at vertex 0 from the sample already drawn.
    float x0ConeAlpha;
#else
    uint unused;
#endif
};

PathTraceRadianceCacheHarvest PathTraceRadianceCacheHarvestBegin()
{
    return (PathTraceRadianceCacheHarvest)0;
}

// Adds the wave's lanes for which condition holds to a query counter.
void PathTraceRadianceCacheCountQuery(uint counter, bool condition)
{
    uint count = WaveActiveCountBits(condition);

    if(WaveIsFirstLane() && count > 0u)
    {
        RWByteAddressBuffer statistics = ResourceDescriptorHeap[radianceCache.queryStatisticsIndex];
        statistics.InterlockedAdd(counter * 4u, count);
    }
}

void PathTraceRadianceCacheHarvestVertex(inout PathTraceRadianceCacheHarvest harvest, uint primary, uint vertex,
    float3 position, PathTraceSurface surface)
{
#ifdef PATH_TRACE_RADIANCE_CACHE_RENDER
    // The render library looks up and substitutes at x1's Continue hook instead (PathTraceRadianceCacheSubstitute).
#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC == 1
    if(vertex == 1u && (primary == PATH_TRACE_PRIMARY_RASTER || primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT))
    {
        g_pathTraceSharcDiagnosticSample = float3(1.0f, 0.0f, 0.0f);
    }
#endif
    return;
#endif

    if(vertex != 1u || primary != PATH_TRACE_PRIMARY_RASTER || radianceCache.queryMode == 0u)
    {
        return;
    }

    uint2 key;
    bool eligible = !surface.deltaSpecular && surface.material.model <= 1u && surface.material.metallic <= 0.1f &&
        RadianceCacheD6MakeBindingKey(radianceCache, position, surface.geometricNormal, key);

    // Mode 1: the key only (one never-taken store depends on it).
    if(radianceCache.queryMode == 1u)
    {
        if(eligible && (key.x ^ key.y) == 0x9E3779B9u && position.x == -1234.5f)
        {
            RWByteAddressBuffer statistics = ResourceDescriptorHeap[radianceCache.statisticsIndex];
            statistics.InterlockedAdd(RADIANCE_CACHE_D6_STATISTIC_REJECTED_SAMPLES * 4u, 1u);
        }

        return;
    }

#ifdef PATH_TRACE_RADIANCE_CACHE_TERMINATION
    if(radianceCache.queryMode >= 4u)
    {
        // Mode 4: the published entry's sample count (word 18) and flags (word 22) decide; mode 5 ends every eligible
        // path without a lookup.
        bool checked = radianceCache.queryMode == 4u && eligible;
        bool trusted = checked && RadianceCacheD6BindingAccepted(radianceCache);
        uint terminationSlot = trusted ? RadianceCacheD6FindSlot(radianceCache, key) : RADIANCE_CACHE_D6_NO_SLOT;
        bool published = false;
        float sampleCount = 0.0f;

        if(terminationSlot != RADIANCE_CACHE_D6_NO_SLOT)
        {
            RWByteAddressBuffer entries = ResourceDescriptorHeap[radianceCache.publishedIndex];
            uint metaBase = (terminationSlot * RADIANCE_CACHE_D6_PUBLISHED_WORDS + RADIANCE_CACHE_D6_META_WORD) * 4u;
            sampleCount = asfloat(entries.Load(metaBase));
            published = (entries.Load(metaBase + 16u) & RADIANCE_CACHE_D6_FLAG_PUBLISHED) != 0u;
        }

        harvest.terminate = radianceCache.queryMode == 5u ? eligible :
            published && sampleCount >= radianceCache.queryThreshold;

        if(radianceCache.queryCounters != 0u)
        {
            PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_ELIGIBLE, eligible);
            PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_HEADER_REJECTED, checked && !trusted);
            PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_FOUND, published);
            PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_COUNT_16, published && sampleCount >= 16.0f);
            PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_COUNT_64, published && sampleCount >= 64.0f);
            PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_COUNT_256, published && sampleCount >= 256.0f);
            PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_TERMINATED, harvest.terminate);
        }

        return;
    }
#endif

    // A lookup trusts the entries only if the generation header names the frame's epoch and identity.
    bool accepted = RadianceCacheD6BindingAccepted(radianceCache);
    uint slot = eligible && accepted ? RadianceCacheD6FindSlot(radianceCache, key) : RADIANCE_CACHE_D6_NO_SLOT;
    float found = 0.0f;
    float count = 0.0f;
    uint lastUpdate = 0u;
    uint flags = 0u;

    if(slot != RADIANCE_CACHE_D6_NO_SLOT)
    {
        RWByteAddressBuffer published = ResourceDescriptorHeap[radianceCache.publishedIndex];

        [unroll]
        for(uint word = 0u; word < RADIANCE_CACHE_D6_PUBLISHED_WORDS; word += 4u)
        {
            uint4 words = published.Load4((slot * RADIANCE_CACHE_D6_PUBLISHED_WORDS + word) * 4u);
            found += asfloat(words.x) + asfloat(words.y) + asfloat(words.z) + asfloat(words.w);

            if(word == 16u)
            {
                // Words 18-23: sample count, relative variance, last update, first frame, flags, keep-alive.
                count = asfloat(words.z);
            }
            else if(word == 20u)
            {
                lastUpdate = words.x;
                flags = words.z;
            }
        }
    }

    // Mode 2: the lookup's result is discarded (one never-taken store depends on it).
    if(radianceCache.queryMode == 2u)
    {
        if(found == -1234.5f)
        {
            RWByteAddressBuffer statistics = ResourceDescriptorHeap[radianceCache.statisticsIndex];
            statistics.InterlockedAdd(RADIANCE_CACHE_D6_STATISTIC_REJECTED_SAMPLES * 4u, 1u);
        }

        return;
    }

    // Mode 3: coverage. A published entry is a hit; its sample count and its age in resolves since its last update.
    bool hit = slot != RADIANCE_CACHE_D6_NO_SLOT && (flags & RADIANCE_CACHE_D6_FLAG_PUBLISHED) != 0u;
    uint age = radianceCache.frameIndex - lastUpdate;
    PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_ELIGIBLE, eligible);
    PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_HEADER_REJECTED, eligible && !accepted);
    PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_FOUND, hit);
    PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_COUNT_64, hit && count >= 64.0f);
    PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_COUNT_256, hit && count >= 256.0f);
    PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_COUNT_1024, hit && count >= 1024.0f);
    PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_COUNT_4096, hit && count >= 4096.0f);
    PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_AGE_1, hit && age <= 1u);
    PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_AGE_16, hit && age <= 16u);
    PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_AGE_64, hit && age <= 64u);
}

#ifdef PATH_TRACE_RADIANCE_CACHE_RENDER
// SHARC.6A history prior (build/sharc-6a/REPORT.md section 1), extraction frames of the render library only, with the
// binding's priorOutputIndex. The denoiser's diffuse signal at x0 is D0 + Ind:
//   D0   x0's direct diffuse light: its next-event estimate (RTXDI's in the ReSTIR DI indirect pass) and, in Raster
//        Primary, the miss of x0's continuation, each through x0's diffuse share
//   Ind  the diffuse share of the hit-only radiance along x0's continuation: Int f_d(l) L_hit(x0, l) (n.l) dl
// The prior is this frame's own D0 (accumulated by the Direct and Miss hooks below, exactly as the extraction adds it)
// plus Ind reconstructed from x0's primary-class entry, whose samples are this estimator's L_hit at x0 (training
// library, PathTraceRadianceCacheHarvest.hlsli), in the signal's units: Ind ~= rho T(v) E / pi. E = Int L (n.l) dl is
// exact from the two m = 0 moments (RadianceCacheD6Irradiance); with f_d = rho_EON(l, v) T(l) T(v) / T_avg
// (PathTraceDiffuseWeight), rho T(v) is the lobe's directional albedo, exact for uniform incident light and approximate
// for directional light. (A 16-direction quadrature of the lobe against the reconstructed radiance measured +0.25%
// against the signal but cost 6-19 ms.)
static float3 g_pathTraceHistoryPriorDirect = float3(0.0f, 0.0f, 0.0f);
static uint g_pathTraceHistoryPriorSamples = 0u;

#define PathTraceRadianceCacheHarvestDirect(harvest, vertex, direct) \
    if(extract && vertex == 0u) \
    { \
        g_pathTraceHistoryPriorDirect += directDiffuse; \
    }

// The loop adds a non-delta miss of x0's continuation only in Raster Primary; it is part of D0 there.
#define PathTraceRadianceCacheHarvestMiss(harvest, vertex, missRadiance) \
    if(extract && vertex == 1u) \
    { \
        g_pathTraceHistoryPriorDirect += extraction.diffuseThroughput * (missRadiance); \
    }

// Adds the wave's lanes for which condition holds to a prior counter.
void PathTraceHistoryPriorCount(uint counter, bool condition)
{
    uint count = WaveActiveCountBits(condition);

    if(WaveIsFirstLane() && count > 0u)
    {
        RWByteAddressBuffer counters = ResourceDescriptorHeap[radianceCache.priorCountersIndex];
        counters.InterlockedAdd(counter * 4u, count);
    }
}

// The ray generation's part of the prior: this frame's D0 in RGB, and 1 in alpha on a supported surface,
// RADIANCE_CACHE_D6_PRIOR_CODE_UNSUPPORTED on another, 0 without a surface. The cache lookup, T(v) and Ind run in the
// temporal pass, only where it rejects the history (PathTraceTemporal.hlsl step 6): per pixel here, the lookup measured
// +1.4 ms in the sealed room and T(v)'s table lookup +1.1 ms in the 16-light hall (build/sharc-6a/REPORT.md
// section 10).
void PathTraceHistoryPriorWrite(PathTraceRasterFirstVertex rasterFirstVertex, float3 direct)
{
    uint2 pixel = DispatchRaysIndex().xy;
    bool surfacePixel = rasterFirstVertex.state > 0;
    PathTraceSurface surface = rasterFirstVertex.surface;
    bool supported = surfacePixel && !surface.deltaSpecular && surface.material.model <= 1u &&
        surface.material.metallic <= 0.1f;
    float code = supported ? 1.0f : RADIANCE_CACHE_D6_PRIOR_CODE_UNSUPPORTED;
    RWTexture2D<float4> output = ResourceDescriptorHeap[radianceCache.priorOutputIndex];
    output[pixel] = float4(all(isfinite(direct)) ? direct : float3(0.0f, 0.0f, 0.0f), surfacePixel ? code : 0.0f);

    if(radianceCache.priorCountersIndex != 0u)
    {
        PathTraceHistoryPriorCount(RADIANCE_CACHE_D6_PRIOR_SURFACES, surfacePixel);
        PathTraceHistoryPriorCount(RADIANCE_CACHE_D6_PRIOR_UNSUPPORTED, surfacePixel && !supported);
    }
}

// After each path of an extraction frame (every path of the pixel, in order): the last one writes the prior with the
// mean D0 of the pixel's paths.
void PathTraceHistoryPriorEndPath(uint primary, PathTraceRasterFirstVertex rasterFirstVertex)
{
    bool rasterPrimary = primary == PATH_TRACE_PRIMARY_RASTER || primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT;

    if(!rasterPrimary || radianceCache.priorOutputIndex == 0u || ++g_pathTraceHistoryPriorSamples < samplesPerFrame)
    {
        return;
    }

    PathTraceHistoryPriorWrite(rasterFirstVertex, g_pathTraceHistoryPriorDirect / float(samplesPerFrame));
    g_pathTraceHistoryPriorDirect = float3(0.0f, 0.0f, 0.0f);
    g_pathTraceHistoryPriorSamples = 0u;
}
#else
void PathTraceRadianceCacheHarvestDirect(inout PathTraceRadianceCacheHarvest harvest, uint vertex, float3 direct)
{
}

void PathTraceRadianceCacheHarvestMiss(inout PathTraceRadianceCacheHarvest harvest, uint vertex, float3 missRadiance)
{
}
#endif

#ifdef PATH_TRACE_RADIANCE_CACHE_TERMINATION
bool PathTraceRadianceCacheQueryTerminates(PathTraceRadianceCacheHarvest harvest, uint vertex)
{
    return vertex == 1u && harvest.terminate;
}

// The probe's termination. PathTraceReference.hlsl's Continue hook statement sits directly in the path loop, after the
// vertex's BSDF sample and before the continuation is traced; in this library it expands to a real break out of that
// loop, so none of the path's later work executes.
#define PathTraceRadianceCacheHarvestContinue(harvest, vertex, bsdfSample) \
    if(PathTraceRadianceCacheQueryTerminates(harvest, vertex)) break
#elif defined(PATH_TRACE_RADIANCE_CACHE_RENDER)
// SHARC.4E cached-radiance rendering (build/sharc-4e/REPORT.md section 2). At x1 of a raster-primary path, after x1's
// next-event estimation and BSDF sample (throughput already holds x1's weight f cos / p), an eligible surface whose
// published entry has at least queryThreshold samples completes the path:
//   radiance += throughput * max(L~(w), 0)                  the cached hit-only incident radiance along w
//   radiance += throughput * miss(w)  if w's ray misses       exactly the reference's MIS-weighted distant light,
//                                                            which the cache excludes (a shadow ray: no x2 shading)
// and the path ends. Roulette at x1 is replaced by its expectation, as the training suffix divided by survival.
// Everything else (misses, ineligible surfaces, delta continuations, unconfident entries) continues the reference path
// unchanged: the lookup draws no random numbers.
// SHARC.4E.1: with signal extraction (the denoiser's input), the completed path's radiance after x1 also joins the
// diffuse signal through the vertex-0 diffuse throughput, exactly as the loop adds x1's later light; the specular
// signal is the remainder the ray generation derives.
// Experiment A: minimum perceptual roughness for production x1 substitution only (not GGX alpha).
// Test values: 0.0, 0.1, 0.2, 0.25, 0.4. Zero disables the gate and preserves baseline eligibility.
static const float PATH_TRACE_SHARC_EXPERIMENT_A_MIN_PERCEPTUAL_ROUGHNESS = 0.0f;

#if PATH_TRACE_SHARC_EXPERIMENT_BC
// Experiments B and C: whether the x0-to-x1 segment admits the x1 lookup on the cell of level (the lookup key's own
// adaptive level; 0 on the uniform grid). A NaN input, a degenerate cell or a zero-length segment rejects, so the path
// continues as after any failed lookup.
bool PathTraceSharcExperimentAdmits(float segmentLength, float x0ConeAlpha, uint level)
{
    float cellEdge = exp2(float(level)) / radianceCache.inverseCellSize;
    bool admitted = true;

#if PATH_TRACE_SHARC_EXPERIMENT_B_SEGMENT_LENGTH != 0
    admitted = admitted && segmentLength >= cellEdge;
#endif

#if PATH_TRACE_SHARC_EXPERIMENT_C_CONE_SPREAD != 0
    // S = 2 L sqrt(0.5 a^2 / (1 - a^2)) >= edge, squared without a square root or division:
    // 2 L^2 a^2 >= edge^2 (1 - a^2). a = 0 (delta) always rejects; a = 1 always admits.
    if(!(x0ConeAlpha < 0.0f))
    {
        float alphaSquared = x0ConeAlpha * x0ConeAlpha;
        admitted = admitted &&
            2.0f * segmentLength * segmentLength * alphaSquared >= cellEdge * cellEdge * (1.0f - alphaSquared);
    }
#endif

    return admitted;
}
#endif

bool PathTraceRadianceCacheSubstitute(uint primary, float3 position, PathTraceSurface surface,
    PathTraceBSDFSample bsdfSample, float3 throughput, inout float3 radiance, inout uint rayCount, bool extract,
    inout PathTraceExtraction extraction
#if PATH_TRACE_SHARC_EXPERIMENT_BC
    , float segmentLength, float x0ConeAlpha
#endif
#if PATH_TRACE_SHARC_EXPERIMENT_D
    , float x0Roughness
#endif
    )
{
    // SHARC.5B: the ReSTIR DI indirect pass (RASTER_SPLIT_INDIRECT) differs from Raster Primary only at x0 (RTXDI's
    // direct light, and no non-delta miss of x0's continuation); from x1 on it is the same estimator, so the same
    // cached quantity and substitution apply (build/sharc-5a/REPORT.md section 4).
    bool rasterPrimary = primary == PATH_TRACE_PRIMARY_RASTER || primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT;

    if(!rasterPrimary || radianceCache.queryMode == 0u || bsdfSample.delta)
    {
        return false;
    }

    uint2 key;
#if PATH_TRACE_SHARC_EXPERIMENT_BC
    // The binding key's own construction, which also names its level.
    uint level = 0u;
    bool eligible = !surface.deltaSpecular && surface.material.model <= 1u && surface.material.metallic <= 0.1f &&
        RadianceCacheD6MakeClassKey(radianceCache, position, surface.geometricNormal, false, key, level);
#else
    bool eligible = !surface.deltaSpecular && surface.material.model <= 1u && surface.material.metallic <= 0.1f &&
        RadianceCacheD6MakeBindingKey(radianceCache, position, surface.geometricNormal, key);
#endif
    if(PATH_TRACE_SHARC_EXPERIMENT_A_MIN_PERCEPTUAL_ROUGHNESS > 0.0f)
    {
        eligible = eligible && surface.material.roughness >= PATH_TRACE_SHARC_EXPERIMENT_A_MIN_PERCEPTUAL_ROUGHNESS;
    }
#if PATH_TRACE_SHARC_EXPERIMENT_GATE
    // A rejection leaves ELIGIBLE and HEADER_REJECTED as they were; it is ELIGIBLE minus HEADER_REJECTED, ABSENT_* and
    // FOUND. It skips the header check, lookup and visibility ray; the path continues as after any failed lookup.
#if PATH_TRACE_SHARC_EXPERIMENT_BC && PATH_TRACE_SHARC_EXPERIMENT_D
    bool admitted = eligible && x0Roughness >= PATH_TRACE_SHARC_EXPERIMENT_D_ROUGHNESS_THRESHOLD &&
        PathTraceSharcExperimentAdmits(segmentLength, x0ConeAlpha, level);
#elif PATH_TRACE_SHARC_EXPERIMENT_BC
    bool admitted = eligible && PathTraceSharcExperimentAdmits(segmentLength, x0ConeAlpha, level);
#else
    bool admitted = eligible && x0Roughness >= PATH_TRACE_SHARC_EXPERIMENT_D_ROUGHNESS_THRESHOLD;
#endif
    bool trusted = admitted && RadianceCacheD6BindingAccepted(radianceCache);
#else
    bool trusted = eligible && RadianceCacheD6BindingAccepted(radianceCache);
#endif
    uint slot = trusted ? RadianceCacheD6FindSlot(radianceCache, key) : RADIANCE_CACHE_D6_NO_SLOT;
    bool substitute = false;
    bool clamped = false;
    bool missed = false;
    float sampleCount = 0.0f;
    bool published = false;

    if(slot != RADIANCE_CACHE_D6_NO_SLOT)
    {
        RWByteAddressBuffer entries = ResourceDescriptorHeap[radianceCache.publishedIndex];
        uint base = slot * RADIANCE_CACHE_D6_PUBLISHED_WORDS * 4u;
        uint4 meta = entries.Load4(base + 16u * 4u);
        uint flags = entries.Load(base + 22u * 4u);
        sampleCount = asfloat(meta.z);
        published = (flags & RADIANCE_CACHE_D6_FLAG_PUBLISHED) != 0u;
        substitute = published && sampleCount >= radianceCache.queryThreshold;

        if(substitute)
        {
            float3 moments[RADIANCE_CACHE_D6_MOMENTS];

            [unroll]
            for(uint moment = 0u; moment < RADIANCE_CACHE_D6_MOMENTS; ++moment)
            {
                moments[moment] = asfloat(entries.Load3(base + moment * 12u));
            }

            ByteAddressBuffer basis = ResourceDescriptorHeap[radianceCache.basisIndex];
            float3 cached = RadianceCacheD6Reconstruct(basis, surface.geometricNormal, bsdfSample.direction, moments);
            clamped = any(cached < 0.0f);

            // SHARC.5B diagnostics (queryMode 4): unclamped, to measure the clamp's share of the cache's bias. A
            // negative path sample is then rejected by the ray generation (counted).
            if(radianceCache.queryMode != 4u)
            {
                cached = max(cached, 0.0f);
            }
            substitute = all(isfinite(cached));

            if(substitute)
            {
#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC == 1
                g_pathTraceSharcDiagnosticSample = float3(0.0f, 1.0f, 0.0f);
#elif PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC == 2
                g_pathTraceSharcDiagnosticSample = PathTraceSharcDiagnosticKeyColor(key);
#elif PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC == 3
                g_pathTraceSharcDiagnosticSample = max(cached, 0.0f);
#elif PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC == 4
                // Keep packed XYZ and the version/level nibble, but discard normal-bin and quarter-slice bits.
                g_pathTraceSharcDiagnosticSample = PathTraceSharcDiagnosticKeyColor(uint2(key.x, key.y & 0xF03FFFFFu));
#endif
                missed = PathTraceVisible(position, surface.geometricNormal, bsdfSample.direction,
                    PATH_TRACE_DISTANT_LIGHT_DISTANCE, rayCount);
                // SHARC.5B diagnostics (queryMode 2, 3): the cached part zeroed, and (3) the miss term too, so a
                // terminated path's terms can be compared one by one with the depth-2 reference.
                bool cachedTerm = radianceCache.queryMode == 1u || radianceCache.queryMode == 4u;
                float3 incident = cachedTerm ? cached : float3(0.0f, 0.0f, 0.0f);

                if(missed && radianceCache.queryMode != 3u)
                {
                    incident += PathTraceMissRadiance(bsdfSample.direction, false, bsdfSample.pdf);
                }

                radiance += throughput * incident;

                if(extract)
                {
                    extraction.diffuse += extraction.diffuseThroughput * incident;
                }
            }
        }
    }

    if(radianceCache.queryCounters != 0u)
    {
        // SHARC.5E: an absent key's bucket, full or with a free slot (statistics only; the slots are read again).
        bool absent = trusted && slot == RADIANCE_CACHE_D6_NO_SLOT;
        bool bucketFull = false;

        if(absent)
        {
            RWByteAddressBuffer tags = ResourceDescriptorHeap[radianceCache.tagsIndex];
            uint firstSlot = RadianceCacheD6Bucket(key, radianceCache.bucketCount) * RADIANCE_CACHE_D6_BUCKET_SLOTS;
            bucketFull = true;

            for(uint index = 0u; index < RADIANCE_CACHE_D6_BUCKET_SLOTS; ++index)
            {
                bucketFull = bucketFull && tags.Load((firstSlot + index) * 4u) != 0u;
            }
        }

        PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_ELIGIBLE, eligible);
#if PATH_TRACE_SHARC_EXPERIMENT_GATE
        PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_HEADER_REJECTED, admitted && !trusted);
#else
        PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_HEADER_REJECTED, eligible && !trusted);
#endif
        PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_ABSENT_FULL, absent && bucketFull);
        PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_ABSENT_FREE, absent && !bucketFull);
        PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_FOUND, published);
        PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_COUNT_64, published && sampleCount >= 64.0f);
        PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_COUNT_256, published && sampleCount >= 256.0f);
        PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_TERMINATED, substitute);
        PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_CLAMPED, substitute && clamped);
        PathTraceRadianceCacheCountQuery(RADIANCE_CACHE_D6_QUERY_MISSED, substitute && missed);
    }

    return substitute;
}

// PathTraceReference.hlsl's Continue hook statement sits directly in the path loop; in this library it expands to the
// substitution, which reads the loop's own locals (primary, position, surface, throughput, radiance, rayCount,
// extract, extraction), and a real break when it completed the path.
#if PATH_TRACE_SHARC_EXPERIMENT_GATE
// Experiments B and C also read the loop's ray, still x0's continuation at vertex 1 (its origin is x0's offset
// position), and C at vertex 0 records x0's sampled strategy (specularStrategy) and event; no random number is drawn.
// D reads PathTraceTracePath's rasterFirstVertex, x0 of every raster-primary path (the only paths that substitute).
#if PATH_TRACE_SHARC_EXPERIMENT_C_CONE_SPREAD != 0
#define PATH_TRACE_SHARC_EXPERIMENT_C_RECORD(harvest, vertex, bsdfSample) \
    if(vertex == 0u) \
    { \
        harvest.x0ConeAlpha = !specularStrategy ? -1.0f : bsdfSample.delta ? 0.0f : \
            PathTraceGGXAlpha(surface.material.roughness); \
    }
#define PATH_TRACE_SHARC_EXPERIMENT_C_ALPHA(harvest) harvest.x0ConeAlpha
#else
#define PATH_TRACE_SHARC_EXPERIMENT_C_RECORD(harvest, vertex, bsdfSample)
#define PATH_TRACE_SHARC_EXPERIMENT_C_ALPHA(harvest) -1.0f
#endif

#if PATH_TRACE_SHARC_EXPERIMENT_BC
#define PATH_TRACE_SHARC_EXPERIMENT_BC_ARGUMENTS(harvest) \
    , length(position - ray.Origin), PATH_TRACE_SHARC_EXPERIMENT_C_ALPHA(harvest)
#else
#define PATH_TRACE_SHARC_EXPERIMENT_BC_ARGUMENTS(harvest)
#endif

#if PATH_TRACE_SHARC_EXPERIMENT_D
#define PATH_TRACE_SHARC_EXPERIMENT_D_ARGUMENTS , rasterFirstVertex.surface.material.roughness
#else
#define PATH_TRACE_SHARC_EXPERIMENT_D_ARGUMENTS
#endif

#define PathTraceRadianceCacheHarvestContinue(harvest, vertex, bsdfSample) \
    PATH_TRACE_SHARC_EXPERIMENT_C_RECORD(harvest, vertex, bsdfSample) \
    if(vertex == 1u && PathTraceRadianceCacheSubstitute(primary, position, surface, bsdfSample, throughput, radiance, \
        rayCount, extract, extraction PATH_TRACE_SHARC_EXPERIMENT_BC_ARGUMENTS(harvest) \
        PATH_TRACE_SHARC_EXPERIMENT_D_ARGUMENTS)) \
        break
#else
#define PathTraceRadianceCacheHarvestContinue(harvest, vertex, bsdfSample) \
    if(vertex == 1u && PathTraceRadianceCacheSubstitute(primary, position, surface, bsdfSample, throughput, radiance, \
        rayCount, extract, extraction)) break
#endif
#else
void PathTraceRadianceCacheHarvestContinue(inout PathTraceRadianceCacheHarvest harvest, uint vertex,
    PathTraceBSDFSample bsdfSample)
{
}
#endif

void PathTraceRadianceCacheHarvestSurvive(inout PathTraceRadianceCacheHarvest harvest, uint vertex, float survival)
{
}

#ifdef PATH_TRACE_RADIANCE_CACHE_RENDER
// The end-of-path hook statement sits in PathTraceTracePath; it reads that function's primary, rasterFirstVertex and
// extract (SHARC.6A: the history prior of extraction frames).
#define PathTraceRadianceCacheHarvestEnd(harvest) \
    if(extract) \
    { \
        PathTraceHistoryPriorEndPath(primary, rasterFirstVertex); \
    }
#else
void PathTraceRadianceCacheHarvestEnd(PathTraceRadianceCacheHarvest harvest)
{
}
#endif
