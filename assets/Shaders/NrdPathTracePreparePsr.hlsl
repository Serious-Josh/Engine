// PSR.2 Stage 3B phase 4: the path-traced NRD preparation and resolves with NRD_PATH_TRACE_PSR (PsrDenoise.hlsli),
// selected on frames that light replaced pixels. Development only; production frames keep NrdPathTracePrepare.hlsl.
#define NRD_PATH_TRACE_PSR 1
#include "NrdPathTracePrepare.hlsl"
