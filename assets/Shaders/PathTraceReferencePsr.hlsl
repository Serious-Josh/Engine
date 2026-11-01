// PSR.1a: Raster Primary primary surface replacement (PATH_TRACE_PSR_LIBRARY, PathTraceReference.hlsl), in a DXR
// library of its own so every production library compiles exactly as before. Development only: created when the PSR
// toggle is on; not in the cooked manifests, so packaged runtimes report it unavailable.
// Compiled as lib_6_6, as PathTraceReference.hlsl.
#define PATH_TRACE_PSR_LIBRARY 1
#include "PathTraceReference.hlsl"
