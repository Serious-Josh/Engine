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
