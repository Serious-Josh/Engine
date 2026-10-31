// R.9I.3 pixel-centred caustics, verification variant: the production kernels plus B3-T selected-probe capture
// (attempt records, raster tuples, the 64-byte output record) and fault injection for the independent assessor.
// Used only by verification builds (PixelCausticPass diagnostics); never by production frames.
#define PIXEL_CAUSTIC_PROBES 1
#include "CausticPixel.hlsl"
