// Multibounce PSR (K = 2): PathTraceReferenceRawDiffuseHitDistancePsrGBuffer.hlsl with two-mirror chains (PATH_TRACE_PSR_CHAIN 2), selected when the
// PSR mode is two-bounce. The single-bounce library is compiled from PathTraceReferenceRawDiffuseHitDistancePsrGBuffer.hlsl unchanged.
#define PATH_TRACE_PSR_CHAIN 2
#include "../PathTraceReferenceRawDiffuseHitDistancePsrGBuffer.hlsl"
