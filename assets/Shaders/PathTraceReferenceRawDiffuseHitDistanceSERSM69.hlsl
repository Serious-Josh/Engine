// NRD.RELAX.2B: the extraction exports with the raw primary diffuse hit distance (PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY,
// PathTraceReference.hlsl), in a DXR library of their own so the production and REBLUR extraction libraries compile
// exactly as before. Diagnostics only: created when RELAX inputs are requested.
// Compiled as lib_6_9 with SER, as PathTraceReferenceSERSM69.hlsl.
#define PATH_TRACE_RAW_DIFFUSE_HIT_DISTANCE_LIBRARY 1
#define PATH_TRACE_SER 1
#include "PathTraceReference.hlsl"
