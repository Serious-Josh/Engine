// SHARC.4D-R sparse training (diagnostics): a training-only ray generation, included after PathTraceReference.hlsl by
// the training libraries (PathTraceReferenceRadianceCacheTraining*.hlsl, which are the deferred learning library with
// PATH_TRACE_RADIANCE_CACHE_TRAINING). One thread per trainingTile x trainingTile pixel tile traces its tile's
// selected pixel (RadianceCacheD6TrainingPixel) exactly as the raster-primary ray generation does: the same G-buffer
// vertex, the same random sequence (pixel, accumulatedSamples + sample) and the same path loop, whose harvest stores
// the training record at the thread's compact index. Nothing is written to the image or the path statistics.

[shader("raygeneration")]
void PathTraceRadianceCacheTrainingRayGeneration()
{
    uint2 pixel;

    if(!RadianceCacheD6TrainingPixel(radianceCache, DispatchRaysIndex().x, resolution, pixel))
    {
        return;
    }

    PathTraceRasterFirstVertex rasterFirstVertex = PathTraceDecodeRasterFirstVertex(pixel);
    uint rayCount = 0u;

    for(uint sampleOffset = 0u; sampleOffset < samplesPerFrame; ++sampleOffset)
    {
        PathTraceRandom random = PathTraceCreateRandom(pixel, accumulatedSamples + sampleOffset);
        PathTraceExtraction extraction = PathTraceEmptyExtraction();
        PathTracePrimaryRadiance(PATH_TRACE_PRIMARY_RASTER, pixel, rasterFirstVertex, random, rayCount, false,
            extraction);
    }
}
