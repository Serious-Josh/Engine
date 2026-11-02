// Multibounce PSR (K = 2): PathTraceReSTIRDIPsrGBuffer.hlsl with two-mirror chains (PATH_TRACE_PSR_CHAIN 2), selected when the
// PSR mode is two-bounce. The single-bounce library is compiled from PathTraceReSTIRDIPsrGBuffer.hlsl unchanged.
#define PATH_TRACE_PSR_CHAIN 2
#include "../PathTraceReSTIRDIPsrGBuffer.hlsl"
