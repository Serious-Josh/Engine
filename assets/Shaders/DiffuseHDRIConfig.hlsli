#ifndef DIFFUSE_HDRI_CONFIG_HLSLI
#define DIFFUSE_HDRI_CONFIG_HLSLI

// Reversible diffuse-only prefilter experiment: test 0, 4, or 6.
// Rebuild/stage and restart so convolution and production RTGI use the same value.
#ifndef DIFFUSE_HDRI_SOURCE_MIP
#define DIFFUSE_HDRI_SOURCE_MIP 6
#endif

#endif
