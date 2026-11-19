// NVIDIA SHARC v1.8.3 update (training) library: PathTraceReference.hlsl with the SHARC update hooks and cache resampling
// (NVIDIA's multi-bounce feedback), compiled as lib_6_9 with -enable-16bit-types (SharcNv/ variant tree).
#define SHARC_UPDATE 1
#define PATH_TRACE_SHARC_NV_TRAINING 1
#include "../PathTraceReference.hlsl"
#include "../PathTraceSharcNvTraining.hlsli"
