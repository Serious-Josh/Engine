// NVIDIA SHARC v1.8.3 hooks of PathTraceReference.hlsl's path loop (Documentation/Rendering/NVIDIA_SHARC_Integration_A.md).
// Included only by the SharcNv/ libraries: PATH_TRACE_SHARC_NV_TRAINING (the update pass, SHARC_UPDATE 1) or
// PATH_TRACE_SHARC_NV_RENDER (the image pass that queries the cache). Every other library compiles none of this.
//
// Transport ownership. NVIDIA SHARC caches a hit vertex's outgoing radiance toward the previous vertex: everything the
// path adds from that vertex on (its next-event estimation, the MIS-weighted misses of its continuations and all deeper
// vertices). Engine's path tracer has no emissive surfaces, so a vertex's own emission is zero.
//   Render: a raster-primary path that reaches vertex queryVertex (x2, the hit of x1's continuation) with an admitted,
//   cached entry adds throughput * L_o(x2) and ends before x2's next-event estimation. Everything before x2 (x0's
//   ReSTIR DI or exhaustive direct light and HDRI MIS, x1's next-event estimation, the miss of x0's and x1's
//   continuations) is traced and estimated as without the cache, so nothing the cache returns is counted twice and
//   nothing before x2 is lost. A refused or missing entry continues the ordinary path.
//   Update: each training path deposits, at every vertex from depositFirstVertex to depositLastVertex, that vertex's local
//   next-event estimate (after the loop's indirect clamp); NVIDIA's propagation adds later deposits and misses, weighted
//   by the per-segment BSDF weights, to the earlier stored vertices. Pure-delta vertices are not inserted (their outgoing
//   radiance is view-dependent and the render pass never queries them); their delta weight still propagates.

#ifndef PATH_TRACE_SHARC_NV_HLSLI
#define PATH_TRACE_SHARC_NV_HLSLI

#if defined(PATH_TRACE_SHARC_NV_TRAINING) == defined(PATH_TRACE_SHARC_NV_RENDER)
#error "An NVIDIA SHARC library is either the update (training) or the render library."
#endif

#include "SharcNvCommon.hlsli"

cbuffer SharcNvLibraryConstants : register(b3)
{
    SharcNvConstants sharcNv;
};

struct PathTraceSharcNvPath
{
#ifdef PATH_TRACE_SHARC_NV_TRAINING
    SharcState state;
    // The effective primary surface's vertex (non-zero for a PSR-replaced pixel): the deposit range counts from it.
    uint vertexOffset;
#else
    // The path's cache vertex: queryVertex vertices after its effective primary surface (vertex 0, or the replaced surface
    // of a PSR pixel), so the effective primary's and its first bounce's direct light always stay live.
    uint queryVertex;
    // The GGX alpha of the specular sample leaving the vertex before the query vertex: negative for a diffuse sample,
    // 0 for a delta one.
    float queryConeAlpha;
#endif
};

PathTraceSharcNvPath PathTraceSharcNvBegin(PathTraceRasterFirstVertex rasterFirstVertex)
{
    PathTraceSharcNvPath path;
#ifdef PATH_TRACE_SHARC_NV_TRAINING
    SharcInit(path.state);
    path.vertexOffset = 0u;
#ifdef PATH_TRACE_PSR_LIBRARY
    path.vertexOffset = PathTraceEffectivePrimaryVertex(rasterFirstVertex);
#endif
#else
    path.queryConeAlpha = -1.0f;
    path.queryVertex = sharcNv.queryVertex;
#ifdef PATH_TRACE_PSR_LIBRARY
    path.queryVertex += PathTraceEffectivePrimaryVertex(rasterFirstVertex);
#endif
#endif
    return path;
}

#ifdef PATH_TRACE_SHARC_NV_TRAINING
// The update library's path depth replaces maxPathDepth in the loop.
#define PATH_TRACE_SHARC_NV_MAX_DEPTH sharcNv.trainingMaxDepth

