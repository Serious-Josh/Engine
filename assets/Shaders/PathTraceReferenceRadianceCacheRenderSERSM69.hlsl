// SHARC.4E (experimental, default off): PathTraceReference.hlsl rendering with cached D6 radiance at path vertex 1
// (PATH_TRACE_RADIANCE_CACHE_RENDER, PathTraceRadianceCacheQuery.hlsli), compiled as lib_6_9 with shader execution
// reordering (ShaderCompiler::GetLibraryProfile).
#define PATH_TRACE_SER 1
#define PATH_TRACE_RADIANCE_CACHE_LIBRARY 1
#define PATH_TRACE_RADIANCE_CACHE_QUERY 1
#define PATH_TRACE_RADIANCE_CACHE_RENDER 1
#include "PathTraceReference.hlsl"
