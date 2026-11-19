// NVIDIA SHARC v1.8.3 update (training) ray generation, included after PathTraceReference.hlsl by the SharcNv/ training
// libraries. One thread per trainingTile x trainingTile pixel tile traces that tile's chosen pixel from its decoded
// G-buffer vertex through the production path loop, whose PathTraceSharcNv.hlsli hooks deposit into the cache. Nothing
// is written to the image, the accumulation or the path statistics.

[shader("raygeneration")]
void PathTraceSharcNvTrainingRayGeneration()
{
    uint2 pixel;

    if(!PathTraceSharcNvTrainingPixel(DispatchRaysIndex().x, pixel))
    {
        return;
    }

    PathTraceRasterFirstVertex rasterFirstVertex = PathTraceDecodeRasterFirstVertex(pixel);
#ifdef PATH_TRACE_PSR_GBUFFER
    // PSR.2: a replaced pixel's G-buffer holds the virtual surface; training traces from the physical mirror, as the
    // image does (its mirror vertex is delta and is never deposited).
    PathTraceResolveEffectivePrimary(pixel, rasterFirstVertex);
#endif
    uint rayCount = 0u;

    for(uint sampleOffset = 0u; sampleOffset < samplesPerFrame; ++sampleOffset)
    {
        // A random stream of its own (the top bit of the sample index), so a trained pixel's path is independent of the
        // path the image traces for the same pixel and sample.
        PathTraceRandom random = PathTraceCreateRandom(pixel, (accumulatedSamples + sampleOffset) | 0x80000000u);
        PathTraceExtraction extraction = PathTraceEmptyExtraction();
        PathTracePrimaryRadiance(PATH_TRACE_PRIMARY_RASTER, pixel, rasterFirstVertex, random, rayCount, false,
            extraction);
    }

    SharcNvCount(sharcNv, SHARC_NV_STATISTIC_TRAINING_PATHS, true);
}
