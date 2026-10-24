// SHARC.4D-Q shader-floor experiment (diagnostics): PathTraceReferenceRadianceCacheDeferredSM69.hlsl with the
// whole training record stored at the end of each path (PATH_TRACE_RADIANCE_CACHE_DEFERRED_LATE), as the 4D-P store
// probe did, instead of x1's part at vertex 1.
#define PATH_TRACE_RADIANCE_CACHE_LIBRARY 1
#define PATH_TRACE_RADIANCE_CACHE_DEFERRED 1
#define PATH_TRACE_RADIANCE_CACHE_DEFERRED_LATE 1
#include "PathTraceReference.hlsl"