// The miss of the ray leaving the latest vertex: its (MIS-weighted, clamped) radiance propagates to the stored vertices.
void PathTraceSharcNvMiss(PathTraceSharcNvPath path, float3 missRadiance)
{
    SharcParameters parameters = SharcNvParameters(sharcNv);
    SharcUpdateMiss(parameters, path.state, missRadiance);
    SharcNvCount(sharcNv, SHARC_NV_STATISTIC_MISSES, path.state.pathLength > 0u);
}

// After vertex's next-event estimate `direct` (local, not multiplied by the path throughput). False when the path must
// end: NVIDIA's cache resampling replaced the rest of it with the cached value, or the hash grid had no free slot.
bool PathTraceSharcNvDeposit(inout PathTraceSharcNvPath path, uint vertex, float3 position, PathTraceSurface surface,
    float3 direct, inout PathTraceRandom random)
{
    // Vertices before the effective primary surface (a PSR pixel's mirror) are never deposited.
    uint relativeVertex = vertex >= path.vertexOffset ? vertex - path.vertexOffset : 0u;
    bool deposit = relativeVertex >= sharcNv.depositFirstVertex && relativeVertex <= sharcNv.depositLastVertex;
    float limit = sharcNv.depositLuminanceLimit;
    float luminance = PathTraceLuminance(direct);
    bool clamped = deposit && limit > 0.0f && luminance > limit;

    if(clamped)
    {
        direct *= limit / luminance;
    }

    bool continuePath = true;

    if(deposit)
    {
        SharcHitData hit;
        hit.positionWorld = position;
        hit.normalWorld = surface.geometricNormal;
        SharcParameters parameters = SharcNvParameters(sharcNv);
        continuePath = SharcUpdateHit(parameters, path.state, hit, direct, PathTraceNextFloat(random));
    }
    else if(relativeVertex > sharcNv.depositLastVertex)
    {
        // The depth-matched diagnostic stores only its query vertex; a later vertex's direct light still belongs to that
        // vertex's outgoing radiance, so it propagates to the stored vertices without an entry of its own (NVIDIA's miss
        // propagation, which RTXPT uses for the same purpose).
        SharcParameters parameters = SharcNvParameters(sharcNv);
        SharcUpdateMiss(parameters, path.state, direct);
    }

    SharcNvCount(sharcNv, SHARC_NV_STATISTIC_DEPOSITS, deposit);
    SharcNvCount(sharcNv, SHARC_NV_STATISTIC_DEPOSIT_STOPS, deposit && !continuePath);
    SharcNvCount(sharcNv, SHARC_NV_STATISTIC_CLAMPED_DEPOSITS, clamped);
    SharcNvMaximum(sharcNv, SHARC_NV_STATISTIC_MAXIMUM_DEPOSIT, deposit,
        max(direct.r, max(direct.g, direct.b)) * sharcNv.radianceScale);
    return continuePath;
}

// After vertex's BSDF sample: the segment's weight scales every stored vertex's propagation weight.
void PathTraceSharcNvSegment(inout PathTraceSharcNvPath path, uint vertex, PathTraceBSDFSample bsdfSample,
    bool specularStrategy, PathTraceSurface surface)
{
    SharcSetThroughput(path.state, bsdfSample.weight);
}

// The update pass traces from the decoded G-buffer vertex like the raster primary, but x0 is never deposited, so its
// next-event estimation is skipped.
#define PATH_TRACE_SHARC_NV_SHADES(vertex) ((vertex) >= 1u)

