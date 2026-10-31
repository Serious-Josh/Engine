// R.9I.3 P2: whole-image transactional insertion of the pixel-centred caustic lobes (PixelCausticPass) into the
// path tracer's diffuse/specular extraction textures, before any denoiser reads them. GPU only; no CPU readback or
// wait gates it.
//
//   CSBegin    (before the estimator) stamps every pixel's state with the unpublished sentinel and clears the
//              latch's insertion words, so a pixel the estimator did not publish this frame is detectable.
//   CSValidate (after the estimator) checks the whole image and ORs refusal bits into latch word 5; it writes no
//              image. It counts the pixels insertion would change (word 6).
//   CSInsert   (after validation) writes nothing anywhere unless the estimator latch (word 0) and the refusal word
//              are zero and the latch carries this recording's epoch; then each accepted nonzero pixel stores
//              extraction + caustic per lobe (FP32 sum, half store), keeping the extraction alpha. It counts the
//              pixels written (word 7).
//
// Latch words: [0] estimator fault bits, [1] faulting receivers, [2] first faulting pixel, [3] estimator epoch,
// [4] reserved, [5] insertion refusal bits, [6] pixels validated for insertion, [7] pixels inserted.
cbuffer InsertConstants : register(b0)
{
    uint causticDiffuseIndex;
    uint causticSpecularIndex;
    uint extractionDiffuseIndex;
    uint extractionSpecularIndex;
    uint latchIndex;
    uint expectedEpoch;
    uint2 resolution;
};

static const uint StateBackground = 0;
static const uint StateUncertified = 1;
static const uint StateUnscheduled = 3;
static const uint StateAcceptedZero = 4;
static const uint StateAccepted = 5;
static const uint StateFault = 6;
static const float StateUnpublished = 255.0f;
static const float HalfMaximum = 65504.0f;

static const uint RefuseEstimatorFault = 1u << 0;     // Latch word 0 nonzero.
static const uint RefuseEpoch = 1u << 1;              // The latch is not this recording's estimator run.
static const uint RefuseUnpublished = 1u << 2;        // A pixel kept the sentinel: a tile did not publish.
static const uint RefuseInvalidState = 1u << 3;       // A state code outside the published set.
static const uint RefuseInconsistent = 1u << 4;       // A publication contradicting its state.
static const uint RefuseDestinationRange = 1u << 5;   // A sum nonfinite or outside the fp16 range.

[numthreads(8, 8, 1)] void CSBegin(uint3 id : SV_DispatchThreadID)
{
    if(all(id.xy == 0))
    {
        RWByteAddressBuffer latch = ResourceDescriptorHeap[latchIndex];
        latch.Store4(16, uint4(0, 0, 0, 0));
    }

    if(any(id.xy >= resolution))
    {
        return;
    }

    RWTexture2D<float4> causticDiffuse = ResourceDescriptorHeap[causticDiffuseIndex];
    causticDiffuse[id.xy] = float4(0.0f, 0.0f, 0.0f, StateUnpublished);
}

bool ValidRadiance(float3 value)
{
    return all(isfinite(value)) && all(value >= 0.0f);
}

bool RepresentableSum(float3 sum)
{
    return all(isfinite(sum)) && all(abs(sum) <= HalfMaximum);
}

