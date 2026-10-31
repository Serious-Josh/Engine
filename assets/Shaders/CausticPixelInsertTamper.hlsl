// R.9I.3 P2, verification only: corrupts one completed estimator publication (or the frame latch) between the
// estimator and insertion validation, so the whole-image refusal can be exercised. Never used by production frames.
cbuffer TamperConstants : register(b0)
{
    uint causticDiffuseIndex;
    uint causticSpecularIndex;
    uint extractionDiffuseIndex;
    uint extractionSpecularIndex;
    uint latchIndex;
    uint mode;
    uint2 pixel;
};

static const uint TamperLatchFault = 1;     // An estimator fault bit set after execution.
static const uint TamperStaleEpoch = 2;     // The latch no longer carries this recording's epoch.
static const uint TamperOverflow = 3;       // A finite specular sum above the fp16 maximum at one accepted pixel.
static const uint TamperInvalidState = 4;   // A state code outside the published set.
static const uint TamperUnpublished = 5;    // A pixel the estimator did not publish (the sentinel kept).
static const uint TamperInconsistent = 6;   // Accepted zero carrying radiance.

[numthreads(1, 1, 1)] void CSTamper(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer latch = ResourceDescriptorHeap[latchIndex];
    RWTexture2D<float4> causticDiffuse = ResourceDescriptorHeap[causticDiffuseIndex];
    RWTexture2D<float4> causticSpecular = ResourceDescriptorHeap[causticSpecularIndex];
    RWTexture2D<float4> extractionSpecular = ResourceDescriptorHeap[extractionSpecularIndex];

    if(mode == TamperLatchFault)
    {
        uint previous;
        latch.InterlockedOr(0, 8u, previous);
    }
    else if(mode == TamperStaleEpoch)
    {
        latch.Store(12, latch.Load(12) + 1u);
    }
    else if(mode == TamperOverflow)
    {
        // A valid accepted publication whose specular lobe alone is representable, but whose sum is not.
        causticDiffuse[pixel] = float4(causticDiffuse[pixel].rgb, 5.0f);
        causticSpecular[pixel] = float4(65504.0f, 0.0f, 0.0f, 0.0f);
        float4 baseline = extractionSpecular[pixel];
        extractionSpecular[pixel] = float4(1.0f, baseline.gba);
    }
    else if(mode == TamperInvalidState)
    {
        causticDiffuse[pixel] = float4(causticDiffuse[pixel].rgb, 2.0f);
    }
    else if(mode == TamperUnpublished)
    {
        causticDiffuse[pixel] = float4(0.0f, 0.0f, 0.0f, 255.0f);
    }
    else if(mode == TamperInconsistent)
    {
        causticDiffuse[pixel] = float4(1.0f, 0.0f, 0.0f, 4.0f);
        causticSpecular[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
}
