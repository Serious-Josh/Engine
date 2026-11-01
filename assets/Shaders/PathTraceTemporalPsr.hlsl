// PSR.2 Stage 3B: the temporal pass with PATH_TRACE_PSR_DENOISE (PsrDenoise.hlsli), selected on frames that light
// replaced pixels. Development only; production frames keep PathTraceTemporal.hlsl.
#define PATH_TRACE_PSR_DENOISE 1
#include "PathTraceTemporal.hlsl"
