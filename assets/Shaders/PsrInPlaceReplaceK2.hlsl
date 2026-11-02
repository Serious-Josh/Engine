// Multibounce PSR: the in-place replacement pass with two-mirror chains (PSR_MAX_CHAIN 2), selected on frames whose PSR
// mode is two-bounce. Single-bounce frames keep PsrInPlaceReplace.hlsl, whose compiled code this variant leaves unchanged.
#define PSR_MAX_CHAIN 2
#include "PsrInPlaceReplace.hlsl"
