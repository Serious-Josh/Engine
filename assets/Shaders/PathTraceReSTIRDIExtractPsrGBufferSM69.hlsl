// PSR.2 Stage 3B: the extraction direct pass (PathTraceReSTIRDIExtractSM69.hlsl) as a PSR library with the G-buffer
// storage (PATH_TRACE_PSR_GBUFFER): replaced pixels are lit and split at their reflected surface. Development only.
#define PATH_TRACE_PSR_LIBRARY 1
#define PATH_TRACE_PSR_GBUFFER 1
#include "PathTraceReSTIRDIExtractSM69.hlsl"
