// Production ReSTIR DI direct pass as a compute shader (cs_6_6, entry PathTraceReSTIRDIDirectCS; ShaderCompiler maps
// DirectLighting/ to it), compiled from PathTraceReference.hlsl: PATH_TRACE_RESTIR_DI_LIBRARY selects the ReSTIR DI
// direct pass, PATH_TRACE_DIRECT_COMPUTE its compute entry, and its shadow rays are inline RayQuery
// (PATH_TRACE_PRIMARY_DI_SHADOW_RAY_QUERY).
// Two-bounce PSR (PSR.2 G-buffer storage, K = 2).
#define PATH_TRACE_DIRECT_COMPUTE 1
#define PATH_TRACE_PRIMARY_DI_SHADOW_RAY_QUERY 1
#define PATH_TRACE_RESTIR_DI_LIBRARY 1
#define PATH_TRACE_PSR_CHAIN 2
#define PATH_TRACE_PSR_LIBRARY 1
#define PATH_TRACE_PSR_GBUFFER 1
#include "../PathTraceReference.hlsl"
