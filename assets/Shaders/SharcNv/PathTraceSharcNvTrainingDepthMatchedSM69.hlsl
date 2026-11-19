// NVIDIA SHARC v1.8.3 depth-matched diagnostic update library: as PathTraceSharcNvTrainingSM69.hlsl without cache resampling,
// so cached radiance holds only what the traced training paths add (compiled as lib_6_9 with -enable-16bit-types).
#define SHARC_UPDATE 1
#define SHARC_ENABLE_CACHE_RESAMPLING 0
#define PATH_TRACE_SHARC_NV_TRAINING 1
#include "../PathTraceReference.hlsl"
#include "../PathTraceSharcNvTraining.hlsli"
