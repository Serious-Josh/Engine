// SHARC.4D: PathTraceReference.hlsl as the D6 radiance cache's learning library (lib_6_6). Only the
// raster-primary export with the cache binding (b3) is used; it harvests training samples and renders exactly the
// reference image. Selected only while the experimental D6 cache is enabled and usable (PathTraceReferencePass).
#define PATH_TRACE_RADIANCE_CACHE_LIBRARY 1
#include "PathTraceReference.hlsl"
