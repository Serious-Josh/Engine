// RTXPT D.1B.2: the split ReSTIR DI direct pass with diffuse/specular signal extraction, in a DXR library of its
// own. A second RTXDI direct-pass ray generation in PathTraceReSTIRDI.hlsl changed what the production direct pass
// rendered at runtime, so this library holds the extraction direct pass in place of the production one
// (PathTraceReSTIRDISplitDirectExtractRayGeneration only). Diagnostics only: no production path selects it.
#define PATH_TRACE_RESTIR_DI_EXTRACTION_LIBRARY 1
#include "PathTraceReSTIRDI.hlsl"