// One training path per trainingTile x trainingTile pixel tile, at a pixel of the tile chosen per frame.
bool PathTraceSharcNvTrainingPixel(uint thread, out uint2 pixel)
{
    pixel = uint2(0u, 0u);
    uint tile = max(sharcNv.trainingTile, 1u);
    uint2 tileIndex = uint2(thread % max(sharcNv.trainingTilesX, 1u), thread / max(sharcNv.trainingTilesX, 1u));

    if(tileIndex.y >= sharcNv.trainingTilesY)
    {
        return false;
    }

    uint hash = PathTraceHash(thread ^ PathTraceHash(sharcNv.frameIndex ^ 0x53484152u));
    pixel = tileIndex * tile + uint2(hash % tile, (hash / tile) % tile);
    return all(pixel < resolution);
}
#else
#define PATH_TRACE_SHARC_NV_MAX_DEPTH maxPathDepth
#define PATH_TRACE_SHARC_NV_SHADES(vertex) true

void PathTraceSharcNvMiss(PathTraceSharcNvPath path, float3 missRadiance)
{
}

void PathTraceSharcNvSegment(inout PathTraceSharcNvPath path, uint vertex, PathTraceBSDFSample bsdfSample,
    bool specularStrategy, PathTraceSurface surface)
{
    if(vertex + 1u != path.queryVertex)
    {
        return;
    }

    if(bsdfSample.delta)
    {
        path.queryConeAlpha = 0.0f;
    }
    else if(specularStrategy)
    {
        path.queryConeAlpha = PathTraceGGXAlpha(surface.material.roughness);
    }
    else
    {
        path.queryConeAlpha = -1.0f;
    }
}

// At vertex's surface, before its next-event estimation. True when the cache completed the path: radiance (and the
// extraction's diffuse signal) received throughput * L_o, and the caller ends the path.
bool PathTraceSharcNvQuery(PathTraceSharcNvPath path, uint primary, uint vertex, float3 position, float3 previousPosition,
    PathTraceSurface surface, float3 throughput, inout float3 radiance, bool extract, inout PathTraceExtraction extraction)
{
    bool rasterPrimary = primary == PATH_TRACE_PRIMARY_RASTER || primary == PATH_TRACE_PRIMARY_RASTER_SPLIT_INDIRECT;
    bool reached = rasterPrimary && vertex == path.queryVertex;
    bool admitted = reached && !surface.deltaSpecular;

    if(admitted)
    {
        // NVIDIA's integration rules (docs/Integration.md): the segment must span the voxel, and a glossy lobe's cone
        // must have spread to the voxel size; a delta continuation sees the voxel sharply and never qualifies.
        HashGridParameters grid = SharcNvGridParameters(sharcNv);
        float voxelSize = HashGridGetVoxelSize(HashGridGetLevel(position, grid), grid);
        float segment = length(position - previousPosition);
        admitted = segment > voxelSize * 1.7320508f;

        if(admitted && path.queryConeAlpha >= 0.0f)
        {
            float alphaSquared = path.queryConeAlpha * path.queryConeAlpha;
            float spread = alphaSquared < 1.0f ? 2.0f * segment * sqrt(0.5f * alphaSquared / (1.0f - alphaSquared)) : 1.0e30f;
            admitted = spread > voxelSize;
        }
    }

    float3 cached = float3(0.0f, 0.0f, 0.0f);
    bool hit = false;

    if(admitted)
    {
        SharcHitData hitData;
        hitData.positionWorld = position;
        hitData.normalWorld = surface.geometricNormal;
        SharcParameters parameters = SharcNvParameters(sharcNv);
        hit = SharcGetCachedRadiance(parameters, hitData, cached, false) && all(isfinite(cached));
    }

    if(hit)
    {
        radiance += throughput * cached;

        if(extract)
        {
            extraction.diffuse += extraction.diffuseThroughput * cached;
        }
    }

    SharcNvCount(sharcNv, SHARC_NV_STATISTIC_QUERY_REACHED, reached);
    SharcNvCount(sharcNv, SHARC_NV_STATISTIC_QUERY_ADMITTED, admitted);
    SharcNvCount(sharcNv, SHARC_NV_STATISTIC_QUERY_HIT, hit);
    return hit;
}
#endif

#endif
