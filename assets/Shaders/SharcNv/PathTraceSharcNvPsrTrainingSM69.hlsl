// NVIDIA SHARC v1.8.3 PSR.2 update library: PathTraceSharcNvTrainingSM69.hlsl with the PSR G-buffer, so replaced pixels
// train from their physical mirror (SharcNv/ variant tree, -enable-16bit-types).
#define PATH_TRACE_PSR_LIBRARY 1
#define PATH_TRACE_PSR_GBUFFER 1
#include "PathTraceSharcNvTrainingSM69.hlsl"
