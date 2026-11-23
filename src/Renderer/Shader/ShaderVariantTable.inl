// Engine's shader variants: one row per variant library or shader, named by the shader-root-relative path the
// renderer requests. Each compiles as its defines, in order, followed by #include of its base (a source file or another
// variant, expanded the same way) and then of each file listed in `after`. The text is generated in memory
// (ShaderVariants.cpp), so a variant needs no file of its own; add a row here instead of a wrapper .hlsl.
// Generated once from the former wrapper files (tools/tests/shader_variants/generate_variant_table.py).
//
// Columns: path, base, defines ("NAME=VALUE", space separated), after (files, space separated).

// ---- Shader root ----
// NRD RELAX R.9B diagnostics: the confidence filter and mapping (CSFilter). See NrdHistoryConfidence.hlsli.
{"NrdHistoryConfidenceFilter.hlsl", "NrdHistoryConfidence.hlsli", "NRD_CONFIDENCE_FILTER=1", ""},
// NRD RELAX R.9B diagnostics: the paired-visibility probe pass (CSProbe). See NrdHistoryConfidence.hlsli.
{"NrdHistoryConfidenceProbe.hlsl", "NrdHistoryConfidence.hlsli", "NRD_CONFIDENCE_PROBE=1", ""},
// PSR.2 Stage 3B phase 4: the path-traced NRD preparation and resolves with NRD_PATH_TRACE_PSR (PsrDenoise.hlsli),
// selected on frames that light replaced pixels. Development only; production frames keep NrdPathTracePrepare.hlsl.
{"NrdPathTracePreparePsr.hlsl", "NrdPathTracePrepare.hlsl", "NRD_PATH_TRACE_PSR=1", ""},
// RR.1B: the DLSS Ray Reconstruction input preparation with RR_INPUT_PSR (PsrDenoise.hlsli), on frames that light replaced
// pixels. Editor diagnostics only (RR is not a rendering mode before RR.1D); not cooked.
{"RrInputPreparationPsr.hlsl", "RrInputPreparation.hlsl", "RR_INPUT_PSR=1", ""},
// PSR.1a: Raster Primary primary surface replacement (PATH_TRACE_PSR_LIBRARY, PathTraceReference.hlsl), in a DXR
// library of its own so every production library compiles exactly as before. Development only: created when the PSR
// toggle is on; not in the cooked manifests, so packaged runtimes report it unavailable. Compiled as lib_6_6, as
// PathTraceReference.hlsl.
{"PathTraceReferencePsr.hlsl", "PathTraceReference.hlsl", "PATH_TRACE_PSR_LIBRARY=1", ""},
// PSR.2 Stage 3A: the PSR.1c-A reference library (PathTraceReferencePsr.hlsl) with the G-buffer storage
// (PATH_TRACE_PSR_GBUFFER); only its split indirect pass is used. Development only; not cooked.
{"PathTraceReferencePsrGBuffer.hlsl", "PathTraceReferencePsr.hlsl", "PATH_TRACE_PSR_GBUFFER=1", ""},
// PSR.2 Stage 3A: the PSR.1c-A reference library (PathTraceReferencePsrSERSM69.hlsl) with the G-buffer storage
// (PATH_TRACE_PSR_GBUFFER); only its split indirect pass is used. Development only; not cooked.
{"PathTraceReferencePsrGBufferSERSM69.hlsl", "PathTraceReferencePsrSERSM69.hlsl", "PATH_TRACE_PSR_GBUFFER=1", ""},
// PSR.2 Stage 3A: the PSR.1c-A reference library (PathTraceReferencePsrSM69.hlsl) with the G-buffer storage
// (PATH_TRACE_PSR_GBUFFER); only its split indirect pass is used. Development only; not cooked.
{"PathTraceReferencePsrGBufferSM69.hlsl", "PathTraceReferencePsrSM69.hlsl", "PATH_TRACE_PSR_GBUFFER=1", ""},
// PSR.1a: Raster Primary primary surface replacement (PATH_TRACE_PSR_LIBRARY, PathTraceReference.hlsl), in a DXR
// library of its own so every production library compiles exactly as before. Development only. Compiled as lib_6_9 with
// SER, as PathTraceReferenceSERSM69.hlsl.
{"PathTraceReferencePsrSERSM69.hlsl", "PathTraceReference.hlsl", "PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_SER=1", ""},
// PSR.1a: Raster Primary primary surface replacement (PATH_TRACE_PSR_LIBRARY, PathTraceReference.hlsl), in a DXR
// library of its own so every production library compiles exactly as before. Development only. Compiled as lib_6_9, as
// PathTraceReferenceSM69.hlsl (ShaderCompiler::GetLibraryProfile).
{"PathTraceReferencePsrSM69.hlsl", "PathTraceReference.hlsl", "PATH_TRACE_PSR_LIBRARY=1", ""},
// Isolated M2A energy validation; production wrappers do not define these observers.
{"PathTraceReferencePsrSharcEnergyReference.hlsl", "PathTraceReferencePsrSharcTraining.hlsl",
    "PATH_TRACE_PSR_SHARC_ENERGY_DIAGNOSTIC=1 PATH_TRACE_PSR_SHARC_ENERGY_REFERENCE=1", ""},
{"PathTraceReferencePsrSharcEnergyReferenceSERSM69.hlsl", "PathTraceReferencePsrSharcTrainingSERSM69.hlsl",
    "PATH_TRACE_PSR_SHARC_ENERGY_DIAGNOSTIC=1 PATH_TRACE_PSR_SHARC_ENERGY_REFERENCE=1", ""},
{"PathTraceReferencePsrSharcEnergyReferenceSM69.hlsl", "PathTraceReferencePsrSharcTrainingSM69.hlsl",
    "PATH_TRACE_PSR_SHARC_ENERGY_DIAGNOSTIC=1 PATH_TRACE_PSR_SHARC_ENERGY_REFERENCE=1", ""},
{"PathTraceReferencePsrSharcEnergyTraining.hlsl", "PathTraceReferencePsrSharcTraining.hlsl",
    "PATH_TRACE_PSR_SHARC_ENERGY_DIAGNOSTIC=1", ""},
{"PathTraceReferencePsrSharcEnergyTrainingSERSM69.hlsl", "PathTraceReferencePsrSharcTrainingSERSM69.hlsl",
    "PATH_TRACE_PSR_SHARC_ENERGY_DIAGNOSTIC=1", ""},
{"PathTraceReferencePsrSharcEnergyTrainingSM69.hlsl", "PathTraceReferencePsrSharcTrainingSM69.hlsl",
    "PATH_TRACE_PSR_SHARC_ENERGY_DIAGNOSTIC=1", ""},
