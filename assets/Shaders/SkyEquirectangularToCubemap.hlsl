// Physical sky: the GGX-prefiltered cube of the sky's environment sets, the HDRI path's pass with fewer samples per texel.
// The sky has no detail finer than its aureole once the sun disk is excluded, and the pass filters each sample from the
// source mip that matches the sample's solid angle, so fewer samples blur no more than the lobe itself.
#define ENVIRONMENT_PREFILTER_SAMPLE_COUNT 256u
#include "EquirectangularToCubemap.hlsl"
