#ifndef ENGINE_RTXDI_ENVIRONMENT_CONFIG
#define ENGINE_RTXDI_ENVIRONMENT_CONFIG

// PT.2B: who estimates the HDRI's direct light at the ReSTIR DI vertex (the camera-visible surface). Shared by every path
// tracer library (all include PathTraceReference.hlsl) and by the renderer's RTXDI light set (RtxdiLightBridge.cpp), so a
// build always pairs the shaders with the matching light layout. Compile-time only: each library contains exactly one
// strategy (no runtime branch between them). Diagnostics compare strategies by building with another value.
//
// Production (PT.2B): SEPARATE_MIS. In rooms lit through occluded sky it was 18-40x lower in error than the other two (an
// RTXDI HDRI candidate's unshadowed target takes reservoir selections from the local lights, and is then occluded); outdoors it
// is about 1.3x noisier and costs one shadow ray per HDRI sample (PT2B_Environment_Light_Infrastructure.md).
//
// INFINITE_LIGHT (pre-PT.2B baseline): the HDRI is one of RTXDI's infinite lights, sampled through Engine's CDF with
//   uniform selection among the infinite lights; no BSDF strategy reaches it.
// RESERVOIR (Approach A): the HDRI is RTXDI's environment light. Environment candidates draw RTXDI's uv uniformly (Engine's
//   CDF warp turns that into luminance importance sampling) and BSDF candidates trace a ray and, when it escapes, enter the
//   HDRI at the inverse-warped uv; the SDK's balance-heuristic weights combine the two. Reused like every other sample.
// SEPARATE_MIS (Approach B): the HDRI is not an RTXDI light. The direct pass adds one HDRI light sample and the indirect pass
//   adds the vertex's BSDF continuation when it escapes, both power-heuristic weighted (the reference path tracer's
//   estimator); RTXDI keeps the local lights, directional lights and the sun.
#define RTXDI_ENVIRONMENT_STRATEGY_INFINITE_LIGHT 0
#define RTXDI_ENVIRONMENT_STRATEGY_RESERVOIR 1
#define RTXDI_ENVIRONMENT_STRATEGY_SEPARATE_MIS 2

#ifndef RTXDI_ENVIRONMENT_STRATEGY
#define RTXDI_ENVIRONMENT_STRATEGY RTXDI_ENVIRONMENT_STRATEGY_SEPARATE_MIS
#endif

#endif
