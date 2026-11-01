// PSR.2 Stage 3B: the a-trous spatial pass with PATH_TRACE_PSR_DENOISE (PsrDenoise.hlsli), selected on frames that
// light replaced pixels. Development only; production frames keep PathTraceSpatialAtrous.hlsl.
#define PATH_TRACE_PSR_DENOISE 1
#include "PathTraceSpatialAtrous.hlsl"
