// SHARC.4D-Q: PathTraceReference.hlsl as the D6 radiance cache's deferred learning library (lib_6_6). Like
// PathTraceReferenceRadianceCache.hlsl, but each path stores its training record (PathTraceRadianceCacheHarvest.hlsli)
// and CSInsertRecords inserts it; no insertion code is compiled into the ray generation.
#define PATH_TRACE_RADIANCE_CACHE_LIBRARY 1
#define PATH_TRACE_RADIANCE_CACHE_DEFERRED 1
#include "PathTraceReference.hlsl"
