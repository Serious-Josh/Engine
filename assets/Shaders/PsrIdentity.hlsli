#ifndef ENGINE_PSR_IDENTITY
#define ENGINE_PSR_IDENTITY

// PSR.2: the raster identity target (GBuffer::IdentityFormat) is (TLAS InstanceID + 1, TLAS primitive), 0 for the
// background, or all ones for a draw without an exact TLAS mapping. On Path Tracing frames with in-place replacement
// enabled, Triangle.hlsl also sets bit 31 of x for a pixel whose G-buffer surface is a PSR-eligible pure-delta mirror
// (PsrEligibility.hlsli). InstanceID is 24 bits, so a flagged value never equals the all-ones sentinel.
static const uint PSR_IDENTITY_ELIGIBLE = 0x80000000u;
static const uint PSR_IDENTITY_UNMAPPED = 0xFFFFFFFFu;

// The identity's x without the eligibility flag; the background (0) and unmapped (all ones) values are unchanged.
uint PsrIdentityInstance(uint identityX)
{
    return identityX == PSR_IDENTITY_UNMAPPED ? identityX : identityX & ~PSR_IDENTITY_ELIGIBLE;
}

bool PsrIdentityEligible(uint identityX)
{
    return identityX != PSR_IDENTITY_UNMAPPED && (identityX & PSR_IDENTITY_ELIGIBLE) != 0u;
}

#endif
