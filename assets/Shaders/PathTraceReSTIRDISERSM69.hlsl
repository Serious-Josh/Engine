// SER S5A: PathTraceReSTIRDI.hlsl compiled as lib_6_9 with PATH_TRACE_SER (ShaderCompiler::GetLibraryProfile), the
// ReSTIR DI library of the SER library set. Its direct pass traces visibility rays only, which never reorder; the
// ReSTIR DI continuation rays reorder in PathTraceReferenceSERSM69.hlsl's indirect pass. PathTraceReSTIRDISM69.hlsl
// is its non-SER comparison baseline.
#define PATH_TRACE_SER 1
#include "PathTraceReSTIRDI.hlsl"
