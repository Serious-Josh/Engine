// Multibounce PSR (K = 2): PathTraceReferencePsrGBuffer.hlsl with two-mirror chains (PATH_TRACE_PSR_CHAIN 2), selected when the
// PSR mode is two-bounce. The single-bounce library is compiled from PathTraceReferencePsrGBuffer.hlsl unchanged.
#define PATH_TRACE_PSR_CHAIN 2
#include "../PathTraceReferencePsrGBuffer.hlsl"
