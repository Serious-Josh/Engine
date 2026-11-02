// Multibounce PSR (K = 2): PathTraceReSTIRDIExtractPsrGBuffer.hlsl with two-mirror chains (PATH_TRACE_PSR_CHAIN 2), selected when the
// PSR mode is two-bounce. The single-bounce library is compiled from PathTraceReSTIRDIExtractPsrGBuffer.hlsl unchanged.
#define PATH_TRACE_PSR_CHAIN 2
#include "../PathTraceReSTIRDIExtractPsrGBuffer.hlsl"