// PSR.2 + SHARC combined render library: ordinary pixels substitute SHARC; replaced pixels trace physical B.
{"PathTraceReferencePsrSharcRender.hlsl", "PathTraceReferenceRadianceCacheRender.hlsl",
    "PATH_TRACE_PSR_SHARC_INFRASTRUCTURE=1 PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
{"PathTraceReferencePsrSharcRenderRawDiffuseHitDistance.hlsl",
    "PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistance.hlsl",
    "PATH_TRACE_PSR_SHARC_INFRASTRUCTURE=1 PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
{"PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PATH_TRACE_PSR_SHARC_INFRASTRUCTURE=1 PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
{"PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSM69.hlsl",
    "PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistanceSM69.hlsl",
    "PATH_TRACE_PSR_SHARC_INFRASTRUCTURE=1 PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
{"PathTraceReferencePsrSharcRenderSERSM69.hlsl", "PathTraceReferenceRadianceCacheRenderSERSM69.hlsl",
    "PATH_TRACE_PSR_SHARC_INFRASTRUCTURE=1 PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
{"PathTraceReferencePsrSharcRenderSM69.hlsl", "PathTraceReferenceRadianceCacheRenderSM69.hlsl",
    "PATH_TRACE_PSR_SHARC_INFRASTRUCTURE=1 PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// PSR.2 + SHARC combined sparse training: replaced pixels train SHARC at physical B (original vertex 1).
{"PathTraceReferencePsrSharcTraining.hlsl", "PathTraceReferenceRadianceCacheTraining.hlsl",
    "PATH_TRACE_PSR_SHARC_INFRASTRUCTURE=1 PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
{"PathTraceReferencePsrSharcTrainingSERSM69.hlsl", "PathTraceReferenceRadianceCacheTrainingSERSM69.hlsl",
    "PATH_TRACE_PSR_SHARC_INFRASTRUCTURE=1 PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
{"PathTraceReferencePsrSharcTrainingSM69.hlsl", "PathTraceReferenceRadianceCacheTrainingSM69.hlsl",
    "PATH_TRACE_PSR_SHARC_INFRASTRUCTURE=1 PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// SHARC.4D-Q: PathTraceReference.hlsl as the D6 radiance cache's dense learning library (lib_6_6): it renders the
// unchanged raster-primary image, and each path stores its training record (PathTraceRadianceCacheHarvest.hlsli) for
// CSInsertRecords to insert; no insertion code is compiled into the ray generation.
{"PathTraceReferenceRadianceCacheDeferred.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_DEFERRED=1", ""},
// SHARC.4D-Q shader-floor experiment (diagnostics): PathTraceReferenceRadianceCacheDeferred.hlsl with the whole
// training record stored at the end of each path (PATH_TRACE_RADIANCE_CACHE_DEFERRED_LATE), as the 4D-P store probe
// did, instead of x1's part at vertex 1.
{"PathTraceReferenceRadianceCacheDeferredLate.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_DEFERRED=1 "
    "PATH_TRACE_RADIANCE_CACHE_DEFERRED_LATE=1", ""},
// SHARC.4D-Q shader-floor experiment (diagnostics): PathTraceReferenceRadianceCacheDeferredSERSM69.hlsl with the whole
// training record stored at the end of each path (PATH_TRACE_RADIANCE_CACHE_DEFERRED_LATE), as the 4D-P store probe
// did, instead of x1's part at vertex 1.
{"PathTraceReferenceRadianceCacheDeferredLateSERSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_SER=1 PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_DEFERRED=1 "
    "PATH_TRACE_RADIANCE_CACHE_DEFERRED_LATE=1", ""},
// SHARC.4D-Q shader-floor experiment (diagnostics): PathTraceReferenceRadianceCacheDeferredSM69.hlsl with the whole
// training record stored at the end of each path (PATH_TRACE_RADIANCE_CACHE_DEFERRED_LATE), as the 4D-P store probe
// did, instead of x1's part at vertex 1.
{"PathTraceReferenceRadianceCacheDeferredLateSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_DEFERRED=1 "
    "PATH_TRACE_RADIANCE_CACHE_DEFERRED_LATE=1", ""},
// SHARC.4D-Q: the deferred learning library of PathTraceReferenceRadianceCacheDeferred.hlsl compiled as lib_6_9 with
// shader execution reordering (ShaderCompiler::GetLibraryProfile), the counterpart of PathTraceReferenceSERSM69.hlsl.
{"PathTraceReferenceRadianceCacheDeferredSERSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_SER=1 PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_DEFERRED=1", ""},
// SHARC.4D-Q: the deferred learning library of PathTraceReferenceRadianceCacheDeferred.hlsl compiled as lib_6_9
// (ShaderCompiler::GetLibraryProfile), the counterpart of PathTraceReferenceSM69.hlsl.
{"PathTraceReferenceRadianceCacheDeferredSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_DEFERRED=1", ""},
// SHARC.4D-R shader-floor experiment (diagnostics): PathTraceReference.hlsl with the D6 cache's query-only hooks
// (PathTraceRadianceCacheQuery.hlsli), compiled as lib_6_6 (ShaderCompiler::GetLibraryProfile).
{"PathTraceReferenceRadianceCacheQuery.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_QUERY=1", ""},
// SHARC.4D-R shader-floor experiment (diagnostics): PathTraceReference.hlsl with the D6 cache's query-only hooks
// (PathTraceRadianceCacheQuery.hlsli), compiled as lib_6_9 with shader execution reordering
// (ShaderCompiler::GetLibraryProfile).
{"PathTraceReferenceRadianceCacheQuerySERSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_SER=1 PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_QUERY=1", ""},
// SHARC.4D-R shader-floor experiment (diagnostics): PathTraceReference.hlsl with the D6 cache's query-only hooks
// (PathTraceRadianceCacheQuery.hlsli), compiled as lib_6_9 (ShaderCompiler::GetLibraryProfile).
{"PathTraceReferenceRadianceCacheQuerySM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_QUERY=1", ""},
// SHARC.4D-S termination probe (diagnostics): the query library (PathTraceReferenceRadianceCacheQuery.hlsl) with
// vertex-1 termination (PATH_TRACE_RADIANCE_CACHE_TERMINATION, PathTraceRadianceCacheQuery.hlsli), compiled as lib_6_6
// (ShaderCompiler::GetLibraryProfile). Its images are wrong by construction; timing only.
{"PathTraceReferenceRadianceCacheQueryTermination.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_QUERY=1 "
    "PATH_TRACE_RADIANCE_CACHE_TERMINATION=1", ""},
// SHARC.4D-S termination probe (diagnostics): the query library (PathTraceReferenceRadianceCacheQuerySERSM69.hlsl) with
// vertex-1 termination (PATH_TRACE_RADIANCE_CACHE_TERMINATION, PathTraceRadianceCacheQuery.hlsli), compiled as lib_6_9
// with shader execution reordering (ShaderCompiler::GetLibraryProfile). Its images are wrong by construction; timing
// only.
{"PathTraceReferenceRadianceCacheQueryTerminationSERSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_SER=1 PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_QUERY=1 "
    "PATH_TRACE_RADIANCE_CACHE_TERMINATION=1", ""},
// SHARC.4D-S termination probe (diagnostics): the query library (PathTraceReferenceRadianceCacheQuerySM69.hlsl) with
// vertex-1 termination (PATH_TRACE_RADIANCE_CACHE_TERMINATION, PathTraceRadianceCacheQuery.hlsli), compiled as lib_6_9
// (ShaderCompiler::GetLibraryProfile). Its images are wrong by construction; timing only.
{"PathTraceReferenceRadianceCacheQueryTerminationSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_QUERY=1 "
    "PATH_TRACE_RADIANCE_CACHE_TERMINATION=1", ""},
// SHARC.4E (experimental, default off): PathTraceReference.hlsl rendering with cached D6 radiance at path vertex 1
// (PATH_TRACE_RADIANCE_CACHE_RENDER, PathTraceRadianceCacheQuery.hlsli), compiled as lib_6_6
// (ShaderCompiler::GetLibraryProfile).
{"PathTraceReferenceRadianceCacheRender.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_QUERY=1 PATH_TRACE_RADIANCE_CACHE_RENDER=1", ""},
// NRD.RELAX.2B: the extraction exports with the raw primary diffuse hit distance
// (PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY, PathTraceReference.hlsl), in a DXR library of their own so the
// production and REBLUR extraction libraries compile exactly as before. Diagnostics only: created when RELAX inputs are
// requested. SHARC render library, as PathTraceReferenceRadianceCacheRender.hlsl (lib_6_6).
{"PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistance.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 "
    "PATH_TRACE_RADIANCE_CACHE_QUERY=1 PATH_TRACE_RADIANCE_CACHE_RENDER=1", ""},
// NRD.RELAX.2B: the extraction exports with the raw primary diffuse hit distance
// (PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY, PathTraceReference.hlsl), in a DXR library of their own so the
// production and REBLUR extraction libraries compile exactly as before. Diagnostics only: created when RELAX inputs are
// requested. SHARC render library, as PathTraceReferenceRadianceCacheRenderSERSM69.hlsl (lib_6_9, SER).
{"PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistanceSERSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY=1 PATH_TRACE_SER=1 PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 "
    "PATH_TRACE_RADIANCE_CACHE_QUERY=1 PATH_TRACE_RADIANCE_CACHE_RENDER=1", ""},
// NRD.RELAX.2B: the extraction exports with the raw primary diffuse hit distance
// (PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY, PathTraceReference.hlsl), in a DXR library of their own so the
// production and REBLUR extraction libraries compile exactly as before. Diagnostics only: created when RELAX inputs are
// requested. SHARC render library, as PathTraceReferenceRadianceCacheRenderSM69.hlsl (lib_6_9).
{"PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistanceSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 "
    "PATH_TRACE_RADIANCE_CACHE_QUERY=1 PATH_TRACE_RADIANCE_CACHE_RENDER=1", ""},
// SHARC.4E (experimental, default off): PathTraceReference.hlsl rendering with cached D6 radiance at path vertex 1
// (PATH_TRACE_RADIANCE_CACHE_RENDER, PathTraceRadianceCacheQuery.hlsli), compiled as lib_6_9 with shader execution
// reordering (ShaderCompiler::GetLibraryProfile).
{"PathTraceReferenceRadianceCacheRenderSERSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_SER=1 PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_QUERY=1 "
    "PATH_TRACE_RADIANCE_CACHE_RENDER=1", ""},
// SHARC.4E (experimental, default off): PathTraceReference.hlsl rendering with cached D6 radiance at path vertex 1
// (PATH_TRACE_RADIANCE_CACHE_RENDER, PathTraceRadianceCacheQuery.hlsli), compiled as lib_6_9
// (ShaderCompiler::GetLibraryProfile).
{"PathTraceReferenceRadianceCacheRenderSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_QUERY=1 PATH_TRACE_RADIANCE_CACHE_RENDER=1", ""},
// SHARC.4D-R sparse training (diagnostics): the deferred learning library with the training-only ray generation
// (PathTraceRadianceCacheTraining.hlsli), compiled as lib_6_6 (ShaderCompiler::GetLibraryProfile).
{"PathTraceReferenceRadianceCacheTraining.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_DEFERRED=1 "
    "PATH_TRACE_RADIANCE_CACHE_TRAINING=1", "PathTraceRadianceCacheTraining.hlsli"},
// SHARC.4D-R sparse training (diagnostics): the deferred learning library with the training-only ray generation
// (PathTraceRadianceCacheTraining.hlsli), compiled as lib_6_9 with shader execution reordering
// (ShaderCompiler::GetLibraryProfile).
{"PathTraceReferenceRadianceCacheTrainingSERSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_SER=1 PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_DEFERRED=1 "
    "PATH_TRACE_RADIANCE_CACHE_TRAINING=1", "PathTraceRadianceCacheTraining.hlsli"},
// SHARC.4D-R sparse training (diagnostics): the deferred learning library with the training-only ray generation
// (PathTraceRadianceCacheTraining.hlsli), compiled as lib_6_9 (ShaderCompiler::GetLibraryProfile).
{"PathTraceReferenceRadianceCacheTrainingSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_RADIANCE_CACHE_LIBRARY=1 PATH_TRACE_RADIANCE_CACHE_DEFERRED=1 "
    "PATH_TRACE_RADIANCE_CACHE_TRAINING=1", "PathTraceRadianceCacheTraining.hlsli"},
// NRD.RELAX.2B: the extraction exports with the raw primary diffuse hit distance
// (PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY, PathTraceReference.hlsl), in a DXR library of their own so the
// production and REBLUR extraction libraries compile exactly as before. Diagnostics only: created when RELAX inputs are
// requested. Compiled as lib_6_6, as PathTraceReference.hlsl.
{"PathTraceReferenceRawDiffuseHitDistance.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY=1", ""},
// PSR.2 Stage 3B: the raw diffuse hit-distance extraction library (PathTraceReferenceRawDiffuseHitDistance.hlsl) as a
// PSR library with the G-buffer storage (PATH_TRACE_PSR_GBUFFER); only its split indirect extraction is used.
// Development only.
{"PathTraceReferenceRawDiffuseHitDistancePsrGBuffer.hlsl", "PathTraceReferenceRawDiffuseHitDistance.hlsl",
    "PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// PSR.2 Stage 3B: the raw diffuse hit-distance extraction library (PathTraceReferenceRawDiffuseHitDistanceSERSM69.hlsl)
// as a PSR library with the G-buffer storage (PATH_TRACE_PSR_GBUFFER); only its split indirect extraction is used.
// Development only.
{"PathTraceReferenceRawDiffuseHitDistancePsrGBufferSERSM69.hlsl", "PathTraceReferenceRawDiffuseHitDistanceSERSM69.hlsl",
    "PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// PSR.2 Stage 3B: the raw diffuse hit-distance extraction library (PathTraceReferenceRawDiffuseHitDistanceSM69.hlsl) as
// a PSR library with the G-buffer storage (PATH_TRACE_PSR_GBUFFER); only its split indirect extraction is used.
// Development only.
{"PathTraceReferenceRawDiffuseHitDistancePsrGBufferSM69.hlsl", "PathTraceReferenceRawDiffuseHitDistanceSM69.hlsl",
    "PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// NRD.RELAX.2B: the extraction exports with the raw primary diffuse hit distance
// (PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY, PathTraceReference.hlsl), in a DXR library of their own so the
// production and REBLUR extraction libraries compile exactly as before. Diagnostics only: created when RELAX inputs are
// requested. Compiled as lib_6_9 with SER, as PathTraceReferenceSERSM69.hlsl.
{"PathTraceReferenceRawDiffuseHitDistanceSERSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY=1 PATH_TRACE_SER=1", ""},
// NRD.RELAX.2B: the extraction exports with the raw primary diffuse hit distance
// (PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY, PathTraceReference.hlsl), in a DXR library of their own so the
// production and REBLUR extraction libraries compile exactly as before. Diagnostics only: created when RELAX inputs are
// requested. Compiled as lib_6_9, as PathTraceReferenceSM69.hlsl (ShaderCompiler::GetLibraryProfile).
{"PathTraceReferenceRawDiffuseHitDistanceSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY=1", ""},
// SER S3A: PathTraceReference.hlsl compiled as lib_6_9 with standardized DirectX shader execution reordering on the
// Reference primary's radiance continuation rays (ShaderCompiler::GetLibraryProfile). Experimental: no renderer path
// selects this library yet; PathTraceReferenceSM69.hlsl is its non-SER comparison baseline.
{"PathTraceReferenceSERSM69.hlsl", "PathTraceReference.hlsl", "PATH_TRACE_SER=1", ""},
// SER S1: PathTraceReference.hlsl compiled as lib_6_9 (ShaderCompiler::GetLibraryProfile). Experimental: no SER
// instructions, and no renderer path selects this library yet.
{"PathTraceReferenceSM69.hlsl", "PathTraceReference.hlsl", "", ""},
// PSR.2 Stage 3B: the a-trous spatial pass with PATH_TRACE_PSR_DENOISE (PsrDenoise.hlsli), selected on frames that
// light replaced pixels. Development only; production frames keep PathTraceSpatialAtrous.hlsl.
{"PathTraceSpatialAtrousPsr.hlsl", "PathTraceSpatialAtrous.hlsl", "PATH_TRACE_PSR_DENOISE=1", ""},
// PSR.2 Stage 3B: the temporal pass with PATH_TRACE_PSR_DENOISE (PsrDenoise.hlsli), selected on frames that light
// replaced pixels. Development only; production frames keep PathTraceTemporal.hlsl.
{"PathTraceTemporalPsr.hlsl", "PathTraceTemporal.hlsl", "PATH_TRACE_PSR_DENOISE=1", ""},
// Multibounce PSR: the in-place replacement pass with two-mirror chains (PSR_MAX_CHAIN 2), selected on frames whose PSR
// mode is two-bounce. Single-bounce frames keep PsrInPlaceReplace.hlsl, whose compiled code this variant leaves
// unchanged.
{"PsrInPlaceReplaceK2.hlsl", "PsrInPlaceReplace.hlsl", "PSR_MAX_CHAIN=2", ""},

// ---- DirectLighting ----
// Production ReSTIR DI direct pass as a compute shader (cs_6_6, entry PathTraceReSTIRDIDirectCS; ShaderCompiler maps
// DirectLighting/ to it), compiled from PathTraceReference.hlsl: PATH_TRACE_RESTIR_DI_LIBRARY selects the ReSTIR DI
// direct pass, PATH_TRACE_DIRECT_COMPUTE its compute entry, and its shadow rays are inline RayQuery
// (PATH_TRACE_PRIMARY_DI_SHADOW_RAY_QUERY). PSR off.
{"DirectLighting/ReSTIRDIDirect.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_DIRECT_COMPUTE=1 PATH_TRACE_PRIMARY_DI_SHADOW_RAY_QUERY=1 PATH_TRACE_RESTIR_DI_LIBRARY=1", ""},
// Production ReSTIR DI direct pass as a compute shader (cs_6_6, entry PathTraceReSTIRDIDirectCS; ShaderCompiler maps
// DirectLighting/ to it), compiled from PathTraceReference.hlsl: PATH_TRACE_RESTIR_DI_LIBRARY selects the ReSTIR DI
// direct pass, PATH_TRACE_DIRECT_COMPUTE its compute entry, and its shadow rays are inline RayQuery
// (PATH_TRACE_PRIMARY_DI_SHADOW_RAY_QUERY). PSR off, with NRD signal extraction.
{"DirectLighting/ReSTIRDIDirectExtract.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_DIRECT_COMPUTE=1 PATH_TRACE_PRIMARY_DI_SHADOW_RAY_QUERY=1 PATH_TRACE_RESTIR_DI_LIBRARY=1 "
    "PATH_TRACE_RESTIR_DI_EXTRACTION_LIBRARY=1", ""},
// Production ReSTIR DI direct pass as a compute shader (cs_6_6, entry PathTraceReSTIRDIDirectCS; ShaderCompiler maps
// DirectLighting/ to it), compiled from PathTraceReference.hlsl: PATH_TRACE_RESTIR_DI_LIBRARY selects the ReSTIR DI
// direct pass, PATH_TRACE_DIRECT_COMPUTE its compute entry, and its shadow rays are inline RayQuery
// (PATH_TRACE_PRIMARY_DI_SHADOW_RAY_QUERY). Single-bounce PSR (PSR.2 G-buffer storage).
{"DirectLighting/ReSTIRDIDirectPsr.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_DIRECT_COMPUTE=1 PATH_TRACE_PRIMARY_DI_SHADOW_RAY_QUERY=1 PATH_TRACE_RESTIR_DI_LIBRARY=1 "
    "PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// Production ReSTIR DI direct pass as a compute shader (cs_6_6, entry PathTraceReSTIRDIDirectCS; ShaderCompiler maps
// DirectLighting/ to it), compiled from PathTraceReference.hlsl: PATH_TRACE_RESTIR_DI_LIBRARY selects the ReSTIR DI
// direct pass, PATH_TRACE_DIRECT_COMPUTE its compute entry, and its shadow rays are inline RayQuery
// (PATH_TRACE_PRIMARY_DI_SHADOW_RAY_QUERY). Single-bounce PSR (PSR.2 G-buffer storage), with NRD signal extraction.
{"DirectLighting/ReSTIRDIDirectPsrExtract.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_DIRECT_COMPUTE=1 PATH_TRACE_PRIMARY_DI_SHADOW_RAY_QUERY=1 PATH_TRACE_RESTIR_DI_LIBRARY=1 "
    "PATH_TRACE_RESTIR_DI_EXTRACTION_LIBRARY=1 PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// Production ReSTIR DI direct pass as a compute shader (cs_6_6, entry PathTraceReSTIRDIDirectCS; ShaderCompiler maps
// DirectLighting/ to it), compiled from PathTraceReference.hlsl: PATH_TRACE_RESTIR_DI_LIBRARY selects the ReSTIR DI
// direct pass, PATH_TRACE_DIRECT_COMPUTE its compute entry, and its shadow rays are inline RayQuery
// (PATH_TRACE_PRIMARY_DI_SHADOW_RAY_QUERY). Two-bounce PSR (PSR.2 G-buffer storage, K = 2).
{"DirectLighting/ReSTIRDIDirectPsrK2.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_DIRECT_COMPUTE=1 PATH_TRACE_PRIMARY_DI_SHADOW_RAY_QUERY=1 PATH_TRACE_RESTIR_DI_LIBRARY=1 "
    "PATH_TRACE_PSR_CHAIN=2 PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// Production ReSTIR DI direct pass as a compute shader (cs_6_6, entry PathTraceReSTIRDIDirectCS; ShaderCompiler maps
// DirectLighting/ to it), compiled from PathTraceReference.hlsl: PATH_TRACE_RESTIR_DI_LIBRARY selects the ReSTIR DI
// direct pass, PATH_TRACE_DIRECT_COMPUTE its compute entry, and its shadow rays are inline RayQuery
// (PATH_TRACE_PRIMARY_DI_SHADOW_RAY_QUERY). Two-bounce PSR (PSR.2 G-buffer storage, K = 2), with NRD signal extraction.
{"DirectLighting/ReSTIRDIDirectPsrK2Extract.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_DIRECT_COMPUTE=1 PATH_TRACE_PRIMARY_DI_SHADOW_RAY_QUERY=1 PATH_TRACE_RESTIR_DI_LIBRARY=1 "
    "PATH_TRACE_PSR_CHAIN=2 PATH_TRACE_RESTIR_DI_EXTRACTION_LIBRARY=1 PATH_TRACE_PSR_LIBRARY=1 "
    "PATH_TRACE_PSR_GBUFFER=1", ""},

// M4.3 ReGIR (experimental, ENGINE_REGIR=1): each ReSTIR DI direct pass with ReGIR local-light candidates
// (PATH_TRACE_REGIR, RtxdiApplicationBridge.hlsli), selected only on frames whose ReGIR slots were presampled for the
// frame's light table; production frames keep the passes above.
{"DirectLighting/ReGIR/ReSTIRDIDirect.hlsl", "DirectLighting/ReSTIRDIDirect.hlsl", "PATH_TRACE_REGIR=1", ""},
{"DirectLighting/ReGIR/ReSTIRDIDirectExtract.hlsl", "DirectLighting/ReSTIRDIDirectExtract.hlsl", "PATH_TRACE_REGIR=1", ""},
{"DirectLighting/ReGIR/ReSTIRDIDirectPsr.hlsl", "DirectLighting/ReSTIRDIDirectPsr.hlsl", "PATH_TRACE_REGIR=1", ""},
{"DirectLighting/ReGIR/ReSTIRDIDirectPsrExtract.hlsl", "DirectLighting/ReSTIRDIDirectPsrExtract.hlsl", "PATH_TRACE_REGIR=1", ""},
{"DirectLighting/ReGIR/ReSTIRDIDirectPsrK2.hlsl", "DirectLighting/ReSTIRDIDirectPsrK2.hlsl", "PATH_TRACE_REGIR=1", ""},
{"DirectLighting/ReGIR/ReSTIRDIDirectPsrK2Extract.hlsl", "DirectLighting/ReSTIRDIDirectPsrK2Extract.hlsl", "PATH_TRACE_REGIR=1", ""},

// ---- GI6 ----
// Diagnostic GI.6 image-path visibility variant; original wrapper preserves profile and SER.
{"GI6/PathTraceReference.hlsl", "PathTraceReference.hlsl", "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// PSR.1a GI.6 image-path visibility variant; original wrapper preserves profile and SER.
{"GI6/PathTraceReferencePsr.hlsl", "PathTraceReferencePsr.hlsl", "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// PSR.2 Stage 3A GI.6 image-path visibility variant; original wrapper preserves profile and SER.
{"GI6/PathTraceReferencePsrGBuffer.hlsl", "PathTraceReferencePsrGBuffer.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferencePsrGBufferSERSM69.hlsl", "PathTraceReferencePsrGBufferSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferencePsrGBufferSM69.hlsl", "PathTraceReferencePsrGBufferSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// PSR.1a GI.6 image-path visibility variant; original wrapper preserves profile and SER.
{"GI6/PathTraceReferencePsrSERSM69.hlsl", "PathTraceReferencePsrSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferencePsrSM69.hlsl", "PathTraceReferencePsrSM69.hlsl", "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// Existing image-path inline visibility capability.
{"GI6/PathTraceReferencePsrSharcRender.hlsl", "PathTraceReferencePsrSharcRender.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferencePsrSharcRenderRawDiffuseHitDistance.hlsl",
    "PathTraceReferencePsrSharcRenderRawDiffuseHitDistance.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSM69.hlsl",
    "PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferencePsrSharcRenderSERSM69.hlsl", "PathTraceReferencePsrSharcRenderSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferencePsrSharcRenderSM69.hlsl", "PathTraceReferencePsrSharcRenderSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// Diagnostic GI.6 image-path visibility variant; original wrapper preserves profile and SER.
{"GI6/PathTraceReferenceRadianceCacheDeferred.hlsl", "PathTraceReferenceRadianceCacheDeferred.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRadianceCacheDeferredLate.hlsl", "PathTraceReferenceRadianceCacheDeferredLate.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRadianceCacheDeferredLateSERSM69.hlsl",
    "PathTraceReferenceRadianceCacheDeferredLateSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRadianceCacheDeferredLateSM69.hlsl", "PathTraceReferenceRadianceCacheDeferredLateSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRadianceCacheDeferredSERSM69.hlsl", "PathTraceReferenceRadianceCacheDeferredSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRadianceCacheDeferredSM69.hlsl", "PathTraceReferenceRadianceCacheDeferredSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRadianceCacheQuery.hlsl", "PathTraceReferenceRadianceCacheQuery.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRadianceCacheQuerySERSM69.hlsl", "PathTraceReferenceRadianceCacheQuerySERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRadianceCacheQuerySM69.hlsl", "PathTraceReferenceRadianceCacheQuerySM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRadianceCacheQueryTermination.hlsl", "PathTraceReferenceRadianceCacheQueryTermination.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRadianceCacheQueryTerminationSERSM69.hlsl",
    "PathTraceReferenceRadianceCacheQueryTerminationSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRadianceCacheQueryTerminationSM69.hlsl",
    "PathTraceReferenceRadianceCacheQueryTerminationSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRadianceCacheRender.hlsl", "PathTraceReferenceRadianceCacheRender.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistance.hlsl",
    "PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistance.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistanceSM69.hlsl",
    "PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistanceSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRadianceCacheRenderSERSM69.hlsl", "PathTraceReferenceRadianceCacheRenderSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRadianceCacheRenderSM69.hlsl", "PathTraceReferenceRadianceCacheRenderSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRawDiffuseHitDistance.hlsl", "PathTraceReferenceRawDiffuseHitDistance.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// PSR.2 Stage 3B GI.6 image-path visibility variant; original wrapper preserves profile and SER.
{"GI6/PathTraceReferenceRawDiffuseHitDistancePsrGBuffer.hlsl", "PathTraceReferenceRawDiffuseHitDistancePsrGBuffer.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRawDiffuseHitDistancePsrGBufferSERSM69.hlsl",
    "PathTraceReferenceRawDiffuseHitDistancePsrGBufferSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRawDiffuseHitDistancePsrGBufferSM69.hlsl",
    "PathTraceReferenceRawDiffuseHitDistancePsrGBufferSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// Diagnostic GI.6 image-path visibility variant; original wrapper preserves profile and SER.
{"GI6/PathTraceReferenceRawDiffuseHitDistanceSERSM69.hlsl", "PathTraceReferenceRawDiffuseHitDistanceSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceRawDiffuseHitDistanceSM69.hlsl", "PathTraceReferenceRawDiffuseHitDistanceSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceSERSM69.hlsl", "PathTraceReferenceSERSM69.hlsl", "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
{"GI6/PathTraceReferenceSM69.hlsl", "PathTraceReferenceSM69.hlsl", "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// Multibounce PSR (K = 2): the GI6 twin of PsrK2/PathTraceReferencePsrGBuffer.hlsl.
{"GI6/PsrK2/PathTraceReferencePsrGBuffer.hlsl", "PsrK2/PathTraceReferencePsrGBuffer.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// Multibounce PSR (K = 2): the GI6 twin of PsrK2/PathTraceReferencePsrGBufferSERSM69.hlsl.
{"GI6/PsrK2/PathTraceReferencePsrGBufferSERSM69.hlsl", "PsrK2/PathTraceReferencePsrGBufferSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// Multibounce PSR (K = 2): the GI6 twin of PsrK2/PathTraceReferencePsrGBufferSM69.hlsl.
{"GI6/PsrK2/PathTraceReferencePsrGBufferSM69.hlsl", "PsrK2/PathTraceReferencePsrGBufferSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// Multibounce PSR (K = 2): the GI6 twin of PsrK2/PathTraceReferencePsrSharcRender.hlsl.
{"GI6/PsrK2/PathTraceReferencePsrSharcRender.hlsl", "PsrK2/PathTraceReferencePsrSharcRender.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// Multibounce PSR (K = 2): the GI6 twin of PsrK2/PathTraceReferencePsrSharcRenderRawDiffuseHitDistance.hlsl.
{"GI6/PsrK2/PathTraceReferencePsrSharcRenderRawDiffuseHitDistance.hlsl",
    "PsrK2/PathTraceReferencePsrSharcRenderRawDiffuseHitDistance.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// Multibounce PSR (K = 2): the GI6 twin of PsrK2/PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSERSM69.hlsl.
{"GI6/PsrK2/PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PsrK2/PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// Multibounce PSR (K = 2): the GI6 twin of PsrK2/PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSM69.hlsl.
{"GI6/PsrK2/PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSM69.hlsl",
    "PsrK2/PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// Multibounce PSR (K = 2): the GI6 twin of PsrK2/PathTraceReferencePsrSharcRenderSERSM69.hlsl.
{"GI6/PsrK2/PathTraceReferencePsrSharcRenderSERSM69.hlsl", "PsrK2/PathTraceReferencePsrSharcRenderSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// Multibounce PSR (K = 2): the GI6 twin of PsrK2/PathTraceReferencePsrSharcRenderSM69.hlsl.
{"GI6/PsrK2/PathTraceReferencePsrSharcRenderSM69.hlsl", "PsrK2/PathTraceReferencePsrSharcRenderSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// Multibounce PSR (K = 2): the GI6 twin of PsrK2/PathTraceReferenceRawDiffuseHitDistancePsrGBuffer.hlsl.
{"GI6/PsrK2/PathTraceReferenceRawDiffuseHitDistancePsrGBuffer.hlsl",
    "PsrK2/PathTraceReferenceRawDiffuseHitDistancePsrGBuffer.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// Multibounce PSR (K = 2): the GI6 twin of PsrK2/PathTraceReferenceRawDiffuseHitDistancePsrGBufferSERSM69.hlsl.
{"GI6/PsrK2/PathTraceReferenceRawDiffuseHitDistancePsrGBufferSERSM69.hlsl",
    "PsrK2/PathTraceReferenceRawDiffuseHitDistancePsrGBufferSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// Multibounce PSR (K = 2): the GI6 twin of PsrK2/PathTraceReferenceRawDiffuseHitDistancePsrGBufferSM69.hlsl.
{"GI6/PsrK2/PathTraceReferenceRawDiffuseHitDistancePsrGBufferSM69.hlsl",
    "PsrK2/PathTraceReferenceRawDiffuseHitDistancePsrGBufferSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// GI.6 RayQuery twin of PsrK2/SharcNv/PathTraceSharcNvPsrRender.hlsl.
{"GI6/PsrK2/SharcNv/PathTraceSharcNvPsrRender.hlsl", "PsrK2/SharcNv/PathTraceSharcNvPsrRender.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// GI.6 RayQuery twin of PsrK2/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistance.hlsl.
{"GI6/PsrK2/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistance.hlsl",
    "PsrK2/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistance.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// GI.6 RayQuery twin of PsrK2/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSERSM69.hlsl.
{"GI6/PsrK2/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PsrK2/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// GI.6 RayQuery twin of PsrK2/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSM69.hlsl.
{"GI6/PsrK2/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSM69.hlsl",
    "PsrK2/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// GI.6 RayQuery twin of PsrK2/SharcNv/PathTraceSharcNvPsrRenderSERSM69.hlsl.
{"GI6/PsrK2/SharcNv/PathTraceSharcNvPsrRenderSERSM69.hlsl", "PsrK2/SharcNv/PathTraceSharcNvPsrRenderSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// GI.6 RayQuery twin of PsrK2/SharcNv/PathTraceSharcNvPsrRenderSM69.hlsl.
{"GI6/PsrK2/SharcNv/PathTraceSharcNvPsrRenderSM69.hlsl", "PsrK2/SharcNv/PathTraceSharcNvPsrRenderSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// GI.6 RayQuery twin of SharcNv/PathTraceSharcNvPsrRender.hlsl.
{"GI6/SharcNv/PathTraceSharcNvPsrRender.hlsl", "SharcNv/PathTraceSharcNvPsrRender.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// GI.6 RayQuery twin of SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistance.hlsl.
{"GI6/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistance.hlsl",
    "SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistance.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// GI.6 RayQuery twin of SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSERSM69.hlsl.
{"GI6/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// GI.6 RayQuery twin of SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSM69.hlsl.
{"GI6/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSM69.hlsl",
    "SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// GI.6 RayQuery twin of SharcNv/PathTraceSharcNvPsrRenderSERSM69.hlsl.
{"GI6/SharcNv/PathTraceSharcNvPsrRenderSERSM69.hlsl", "SharcNv/PathTraceSharcNvPsrRenderSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// GI.6 RayQuery twin of SharcNv/PathTraceSharcNvPsrRenderSM69.hlsl.
{"GI6/SharcNv/PathTraceSharcNvPsrRenderSM69.hlsl", "SharcNv/PathTraceSharcNvPsrRenderSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// GI.6 RayQuery twin of SharcNv/PathTraceSharcNvRender.hlsl: secondary next-event estimation's shadow rays as inline
// RayQuery, like every other render library's GI6/ twin (NVIDIA SHARC Checkpoint A; compiled with -enable-16bit-types).
{"GI6/SharcNv/PathTraceSharcNvRender.hlsl", "SharcNv/PathTraceSharcNvRender.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// GI.6 RayQuery twin of SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistance.hlsl: secondary next-event estimation's
// shadow rays as inline RayQuery, like every other render library's GI6/ twin (NVIDIA SHARC Checkpoint A; compiled with
// -enable-16bit-types).
{"GI6/SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistance.hlsl",
    "SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistance.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// GI.6 RayQuery twin of SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistanceSERSM69.hlsl: secondary next-event
// estimation's shadow rays as inline RayQuery, like every other render library's GI6/ twin (NVIDIA SHARC Checkpoint A;
// compiled with -enable-16bit-types).
{"GI6/SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// GI.6 RayQuery twin of SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistanceSM69.hlsl: secondary next-event estimation's
// shadow rays as inline RayQuery, like every other render library's GI6/ twin (NVIDIA SHARC Checkpoint A; compiled with
// -enable-16bit-types).
{"GI6/SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistanceSM69.hlsl",
    "SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistanceSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// GI.6 RayQuery twin of SharcNv/PathTraceSharcNvRenderSERSM69.hlsl: secondary next-event estimation's shadow rays as
// inline RayQuery, like every other render library's GI6/ twin (NVIDIA SHARC Checkpoint A; compiled with
// -enable-16bit-types).
{"GI6/SharcNv/PathTraceSharcNvRenderSERSM69.hlsl", "SharcNv/PathTraceSharcNvRenderSERSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},
// GI.6 RayQuery twin of SharcNv/PathTraceSharcNvRenderSM69.hlsl: secondary next-event estimation's shadow rays as
// inline RayQuery, like every other render library's GI6/ twin (NVIDIA SHARC Checkpoint A; compiled with
// -enable-16bit-types).
{"GI6/SharcNv/PathTraceSharcNvRenderSM69.hlsl", "SharcNv/PathTraceSharcNvRenderSM69.hlsl",
    "PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1", ""},

// ---- M42A ----
// M4.2A sampled indirect NEE (ENGINE_SAMPLED_NEE=1): the GI.6 RayQuery twin plus the sampled finite-light estimator
// (PathTraceReference.hlsl, PATH_TRACE_SAMPLED_NEE).
{"M42A/PathTraceReference.hlsl", "GI6/PathTraceReference.hlsl", "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferencePsr.hlsl", "GI6/PathTraceReferencePsr.hlsl", "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferencePsrGBuffer.hlsl", "GI6/PathTraceReferencePsrGBuffer.hlsl", "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferencePsrGBufferSERSM69.hlsl", "GI6/PathTraceReferencePsrGBufferSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferencePsrGBufferSM69.hlsl", "GI6/PathTraceReferencePsrGBufferSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferencePsrSERSM69.hlsl", "GI6/PathTraceReferencePsrSERSM69.hlsl", "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferencePsrSM69.hlsl", "GI6/PathTraceReferencePsrSM69.hlsl", "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferencePsrSharcRender.hlsl", "GI6/PathTraceReferencePsrSharcRender.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferencePsrSharcRenderRawDiffuseHitDistance.hlsl",
    "GI6/PathTraceReferencePsrSharcRenderRawDiffuseHitDistance.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "GI6/PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSM69.hlsl",
    "GI6/PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferencePsrSharcRenderSERSM69.hlsl", "GI6/PathTraceReferencePsrSharcRenderSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferencePsrSharcRenderSM69.hlsl", "GI6/PathTraceReferencePsrSharcRenderSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRadianceCacheDeferred.hlsl", "GI6/PathTraceReferenceRadianceCacheDeferred.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRadianceCacheDeferredLate.hlsl", "GI6/PathTraceReferenceRadianceCacheDeferredLate.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRadianceCacheDeferredLateSERSM69.hlsl",
    "GI6/PathTraceReferenceRadianceCacheDeferredLateSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRadianceCacheDeferredLateSM69.hlsl",
    "GI6/PathTraceReferenceRadianceCacheDeferredLateSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRadianceCacheDeferredSERSM69.hlsl", "GI6/PathTraceReferenceRadianceCacheDeferredSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRadianceCacheDeferredSM69.hlsl", "GI6/PathTraceReferenceRadianceCacheDeferredSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRadianceCacheQuery.hlsl", "GI6/PathTraceReferenceRadianceCacheQuery.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRadianceCacheQuerySERSM69.hlsl", "GI6/PathTraceReferenceRadianceCacheQuerySERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRadianceCacheQuerySM69.hlsl", "GI6/PathTraceReferenceRadianceCacheQuerySM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRadianceCacheQueryTermination.hlsl",
    "GI6/PathTraceReferenceRadianceCacheQueryTermination.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRadianceCacheQueryTerminationSERSM69.hlsl",
    "GI6/PathTraceReferenceRadianceCacheQueryTerminationSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRadianceCacheQueryTerminationSM69.hlsl",
    "GI6/PathTraceReferenceRadianceCacheQueryTerminationSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRadianceCacheRender.hlsl", "GI6/PathTraceReferenceRadianceCacheRender.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistance.hlsl",
    "GI6/PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistance.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "GI6/PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistanceSM69.hlsl",
    "GI6/PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistanceSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRadianceCacheRenderSERSM69.hlsl", "GI6/PathTraceReferenceRadianceCacheRenderSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRadianceCacheRenderSM69.hlsl", "GI6/PathTraceReferenceRadianceCacheRenderSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRawDiffuseHitDistance.hlsl", "GI6/PathTraceReferenceRawDiffuseHitDistance.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRawDiffuseHitDistancePsrGBuffer.hlsl",
    "GI6/PathTraceReferenceRawDiffuseHitDistancePsrGBuffer.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRawDiffuseHitDistancePsrGBufferSERSM69.hlsl",
    "GI6/PathTraceReferenceRawDiffuseHitDistancePsrGBufferSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRawDiffuseHitDistancePsrGBufferSM69.hlsl",
    "GI6/PathTraceReferenceRawDiffuseHitDistancePsrGBufferSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRawDiffuseHitDistanceSERSM69.hlsl", "GI6/PathTraceReferenceRawDiffuseHitDistanceSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceRawDiffuseHitDistanceSM69.hlsl", "GI6/PathTraceReferenceRawDiffuseHitDistanceSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceSERSM69.hlsl", "GI6/PathTraceReferenceSERSM69.hlsl", "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PathTraceReferenceSM69.hlsl", "GI6/PathTraceReferenceSM69.hlsl", "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/PathTraceReferencePsrGBuffer.hlsl", "GI6/PsrK2/PathTraceReferencePsrGBuffer.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/PathTraceReferencePsrGBufferSERSM69.hlsl", "GI6/PsrK2/PathTraceReferencePsrGBufferSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/PathTraceReferencePsrGBufferSM69.hlsl", "GI6/PsrK2/PathTraceReferencePsrGBufferSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/PathTraceReferencePsrSharcRender.hlsl", "GI6/PsrK2/PathTraceReferencePsrSharcRender.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/PathTraceReferencePsrSharcRenderRawDiffuseHitDistance.hlsl",
    "GI6/PsrK2/PathTraceReferencePsrSharcRenderRawDiffuseHitDistance.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "GI6/PsrK2/PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSM69.hlsl",
    "GI6/PsrK2/PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/PathTraceReferencePsrSharcRenderSERSM69.hlsl", "GI6/PsrK2/PathTraceReferencePsrSharcRenderSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/PathTraceReferencePsrSharcRenderSM69.hlsl", "GI6/PsrK2/PathTraceReferencePsrSharcRenderSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/PathTraceReferenceRawDiffuseHitDistancePsrGBuffer.hlsl",
    "GI6/PsrK2/PathTraceReferenceRawDiffuseHitDistancePsrGBuffer.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/PathTraceReferenceRawDiffuseHitDistancePsrGBufferSERSM69.hlsl",
    "GI6/PsrK2/PathTraceReferenceRawDiffuseHitDistancePsrGBufferSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/PathTraceReferenceRawDiffuseHitDistancePsrGBufferSM69.hlsl",
    "GI6/PsrK2/PathTraceReferenceRawDiffuseHitDistancePsrGBufferSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/SharcNv/PathTraceSharcNvPsrRender.hlsl", "GI6/PsrK2/SharcNv/PathTraceSharcNvPsrRender.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistance.hlsl",
    "GI6/PsrK2/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistance.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "GI6/PsrK2/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSM69.hlsl",
    "GI6/PsrK2/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/SharcNv/PathTraceSharcNvPsrRenderSERSM69.hlsl", "GI6/PsrK2/SharcNv/PathTraceSharcNvPsrRenderSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/SharcNv/PathTraceSharcNvPsrRenderSM69.hlsl", "GI6/PsrK2/SharcNv/PathTraceSharcNvPsrRenderSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
// M4.2C sampled SHARC training (ENGINE_SAMPLED_NEE=1): the NVIDIA SHARC update library plus the sampled finite-light
// estimator for its next-event estimation (PathTraceReference.hlsl, PATH_TRACE_SAMPLED_NEE).
{"M42A/PsrK2/SharcNv/PathTraceSharcNvPsrTraining.hlsl", "PsrK2/SharcNv/PathTraceSharcNvPsrTraining.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/SharcNv/PathTraceSharcNvPsrTrainingDepthMatched.hlsl",
    "PsrK2/SharcNv/PathTraceSharcNvPsrTrainingDepthMatched.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/SharcNv/PathTraceSharcNvPsrTrainingDepthMatchedSERSM69.hlsl",
    "PsrK2/SharcNv/PathTraceSharcNvPsrTrainingDepthMatchedSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/SharcNv/PathTraceSharcNvPsrTrainingDepthMatchedSM69.hlsl",
    "PsrK2/SharcNv/PathTraceSharcNvPsrTrainingDepthMatchedSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/SharcNv/PathTraceSharcNvPsrTrainingSERSM69.hlsl", "PsrK2/SharcNv/PathTraceSharcNvPsrTrainingSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/PsrK2/SharcNv/PathTraceSharcNvPsrTrainingSM69.hlsl", "PsrK2/SharcNv/PathTraceSharcNvPsrTrainingSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
// M4.2A sampled indirect NEE (ENGINE_SAMPLED_NEE=1): the GI.6 RayQuery twin plus the sampled finite-light estimator
// (PathTraceReference.hlsl, PATH_TRACE_SAMPLED_NEE).
{"M42A/SharcNv/PathTraceSharcNvPsrRender.hlsl", "GI6/SharcNv/PathTraceSharcNvPsrRender.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistance.hlsl",
    "GI6/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistance.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "GI6/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSM69.hlsl",
    "GI6/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvPsrRenderSERSM69.hlsl", "GI6/SharcNv/PathTraceSharcNvPsrRenderSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvPsrRenderSM69.hlsl", "GI6/SharcNv/PathTraceSharcNvPsrRenderSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
// M4.2C sampled SHARC training (ENGINE_SAMPLED_NEE=1): the NVIDIA SHARC update library plus the sampled finite-light
// estimator for its next-event estimation (PathTraceReference.hlsl, PATH_TRACE_SAMPLED_NEE).
{"M42A/SharcNv/PathTraceSharcNvPsrTraining.hlsl", "SharcNv/PathTraceSharcNvPsrTraining.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvPsrTrainingDepthMatched.hlsl", "SharcNv/PathTraceSharcNvPsrTrainingDepthMatched.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvPsrTrainingDepthMatchedSERSM69.hlsl",
    "SharcNv/PathTraceSharcNvPsrTrainingDepthMatchedSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvPsrTrainingDepthMatchedSM69.hlsl",
    "SharcNv/PathTraceSharcNvPsrTrainingDepthMatchedSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvPsrTrainingSERSM69.hlsl", "SharcNv/PathTraceSharcNvPsrTrainingSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvPsrTrainingSM69.hlsl", "SharcNv/PathTraceSharcNvPsrTrainingSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
// M4.2A sampled indirect NEE (ENGINE_SAMPLED_NEE=1): the GI.6 RayQuery twin plus the sampled finite-light estimator
// (PathTraceReference.hlsl, PATH_TRACE_SAMPLED_NEE).
{"M42A/SharcNv/PathTraceSharcNvRender.hlsl", "GI6/SharcNv/PathTraceSharcNvRender.hlsl", "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistance.hlsl",
    "GI6/SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistance.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "GI6/SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistanceSM69.hlsl",
    "GI6/SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistanceSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvRenderSERSM69.hlsl", "GI6/SharcNv/PathTraceSharcNvRenderSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvRenderSM69.hlsl", "GI6/SharcNv/PathTraceSharcNvRenderSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
// M4.2C sampled SHARC training (ENGINE_SAMPLED_NEE=1): the NVIDIA SHARC update library plus the sampled finite-light
// estimator for its next-event estimation (PathTraceReference.hlsl, PATH_TRACE_SAMPLED_NEE).
{"M42A/SharcNv/PathTraceSharcNvTraining.hlsl", "SharcNv/PathTraceSharcNvTraining.hlsl", "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvTrainingDepthMatched.hlsl", "SharcNv/PathTraceSharcNvTrainingDepthMatched.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvTrainingDepthMatchedSERSM69.hlsl",
    "SharcNv/PathTraceSharcNvTrainingDepthMatchedSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvTrainingDepthMatchedSM69.hlsl", "SharcNv/PathTraceSharcNvTrainingDepthMatchedSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvTrainingSERSM69.hlsl", "SharcNv/PathTraceSharcNvTrainingSERSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},
{"M42A/SharcNv/PathTraceSharcNvTrainingSM69.hlsl", "SharcNv/PathTraceSharcNvTrainingSM69.hlsl",
    "PATH_TRACE_SAMPLED_NEE=1", ""},

// ---- PsrDiagnostics ----
// PSR.2 Stage 2 diagnostic variant (development only): reflected-ray origin 1 (decoded) with per-pixel hit records. See
// PsrInPlaceReplace.hlsl.
{"PsrDiagnostics/PsrInPlaceReplaceDecodedHits.hlsl", "PsrInPlaceReplace.hlsl",
    "PSR_REPLACEMENT_ORIGIN=1 PSR_DIAGNOSTIC_HITS=1", ""},
// PSR.2 Stage 2 diagnostic variant (development only): reflected-ray origin 2 (exact) with per-pixel hit records. See
// PsrInPlaceReplace.hlsl.
{"PsrDiagnostics/PsrInPlaceReplaceExactHits.hlsl", "PsrInPlaceReplace.hlsl",
    "PSR_REPLACEMENT_ORIGIN=2 PSR_DIAGNOSTIC_HITS=1", ""},
// PSR.2 Stage 2 diagnostic variant (development only): reflected-ray origin 2 (exact) with the mirror's emulated
// normals rounded to nearest even, and per-pixel hit records. See PsrInPlaceReplace.hlsl.
{"PsrDiagnostics/PsrInPlaceReplaceExactNearestHits.hlsl", "PsrInPlaceReplace.hlsl",
    "PSR_REPLACEMENT_ORIGIN=2 PSR_MIRROR_HALF_ROUNDING=1 PSR_DIAGNOSTIC_HITS=1", ""},
// PSR.2 Stage 2 diagnostic variant (development only): reflected-ray origin 0 (lift) with per-pixel hit records. See
// PsrInPlaceReplace.hlsl.
{"PsrDiagnostics/PsrInPlaceReplaceLiftHits.hlsl", "PsrInPlaceReplace.hlsl",
    "PSR_REPLACEMENT_ORIGIN=0 PSR_DIAGNOSTIC_HITS=1", ""},

// ---- PsrK2 ----
// Multibounce PSR (K = 2): PathTraceReferencePsrGBuffer.hlsl with two-mirror chains (PATH_TRACE_PSR_CHAIN 2), selected
// when the PSR mode is two-bounce. The single-bounce library is compiled from PathTraceReferencePsrGBuffer.hlsl
// unchanged.
{"PsrK2/PathTraceReferencePsrGBuffer.hlsl", "PathTraceReferencePsrGBuffer.hlsl", "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): PathTraceReferencePsrGBufferSERSM69.hlsl with two-mirror chains (PATH_TRACE_PSR_CHAIN 2),
// selected when the PSR mode is two-bounce. The single-bounce library is compiled from
// PathTraceReferencePsrGBufferSERSM69.hlsl unchanged.
{"PsrK2/PathTraceReferencePsrGBufferSERSM69.hlsl", "PathTraceReferencePsrGBufferSERSM69.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): PathTraceReferencePsrGBufferSM69.hlsl with two-mirror chains (PATH_TRACE_PSR_CHAIN 2),
// selected when the PSR mode is two-bounce. The single-bounce library is compiled from
// PathTraceReferencePsrGBufferSM69.hlsl unchanged.
{"PsrK2/PathTraceReferencePsrGBufferSM69.hlsl", "PathTraceReferencePsrGBufferSM69.hlsl", "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): PathTraceReferencePsrSharcRender.hlsl with two-mirror chains (PATH_TRACE_PSR_CHAIN 2),
// selected when the PSR mode is two-bounce. The single-bounce library is compiled from
// PathTraceReferencePsrSharcRender.hlsl unchanged.
{"PsrK2/PathTraceReferencePsrSharcRender.hlsl", "PathTraceReferencePsrSharcRender.hlsl", "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): PathTraceReferencePsrSharcRenderRawDiffuseHitDistance.hlsl with two-mirror chains
// (PATH_TRACE_PSR_CHAIN 2), selected when the PSR mode is two-bounce. The single-bounce library is compiled from
// PathTraceReferencePsrSharcRenderRawDiffuseHitDistance.hlsl unchanged.
{"PsrK2/PathTraceReferencePsrSharcRenderRawDiffuseHitDistance.hlsl",
    "PathTraceReferencePsrSharcRenderRawDiffuseHitDistance.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSERSM69.hlsl with two-mirror chains
// (PATH_TRACE_PSR_CHAIN 2), selected when the PSR mode is two-bounce. The single-bounce library is compiled from
// PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSERSM69.hlsl unchanged.
{"PsrK2/PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSM69.hlsl with two-mirror chains
// (PATH_TRACE_PSR_CHAIN 2), selected when the PSR mode is two-bounce. The single-bounce library is compiled from
// PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSM69.hlsl unchanged.
{"PsrK2/PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSM69.hlsl",
    "PathTraceReferencePsrSharcRenderRawDiffuseHitDistanceSM69.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): PathTraceReferencePsrSharcRenderSERSM69.hlsl with two-mirror chains (PATH_TRACE_PSR_CHAIN
// 2), selected when the PSR mode is two-bounce. The single-bounce library is compiled from
// PathTraceReferencePsrSharcRenderSERSM69.hlsl unchanged.
{"PsrK2/PathTraceReferencePsrSharcRenderSERSM69.hlsl", "PathTraceReferencePsrSharcRenderSERSM69.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): PathTraceReferencePsrSharcRenderSM69.hlsl with two-mirror chains (PATH_TRACE_PSR_CHAIN 2),
// selected when the PSR mode is two-bounce. The single-bounce library is compiled from
// PathTraceReferencePsrSharcRenderSM69.hlsl unchanged.
{"PsrK2/PathTraceReferencePsrSharcRenderSM69.hlsl", "PathTraceReferencePsrSharcRenderSM69.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): PathTraceReferencePsrSharcTraining.hlsl with two-mirror chains (PATH_TRACE_PSR_CHAIN 2),
// selected when the PSR mode is two-bounce. The single-bounce library is compiled from
// PathTraceReferencePsrSharcTraining.hlsl unchanged.
{"PsrK2/PathTraceReferencePsrSharcTraining.hlsl", "PathTraceReferencePsrSharcTraining.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): PathTraceReferencePsrSharcTrainingSERSM69.hlsl with two-mirror chains (PATH_TRACE_PSR_CHAIN
// 2), selected when the PSR mode is two-bounce. The single-bounce library is compiled from
// PathTraceReferencePsrSharcTrainingSERSM69.hlsl unchanged.
{"PsrK2/PathTraceReferencePsrSharcTrainingSERSM69.hlsl", "PathTraceReferencePsrSharcTrainingSERSM69.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): PathTraceReferencePsrSharcTrainingSM69.hlsl with two-mirror chains (PATH_TRACE_PSR_CHAIN 2),
// selected when the PSR mode is two-bounce. The single-bounce library is compiled from
// PathTraceReferencePsrSharcTrainingSM69.hlsl unchanged.
{"PsrK2/PathTraceReferencePsrSharcTrainingSM69.hlsl", "PathTraceReferencePsrSharcTrainingSM69.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): PathTraceReferenceRawDiffuseHitDistancePsrGBuffer.hlsl with two-mirror chains
// (PATH_TRACE_PSR_CHAIN 2), selected when the PSR mode is two-bounce. The single-bounce library is compiled from
// PathTraceReferenceRawDiffuseHitDistancePsrGBuffer.hlsl unchanged.
{"PsrK2/PathTraceReferenceRawDiffuseHitDistancePsrGBuffer.hlsl",
    "PathTraceReferenceRawDiffuseHitDistancePsrGBuffer.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): PathTraceReferenceRawDiffuseHitDistancePsrGBufferSERSM69.hlsl with two-mirror chains
// (PATH_TRACE_PSR_CHAIN 2), selected when the PSR mode is two-bounce. The single-bounce library is compiled from
// PathTraceReferenceRawDiffuseHitDistancePsrGBufferSERSM69.hlsl unchanged.
{"PsrK2/PathTraceReferenceRawDiffuseHitDistancePsrGBufferSERSM69.hlsl",
    "PathTraceReferenceRawDiffuseHitDistancePsrGBufferSERSM69.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): PathTraceReferenceRawDiffuseHitDistancePsrGBufferSM69.hlsl with two-mirror chains
// (PATH_TRACE_PSR_CHAIN 2), selected when the PSR mode is two-bounce. The single-bounce library is compiled from
// PathTraceReferenceRawDiffuseHitDistancePsrGBufferSM69.hlsl unchanged.
{"PsrK2/PathTraceReferenceRawDiffuseHitDistancePsrGBufferSM69.hlsl",
    "PathTraceReferenceRawDiffuseHitDistancePsrGBufferSM69.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): SharcNv/PathTraceSharcNvPsrRender.hlsl with two-mirror chains (PATH_TRACE_PSR_CHAIN 2).
{"PsrK2/SharcNv/PathTraceSharcNvPsrRender.hlsl", "SharcNv/PathTraceSharcNvPsrRender.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistance.hlsl with two-mirror chains
// (PATH_TRACE_PSR_CHAIN 2).
{"PsrK2/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistance.hlsl",
    "SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistance.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSERSM69.hlsl with two-mirror chains
// (PATH_TRACE_PSR_CHAIN 2).
{"PsrK2/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSM69.hlsl with two-mirror chains
// (PATH_TRACE_PSR_CHAIN 2).
{"PsrK2/SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSM69.hlsl",
    "SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSM69.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): SharcNv/PathTraceSharcNvPsrRenderSERSM69.hlsl with two-mirror chains (PATH_TRACE_PSR_CHAIN
// 2).
{"PsrK2/SharcNv/PathTraceSharcNvPsrRenderSERSM69.hlsl", "SharcNv/PathTraceSharcNvPsrRenderSERSM69.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): SharcNv/PathTraceSharcNvPsrRenderSM69.hlsl with two-mirror chains (PATH_TRACE_PSR_CHAIN 2).
{"PsrK2/SharcNv/PathTraceSharcNvPsrRenderSM69.hlsl", "SharcNv/PathTraceSharcNvPsrRenderSM69.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): SharcNv/PathTraceSharcNvPsrTraining.hlsl with two-mirror chains (PATH_TRACE_PSR_CHAIN 2).
{"PsrK2/SharcNv/PathTraceSharcNvPsrTraining.hlsl", "SharcNv/PathTraceSharcNvPsrTraining.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): SharcNv/PathTraceSharcNvPsrTrainingDepthMatched.hlsl with two-mirror chains.
{"PsrK2/SharcNv/PathTraceSharcNvPsrTrainingDepthMatched.hlsl", "SharcNv/PathTraceSharcNvPsrTrainingDepthMatched.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): SharcNv/PathTraceSharcNvPsrTrainingDepthMatchedSERSM69.hlsl with two-mirror chains.
{"PsrK2/SharcNv/PathTraceSharcNvPsrTrainingDepthMatchedSERSM69.hlsl",
    "SharcNv/PathTraceSharcNvPsrTrainingDepthMatchedSERSM69.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): SharcNv/PathTraceSharcNvPsrTrainingDepthMatchedSM69.hlsl with two-mirror chains.
{"PsrK2/SharcNv/PathTraceSharcNvPsrTrainingDepthMatchedSM69.hlsl",
    "SharcNv/PathTraceSharcNvPsrTrainingDepthMatchedSM69.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): SharcNv/PathTraceSharcNvPsrTrainingSERSM69.hlsl with two-mirror chains (PATH_TRACE_PSR_CHAIN
// 2).
{"PsrK2/SharcNv/PathTraceSharcNvPsrTrainingSERSM69.hlsl", "SharcNv/PathTraceSharcNvPsrTrainingSERSM69.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},
// Multibounce PSR (K = 2): SharcNv/PathTraceSharcNvPsrTrainingSM69.hlsl with two-mirror chains (PATH_TRACE_PSR_CHAIN
// 2).
{"PsrK2/SharcNv/PathTraceSharcNvPsrTrainingSM69.hlsl", "SharcNv/PathTraceSharcNvPsrTrainingSM69.hlsl",
    "PATH_TRACE_PSR_CHAIN=2", ""},

// ---- SharcNv ----
// NVIDIA SHARC v1.8.3 PSR.2 render library: PathTraceSharcNvRender.hlsl with the PSR G-buffer. A replaced pixel's cache
// vertex is two past its replaced surface (PathTraceSharcNvBegin); ordinary pixels query at x2.
{"SharcNv/PathTraceSharcNvPsrRender.hlsl", "SharcNv/PathTraceSharcNvRender.hlsl",
    "PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// NVIDIA SHARC v1.8.3 PSR.2 render library: PathTraceSharcNvRenderRawDiffuseHitDistance.hlsl with the PSR G-buffer. A
// replaced pixel's cache vertex is two past its replaced surface (PathTraceSharcNvBegin); ordinary pixels query at x2.
{"SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistance.hlsl",
    "SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistance.hlsl",
    "PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// NVIDIA SHARC v1.8.3 PSR.2 render library: PathTraceSharcNvRenderRawDiffuseHitDistanceSERSM69.hlsl with the PSR
// G-buffer. A replaced pixel's cache vertex is two past its replaced surface (PathTraceSharcNvBegin); ordinary pixels
// query at x2.
{"SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistanceSERSM69.hlsl",
    "PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// NVIDIA SHARC v1.8.3 PSR.2 render library: PathTraceSharcNvRenderRawDiffuseHitDistanceSM69.hlsl with the PSR G-buffer.
// A replaced pixel's cache vertex is two past its replaced surface (PathTraceSharcNvBegin); ordinary pixels query at
// x2.
{"SharcNv/PathTraceSharcNvPsrRenderRawDiffuseHitDistanceSM69.hlsl",
    "SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistanceSM69.hlsl",
    "PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// NVIDIA SHARC v1.8.3 PSR.2 render library: PathTraceSharcNvRenderSERSM69.hlsl with the PSR G-buffer. A replaced
// pixel's cache vertex is two past its replaced surface (PathTraceSharcNvBegin); ordinary pixels query at x2.
{"SharcNv/PathTraceSharcNvPsrRenderSERSM69.hlsl", "SharcNv/PathTraceSharcNvRenderSERSM69.hlsl",
    "PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// NVIDIA SHARC v1.8.3 PSR.2 render library: PathTraceSharcNvRenderSM69.hlsl with the PSR G-buffer. A replaced pixel's
// cache vertex is two past its replaced surface (PathTraceSharcNvBegin); ordinary pixels query at x2.
{"SharcNv/PathTraceSharcNvPsrRenderSM69.hlsl", "SharcNv/PathTraceSharcNvRenderSM69.hlsl",
    "PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// NVIDIA SHARC v1.8.3 PSR.2 update library: PathTraceSharcNvTraining.hlsl with the PSR G-buffer, so replaced pixels
// train from their physical mirror (SharcNv/ variant tree, -enable-16bit-types).
{"SharcNv/PathTraceSharcNvPsrTraining.hlsl", "SharcNv/PathTraceSharcNvTraining.hlsl",
    "PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// NVIDIA SHARC v1.8.3 PSR.2 depth-matched diagnostic update library: PathTraceSharcNvTrainingDepthMatched.hlsl with the
// PSR G-buffer (SharcNv/ variant tree, -enable-16bit-types).
{"SharcNv/PathTraceSharcNvPsrTrainingDepthMatched.hlsl", "SharcNv/PathTraceSharcNvTrainingDepthMatched.hlsl",
    "PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// NVIDIA SHARC v1.8.3 PSR.2 depth-matched diagnostic update library: PathTraceSharcNvTrainingDepthMatchedSERSM69.hlsl
// with the PSR G-buffer (SharcNv/ variant tree, -enable-16bit-types).
{"SharcNv/PathTraceSharcNvPsrTrainingDepthMatchedSERSM69.hlsl",
    "SharcNv/PathTraceSharcNvTrainingDepthMatchedSERSM69.hlsl",
    "PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// NVIDIA SHARC v1.8.3 PSR.2 depth-matched diagnostic update library: PathTraceSharcNvTrainingDepthMatchedSM69.hlsl with
// the PSR G-buffer (SharcNv/ variant tree, -enable-16bit-types).
{"SharcNv/PathTraceSharcNvPsrTrainingDepthMatchedSM69.hlsl", "SharcNv/PathTraceSharcNvTrainingDepthMatchedSM69.hlsl",
    "PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// NVIDIA SHARC v1.8.3 PSR.2 update library: PathTraceSharcNvTrainingSERSM69.hlsl with the PSR G-buffer, so replaced
// pixels train from their physical mirror (SharcNv/ variant tree, -enable-16bit-types).
{"SharcNv/PathTraceSharcNvPsrTrainingSERSM69.hlsl", "SharcNv/PathTraceSharcNvTrainingSERSM69.hlsl",
    "PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// NVIDIA SHARC v1.8.3 PSR.2 update library: PathTraceSharcNvTrainingSM69.hlsl with the PSR G-buffer, so replaced pixels
// train from their physical mirror (SharcNv/ variant tree, -enable-16bit-types).
{"SharcNv/PathTraceSharcNvPsrTrainingSM69.hlsl", "SharcNv/PathTraceSharcNvTrainingSM69.hlsl",
    "PATH_TRACE_PSR_LIBRARY=1 PATH_TRACE_PSR_GBUFFER=1", ""},
// NVIDIA SHARC v1.8.3 render library: PathTraceReference.hlsl querying the cache at the query vertex (x2) of
// raster-primary paths, compiled as lib_6_6 with -enable-16bit-types (SharcNv/ variant tree).
{"SharcNv/PathTraceSharcNvRender.hlsl", "PathTraceReference.hlsl", "PATH_TRACE_SHARC_NV_RENDER=1", ""},
// NVIDIA SHARC v1.8.3 render library with the raw diffuse hit distance extraction (NRD RELAX and the hybrid), compiled
// as lib_6_6 with -enable-16bit-types (SharcNv/ variant tree).
{"SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistance.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_SHARC_NV_RENDER=1 PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY=1", ""},
// NVIDIA SHARC v1.8.3 render library with the raw diffuse hit distance extraction (NRD RELAX and the hybrid), compiled
// as lib_6_9 with shader execution reordering with -enable-16bit-types (SharcNv/ variant tree).
{"SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistanceSERSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_SER=1 PATH_TRACE_SHARC_NV_RENDER=1 PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY=1", ""},
// NVIDIA SHARC v1.8.3 render library with the raw diffuse hit distance extraction (NRD RELAX and the hybrid), compiled
// as lib_6_9 with -enable-16bit-types (SharcNv/ variant tree).
{"SharcNv/PathTraceSharcNvRenderRawDiffuseHitDistanceSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_SHARC_NV_RENDER=1 PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY=1", ""},
// NVIDIA SHARC v1.8.3 render library: PathTraceReference.hlsl querying the cache at the query vertex (x2) of
// raster-primary paths, compiled as lib_6_9 with shader execution reordering and -enable-16bit-types (SharcNv/ variant
// tree).
{"SharcNv/PathTraceSharcNvRenderSERSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_SER=1 PATH_TRACE_SHARC_NV_RENDER=1", ""},
// NVIDIA SHARC v1.8.3 render library: PathTraceReference.hlsl querying the cache at the query vertex (x2) of
// raster-primary paths, compiled as lib_6_9 with -enable-16bit-types (SharcNv/ variant tree).
{"SharcNv/PathTraceSharcNvRenderSM69.hlsl", "PathTraceReference.hlsl", "PATH_TRACE_SHARC_NV_RENDER=1", ""},
// NVIDIA SHARC v1.8.3 update (training) library: PathTraceReference.hlsl with the SHARC update hooks and cache
// resampling (NVIDIA's multi-bounce feedback), compiled as lib_6_6 with -enable-16bit-types (SharcNv/ variant tree).
{"SharcNv/PathTraceSharcNvTraining.hlsl", "PathTraceReference.hlsl",
    "SHARC_UPDATE=1 PATH_TRACE_SHARC_NV_TRAINING=1", "PathTraceSharcNvTraining.hlsli"},
// NVIDIA SHARC v1.8.3 depth-matched diagnostic update library: as PathTraceSharcNvTraining.hlsl without cache
// resampling, so cached radiance holds only what the traced training paths add (compiled as lib_6_6 with
// -enable-16bit-types).
{"SharcNv/PathTraceSharcNvTrainingDepthMatched.hlsl", "PathTraceReference.hlsl",
    "SHARC_UPDATE=1 SHARC_ENABLE_CACHE_RESAMPLING=0 PATH_TRACE_SHARC_NV_TRAINING=1", "PathTraceSharcNvTraining.hlsli"},
// NVIDIA SHARC v1.8.3 depth-matched diagnostic update library: as PathTraceSharcNvTrainingSERSM69.hlsl without cache
// resampling, so cached radiance holds only what the traced training paths add (compiled as lib_6_9 with shader
// execution reordering and -enable-16bit-types).
{"SharcNv/PathTraceSharcNvTrainingDepthMatchedSERSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_SER=1 SHARC_UPDATE=1 SHARC_ENABLE_CACHE_RESAMPLING=0 PATH_TRACE_SHARC_NV_TRAINING=1",
    "PathTraceSharcNvTraining.hlsli"},
// NVIDIA SHARC v1.8.3 depth-matched diagnostic update library: as PathTraceSharcNvTrainingSM69.hlsl without cache
// resampling, so cached radiance holds only what the traced training paths add (compiled as lib_6_9 with
// -enable-16bit-types).
{"SharcNv/PathTraceSharcNvTrainingDepthMatchedSM69.hlsl", "PathTraceReference.hlsl",
    "SHARC_UPDATE=1 SHARC_ENABLE_CACHE_RESAMPLING=0 PATH_TRACE_SHARC_NV_TRAINING=1", "PathTraceSharcNvTraining.hlsli"},
// NVIDIA SHARC v1.8.3 update (training) library: PathTraceReference.hlsl with the SHARC update hooks and cache
// resampling (NVIDIA's multi-bounce feedback), compiled as lib_6_9 with shader execution reordering and
// -enable-16bit-types (SharcNv/ variant tree).
{"SharcNv/PathTraceSharcNvTrainingSERSM69.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_SER=1 SHARC_UPDATE=1 PATH_TRACE_SHARC_NV_TRAINING=1", "PathTraceSharcNvTraining.hlsli"},
// NVIDIA SHARC v1.8.3 update (training) library: PathTraceReference.hlsl with the SHARC update hooks and cache
// resampling (NVIDIA's multi-bounce feedback), compiled as lib_6_9 with -enable-16bit-types (SharcNv/ variant tree).
{"SharcNv/PathTraceSharcNvTrainingSM69.hlsl", "PathTraceReference.hlsl",
    "SHARC_UPDATE=1 PATH_TRACE_SHARC_NV_TRAINING=1", "PathTraceSharcNvTraining.hlsli"},

// ---- ReSTIRPT ----
// M5.1 ReSTIR PT Enhanced (configuration B, Editor-only, experimental): the unified initial path sampling as a compute
// shader (cs_6_6, entry RestirPtInitialCS), compiled from PathTraceReference.hlsl in its compute form so it shares the
// surfaces, lights, BSDF primitives and inline radiance tracing: PATH_TRACE_DIRECT_COMPUTE (inline radiance rays),
// PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY (inline visibility), PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY (the raw-distance
// constant row). PATH_TRACE_RESTIR_PT_LIBRARY marks the build; the path tree itself is ReSTIRPT/RestirPtInitialBody.hlsli.
// No production library includes it.
{"ReSTIRPT/RestirPtInitial.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_DIRECT_COMPUTE=1 PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1 PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY=1 "
    "PATH_TRACE_RESTIR_PT_LIBRARY=1", "ReSTIRPT/RestirPtInitialBody.hlsli"},
// M5.2: the same kernel with temporal reuse compiled in (RESTIR_PT_TEMPORAL), selected while temporal reuse is on; the
// row above carries none of the temporal code.
{"ReSTIRPT/RestirPtInitialTemporal.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_DIRECT_COMPUTE=1 PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1 PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY=1 "
    "PATH_TRACE_RESTIR_PT_LIBRARY=1 RESTIR_PT_TEMPORAL=1", "ReSTIRPT/RestirPtInitialBody.hlsli"},
// M5.6 O0: the production kernel with the shared ReGIR candidates compiled in (RESTIR_PT_REGIR), selected only on frames
// whose ReGIR slots were built (more than 64 local lights, O0 on); the rows above carry none of its code.
{"ReSTIRPT/RestirPtInitialReGIR.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_DIRECT_COMPUTE=1 PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1 PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY=1 "
    "PATH_TRACE_RESTIR_PT_LIBRARY=1 RESTIR_PT_REGIR=1", "ReSTIRPT/RestirPtInitialBody.hlsli"},
// M5.2 C0: test-only variants of the same kernel, isolated so production B never compiles their state (it lowered
// occupancy): the T-2.1 self-shift and its diagnostics (RESTIR_PT_DIAGNOSTICS), and the reconnection visibility bench
// (RESTIR_PT_VISIBILITY_BENCH). ReSTIRPTPass selects one only when its diagnostic setting is on.
{"ReSTIRPT/RestirPtInitialDiagnostics.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_DIRECT_COMPUTE=1 PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1 PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY=1 "
    "PATH_TRACE_RESTIR_PT_LIBRARY=1 RESTIR_PT_DIAGNOSTICS=1", "ReSTIRPT/RestirPtInitialBody.hlsli"},
{"ReSTIRPT/RestirPtInitialVisibilityBench.hlsl", "PathTraceReference.hlsl",
    "PATH_TRACE_DIRECT_COMPUTE=1 PATH_TRACE_SECONDARY_SHADOW_RAY_QUERY=1 PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY=1 "
    "PATH_TRACE_RESTIR_PT_LIBRARY=1 RESTIR_PT_VISIBILITY_BENCH=1", "ReSTIRPT/RestirPtInitialBody.hlsli"},
