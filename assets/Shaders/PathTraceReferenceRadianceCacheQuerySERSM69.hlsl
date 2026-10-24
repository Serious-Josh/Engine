// SHARC.4D-R shader-floor experiment (diagnostics): PathTraceReference.hlsl with the D6 cache's query-only hooks
// (PathTraceRadianceCacheQuery.hlsli), compiled as lib_6_9 with shader execution reordering
// (ShaderCompiler::GetLibraryProfile).
#define PATH_TRACE_SER 1
#define PATH_TRACE_RADIANCE_CACHE_LIBRARY 1
#define PATH_TRACE_RADIANCE_CACHE_QUERY 1
#include "PathTraceReference.hlsl"
