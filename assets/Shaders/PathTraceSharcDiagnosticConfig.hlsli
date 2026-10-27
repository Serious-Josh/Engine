#ifndef PATH_TRACE_SHARC_DIAGNOSTIC_CONFIG_HLSLI
#define PATH_TRACE_SHARC_DIAGNOSTIC_CONFIG_HLSLI

// Temporary compile-time diagnostic selector shared by C++ presentation and HLSL. Rebuild and redeploy together.
// 0 production; 1 substitution coverage; 2 full key; 3 reconstructed cached RGB; 4 spatial cell.
// No runtime/Editor setting. Use 1 spp for categorical inspection. Zero removes all diagnostic presentation work.
#ifndef PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC
#define PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC 0
#endif

#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC < 0 || PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC > 4
#error Invalid SHARC spatial diagnostic mode
#endif

#endif