[numthreads(8, 8, 1)] void CSValidate(uint3 id : SV_DispatchThreadID)
{
    if(any(id.xy >= resolution))
    {
        return;
    }

    RWByteAddressBuffer latch = ResourceDescriptorHeap[latchIndex];
    Texture2D<float4> causticDiffuse = ResourceDescriptorHeap[causticDiffuseIndex];
    Texture2D<float4> causticSpecular = ResourceDescriptorHeap[causticSpecularIndex];
    RWTexture2D<float4> extractionDiffuse = ResourceDescriptorHeap[extractionDiffuseIndex];
    RWTexture2D<float4> extractionSpecular = ResourceDescriptorHeap[extractionSpecularIndex];
    uint refusal = 0;

    // Frame-level ownership, checked by every invocation so no pixel's verdict depends on another's.
    if(latch.Load(0) != 0u)
    {
        refusal |= RefuseEstimatorFault;
    }

    if(latch.Load(12) != expectedEpoch || expectedEpoch == 0u)
    {
        refusal |= RefuseEpoch;
    }

    float4 d = causticDiffuse.Load(int3(id.xy, 0));
    float4 s = causticSpecular.Load(int3(id.xy, 0));
    bool insertable = false;

    if(d.w == StateUnpublished)
    {
        refusal |= RefuseUnpublished;
    }
    else
    {
        uint state = (uint)d.w;
        bool knownState = (float)state == d.w && (state == StateBackground || state == StateUncertified ||
            state == StateUnscheduled || state == StateAcceptedZero || state == StateAccepted || state == StateFault);
        bool anyRadiance = any(d.rgb != 0.0f) || any(s.rgb != 0.0f);

        if(!knownState)
        {
            refusal |= RefuseInvalidState;
        }
        else if(!ValidRadiance(d.rgb) || !ValidRadiance(s.rgb) || !(s.w >= 0.0f) || (float)(uint)s.w != s.w)
        {
            refusal |= RefuseInconsistent;
        }
        else if(state == StateAccepted)
        {
            // Accepted: no fault bits, some radiance, and both sums representable in the destination.
            insertable = true;

            if(s.w != 0.0f || !anyRadiance)
            {
                refusal |= RefuseInconsistent;
            }

            float3 diffuseSum = extractionDiffuse[id.xy].rgb + d.rgb;
            float3 specularSum = extractionSpecular[id.xy].rgb + s.rgb;

            if(!RepresentableSum(diffuseSum) || !RepresentableSum(specularSum))
            {
                refusal |= RefuseDestinationRange;
            }
        }
        else if(state == StateFault)
        {
            // A published fault must also be in the estimator latch; it contributes nothing.
            if(anyRadiance || s.w == 0.0f)
            {
                refusal |= RefuseInconsistent;
            }
        }
        else if(anyRadiance || s.w != 0.0f)
        {
            // Background, uncertified, unscheduled and accepted-zero pixels publish no radiance and no faults.
            refusal |= RefuseInconsistent;
        }
    }

    uint previous;

    if(refusal != 0u)
    {
        latch.InterlockedOr(20, refusal, previous);
    }

    if(insertable)
    {
        latch.InterlockedAdd(24, 1u, previous);
    }
}

[numthreads(8, 8, 1)] void CSInsert(uint3 id : SV_DispatchThreadID)
{
    if(any(id.xy >= resolution))
    {
        return;
    }

    RWByteAddressBuffer latch = ResourceDescriptorHeap[latchIndex];

    // The whole-image decision: every invocation reads the same completed validation result.
    if(latch.Load(0) != 0u || latch.Load(12) != expectedEpoch || latch.Load(20) != 0u)
    {
        return;
    }

    Texture2D<float4> causticDiffuse = ResourceDescriptorHeap[causticDiffuseIndex];
    float4 d = causticDiffuse.Load(int3(id.xy, 0));

    if(d.w != (float)StateAccepted)
    {
        return;
    }

    Texture2D<float4> causticSpecular = ResourceDescriptorHeap[causticSpecularIndex];
    RWTexture2D<float4> extractionDiffuse = ResourceDescriptorHeap[extractionDiffuseIndex];
    RWTexture2D<float4> extractionSpecular = ResourceDescriptorHeap[extractionSpecularIndex];
    float4 s = causticSpecular.Load(int3(id.xy, 0));

    // A lobe with no caustic radiance is not rewritten, so its stored bits (including a signed zero) stay exact.
    if(any(d.rgb != 0.0f))
    {
        float4 baseline = extractionDiffuse[id.xy];
        extractionDiffuse[id.xy] = float4(baseline.rgb + d.rgb, baseline.a);
    }

    if(any(s.rgb != 0.0f))
    {
        float4 baseline = extractionSpecular[id.xy];
        extractionSpecular[id.xy] = float4(baseline.rgb + s.rgb, baseline.a);
    }

    uint previous;
    latch.InterlockedAdd(28, 1u, previous);
}
