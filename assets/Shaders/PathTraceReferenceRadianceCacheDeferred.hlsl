// SHARC.4D-Q: PathTraceReference.hlsl as the D6 radiance cache's dense learning library (lib_6_6): it renders the
// unchanged raster-primary image, and each path stores its training record (PathTraceRadianceCacheHarvest.hlsli) for
// CSInsertRecords to insert; no insertion code is compiled into the ray generation.
#define PATH_TRACE_RADIANCE_CACHE_LIBRARY 1
#define PATH_TRACE_RADIANCE_CACHE_DEFERRED 1
#include "PathTraceReference.hlsl"
