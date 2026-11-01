// PSR.2 Stage 2 diagnostic variant (development only): reflected-ray origin 2 (exact) with the mirror's
// emulated normals rounded to nearest even, and per-pixel hit records. See PsrInPlaceReplace.hlsl.
#define PSR_REPLACEMENT_ORIGIN 2
#define PSR_MIRROR_HALF_ROUNDING 1
#define PSR_DIAGNOSTIC_HITS 1
#include "../PsrInPlaceReplace.hlsl"
